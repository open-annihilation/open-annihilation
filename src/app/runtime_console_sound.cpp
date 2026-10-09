// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The sound object's 3D switch and the novelty voice: the clips the match
// plays at a point, wave files played by path, and the "Sound3D" and "Sing"
// console commands.
#include "oa/app/runtime.hpp"
#include "director_state.hpp"

#include "oa/data/mod_profile.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/platform/preferences.hpp"
#include "match_fault.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace oa::app {

namespace {

using PointSound = oa::sim::match_runtime::Match::PointSound;

constexpr uint32_t announcement_window_ticks = 30;
/// How the asset store's error for a file it does not hold begins.
constexpr std::string_view asset_not_found_error = "asset not found: ";
// The novelty voice's first sound plays on one window in eight.
constexpr uint32_t novelty_first_sound_windows = 8;

} // namespace

void Runtime::play_point_sound(const char* name, const PointSound& sound) {
    // Director mode hears every point sound through its hooks, whatever the
    // sound options say, and never through the sound device.
    if (director_ != nullptr) {
        const auto resource = oa::audio::game_audio::sound_resource(name);
        ++director_->tally.point_sounds;
        director_->send(
            {resource.c_str(),
             sound.volume,
             sound.placed,
             sound.x,
             sound.y,
             sound.z,
             sound.min_distance,
             sound.max_distance,
             0,
             match_ ? match_->simulation().tick : 0U}
        );
        return;
    }
    if (options_.mute || options_.launch.playback_suppressed != 0 || preferences_.fx_volume == 0 ||
        (preferences_.sound_flags & init::preference_flags::sound_mode) == 0)
        return;
    const oa::audio::VoicePosition position{sound.x, sound.y, sound.z};
    const auto spatial = oa::audio::voice_spatial(
        sound_spatial_, sound.min_distance, sound.max_distance, sound.placed ? &position : nullptr
    );
    const std::string resource = oa::audio::game_audio::sound_resource(name);
    if (sound_found_missing(resource))
        return;
    std::string error;
    if (!audio_player_.play_placed(resource, sound.volume, spatial, error))
        report_unplayed_sound(resource, error);
}

void Runtime::play_wave_file(std::string_view path) {
    if (options_.mute)
        return;
    const oa::audio::game_audio::PlaybackState state{
        preferences_.fx_volume != 0,
        static_cast<uint8_t>(preferences_.sound_flags & init::preference_flags::sound_mode),
        options_.launch.playback_suppressed != 0,
        options_.launch.system_sound != 0,
        oa::audio::game_audio::PlaybackRoute::primary
    };
    if (oa::audio::game_audio::wave_file_route(path, state) ==
        oa::audio::game_audio::WaveRoute::none)
        return;
    std::string resource(path);
    std::replace(resource.begin(), resource.end(), '\\', '/');
    if (sound_found_missing(resource))
        return;
    std::string error;
    if (!audio_player_.play_resource(resource, error))
        report_unplayed_sound(resource, error);
}

bool Runtime::sound_found_missing(std::string_view resource) const {
    return missing_sounds_.contains(oa::audio::game_audio::sound_resource_key(resource));
}

void Runtime::report_unplayed_sound(std::string_view resource, const std::string& error) {
    std::string key = oa::audio::game_audio::sound_resource_key(resource);
    // The asset store's words for a file it does not hold.
    if (error.starts_with(asset_not_found_error))
        missing_sounds_.insert(key);
    if (oa::audio::game_audio::known_missing_sound(resource) ||
        !reported_sounds_.insert(std::move(key)).second)
        return;
    std::cerr << "sound unavailable: " << error << '\n';
}

void Runtime::toggle_novelty_voice() {
    novelty_voice_ = novelty_voice_ == 0 ? 1 : 0;
    offline_services_.announcement_gates().novelty_voice = novelty_voice_ != 0;
}

void Runtime::check_console_sound_commands(const std::function<void(const char*)>& enter_line) {
    oa::Game& game = match_->state().game;
    const auto require = [](bool ok, const char* what) {
        if (!ok)
            throw std::runtime_error(std::string("console sound check: ") + what);
    };
    const auto saved = [&](const char* key) { return saved_general_number(key); };
    const oa::sim::unit_spawn::Slot* commander = nullptr;
    for (const auto& slot : match_->world().slots)
        if (commander == nullptr && slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_)
            commander = &slot;
    require(commander != nullptr, "no local unit");

    const auto mode =
        static_cast<int64_t>(preferences_.sound_flags & init::preference_flags::sound_mode);
    require(sound_spatial_ == (mode == 2 ? 1 : 0), "the 3D switch does not follow Sound Mode");
    const auto scroll = game.scroll_speed;
    game.scroll_speed = scroll == 7 ? 8 : 7;
    const auto unsaved = game.scroll_speed;
    enter_line("+sound3d");
    require(sound_spatial_ == 1, "+sound3d did not turn 3D sound on");
    require(saved("scrollspeed") == unsaved, "+sound3d did not save the options");
    require(saved("Sound Mode") == mode, "+sound3d changed the saved Sound Mode");

    struct Heard {
        const Runtime* runtime{};
        std::vector<PointSound> sounds;
    } heard{this, {}};

    const auto kept_hook = match_->point_sound;
    match_->point_sound = {
        &heard,
        [](void* context) { return static_cast<Heard*>(context)->runtime->sound_spatial_ != 0; },
        [](void* context, const char*, const PointSound& sound) {
            static_cast<Heard*>(context)->sounds.push_back(sound);
        }
    };
    const FixedVec3 at = commander->record.position;
    const int32_t px = at.x >> 16;
    const int32_t py = at.y >> 16;
    const int32_t pz = at.z >> 16;
    constexpr int32_t cell = 16;
    constexpr int32_t margin = 3 * cell;
    game.camera_x = static_cast<uint32_t>(px - margin);
    game.camera_y = static_cast<uint32_t>(pz - margin);
    match_->play_sound_at("xplomed2", at);
    require(heard.sounds.size() == 1, "the commander's point was not heard");
    const auto& placed = heard.sounds.back();
    require(
        placed.placed && placed.volume == -585 && placed.y == 0,
        "a clip was not placed with 3D sound on"
    );
    require(
        placed.x == margin - game.view_cells_width / 2 * cell &&
            placed.z == (py >> 1) - pz + game.view_cells_height / 2 * cell + pz - margin,
        "a clip was not placed from the middle of the view"
    );
    require(
        placed.min_distance ==
                static_cast<float>((game.view_cells_width + game.view_cells_height) / 2 * cell) &&
            placed.max_distance == static_cast<float>((game.map_width + game.map_height) * cell),
        "a placed clip has the wrong distance range"
    );

    game.scroll_speed = scroll;
    enter_line("+sound3d");
    require(sound_spatial_ == 0, "+sound3d did not turn 3D sound off");
    require(
        saved("scrollspeed") == scroll && saved("Sound Mode") == mode,
        "+sound3d did not save the options"
    );
    match_->play_sound_at("xplomed2", at);
    game.camera_x = static_cast<uint32_t>(px + 1);
    match_->play_sound_at("xplomed2", at);
    require(heard.sounds.size() == 3, "the commander's point was not heard with 3D sound off");
    require(
        !heard.sounds[1].placed && heard.sounds[1].volume == -585,
        "an on-screen clip was not unplaced at the near volume"
    );
    require(
        !heard.sounds[2].placed && heard.sounds[2].volume == -1585,
        "an off-screen clip was not unplaced at the far volume"
    );
    match_->point_sound = kept_hook;
    bind_match_view();
    const auto sung = check_console_sing_command(enter_line);
    std::cout << "console sound check: +sound3d placed a clip from the view centre and saved, "
                 "+sing "
              << sung << '\n';
}

std::string
Runtime::check_console_sing_command(const std::function<void(const char*)>& enter_line) {
    const auto require = [](bool ok, const char* what) {
        if (!ok)
            throw std::runtime_error(std::string("console sing check: ") + what);
    };
    namespace game_audio = oa::audio::game_audio;
    const oa::World& world = match_->state();
    // A live unit of the viewed player whose sound category offers a sound
    // for being selected: only the viewed player's units speak.
    const auto speaks_when_selected = [&](const oa::sim::unit_spawn::Slot& slot) {
        const auto type = slot.record.type_index;
        if (slot.unit == nullptr || type == 0 || type > unit_definitions_.size() ||
            slot.record.owner_index != world.game.viewpoint_player ||
            (slot.record.flags & OA_UNIT_FLAG_LIVE) == 0)
            return false;
        const auto* sounds = unit_sound_catalog_.choices(
            unit_definitions_[type - 1U].sound_category,
            game_audio::UnitAnnouncementCategory::select
        );
        return sounds != nullptr && !sounds->empty();
    };
    const oa::sim::unit_spawn::Slot* speaker = nullptr;
    for (const auto& slot : match_->world().slots)
        if (speaker == nullptr && speaks_when_selected(slot))
            speaker = &slot;
    require(speaker != nullptr, "the viewed player has no unit with a sound for being selected");
    // The two sounds the match plays: the mod profile's, or 3.1c's honk and
    // sing.
    const auto* profile = mod_profile();
    const auto& names = profile != nullptr ? profile->strings.cheat.sing_sounds
                                           : oa::data::mod_profile::StringsCheat{}.sing_sounds;
    const auto first = game_audio::sound_resource(names[0]);
    const auto second = game_audio::sound_resource(names[1]);
    auto& gates = offline_services_.announcement_gates();
    require(novelty_voice_ == 0 && !gates.novelty_voice, "the novelty voice started on");
    require(
        gates.novelty_sounds[0] == names[0] && gates.novelty_sounds[1] == names[1],
        "the novelty voice does not hold the profile's sing sounds"
    );
    const auto kept_gates = gates;
    gates.play_audio = true;
    gates.unit_speech_mode = true;
    gates.unit_sound_volume = 10;
    auto& slot = match_->world().slots[speaker->unit_index];
    // Selects the unit each tick until speech plays: a record presented
    // within 30 ticks of the last speech is shown without audio.
    const auto spoken = [&]() -> std::string {
        for (uint32_t step = 0; step <= 2 * announcement_window_ticks; ++step) {
            offline_services_.command_sound(
                slot, static_cast<uint32_t>(game_audio::UnitAnnouncementCategory::select)
            );
            for (auto& event : offline_services_.pump_announcements())
                if (event.sound_resource)
                    return *event.sound_resource;
            ++match_timing_.tick;
            match_->simulation().tick = match_timing_.tick;
            tick_or_raise(*match_);
        }
        return {};
    };
    // "+Sing" says nothing of its own: the log's last line is its echo.
    const auto echoed = [this] {
        const auto lines = match_message_lines();
        return !lines.empty() && lines.back().ends_with("> +sing");
    };
    enter_line("+sing");
    require(novelty_voice_ == 1 && gates.novelty_voice, "+sing did not turn the novelty voice on");
    require(echoed(), "+sing was not echoed, or posted a line of its own");
    const auto novelty = spoken();
    const uint32_t tick = match_->simulation().tick;
    const bool first_window = tick / announcement_window_ticks % novelty_first_sound_windows == 0;
    require(
        novelty == (first_window ? first : second),
        "unit speech did not play the novelty voice's sound for its window"
    );
    enter_line("+sing");
    require(
        novelty_voice_ == 0 && !gates.novelty_voice, "+sing did not turn the novelty voice off"
    );
    require(echoed(), "the second +sing was not echoed, or posted a line of its own");
    const auto own = spoken();
    require(
        !own.empty() && own != first && own != second,
        "unit speech did not play the unit's own sound with the novelty voice off"
    );
    gates = kept_gates;
    return "played " + novelty + " at tick " + std::to_string(tick) + " and then " + own;
}

} // namespace oa::app
