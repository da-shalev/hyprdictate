#include "Audio.hpp"
#include "InputMethod.hpp"
#include "Transcriber.hpp"

#include <hyprdictate_core-client.hpp>
#include <hyprdictate_core-server.hpp>
#include <hyprlang.hpp>
#include <hyprutils/cli/ArgumentParser.hpp>
#include <hyprutils/cli/Logger.hpp>
#include <hyprutils/eventLoop/EventLoop.hpp>
#include <hyprutils/memory/WeakPtr.hpp>
#include <hyprutils/path/Path.hpp>
#include <hyprwire/hyprwire.hpp>

#include <any>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <unistd.h>
#include <vector>

using namespace Hyprutils;
using Memory::CSharedPointer;
using Memory::CWeakPointer;
using Memory::makeShared;

constexpr uint32_t PROTOCOL_VERSION = 1;

static std::string socketPath() {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        throw std::runtime_error("XDG_RUNTIME_DIR is not set");
    return std::string{dir} + "/hyprdictate.sock";
}

static Hyprutils::CLI::CLogger logger;

// The daemon's settings, from the file given with --config or else
// hyprdictate.conf where the Hypr tools keep theirs ($XDG_CONFIG_HOME/hypr/,
// ~/.config/hypr/, $XDG_CONFIG_DIRS/hypr/, /etc/xdg/hypr/).
static CTranscriber::SOptions loadConfig(std::optional<std::string> path) {
    if (!path)
        path = Path::findConfig("hyprdictate").first;
    if (!path)
        // As hypridle words it (src/main.cpp).
        throw std::runtime_error("No hyprdictate.conf file found in:\n"
                                 "    $XDG_CONFIG_HOME/hypr/, ~/.config/hypr/, [XDG_CONFIG_DIRS]/hypr/, /etc/xdg/hypr/\n"
                                 "Create a config or specify one manually:\n"
                                 "    hyprdictate -c /path/to/conf");
    // hyprlang throws a bare string for a missing file.
    if (!std::filesystem::exists(*path))
        throw std::runtime_error(std::format("Specified file not found: {}", *path));

    // Settings live in `general`, so a misspelled one is an error: hyprlang
    // passes unknown top-level keys over as handlers.
    Hyprlang::CConfig config{path->c_str(), Hyprlang::SConfigOptions{.throwAllErrors = true}};
    config.addConfigValue("general:model", Hyprlang::STRING{""});
    config.addConfigValue("general:language", Hyprlang::STRING{""});
    config.addConfigValue("general:lookahead", Hyprlang::INT{-1});
    config.commence();
    if (const auto RESULT = config.parse(); RESULT.error)
        throw std::runtime_error(std::format("Config has errors:\n{}", RESULT.getError()));

    const std::string MODEL = std::any_cast<Hyprlang::STRING>(config.getConfigValue("general:model"));
    if (MODEL.empty())
        throw std::runtime_error(std::format("{}: general:model is not set", *path));
    // ~ and paths relative to the config's directory.
    const auto RESOLVED = Path::resolvePath(MODEL, std::filesystem::path{*path}.parent_path());
    if (!RESOLVED)
        throw std::runtime_error(std::format("{}: model {}: {}", *path, MODEL, RESOLVED.error()));

    return {
        .model     = *RESOLVED,
        .language  = std::any_cast<Hyprlang::STRING>(config.getConfigValue("general:language")),
        .lookahead = static_cast<int>(std::any_cast<Hyprlang::INT>(config.getConfigValue("general:lookahead"))),
    };
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
        m_transcriber(options, [this, executor = m_loop->executor()](CTranscriber::SUpdate update) { executor->post([this, update = std::move(update)] { onUpdate(update); }); }) {
        const auto      PATH = socketPath();
        std::error_code ec;
        std::filesystem::remove(PATH, ec);

        m_socket = Hyprwire::IServerSocket::open(PATH);
        if (!m_socket)
            throw std::runtime_error("can't open the control socket " + PATH);

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
        logger.log(Hyprutils::CLI::LOG_DEBUG, "ready");

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
        const auto MANAGER = m_managers.emplace_back(makeShared<CHyprdictateManagerObject>(std::move(object)));

        MANAGER->setToggle([this] { setDictating(!m_dictating); });
        MANAGER->setOnDestroy([this, weak = CWeakPointer<CHyprdictateManagerObject>{MANAGER}] { std::erase(m_managers, weak); });
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
            logger.log(Hyprutils::CLI::LOG_ERR, "{}", update.error);
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
    const auto SOCKET = Hyprwire::IClientSocket::open(socketPath());
    if (!SOCKET) {
        logger.log(Hyprutils::CLI::LOG_ERR, "hyprdictate isn't running (no {})", socketPath());
        return 1;
    }

    const auto IMPL = makeShared<CCHyprdictateCoreImpl>(PROTOCOL_VERSION);
    SOCKET->addImplementation(IMPL);

    if (!SOCKET->waitForHandshake() || !SOCKET->getSpec(IMPL->protocol()->specName())) {
        logger.log(Hyprutils::CLI::LOG_ERR, "the daemon doesn't speak the hyprdictate protocol");
        return 1;
    }

    const auto MANAGER = makeShared<CCHyprdictateManagerObject>(SOCKET->bindProtocol(IMPL->protocol(), PROTOCOL_VERSION));
    MANAGER->sendToggle();
    // Returns once the daemon has handled it.
    SOCKET->roundtrip();
    return 0;
}

// Everything is caught; only logging that last error could still throw, which
// can only end the process anyway.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
    try {
        std::vector<const char*> args(argv, argv + argc);
        CLI::CArgumentParser     parser{args};
        // WATCH(no upstream report yet): hyprutils' help table never wraps a
        // description's last word, so one wider than 58 columns breaks the
        // table's border (src/cli/ArgumentParser.cpp, wrap()). Keep these
        // short until that's fixed. Check: a longer description lines up.
        for (const auto& registered : {
                 parser.registerBoolOption("toggle", "t", "Start or stop dictating into the focused text field"),
                 parser.registerStringOption("config", "c", "Use this config instead of hyprdictate.conf"),
                 parser.registerBoolOption("version", "V", "Show hyprdictate's version"),
                 parser.registerBoolOption("help", "h", "Show this help"),
             }) {
            if (!registered) {
                logger.log(Hyprutils::CLI::LOG_CRIT, "{}", registered.error());
                return 1;
            }
        }

        if (const auto PARSED = parser.parse(); !PARSED) {
            logger.log(Hyprutils::CLI::LOG_ERR, "Failed parsing arguments: {}", PARSED.error());
            return 1;
        }

        if (parser.getBool("help").value_or(false)) {
            std::println("{}", parser.getDescription(std::format("hyprdictate v{}", HYPRDICTATE_VERSION)));
            return 0;
        }

        if (parser.getBool("version").value_or(false)) {
            std::println("hyprdictate v{}", HYPRDICTATE_VERSION);
            return 0;
        }

        if (parser.getBool("toggle").value_or(false))
            return sendToggle();

        const auto CONFIG = parser.getString("config");
        CDaemon{loadConfig(CONFIG ? std::optional<std::string>{*CONFIG} : std::nullopt)}.run();
    } catch (const std::exception& e) {
        logger.log(Hyprutils::CLI::LOG_CRIT, "{}", e.what());
        return 1;
    } catch (...) {
        logger.log(Hyprutils::CLI::LOG_CRIT, "unknown error");
        return 1;
    }
}
