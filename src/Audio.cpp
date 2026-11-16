#include "Audio.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <print>
#include <stdexcept>

constexpr uint32_t SAMPLE_RATE = 16000;

static const pw_stream_events STREAM_EVENTS = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = &CAudio::onProcess,
};

CAudio::CAudio(SamplesFn onSamples) : m_onSamples(std::move(onSamples)) {
    pw_init(nullptr, nullptr);

    m_loop = pw_thread_loop_new("hyprdictate-audio", nullptr);
    if (!m_loop || pw_thread_loop_start(m_loop) < 0)
        throw std::runtime_error("can't start the PipeWire loop");
}

CAudio::~CAudio() {
    stop();
    pw_thread_loop_stop(m_loop);
    pw_thread_loop_destroy(m_loop);
    pw_deinit();
}

bool CAudio::start() {
    pw_thread_loop_lock(m_loop);

    if (m_stream) {
        pw_thread_loop_unlock(m_loop);
        return true;
    }

    auto* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_APP_NAME, "hyprdictate",
                                    PW_KEY_NODE_NAME, "hyprdictate", nullptr);

    m_stream = pw_stream_new_simple(pw_thread_loop_get_loop(m_loop), "dictation", props, &STREAM_EVENTS, this);

    uint8_t                buffer[1024];
    spa_pod_builder        builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw     info    = {.format = SPA_AUDIO_FORMAT_F32, .rate = SAMPLE_RATE, .channels = 1};
    const spa_pod*         params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};

    const auto             flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    const int              res   = m_stream ? pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) : -1;

    if (res < 0) {
        std::println(stderr, "can't open the microphone stream");
        if (m_stream)
            pw_stream_destroy(m_stream);
        m_stream = nullptr;
    }

    pw_thread_loop_unlock(m_loop);
    return m_stream;
}

void CAudio::stop() {
    pw_thread_loop_lock(m_loop);
    if (m_stream) {
        pw_stream_destroy(m_stream);
        m_stream = nullptr;
    }
    pw_thread_loop_unlock(m_loop);
}

void CAudio::onProcess(void* data) {
    auto* self = static_cast<CAudio*>(data);

    pw_buffer* b = pw_stream_dequeue_buffer(self->m_stream);
    if (!b)
        return;

    const spa_data& d = b->buffer->datas[0];
    if (d.data && d.chunk) {
        const auto* samples = reinterpret_cast<const float*>(static_cast<const uint8_t*>(d.data) + d.chunk->offset);
        self->m_onSamples(samples, d.chunk->size / sizeof(float));
    }

    pw_stream_queue_buffer(self->m_stream, b);
}
