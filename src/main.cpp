#include "Audio.hpp"
#include "InputMethod.hpp"
#include "Transcriber.hpp"

#include <hyprdictate_core-client.hpp>
#include <hyprdictate_core-server.hpp>
#include <hyprutils/cli/ArgumentParser.hpp>
#include <hyprutils/eventLoop/EventLoop.hpp>
#include <hyprwire/hyprwire.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

using namespace Hyprutils;
using Memory::CSharedPointer;
using Memory::makeShared;

constexpr uint32_t PROTOCOL_VERSION = 1;

static std::string socketPath() {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        throw std::runtime_error("XDG_RUNTIME_DIR is not set");
    return std::string{dir} + "/hyprdictate.sock";
}

static CSharedPointer<EventLoop::IEventLoop> createLoop() {
    auto loop = EventLoop::IEventLoop::create();
    if (!loop)
        throw std::runtime_error(loop.error());
    return *loop;
}

class CDaemon {
  public:
    explicit CDaemon(const CTranscriber::SOptions& options) :
        m_transcriber(options, [this, executor = m_loop->executor()](CTranscriber::SUpdate update) {
            executor->post([this, update = std::move(update)] { onUpdate(update); });
        }) {
        const auto path = socketPath();
        std::error_code ec;
        std::filesystem::remove(path, ec);

        m_socket = Hyprwire::IServerSocket::open(path);
        if (!m_socket)
            throw std::runtime_error("can't open the control socket " + path);

        m_impl = makeShared<CHyprdictateCoreImpl>(PROTOCOL_VERSION, [this](CSharedPointer<Hyprwire::IObject> object) { bind(std::move(object)); });
        m_socket->addImplementation(m_impl);

        watch(m_inputMethod.fd(), [this] {
            if (!m_inputMethod.dispatch())
                fail("lost the Wayland connection");
            else if (m_inputMethod.unavailable())
                fail("another input method is running on this seat");
        });
        watch(m_socket->extractLoopFD(), [this] { m_socket->dispatchEvents(false); });
        m_flush = m_loop->addPostDispatch([this] { m_inputMethod.flush(); });
    }

    // Runs until killed: the compositor drops the input method and PipeWire
    // the stream when the process goes away.
    [[noreturn]] void run() {
        m_inputMethod.flush();
        std::println(stderr, "ready");

        if (auto result = m_loop->enterLoop(); !result)
            throw std::runtime_error(result.error());
        throw std::runtime_error(m_error);
    }

  private:
    // Runs fn on the loop whenever fd is readable. The loop owns what it's
    // given, so it gets a duplicate of a descriptor its library keeps.
    void watch(int fd, std::function<void()> fn) {
        auto source = m_loop->addFD(OS::CFileDescriptor{dup(fd)}, EventLoop::eEventMask::READABLE, [fn = std::move(fn)](EventLoop::IFDSource&, EventLoop::FdEventMask) { fn(); });
        if (!source)
            throw std::runtime_error(source.error());
        m_sources.emplace_back(*source);
    }

    void fail(std::string error) {
        m_error = std::move(error);
        m_loop->stop();
    }

    void bind(CSharedPointer<Hyprwire::IObject>&& object) {
        auto  manager = m_managers.emplace_back(makeShared<CHyprdictateManagerObject>(std::move(object)));
        auto* raw     = manager.get();

        manager->setToggle([this] { setDictating(!m_dictating); });
        manager->setOnDestroy([this, raw] { std::erase_if(m_managers, [raw](const auto& m) { return m.get() == raw; }); });
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

    CSharedPointer<EventLoop::IEventLoop>                  m_loop = createLoop();
    CInputMethod                                           m_inputMethod;
    CTranscriber                                           m_transcriber;
    CAudio                                                 m_audio{[this](const float* samples, size_t count) { m_transcriber.pushAudio(samples, count); }};

    CSharedPointer<Hyprwire::IServerSocket>                m_socket;
    CSharedPointer<CHyprdictateCoreImpl>                   m_impl;
    std::vector<CSharedPointer<CHyprdictateManagerObject>> m_managers;

    std::vector<CSharedPointer<EventLoop::IFDSource>>      m_sources;
    CSharedPointer<EventLoop::IPostDispatchHook>           m_flush;

    bool                                                   m_dictating = false;
    std::string                                            m_error;
};

static int sendToggle() {
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

    auto manager = makeShared<CCHyprdictateManagerObject>(socket->bindProtocol(impl->protocol(), PROTOCOL_VERSION));
    manager->sendToggle();
    // Returns once the daemon has handled it.
    socket->roundtrip();
    return 0;
}

int main(int argc, char** argv) {
    std::vector<const char*> args(argv, argv + argc);
    CLI::CArgumentParser     parser{args};
    (void)parser.registerStringOption("model", "m", "Run the daemon with this streaming transcribe.cpp GGUF");
    (void)parser.registerStringOption("language", "l", "Language code the model expects, e.g. en-US");
    (void)parser.registerIntOption("lookahead", "a", "Encoder lookahead in 80 ms frames (0, 3, 6 or 13): lower is faster, higher more accurate");
    (void)parser.registerBoolOption("toggle", "t", "Start dictating into the focused text field, or stop");
    (void)parser.registerBoolOption("help", "h", "Show this help");

    const auto parsed = parser.parse();
    const auto model  = parser.getString("model");
    const bool help   = parser.getBool("help").value_or(false);
    const bool toggle = parser.getBool("toggle").value_or(false);

    if (!parsed || help || (!model && !toggle)) {
        if (!parsed)
            std::println(stderr, "{}", parsed.error());
        std::println("{}", parser.getDescription("hyprdictate: streaming dictation into the focused text field"));
        return parsed && help ? 0 : 1;
    }

    try {
        if (toggle)
            return sendToggle();

        CDaemon{{
                    .model     = std::string{*model},
                    .language  = std::string{parser.getString("language").value_or("")},
                    .lookahead = parser.getInt("lookahead").value_or(-1),
                }}
            .run();
    } catch (const std::exception& e) {
        std::println(stderr, "{}", e.what());
        return 1;
    }
}
