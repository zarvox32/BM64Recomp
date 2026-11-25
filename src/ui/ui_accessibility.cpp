#include "ui_accessibility.h"

#include <string>

#include "RmlUi/Core.h"
#include "RmlUi/Core/ElementText.h"

#include "bm64_screen_reader.h"

namespace {
    // Identity of the last announced element. Only compared, never dereferenced,
    // since the element may have been destroyed since.
    Rml::Element* last_focused = nullptr;
    // The first announcement after startup is queued behind the game title.
    bool announced_any = false;

    bool is_focusable(Rml::Element* element) {
        return element->GetOwnerDocument() != nullptr &&
            element->GetProperty(Rml::PropertyId::TabIndex)->Get<Rml::Style::TabIndex>() != Rml::Style::TabIndex::None;
    }

    void collect_text(Rml::Element* element, std::string& out) {
        if (!element->IsVisible()) {
            return;
        }
        if (auto* text = dynamic_cast<Rml::ElementText*>(element)) {
            out += text->GetText();
            out += ' ';
            return;
        }
        for (int i = 0; i < element->GetNumChildren(); i++) {
            collect_text(element->GetChild(i), out);
        }
    }

    // Removes the "•" bullets used as decoration in menu lists and collapses
    // runs of whitespace into single spaces.
    std::string clean_text(const std::string& text) {
        static const std::string bullet = "\xE2\x80\xA2"; // U+2022
        std::string out;
        bool pending_space = false;
        for (size_t i = 0; i < text.size(); i++) {
            if (text.compare(i, bullet.size(), bullet) == 0) {
                i += bullet.size() - 1;
                pending_space = true;
                continue;
            }
            char c = text[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                pending_space = true;
                continue;
            }
            if (pending_space && !out.empty()) {
                out += ' ';
            }
            pending_space = false;
            out += c;
        }
        return out;
    }

    // "N of M" among the visible, focusable siblings of an element, or empty if
    // it has no such siblings.
    std::string position_in_parent(Rml::Element* element) {
        Rml::Element* parent = element->GetParentNode();
        if (parent == nullptr) {
            return {};
        }
        int index = 0;
        int count = 0;
        for (int i = 0; i < parent->GetNumChildren(); i++) {
            Rml::Element* sibling = parent->GetChild(i);
            if (!sibling->IsVisible() || !is_focusable(sibling)) {
                continue;
            }
            count++;
            if (sibling == element) {
                index = count;
            }
        }
        if (count < 2 || index == 0) {
            return {};
        }
        return std::to_string(index) + " of " + std::to_string(count);
    }
}

std::string recompui::accessibility::accessible_name(Rml::Element* element) {
    std::string label = element->GetAttribute<Rml::String>("aria-label", "");
    if (!label.empty()) {
        return label;
    }
    std::string text;
    collect_text(element, text);
    return clean_text(text);
}

void recompui::accessibility::announce_focus(Rml::Context* context, bool user_input) {
    Rml::Element* focused = context->GetFocusElement();
    if (focused == last_focused) {
        return;
    }
    // Only announce real controls. When no control has focus, the context
    // reports a document or the root, whose text would be the whole screen.
    if (focused == nullptr || !is_focusable(focused) || !focused->IsVisible(true)) {
        return;
    }
    last_focused = focused;

    // An unnamed control (e.g. an empty decorative title button) still says
    // something, so moving onto it isn't silent.
    std::string name = accessible_name(focused);
    if (name.empty()) {
        name = "Blank";
    }
    std::string position = position_in_parent(focused);
    if (!position.empty()) {
        name += ", " + position;
    }
    bm64::screen_reader::speak(std::move(name), user_input && announced_any);
    announced_any = true;
}

void recompui::accessibility::reset_focus() {
    last_focused = nullptr;
}
