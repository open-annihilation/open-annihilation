// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/defs/layout.hpp"

#include <algorithm>

namespace oa::data::defs {
namespace {

/// The layout in use; the base game's until use_data_layout() replaces it.
DataLayout& current_layout() noexcept {
    static DataLayout layout{};
    return layout;
}

} // namespace

const DataLayout& data_layout() noexcept {
    return current_layout();
}

void use_data_layout(const DataLayout& layout) {
    current_layout() = layout;
}

const char* directory_name(DataDirectory directory) noexcept {
    return current_layout().directories[static_cast<std::size_t>(directory)].c_str();
}

std::string data_path(DataDirectory directory, std::string_view name) {
    std::string path = directory_name(directory);
    path += '/';
    path += name;
    return path;
}

std::string gui_path(std::string_view file) {
    return data_path(DataDirectory::guis, file);
}

const char* unit_extension() noexcept {
    return current_layout().unit_extension.c_str();
}

std::string unit_file_suffix() {
    return std::string(".") + unit_extension();
}

const char* side_name(uint8_t side) noexcept {
    const auto& names = current_layout().side_names;
    if (names.empty())
        return "";
    return names[std::min<std::size_t>(side, names.size() - 1)].c_str();
}

uint8_t watcher_side_frame() noexcept {
    return current_layout().watcher_side_frame;
}

uint32_t skirmish_hidden_sides() noexcept {
    return current_layout().skirmish_hidden_sides;
}

uint32_t battle_room_hidden_sides() noexcept {
    return current_layout().battle_room_hidden_sides;
}

uint32_t computer_player_sides() noexcept {
    return current_layout().computer_player_sides;
}

const char* map_units_section() noexcept {
    return current_layout().map_units_section.c_str();
}

} // namespace oa::data::defs
