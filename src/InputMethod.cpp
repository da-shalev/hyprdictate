#include "InputMethod.hpp"

#include <input-method-unstable-v2.hpp>
#include <wayland.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <utility>

using Hyprutils::Memory::makeShared;

CInputMethod::CInputMethod() {
    m_display = wl_display_connect(nullptr);
    if (!m_display)
        throw std::runtime_error("can't connect to the Wayland display");

    m_registry = makeShared<CCWlRegistry>(reinterpret_cast<wl_proxy*>(wl_display_get_registry(m_display)));
    m_registry->setGlobal([this](CCWlRegistry* registry, uint32_t name, const char* interface, uint32_t version) {
        const auto bind = [&](const wl_interface* iface, uint32_t v) {
            return static_cast<wl_proxy*>(wl_registry_bind(reinterpret_cast<wl_registry*>(registry->resource()), name, iface, v));
        };
        if (!m_seat && std::strcmp(interface, wl_seat_interface.name) == 0)
            // 5 for the release request its destructor sends.
            m_seat = makeShared<CCWlSeat>(bind(&wl_seat_interface, std::min(version, 5u)));
        else if (std::strcmp(interface, zwp_input_method_manager_v2_interface.name) == 0)
            m_manager = makeShared<CCZwpInputMethodManagerV2>(bind(&zwp_input_method_manager_v2_interface, 1));
    });
    wl_display_roundtrip(m_display);

    if (!m_seat || !m_manager)
        throw std::runtime_error("the compositor doesn't support zwp_input_method_manager_v2");

    m_method = makeShared<CCZwpInputMethodV2>(m_manager->sendGetInputMethod(m_seat->resource()));

    m_method->setActivate([this](CCZwpInputMethodV2*) { m_pending = {.active = true}; });
    m_method->setDeactivate([this](CCZwpInputMethodV2*) { m_pending.active = false; });
    m_method->setSurroundingText([this](CCZwpInputMethodV2*, const char* text, uint32_t cursor, uint32_t) {
        m_pending.surrounding = text;
        m_pending.cursor      = cursor;
    });
    m_method->setDone([this](CCZwpInputMethodV2*) {
        // commit() carries the number of done events so far.
        ++m_serial;
        m_current = m_pending;
        // Surrounding text resets after each done; the field resends it.
        m_pending.surrounding.clear();
    });
    m_method->setUnavailable([this](CCZwpInputMethodV2*) { m_unavailable = true; });

    wl_display_roundtrip(m_display);
}

CInputMethod::~CInputMethod() {
    // The objects destroy themselves; the connection goes last.
    m_method.reset();
    m_manager.reset();
    m_seat.reset();
    m_registry.reset();
    wl_display_disconnect(m_display);
}

int CInputMethod::fd() const {
    return wl_display_get_fd(m_display);
}

bool CInputMethod::dispatch() {
    return wl_display_dispatch(m_display) >= 0;
}

void CInputMethod::flush() {
    wl_display_flush(m_display);
}

bool CInputMethod::unavailable() const {
    return m_unavailable;
}

void CInputMethod::begin() {
    // As FUTO Voice Input does: a space before the text when it would run
    // into the word before the cursor, and after it when a word follows.
    const auto& text   = m_current.surrounding;
    const auto  cursor = m_current.cursor;
    const auto  letter = [](unsigned char c) { return c >= 0x80 || std::isalpha(c); };

    m_before = cursor > 0 && cursor <= text.size() && !std::isspace(static_cast<unsigned char>(text[cursor - 1])) ? " " : "";
    m_after  = cursor < text.size() && letter(text[cursor]) ? " " : "";
}

void CInputMethod::update(const std::string& committed, const std::string& tentative) {
    send(committed.empty() ? "" : std::exchange(m_before, {}) + committed, tentative);
}

void CInputMethod::end() {
    // m_before still set means nothing was committed.
    send(m_before.empty() ? m_after : "", "");
}

void CInputMethod::send(const std::string& commit, const std::string& preedit) {
    if (!m_current.active)
        return;

    if (!commit.empty())
        m_method->sendCommitString(commit.c_str());
    // Each commit replaces the preedit; an empty one clears it.
    const int cursor = static_cast<int>(preedit.size());
    m_method->sendSetPreeditString(preedit.c_str(), cursor, cursor);
    m_method->sendCommit(m_serial);
}
