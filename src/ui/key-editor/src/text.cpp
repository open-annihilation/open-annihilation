// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/key_editor/text.hpp"

#include <string>

namespace oa::ui::key_editor {

std::string show(const TextHooks& hooks, std::string_view english) {
    if (hooks.shown == nullptr)
        return std::string(english);
    return hooks.shown(hooks.user, english);
}

std::string fill(
    std::string_view pattern,
    std::initializer_list<std::pair<std::string_view, std::string_view>> places
) {
    std::string filled(pattern);
    for (const auto& place : places) {
        const std::string token = std::string("{") + std::string(place.first) + "}";
        for (std::size_t at = filled.find(token); at != std::string::npos;
             at = filled.find(token, at)) {
            filled.replace(at, token.size(), place.second);
            at += place.second.size();
        }
    }
    return filled;
}

} // namespace oa::ui::key_editor
