#pragma once

#include <cstddef>
#include <functional>

struct pw_thread_loop;
struct pw_stream;

// Captures the default PipeWire source as 16 kHz mono float PCM. The stream
// exists only between start() and stop(), so the microphone is only open
// while dictating.
class CAudio {
  public:
    // Called on PipeWire's thread.
    using SamplesFn = std::function<void(const float* samples, size_t count)>;

    explicit CAudio(SamplesFn onSamples);
    ~CAudio();

    CAudio(const CAudio&)            = delete;
    CAudio& operator=(const CAudio&) = delete;

    bool    start();
    void    stop();

    // PipeWire's process callback.
    static void onProcess(void* data);

  private:
    SamplesFn       m_onSamples;
    pw_thread_loop* m_loop   = nullptr;
    pw_stream*      m_stream = nullptr;
};
