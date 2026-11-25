#include "ui_accessibility.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
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
    // What last_focused's value was when last spoken (e.g. "50%" or "checked"),
    // so changes made without moving focus can be announced.
    std::string last_state;
    // The settings option block (.config-option) of the last announcement, so
    // moving between choices of one option doesn't repeat its title.
    Rml::Element* last_option = nullptr;
    // Frames until a newly focused option's description is read (0 = none
    // pending). It appears a frame or two after focus moves, because it follows
    // the menu's data model.
    int description_delay = 0;
    // A focused control with no text yet, and how many frames it has waited.
    Rml::Element* unnamed_candidate = nullptr;
    int unnamed_frames = 0;
    // Queue the next announcement instead of interrupting (it follows a prompt).
    bool queue_next = false;
    // Set by announce_message, which may run on another thread; everything else
    // here runs on the UI thread under the UI lock.
    std::atomic<bool> message_spoken{ false };

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

    std::string text_of(Rml::Element* element) {
        std::string text;
        collect_text(element, text);
        return clean_text(text);
    }

    bool is_input_type(Rml::Element* element, const char* type) {
        return element->GetTagName() == "input" && element->GetAttribute<Rml::String>("type", "") == type;
    }

    bool is_choice(Rml::Element* element) {
        return is_input_type(element, "radio") || is_input_type(element, "checkbox");
    }

    // The text of the <label for="id"> beside a radio button or checkbox.
    std::string label_for(Rml::Element* input) {
        const Rml::String& id = input->GetId();
        Rml::Element* parent = input->GetParentNode();
        if (id.empty() || parent == nullptr) {
            return {};
        }
        for (int i = 0; i < parent->GetNumChildren(); i++) {
            Rml::Element* child = parent->GetChild(i);
            if (child->GetTagName() == "label" && child->GetAttribute<Rml::String>("for", "") == id) {
                return text_of(child);
            }
        }
        return {};
    }

    // The part of a control's announcement that can change while it keeps
    // focus: a slider's value or whether a choice is checked.
    std::string control_state(Rml::Element* element) {
        if (is_input_type(element, "range")) {
            // Settings sliders show their value, with units, in a label beside them.
            if (Rml::Element* option = element->Closest(".config-option")) {
                if (Rml::Element* value_label = option->QuerySelector(".config-option__range-label")) {
                    std::string value = text_of(value_label);
                    if (!value.empty()) {
                        return value;
                    }
                }
            }
            return element->GetAttribute<Rml::String>("value", "");
        }
        if (is_choice(element)) {
            return element->HasAttribute("checked") ? "checked" : "not checked";
        }
        // Moving onto a tab doesn't open it; Enter does. Say which one is open.
        if (element->GetTagName() == "tab" && element->IsPseudoClassSet("selected")) {
            return "selected";
        }
        return {};
    }

    // The kind of control, where it isn't obvious from the name.
    std::string control_role(Rml::Element* element) {
        if (is_input_type(element, "range")) {
            return "slider";
        }
        if (element->GetTagName() == "tab") {
            return "tab";
        }
        return {};
    }

    // The description the settings menus show beside the focused option, if any.
    // Options sit in the first .config__wrapper column of a .config__hz-wrapper
    // and descriptions in the next; only the matching description is visible.
    std::string option_description(Rml::Element* option) {
        Rml::Element* columns = option->Closest(".config__hz-wrapper");
        Rml::Element* options_column = option->Closest(".config__wrapper");
        if (columns == nullptr) {
            return {};
        }
        for (int i = 0; i < columns->GetNumChildren(); i++) {
            Rml::Element* column = columns->GetChild(i);
            if (column != options_column && column->IsClassSet("config__wrapper")) {
                return text_of(column);
            }
        }
        return {};
    }

    void append_part(std::string& text, const std::string& part) {
        if (part.empty()) {
            return;
        }
        if (!text.empty()) {
            text += ", ";
        }
        text += part;
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
    // A prompt was just spoken: announce its focused button after it, even if
    // that button had focus in a previous prompt.
    if (message_spoken.exchange(false)) {
        reset_focus();
        queue_next = true;
    }

    Rml::Element* focused = context->GetFocusElement();
    // Only announce real controls. When no control has focus, the context
    // reports a document or the root, whose text would be the whole screen.
    if (focused == nullptr || !is_focusable(focused) || !focused->IsVisible(true)) {
        return;
    }

    if (focused == last_focused) {
        // Announce value changes made without moving focus.
        std::string state = control_state(focused);
        if (state != last_state) {
            last_state = state;
            bm64::screen_reader::speak(state, true);
        }
        // Look the option up again rather than keeping a pointer to it, since
        // the focused element is the one thing known to still exist.
        if (description_delay > 0 && --description_delay == 0) {
            if (Rml::Element* option = focused->Closest(".config-option")) {
                bm64::screen_reader::speak(option_description(option), false);
            }
        }
        return;
    }
    std::string state = control_state(focused);

    // In the settings menus, a control's name is its option's title. Choices
    // (radio buttons) are also named by their own label, and the title is only
    // repeated when moving to a different option.
    Rml::Element* option = focused->Closest(".config-option");
    bool new_option = option != last_option;

    std::string name;
    if (option != nullptr) {
        if (new_option) {
            if (Rml::Element* title = option->QuerySelector(".config-option__title")) {
                append_part(name, text_of(title));
            }
        }
        if (is_choice(focused)) {
            append_part(name, label_for(focused));
        }
        else if (!is_input_type(focused, "range")) {
            append_part(name, accessible_name(focused));
        }
    }
    else {
        name = accessible_name(focused);
    }
    if (name.empty() && state.empty()) {
        // Controls built in code (e.g. prompt buttons) get their text on the
        // menu's next update, which can be after they gain focus. Give the text
        // a few frames to appear before calling the control unnamed.
        if (focused != unnamed_candidate) {
            unnamed_candidate = focused;
            unnamed_frames = 0;
        }
        if (++unnamed_frames < 4) {
            return;
        }
        // An unnamed control (e.g. an empty decorative title button) still says
        // something, so moving onto it isn't silent.
        name = "Blank";
        // With the speech log on, say which element needs a label (an aria-label
        // attribute in its RML).
        static const bool log_unnamed = std::getenv("BM64_SPEECH_LOG") != nullptr;
        if (log_unnamed) {
            fprintf(stderr, "[A11y] Unnamed control: %s\n", focused->GetAddress().c_str());
        }
    }

    last_focused = focused;
    last_state = state;
    last_option = option;
    unnamed_candidate = nullptr;

    // Same order as screen readers: name, role, value, position.
    std::string text = name;
    append_part(text, control_role(focused));
    append_part(text, state);
    append_part(text, position_in_parent(focused));
    bm64::screen_reader::speak(std::move(text), user_input && announced_any && !queue_next);
    announced_any = true;
    queue_next = false;

    description_delay = (option != nullptr && new_option) ? 3 : 0;
}

void recompui::accessibility::reset_focus() {
    last_focused = nullptr;
    last_option = nullptr;
    description_delay = 0;
}

void recompui::accessibility::announce_message(const std::string& header, const std::string& content) {
    std::string text = header;
    if (!content.empty()) {
        if (!text.empty()) {
            text += ". ";
        }
        text += content;
    }
    bm64::screen_reader::speak(std::move(text), true);
    message_spoken = true;
}
