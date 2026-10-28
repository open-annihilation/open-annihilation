// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Clone's and Rename and Describe's answer. The layout draws the question
// from it and the input applies it, so both read the name the same way.
#pragma once

#include "oa/ui/key_editor/model.hpp"
#include "oa/ui/key_editor/screen.hpp"
#include "oa/ui/key_editor/text.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace oa::ui::key_editor {

/// The longest name the question takes, in bytes, as the model does.
inline constexpr std::size_t layout_name_limit = 60;

/// Text without spaces at its ends, as the model keeps a name and a description.
///
/// @param text the text the player typed
/// @return the text without its outer spaces
[[nodiscard]] inline std::string_view without_outer_spaces(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

/// The name the player typed, without spaces at its ends.
///
/// @param state the question's fields
/// @return the name
[[nodiscard]] inline std::string_view typed_layout_name(const ScreenState& state) noexcept {
    return without_outer_spaces(state.layout_name_field.text);
}

/// Why the typed name cannot be used: another layout has it, or it is too
/// long. Empty when it can be used, and for an empty name, which only keeps
/// the button off.
///
/// @param editor the layouts
/// @param state the question's fields
/// @return the line, already looked up; empty when there is nothing to say
[[nodiscard]] inline std::string
layout_name_problem(const Editor& editor, const ScreenState& state) {
    const std::string_view name = typed_layout_name(state);
    if (name.empty())
        return {};
    if (name.size() > layout_name_limit)
        return show(editor.text(), words::name_too_long);
    const std::string except =
        state.prompt == Prompt::describe ? editor.layout_id() : std::string();
    if (editor.name_taken(name, except))
        return fill(show(editor.text(), words::name_taken), {{"name", std::string(name)}});
    return {};
}

/// True when the question's main button may be pressed: a name that can be
/// used and, when renaming, a name or a description that changed.
///
/// @param editor the layouts
/// @param state the question's fields
/// @return true when Clone or Save does something
[[nodiscard]] inline bool layout_answer_ready(const Editor& editor, const ScreenState& state) {
    const std::string_view name = typed_layout_name(state);
    if (name.empty() || !layout_name_problem(editor, state).empty())
        return false;
    if (state.prompt == Prompt::describe)
        return name != editor.layout_name() ||
               without_outer_spaces(state.layout_description_field.text) !=
                   editor.layout_description();
    return true;
}

} // namespace oa::ui::key_editor
