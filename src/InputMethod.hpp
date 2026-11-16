#pragma once

#include <hyprutils/memory/SharedPtr.hpp>

#include <cstdint>
#include <string>

struct wl_display;
struct wl_registry;
struct wl_seat;
class CCZwpInputMethodManagerV2;
class CCZwpInputMethodV2;

// The seat's input method (zwp_input_method_v2): puts text into the focused
// text field.
class CInputMethod {
  public:
    CInputMethod();
    ~CInputMethod();

    CInputMethod(const CInputMethod&)            = delete;
    CInputMethod& operator=(const CInputMethod&) = delete;

    int         fd() const;
    // Before poll(): dispatch queued events and flush requests.
    void        prepare();
    // After poll(): read what arrived (if readable) and dispatch it.
    void        dispatch(bool readable);
    // Another input method owns the seat.
    bool        unavailable() const;

    // Starts a dictation at the cursor.
    void        begin();
    // Commits text the model won't revise; shows the rest as the preedit.
    void        update(const std::string& committed, const std::string& tentative);
    // Ends the dictation.
    void        end();

    // wl_registry's global event.
    static void onGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version);

  private:
    struct SState {
        bool        active = false;
        // Text around the cursor and the cursor's byte offset in it, when the
        // field reports them.
        std::string surrounding;
        uint32_t    cursor = 0;
    };

    void                                                         send(const std::string& commit, const std::string& preedit);

    wl_display*                                                  m_display  = nullptr;
    wl_registry*                                                 m_registry = nullptr;
    wl_seat*                                                     m_seat     = nullptr;
    Hyprutils::Memory::CSharedPointer<CCZwpInputMethodManagerV2> m_manager;
    Hyprutils::Memory::CSharedPointer<CCZwpInputMethodV2>        m_method;

    // Events build the pending state; done applies it.
    SState                                                       m_pending;
    SState                                                       m_current;
    uint32_t                                                     m_serial      = 0;
    bool                                                         m_unavailable = false;

    // Spaces around the dictated text, decided at begin() from the text
    // around the cursor.
    std::string                                                  m_before;
    std::string                                                  m_after;
};
