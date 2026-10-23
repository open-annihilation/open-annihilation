// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// System's file-type switches as the file types they choose.

#include "oa/ui/engine_settings/file_types.hpp"

#include <array>
#include <string_view>

namespace oa::ui::engine_settings {

namespace {

namespace file_types = oa::platform::file_types;

/// One switch of System and the file type it stands for.
struct SwitchType {
    bool EngineSettings::* setting{};   ///< the switch
    bool file_types::TypeSet::* type{}; ///< its type in a set
    std::string_view extension{};       ///< its type's extension, without its dot
};

/// The seven switches, in the order System lists them.
constexpr std::array<SwitchType, 7> switch_types{{
    {&EngineSettings::file_type_oamod, &file_types::TypeSet::mod, file_types::mod_extension},
    {&EngineSettings::file_type_oamap, &file_types::TypeSet::map, file_types::map_extension},
    {&EngineSettings::file_type_oalang,
     &file_types::TypeSet::language,
     file_types::language_extension},
    {&EngineSettings::file_type_oareg,
     &file_types::TypeSet::registry,
     file_types::registry_extension},
    {&EngineSettings::file_type_oamut,
     &file_types::TypeSet::mutator,
     file_types::mutator_extension},
    {&EngineSettings::file_type_oabrain, &file_types::TypeSet::brain, file_types::brain_extension},
    {&EngineSettings::file_type_oalaunch,
     &file_types::TypeSet::launch,
     file_types::launch_extension},
}};

} // namespace

file_types::TypeSet file_type_set(const EngineSettings& settings) noexcept {
    file_types::TypeSet types;
    for (const SwitchType& entry : switch_types)
        types.*entry.type = settings.*entry.setting;
    return types;
}

void set_file_type_switches(EngineSettings& settings, const file_types::TypeSet types) noexcept {
    for (const SwitchType& entry : switch_types)
        settings.*entry.setting = types.*entry.type;
}

std::vector<file_types::OpenerChange>
moved_file_type_switches(const EngineSettings& opened, const EngineSettings& chosen) {
    std::vector<file_types::OpenerChange> changes;
    for (const SwitchType& entry : switch_types)
        if (opened.*entry.setting != chosen.*entry.setting)
            changes.push_back(file_types::OpenerChange{entry.extension, chosen.*entry.setting});
    return changes;
}

} // namespace oa::ui::engine_settings
