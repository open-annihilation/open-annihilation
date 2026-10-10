// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The in-game message log (Game.chat_lines) drawn over the battlefield, and
// the game speed keys that post to it.
#include "oa/app/asset_files.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/ui/decoded.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/text/line_break.hpp"
#include "oa/data/languages/unit_texts.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/present/model/mesh_raster.hpp"
#include "oa/sim/speed.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/ui/hud/chat_panel.hpp"
#include "oa/ui/frontend/ingame_menu.hpp"
#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/present/game_text.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {
namespace {

namespace messages = oa::sim::messages;

std::optional<oa::formats::fnt::Font>
load_engine_font(oa::AssetStore& assets, const char* name, const char* language) {
    try {
        return oa::ui::decoded::require(
            oa::formats::fnt::load_named_fnt(assets, name, language != nullptr ? language : ""),
            name
        );
    } catch (const std::exception& error) {
        std::cerr << name << " font unavailable: " << error.what() << '\n';
        return std::nullopt;
    }
}

// The interface's texts by language, and the language when the command line
// names none.
constexpr const char* kTranslationFile = "translate.tdf";
constexpr const char* kDefaultLanguage = "English";

/// Returns the step from one message log line to the next: the height in the
/// font file's header, the low byte of its first word.
///
/// @param font the message log's font
/// @return pixels between the tops of two lines
int32_t message_line_step(const oa::formats::fnt::Font& font) {
    return static_cast<uint8_t>(font.nominal_height);
}

// Rows below the pen a message line's GUI-font glyphs may reach: the whole
// height kept, as the log's lines are cut only by the battlefield's edge.
constexpr int kGuiTextRowsBelowPen = 32;

/// Returns the canvas corner of the message log: the point of the 640x480
/// battlefield the log starts at, where the battlefield's own mapping puts
/// it, moved by as much as the overlays' area's corner lies from the
/// battlefield's. Without the touch controls the area is the battlefield and
/// the corner is the battlefield's mapping alone; on a phone, whose HUD shows
/// in placed regions, the battlefield's mapping is taken whatever region
/// shows that part of the 640x480 screen.
///
/// @param layout the match layout
/// @param area the overlays' area (Runtime::overlay_area), canvas pixels
/// @return the corner, canvas pixels
oa::ui::display_layout::Point message_log_corner(
    const oa::ui::display_layout::MatchLayout& layout, const oa::ui::display_layout::Rect& area
) {
    namespace layout_space = oa::ui::display_layout;
    const auto point = layout_space::placed_mode(layout)
                           ? layout_space::source_battlefield_to_canvas(
                                 layout, oa::ui::hud::kMessageLogLeft, oa::ui::hud::kMessageLogTop
                             )
                           : layout_space::source_to_canvas(
                                 layout, oa::ui::hud::kMessageLogLeft, oa::ui::hud::kMessageLogTop
                             );
    return {point.x + area.x - layout.battlefield_x(), point.y + area.y - layout.battlefield_y()};
}

#if OA_SELF_CHECKS
// The speeds the speed check's stand-in for Extension::speed_changed was
// told of, in order.
std::vector<uint16_t>& reported_speeds() {
    static std::vector<uint16_t> speeds;
    return speeds;
}

void report_speed(void* /*context*/, Runtime& /*runtime*/, uint16_t speed) {
    reported_speeds().push_back(speed);
}
#endif

} // namespace

void Runtime::load_common_fonts() {
    const char* language = game_language();
    message_font_ = load_engine_font(assets_, "COMIX", language);
    small_font_ = load_engine_font(assets_, "smlfont", language);
}

void Runtime::load_translations(const char* language) {
    // A missing or unreadable file leaves every text as it is. Most games
    // have no translation file, so only one that is there and does not load
    // is reported.
    const auto files = asset_files(assets_);
    const auto path =
        std::string(oa::data::defs::directory_name(oa::data::defs::DataDirectory::gamedata)) +
        '\\' + kTranslationFile;
    if (!oa::data::defs::load_locale_table(
            &files,
            &translations_.table,
            path.c_str(),
            language != nullptr ? language : kDefaultLanguage
        ) &&
        files.exists != nullptr && files.exists(files.context, path.c_str()))
        std::cerr << "open-annihilation: " << path
                  << " did not load in full; the texts it misses stay as they are\n";
}

messages::Hooks Runtime::message_hooks() {
    messages::Hooks hooks{};
    hooks.context = this;
    hooks.side_name = [](void*, uint8_t side) { return oa::data::defs::side_name(side); };
    hooks.play_sound = [](void* context, const char* name) {
        static_cast<Runtime*>(context)->play_match_interface_sound(name);
    };
    // Camera centring moves the camera at once.
    hooks.center_camera = [](void* context, int32_t x, int32_t y, int32_t) {
        auto& runtime = *static_cast<Runtime*>(context);
        runtime.match_camera_x_ = x - runtime.visible_map_width() / 2;
        runtime.match_camera_z_ = y - runtime.visible_map_height() / 2;
    };
    // The log's choices, such as the elimination taunt, draw from a stream
    // of its own: a line posted never changes the game.
    hooks.random = [](void* context) -> uint32_t {
        return static_cast<Runtime*>(context)->message_random_.next();
    };
    // The log's own phrases, such as the elimination taunts and the speed
    // line, in the game's language, as gamedata\translate.tdf gives them.
    hooks.translate = translation_hook;
    // The profile's kill-lead line and elimination endings, where they
    // differ from 3.1c's, in place of the log's own whatever the language.
    hooks.kill_lead_text = [](void* context) {
        return view_rules::profile_texts(static_cast<Runtime*>(context)->mod_profile()).kill_lead;
    };
    hooks.elimination_ending = [](void* context, uint32_t index) -> const char* {
        const auto endings =
            view_rules::profile_texts(static_cast<Runtime*>(context)->mod_profile())
                .elimination_endings;
        return index < endings.size() ? endings[index] : nullptr;
    };
    // The extension adds its hooks to a copy, which replaces the engine's
    // only when the hook returns: one that throws leaves the log with the
    // engine's own hooks for this call. The report goes to standard error
    // alone, since a line posted to the log would build its hooks again.
    messages::Hooks filled = hooks;
    HookError error;
    call_hook<&Extension::message_hooks>(extension_, error, *this, filled);
    if (error.caught) {
        report_hook_error(HookTraits<&Extension::message_hooks>::entry.name, error.message.c_str());
        return hooks;
    }
    return filled;
}

void Runtime::bind_message_log() {
    auto& game = match_->state().game;
    message_random_ = {};
    messages::set_log_options(
        game,
        static_cast<int32_t>(preferences_.text_lines),
        static_cast<int32_t>(preferences_.text_scroll),
        messages::filter_session_start,
        preferences_.screen_chat
    );
    ensure_ui_colors();
    std::copy_n(ui_colors_.begin(), sizeof game.ui_colors, game.ui_colors);
    // Elimination lights the killer's kills and the victim's losses on the
    // kills board while F4 holds it out.
    kill_board_ = {};
    console_clock_fade_ = {};
    match_->kill_board.context = this;
    match_->kill_board.flash = [](void* context, uint8_t killer, uint8_t victim) {
        oa::ui::hud::flash_kill(static_cast<Runtime*>(context)->kill_board_, killer, victim);
    };
    // A player taking the top row of the kills board is announced in the log
    // with its board score.
    match_->kill_board.took_lead = [](void* context, uint8_t player, int16_t score) {
        auto& runtime = *static_cast<Runtime*>(context);
        auto& world = runtime.match_->state();
        if (const oa::Player* leader = oa::world_player(&world, player))
            messages::post_kill_lead(world, *leader, score, runtime.message_hooks());
    };
    // Once a player's last unit is gone: a multiplayer game
    // announces the player leaving, a skirmish its forces' end, a campaign
    // nothing.
    match_->last_unit = {this, [](void* context, uint8_t player) {
                             auto& runtime = *static_cast<Runtime*>(context);
                             auto& world = runtime.match_->state();
                             const oa::Player* owner = oa::world_player(&world, player);
                             if (owner == nullptr)
                                 return;
                             // Called inside the tick: what the hook throws
                             // is reported as a simulation error, and the
                             // tick goes on as for a null hook.
                             const auto report_in_tick =
                                 [&runtime](const char*, const char* message) {
                                     runtime.report_match_tick_error(message);
                                 };
                             if (call_hook_or_report<&Extension::player_gone>(
                                     runtime.extension_, report_in_tick, runtime, world, *owner
                                 ))
                                 return;
                             if (!runtime.campaign_mission_)
                                 messages::post_elimination(world, *owner, runtime.message_hooks());
                         }};
}

void Runtime::check_profile_texts() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("profile text check: " + what);
    };
    require(match_ && screen_ == Screen::match && !match_paused_, "no running match");
    auto& world = match_->state();
    const auto texts = view_rules::profile_texts(mod_profile());
    const auto last_line = [this] {
        const auto lines = match_message_lines();
        return lines.empty() ? std::string() : lines.back();
    };
    const uint8_t viewer = world.game.viewpoint_player;
    require(viewer < OA_PLAYER_COUNT, "no viewed player");
    const oa::Player& player = world.game.players[viewer];
    // The kills board's new leader, with its name and board score.
    require(match_->kill_board.took_lead != nullptr, "the kills board's new leader is not posted");
    match_->kill_board.took_lead(match_->kill_board.context, viewer, 3);
    char lead[messages::text_bytes];
    messages::format_kill_lead(
        lead,
        sizeof lead,
        texts.kill_lead != nullptr ? texts.kill_lead : messages::kill_lead_message,
        {player.name, strnlen(player.name, sizeof player.name)},
        3
    );
    require(last_line() == lead, "the new leader's line reads \"" + last_line() + '"');
    // The elimination line ends with one of the three endings.
    messages::post_elimination(world, player, message_hooks());
    const auto eliminated = last_line();
    bool ending_shown = false;
    for (uint32_t index = 0; index < messages::elimination_message_count; ++index) {
        const char* ending = texts.elimination_endings[index] != nullptr
                                 ? texts.elimination_endings[index]
                                 : messages::elimination_messages[index];
        ending_shown = ending_shown || eliminated.ends_with(std::string(" ") + ending);
    }
    require(ending_shown, "the elimination line reads \"" + eliminated + '"');
    // The question a request to close the window asks, answered No.
    request_match_close();
    std::string title;
    if (match_hud_)
        for (const auto& gadget : match_hud_->layout.gadgets)
            if (const auto* label = std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields);
                label != nullptr && gadget.common.name == "TITLE")
                title = label->text;
    const std::string_view question = texts.leave_question != nullptr
                                          ? std::string_view(texts.leave_question)
                                          : oa::ui::frontend::kLeaveGameTitle;
    require(match_paused_ && title == question, "the leave question reads \"" + title + '"');
    activate_pause_gadget("CHOICE2");
    require(!match_paused_ && !exit_requested_, "No did not return to the running match");
    std::cout << "profile text check: \"" << lead << "\", \"" << eliminated << "\", \"" << title
              << "\"\n";
}

void Runtime::post_match_message(
    std::string_view text, uint8_t kind, uint16_t value, uint8_t sender
) {
    if (!match_)
        return;
    // The line keeps what fits, less a UTF-8 character the cut would split.
    char line[messages::text_bytes];
    std::snprintf(
        line,
        sizeof line,
        "%.*s",
        static_cast<int>(oa::base::text::whole_character_bytes(text, sizeof line - 1)),
        text.data()
    );
    messages::post_message(match_->state(), line, kind, value, sender, message_hooks());
}

void Runtime::post_unit_report(uint16_t unit, std::string_view text) {
    // The unit's name in the player's language, as its file gives it there,
    // and the caption as gamedata\translate.tdf gives it.
    std::string name;
    if (match_ && unit != 0 && unit < match_->world().slots.size()) {
        const auto& slot = match_->world().slots[unit];
        if (slot.unit != nullptr)
            if (const auto* def = oa::world_unit_def_of(&match_->state(), &slot.record))
                name = std::string(oa::data::languages::unit_display_name(*def));
    }
    if (name.empty()) {
        const auto* definition = definition_for(unit);
        name = definition != nullptr && !definition->display_name.empty() ? definition->display_name
                                                                          : unit_info_name(unit);
    }
    post_match_message(name + ": " + translate_ui(text), messages::kind_unit_report, unit);
}

std::vector<std::string> Runtime::match_message_lines() {
    std::vector<std::string> lines;
    if (!match_)
        return lines;
    auto& game = match_->state().game;
    for (uint32_t index = game.chat_tail; index != game.chat_head;
         index = (index + 1U) % OA_CHAT_LINE_COUNT) {
        const auto* line = messages::message_line(game, index);
        if (line == nullptr)
            break;
        lines.emplace_back(line->text, strnlen(line->text, sizeof line->text));
    }
    return lines;
}

const oa::formats::fnt::Font& Runtime::message_font() {
    if (!message_font_)
        message_font_ = match_small_font_ ? *match_small_font_ : oa::formats::fnt::Font{};
    return *message_font_;
}

int32_t Runtime::message_log_step() {
    return oa::present::sized_length(
        message_line_step(message_font()), oa::present::game_text_size()
    );
}

std::vector<std::string> Runtime::message_log_rows(std::string_view text, int32_t x) {
    std::vector<std::string> rows;
    if (!match_ || text.empty())
        return rows;
    const bool gui = !gui_font_.sequences.empty();
    const auto runs = renderer::split_game_text(
        text,
        gui ? renderer::gui_font_characters(gui_font_)
            : renderer::fnt_font_characters(message_font()),
        true
    );
    if (runs.size() != 1 || !runs.front().modern)
        return rows;
    const auto& run = runs.front();
    const auto face =
        gui ? renderer::gui_font_face(gui_font_) : renderer::fnt_font_face(message_font());
    // Each row reaches the right edge of the overlays' area (the
    // battlefield's without the touch controls) from where the line's text
    // starts.
    const int scale = hud_text_scale();
    const auto area = overlay_area();
    const auto corner = message_log_corner(match_layout_, area);
    const int right = area.x + area.width;
    const int width = right - (corner.x + (x - oa::ui::hud::kMessageLogLeft) * scale);
    for (const auto& span :
         oa::present::modern_text_rows(run.text, face, scale, painted_text_size(run), width))
        rows.push_back(run.text.substr(span.offset, span.bytes));
    return rows;
}

int32_t Runtime::message_log_most_rows(int32_t step) {
    const auto area = overlay_area();
    const auto corner = message_log_corner(match_layout_, area);
    int bottom = area.y + area.height;
    // The rows stop a row above the clock while it shows.
    const int scale = hud_text_scale();
    if (const auto clock = console_clock_pen_row())
        bottom = std::min(bottom, *clock - scale);
    return std::max((bottom - corner.y) / std::max(step * scale, 1), 1);
}

void Runtime::draw_match_message_log() {
    if (!match_)
        return;
    ensure_gui_font();

    // A row of a line's text, written once every backdrop is laid, so that
    // no row's backdrop covers the row above's descenders and shadow.
    struct Line {
        oa::ui::display_layout::Point pen{};
        std::string text{};
        std::array<uint8_t, 3> color{};
        bool boxed{};
        /// UTF-8 of a row of a line the modern fonts draw whole, broken at
        /// the battlefield's right edge; else the line's game text
        bool row{};
    };

    struct Paint {
        Runtime* runtime{};
        const oa::formats::fnt::Font* font{};
        std::array<uint8_t, 3> color{};
        oa::ui::display_layout::Point corner{};
        int scale{};
        int32_t step{};               ///< source rows from one row to the next
        int32_t size{};               ///< the text size, in percent
        oa::present::TextFace face{}; ///< the modern face the log's font stands for
        std::vector<Line> lines{};
    };

    const bool gui = !gui_font_.sequences.empty();
    // The log is painted on the battlefield's layer, from its corner in the
    // overlays' area.
    const auto log_corner = message_log_corner(match_layout_, overlay_area());
    Paint paint{
        this,
        &message_font(),
        {255, 255, 255},
        canvas_paint(log_corner.x, log_corner.y),
        hud_text_scale(),
        message_log_step(),
        oa::present::game_text_size(),
        gui ? renderer::gui_font_face(gui_font_) : renderer::fnt_font_face(message_font()),
    };
    oa::ui::hud::MessageLogSink sink{};
    sink.user = &paint;
    sink.font_height = [](void* user) { return static_cast<Paint*>(user)->step; };
    sink.set_color = [](void* user, uint8_t color) {
        auto& target = *static_cast<Paint*>(user);
        target.color = target.runtime->palette_rgb(color);
    };
    // The whole frame of the sender's colour logo is stretched over the
    // square, which covers x0..x1-1 and y0..y1-1.
    sink.logo =
        [](void* user, const oa::Player& player, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
            auto& target = *static_cast<Paint*>(user);
            const auto frame = target.runtime->player_logo_frame(player);
            if (!frame)
                return;
            const int32_t square[4] = {x0, y0, x1, y1};
            const auto blit = oa::ui::hud::player_logo_blit(square, frame->width, frame->height, 0);
            struct Logo {
                oa::Sprite texture{};
                oa::present::PolygonVertex quad[4]{};
                oa::present::model::TexturePoint uv[4]{};
            } logo{};
            logo.texture.width = frame->width;
            logo.texture.height = frame->height;
            logo.texture.key = frame->transparency_index;
            logo.texture.encoding = OA_SPRITE_RAW;
            logo.texture.data = const_cast<uint8_t*>(frame->pixels.data());
            for (int corner = 0; corner < 4; ++corner) {
                logo.quad[corner] = {blit.dest[2 * corner] - x0, blit.dest[2 * corner + 1] - y0};
                logo.uv[corner] = {blit.source[2 * corner], blit.source[2 * corner + 1]};
            }
            const int width = x1 - x0 + 1;
            target.runtime->overlay_patch(
                {target.corner.x + (x0 - oa::ui::hud::kMessageLogLeft) * target.scale,
                 target.corner.y + (y0 - oa::ui::hud::kMessageLogTop) * target.scale},
                width,
                y1 - y0 + 1,
                width,
                [](void* context, oa::Surface& surface) {
                    const auto& logo = *static_cast<const Logo*>(context);
                    oa::present::model::texture_quad(&surface, &logo.texture, logo.quad, logo.uv);
                },
                &logo
            );
        };
    // While the modern fonts draw game text whole, a line wider than the
    // battlefield leaves it is broken into rows, and the log keeps the rows
    // that fit above the clock or the battlefield's bottom.
    if (game_text_settings().style.modern_fonts) {
        sink.rows = [](void* user, const char* text, int32_t x) {
            const auto rows = static_cast<Paint*>(user)->runtime->message_log_rows(text, x);
            return std::max(static_cast<int32_t>(rows.size()), int32_t{1});
        };
        sink.most_rows = message_log_most_rows(paint.step);
    }
    // Gadget text: the GUI's font when it has one, else a label in the
    // line's colour.
    sink.text = [](void* user, const char* text, int32_t x, int32_t y) {
        auto& target = *static_cast<Paint*>(user);
        auto& runtime = *target.runtime;
        // The message log's backdrop, which the profile's accessible chat
        // forces on and the background setting asks for, lies under each
        // row: a black box a little wider and taller than its text.
        const auto settings = runtime.game_text_settings();
        const bool boxed = runtime.chat_backdrop_shown() ||
                           (settings.style.background && settings.style.modern_fonts);
        auto rows = runtime.message_log_rows(text, x);
        const bool broken = !rows.empty();
        if (!broken)
            rows.emplace_back(text);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            const int32_t row_y = y + static_cast<int32_t>(row) * target.step;
            const oa::ui::display_layout::Point pen{
                target.corner.x + (x - oa::ui::hud::kMessageLogLeft) * target.scale,
                target.corner.y + (row_y - oa::ui::hud::kMessageLogTop) * target.scale
            };
            if (boxed) {
                int32_t width = 0;
                if (broken) {
                    if (const auto layers = oa::present::modern_text(
                            rows[row], target.face, target.scale, target.size, false
                        ))
                        width = (layers->advance + target.scale - 1) / target.scale;
                } else if (!runtime.gui_font_.sequences.empty()) {
                    width = runtime.gui_text_width(runtime.gui_font_, text);
                } else {
                    width = (runtime.match_text_width(*target.font, text, target.scale) +
                             target.scale - 1) /
                            target.scale;
                }
                const auto backdrop = view_rules::chat_backdrop_rect(
                    x != oa::ui::hud::kMessageLogLeft, row_y, width, target.step
                );
                runtime.fill_hud_rect(
                    target.corner.x + (backdrop.x - oa::ui::hud::kMessageLogLeft) * target.scale,
                    target.corner.y + (backdrop.y - oa::ui::hud::kMessageLogTop) * target.scale,
                    backdrop.width * target.scale,
                    backdrop.height * target.scale,
                    view_rules::chat_backdrop_color
                );
            }
            target.lines.push_back({pen, std::move(rows[row]), target.color, boxed, broken});
        }
    };
    oa::ui::hud::draw_message_log(match_->state(), sink);
    // The rows over the backdrops, which lay their own boxes.
    const int32_t font_baseline =
        gui ? renderer::gui_font_baseline(gui_font_) : renderer::fnt_font_baseline(*paint.font);
    for (const auto& line : paint.lines) {
        if (line.row) {
            // A row of a broken line: the modern fonts at the text size,
            // its top at the pen, in the colour the line takes.
            if (const auto layers = oa::present::modern_text(
                    line.text, paint.face, paint.scale, paint.size, !line.boxed
                )) {
                std::ignore = paint_modern_text(
                    *layers,
                    line.pen.x,
                    line.pen.y + painted_baseline(font_baseline, paint.size) * paint.scale,
                    gui ? oa::present::gui_font_color : line.color
                );
                continue;
            }
        }
        // A row the modern fonts cannot draw shows in the font, as the code
        // page holds it.
        const std::string text =
            line.row ? oa::present::encode_game_text(line.text, game_text_utf8()) : line.text;
        if (gui) {
            overlay_gui_text(gui_font_, line.pen, text, kGuiTextRowsBelowPen, !line.boxed);
            continue;
        }
        paint_text(*paint.font, line.pen.x, line.pen.y, text, line.color, paint.scale, !line.boxed);
    }
}

CanvasRect Runtime::message_log_rect(std::size_t lines) {
    const auto area = overlay_area();
    const auto corner = message_log_corner(match_layout_, area);
    const int right = area.x + area.width;
    const int bottom = area.y + area.height;
    const auto height = static_cast<std::size_t>(message_log_step()) *
                        static_cast<std::size_t>(hud_text_scale()) * lines;
    return {
        corner.x,
        corner.y,
        right - corner.x,
        static_cast<int>(std::min(height, static_cast<std::size_t>(std::max(0, bottom - corner.y))))
    };
}

// A step of the self-checks (runtime_checks.cpp), which a build without them
// leaves out.
#if OA_SELF_CHECKS
void Runtime::check_game_speed_messages() {
    if (screen_ != Screen::match || !match_)
        throw std::runtime_error("speed check needs a running match");
    bool running = true;
    const auto key = [&](SDL_Keycode code, SDL_Scancode scancode, int times) {
        for (int press = 0; press < times; ++press) {
            SDL_Event event{};
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.key = code;
            event.key.scancode = scancode;
            handle_sdl_event(event, running);
        }
    };
    constexpr uint32_t kFrameMs = 25;
    constexpr uint32_t kFramesPerSecond = 1000 / kFrameMs;
    uint32_t clock_ms = 1000;
    const auto ticks_in_one_second = [&] {
        match_timing_.previous_clock =
            oa::base::game_loop::scaled_clock(clock_ms, match_clock_scale());
        match_timing_.remainder = 0.0F;
        const auto before = match_timing_.tick;
        for (uint32_t frame = 0; frame < kFramesPerSecond; ++frame) {
            clock_ms += kFrameMs;
            advance_match_clock(clock_ms);
        }
        return static_cast<int32_t>(match_timing_.tick - before);
    };
    const auto expect_speed = [&](int32_t speed, const char* step) {
        if (match_timing_.requested_rate != speed || match_timing_.actual_rate != speed)
            throw std::runtime_error(
                std::string("speed check: ") + step + " left the rate unchanged"
            );
        char expected[oa::sim::speed::message_bytes];
        oa::sim::speed::format_message(expected, speed, message_hooks());
        const auto lines = match_message_lines();
        if (lines.empty() || lines.back() != expected)
            throw std::runtime_error(std::string("speed check: ") + step + " posted no speed line");
        const auto ticks = ticks_in_one_second();
        const auto wanted = speed * 3;
        if (std::abs(ticks - wanted) > 1)
            throw std::runtime_error(
                std::string("speed check: ") + step + " ran " + std::to_string(ticks) +
                " ticks in a second, not " + std::to_string(wanted)
            );
    };
    // From the preferences' speed to normal, then three steps up and six down.
    const int32_t base = match_timing_.requested_rate;
    constexpr int32_t normal = oa::sim::speed::normal;
    key(SDLK_MINUS, SDL_SCANCODE_MINUS, std::max(0, base - normal));
    key(SDLK_EQUALS, SDL_SCANCODE_EQUALS, std::max(0, normal - base));
    if (match_timing_.requested_rate != normal)
        throw std::runtime_error("speed check: the keys did not reach normal speed");
    const auto normal_ticks = ticks_in_one_second();
    if (std::abs(normal_ticks - normal * 3) > 1)
        throw std::runtime_error(
            "speed check: normal speed ran " + std::to_string(normal_ticks) + " ticks in a second"
        );
    constexpr int32_t faster = normal + 3;
    key(SDLK_EQUALS, SDL_SCANCODE_EQUALS, 3);
    expect_speed(faster, "'+'");
    constexpr int32_t slower = faster - 6;
    key(SDLK_MINUS, SDL_SCANCODE_MINUS, 6);
    expect_speed(slower, "'-'");
    key(SDLK_EQUALS, SDL_SCANCODE_EQUALS, base - slower);
    if (match_timing_.requested_rate != base)
        throw std::runtime_error("speed check: '+' did not return to the starting speed");
    // A held '+' raises the speed a step with its press and with each
    // repeat, each step told to the extensions, which share it with the
    // other players; at the fastest its repeats change and tell nothing. A
    // held '-' comes back down the same way.
    const auto hold = [&](SDL_Keycode code, SDL_Scancode scancode, int repeats) {
        for (int press = 0; press <= repeats; ++press) {
            SDL_Event event{};
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.key = code;
            event.key.scancode = scancode;
            event.key.down = true;
            event.key.repeat = press > 0;
            handle_sdl_event(event, running);
        }
    };
    constexpr int kRepeatsAtFastest = 2;
    const auto range = game_speed_range();
    std::vector<uint16_t> steps_up;
    for (int32_t speed = base + 1; speed <= range.fastest; ++speed)
        steps_up.push_back(static_cast<uint16_t>(speed));
    std::vector<uint16_t> steps_down(steps_up.rbegin(), steps_up.rend());
    if (!steps_down.empty()) {
        steps_down.erase(steps_down.begin());
        steps_down.push_back(static_cast<uint16_t>(base));
    }
    const auto told_speeds = std::exchange(extension_.speed_changed, report_speed);
    auto& told = reported_speeds();
    told.clear();
    hold(
        SDLK_EQUALS, SDL_SCANCODE_EQUALS, static_cast<int>(steps_up.size()) - 1 + kRepeatsAtFastest
    );
    const auto raised = std::exchange(told, {});
    const auto held_up_to = match_timing_.requested_rate;
    hold(SDLK_MINUS, SDL_SCANCODE_MINUS, static_cast<int>(steps_down.size()) - 1);
    const auto lowered = std::exchange(told, {});
    extension_.speed_changed = told_speeds;
    if (raised != steps_up || held_up_to != range.fastest)
        throw std::runtime_error(
            "speed check: a held '+' set " + std::to_string(raised.size()) + " speeds up to " +
            std::to_string(held_up_to) + ", not a step a repeat to " + std::to_string(range.fastest)
        );
    if (lowered != steps_down || match_timing_.requested_rate != base)
        throw std::runtime_error(
            "speed check: a held '-' set " + std::to_string(lowered.size()) + " speeds down to " +
            std::to_string(match_timing_.requested_rate) + ", not a step a repeat to " +
            std::to_string(base)
        );

    uint16_t commander = 0;
    for (const auto& slot : match_->world().slots)
        if (commander == 0 && slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_)
            commander = slot.unit_index;
    if (commander == 0)
        throw std::runtime_error("speed check needs the local commander");
    // Only a unit off screen says it is under attack; with no unit listed on
    // screen the commander's notice reaches the log.
    match_->state().game.hot_unit_count = 0;
    offline_services_.command_sound(
        match_->world().slots[commander],
        static_cast<uint32_t>(oa::audio::game_audio::UnitAnnouncementCategory::under_attack)
    );
    const auto* definition = definition_for(commander);
    const std::string report =
        (definition != nullptr ? definition->display_name : std::string()) + ": Under Attack";
    bool reported = false;
    for (std::size_t pump = 0;
         pump < oa::audio::game_audio::AnnouncementQueue::capacity && !reported;
         ++pump) {
        present_unit_announcements();
        const auto lines = match_message_lines();
        reported = !lines.empty() && lines.back() == report;
    }
    if (!reported)
        throw std::runtime_error("speed check: \"" + report + "\" did not reach the message log");

    // The lines the log shows, found by the log's walk without painting,
    // must each change their rows of the battlefield in the composed frame
    // between a render with them and one after the log is cleared.
    std::size_t shown = 0;
    oa::ui::hud::MessageLogSink counter{};
    counter.user = &shown;
    counter.text = [](void* user, const char*, int32_t, int32_t) {
        ++*static_cast<std::size_t*>(user);
    };
    oa::ui::hud::draw_message_log(match_->state(), counter);
    const auto log = message_log_rect(shown);
    const auto log_pixels = [&] {
        renderer::Surface frame;
        render_match_surface();
        compose_match_frame(frame);
        return copy_rect(frame, log);
    };
    const auto with_log = log_pixels();
    oa::sim::messages::clear_messages(match_->state().game);
    const auto changed = changed_pixels(with_log, log_pixels());
    if (shown == 0 || changed < kTextMinPixels * shown)
        throw std::runtime_error(
            "speed check: the message log's " + std::to_string(shown) + " lines changed " +
            std::to_string(changed) + " pixels of the battlefield"
        );

    // A chat line another player sent, posted through the console's host with
    // its sender, is stored as such and starts with the logo of the sender's
    // colour over the square the log's walk sets aside for it; its text
    // starts past the logo. The same line from no player starts its text at
    // the log's left edge, inside that square.
    const auto* log_console = match_console();
    const auto* host = log_console != nullptr ? log_console->host : nullptr;
    if (host == nullptr || host->post_message == nullptr)
        throw std::runtime_error("speed check: the console's host posts no lines");
    auto& game = match_->state().game;
    const uint8_t sender = match_local_player_;
    const auto logo = player_logo_frame(game.players[sender]);
    if (!logo)
        throw std::runtime_error("speed check: the sender has no colour logo");

    struct LogWalk {
        int32_t line_height{};
        bool logo{};
        int32_t square[4]{}; // x0, y0, x1, y1: the logo covers x0..x1-1, y0..y1-1
        int32_t text_x{};
        int32_t text_y{};
    };

    const auto walk_log = [&] {
        LogWalk walk{message_line_step(message_font())};
        oa::ui::hud::MessageLogSink walker{};
        walker.user = &walk;
        walker.font_height = [](void* user) { return static_cast<LogWalk*>(user)->line_height; };
        walker.logo =
            [](void* user, const oa::Player&, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
                auto& walk = *static_cast<LogWalk*>(user);
                walk.logo = true;
                walk.square[0] = x0;
                walk.square[1] = y0;
                walk.square[2] = x1;
                walk.square[3] = y1;
            };
        walker.text = [](void* user, const char*, int32_t x, int32_t y) {
            auto& walk = *static_cast<LogWalk*>(user);
            walk.text_x = x;
            walk.text_y = y;
        };
        oa::ui::hud::draw_message_log(match_->state(), walker);
        return walk;
    };
    const auto newest_line = [&] {
        return messages::message_line(
            game, (game.chat_head + OA_CHAT_LINE_COUNT - 1U) % OA_CHAT_LINE_COUNT
        );
    };
    const auto frame_now = [&] {
        renderer::Surface frame;
        render_match_surface();
        compose_match_frame(frame);
        return frame;
    };
    // The canvas pixels of the log's screen pixel (x, y) are a scale x scale
    // block from the log's corner.
    const int scale = hud_text_scale();
    const auto overlays = overlay_area();
    const auto log_corner = message_log_corner(match_layout_, overlays);
    const auto rgb_at = [](const renderer::Surface& frame, int x, int y) {
        std::array<uint8_t, 3> rgb{};
        if (x < 0 || y < 0 || x >= static_cast<int>(frame.width) ||
            y >= static_cast<int>(frame.height))
            return rgb;
        const auto* pixel =
            frame.rgb.data() +
            (static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)) * 3U;
        std::copy(pixel, pixel + 3, rgb.begin());
        return rgb;
    };
    // Visits the canvas pixels of the log's screen rectangle x0..x1-1, y0..y1-1.
    const auto each_pixel = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1, const auto& visit) {
        for (int y = log_corner.y + (y0 - oa::ui::hud::kMessageLogTop) * scale;
             y < log_corner.y + (y1 - oa::ui::hud::kMessageLogTop) * scale;
             ++y)
            for (int x = log_corner.x + (x0 - oa::ui::hud::kMessageLogLeft) * scale;
                 x < log_corner.x + (x1 - oa::ui::hud::kMessageLogLeft) * scale;
                 ++x)
                visit(x, y);
    };
    const auto changed_in = [&](const renderer::Surface& before,
                                const renderer::Surface& after,
                                int32_t x0,
                                int32_t y0,
                                int32_t x1,
                                int32_t y1) {
        std::size_t count = 0;
        each_pixel(x0, y0, x1, y1, [&](int x, int y) {
            count += rgb_at(before, x, y) != rgb_at(after, x, y) ? 1 : 0;
        });
        return count;
    };
    constexpr const char* kChatText = "<sender> hello";
    // The log's right edge, in the log's screen pixels: the right edge of the
    // overlays' area, the battlefield's without the touch controls.
    const auto log_right =
        oa::ui::hud::kMessageLogLeft + (overlays.x + overlays.width - log_corner.x) / scale;

    const auto empty = frame_now();
    host->post_message(host->context, kChatText, messages::kind_player_chat, sender);
    const auto* chat = newest_line();
    if (chat == nullptr || chat->sender != sender ||
        (chat->kind & messages::kind_mask) != messages::kind_player_chat)
        throw std::runtime_error(
            "speed check: the console's host did not store the line as the sender's chat"
        );
    const auto marked = walk_log();
    if (!marked.logo || marked.text_x <= marked.square[2])
        throw std::runtime_error("speed check: the log set no logo before the sender's line");
    const auto marked_frame = frame_now();
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    write_ppm(report_directory / "native-message-log-sender.ppm", marked_frame);
    // The square shows the whole frame of the sender's colour logo, from
    // its corner (0, 0) to (width, height), stretched over it.
    const auto& square = marked.square;
    const int32_t size = square[2] - square[0];
    if (size <= 0 || square[3] - square[1] != size)
        throw std::runtime_error("speed check: the log's logo square is not square");
    auto stretched = oa::present::create_surface(size + 1, size + 1);
    oa::Sprite texture{};
    texture.width = logo->width;
    texture.height = logo->height;
    texture.key = logo->transparency_index;
    texture.encoding = OA_SPRITE_RAW;
    texture.data = const_cast<uint8_t*>(logo->pixels.data());
    const oa::present::PolygonVertex quad[4] = {{0, 0}, {size, 0}, {size, size}, {0, size}};
    const int32_t frame_width = logo->width;
    const int32_t frame_height = logo->height;
    const oa::present::model::TexturePoint whole_frame[4] = {
        {0, 0}, {frame_width, 0}, {frame_width, frame_height}, {0, frame_height}
    };
    oa::present::model::texture_quad(&stretched.surface, &texture, quad, whole_frame);
    std::size_t unlike = 0;
    each_pixel(square[0], square[1], square[2], square[3], [&](int x, int y) {
        const auto column = (x - log_corner.x) / scale - (square[0] - oa::ui::hud::kMessageLogLeft);
        const auto row = (y - log_corner.y) / scale - (square[1] - oa::ui::hud::kMessageLogTop);
        const auto index = stretched.pixels[static_cast<std::size_t>(row * (size + 1) + column)];
        unlike += rgb_at(marked_frame, x, y) != palette_rgb(index) ? 1 : 0;
    });
    if (unlike != 0)
        throw std::runtime_error(
            "speed check: the sender's line does not start with the logo of its colour (" +
            std::to_string(unlike) + " of " + std::to_string(size * size * scale * scale) +
            " pixels of its square differ)"
        );
    const auto text_bottom = marked.text_y + marked.line_height;
    const auto gap_changed =
        changed_in(empty, marked_frame, square[2], marked.text_y, marked.text_x, text_bottom);
    const auto text_changed =
        changed_in(empty, marked_frame, marked.text_x, marked.text_y, log_right, text_bottom);
    if (gap_changed != 0 || text_changed < kTextMinPixels)
        throw std::runtime_error(
            "speed check: the sender's line does not start its text past the logo (" +
            std::to_string(gap_changed) + " pixels changed between them, " +
            std::to_string(text_changed) + " past them)"
        );

    messages::clear_messages(game);
    host->post_message(
        host->context, kChatText, oa::ui::hud::kMessageKindChat, oa::ui::console::kMessageNoSender
    );
    const auto* echo = newest_line();
    if (echo == nullptr || echo->sender != messages::sender_none)
        throw std::runtime_error("speed check: a line from no player was stored with a sender");
    const auto plain = walk_log();
    if (plain.logo || plain.text_x != oa::ui::hud::kMessageLogLeft)
        throw std::runtime_error(
            "speed check: the line from no player does not start at the log's edge"
        );
    const auto plain_frame = frame_now();
    const auto plain_changed =
        changed_in(empty, plain_frame, square[0], square[1], square[2], square[3]);
    if (plain_changed == 0)
        throw std::runtime_error(
            "speed check: the line from no player left the logo's square as it was"
        );
    // The line's text is hattfont12's glyphs in their own colours with the
    // pen at the line's corner, as gadget text writes them; nothing else of
    // the battlefield changes.
    ensure_gui_font();
    if (gui_font_.sequences.empty())
        throw std::runtime_error("speed check: hattfont12.gaf did not load");
    constexpr int reach = 16;
    int text_width = 0;
    for (const unsigned char byte : std::string_view(kChatText))
        if (const auto* glyph = oa::present::gaf_frame(&gui_font_.sequences.front(), byte))
            text_width += glyph->width;
    const int patch_width = text_width + 2 * reach;
    const int patch_height = 3 * reach;
    oa::present::SurfaceBuffer passes[2]{
        oa::present::create_surface(patch_width, patch_height),
        oa::present::create_surface(patch_width, patch_height)
    };
    for (int pass = 0; pass < 2; ++pass) {
        std::fill(passes[pass].pixels.begin(), passes[pass].pixels.end(), pass == 0 ? 0x00 : 0xff);
        renderer::draw_gadget_text(
            &passes[pass].surface,
            &gui_font_,
            kChatText,
            reach,
            reach,
            renderer::gadget_text_unbounded,
            0
        );
    }
    const auto patch_glyph = [&](int32_t x, int32_t y) -> std::optional<uint8_t> {
        const int column = x - plain.text_x + reach;
        const int row = y - plain.text_y + reach;
        if (column < 0 || row < 0 || column >= patch_width || row >= patch_height)
            return std::nullopt;
        const auto offset = static_cast<std::size_t>(row * patch_width + column);
        if (passes[0].pixels[offset] != passes[1].pixels[offset])
            return std::nullopt;
        return passes[0].pixels[offset];
    };
    std::size_t glyph_pixels = 0, wrong = 0, stray = 0;
    const int32_t band_top = oa::ui::hud::kMessageLogTop;
    const int32_t band_bottom = plain.text_y + 2 * reach;
    each_pixel(oa::ui::hud::kMessageLogLeft, band_top, log_right, band_bottom, [&](int x, int y) {
        const int32_t source_x = oa::ui::hud::kMessageLogLeft + (x - log_corner.x) / scale;
        const int32_t source_y = oa::ui::hud::kMessageLogTop + (y - log_corner.y) / scale;
        if (const auto index = patch_glyph(source_x, source_y)) {
            ++glyph_pixels;
            wrong += rgb_at(plain_frame, x, y) != palette_rgb(*index) ? 1 : 0;
        } else if (rgb_at(plain_frame, x, y) != rgb_at(empty, x, y)) {
            ++stray;
        }
    });
    if (glyph_pixels == 0 || wrong != 0 || stray != 0)
        throw std::runtime_error(
            "speed check: the line from no player is not hattfont12 at its pen (" +
            std::to_string(wrong) + " of " + std::to_string(glyph_pixels) +
            " glyph pixels differ, " + std::to_string(stray) + " other pixels changed)"
        );
    messages::clear_messages(game);

    // Every kind of line the log shows steps down by the height the font's
    // header gives (14 for COMIX), whether or not it starts with a logo.
    const int32_t step = message_line_step(message_font());
    const auto quiet = frame_now();
    host->post_message(
        host->context, "unit report", messages::kind_unit_report, oa::ui::console::kMessageNoSender
    );
    host->post_message(host->context, kChatText, messages::kind_player_chat, sender);
    host->post_message(
        host->context, "elimination", messages::kind_elimination, oa::ui::console::kMessageNoSender
    );
    host->post_message(
        host->context, kChatText, messages::kind_player_chat, oa::ui::console::kMessageNoSender
    );

    // The walk draws with the step the log paints with and records each
    // line's top.
    struct RowWalk {
        int32_t step{};
        std::vector<int32_t> rows;
    } row_walk{step, {}};

    oa::ui::hud::MessageLogSink rower{};
    rower.user = &row_walk;
    rower.font_height = [](void* user) { return static_cast<RowWalk*>(user)->step; };
    rower.text = [](void* user, const char*, int32_t, int32_t y) {
        static_cast<RowWalk*>(user)->rows.push_back(y);
    };
    oa::ui::hud::draw_message_log(match_->state(), rower);
    const auto& rows = row_walk.rows;
    bool stepped = rows.size() == 4 && step + 2 == oa::formats::fnt::line_height(message_font());
    for (std::size_t line = 0; stepped && line < rows.size(); ++line)
        stepped = rows[line] == oa::ui::hud::kMessageLogTop + static_cast<int32_t>(line) * step;
    if (!stepped)
        throw std::runtime_error(
            "speed check: the log's " + std::to_string(rows.size()) +
            " lines of four kinds are not " + std::to_string(step) + " pixels apart"
        );
    // Painted, the fourth line fills its rows and nothing below them.
    const auto four_lines = frame_now();
    const auto fourth_top = oa::ui::hud::kMessageLogTop + 3 * step;
    const auto fourth = changed_in(
        quiet, four_lines, oa::ui::hud::kMessageLogLeft, fourth_top, log_right, fourth_top + step
    );
    const auto below = changed_in(
        quiet,
        four_lines,
        oa::ui::hud::kMessageLogLeft,
        fourth_top + step,
        log_right,
        fourth_top + 2 * step
    );
    if (fourth < kTextMinPixels || below != 0)
        throw std::runtime_error(
            "speed check: the log's fourth line changed " + std::to_string(fourth) +
            " pixels of its rows and " + std::to_string(below) + " below them"
        );
    messages::clear_messages(game);

    std::cout << "speed check: '+' and '-' ran " << normal * 3 << ", " << faster * 3 << " and "
              << slower * 3 << " ticks a second and posted their speed lines; \"" << report
              << "\" reached the log, whose " << shown << " lines changed " << changed
              << " pixels of the battlefield; the sender's line starts with its " << size << "x"
              << size << " colour logo, its text at x " << marked.text_x << "\n";
}
#endif

} // namespace oa::app
