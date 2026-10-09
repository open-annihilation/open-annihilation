// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kills board F4 slides in at the top right of the battlefield.
#include "oa/app/runtime.hpp"
#include "full_fog.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/present/model/mesh_raster.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/hud/game_clock.hpp"
#include "oa/ui/hud/health_bar.hpp"
#include "oa/ui/hud/kill_board.hpp"
#include "oa/ui/hud/resource_bar.hpp"
#include "oa/ui/hud/status_panel.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace hud = oa::ui::hud;

// An overlay is drawn over these two backgrounds; its pixels are where the
// passes agree.
constexpr uint8_t kPassFill[2] = {0x00, 0xff};
/// Most a channel of the battlefield the Full tier's card darkened under
/// the board may differ from the darkening computed exactly: the renderer
/// rounds the blend its own way.
constexpr int kMostCardShadeDifference = 2;
// Rows above and below a text line, and columns past its width, its glyphs
// may reach.
constexpr int kGlyphReach = 16;

uint32_t rgb_key(const uint8_t* rgb) {
    return static_cast<uint32_t>(rgb[0]) << 16 | static_cast<uint32_t>(rgb[1]) << 8 | rgb[2];
}

// Palette index of a composed pixel: the lowest entry of its colour, else
// the nearest by summed channel difference.
class PaletteLookup {
  public:

    explicit PaletteLookup(const oa::PaletteBytes& palette) : palette_(palette) {
        for (std::size_t i = OA_PALETTE_COLORS; i-- > 0;)
            index_of_[rgb_key(&palette_[i * 4U])] = static_cast<uint8_t>(i);
    }

    uint8_t index(const uint8_t* rgb) {
        const auto key = rgb_key(rgb);
        if (const auto found = index_of_.find(key); found != index_of_.end())
            return found->second;
        int best = std::numeric_limits<int>::max();
        uint8_t nearest = 0;
        for (std::size_t i = 0; i < OA_PALETTE_COLORS; ++i) {
            const auto* entry = &palette_[i * 4U];
            const int distance = std::abs(entry[0] - rgb[0]) + std::abs(entry[1] - rgb[1]) +
                                 std::abs(entry[2] - rgb[2]);
            if (distance < best) {
                best = distance;
                nearest = static_cast<uint8_t>(i);
            }
        }
        index_of_.emplace(key, nearest);
        return nearest;
    }

  private:

    const oa::PaletteBytes& palette_;
    std::unordered_map<uint32_t, uint8_t> index_of_;
};

} // namespace

oa::ui::display_layout::Point Runtime::board_canvas(int x, int y) const {
    // The board hangs from the top right corner of the overlays' area: the
    // battlefield's, or with the touch controls on, the part of it they
    // leave clear (left of the rail, under PAUSE and MENU on a phone).
    const int scale = hud_text_scale();
    const auto area = overlay_area();
    return {
        area.x + area.width - (oa::ui::display_layout::kSourceWidth - x) * scale,
        area.y + (y - oa::ui::display_layout::kSourceTop) * scale
    };
}

uint8_t* Runtime::board_pixel(int canvas_x, int canvas_y) {
    const auto point = canvas_paint(canvas_x, canvas_y);
    auto& layer = paint_target();
    if (point.x < 0 || point.y < 0 || point.x >= static_cast<int>(layer.width) ||
        point.y >= static_cast<int>(layer.height))
        return nullptr;
    const auto offset =
        static_cast<std::size_t>(point.y) * layer.width + static_cast<std::size_t>(point.x);
    return layer.rgb.data() + offset * 3U;
}

void Runtime::shade_board_rect(int x0, int y0, int x1, int y1, int level) {
    const auto corner = board_canvas(x0, y0);
    const auto end = board_canvas(x1 + 1, y1 + 1);
    const int width = end.x - corner.x;
    const int height = end.y - corner.y;
    if (width <= 0 || height <= 0)
        return;
    // In the Full tier the card darkens or lights the battlefield under the
    // board; the board's foreground goes on the overlay canvas.
    const auto at = canvas_paint(corner.x, corner.y);
    if (paint_world_level(at.x, at.y, width, height, level))
        return;
    PaletteLookup lookup(match_palette_);
    auto patch = oa::present::create_surface(width, height);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (const auto* rgb = board_pixel(corner.x + x, corner.y + y))
                patch.pixels[static_cast<std::size_t>(y * width + x)] = lookup.index(rgb);
    oa::Rect32 rect{0, 0, width - 1, height - 1};
    if (oa::present::shade_rect_level(&patch.surface, &rect, level) == 0)
        return;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (auto* rgb = board_pixel(corner.x + x, corner.y + y)) {
                const auto index = patch.pixels[static_cast<std::size_t>(y * width + x)];
                const auto color = palette_rgb(index);
                std::copy(color.begin(), color.end(), rgb);
            }
}

void Runtime::overlay_patch(
    oa::ui::display_layout::Point corner,
    int width,
    int height,
    int columns,
    void (*draw)(void* user, oa::Surface& surface),
    void* user,
    int scale
) {
    if (width <= 0 || height <= 0)
        return;
    oa::present::SurfaceBuffer passes[2]{
        oa::present::create_surface(width, height), oa::present::create_surface(width, height)
    };
    for (int pass = 0; pass < 2; ++pass) {
        std::fill(passes[pass].pixels.begin(), passes[pass].pixels.end(), kPassFill[pass]);
        draw(user, passes[pass].surface);
    }
    auto& layer = paint_target();
    if (scale <= 0)
        scale = hud_text_scale();
    columns = std::min(width, columns);
    for (int row = 0; row < height; ++row)
        for (int column = 0; column < columns; ++column) {
            const auto offset = static_cast<std::size_t>(row * width + column);
            const auto index = passes[0].pixels[offset];
            if (index != passes[1].pixels[offset])
                continue;
            const auto color = palette_rgb(index);
            for (int by = 0; by < scale; ++by)
                for (int bx = 0; bx < scale; ++bx) {
                    const int px = corner.x + column * scale + bx;
                    const int py = corner.y + row * scale + by;
                    if (px < 0 || py < 0 || px >= static_cast<int>(layer.width) ||
                        py >= static_cast<int>(layer.height))
                        continue;
                    const auto pixel =
                        static_cast<std::size_t>(py) * layer.width + static_cast<std::size_t>(px);
                    std::copy(color.begin(), color.end(), layer.rgb.data() + pixel * 3U);
                }
        }
}

std::optional<oa::formats::gaf::RenderedFrame>
Runtime::player_logo_frame(const oa::Player& player) {
    if (!match_)
        return std::nullopt;
    const auto* info = oa::world_player_info(&match_->state(), &player);
    const auto* logos = logo_sequence_;
    if (info == nullptr || logos == nullptr || info->color >= logos->frames.size())
        return std::nullopt;
    auto rendered = oa::formats::gaf::render_normal(logos->frames[info->color]);
    if (!rendered.ok())
        return std::nullopt;
    return std::move(*rendered.frame);
}

void Runtime::overlay_board_patch(
    int x, int y, int width, int height, void (*draw)(void* user, oa::Surface& surface), void* user
) {
    const auto corner = board_canvas(x, y);
    overlay_patch(
        canvas_paint(corner.x, corner.y),
        width,
        height,
        oa::ui::display_layout::kSourceWidth - x,
        draw,
        user
    );
}

void Runtime::ensure_gui_font() {
    if (gui_fonts_loaded_)
        return;
    gui_fonts_loaded_ = true;
    const auto load = [this](const char* path, oa::present::GafSprites& font) {
        try {
            const auto bytes = assets_.read(path).bytes;
            const auto status = renderer::load_gui_font(bytes, font);
            if (status != oa::present::GafStatus::ok) {
                font = {};
                std::cerr << path << " unavailable: " << oa::present::gaf_status_text(status)
                          << '\n';
            }
        } catch (const std::exception& error) {
            font = {};
            std::cerr << path << " unavailable: " << error.what() << '\n';
        }
    };
    load("anims/hattfont12.gaf", gui_font_);
    load("anims/hattfont11.gaf", gui_label_font_);
}

void Runtime::overlay_gui_text(
    const oa::present::GafSprites& font,
    oa::ui::display_layout::Point pen,
    std::string_view text,
    int rows_below_pen,
    bool allow_background,
    int scale
) {
    if (font.sequences.empty() || text.empty() || rows_below_pen <= 0)
        return;
    if (scale <= 0)
        scale = hud_text_scale();
    if (!renderer::needs_text_runs(text, true)) {
        std::ignore = overlay_gui_glyphs(font, pen, text, rows_below_pen, scale);
        return;
    }
    // The modern fonts sit on the font's baseline, drawn at the text's
    // scale and the size the text takes here, in hattfont12's colour.
    const int32_t font_baseline = renderer::gui_font_baseline(font);
    const auto face = renderer::gui_font_face(font);
    for (const auto& run :
         renderer::split_game_text(text, renderer::gui_font_characters(font), true)) {
        if (run.modern) {
            const int32_t size = painted_text_size(run);
            if (const auto layers =
                    oa::present::modern_text(run.text, face, scale, size, allow_background)) {
                const int baseline = pen.y + painted_baseline(font_baseline, size) * scale;
                pen.x += paint_modern_text(*layers, pen.x, baseline, oa::present::gui_font_color);
                continue;
            }
        }
        const std::string bytes =
            run.modern ? oa::present::encode_game_text(run.text, false) : run.text;
        pen.x += overlay_gui_glyphs(font, pen, bytes, rows_below_pen, scale) * scale;
    }
}

int Runtime::gui_text_width(const oa::present::GafSprites& font, std::string_view text) const {
    const int scale = hud_text_scale();
    if (!renderer::needs_text_runs(text, true))
        return renderer::measure_gadget_glyphs(font, text);
    int width = 0;
    for (const auto& run :
         renderer::split_game_text(text, renderer::gui_font_characters(font), true)) {
        if (run.modern)
            if (const auto layers = oa::present::modern_text(
                    run.text, renderer::gui_font_face(font), scale, painted_text_size(run)
                )) {
                width += (layers->advance + scale - 1) / scale;
                continue;
            }
        const std::string bytes =
            run.modern ? oa::present::encode_game_text(run.text, false) : run.text;
        width += renderer::measure_gadget_glyphs(font, bytes);
    }
    return width;
}

int Runtime::overlay_gui_glyphs(
    const oa::present::GafSprites& font,
    oa::ui::display_layout::Point pen,
    std::string_view text,
    int rows_below_pen,
    int scale
) {
    if (font.sequences.empty() || text.empty() || rows_below_pen <= 0)
        return 0;
    if (scale <= 0)
        scale = hud_text_scale();
    const std::string line(text);
    const auto* glyphs = &font.sequences.front();
    int width = 0;
    for (const unsigned char byte : line)
        if (const oa::Sprite* glyph = byte >= ' ' ? oa::present::gaf_frame(glyphs, byte) : nullptr)
            width += glyph->width;

    struct Run {
        const oa::present::GafSprites* font;
        const char* text;
    } const run{&font, line.c_str()};

    // The patch reaches a glyph's width left of the pen and a glyph's height
    // above it; below, it stops where the caller cuts the text off.
    overlay_patch(
        {pen.x - kGlyphReach * scale, pen.y - kGlyphReach * scale},
        width + 2 * kGlyphReach,
        kGlyphReach + std::min(rows_below_pen, 2 * kGlyphReach),
        width + 2 * kGlyphReach,
        [](void* context, oa::Surface& surface) {
            const auto& run = *static_cast<const Run*>(context);
            std::ignore = renderer::draw_gadget_glyphs(
                &surface,
                *run.font,
                run.text,
                kGlyphReach,
                kGlyphReach,
                renderer::gadget_text_unbounded,
                0
            );
        },
        const_cast<Run*>(&run),
        scale
    );
    return width;
}

void Runtime::draw_board_text(
    std::string_view text, int32_t x, int32_t y, int32_t width, uint8_t flash
) {
    const auto draw_glyphs = [this,
                              y](int32_t pen, std::string_view bytes, int32_t room, uint8_t light) {
        struct Line {
            const oa::present::GafSprites* font;
            std::string_view text;
            int32_t width;
            uint8_t flash;
        } line{&gui_font_, bytes, room, light};
        overlay_board_patch(
            pen,
            y - kGlyphReach,
            room + kGlyphReach,
            3 * kGlyphReach,
            [](void* context, oa::Surface& surface) {
                const auto& line = *static_cast<const Line*>(context);
                std::ignore = renderer::draw_gadget_glyphs(
                    &surface, *line.font, line.text, 0, kGlyphReach, line.width, line.flash
                );
            },
            &line
        );
    };
    if (!renderer::needs_text_runs(text, true)) {
        draw_glyphs(x, text, width, flash);
        return;
    }
    // Runs are laid one after another from the board's pen; the modern
    // fonts' are painted at the text's scale on the font's baseline, lit as
    // the glyphs are.
    const int scale = hud_text_scale();
    const auto face = renderer::gui_font_face(gui_font_);
    const auto lit = renderer::lit_text_color(oa::present::gui_font_color, match_palette_, flash);
    int32_t pen = x;
    int32_t room = width;
    for (const auto& run :
         renderer::split_game_text(text, renderer::gui_font_characters(gui_font_), true)) {
        const int32_t size = painted_text_size(run);
        std::optional<oa::present::TextLayers> layers;
        if (run.modern)
            layers = oa::present::modern_text(run.text, face, scale, size);
        if (!layers) {
            const std::string bytes =
                run.modern ? oa::present::encode_game_text(run.text, false) : run.text;
            const int32_t advance = renderer::measure_gadget_glyphs(gui_font_, bytes);
            draw_glyphs(pen, bytes, room, flash);
            if (advance > room)
                return;
            pen += advance;
            room -= advance;
            continue;
        }
        const auto corner = board_canvas(pen, y);
        const auto at = canvas_paint(corner.x, corner.y);
        const int baseline =
            at.y + painted_baseline(renderer::gui_font_baseline(gui_font_), size) * scale;
        if (layers->advance > room * scale) {
            const std::size_t fitted =
                oa::present::modern_text_fit(run.text, face, scale, size, room * scale);
            if (fitted != 0)
                if (const auto part =
                        oa::present::modern_text(run.text.substr(0, fitted), face, scale, size))
                    std::ignore = paint_modern_text(*part, at.x, baseline, lit);
            return;
        }
        const int32_t advance =
            (paint_modern_text(*layers, at.x, baseline, lit) + scale - 1) / scale;
        pen += advance;
        room -= advance;
    }
}

void Runtime::draw_match_kill_board() {
    if (!match_ || campaign_mission_)
        return;
    ensure_gui_font();
    // Its rows are laid out for the game's font.
    const PanelText panel(*this);
    auto& world = match_->state();
    hud::KillBoardSink sink{};
    sink.user = this;
    // "Kills" and "Losses" in the game's language, as gamedata\translate.tdf
    // gives them.
    sink.localize = translation_hook;
    sink.play_sound = [](void* user, const char* name) {
        static_cast<Runtime*>(user)->play_match_interface_sound(name);
    };
    sink.shade = [](void* user, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t level) {
        static_cast<Runtime*>(user)->shade_board_rect(x0, y0, x1, y1, level);
    };
    sink.text = [](
                    void* user, const char* text, int32_t x, int32_t y, int32_t width, uint8_t flash
                ) { static_cast<Runtime*>(user)->draw_board_text(text, x, y, width, flash); };
    // The advances of every byte the font has a glyph for, or as the modern
    // fonts draw the text.
    sink.text_width = [](void* user, const char* text) {
        const auto& runtime = *static_cast<Runtime*>(user);
        return static_cast<int32_t>(runtime.gui_text_width(runtime.gui_font_, text));
    };
    // The colour's logo, less its outer pixel, is stretched over the row.
    sink.logo =
        [](void* user, const oa::Player& player, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
            auto& runtime = *static_cast<Runtime*>(user);
            const auto rendered = runtime.player_logo_frame(player);
            if (!rendered)
                return;
            const auto& frame = *rendered;
            oa::Sprite texture{};
            texture.width = frame.width;
            texture.height = frame.height;
            texture.key = frame.transparency_index;
            texture.encoding = OA_SPRITE_RAW;
            texture.data = const_cast<uint8_t*>(frame.pixels.data());
            const int32_t right = x1 - x0, bottom = y1 - y0;
            const int32_t u = frame.width - 1, v = frame.height - 1;
            struct Logo {
                oa::Sprite texture;
                oa::present::PolygonVertex quad[4];
                oa::present::model::TexturePoint uv[4];
            } const logo{
                texture,
                {{0, 0}, {right, 0}, {right, bottom}, {0, bottom}},
                {{1, 1}, {u, 1}, {u, v}, {1, v}},
            };
            runtime.overlay_board_patch(
                x0,
                y0,
                right + 1,
                bottom + 1,
                [](void* context, oa::Surface& surface) {
                    const auto& logo = *static_cast<const Logo*>(context);
                    oa::present::model::texture_quad(&surface, &logo.texture, logo.quad, logo.uv);
                },
                const_cast<Logo*>(&logo)
            );
        };
    const bool held = control_key_down(oa::ui::gui_input::ControlKey::space);
    const auto now = oa::base::game_loop::scaled_clock(clock_milliseconds(), match_clock_scale());
    hud::draw_kill_board(
        world,
        kill_board_,
        now,
        hud::kill_board_wanted(world.game, held, chat_composing_),
        oa::ui::display_layout::kSourceWidth,
        sink
    );
}

namespace {

using oa::ui::display_layout::Rect;

// Distance from the pointer that covers every cursor frame.
constexpr int kCursorReach = 64;
// Readout steps that close any store gap (the readout eases an eighth a step).
constexpr int kReadoutSettleSteps = 256;
// Frames the board takes to slide all the way in or out.
constexpr int kSlideFrames = 18;
// Frames Space is held or let go for at most: the strip steps on the clock,
// once every kStatusPanelStepMs, and each frame waits a step.
constexpr int kMostSpaceFrames = 200;
constexpr int kShadeRow = 0x20 - 0x18; // shade table row of the board's level
constexpr int kHeaderWidth = 30;       // screen columns the "Kills" header covers
constexpr int kHeaderRows = 14;
constexpr int kLogoRowsBeforeName = 4;

bool inside(const Rect& rect, int x, int y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

const uint8_t* pixel(const renderer::Surface& frame, int x, int y) {
    const auto offset = static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x);
    return &frame.rgb[offset * 3U];
}

bool same_pixel(const renderer::Surface& a, const renderer::Surface& b, int x, int y) {
    return std::equal(pixel(a, x, y), pixel(a, x, y) + 3, pixel(b, x, y));
}

std::size_t
differing_inside(const renderer::Surface& a, const renderer::Surface& b, const Rect& area) {
    std::size_t count = 0;
    for (int y = area.y; y < area.y + area.height; ++y)
        for (int x = area.x; x < area.x + area.width; ++x)
            if (!same_pixel(a, b, x, y))
                ++count;
    return count;
}

std::size_t differing_outside(
    const renderer::Surface& a, const renderer::Surface& b, std::initializer_list<Rect> excluded
) {
    std::size_t count = 0;
    for (int y = 0; y < static_cast<int>(a.height); ++y)
        for (int x = 0; x < static_cast<int>(a.width); ++x) {
            const auto covers = [x, y](const Rect& rect) { return inside(rect, x, y); };
            const bool skipped = std::any_of(excluded.begin(), excluded.end(), covers);
            if (!skipped && !same_pixel(a, b, x, y))
                ++count;
        }
    return count;
}

} // namespace

void Runtime::check_kill_board() {
    namespace dialogs = oa::ui::frontend_dialogs;
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    start_benchmark_skirmish();
    auto& game = match_->state().game;
    if (const auto viewer = match_view_player(); viewer < OA_PLAYER_COUNT)
        for (int step = 0; step < kReadoutSettleSteps; ++step)
            hud::update_resource_readout(game.resource_readout, game.players[viewer], game.tick);
    // With mapping and line of sight off the whole map shows, so the board
    // shades terrain rather than unexplored black.
    namespace visibility_flag = oa::ui::console::visibility_flag;
    auto& visibility = game.visibility_flags;
    visibility = static_cast<uint8_t>(
        visibility & ~(visibility_flag::mapping | visibility_flag::line_of_sight)
    );
    reset_sight_presentation(false);
    pointer_x_ = static_cast<float>(match_layout_.left + kCursorReach);
    pointer_y_ = static_cast<float>(match_layout_.height / 2);
    const Rect cursor{
        static_cast<int>(pointer_x_) - kCursorReach,
        static_cast<int>(pointer_y_) - kCursorReach,
        2 * kCursorReach,
        2 * kCursorReach
    };
    bool running = true;
    const auto press_f4 = [&] {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = SDLK_F4;
        event.key.scancode = SDL_SCANCODE_F4;
        handle_sdl_event(event, running);
    };
    const auto capture = [&](renderer::Surface& frame) {
        capture_frame_ = &frame;
        render();
        capture_frame_ = nullptr;
    };
    const auto slide_to = [&](int32_t slide, renderer::Surface& frame) {
        int frames = 0;
        while (kill_board_.slide != slide && frames <= kSlideFrames) {
            capture(frame);
            ++frames;
        }
        if (frames != kSlideFrames)
            throw std::runtime_error(
                "kill board check: the board took " + std::to_string(frames) +
                " frames to slide to " + std::to_string(slide)
            );
        capture(frame);
    };

    renderer::Surface hidden;
    capture(hidden);
    write_ppm(report_directory / "native-kill-board-off.ppm", hidden);
    // In the Full tier the board's shading is a black quad the card blends
    // over the battlefield at the shade level's alpha; elsewhere the shade
    // table's row.
    const bool full = full_presentation();
    if (kill_board_.slide != 0)
        throw std::runtime_error("kill board check: the board is out before F4");
    press_f4();
    if ((game.graphics_flags & hud::kGraphicsBoardPinned) == 0)
        throw std::runtime_error("kill board check: F4 did not pin the board");
    if (dialogs::dialog_count() != 0 || match_paused_ || screen_ != Screen::match)
        throw std::runtime_error("kill board check: F4 opened a dialog");
    renderer::Surface shown;
    slide_to(hud::kBoardWidth, shown);
    write_ppm(report_directory / "native-kill-board-on.ppm", shown);
    if (dialogs::dialog_count() != 0 || match_paused_)
        throw std::runtime_error("kill board check: a dialog opened with the board");

    const int scale = hud_text_scale();
    const int left = oa::ui::display_layout::kSourceWidth - hud::kBoardWidth;
    const int bottom = game.player_count * hud::kBoardRowHeight + 0x2e;
    const auto corner = board_canvas(left, hud::kBoardTop);
    const auto end = board_canvas(oa::ui::display_layout::kSourceWidth, bottom + 1);
    const Rect board{corner.x, corner.y, end.x - corner.x, end.y - corner.y};
    const auto overlays = overlay_area();
    if (board.x + board.width != overlays.x + overlays.width || board.y != overlays.y)
        throw std::runtime_error("kill board check: the board is not at the battlefield's corner");
    if (differing_outside(hidden, shown, {board, cursor}) != 0)
        throw std::runtime_error("kill board check: the frame changed outside the board");
    const auto area = static_cast<std::size_t>(board.width * board.height);
    if (differing_inside(hidden, shown, board) < area / 2)
        throw std::runtime_error("kill board check: the board's corner is not drawn");

    const auto* shade = display_.context.shade_table;
    if (shade == nullptr)
        throw std::runtime_error("kill board check: no shade table");
    PaletteLookup lookup(match_palette_);
    const float kept = 1.0F - oa::app::full_fog::level_quad(hud::kBoardShadeLevel).colour.alpha;
    const auto shaded = [&](int x, int y) {
        if (full) {
            const uint8_t* under = pixel(hidden, x, y);
            std::array<uint8_t, 3> darkened{};
            for (std::size_t channel = 0; channel < 3; ++channel)
                darkened[channel] =
                    static_cast<uint8_t>(std::lround(static_cast<float>(under[channel]) * kept));
            return darkened;
        }
        const auto index = static_cast<int8_t>(lookup.index(pixel(hidden, x, y)));
        return palette_rgb(shade[kShadeRow * 0x100 + index]);
    };
    const auto shows = [&](int x, int y, const std::array<uint8_t, 3>& color) {
        const uint8_t* at = pixel(shown, x, y);
        if (!full)
            return std::equal(color.begin(), color.end(), at);
        for (std::size_t channel = 0; channel < 3; ++channel)
            if (std::abs(int{at[channel]} - int{color[channel]}) > kMostCardShadeDifference)
                return false;
        return true;
    };
    // Screen columns 515-516, left of the header and the highlight.
    for (int y = board.y; y < board.y + board.height; ++y)
        for (int x = board.x; x < board.x + 2 * scale; ++x)
            if (!shows(x, y, shaded(x, y)))
                throw std::runtime_error("kill board check: the margin is not shaded battlefield");
    std::size_t header = 0;
    const auto header_corner = board_canvas(left + 2, hud::kBoardTop);
    for (int y = header_corner.y; y < header_corner.y + kHeaderRows * scale; ++y)
        for (int x = header_corner.x; x < header_corner.x + kHeaderWidth * scale; ++x)
            if (!shows(x, y, shaded(x, y)))
                ++header;
    if (header < static_cast<std::size_t>(kHeaderWidth * scale * scale))
        throw std::runtime_error("kill board check: the Kills header is not drawn");
    const auto& local = game.players[game.local_player_index];
    const auto logo = player_logo_frame(local);
    if (!logo)
        throw std::runtime_error("kill board check: the local player has no colour logo");
    std::unordered_map<uint32_t, bool> logo_colors;
    for (const auto index : logo->pixels)
        logo_colors[rgb_key(palette_rgb(index).data())] = true;
    const int row_top = 0x54 + local.board_row * hud::kBoardRowHeight - 0x24;
    const auto logo_corner = board_canvas(left + 7, row_top);
    const auto logo_end = board_canvas(left + 0x77, row_top + kLogoRowsBeforeName);
    for (int y = logo_corner.y; y < logo_end.y; ++y)
        for (int x = logo_corner.x; x < logo_end.x; ++x)
            if (!logo_colors.contains(rgb_key(pixel(shown, x, y))))
                throw std::runtime_error("kill board check: the local row starts without its logo");

    press_f4();
    if ((game.graphics_flags & hud::kGraphicsBoardPinned) != 0)
        throw std::runtime_error("kill board check: a second F4 did not release the board");
    renderer::Surface gone;
    slide_to(0, gone);
    write_ppm(report_directory / "native-kill-board-gone.ppm", gone);
    if (differing_outside(hidden, gone, {cursor}) != 0)
        throw std::runtime_error("kill board check: the board left pixels behind");

    // The key below Escape turns the damage bars on and off where it types
    // '\', as on an Italian layout, which has no '`' key.
    const auto press_below_escape = [&] {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = SDLK_BACKSLASH;
        event.key.scancode = SDL_SCANCODE_GRAVE;
        handle_sdl_event(event, running);
    };
    const auto damage_bars = [&] { return (game.graphics_flags & hud::kGraphicsDamageBars) != 0; };
    const bool bars_before = damage_bars();
    press_below_escape();
    if (damage_bars() == bars_before)
        throw std::runtime_error(
            "kill board check: the key below Escape typing '\\' did not turn the damage bars"
        );
    press_below_escape();
    if (damage_bars() != bars_before)
        throw std::runtime_error(
            "kill board check: a second press of the key below Escape did not turn the damage "
            "bars back"
        );

    // Space: from the map's far corner with nothing selected, its press
    // selects nothing and moves no camera.
    clear_local_selection();
    apply_match_hud_for_selection();
    set_camera_position(
        std::numeric_limits<int32_t>::max() / 2, std::numeric_limits<int32_t>::max() / 2, 0
    );
    const auto selected_count = [&] {
        int count = 0;
        for (const auto& slot : match_->world().slots)
            count += slot.unit != nullptr && slot.owner_index == match_local_player_ &&
                     (slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
        return count;
    };
    // The frame drawn holds the camera to the map.
    renderer::Surface still;
    capture(still);
    const std::array<int32_t, 2> camera{match_camera_x_, match_camera_z_};
    const auto unmoved = [&](const char* when) {
        if (selected_count() != 0 || selected_match_unit_ != 0)
            throw std::runtime_error(
                std::string("kill board check: Space selected a unit ") + when
            );
        if (match_camera_x_ != camera[0] || match_camera_z_ != camera[1] ||
            tracked_match_unit_ != 0)
            throw std::runtime_error(
                std::string("kill board check: Space moved the camera ") + when
            );
    };
    const auto send_space = [&](bool down) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.key = SDLK_SPACE;
        event.key.scancode = SDL_SCANCODE_SPACE;
        event.key.down = down;
        handle_sdl_event(event, running);
    };
    send_space(true);
    space_held_by_check_ = true;
    unmoved("when pressed");
    // Held, the board slides in and the strip rises over the bottom of the
    // battlefield; let go, both leave. No frame changes the interface
    // outside the battlefield, the bottom bar among it.
    const Rect field{
        match_layout_.battlefield_x(),
        match_layout_.battlefield_y(),
        match_layout_.battlefield_width(),
        match_layout_.battlefield_height()
    };
    const auto& strip_offset = game.status_panel_offset;
    std::vector<std::string> heard;
    heard_interface_sounds_ = &heard;

    struct StopListening {
        Runtime& runtime;

        ~StopListening() { runtime.heard_interface_sounds_ = nullptr; }
    } const stop_listening{*this};

    // Each of the board and the strip sounds "Panel" as it leaves an end
    // and "Options" as it reaches the other.
    const auto heard_twice = [&](const char* name) {
        return std::count(heard.begin(), heard.end(), name) == 2;
    };
    const auto play = [&](bool held, renderer::Surface& frame, renderer::Surface* rising) {
        const int32_t slide = held ? hud::kBoardWidth : 0;
        const int32_t offset = held ? -hud::kStatusPanelRise : 0;
        heard.clear();
        int frames = 0;
        do {
            SDL_Delay(hud::kStatusPanelStepMs + 1);
            capture(frame);
            if (differing_outside(still, frame, {field, cursor}) != 0)
                throw std::runtime_error(
                    "kill board check: the interface outside the battlefield changed with the "
                    "strip's offset at " +
                    std::to_string(strip_offset)
                );
            // The first frame whose strip shows its text cut off.
            if (rising != nullptr && rising->rgb.empty() &&
                strip_offset < -hud::kStatusPanelTextDrop && strip_offset > offset)
                *rising = frame;
            ++frames;
        } while ((kill_board_.slide != slide || strip_offset != offset) &&
                 frames < kMostSpaceFrames);
        if (kill_board_.slide != slide || strip_offset != offset)
            throw std::runtime_error(
                std::string("kill board check: the board and the strip did not ") +
                (held ? "come out" : "leave") + " with Space " + (held ? "held" : "let go")
            );
        if (heard.size() != 4 || !heard_twice("Panel") || !heard_twice("Options"))
            throw std::runtime_error(
                std::string(
                    "kill board check: the board and the strip did not each sound "
                    "Panel and Options as they "
                ) +
                (held ? "came out" : "left")
            );
    };
    renderer::Surface rising;
    renderer::Surface raised;
    play(true, raised, &rising);
    unmoved("while held");
    write_ppm(report_directory / "native-kill-board-space-rising.ppm", rising);
    write_ppm(report_directory / "native-kill-board-space.ppm", raised);
    if (!status_lightbar_)
        throw std::runtime_error("kill board check: the status strip has no LIGHTBAR frame");
    // At the text's scale, less while the strip would be wider than the
    // battlefield.
    const auto strip_width = static_cast<int>(status_lightbar_->width);
    int strip_scale = scale;
    while (strip_scale > 1 && strip_width * strip_scale > overlays.width)
        --strip_scale;
    const int strip_rows = (1 + hud::kStatusPanelRise) * strip_scale;
    const Rect strip{
        overlays.x,
        overlays.y + overlays.height - strip_rows,
        std::min(strip_width * strip_scale, overlays.width),
        strip_rows
    };
    if (differing_inside(still, raised, strip) <
        static_cast<std::size_t>(strip.width) * strip.height / 2)
        throw std::runtime_error("kill board check: the status strip did not rise");
    if (differing_inside(still, raised, board) < area / 2)
        throw std::runtime_error("kill board check: Space did not slide the board in");
    if (differing_outside(still, raised, {board, strip, cursor}) != 0)
        throw std::runtime_error(
            "kill board check: Space changed the frame outside the board and the strip"
        );
    space_held_by_check_ = false;
    send_space(false);
    renderer::Surface after;
    play(false, after, nullptr);
    unmoved("after it was let go");
    if (differing_outside(still, after, {cursor}) != 0)
        throw std::runtime_error("kill board check: Space left pixels behind");

    // The console's clock gives way to the strip, which shows the game time
    // where the clock stands: hidden from the first frame Space is held,
    // while it is held and while the strip and the board close, it fades
    // back in from the first frame after their last closing frame and
    // shows whole kClockFadeInMs later.
    namespace console = oa::ui::console;
    const auto flags = console::console_flags(game);
    const auto clock_flags = [&](bool on) {
        console::set_console_flags(
            game,
            static_cast<uint16_t>(
                on ? flags | console::console_flag::clock : flags & ~console::console_flag::clock
            )
        );
    };
    // The strip closed on the last frame drawn, so the clock waits out its
    // fade first.
    SDL_Delay(hud::kClockFadeInMs);
    clock_flags(false);
    renderer::Surface no_clock;
    capture(no_clock);
    clock_flags(true);
    renderer::Surface clock_shown;
    capture(clock_shown);
    if (console_clock_opacity_ != hud::kClockOpaque)
        throw std::runtime_error("kill board check: the clock did not show whole before Space");
    write_ppm(report_directory / "native-kill-board-clock-before.ppm", clock_shown);
    Rect clock{};
    {
        int x0 = static_cast<int>(no_clock.width), y0 = static_cast<int>(no_clock.height);
        int x1 = -1, y1 = -1;
        for (int y = 0; y < static_cast<int>(no_clock.height); ++y)
            for (int x = 0; x < static_cast<int>(no_clock.width); ++x)
                if (!same_pixel(no_clock, clock_shown, x, y)) {
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                }
        if (x1 < 0)
            throw std::runtime_error("kill board check: +clock drew nothing");
        clock = {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    }
    const auto clock_hidden = [&](const char* when) {
        if (console_clock_opacity_ != 0)
            throw std::runtime_error(std::string("kill board check: the clock showed ") + when);
    };
    const auto strip_closed = [&] {
        return strip_offset == 0 &&
               (kill_board_.slide == 0 || kill_board_.slide == hud::kBoardWidth);
    };
    // The fade lasts kClockFadeInMs; a loaded machine may draw no frame in
    // all of it, and then Space is held and let go again, up to
    // kClockFadeTries times.
    constexpr int kClockFadeTries = 3;
    renderer::Surface clock_frame;
    uint32_t fading_opacity = 0;
    for (int attempt = 1;; ++attempt) {
        send_space(true);
        space_held_by_check_ = true;
        capture(clock_frame);
        clock_hidden("on the first frame Space was held");
        int clock_frames = 0;
        while ((strip_offset != -hud::kStatusPanelRise || kill_board_.slide != hud::kBoardWidth) &&
               ++clock_frames < kMostSpaceFrames) {
            SDL_Delay(hud::kStatusPanelStepMs + 1);
            capture(clock_frame);
            clock_hidden("while the strip rose");
        }
        write_ppm(report_directory / "native-kill-board-clock-held.ppm", clock_frame);
        clock_flags(false);
        renderer::Surface held_no_clock;
        capture(held_no_clock);
        clock_flags(true);
        if (differing_inside(held_no_clock, clock_frame, clock) != 0)
            throw std::runtime_error("kill board check: the clock showed over the strip");
        space_held_by_check_ = false;
        send_space(false);
        renderer::Surface last_closing;
        clock_frames = 0;
        do {
            last_closing = clock_frame;
            SDL_Delay(hud::kStatusPanelStepMs + 1);
            capture(clock_frame);
            if (!strip_closed())
                clock_hidden("while the strip closed");
            if (clock_frames == 0)
                write_ppm(report_directory / "native-kill-board-clock-closing.ppm", clock_frame);
        } while (!strip_closed() && ++clock_frames < kMostSpaceFrames);
        if (!strip_closed())
            throw std::runtime_error("kill board check: the strip did not close with the clock on");
        // The first frame after the strip's and the board's last closing frame
        // starts the fade.
        clock_hidden("on the frame the strip closed");
        write_ppm(report_directory / "native-kill-board-clock-last-closing.ppm", last_closing);
        // A frame every tenth of the fade until the clock shows.
        clock_frames = 0;
        do {
            SDL_Delay(hud::kClockFadeInMs / 10);
            capture(clock_frame);
        } while (console_clock_opacity_ == 0 && ++clock_frames < kMostSpaceFrames);
        fading_opacity = console_clock_opacity_;
        if (fading_opacity != hud::kClockOpaque || attempt == kClockFadeTries)
            break;
        std::cout << "kill board check: no frame was drawn while the clock faded in; Space again\n";
    }
    if (fading_opacity == 0 || fading_opacity >= hud::kClockOpaque ||
        differing_inside(no_clock, clock_frame, clock) == 0 ||
        differing_inside(clock_shown, clock_frame, clock) == 0)
        throw std::runtime_error(
            "kill board check: the clock did not fade in, opacity " + std::to_string(fading_opacity)
        );
    if (differing_outside(no_clock, clock_frame, {clock, cursor}) != 0)
        throw std::runtime_error("kill board check: the fading clock changed the frame around it");
    write_ppm(report_directory / "native-kill-board-clock-fading.ppm", clock_frame);
    SDL_Delay(hud::kClockFadeInMs);
    capture(clock_frame);
    if (console_clock_opacity_ != hud::kClockOpaque ||
        differing_outside(clock_shown, clock_frame, {cursor}) != 0)
        throw std::runtime_error(
            "kill board check: the clock did not show whole after its fade, opacity " +
            std::to_string(console_clock_opacity_) + ", " +
            std::to_string(differing_outside(clock_shown, clock_frame, {cursor})) +
            " pixels changed"
        );
    write_ppm(report_directory / "native-kill-board-clock-after.ppm", clock_frame);
    console::set_console_flags(game, flags);
    std::cout << "kill board check: the console clock hid while Space was held and the strip "
                 "closed, then faded in (at "
              << fading_opacity << "/256 on its first frame shown)\n";
    std::cout << "kill board check: " << board.width << 'x' << board.height << " at " << board.x
              << ',' << board.y << " on the " << match_layout_.width << 'x' << match_layout_.height
              << " canvas, shaded by " << (full ? "the card in the full tier" : "the shade table")
              << "; Space raised a " << strip.width << 'x' << strip.height << " strip at "
              << strip.x << ',' << strip.y << " and left the bottom bar as it was\n";
}

} // namespace oa::app
