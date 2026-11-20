#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct transcribe_session;

// Runs a streaming speech model on its own thread. Audio goes in from any
// thread; text comes out through the update callback, on that thread.
class CTranscriber {
  public:
    struct SOptions {
        std::string model;
        std::string language;
        // How soon words appear after they're spoken, in milliseconds, set as
        // each model family's own setting. -1 keeps the model's default.
        int latency = -1;
    };

    struct SUpdate {
        // Text the model won't revise, new since the previous update.
        std::string committed;
        // The rest of the current hypothesis, which may still change.
        std::string tentative;
        // Last update of a dictation.
        bool        final = false;
        std::string error;
    };

    using UpdateFn = std::function<void(SUpdate)>;

    CTranscriber(const SOptions& options, UpdateFn onUpdate);
    ~CTranscriber();

    CTranscriber(const CTranscriber&)            = delete;
    CTranscriber& operator=(const CTranscriber&) = delete;

    // Starts or stops a dictation; the worker follows.
    void setActive(bool active);
    void pushAudio(const float* samples, size_t count);

  private:
    void                run();
    bool                begin();
    void                feed(std::vector<float>& audio);
    void                finish(std::string error = {});
    void                publish(bool final, std::string error = {});

    SOptions            m_options;
    UpdateFn            m_onUpdate;
    transcribe_session* m_session = nullptr;

    // Shared with the worker.
    std::mutex              m_lock;
    std::condition_variable m_wake;
    bool                    m_active = false;
    bool                    m_quit   = false;
    std::vector<float>      m_audio;

    // Worker only.
    bool        m_streaming = false;
    size_t      m_published = 0;

    std::thread m_thread;
};
