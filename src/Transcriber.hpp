#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct transcribe_session;

// Runs a streaming speech model on its own thread. Audio goes in from any
// thread; text comes out as updates the main loop collects when eventFD()
// turns readable.
class CTranscriber {
  public:
    struct SOptions {
        std::string model;
        std::string language;
        // Encoder lookahead in 80 ms frames for cache-aware models; -1 keeps
        // the model's default.
        int lookahead = -1;
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

    explicit CTranscriber(const SOptions& options);
    ~CTranscriber();

    CTranscriber(const CTranscriber&)            = delete;
    CTranscriber& operator=(const CTranscriber&) = delete;

    // Starts or stops a dictation; the worker follows.
    void                 setActive(bool active);
    void                 pushAudio(const float* samples, size_t count);

    int                  eventFD() const;
    std::vector<SUpdate> takeUpdates();

  private:
    void                    run();
    bool                    begin();
    void                    feed(std::vector<float>& audio);
    void                    finish(std::string error = {});
    void                    publish(bool final, std::string error = {});

    SOptions                m_options;
    transcribe_session*     m_session = nullptr;
    int                     m_eventFD = -1;

    // Shared with the worker.
    std::mutex              m_lock;
    std::condition_variable m_wake;
    bool                    m_active = false;
    bool                    m_quit   = false;
    std::vector<float>      m_audio;
    std::vector<SUpdate>    m_updates;

    // Worker only.
    bool                    m_streaming = false;
    size_t                  m_published = 0;

    std::thread             m_thread;
};
