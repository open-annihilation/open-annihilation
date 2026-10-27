// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The words a prompt is built from: catalogue lookup, filled templates, and
// the buttons the settings dialogs share. Kinds use them; they name no kind.
#pragma once

#include "files.hpp"

#include "oa/app/package_install/prompts.hpp"
#include "oa/data/languages/interface_text.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace oa::app::package_install::detail {

namespace fs = std::filesystem;
namespace settings = oa::ui::engine_settings;

/// A template's place and its value.
using Place = std::pair<std::string_view, std::string>;

/// Returns an English text in the language shown.
///
/// @param english the text
/// @return its translation, or the text
inline std::string tr(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// Returns a template, looked up in the language shown, with its places
/// ({name}) filled; the values are not looked up.
///
/// @param english the template
/// @param places the places and their values
/// @return the text
inline std::string fill(std::string_view english, std::initializer_list<Place> places) {
    const std::string pattern = tr(english);
    std::string text;
    std::size_t at = 0;
    while (at < pattern.size()) {
        const std::size_t open = pattern.find('{', at);
        if (open == std::string::npos) {
            text.append(pattern, at, std::string::npos);
            break;
        }
        text.append(pattern, at, open - at);
        const std::size_t close = pattern.find('}', open + 1);
        if (close == std::string::npos) {
            text.append(pattern, open, std::string::npos);
            break;
        }
        const std::string_view name(pattern.data() + open + 1, close - open - 1);
        bool filled = false;
        for (const Place& place : places)
            if (place.first == name) {
                text += place.second;
                filled = true;
                break;
            }
        if (!filled)
            text.append(pattern, open, close - open + 1);
        at = close + 1;
    }
    return text;
}

/// Returns a paragraph of text.
///
/// @param text the text
/// @return the paragraph
inline settings::NoticeParagraph text_of(std::string text) {
    return {std::move(text), false};
}

/// Returns a paragraph that shows a folder's path.
///
/// @param folder the folder
/// @return the paragraph
inline settings::NoticeParagraph shown_path(const fs::path& folder) {
    std::error_code error;
    const fs::path whole = fs::absolute(folder, error);
    return {utf8_of((error ? folder : whole).lexically_normal()), true};
}

/// The buttons' captions, the same English words the settings' own dialogs show.
inline constexpr std::string_view cancel_caption = "Cancel";
inline constexpr std::string_view ok_caption = "OK";
inline constexpr std::string_view open_folder_caption = "Open folder";
inline constexpr std::string_view replace_caption = "Replace";
inline constexpr std::string_view alongside_caption = "Install alongside";
inline constexpr std::string_view reinstall_caption = "Reinstall";
inline constexpr std::string_view play_now_caption = "Play now";

/// Adds a button.
///
/// @param[in,out] made the prompt
/// @param caption its caption, in English
/// @param answer what it answers
/// @param accent drawn as OK is
inline void
add_button(PackagePrompt& made, std::string_view caption, Answer answer, bool accent = false) {
    made.prompt.buttons.push_back({tr(caption), accent});
    made.answers.push_back(answer);
}

/// Returns a button's number by what it answers.
///
/// @param made the prompt
/// @param answer the answer
/// @return its number; 0 when none answers it
inline int32_t button_of(const PackagePrompt& made, Answer answer) {
    for (std::size_t index = 0; index < made.answers.size(); ++index)
        if (made.answers[index] == answer)
            return static_cast<int32_t>(index);
    return 0;
}

/// Sets the buttons Escape, Y and the keys' first mark answer.
///
/// @param[in,out] made the prompt
/// @param cancel what Escape and N answer
/// @param primary what Y answers
/// @param marked what is marked first
inline void set_keys(PackagePrompt& made, Answer cancel, Answer primary, Answer marked) {
    made.prompt.cancel_button = button_of(made, cancel);
    made.prompt.primary_button = button_of(made, primary);
    made.prompt.marked = button_of(made, marked);
}

} // namespace oa::app::package_install::detail
