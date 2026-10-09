// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// MSNBRIEF.GUI mission briefing panel and campaign unit restrictions.
#include "oa/ui/campaign/campaign.hpp"

#include "oa/core/unit_def.h"
#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/base/text.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oa::ui::campaign {
namespace {

constexpr PlanetArt kPlanets[kPlanetCount] = {
    {"Green planet", "Greenbrief", "GreenPan", "GreenRotate"},
    {"Archipelago", "Archibrief", "ArchiPan", "ArchiRotate"},
    {"Wet Desert", "WDesertbrief", "WDesPan", "WDesertRotate"},
    {"Desert", "Desertbrief", "DDesPan", "DDesRotate"},
    {"Lava", "Lavabrief", "LavaPan", "LavaRotate"},
    {"Red Planet", "Marsbrief", "MarsPan", "MarsRotate"},
    {"Lunar", "Lunarbrief", "LunarPan", "LunarRotate"},
    {"Metal", "Metalbrief", "MetalPan", "MetalRotate"},
    {"Lunar2", "Lunar2brief", "Lunar2Pan", "Lunar2Rotate"},
    {"Ice", "Icebrief", "IcePan", "IceRotate"},
    {"Lush", "Lushbrief", "LushPan", "LushRotate"},
    {"Slate", "Slatebrief", "SlatePan", "SlateRotate"},
    {"Water World", "Waterbrief", "WaterPan", "WaterRotate"},
    {"Acid", "Acidbrief", "AcidPan", "AcidRotate"},
    {"Crystal", "Crystalbrief", "CrystPan", "CrystalRotate"},
};

constexpr int32_t kWindRegenModulo = 0x3f;
constexpr int32_t kWindRegenInitialModulo = 0x40;
// The gravity line reads a mission's gravity as a multiple of the standard
// gravity, 112, which it shows as 1.0.
constexpr double kStandardGravity = 112.0;

bool named(const char* control, const char* name) {
    if (control == nullptr)
        return false;
    for (; *control != '\0' && *name != '\0'; ++control, ++name)
        if (std::tolower(static_cast<unsigned char>(*control)) !=
            std::tolower(static_cast<unsigned char>(*name)))
            return false;
    return *control == *name;
}

int32_t roll(RandomFn random, void* context) {
    return random != nullptr ? random(context) : 0;
}

} // namespace

const PlanetArt& planet_art(std::size_t index) {
    return kPlanets[index < kPlanetCount ? index : 0];
}

std::size_t briefing_planet_index(const char* planet, int32_t side) {
    char name[oa::data::campaign::kCampaignShortTextBytes + 2];
    std::snprintf(name, sizeof(name), "%s", planet != nullptr ? planet : "");
    if (std::strcmp(name, "Lunar") == 0 && side != 0)
        oa::base::text::append_terminated(name, "2");
    for (std::size_t i = 0; i < kPlanetCount; ++i)
        if (std::strcmp(name, kPlanets[i].planet) == 0)
            return i;
    return 0;
}

void briefing_start_narration(
    const oa::data::campaign::CampaignFile* campaign, const FrontendHost* host, bool narrate
) {
    if (!narrate || campaign == nullptr)
        return;
    const char* path =
        oa::data::campaign::campaign_path(campaign, oa::data::campaign::CampaignPath::narration);
    if (path != nullptr)
        host_play_narration(host, path, kNarrationDelay);
}

void briefing_update_text(
    BriefingPanel* panel,
    const oa::data::campaign::CampaignFile* campaign,
    const BriefingRegion* region,
    int32_t wrap_width,
    MeasureText measure,
    void* measure_context
) {
    const char* text =
        campaign != nullptr ? oa::data::campaign::campaign_briefing_text(campaign) : nullptr;
    if (text == nullptr) {
        briefing_pager_reset(&panel->pager, nullptr);
        panel->page.row_count = 0;
        panel->page.highlight_count = 0;
        return;
    }
    wrap_text(text, wrap_width, measure, measure_context, panel->wrapped, sizeof(panel->wrapped));
    reflow_span_text(panel->wrapped, panel->text, sizeof(panel->text));
    briefing_pager_reset(&panel->pager, panel->text);
    briefing_next_page(&panel->pager, region, measure, measure_context, &panel->page);
}

void briefing_panel_enter(
    BriefingPanel* panel,
    const oa::data::campaign::CampaignFile* campaign,
    int32_t side,
    const BriefingRegion* region,
    int32_t wrap_width,
    MeasureText measure,
    void* measure_context,
    RandomFn random,
    void* random_context,
    const FrontendHost* host,
    bool narrate
) {
    panel->narration_on = true;
    const int32_t min_wind = campaign->min_wind;
    const int32_t max_wind = campaign->max_wind;
    const int32_t span = max_wind - min_wind + 1;
    const int32_t value = roll(random, random_context);
    panel->wind_walk = (span != 0 ? value % span : 0) + min_wind;
    panel->wind_regen_countdown = roll(random, random_context) % kWindRegenInitialModulo;
    panel->planet = briefing_planet_index(campaign->planet, side);
    panel->panorama_scroll = 0;
    panel->panorama_deadline = 0;
    panel->rotation_frame = 0;
    panel->ticker_next_ms = 0;
    briefing_update_text(panel, campaign, region, wrap_width, measure, measure_context);
    briefing_start_narration(campaign, host, narrate);
    host_cursor(host, cursor_animation::panel_ready);
}

void briefing_solar_system_tick(
    BriefingPanel* panel,
    const oa::data::campaign::CampaignFile* campaign,
    RandomFn random,
    void* random_context,
    uint32_t game_tick,
    int32_t panorama_width,
    const FrontendHost* host
) {
    if (--panel->wind_regen_countdown < 1) {
        panel->wind_walk += roll(random, random_context) % 5 - 2;
        if (panel->wind_walk < campaign->min_wind)
            panel->wind_walk = campaign->min_wind;
        if (campaign->max_wind < panel->wind_walk)
            panel->wind_walk = campaign->max_wind;
        panel->wind_regen_countdown = roll(random, random_context) % kWindRegenModulo;
    }
    std::snprintf(
        panel->wind_label,
        sizeof(panel->wind_label),
        "%s : %d",
        host_translate(host, "Wind Speed"),
        panel->wind_walk
    );
    std::snprintf(
        panel->gravity_label,
        sizeof(panel->gravity_label),
        "%s : %.1f",
        host_translate(host, "Gravity"),
        static_cast<double>(campaign->gravity) / kStandardGravity
    );
    if (panorama_width <= 0)
        return;
    if (static_cast<int32_t>(panel->panorama_deadline) < static_cast<int32_t>(game_tick)) {
        if (panorama_width <= ++panel->panorama_scroll)
            panel->panorama_scroll = 0;
        panel->panorama_deadline = game_tick + kCycleTicks;
    }
}

bool briefing_ticker(
    BriefingPanel* panel,
    uint32_t now_ms,
    uint32_t game_tick,
    bool narration_playing,
    int32_t frame_count,
    const FrontendHost* host
) {
    if (now_ms < panel->ticker_next_ms)
        return false;
    panel->ticker_next_ms = now_ms + kTickerIntervalMs;
    if (!narration_playing && panel->narration_on) {
        panel->narration_on = false;
        host_control_value(host, "SHUTUP", 0);
        host_mark_dirty(host);
    }
    if (frame_count <= 0)
        return false;
    if (game_tick != panel->ticker_last_tick) {
        panel->ticker_last_tick = game_tick;
        panel->rotation_frame = (panel->rotation_frame + 1) % frame_count;
    }
    return true;
}

void briefing_click(
    BriefingPanel* panel,
    const oa::data::campaign::CampaignFile* campaign,
    const FrontendHost* host,
    const char* control,
    const BriefingRegion* region,
    MeasureText measure,
    void* measure_context,
    bool narrate
) {
    if (control == nullptr) {
        briefing_pager_reset(&panel->pager, nullptr);
        return;
    }
    if (named(control, "Start")) {
        host_sound(host, "BigButton");
        if (!host_disc_present(host)) {
            host_message(host, host_translate(host, kCampaignDiscMessage));
            host_clear_selection(host);
            return;
        }
        host_discover_archives(host);
        host_cursor(host, cursor_animation::panel_leaving);
        host_stop_narration(host);
        host_redraw(host);
        host_signal(host, signal::start_mission);
        return;
    }
    if (named(control, "SHUTUP")) {
        host_sound(host, "Options");
        panel->narration_on = !panel->narration_on;
        if (!panel->narration_on)
            host_stop_narration(host);
        else
            briefing_start_narration(campaign, host, narrate);
        host_clear_selection(host);
        host_sound(host, "SmallButton");
        return;
    }
    if (named(control, "PrevMenu")) {
        host_stop_narration(host);
        host_sound(host, "Previous");
        host_redraw(host);
        host_signal(host, signal::back);
        host_cursor(host, cursor_animation::panel_leaving);
        return;
    }
    if ((named(control, "TextRegion") || named(control, "MOREBAR")) &&
        panel->pager.text != nullptr) {
        host_sound(host, "More");
        briefing_next_page(&panel->pager, region, measure, measure_context, &panel->page);
        host_mark_dirty(host);
    }
    host_clear_selection(host);
}

void ingame_briefing_enter(
    BriefingPanel* panel,
    const oa::data::campaign::CampaignFile* campaign,
    const BriefingRegion* region,
    int32_t wrap_width,
    MeasureText measure,
    void* measure_context,
    const FrontendHost* host
) {
    if (host != nullptr && host->clear_attributes != nullptr) {
        host->clear_attributes(host->context, "MOREBAR", gui_layout::attribute::text_list);
        host->clear_attributes(host->context, "TextRegion", gui_layout::attribute::text_list);
    }
    briefing_update_text(panel, campaign, region, wrap_width, measure, measure_context);
    host_mark_dirty(host);
}

void ingame_briefing_click(
    BriefingPanel* panel,
    const FrontendHost* host,
    const char* control,
    const BriefingRegion* region,
    MeasureText measure,
    void* measure_context
) {
    if (control == nullptr) {
        briefing_pager_reset(&panel->pager, nullptr);
        return;
    }
    if (named(control, "OK")) {
        host_sound(host, "Options");
        return;
    }
    if (named(control, "TextRegion") || named(control, "MOREBAR")) {
        host_sound(host, "Options");
        if (panel->pager.text != nullptr)
            briefing_next_page(&panel->pager, region, measure, measure_context, &panel->page);
        host_mark_dirty(host);
        host_clear_selection(host);
    }
    host_clear_selection(host);
}

bool load_unit_availability(
    UnitDef* table,
    uint32_t count,
    const oa::data::campaign::CampaignFile* campaign,
    const oa::data::campaign::CampaignFiles* files
) {
    const char* path =
        oa::data::campaign::campaign_path(campaign, oa::data::campaign::CampaignPath::use_only);
    if (path == nullptr || files == nullptr || files->size == nullptr || files->read == nullptr)
        return false;
    const int32_t size = files->size(files->context, path);
    if (size < 0 || static_cast<uint32_t>(size) > oa::formats::tdf::max_input_bytes)
        return false;
    auto* text = static_cast<char*>(std::malloc(static_cast<std::size_t>(size) + 1));
    if (text == nullptr)
        return false;
    const int32_t read = files->read(files->context, path, text, static_cast<uint32_t>(size));
    oa::formats::tdf::Document document;
    oa::formats::tdf::document_init(&document);
    oa::formats::tdf::ParseError error{};
    const bool parsed =
        read >= 0 &&
        oa::formats::tdf::parse_text(&document, text, static_cast<uint32_t>(read), false, &error);
    std::free(text);
    if (!parsed) {
        oa::formats::tdf::document_free(&document);
        return false;
    }
    for (uint32_t i = 1; i < count; ++i)
        table[i].flags &= ~OA_UNIT_DEF_FLAG_AVAILABLE;
    oa::formats::tdf::reset_cursor(&document);
    for (uint32_t entry = 0; oa::formats::tdf::step_entry(&document, entry); ++entry) {
        const char* name = oa::formats::tdf::cursor(&document)->name;
        for (uint32_t i = 0; i < count; ++i) {
            if (oa::formats::tdf::compare_nocase(name, table[i].unit_name) == 0) {
                table[i].flags |= OA_UNIT_DEF_FLAG_AVAILABLE;
                break;
            }
        }
        oa::formats::tdf::reset_cursor(&document);
    }
    oa::formats::tdf::document_free(&document);
    return true;
}

} // namespace oa::ui::campaign
