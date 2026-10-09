// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/audio/game_audio.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace oa::audio::game_audio {
namespace {

bool equal_ascii_fold(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ac = static_cast<unsigned char>(a[i]);
        const auto bc = static_cast<unsigned char>(b[i]);
        if (std::tolower(ac) != std::tolower(bc))
            return false;
    }
    return true;
}

} // namespace

std::string sound_resource(std::string_view configured_name) {
    std::string value(configured_name);
    std::replace(value.begin(), value.end(), '\\', '/');
    std::string result = "sounds/" + value;
    const auto slash = result.find_last_of('/');
    const auto dot = result.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        result.resize(dot);
    result += ".wav";
    return result;
}

std::string sound_resource_key(std::string_view resource) {
    std::string key(resource);
    for (char& letter : key) {
        if (letter == '\\')
            letter = '/';
        letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    return key;
}

bool known_missing_sound(std::string_view resource) {
    const std::string key = sound_resource_key(resource);
    std::string_view name = key;
    if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos)
        name.remove_prefix(slash + 1);
    if (const auto dot = name.find_last_of('.'); dot != std::string_view::npos)
        name.remove_suffix(name.size() - dot);
    return std::ranges::find(known_missing_sounds, name) != known_missing_sounds.end();
}

void Registry::add(std::string_view name, std::string_view sound) {
    const std::string category(name);
    (void)resolve(category.c_str(), sound);
}

SoundIndex Registry::resolve(const char* category, std::string_view file) {
    const auto stored = [](std::string_view text) {
        return std::string(text.substr(0, stored_name_bytes));
    };
    const auto key = stored(category != nullptr ? std::string_view(category) : file);
    for (std::size_t i = 0; i < sounds_.size(); ++i) {
        const auto& column = category != nullptr ? sounds_[i].name : sounds_[i].file;
        if (category != nullptr && column.empty())
            continue;
        if (equal_ascii_fold(column, key))
            return static_cast<SoundIndex>(i);
    }
    if (sounds_.size() >= maximum_sounds)
        return 0;
    sounds_.push_back(
        {category != nullptr ? key : std::string(), stored(file), sound_resource(file)}
    );
    return static_cast<SoundIndex>(sounds_.size() - 1);
}

SoundIndex Registry::find(std::string_view name) const noexcept {
    for (std::size_t i = 0; i < sounds_.size(); ++i)
        if (!sounds_[i].name.empty() && equal_ascii_fold(sounds_[i].name, name))
            return static_cast<SoundIndex>(i);
    return missing_sound;
}

const Sound* Registry::get(SoundIndex index) const noexcept {
    return index < sounds_.size() ? &sounds_[index] : nullptr;
}

Selection select(
    const Registry& registry, std::string_view name, bool propagate, const PlaybackState& state
) noexcept {
    Selection out;
    out.index = registry.find(name);
    out.route = state.route;
    if (out.index == missing_sound)
        return out;
    out.sound = registry.get(out.index);
    if (state.diagnostic_direct) {
        out.status = SelectionStatus::selected;
        return out;
    }
    if (!state.audio_available) {
        out.status = SelectionStatus::audio_unavailable;
        return out;
    }
    if ((state.sound_mode & 7U) == 0) {
        out.status = SelectionStatus::sound_mode_disabled;
        return out;
    }
    if (state.playback_suppressed) {
        out.status = SelectionStatus::playback_suppressed;
        return out;
    }
    out.status = SelectionStatus::selected;
    out.request_shared_event = propagate;
    return out;
}

Selection select_alternate(
    const Registry& registry, std::string_view name, bool propagate, PlaybackState state
) noexcept {
    state.route = PlaybackRoute::alternate;
    return select(registry, name, propagate, state);
}

void suppress_playback(PlaybackState& state) noexcept {
    state.playback_suppressed = true;
}

WaveRoute wave_file_route(std::string_view path, const PlaybackState& state) noexcept {
    if (state.diagnostic_direct)
        return WaveRoute::system;
    if (path.empty() || !state.audio_available || state.sound_mode == 0 ||
        state.playback_suppressed)
        return WaveRoute::none;
    return WaveRoute::mixer;
}

void restore_sound_options(SoundOptions& options) noexcept {
    options.fx_volume = default_fx_volume;
    options.sound_flags = static_cast<uint16_t>(
        options.sound_flags | sound_flag::acknowledge | sound_flag::build | sound_flag::speech
    );
    options.device_mode_word = device_mode_word_cleared;
    // Mode bits 1 and 2 are cleared and bit 0 set, so the mode becomes 1.
    // Every other bit of the word, including the high byte, stays.
    constexpr auto preserved_bits = static_cast<uint16_t>(~0x6U);
    options.sound_flags = static_cast<uint16_t>((options.sound_flags & preserved_bits) | 1U);
    options.unit_sound_volume = default_unit_sound_volume;
}

float output_gain(uint32_t wave_out_volume, uint32_t fx_volume) noexcept {
    constexpr float channel_full_scale = 65535.0F;
    constexpr float mixer_full_scale = 65535.0F;
    const float device =
        static_cast<float>(std::min(wave_out_volume, full_wave_out_volume)) / channel_full_scale;
    const auto clamped_fx = std::min(fx_volume, maximum_fx_volume);
    const float effects =
        static_cast<float>(std::min(clamped_fx << 10U, full_wave_out_volume)) / mixer_full_scale;
    return std::clamp(device * effects, 0.0F, 1.0F);
}

void scale_pcm_s16(std::span<int16_t> samples, float gain) noexcept {
    const float bounded = std::clamp(std::isfinite(gain) ? gain : 0.0F, 0.0F, 1.0F);
    for (auto& sample : samples) {
        const auto scaled = std::lround(static_cast<float>(sample) * bounded);
        sample = static_cast<int16_t>(std::clamp<long>(
            scaled, std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max()
        ));
    }
}

} // namespace oa::audio::game_audio
