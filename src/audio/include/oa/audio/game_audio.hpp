// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/formats/hpi.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::audio::game_audio {

using SoundIndex = uint16_t;
inline constexpr SoundIndex missing_sound = 0xffff;
inline constexpr std::size_t maximum_sounds = 255;   // registration needs count + 1 < 0x100
inline constexpr std::size_t stored_name_bytes = 32; // Game.sound_names and sound_files stride

struct Sound {
    std::string name; // allsound section (Game.sound_names); empty for a file-only entry
    std::string file; // configured file name (Game.sound_files)
    std::string resource;
};

/// Returns the sound file loaded for a configured name.
///
/// The result is "sounds/<name>" with its extension replaced by "wav";
/// backslashes become slashes. The audio player loads the WAV from it; no
/// localized copy of the file is looked for.
///
/// @param configured_name File name as configured in allsound.tdf or a unit's sound category.
/// @return Archive path of the WAV file.
[[nodiscard]] std::string sound_resource(std::string_view configured_name);

/// The sounds 3.1c's own data names but its files never held, by file name
/// in lower case without folder or extension: gamedata/sound.tdf names them
/// for the plants' finished units, a building's select, build and repair
/// sounds and some units' select, order and arrival sounds, and the game
/// plays nothing for them. Their failure to play is never reported; a mod
/// that ships one plays it.
inline constexpr std::array<std::string_view, 13> known_missing_sounds{
    "build",
    "hovsmof1",
    "lathelrg",
    "phiblgof",
    "phiblgto",
    "snipok1",
    "snipok2",
    "snipsel1",
    "torpadv1",
    "torpsel1",
    "torpsel2",
    "untdone",
    "untdone1",
};

/// Returns the key a sound resource is known by from one call to the next:
/// its path in lower case, with slashes between folders.
///
/// @param resource the WAV's archive path, as sound_resource gives it or as a unit names it
/// @return the key
[[nodiscard]] std::string sound_resource_key(std::string_view resource);

/// Says whether a sound is one of known_missing_sounds, whatever its folder,
/// extension or case.
///
/// @param resource the WAV's archive path, or its configured name
/// @return true for a sound 3.1c's data names but never shipped
[[nodiscard]] bool known_missing_sound(std::string_view resource);

class Registry {
  public:

    /// Registers a sound under a name unless that name (case-insensitive) is registered or the table is full.
    ///
    /// @param name allsound.tdf section name; truncated to 32 characters.
    /// @param sound The section's sound= file name.
    void add(std::string_view name, std::string_view sound);

    /// Removes every registered sound.
    void clear() noexcept { sounds_.clear(); }

    /// Finds the first sound registered under a name.
    ///
    /// @param name Sound name, compared case-insensitively.
    /// @return Its index, or missing_sound when none is registered.
    [[nodiscard]] SoundIndex find(std::string_view name) const noexcept;

    /// Finds or registers the sound for a category and file.
    ///
    /// @param category Name to match and register under (truncated to 32
    ///        characters); null matches entries by file name instead.
    /// @param file Configured file name, registered when no entry matches.
    /// @return Index of the matching or new entry; 0 once the table is full.
    SoundIndex resolve(const char* category, std::string_view file);

    /// Looks up a registered sound.
    ///
    /// @param index Registry index.
    /// @return The sound, or null when the index is out of range.
    [[nodiscard]] const Sound* get(SoundIndex index) const noexcept;

    /// Returns the number of registered sounds.
    [[nodiscard]] std::size_t size() const noexcept { return sounds_.size(); }

  private:

    std::vector<Sound> sounds_;
};

enum class PlaybackRoute : uint8_t { primary, alternate };
enum class SelectionStatus : uint8_t {
    selected,
    name_not_registered,
    audio_unavailable,
    sound_mode_disabled,
    playback_suppressed,
};

struct PlaybackState {
    bool audio_available{};     // Game.fx_volume != 0
    uint8_t sound_mode{};       // low three bits of Game.sound_flags
    bool playback_suppressed{}; // set by the -s switch
    bool diagnostic_direct{};
    PlaybackRoute route{PlaybackRoute::primary};
};

struct Selection {
    SelectionStatus status{SelectionStatus::name_not_registered};
    SoundIndex index{missing_sound};
    const Sound* sound{};
    PlaybackRoute route{PlaybackRoute::primary};
    bool request_shared_event{};
};

/// Picks a registered sound by name for playing.
///
/// Looks up the name's index, then applies the player's gates in order:
/// diagnostic-direct selects at once, then audio availability, sound mode and
/// suppression.
///
/// @param registry Registered sounds.
/// @param name Sound name, compared case-insensitively.
/// @param propagate True when the sound should also be announced to the other
///        players' simulations; copied to request_shared_event on selection.
/// @param state Current playback gates.
/// @return The selection and why it was or was not selected; telling the other
///         players when request_shared_event is set is left to the caller.
[[nodiscard]] Selection select(
    const Registry& registry, std::string_view name, bool propagate, const PlaybackState& state
) noexcept;

/// Runs select() on the alternate route; the menu music loop selects its sound this way.
///
/// @param registry Registered sounds.
/// @param name Sound name, compared case-insensitively.
/// @param propagate True when the sound should also be announced to the other players' simulations.
/// @param state Playback gates; a copy whose route is replaced.
/// @return The selection, on the alternate route.
[[nodiscard]] Selection select_alternate(
    const Registry& registry, std::string_view name, bool propagate, PlaybackState state
) noexcept;

/// Applies the -s command-line switch: registered sounds are selected but not played.
///
/// @param[in,out] state Playback gates; the diagnostic-direct flag is not written.
void suppress_playback(PlaybackState& state) noexcept;

enum class WaveRoute : uint8_t { none, mixer, system };

/// Chooses how a wave file named by its path plays.
///
/// Used for unit chatter and the sound panel's TEST play: through the
/// system-sound player while that fallback is selected, else by the mixer at
/// the near volume (-585) when the name is not empty, effects have a volume,
/// a sound mode is on and playback is not suppressed; otherwise not at all.
///
/// @param path Wave file path.
/// @param state Current playback gates.
/// @return The route to play on.
[[nodiscard]]
WaveRoute wave_file_route(std::string_view path, const PlaybackState& state) noexcept;

inline constexpr uint32_t default_fx_volume = 27;
inline constexpr uint32_t maximum_fx_volume = 64; // the << 10 shift reaches the device clamp
inline constexpr uint32_t full_wave_out_volume = 0xffffU; // device volume clamp
inline constexpr uint8_t default_unit_sound_volume = 10;  // unitchat byte

// Bits of Game.sound_flags. The low three are the sound mode the selection
// gates test.
namespace sound_flag {
inline constexpr uint16_t mode = 7;
inline constexpr uint16_t restore_volume = 8;
inline constexpr uint16_t acknowledge = 0x10; // ackfx
inline constexpr uint16_t build = 0x20;       // buildfx
inline constexpr uint16_t speech = 0x40;      // speechfx
} // namespace sound_flag

// The sound device's 3D mode word. Applying the options stores 1 when the mode bits
// equal 2; the restore stores 0 for every other mode.
inline constexpr uint32_t device_mode_word_cleared = 0;
inline constexpr uint32_t device_mode_word_mode_two = 1;

struct SoundOptions {
    uint32_t fx_volume{default_fx_volume};                // Game.fx_volume
    uint16_t sound_flags{};                               // Game.sound_flags
    uint8_t unit_sound_volume{default_unit_sound_volume}; // Game.unit_sound_volume
    uint32_t device_mode_word{device_mode_word_cleared};
};

/// Applies the sound screen's RESTORE button to the options.
///
/// Sets fxvol 27, turns ackfx/buildfx/speechfx on, forces the low three mode
/// bits to 1, sets unitchat 10, and clears the device mode word. The
/// saved-volume reapply that follows reapplies the mixer from these fields;
/// its fx path is output_gain(), and its music volume and gamma-table writes
/// are unchanged by these stores.
///
/// @param[in,out] options Sound options; every other bit of sound_flags stays.
void restore_sound_options(SoundOptions& options) noexcept;

/// Returns the linear gain effects play at.
///
/// fxvol << 10 goes to the mixer; the device clamps the result and applies it
/// to both channels.
///
/// @param wave_out_volume Restored WaveOutVolume scalar, 0..0xffff (clamped).
/// @param fx_volume Effects volume preference, 0..64 (clamped).
/// @return Gain in 0..1.
[[nodiscard]] float output_gain(uint32_t wave_out_volume, uint32_t fx_volume) noexcept;

/// Scales signed 16-bit PCM samples in place, rounding and saturating.
///
/// @param[in,out] samples Samples to scale.
/// @param gain Linear gain, clamped to 0..1; a non-finite gain silences.
void scale_pcm_s16(std::span<int16_t> samples, float gain) noexcept;

} // namespace oa::audio::game_audio
