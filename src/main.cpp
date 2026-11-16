#include "Audio.hpp"
#include "InputMethod.hpp"
#include "Transcriber.hpp"

#include <hyprdictate_core-client.hpp>
#include <hyprdictate_core-server.hpp>
#include <hyprwire/hyprwire.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <format>
#include <cstdlib>
#include <filesystem>
#include <poll.h>
#include <print>
#include <string>
#include <string_view>
#include <sys/signalfd.h>
#include <vector>

using Hyprutils::Memory::CSharedPointer;
using Hyprutils::Memory::makeShared;

constexpr uint32_t PROTOCOL_VERSION = 1;

constexpr const char* USAGE = R"(usage:
  hyprdictate daemon --model <file.gguf> [--language <code>] [--lookahead <frames>]
  hyprdictate start | stop | toggle

daemon      Run the input method and keep the speech model loaded.
              --model      a streaming transcribe.cpp GGUF
              --language   language code the model expects, e.g. en-US
              --lookahead  encoder lookahead in 80 ms frames (cache-aware
                           models: 0, 3, 6 or 13); lower is faster, higher
                           more accurate
start/stop  Start or stop dictating into the focused text field.
toggle      Start, or stop if already dictating.)";

static std::string socketPath() {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        throw std::runtime_error("XDG_RUNTIME_DIR is not set");
    return std::string{dir} + "/hyprdictate.sock";
}

class CDaemon {
  public:
    explicit CDaemon(const CTranscriber::SOptions& options) : m_transcriber(options) {
        const auto path = socketPath();
        std::error_code ec;
        std::filesystem::remove(path, ec);

        m_socket = Hyprwire::IServerSocket::open(path);
        if (!m_socket)
            throw std::runtime_error("can't open the control socket " + path);

        m_impl = makeShared<CHyprdictateCoreImpl>(PROTOCOL_VERSION, [this](CSharedPointer<Hyprwire::IObject> object) { bind(std::move(object)); });
        m_socket->addImplementation(m_impl);
    }

    int run() {
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        sigprocmask(SIG_BLOCK, &signals, nullptr);
        const int signalFD = signalfd(-1, &signals, SFD_CLOEXEC);

        pollfd fds[] = {
            {.fd = m_inputMethod.fd(), .events = POLLIN},
            {.fd = m_socket->extractLoopFD(), .events = POLLIN},
            {.fd = m_transcriber.eventFD(), .events = POLLIN},
            {.fd = signalFD, .events = POLLIN},
        };

        std::println(stderr, "ready");

        while (true) {
            m_inputMethod.prepare();
            if (poll(fds, std::size(fds), -1) < 0) {
                m_inputMethod.dispatch(false);
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("poll failed");
            }

            m_inputMethod.dispatch(fds[0].revents & POLLIN);
            if (m_inputMethod.unavailable())
                throw std::runtime_error("another input method is running on this seat");

            if (fds[1].revents & POLLIN)
                m_socket->dispatchEvents(false);

            if (fds[2].revents & POLLIN)
                for (const auto& update : m_transcriber.takeUpdates())
                    onUpdate(update);

            if (fds[3].revents & POLLIN)
                break;
        }

        setDictating(false);
        return 0;
    }

  private:
    void bind(CSharedPointer<Hyprwire::IObject>&& object) {
        auto manager = m_managers.emplace_back(makeShared<CHyprdictateManagerObject>(std::move(object)));
        auto* raw    = manager.get();

        manager->setStart([this] { setDictating(true); });
        manager->setStop([this] { setDictating(false); });
        manager->setToggle([this] { setDictating(!m_dictating); });
        manager->setOnDestroy([this, raw] { std::erase_if(m_managers, [raw](const auto& m) { return m.get() == raw; }); });

        manager->sendState(m_dictating);
    }

    void setDictating(bool on) {
        if (on && !m_dictating) {
            m_inputMethod.begin();
            m_transcriber.setActive(true);
            on = m_audio.start();
        }
        if (!on) {
            m_audio.stop();
            // The last words arrive with the final update.
            m_transcriber.setActive(false);
        }
        m_dictating = on;

        for (const auto& manager : m_managers)
            manager->sendState(m_dictating);
    }

    void onUpdate(const CTranscriber::SUpdate& update) {
        m_inputMethod.update(update.committed, update.final ? "" : update.tentative);
        if (!update.final)
            return;

        m_inputMethod.end();
        if (!update.error.empty()) {
            std::println(stderr, "{}", update.error);
            setDictating(false);
        }
    }

    CInputMethod                                         m_inputMethod;
    CTranscriber                                         m_transcriber;
    CAudio                                               m_audio{[this](const float* samples, size_t count) { m_transcriber.pushAudio(samples, count); }};

    CSharedPointer<Hyprwire::IServerSocket>              m_socket;
    CSharedPointer<CHyprdictateCoreImpl>                 m_impl;
    std::vector<CSharedPointer<CHyprdictateManagerObject>> m_managers;

    bool                                                 m_dictating = false;
};

static int runClient(std::string_view command) {
    auto socket = Hyprwire::IClientSocket::open(socketPath());
    if (!socket) {
        std::println(stderr, "hyprdictate isn't running (no {})", socketPath());
        return 1;
    }

    auto impl = makeShared<CCHyprdictateCoreImpl>(PROTOCOL_VERSION);
    socket->addImplementation(impl);

    if (!socket->waitForHandshake() || !socket->getSpec(impl->protocol()->specName())) {
        std::println(stderr, "the daemon doesn't speak the hyprdictate protocol");
        return 1;
    }

    auto     manager   = makeShared<CCHyprdictateManagerObject>(socket->bindProtocol(impl->protocol(), PROTOCOL_VERSION));
    uint32_t dictating = 0;
    manager->setState([&dictating](uint32_t state) { dictating = state; });

    if (command == "start")
        manager->sendStart();
    else if (command == "stop")
        manager->sendStop();
    else
        manager->sendToggle();

    socket->roundtrip();
    std::println("{}", dictating ? "dictating" : "stopped");
    return 0;
}

static int runDaemon(std::vector<std::string_view> args) {
    CTranscriber::SOptions options;

    for (size_t i = 0; i < args.size(); ++i) {
        const auto value = [&]() -> std::string {
            if (i + 1 >= args.size())
                throw std::runtime_error(std::format("{} needs a value", args[i]));
            return std::string{args[++i]};
        };

        if (args[i] == "--model")
            options.model = value();
        else if (args[i] == "--language")
            options.language = value();
        else if (args[i] == "--lookahead")
            options.lookahead = std::stoi(value());
        else
            throw std::runtime_error(std::format("unknown option {}", args[i]));
    }

    if (options.model.empty())
        throw std::runtime_error("--model is required");

    CDaemon daemon{options};
    return daemon.run();
}

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);

    try {
        if (args.empty())
            throw std::runtime_error(USAGE);

        if (args[0] == "daemon")
            return runDaemon({args.begin() + 1, args.end()});
        if ((args[0] == "start" || args[0] == "stop" || args[0] == "toggle") && args.size() == 1)
            return runClient(args[0]);

        throw std::runtime_error(USAGE);
    } catch (const std::exception& e) {
        std::println(stderr, "{}", e.what());
        return 1;
    }
}
