#include "Transcriber.hpp"

#include <transcribe.h>
#include <transcribe/parakeet.h>
#include <transcribe/voxtral_realtime.h>
#include <hyprutils/cli/Logger.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <utility>

// 16 kHz mono float PCM is fed in blocks of at least 100 ms; the model
// buffers up to its own chunk size.
constexpr size_t   MIN_FEED_SAMPLES = 1600;

// Streaming models count their latency in 80 ms steps.
constexpr int      STEP_MS = 80;

static std::string status(int s) {
    return transcribe_status_string(s);
}

static Hyprutils::CLI::CLogger logger;

// Model families with a latency setting, each under its own name.
enum class eFamily {
    NONE,     // no latency setting (Moonshine)
    NEMOTRON, // lookahead
    PARAKEET, // a chunk and its lookahead (Parakeet Unified)
    VOXTRAL,  // delay (Voxtral Realtime)
};

static eFamily family(const transcribe_model* model) {
    const auto ACCEPTS = [model](uint32_t kind) { return transcribe_model_accepts_ext_kind(model, TRANSCRIBE_EXT_SLOT_STREAM, kind); };
    if (ACCEPTS(TRANSCRIBE_EXT_KIND_PARAKEET_STREAM))
        return eFamily::NEMOTRON;
    if (ACCEPTS(TRANSCRIBE_EXT_KIND_PARAKEET_BUFFERED_STREAM))
        return eFamily::PARAKEET;
    if (ACCEPTS(TRANSCRIBE_EXT_KIND_VOXTRAL_REALTIME_STREAM))
        return eFamily::VOXTRAL;
    return eFamily::NONE;
}

// Parakeet Unified's trained (chunk, lookahead) pairs, in steps.
constexpr std::array<std::pair<int, int>, 5> PARAKEET_PAIRS = {{{1, 1}, {2, 2}, {2, 4}, {7, 7}, {13, 13}}};

CTranscriber::CTranscriber(const SOptions& options, UpdateFn onUpdate) : m_options(options), m_onUpdate(std::move(onUpdate)) {
    if (const auto RET = transcribe_init_backends_default(); RET != TRANSCRIBE_OK)
        throw std::runtime_error(std::format("can't initialize compute backends: {}", status(RET)));

    if (const auto RET = transcribe_open(m_options.model.c_str(), nullptr, nullptr, &m_session); RET != TRANSCRIBE_OK)
        throw std::runtime_error(std::format("can't load model {}: {}", m_options.model, status(RET)));

    logger.log(Hyprutils::CLI::LOG_DEBUG, "loaded {} on {}", m_options.model, transcribe_model_backend(transcribe_get_model(m_session)));

    m_thread = std::thread([this] { run(); });
}

CTranscriber::~CTranscriber() {
    {
        std::scoped_lock lock(m_lock);
        m_quit = true;
    }
    m_wake.notify_one();
    m_thread.join();

    transcribe_session_free(m_session);
}

void CTranscriber::setActive(bool active) {
    {
        std::scoped_lock lock(m_lock);
        m_active = active;
    }
    m_wake.notify_one();
}

void CTranscriber::pushAudio(const float* samples, size_t count) {
    std::scoped_lock lock(m_lock);
    if (!m_active)
        return;
    m_audio.insert(m_audio.end(), samples, samples + count);
    if (m_audio.size() >= MIN_FEED_SAMPLES)
        m_wake.notify_one();
}

void CTranscriber::run() {
    std::vector<float> audio;

    while (true) {
        bool active = false;
        {
            std::unique_lock ul(m_lock);
            m_wake.wait(ul, [this] { return m_quit || m_active != m_streaming || m_audio.size() >= MIN_FEED_SAMPLES; });
            if (m_quit)
                return;
            active = m_active;
            audio.swap(m_audio);
        }

        // Audio that came with a start was captured after it; with a stop,
        // before it.
        if (active && !m_streaming && !begin())
            continue;
        if (m_streaming && !audio.empty())
            feed(audio);
        audio.clear();
        if (!active && m_streaming)
            finish();
    }
}

bool CTranscriber::begin() {
    transcribe_run_params run;
    transcribe_run_params_init(&run);
    if (!m_options.language.empty())
        run.language = m_options.language.c_str();

    transcribe_stream_params stream;
    transcribe_stream_params_init(&stream);

    // The latency, as the model family's own setting.
    transcribe_parakeet_stream_ext          nemotron;
    transcribe_parakeet_buffered_stream_ext parakeet;
    transcribe_voxtral_realtime_stream_ext  voxtral;
    const int                               STEPS = (m_options.latency + STEP_MS / 2) / STEP_MS;
    switch (m_options.latency < 0 ? eFamily::NONE : family(transcribe_get_model(m_session))) {
        case eFamily::NONE: break;
        case eFamily::NEMOTRON:
            transcribe_parakeet_stream_ext_init(&nemotron);
            nemotron.att_context_right = STEPS;
            stream.family              = &nemotron.ext;
            break;
        case eFamily::PARAKEET: {
            // The pair nearest the latency.
            const auto [CHUNK, LOOKAHEAD] = std::ranges::min(PARAKEET_PAIRS, {}, [STEPS](const auto& p) { return std::abs(p.first + p.second - STEPS); });
            transcribe_parakeet_buffered_stream_ext_init(&parakeet);
            parakeet.chunk_ms = CHUNK * STEP_MS;
            parakeet.right_ms = LOOKAHEAD * STEP_MS;
            stream.family     = &parakeet.ext;
            break;
        }
        case eFamily::VOXTRAL:
            transcribe_voxtral_realtime_stream_ext_init(&voxtral);
            voxtral.num_delay_tokens = STEPS;
            stream.family            = &voxtral.ext;
            break;
    }

    if (const auto RET = transcribe_stream_begin(m_session, &run, &stream); RET != TRANSCRIBE_OK) {
        transcribe_stream_reset(m_session);
        {
            // Don't retry until the next start.
            std::scoped_lock lock(m_lock);
            m_active = false;
        }
        publish(true, std::format("can't start the stream: {}", status(RET)));
        return false;
    }

    m_streaming = true;
    m_published = 0;
    return true;
}

void CTranscriber::feed(std::vector<float>& audio) {
    transcribe_stream_update update;
    transcribe_stream_update_init(&update);

    if (const auto RET = transcribe_stream_feed(m_session, audio.data(), static_cast<int>(audio.size()), &update); RET != TRANSCRIBE_OK) {
        {
            std::scoped_lock lock(m_lock);
            m_active = false;
        }
        finish(std::format("transcription failed: {}", status(RET)));
    } else if (update.committed_changed || update.tentative_changed)
        publish(false);
}

void CTranscriber::finish(std::string error) {
    if (error.empty()) {
        transcribe_stream_update update;
        transcribe_stream_update_init(&update);
        if (const auto RET = transcribe_stream_finalize(m_session, &update); RET != TRANSCRIBE_OK)
            error = std::format("transcription failed: {}", status(RET));
    }

    publish(true, std::move(error));
    transcribe_stream_reset(m_session);
    m_streaming = false;
}

void CTranscriber::publish(bool final, std::string error) {
    SUpdate                update{.final = final, .error = std::move(error)};

    transcribe_stream_text text;
    transcribe_stream_text_init(&text);
    if (m_streaming && transcribe_stream_get_text(m_session, &text) == TRANSCRIBE_OK) {
        const std::string_view FULL{text.full_text ? text.full_text : "", text.full_text_bytes};
        std::string_view       committed{text.committed_text ? text.committed_text : "", text.committed_text_bytes};

        // WATCH(https://github.com/handy-computer/transcribe.cpp/issues/215):
        // for Parakeet/Nemotron, committed_text stops growing after the first
        // sentence although every token is reported committed, so the rest
        // would stay tentative until the stream ends. When every token is
        // committed, take the whole transcript as committed. Remove this once a
        // transcribe.cpp release contains the fix (PR #218). Check:
        // committed_text keeps growing past a sentence end.
        const int TOKENS = transcribe_n_tokens(m_session);
        if (TOKENS > 0 && transcribe_stream_n_committed_tokens(m_session) >= TOKENS)
            committed = FULL;

        // committed only grows: what's past the published length is new, and
        // the rest of the transcript is tentative.
        if (committed.size() > m_published) {
            update.committed = committed.substr(m_published);
            m_published      = committed.size();
        }
        update.tentative = FULL.substr(std::min(m_published, FULL.size()));
    }

    m_onUpdate(std::move(update));
}
