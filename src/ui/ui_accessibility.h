#ifndef __UI_ACCESSIBILITY_H__
#define __UI_ACCESSIBILITY_H__

#include <string>

namespace Rml {
    class Context;
    class Element;
}

// Screen reader support for the RmlUi menus (launcher, config, mods).
namespace recompui::accessibility {
    // Speaks the focused element when focus has changed since the last call.
    // Call once per frame after focus updates. user_input should be true when
    // keyboard, controller or mouse input arrived this frame: focus moved by
    // the user interrupts current speech, while focus that moved on its own
    // (e.g. a newly shown menu) is queued so it doesn't cut off other messages.
    void announce_focus(Rml::Context* context, bool user_input);

    // Forget the last announced element, so the next focus is spoken even if
    // it is the same element (e.g. when a menu is reopened).
    void reset_focus();

    // Speaks a prompt or notification as it opens ("Header. Content."). The
    // focused button is announced after it. Safe to call from any thread.
    void announce_message(const std::string& header, const std::string& content);

    // The text a screen reader should say for an element: its aria-label
    // attribute if set, otherwise its visible text with decorative bullets
    // removed and whitespace collapsed.
    std::string accessible_name(Rml::Element* element);
}

#endif
