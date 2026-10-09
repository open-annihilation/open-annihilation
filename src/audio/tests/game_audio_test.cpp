// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/hpi.hpp"
#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/sound_categories.hpp"
#include "oa/audio/game_audio.hpp"
#include "oa/audio/unit_announcements.hpp"
#include "oa/test/game_assets.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <stdexcept>
#include <array>
#include <cmath>
#include <cstdint>

using namespace oa::audio::game_audio;

static void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

static bool asset_exists(const oa::AssetStore& assets, const std::string& path) {
    oa::ResourceFile* file = assets.open(path);
    if (file == nullptr)
        return false;
    oa::AssetStore::close(file);
    return true;
}

// A screen package's registered sound names resolve in the packed install: a registered name
// to its WAV whatever the case of the file name, and the allsound.tdf names
// beside them.
static void registered_sound_names(const oa::AssetStore& assets) {
    const oa::data::defs::Files files = oa::data::defs::asset_store_files(&assets);
    Registry registry;
    const oa::data::defs::SoundCache cache{
        &registry,
        [](void* context) { static_cast<Registry*>(context)->clear(); },
        [](void* context, const char* name, const char* sound) {
            static_cast<Registry*>(context)->add(name, sound);
        },
    };
    oa::data::defs::SoundCategoryTable categories{};
    oa::data::defs::load_all_sounds(&files, nullptr, cache, &categories);
    oa::data::defs::sound_category_table_free(&categories);
    registry.add("PACKAGE_BUTTON", "beep2");
    registry.add("PACKAGE_SLIDE", "HOVEROK1");
    const PlaybackState enabled{true, 1, false, false, PlaybackRoute::primary};
    const auto button = select(registry, "package_button", false, enabled);
    require(
        button.status == SelectionStatus::selected &&
            button.sound->resource == "sounds/beep2.wav" &&
            asset_exists(assets, button.sound->resource),
        "PACKAGE_BUTTON plays sounds/beep2.wav"
    );
    const auto slide = select(registry, "PACKAGE_SLIDE", false, enabled);
    require(
        slide.status == SelectionStatus::selected &&
            slide.sound->resource == "sounds/HOVEROK1.wav" &&
            asset_exists(assets, slide.sound->resource),
        "an upper-case WAV name finds sounds/hoverok1.wav"
    );
    const auto more = select(registry, "MORE", false, enabled);
    require(
        more.status == SelectionStatus::selected && more.sound->resource == "sounds/button12.wav" &&
            asset_exists(assets, more.sound->resource),
        "allsound.tdf MORE plays sounds/button12.wav"
    );
    const auto big = select(registry, "BIGBUTTON", false, enabled);
    require(
        big.status == SelectionStatus::selected && big.sound->resource == "sounds/butmain1.wav" &&
            asset_exists(assets, big.sound->resource),
        "allsound.tdf BIGBUTTON plays sounds/butmain1.wav"
    );
    registry.add("MORE", "beep2");
    require(
        select(registry, "MORE", false, enabled).sound->resource == "sounds/button12.wav",
        "a registered name keeps its allsound.tdf WAV"
    );
}

// The names the battle room, the save and load dialogs and the order panel pass to
// the frontend's sound service are allsound.tdf sections, found whatever
// their case, each naming a WAV the packed install holds.
static void frontend_sound_names(const oa::AssetStore& assets) {
    const oa::data::defs::Files files = oa::data::defs::asset_store_files(&assets);
    Registry registry;
    const oa::data::defs::SoundCache cache{
        &registry,
        [](void* context) { static_cast<Registry*>(context)->clear(); },
        [](void* context, const char* name, const char* sound) {
            static_cast<Registry*>(context)->add(name, sound);
        },
    };
    oa::data::defs::SoundCategoryTable categories{};
    oa::data::defs::load_all_sounds(&files, nullptr, cache, &categories);
    oa::data::defs::sound_category_table_free(&categories);

    struct Expected {
        const char* name;
        const char* resource;
    };

    constexpr Expected expected[] = {
        {"Multi", "sounds/butnagr1.wav"},
        {"Options", "sounds/butoptn.wav"},
        {"Previous", "sounds/button1.wav"},
        {"BigButton", "sounds/butmain1.wav"},
        {"SmlButton", "sounds/butmain4.wav"},
        {"SMLBUTTON", "sounds/butmain4.wav"},
        {"smlbutton", "sounds/butmain4.wav"},
        {"SmallButton", "sounds/butscro2.wav"},
        {"Ally", "sounds/butscro1.wav"},
        {"Panel", "sounds/servsml6.wav"},
        {"notoktobuild", "sounds/button13.wav"},
        {"immediateorders", "sounds/button5.wav"},
        {"specialorders", "sounds/button5.wav"},
        {"setfireorders", "sounds/butnagr2.wav"},
        {"setmoveorders", "sounds/butnmbl2.wav"},
    };
    const PlaybackState enabled{true, 1, false, false, PlaybackRoute::primary};
    for (const auto& entry : expected) {
        const auto selection = select(registry, entry.name, false, enabled);
        if (selection.status != SelectionStatus::selected ||
            selection.sound->resource != entry.resource ||
            !asset_exists(assets, selection.sound->resource))
            throw std::runtime_error(std::string(entry.name) + " does not play " + entry.resource);
    }
}

// A named wave goes to the system-sound player while that fallback is
// selected, whatever the gates; otherwise to the mixer only for a non-empty
// name with Game.fx_volume set, a sound mode (Game.sound_flags & 7) and
// playback not suppressed.
static void wave_file_routes() {
    const PlaybackState on{true, 2, false, false, PlaybackRoute::primary};
    require(wave_file_route("sounds\\explode.wav", on) == WaveRoute::mixer, "gates open");
    require(wave_file_route("", on) == WaveRoute::none, "empty name");
    auto state = on;
    state.audio_available = false;
    require(wave_file_route("sounds\\explode.wav", state) == WaveRoute::none, "no effects volume");
    state = on;
    state.sound_mode = 0;
    require(wave_file_route("sounds\\explode.wav", state) == WaveRoute::none, "no sound mode");
    state = on;
    state.playback_suppressed = true;
    require(wave_file_route("sounds\\explode.wav", state) == WaveRoute::none, "mixer vetoed");
    state.diagnostic_direct = true;
    state.audio_available = false;
    require(
        wave_file_route("", state) == WaveRoute::system,
        "the system-sound fallback takes every call"
    );
}

// The known missing sounds are exactly the sounds gamedata/sound.tdf names
// that the installed game does not hold.
static void known_missing_sounds_are_the_unshipped_ones(const oa::AssetStore& assets) {
    const oa::data::defs::Files files = oa::data::defs::asset_store_files(&assets);
    Registry registry;
    const oa::data::defs::SoundCache cache{
        &registry,
        [](void* context) { static_cast<Registry*>(context)->clear(); },
        [](void* context, const char* name, const char* sound) {
            static_cast<Registry*>(context)->add(name, sound);
        },
    };
    oa::data::defs::SoundCategoryTable categories{};
    oa::data::defs::load_all_sounds(&files, nullptr, cache, &categories);
    std::array<bool, known_missing_sounds.size()> named{};
    std::string unlisted;
    for (uint32_t index = 0; index < categories.count; ++index) {
        const oa::data::defs::SoundCategory& category = categories.categories[index];
        for (const oa::data::defs::SoundChoices& choices : category.events)
            for (int32_t choice = 0; choice < choices.count; ++choice) {
                const std::string resource = sound_resource(choices.sounds[choice]);
                const bool missing = !asset_exists(assets, resource);
                if (missing && !known_missing_sound(resource))
                    unlisted += " " + resource;
                for (std::size_t at = 0; at < known_missing_sounds.size(); ++at)
                    if (sound_resource_key(resource) ==
                        "sounds/" + std::string(known_missing_sounds[at]) + ".wav")
                        named[at] = true;
            }
    }
    oa::data::defs::sound_category_table_free(&categories);
    if (!unlisted.empty())
        throw std::runtime_error("sound.tdf names sounds the game lacks:" + unlisted);
    for (std::size_t at = 0; at < known_missing_sounds.size(); ++at) {
        const std::string resource = sound_resource(known_missing_sounds[at]);
        require(named[at], "every known missing sound is one sound.tdf names");
        require(!asset_exists(assets, resource), "every known missing sound is missing");
    }
}

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        const auto assets = oa::test::require_game_assets("the installed sound names");
        registered_sound_names(assets);
        frontend_sound_names(assets);
        known_missing_sounds_are_the_unshipped_ones(assets);
        std::cout << "game audio data tests passed\n";
        return 0;
    }
    wave_file_routes();
    Registry registry;
    registry.add("BigButton", "BigButton");
    registry.add("smlButton", "smlButton");
    registry.add("EXIT", "exit.wav");
    registry.add("bigbutton", "ignored.wav");
    registry.add("Victory Condition", "victory2");
    require(registry.size() == 4, "duplicate names keep the first registration");
    require(registry.find("victory condition") == 3, "section name containing spaces");
    require(registry.find("BIGBUTTON") == 0, "case-insensitive lookup");
    require(
        registry.get(0)->resource == "sounds/BigButton.wav", "default sound path and extension"
    );
    require(registry.get(2)->resource == "sounds/exit.wav", "existing wav extension");
    require(sound_resource("legacy.mp3") == "sounds/legacy.wav", "resolver replaces extension");
    require(
        sound_resource_key("Sounds\\UntDone.WAV") == "sounds/untdone.wav", "a key in lower case"
    );
    require(known_missing_sound("sounds/untdone.wav"), "a sound 3.1c never shipped");
    require(known_missing_sound("UNTDONE1"), "a configured name of one, in any case");
    require(known_missing_sound("Sounds\\Build.wav"), "with backslashes and an extension");
    require(!known_missing_sound("sounds/unitdone.wav"), "a sound 3.1c ships");
    require(!known_missing_sound("sounds/spiderse.wav"), "a mod's own sound");
    require(!known_missing_sound("sounds/untdone/other.wav"), "a folder of that name");
    require(registry.find("unknown") == missing_sound, "missing sentinel");
    // A null category matches by file name and registers the
    // file without a category; a category match ignores the file.
    auto table = registry;
    require(table.resolve(nullptr, "EXIT.WAV") == 2, "file lookup ignores case");
    require(table.resolve(nullptr, "Victory Condition") == 4, "file lookup ignores categories");
    require(
        table.get(4)->name.empty() && table.get(4)->resource == "sounds/Victory Condition.wav",
        "file-only entry"
    );
    require(table.resolve(nullptr, "victory condition") == 4 && table.size() == 5, "cached file");
    require(table.resolve("smlbutton", "other") == 1 && table.size() == 5, "category match");
    require(table.find("") == missing_sound, "file-only entries have no name");
    // The comparison and the stored columns both span all 32 bytes.
    const std::string prefix(31, 'x');
    const auto first_long = table.resolve((prefix + "A").c_str(), "long-a");
    const auto second_long = table.resolve((prefix + "B").c_str(), "long-b");
    require(first_long != second_long, "the 32nd category byte distinguishes entries");
    require(
        table.resolve((prefix + "a-tail").c_str(), "unused") == first_long,
        "comparison ignores bytes after the 32nd and folds case"
    );
    while (table.size() < maximum_sounds)
        (void)table.resolve(nullptr, "fill" + std::to_string(table.size()));
    require(
        table.resolve(nullptr, "overflow") == 0 && table.size() == maximum_sounds, "full table"
    );
    PlaybackState enabled{true, 1, false, false, PlaybackRoute::primary};
    auto selected = select(registry, "smlbutton", true, enabled);
    require(
        selected.status == SelectionStatus::selected && selected.request_shared_event,
        "selected sound"
    );
    require(
        select_alternate(registry, "exit", false, enabled).route == PlaybackRoute::alternate,
        "alternate wrapper"
    );
    enabled.sound_mode = 0;
    require(
        select(registry, "exit", false, enabled).status == SelectionStatus::sound_mode_disabled,
        "mode gate"
    );
    enabled.diagnostic_direct = true;
    require(
        select(registry, "exit", true, enabled).status == SelectionStatus::selected &&
            !select(registry, "exit", true, enabled).request_shared_event,
        "diagnostic direct route"
    );
    PlaybackState suppressed{true, 1, false, false, PlaybackRoute::primary};
    suppress_playback(suppressed);
    require(suppressed.playback_suppressed, "-s sets the playback-suppressed flag");
    require(
        suppressed.audio_available && suppressed.sound_mode == 1 && !suppressed.diagnostic_direct &&
            suppressed.route == PlaybackRoute::primary,
        "-s leaves every other playback field"
    );
    require(
        select(registry, "exit", true, suppressed).status == SelectionStatus::playback_suppressed &&
            !select(registry, "exit", true, suppressed).request_shared_event,
        "suppressed playback is not requested"
    );
    PlaybackState diagnostic{true, 1, false, true, PlaybackRoute::alternate};
    suppress_playback(diagnostic);
    require(
        diagnostic.playback_suppressed && diagnostic.diagnostic_direct &&
            diagnostic.route == PlaybackRoute::alternate,
        "-s does not clear an existing diagnostic route"
    );
    require(output_gain(0, maximum_fx_volume) == 0.0F, "muted WaveOut gain");
    const float maximum = output_gain(full_wave_out_volume, maximum_fx_volume);
    require(maximum == 1.0F, "mixer saturation");
    require(output_gain(full_wave_out_volume, 999) == maximum, "fx volume clamps to its range");
    require(
        std::abs(output_gain(32768U, maximum_fx_volume) - (32768.0F / 65535.0F)) < 0.00001F,
        "scalar WaveOut mapping"
    );
    require(
        output_gain(0xffffffffU, maximum_fx_volume) == 1.0F, "WaveOut clamps at the device maximum"
    );
    SoundOptions restored;
    restored.fx_volume = 64;
    restored.sound_flags = 0x01fe;
    restored.unit_sound_volume = 1;
    restored.device_mode_word = device_mode_word_mode_two;
    restore_sound_options(restored);
    require(restored.fx_volume == default_fx_volume, "RESTORE restores fxvol 27");
    require(restored.sound_flags == 0x01f9, "mode forced to 1; effect bits and high byte kept");
    require(restored.unit_sound_volume == default_unit_sound_volume, "unitchat restored to 10");
    require(restored.device_mode_word == device_mode_word_cleared, "RESTORE clears mode word");
    enabled.sound_mode = static_cast<uint8_t>(restored.sound_flags & sound_flag::mode);
    enabled.diagnostic_direct = false;
    enabled.audio_available = true;
    enabled.playback_suppressed = false;
    require(
        select(registry, "exit", false, enabled).status == SelectionStatus::selected,
        "restored mode enables playback"
    );
    SoundOptions blank;
    blank.sound_flags = 0x0200;
    blank.device_mode_word = 0xffffffffU;
    restore_sound_options(blank);
    require(blank.sound_flags == 0x0271, "effect bits set from a clear low byte");
    require(blank.device_mode_word == device_mode_word_cleared, "mode word cleared from any value");
    require(
        output_gain(full_wave_out_volume, restored.fx_volume) ==
            output_gain(full_wave_out_volume, default_fx_volume),
        "restored fxvol is the saved-volume reapply's mixer input"
    );
    std::array<int16_t, 5> pcm{-32768, -1000, 0, 1000, 32767};
    scale_pcm_s16(pcm, 0.5F);
    require(pcm == std::array<int16_t, 5>{-16384, -500, 0, 500, 16384}, "PCM gain scaling");
    scale_pcm_s16(pcm, 4.0F);
    require(pcm == std::array<int16_t, 5>{-16384, -500, 0, 500, 16384}, "PCM gain clamps high");
    scale_pcm_s16(pcm, -1.0F);
    require(pcm == std::array<int16_t, 5>{0, 0, 0, 0, 0}, "PCM gain clamps low");

    const auto unit_sounds = UnitSoundCatalog::parse_sound_tdf(R"(
      [ARM_KBOT] { select1=kbarmsel; ok=kbarmbase; oktext=base-text;
                   ok1=kbarmmov1; ok1text=move-one; ok2=kbarmmov2; arrived1=kbarmstp; }
      [SILENT] { cant1=cantdo4; }
    )");
    require(unit_sounds.size() == 2, "SOUND.TDF category count");
    const auto* acknowledgements =
        unit_sounds.choices("arm_kbot", UnitAnnouncementCategory::acknowledge);
    require(
        acknowledgements != nullptr && acknowledgements->size() == 3 &&
            (*acknowledgements)[0].sound == "kbarmbase" &&
            (*acknowledgements)[1].text == "move-one",
        "unnumbered then numbered SOUND.TDF choices"
    );
    require(
        unit_announcement_category(5) == UnitAnnouncementCategory::acknowledge &&
            !unit_announcement_category(24),
        "numeric category conversion"
    );

    AnnouncementQueue announcements;
    AnnouncementRequest announcement{
        7, "ARM_KBOT", UnitAnnouncementCategory::acknowledge, 100, false, true, true, {}
    };
    require(
        announcements.enqueue(announcement) == AnnouncementEnqueueStatus::not_local_owner,
        "local-player gate"
    );
    announcement.local_owner = true;
    require(
        announcements.enqueue(announcement) == AnnouncementEnqueueStatus::queued,
        "ground acknowledgement queued"
    );
    require(
        announcements.enqueue(announcement) == AnnouncementEnqueueStatus::duplicate_category,
        "duplicate category suppressed"
    );
    const auto spoken =
        announcements.present_front(unit_sounds, {10, 10, true, true, true}, 0x7fff, 100);
    require(
        spoken && spoken->category == UnitAnnouncementCategory::acknowledge &&
            spoken->sound_resource == "sounds/kbarmmov2.wav" && !spoken->text,
        "RNG selects SOUND.TDF alias for typed event"
    );
    announcements.pop_front();
    announcement.tick = 129;
    require(
        announcements.enqueue(announcement) == AnnouncementEnqueueStatus::cooling_down,
        "one-second acknowledgement cooldown"
    );
    announcement.tick = 130;
    require(
        announcements.enqueue(announcement) == AnnouncementEnqueueStatus::queued,
        "cooldown expires at the 30 Hz boundary"
    );

    AnnouncementQueue priorities;
    require(
        priorities.enqueue(
            {1, "ARM_KBOT", UnitAnnouncementCategory::arrived, 0, true, true, true, {}}
        ) == AnnouncementEnqueueStatus::queued,
        "arrived queued"
    );
    require(
        priorities.enqueue(
            {1, "ARM_KBOT", UnitAnnouncementCategory::select, 0, true, true, true, {}}
        ) == AnnouncementEnqueueStatus::queued,
        "selection queued"
    );
    const auto first = priorities.present_front(unit_sounds, {10, 10, true, true, true}, 0, 0);
    require(
        first && first->category == UnitAnnouncementCategory::select,
        "higher priority inserts ahead of existing announcement"
    );

    AnnouncementQueue cadence;
    require(
        cadence.enqueue(
            {2, "ARM_KBOT", UnitAnnouncementCategory::acknowledge, 1, true, true, true, {}}
        ) == AnnouncementEnqueueStatus::queued,
        "cadence event queued"
    );
    const auto early = cadence.pump(unit_sounds, {10, 10, true, true, true}, 0, 1);
    require(
        early && !early->sound_resource && early->text == "base-text",
        "pre-cadence event draws choice, shows choice text, and suppresses audio"
    );
    std::cout << "game audio tests passed\n";
}
