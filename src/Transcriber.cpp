#include "Transcriber.hpp"

#include <transcribe.h>
#include <transcribe/parakeet.h>

#include <format>
#include <print>
#include <stdexcept>
#include <utility>

// 16 kHz mono float PCM is fed in blocks of at least 100 ms; the model
// buffers up to its own chunk size.
constexpr size_t MIN_FEED_SAMPLES = 1600;

static std::string status(int s) {
    return transcribe_status_string(s);
}

CTranscriber::CTranscriber(const SOptions& options, UpdateFn onUpdate) : m_options(options), m_onUpdate(std::move(onUpdate)) {
    if (const auto s = transcribe_init_backends_default(); s != TRANSCRIBE_OK)
        throw std::runtime_error(std::format("can't initialize compute backends: {}", status(s)));

    if (const auto s = transcribe_open(m_options.model.c_str(), nullptr, nullptr, &m_session); s != TRANSCRIBE_OK)
        throw std::runtime_error(std::format("can't load model {}: {}", m_options.model, status(s)));

    std::println(stderr, "loaded {} on {}", m_options.model, transcribe_model_backend(transcribe_get_model(m_session)));

    m_thread = std::thread([this] { run(); });
}

CTranscriber::~CTranscriber() {
    {
        std::lock_guard lg(m_lock);
        m_quit = true;
    }
    m_wake.notify_one();
    m_thread.join();

    transcribe_session_free(m_session);
}

void CTranscriber::setActive(bool active) {
    {
        std::lock_guard lg(m_lock);
        m_active = active;
    }
    m_wake.notify_one();
}

void CTranscriber::pushAudio(const float* samples, size_t count) {
    std::lock_guard lg(m_lock);
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

    transcribe_parakeet_stream_ext lookahead;
    transcribe_parakeet_stream_ext_init(&lookahead);
    if (m_options.lookahead >= 0 &&
        transcribe_model_accepts_ext_kind(transcribe_get_model(m_session), TRANSCRIBE_EXT_SLOT_STREAM, TRANSCRIBE_EXT_KIND_PARAKEET_STREAM)) {
        lookahead.att_context_right = m_options.lookahead;
        stream.family               = &lookahead.ext;
    }

    if (const auto s = transcribe_stream_begin(m_session, &run, &stream); s != TRANSCRIBE_OK) {
        transcribe_stream_reset(m_session);
        {
            // Don't retry until the next start.
            std::lock_guard lg(m_lock);
            m_active = false;
        }
        publish(true, std::format("can't start the stream: {}", status(s)));
        return false;
    }

    m_streaming = true;
    m_published = 0;
    return true;
}

void CTranscriber::feed(std::vector<float>& audio) {
    transcribe_stream_update update;
    transcribe_stream_update_init(&update);

    if (const auto s = transcribe_stream_feed(m_session, audio.data(), static_cast<int>(audio.size()), &update); s != TRANSCRIBE_OK) {
        {
            std::lock_guard lg(m_lock);
            m_active = false;
        }
        finish(std::format("transcription failed: {}", status(s)));
    } else if (update.committed_changed || update.tentative_changed)
        publish(false);
}

void CTranscriber::finish(std::string error) {
    if (error.empty()) {
        transcribe_stream_update update;
        transcribe_stream_update_init(&update);
        if (const auto s = transcribe_stream_finalize(m_session, &update); s != TRANSCRIBE_OK)
            error = std::format("transcription failed: {}", status(s));
    }

    publish(true, std::move(error));
    transcribe_stream_reset(m_session);
    m_streaming = false;
}

void CTranscriber::publish(bool final, std::string error) {
    SUpdate update{.final = final, .error = std::move(error)};

    transcribe_stream_text text;
    transcribe_stream_text_init(&text);
    if (m_streaming && transcribe_stream_get_text(m_session, &text) == TRANSCRIBE_OK) {
        // committed_text only grows: what's past the published length is new.
        const std::string_view committed{text.committed_text ? text.committed_text : "", text.committed_text_bytes};
        if (committed.size() > m_published) {
            update.committed = committed.substr(m_published);
            m_published      = committed.size();
        }
        if (text.tentative_text)
            update.tentative.assign(text.tentative_text, text.tentative_text_bytes);
    }

    m_onUpdate(std::move(update));
}
