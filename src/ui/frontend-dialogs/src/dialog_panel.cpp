// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Panel stack, record helpers and frame composition shared by the dialogs.
#include "dialog_internal.hpp"

#include "oa/base/text/line_break.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/formats/fnt.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <utility>

namespace oa::ui::frontend_dialogs {

namespace {

constexpr int16_t kLabelHeight = 0xf;
constexpr uint16_t kLabelColor = 0xf;
constexpr std::size_t kLabelTextBytes = 0x7f;
constexpr int32_t kLabelRightGap = 5;
constexpr const char* kGuiArtDirectory = "anims"; // set_gaf_path appends the separator

uint8_t* pixel(renderer::Surface& frame, int32_t x, int32_t y) {
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(frame.width) ||
        y >= static_cast<int32_t>(frame.height))
        return nullptr;
    return &frame.rgb
                [(static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)) * 3U];
}

uint32_t rgb_key(const uint8_t* rgb) {
    return static_cast<uint32_t>(rgb[0]) << 16 | static_cast<uint32_t>(rgb[1]) << 8 | rgb[2];
}

// Palette index of a frame pixel: the lowest entry holding its colour, else
// the nearest by summed channel difference (for pixels drawn from another
// palette). Results are cached per colour.
struct PaletteLookup {
    const PaletteBytes& palette;
    std::unordered_map<uint32_t, uint8_t> index_of;

    explicit PaletteLookup(const PaletteBytes& colors) : palette(colors) {
        for (size_t i = kPaletteColors; i-- > 0;)
            index_of[rgb_key(&palette[i * palette_entry_bytes])] = static_cast<uint8_t>(i);
    }

    uint8_t index(const uint8_t* rgb) {
        const auto key = rgb_key(rgb);
        if (const auto found = index_of.find(key); found != index_of.end())
            return found->second;
        int32_t best = std::numeric_limits<int32_t>::max();
        uint8_t nearest = 0;
        for (size_t i = 0; i < kPaletteColors; ++i) {
            const auto* entry = &palette[i * palette_entry_bytes];
            const int32_t distance = std::abs(entry[0] - rgb[0]) + std::abs(entry[1] - rgb[1]) +
                                     std::abs(entry[2] - rgb[2]);
            if (distance < best) {
                best = distance;
                nearest = static_cast<uint8_t>(i);
            }
        }
        index_of.emplace(key, nearest);
        return nearest;
    }
};

bool frame_consistent(const renderer::Surface& frame) {
    return frame.width != 0 && frame.height != 0 &&
           frame.rgb.size() == static_cast<std::size_t>(frame.width) * frame.height * 3U;
}

// The backdrop bitmap as an 8-bit surface of its own indices.
void build_backdrop(Dialog& dialog, const oa::Image& art) {
    dialog.has_backdrop = art.width != 0 && art.height != 0 &&
                          art.indices.size() == static_cast<size_t>(art.width) * art.height;
    if (!dialog.has_backdrop)
        return;
    dialog.backdrop =
        present::create_surface(static_cast<int32_t>(art.width), static_cast<int32_t>(art.height));
    dialog.backdrop.pixels = art.indices;
    dialog.backdrop.surface.pixels = dialog.backdrop.pixels.data();
}

// Binds a display for the duration of a draw and restores the one before.
struct DisplayBinding {
    present::DisplayContext* previous = present::display_context();

    explicit DisplayBinding(present::DisplayContext& display) { present::bind_display(&display); }

    ~DisplayBinding() { present::bind_display(previous); }

    DisplayBinding(const DisplayBinding&) = delete;
    DisplayBinding& operator=(const DisplayBinding&) = delete;
};

// The draw display: the 8-bit screen, its size and the GUI palette tables.
void prepare_display(DialogStack& stack, const DialogArt& art, int32_t width, int32_t height) {
    auto& screen = stack.screen;
    if (screen.surface.width != width || screen.surface.height != height)
        screen = present::create_surface(width, height);
    auto& display = stack.display;
    display.back_buffer = screen.surface;
    display.width = width;
    display.height = height;
    display.light_table = const_cast<uint8_t*>(art.light_table.data());
    display.shade_table = const_cast<uint8_t*>(art.shade_table.data());
    display.flags = present::display_flag_light_table | present::display_flag_shade_table;
}

// Root rectangle of a dialog clipped to the frame, inclusive.
struct FaceArea {
    int32_t left = 0;
    int32_t top = 0;
    int32_t right = -1;
    int32_t bottom = -1;
};

size_t screen_offset(const present::SurfaceBuffer& screen, int32_t x, int32_t y) {
    return static_cast<size_t>(y) * screen.surface.pitch + static_cast<size_t>(x);
}

bool fail(std::string* error, const char* reason) {
    if (error != nullptr)
        *error = reason;
    return false;
}

/// Loads the dialog as a gadget panel over `under` (the palette indices of
/// `area`, row by row) on the 8-bit screen, gives it its first draw, blits its
/// face there and releases it; `drawn` receives the area as the face left it.
bool render_panel(
    Dialog& dialog,
    const PaletteBytes& palette,
    const FaceArea& area,
    const std::vector<uint8_t>& under,
    std::vector<uint8_t>& drawn,
    std::string* error
) {
    auto& stack = dialog_stack();
    auto& screen = stack.screen;
    size_t at = 0;
    for (int32_t y = area.top; y <= area.bottom; ++y)
        for (int32_t x = area.left; x <= area.right; ++x, ++at)
            screen.pixels[screen_offset(screen, x, y)] = under[at];

    DisplayBinding binding(stack.display);
    auto panel = std::make_unique<ui::gui_input::GadgetPanel>();
    ui::gui_input::init_gadget_panel(*panel);
    ui::gadget_render::bind_renderer(*panel, stack.renderer);
    // A button with stages, such as HELP.GUI's Page 1|Page 2|Page 3, is
    // translated stage by stage as its captions are split, as on the game's
    // own screens. The renderer holds the panel's context, so the hook reads
    // the dialogs' host itself.
    panel->host.translate = [](void*, const char* text) -> const char* {
        const auto& host = dialog_stack().host;
        return host.translate != nullptr ? host.translate(host.context, text) : nullptr;
    };
    ui::gui_input::set_gaf_path(*panel, kGuiArtDirectory);
    ui::gui_input::set_default_font(
        *panel, stack.art->default_font.empty() ? nullptr : stack.art->default_font.data()
    );
    panel->gaf_fonts = stack.art->fonts;
    panel->active_gaf_font = stack.art->fonts[0];
    panel->list_skin = stack.art->common;
    panel->colors = remap_palette(dialog.resources.gui_palette, palette);
    // The dialogs place their roots against the drawn frame and shade the
    // screen below themselves.
    const uint32_t flags = dialog.flags & ~(panel_flag::centre | panel_flag::beside_hud |
                                            panel_flag::shade_below | panel_flag::no_draw);
    auto* owner = ui::gui_input::load_panel(
        *panel, dialog.resources.layout, dialog.name, flags | panel_flag::no_draw
    );
    if (owner == nullptr)
        return fail(error, "the panel records cannot be loaded");
    if ((dialog.flags & panel_flag::modal_backdrop) != 0 && dialog.has_backdrop)
        owner->backdrop = &dialog.backdrop.surface;
    auto records = owner->records();
    for (size_t index = 1; index < dialog.stages.size() && index < records.size(); ++index) {
        auto& record = records[index];
        if (ui::gui_layout::gadget_type_of(record) != ui::gui_layout::gadget_type::button)
            continue;
        record.bytes[ui::gui_layout::field::button_stage] = dialog.stages[index];
        const auto at_record = static_cast<int32_t>(index);
        ui::gui_layout::set_record_i16(
            record,
            ui::gui_layout::field::button_status,
            at_record == dialog.pressed && at_record == dialog.hovered ? 1 : 0
        );
    }
    if (dialog.focus_initialized)
        owner->focus = dialog.focus;
    else {
        dialog.focus = owner->focus;
        dialog.focus_initialized = true;
    }
    const int32_t shown =
        ui::gadget_render::draw_panel(stack.renderer, *panel, flags | panel_flag::first_draw);
    if (shown == 0)
        return fail(error, "the panel does not fit the frame");
    ui::gadget_render::blit_panel_stack(*panel, &screen.surface, nullptr);
    drawn.clear();
    for (int32_t y = area.top; y <= area.bottom; ++y)
        for (int32_t x = area.left; x <= area.right; ++x)
            drawn.push_back(screen.pixels[screen_offset(screen, x, y)]);
    ui::gadget_render::draw_panel(stack.renderer, *panel, ui::gadget_render::draw_flag::release);
    return true;
}

/// Draws the dialog as its gadget panel on the 8-bit screen over the frame
/// under its root, mapped to palette indices, and writes the pixels the panel
/// drew back to the frame through the palette. On a layer, the uncovered
/// pixels map to an index the panel may draw as well: where the panel left
/// that index, a second draw over another index shows whether it drew it.
bool draw_panel_face(
    Dialog& dialog,
    renderer::Surface& frame,
    const PaletteBytes& palette,
    const uint8_t* layer_key,
    std::string* error
) {
    auto& stack = dialog_stack();
    if (!stack.art)
        return fail(error, "GUI art is not loaded");
    const auto frame_width = static_cast<int32_t>(frame.width);
    const auto frame_height = static_cast<int32_t>(frame.height);
    prepare_display(stack, *stack.art, frame_width, frame_height);
    const auto& root = dialog_root(dialog).common;
    const FaceArea area{
        std::max<int32_t>(root.x, 0),
        std::max<int32_t>(root.y, 0),
        std::min<int32_t>(root.x + root.width, frame_width) - 1,
        std::min<int32_t>(root.y + root.height, frame_height) - 1
    };
    PaletteLookup lookup(palette);
    std::vector<uint8_t> under;
    std::vector<uint8_t> uncovered;
    for (int32_t y = area.top; y <= area.bottom; ++y)
        for (int32_t x = area.left; x <= area.right; ++x) {
            const uint8_t* rgb = pixel(frame, x, y);
            under.push_back(lookup.index(rgb));
            uncovered.push_back(layer_key != nullptr && rgb_key(rgb) == rgb_key(layer_key));
        }
    std::vector<uint8_t> drawn;
    if (!render_panel(dialog, palette, area, under, drawn, error))
        return false;
    bool ambiguous = false;
    for (size_t at = 0; at < under.size(); ++at)
        ambiguous = ambiguous || (uncovered[at] != 0 && drawn[at] == under[at]);
    std::vector<uint8_t> redrawn;
    if (ambiguous) {
        std::vector<uint8_t> other = under;
        for (size_t at = 0; at < other.size(); ++at)
            if (uncovered[at] != 0)
                other[at] = static_cast<uint8_t>(~under[at]);
        if (!render_panel(dialog, palette, area, other, redrawn, error))
            return false;
    }
    size_t at = 0;
    for (int32_t y = area.top; y <= area.bottom; ++y)
        for (int32_t x = area.left; x <= area.right; ++x, ++at) {
            const uint8_t index = drawn[at];
            if (index == under[at] && (uncovered[at] == 0 || redrawn[at] != index))
                continue;
            auto* out = pixel(frame, x, y);
            const auto entry = static_cast<size_t>(index) * palette_entry_bytes;
            out[0] = palette[entry];
            out[1] = palette[entry + 1];
            out[2] = palette[entry + 2];
        }
    return true;
}

} // namespace

PaletteBytes dialog_palette(const Dialog& dialog) {
    const auto& host = dialog_stack().host;
    PaletteBytes palette{};
    if (host.active_palette != nullptr && host.active_palette(host.context, &palette))
        return palette;
    return dialog.resources.gui_palette;
}

DialogStack& dialog_stack() noexcept {
    static DialogStack stack;
    return stack;
}

Dialog* dialog_top() noexcept {
    auto& stack = dialog_stack();
    return stack.count == 0 ? nullptr : &stack.dialogs[stack.count - 1];
}

Dialog* dialog_push(
    app::ScreenContext* ctx,
    DialogKind kind,
    const char* layout,
    const char* backdrop,
    uint32_t flags
) {
    auto& stack = dialog_stack();
    if (ctx == nullptr || ctx->assets == nullptr || stack.count >= kDialogStackDepth ||
        dialog_art(*ctx->assets) == nullptr)
        return nullptr;
    auto& dialog = stack.dialogs[stack.count];
    dialog = Dialog{};
    try {
        dialog.resources = renderer::load_screen(
            *ctx->assets, {layout, backdrop != nullptr ? backdrop : "", kGuiPalette, "", kCommonGaf}
        );
    } catch (const std::exception&) {
        return nullptr;
    }
    if (dialog.resources.layout.gadgets.empty())
        return nullptr;
    dialog.kind = kind;
    dialog.flags = flags;
    dialog.stages.assign(dialog.resources.layout.gadgets.size(), 0);
    dialog.authored_x = dialog.resources.layout.gadgets.front().common.x;
    dialog.authored_y = dialog.resources.layout.gadgets.front().common.y;
    dialog.name = oa::data::defs::file_stem(layout);
    build_backdrop(dialog, dialog.resources.background);
    dialog.resources.background = {};
    ++stack.count;
    return &dialog;
}

void dialog_close(Dialog& dialog) {
    auto& stack = dialog_stack();
    std::size_t at = 0;
    while (at < stack.count && &stack.dialogs[at] != &dialog)
        ++at;
    if (at >= stack.count)
        return;
    for (std::size_t index = at; index + 1 < stack.count; ++index)
        stack.dialogs[index] = std::move(stack.dialogs[index + 1]);
    --stack.count;
    stack.dialogs[stack.count] = Dialog{};
}

void dialog_pop() {
    if (auto* top = dialog_top())
        dialog_close(*top);
}

ui::gui_layout::Gadget& dialog_root(Dialog& dialog) noexcept {
    return dialog.resources.layout.gadgets.front();
}

int32_t dialog_find(const Dialog& dialog, std::string_view name) noexcept {
    const auto& gadgets = dialog.resources.layout.gadgets;
    for (std::size_t index = 1; index < gadgets.size(); ++index)
        if (gadgets[index].common.name == name)
            return static_cast<int32_t>(index);
    return kNoGadget;
}

int32_t dialog_add_label(
    Dialog& dialog, std::string_view text, int16_t x, int16_t y, int32_t width, int32_t attributes
) {
    auto& gadgets = dialog.resources.layout.gadgets;
    if (gadgets.size() >= ui::gui_layout::limit::gadgets)
        return kNoGadget;
    ui::gui_layout::Gadget gadget{};
    gadget.common.type = ui::gui_layout::GadgetType::label;
    gadget.common.name = kLabelName;
    gadget.common.x = x;
    gadget.common.y = y;
    gadget.common.width =
        width == kLabelWidthToRootEdge
            ? static_cast<int16_t>(gadgets.front().common.width - x - kLabelRightGap)
            : static_cast<int16_t>(width);
    gadget.common.height = kLabelHeight;
    gadget.common.attributes = attributes;
    gadget.common.foreground_color = kLabelColor;
    gadget.common.active = 1;
    ui::gui_layout::LabelFields fields;
    fields.source_text =
        std::string(text.substr(0, oa::base::text::whole_character_bytes(text, kLabelTextBytes)));
    fields.text = fields.source_text;
    gadget.fields = std::move(fields);
    gadgets.push_back(std::move(gadget));
    dialog.stages.push_back(0);
    return static_cast<int32_t>(gadgets.size() - 1);
}

// Places the root as the whole-panel draw does on its first draw, against
// the drawn frame and the host's HUD strip (which may be scaled): centred,
// right of the strip, or re-centred on an axis it runs past.
void dialog_place(Dialog& dialog, int32_t screen_width, int32_t screen_height) {
    auto& root = dialog_root(dialog).common;
    root.x = dialog.authored_x;
    root.y = dialog.authored_y;
    dialog.placed_width = screen_width;
    dialog.placed_height = screen_height;
    const auto& host = dialog_stack().host;
    int32_t strip = host.hud_strip_width != nullptr ? host.hud_strip_width(host.context) : 0;
    if (strip <= 0)
        strip = kHudStripWidth;
    ui::gui_input::place_root(
        root.x,
        root.y,
        root.width,
        root.height,
        dialog.flags | panel_flag::first_draw,
        screen_width,
        screen_height,
        strip
    );
}

int32_t dialog_screen_width(const app::ScreenContext* ctx) noexcept {
    return ctx != nullptr && ctx->surface != nullptr && ctx->surface->width != 0
               ? static_cast<int32_t>(ctx->surface->width)
               : kCanvasWidth;
}

int32_t dialog_screen_height(const app::ScreenContext* ctx) noexcept {
    return ctx != nullptr && ctx->surface != nullptr && ctx->surface->height != 0
               ? static_cast<int32_t>(ctx->surface->height)
               : kCanvasHeight;
}

std::string dialog_translate(std::string_view text) {
    const auto& host = dialog_stack().host;
    std::string source(text);
    if (host.translate == nullptr)
        return source;
    const char* replacement = host.translate(host.context, source.c_str());
    return replacement != nullptr ? std::string(replacement) : source;
}

int32_t dialog_text_width(const Dialog& dialog, std::string_view text) {
    return static_cast<int32_t>(formats::fnt::measure_text(dialog.resources.font, text));
}

int32_t dialog_line_height(const Dialog& dialog) {
    return formats::fnt::line_height(dialog.resources.font);
}

void dialog_play_sound(const char* name) {
    const auto& host = dialog_stack().host;
    if (host.play_sound != nullptr)
        host.play_sound(host.context, name);
}

std::string dialog_wrap(const Dialog& dialog, std::string_view text, int32_t width) {
    ui::gui_input::GadgetPanel panel;
    panel.host.context = const_cast<formats::fnt::Font*>(&dialog.resources.font);
    panel.host.text_width = [](void* context, const void*, const char* value) {
        return static_cast<int32_t>(
            formats::fnt::measure_text(*static_cast<const formats::fnt::Font*>(context), value)
        );
    };
    return ui::gui_input::wrap_text(panel, text, width, -1);
}

void dialog_shade(
    renderer::Surface& frame,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const PaletteBytes& palette,
    const uint8_t* layer_key
) {
    const auto& art = dialog_stack().art;
    renderer::shade_panel_below(
        frame,
        x,
        y,
        width,
        height,
        palette,
        art != nullptr ? art->shade_table.data() : nullptr,
        layer_key
    );
}

bool dialog_compose(
    Dialog& dialog,
    const Dialog* below,
    renderer::Surface& frame,
    std::string* error,
    const uint8_t* layer_key
) {
    if (!frame_consistent(frame))
        return true;
    const auto frame_width = static_cast<int32_t>(frame.width);
    const auto frame_height = static_cast<int32_t>(frame.height);
    if (dialog.placed_width != frame_width || dialog.placed_height != frame_height)
        dialog_place(dialog, frame_width, frame_height);
    const auto palette = dialog_palette(dialog);
    if ((dialog.flags & panel_flag::shade_below) != 0 &&
        (below != nullptr || layer_key == nullptr)) {
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = frame_width;
        int32_t height = frame_height;
        const auto& host = dialog_stack().host;
        if (below != nullptr) {
            const auto& lower = below->resources.layout.gadgets.front().common;
            x = lower.x;
            y = lower.y;
            width = lower.width;
            height = lower.height;
        } else if (
            host.panel_below == nullptr || !host.panel_below(host.context, &x, &y, &width, &height)
        ) {
            x = 0;
            y = 0;
            width = frame_width;
            height = frame_height;
        }
        dialog_shade(frame, x, y, width, height, palette, layer_key);
    }
    return draw_panel_face(dialog, frame, palette, layer_key, error);
}

} // namespace oa::ui::frontend_dialogs
