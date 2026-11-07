// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// War-map graphics loaded when the Boneyards window opens, and the war-map drawer.
#include "galaxy_graphics.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "oa/present/blit.hpp"
#include "oa/present/colour_vision/colour_vision.hpp"
#include "oa/present/display.hpp"

namespace oa::boneyards::console {

namespace {

constexpr const char* k_map_static_gaf = "anims\\Map_Static.gaf";
constexpr const char* k_map_static_sequence = "Static";
constexpr const char* k_planets_gaf = "anims\\Planets.gaf";
constexpr const char* k_selected_gaf = "anims\\Selected.gaf";
constexpr const char* k_passover_gaf = "anims\\Passover.gaf";
constexpr const char* k_capitol_arm = "Capitol_Arm";
constexpr const char* k_capitol_core = "Capitol_Core";

constexpr std::array<const char*, k_aura_sides> k_aura_gafs = {
    "anims\\PlanetAuras.gaf", "anims\\PlanetAurasArm.gaf", "anims\\PlanetAurasCore.gaf"
};
constexpr std::array<const char*, k_aura_sizes> k_aura_names = {
    "25_L",
    "25_M",
    "25_S",
    "50_L",
    "50_M",
    "50_S",
    "75_L",
    "75_M",
    "75_S",
    "100_L",
    "100_M",
    "100_S"
};
constexpr std::array<const char*, k_ring_sizes> k_selection_names = {"s25", "s50", "s75", "s100"};
constexpr std::array<const char*, k_ring_sizes> k_passover_names = {"p25", "p50", "p75", "p100"};

// Grey ramp of the stars, brightest first.
constexpr std::array<uint8_t, 15> k_star_ramp = {
    81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95
};
constexpr int32_t k_star_dimmest = static_cast<int32_t>(k_star_ramp.size()) - 1;
// A star's second shade is this many steps dimmer than its core.
constexpr int32_t k_star_halo_dim = 4;

// The name label: three rows taller than the font, cleared to its key, and
// drawn 4 pixels right of the anchor and radius + 6 map units below it.
constexpr int32_t k_label_extra_rows = 3;
constexpr uint8_t k_label_key = 9;
constexpr int32_t k_label_x = 4;
constexpr int32_t k_label_drop = 6;

// Resource path the file reader resolves: forward slashes, lower case.
std::string resource_path(const char* path) {
    std::string out(path);
    for (char& c : out)
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::optional<formats::gaf::Archive> load_archive(const FileReader& files, const char* path) {
    std::vector<uint8_t> bytes;
    if (files.read == nullptr || !files.read(files.user, resource_path(path).c_str(), &bytes))
        return std::nullopt;
    formats::gaf::ParseResult parsed = formats::gaf::parse(bytes);
    if (!parsed.ok())
        return std::nullopt;
    return std::move(*parsed.archive);
}

bool same_name(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// First sequence named `name`, ignoring case.
const formats::gaf::Sequence*
find_sequence(const formats::gaf::Archive& archive, const char* name) {
    for (const auto& sequence : archive.sequences) {
        if (same_name(sequence.name, name))
            return &sequence;
    }
    return nullptr;
}

const formats::gaf::Frame* frame_at(const formats::gaf::Sequence& sequence, std::size_t index) {
    return index < sequence.frames.size() ? &sequence.frames[index] : nullptr;
}

// Frames 2, 1 and 0 of a ring sequence; false when the sequence is missing.
bool resolve_ring(
    const formats::gaf::Archive& archive,
    const char* name,
    std::array<const formats::gaf::Frame*, k_ring_frames>& out
) {
    const formats::gaf::Sequence* sequence = find_sequence(archive, name);
    if (sequence == nullptr)
        return false;
    for (std::size_t i = 0; i < k_ring_frames; ++i)
        out[i] = frame_at(*sequence, k_ring_frames - 1 - i);
    return true;
}

void resolve_capitol(
    const formats::gaf::Archive& archive,
    const char* name,
    std::array<const formats::gaf::Frame*, k_capitol_frames>& out
) {
    const formats::gaf::Sequence* sequence = find_sequence(archive, name);
    if (sequence == nullptr)
        return;
    for (std::size_t i = 0; i < k_capitol_frames; ++i)
        out[i] = frame_at(*sequence, i);
}

/// Stores a colour at a point when it lies on the surface; the line and star drawers use the same
/// test.
///
/// @param[in,out] target surface drawn into
/// @param x column
/// @param y row
/// @param color palette index
void put_pixel(Surface* target, int32_t x, int32_t y, uint8_t color) {
    if (x >= 0 && x < target->width && y >= 0 && y < target->height)
        target->pixels[target->pitch * y + x] = color;
}

// The decoded pixels of `frame`, decoding it on first use.
const formats::gaf::RenderedFrame*
decoded_frame(GalaxyGraphics& graphics, const formats::gaf::Frame* frame) {
    for (const DecodedFrame& entry : graphics.decoded) {
        if (entry.frame == frame)
            return &entry.image;
    }
    formats::gaf::RenderResult rendered = formats::gaf::render_normal(*frame);
    if (!rendered.ok())
        return nullptr;
    graphics.decoded.push_back(DecodedFrame{frame, std::move(*rendered.frame)});
    return &graphics.decoded.back().image;
}

// Drops the decoded pixels of every frame of `archive`, before it goes.
void forget_decoded(GalaxyGraphics& graphics, const formats::gaf::Archive& archive) {
    std::erase_if(graphics.decoded, [&](const DecodedFrame& entry) {
        for (const formats::gaf::Sequence& sequence : archive.sequences)
            for (const formats::gaf::Frame& frame : sequence.frames)
                if (entry.frame == &frame)
                    return true;
        return false;
    });
}

// Draws `frame` with its top-left corner, or its origin when `anchored`,
// at (x, y).
void draw_decoded(
    GalaxyGraphics& graphics,
    Surface* target,
    const formats::gaf::Frame* frame,
    int32_t x,
    int32_t y,
    bool anchored
) {
    if (frame == nullptr || target == nullptr)
        return;
    const formats::gaf::RenderedFrame* image = decoded_frame(graphics, frame);
    if (image == nullptr)
        return;
    Sprite sprite{};
    sprite.width = image->width;
    sprite.height = image->height;
    if (anchored) {
        sprite.origin_x = image->origin_x;
        sprite.origin_y = image->origin_y;
    }
    sprite.key = image->transparency_index;
    sprite.encoding = OA_SPRITE_RAW;
    sprite.data = const_cast<uint8_t*>(image->pixels.data());
    present::draw_sprite(target, &sprite, x, y);
}

// `clip` cut to the surface.
Rect32 clip_within(const Surface& target, const Rect32& clip) {
    return Rect32{
        std::max(clip.x1, 0),
        std::max(clip.y1, 0),
        std::min(clip.x2, target.width - 1),
        std::min(clip.y2, target.height - 1)
    };
}

// Arm is slot 0 and Core slot 1 of the saved aura pixels. The independent aura is never saved.
int aura_saved_slot(AuraSide side) {
    if (side == AuraSide::arm)
        return 0;
    if (side == AuraSide::core)
        return 1;
    return -1;
}

void collect_pixel_buffers(
    formats::gaf::Frame& frame, std::vector<std::vector<uint8_t>*>& buffers
) {
    if (!frame.pixels.empty())
        buffers.push_back(&frame.pixels);
    for (formats::gaf::Frame& layer : frame.layers)
        collect_pixel_buffers(layer, buffers);
}

std::vector<std::vector<uint8_t>*> aura_pixel_buffers(formats::gaf::Archive& archive) {
    std::vector<std::vector<uint8_t>*> buffers;
    for (formats::gaf::Sequence& sequence : archive.sequences)
        for (formats::gaf::Frame& frame : sequence.frames)
            collect_pixel_buffers(frame, buffers);
    return buffers;
}

std::span<const uint8_t> graphics_palette(const GalaxyGraphics& graphics) {
    if (graphics.palette == nullptr)
        return {};
    return std::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(graphics.palette->entries), sizeof(oa::Palette)
    );
}

void restore_aura(GalaxyGraphics& graphics, AuraSide side) {
    const int slot = aura_saved_slot(side);
    if (slot < 0)
        return;
    const auto saved_slot = static_cast<std::size_t>(slot);
    const std::vector<std::vector<uint8_t>>& saved = graphics.aura_originals[saved_slot];
    const auto archive_slot = static_cast<std::size_t>(side);
    if (saved.empty() || !graphics.auras[archive_slot])
        return;
    const std::vector<std::vector<uint8_t>*> buffers =
        aura_pixel_buffers(*graphics.auras[archive_slot]);
    if (buffers.size() != saved.size())
        return;
    for (std::size_t i = 0; i < buffers.size(); ++i) {
        if (buffers[i]->size() == saved[i].size())
            *buffers[i] = saved[i];
    }
}

void save_aura(GalaxyGraphics& graphics, AuraSide side) {
    const int slot = aura_saved_slot(side);
    if (slot < 0)
        return;
    std::vector<std::vector<uint8_t>>& saved =
        graphics.aura_originals[static_cast<std::size_t>(slot)];
    if (!saved.empty())
        return;
    const auto archive_slot = static_cast<std::size_t>(side);
    if (!graphics.auras[archive_slot])
        return;
    for (const std::vector<uint8_t>* buffer : aura_pixel_buffers(*graphics.auras[archive_slot]))
        saved.push_back(*buffer);
}

void remap_aura(GalaxyGraphics& graphics, AuraSide side, std::span<const uint8_t> palette) {
    const auto archive_slot = static_cast<std::size_t>(side);
    if (!graphics.auras[archive_slot])
        return;
    const std::array<uint8_t, 256> map = screens::galaxy_aura_map(side == AuraSide::arm, palette);
    for (std::vector<uint8_t>* buffer : aura_pixel_buffers(*graphics.auras[archive_slot]))
        present::colour_vision::remap_indices(*buffer, map, static_cast<int>(screens::kWidgetKey));
}

void forget_aura(GalaxyGraphics& graphics, AuraSide side) {
    const auto archive_slot = static_cast<std::size_t>(side);
    if (graphics.auras[archive_slot])
        forget_decoded(graphics, *graphics.auras[archive_slot]);
}

// A colour-vision mode recolours the Arm and Core auras in place, and off puts the saved pixels
// back. The same mode applied twice leaves the pixels alone, so an unchanged mode does not rewrite
// a frame.
void apply_aura_colours(GalaxyGraphics& graphics) {
    const auto now = static_cast<uint8_t>(present::colour_vision::active_mode());
    if (now == graphics.aura_mode)
        return;
    restore_aura(graphics, AuraSide::arm);
    restore_aura(graphics, AuraSide::core);
    if (now == static_cast<uint8_t>(present::colour_vision::Mode::off)) {
        const bool restored =
            !graphics.aura_originals[0].empty() || !graphics.aura_originals[1].empty();
        graphics.aura_originals[0].clear();
        graphics.aura_originals[1].clear();
        graphics.aura_mode = now;
        if (restored) {
            forget_aura(graphics, AuraSide::arm);
            forget_aura(graphics, AuraSide::core);
        }
        return;
    }
    const std::span<const uint8_t> palette = graphics_palette(graphics);
    if (palette.size() < static_cast<std::size_t>(oa::present::colour_vision::kPaletteColours) * 4U)
        return;
    save_aura(graphics, AuraSide::arm);
    save_aura(graphics, AuraSide::core);
    remap_aura(graphics, AuraSide::arm, palette);
    remap_aura(graphics, AuraSide::core, palette);
    graphics.aura_mode = now;
    forget_aura(graphics, AuraSide::arm);
    forget_aura(graphics, AuraSide::core);
}

uint8_t shown_link_colour(const GalaxyGraphics& graphics, int32_t stored) {
    return static_cast<uint8_t>(screens::galaxy_link_index(stored, graphics_palette(graphics)));
}

} // namespace

const formats::gaf::Sequence*
galaxy_find_sequence(const formats::gaf::Archive& archive, const char* name) {
    return find_sequence(archive, name);
}

bool galaxy_load_auras(GalaxyGraphics& graphics, char side, const FileReader& files) {
    AuraSide slot = AuraSide::independent;
    switch (side) {
    case 'A':
    case 'a':
        slot = AuraSide::arm;
        break;
    case 'C':
    case 'c':
        slot = AuraSide::core;
        break;
    case 'I':
    case 'i':
        break;
    default:
        return false;
    }
    const auto index = static_cast<std::size_t>(slot);
    auto& archive = graphics.auras[index];
    auto& sequences = graphics.aura_sequences[index];
    if (slot == AuraSide::arm || slot == AuraSide::core) {
        const int saved = aura_saved_slot(slot);
        if (saved >= 0)
            graphics.aura_originals[static_cast<std::size_t>(saved)].clear();
        // Not a mode: the next draw puts back any side still recoloured, then applies the mode.
        graphics.aura_mode = 0xFF;
    }
    archive = load_archive(files, k_aura_gafs[index]);
    if (!archive)
        return false;
    sequences.fill(nullptr);
    for (std::size_t i = 0; i < k_aura_sizes; ++i)
        sequences[i] = find_sequence(*archive, k_aura_names[i]);
    for (const formats::gaf::Sequence* sequence : sequences) {
        if (sequence == nullptr) {
            archive.reset();
            sequences.fill(nullptr);
            return false;
        }
    }
    return true;
}

bool galaxy_load_rings(
    GalaxyGraphics& graphics, const formats::gaf::Archive* capitols, const FileReader& files
) {
    graphics.selected = load_archive(files, k_selected_gaf);
    if (!graphics.selected)
        return false;
    graphics.passover = load_archive(files, k_passover_gaf);
    if (!graphics.passover || capitols == nullptr)
        return false;
    graphics.selection_rings = {};
    graphics.passover_rings = {};
    graphics.capitol_core.fill(nullptr);
    graphics.capitol_arm.fill(nullptr);
    for (std::size_t i = 0; i < k_ring_sizes; ++i) {
        if (!resolve_ring(*graphics.selected, k_selection_names[i], graphics.selection_rings[i]))
            return false;
    }
    for (std::size_t i = 0; i < k_ring_sizes; ++i) {
        if (!resolve_ring(*graphics.passover, k_passover_names[i], graphics.passover_rings[i]))
            return false;
    }
    resolve_capitol(*capitols, k_capitol_arm, graphics.capitol_arm);
    resolve_capitol(*capitols, k_capitol_core, graphics.capitol_core);
    return true;
}

bool galaxy_load_graphics(GalaxyGraphics& graphics, const FileReader& files) {
    graphics.map_archive = load_archive(files, k_map_static_gaf);
    if (!graphics.map_archive)
        return false;
    graphics.map_static = find_sequence(*graphics.map_archive, k_map_static_sequence);
    if (graphics.map_static == nullptr)
        return false;
    graphics.map_static_frames = static_cast<std::int32_t>(graphics.map_static->frames.size());
    const auto independent = static_cast<std::size_t>(AuraSide::independent);
    if (!galaxy_load_auras(graphics, 'I', files) || !galaxy_load_auras(graphics, 'C', files) ||
        !galaxy_load_auras(graphics, 'A', files) ||
        !galaxy_load_rings(
            graphics, graphics.auras[independent] ? &*graphics.auras[independent] : nullptr, files
        ))
        return false;
    graphics.planets = load_archive(files, k_planets_gaf);
    return graphics.planets.has_value();
}

void galaxy_draw_frame(
    GalaxyGraphics& graphics,
    Surface* target,
    const formats::gaf::Frame* frame,
    int32_t x,
    int32_t y
) {
    draw_decoded(graphics, target, frame, x, y, true);
}

bool galaxy_load_panorama(
    GalaxyGraphics& graphics,
    std::size_t index,
    const screens::GalaxyPlanet& planet,
    const FileReader& files
) {
    if (index >= screens::kMaxGalaxyPlanets)
        return false;
    const formats::gaf::Sequence*& sequence = graphics.pan_sequences[index];
    if (sequence != nullptr)
        return true;
    std::optional<formats::gaf::Archive>& archive = graphics.pans[index];
    if (archive)
        forget_decoded(graphics, *archive);
    archive = load_archive(files, planet.pan_gaf);
    if (!archive)
        return false;
    sequence = find_sequence(*archive, planet.pan_seq);
    if (sequence == nullptr)
        return false;
    PanoramaStrip& strip = graphics.pan_strips[index];
    strip = {};
    const std::size_t count = std::min(sequence->frames.size(), screens::kMaxReconPanFrames);
    for (std::size_t i = 0; i < count; ++i) {
        strip.offsets[i] = strip.total;
        strip.total += sequence->frames[i].width;
    }
    strip.frame_count = static_cast<int32_t>(count);
    return true;
}

void galaxy_release_panoramas(GalaxyGraphics& graphics) {
    for (std::optional<formats::gaf::Archive>& archive : graphics.pans) {
        if (archive)
            forget_decoded(graphics, *archive);
        archive.reset();
    }
    graphics.pan_sequences = {};
    graphics.pan_strips = {};
}

void galaxy_draw_panorama(
    GalaxyGraphics& graphics,
    std::size_t index,
    Surface* target,
    int32_t scroll,
    int32_t end,
    const Rect32& clip
) {
    if (target == nullptr || index >= screens::kMaxGalaxyPlanets ||
        graphics.pan_sequences[index] == nullptr)
        return;
    const PanoramaStrip& strip = graphics.pan_strips[index];
    if (strip.frame_count <= 0)
        return;
    const auto count = static_cast<std::size_t>(strip.frame_count);
    std::size_t first = 0;
    std::size_t last = 0;
    for (std::size_t i = 1; i < count; ++i) {
        first += strip.offsets[i] <= scroll ? 1 : 0;
        last += strip.offsets[i] <= end ? 1 : 0;
    }
    const std::vector<formats::gaf::Frame>& frames = graphics.pan_sequences[index]->frames;
    const int32_t x = strip.offsets[first] - scroll + k_recon_scan_x;
    const Rect32 saved = target->clip;
    target->clip = clip_within(*target, clip);
    draw_decoded(graphics, target, &frames[first], x, k_recon_scan_y, false);
    if (last != first)
        draw_decoded(
            graphics, target, &frames[last], x + frames[first].width, k_recon_scan_y, false
        );
    target->clip = saved;
}

void galaxy_draw_map_static(
    GalaxyGraphics& graphics, int32_t frame, Surface* target, const Rect32& clip
) {
    if (target == nullptr || graphics.map_static == nullptr)
        return;
    const Rect32 saved = target->clip;
    target->clip = clip_within(*target, clip);
    galaxy_draw_frame(graphics, target, formats::gaf::frame_at(graphics.map_static, frame), 0, 0);
    target->clip = saved;
}

void galaxy_render_label(GalaxyGraphics& graphics, std::size_t index, const char* name) {
    present::SurfaceBuffer& label = graphics.labels[index];
    label = {};
    if (graphics.label_toolkit == nullptr || graphics.system_fonts == nullptr)
        return;
    const Handle font = graphics.system_fonts->small_light;
    int32_t width = 0;
    int32_t height = 0;
    toolkit_measure_text(graphics.label_toolkit, font, name, &width, &height);
    label = present::create_surface(width, height + k_label_extra_rows);
    present::clear_surface(&label.surface, k_label_key);
    toolkit_draw_text_run(
        graphics.label_toolkit, &label.surface, font, name, static_cast<int32_t>(std::strlen(name))
    );
}

void galaxy_draw_line(
    Surface* target, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color
) {
    const int32_t dy = y1 - y0;
    const int32_t dx = x1 - x0;
    if (std::abs(dy) < std::abs(dx)) {
        int32_t x = x1;
        int32_t end = x0;
        double y = y1;
        if (x0 < x1) {
            x = x0;
            end = x1;
            y = y0;
        }
        const double slope = static_cast<double>(dy) / dx;
        for (; x < end; ++x) {
            put_pixel(target, x, static_cast<int32_t>(y), color);
            y += slope;
        }
        return;
    }
    int32_t y = y1;
    int32_t end = y0;
    double x = x1;
    if (y0 < y1) {
        y = y0;
        end = y1;
        x = x0;
    }
    const double slope = static_cast<double>(dx) / dy;
    for (; y < end; ++y) {
        put_pixel(target, static_cast<int32_t>(x), y, color);
        x += slope;
    }
}

void galaxy_draw_star(Surface* target, const screens::GalaxyStar& star, int32_t layer_dim) {
    if (!star.visible)
        return;
    const int32_t dim = star.dim + layer_dim;
    const uint8_t core = k_star_ramp[static_cast<std::size_t>(std::min(dim, k_star_dimmest))];
    const uint8_t halo =
        k_star_ramp[static_cast<std::size_t>(std::min(dim + k_star_halo_dim, k_star_dimmest))];
    const int32_t x = star.view_x;
    const int32_t y = star.view_y;
    switch (star.kind) {
    case screens::StarKind::point:
        put_pixel(target, x, y, core);
        break;
    case screens::StarKind::pair:
        switch (star.variant) {
        case 0:
            put_pixel(target, x, y, halo);
            put_pixel(target, x + 1, y, core);
            break;
        case 1:
            put_pixel(target, x, y, core);
            put_pixel(target, x + 1, y, halo);
            break;
        case 2:
            put_pixel(target, x, y, halo);
            put_pixel(target, x, y + 1, core);
            break;
        case 3:
            put_pixel(target, x, y, core);
            put_pixel(target, x, y + 1, halo);
            break;
        default:
            break;
        }
        break;
    case screens::StarKind::square:
        if (star.variant == 0) {
            put_pixel(target, x, y, halo);
            put_pixel(target, x + 1, y, core);
            put_pixel(target, x, y + 1, core);
            put_pixel(target, x + 1, y + 1, halo);
        } else if (star.variant == 1) {
            put_pixel(target, x, y, core);
            put_pixel(target, x + 1, y, halo);
            put_pixel(target, x, y + 1, halo);
            put_pixel(target, x + 1, y + 1, core);
        }
        break;
    case screens::StarKind::cross:
        put_pixel(target, x + 1, y, halo);
        put_pixel(target, x, y + 1, halo);
        put_pixel(target, x + 1, y + 1, core);
        put_pixel(target, x + 2, y + 1, halo);
        put_pixel(target, x + 1, y + 2, halo);
        break;
    default:
        break;
    }
}

void galaxy_draw_star_layer(Surface* target, const screens::GalaxyStarLayer& layer) {
    for (int32_t i = 0; i < layer.star_count; ++i)
        galaxy_draw_star(target, layer.stars[i], layer.dim);
}

void galaxy_draw(GalaxyGraphics& graphics, const screens::GalaxyView& view, Surface* viewer) {
    apply_aura_colours(graphics);
    if (screens::galaxy_shows_war_map(&view)) {
        for (const screens::GalaxyStarLayer& layer : view.star_layers)
            galaxy_draw_star_layer(viewer, layer);
        for (std::size_t i = 0; i < view.connector_count; ++i) {
            const screens::GalaxyConnector& c = view.connectors[i];
            if (c.visible)
                galaxy_draw_line(
                    viewer,
                    c.view_ax,
                    c.view_ay,
                    c.view_bx,
                    c.view_by,
                    shown_link_colour(graphics, c.color)
                );
        }
        const int32_t scale = view.scale;
        for (std::size_t i = 0; i < view.planet_count; ++i) {
            const screens::GalaxyPlanet& p = view.planets[i];
            if (!p.visible || scale < 0 || scale >= screens::kGalaxyScaleCount ||
                p.size_class < 0 || p.size_class >= static_cast<int32_t>(k_ring_frames))
                continue;
            const auto size = static_cast<std::size_t>(scale);
            const auto size_class = static_cast<std::size_t>(p.size_class);
            const std::size_t variant = size * k_ring_frames + size_class;
            const formats::gaf::Sequence* aura = nullptr;
            switch (p.side) {
            case 'A':
            case 'a':
                aura = graphics.aura_sequences[static_cast<std::size_t>(AuraSide::arm)][variant];
                break;
            case 'C':
            case 'c':
                aura = graphics.aura_sequences[static_cast<std::size_t>(AuraSide::core)][variant];
                break;
            case 'I':
            case 'i':
                aura =
                    graphics
                        .aura_sequences[static_cast<std::size_t>(AuraSide::independent)][variant];
                break;
            default:
                break;
            }
            if (aura != nullptr)
                galaxy_draw_frame(
                    graphics, viewer, formats::gaf::frame_at(aura, p.activity), p.view_x, p.view_y
                );
            if (p.capitol) {
                const formats::gaf::Frame* marker = nullptr;
                switch (p.initial_side) {
                case 'A':
                case 'a':
                    marker = graphics.capitol_arm[variant];
                    break;
                case 'C':
                case 'c':
                    marker = graphics.capitol_core[variant];
                    break;
                default:
                    break;
                }
                galaxy_draw_frame(graphics, viewer, marker, p.view_x, p.view_y);
            }
            const auto index = static_cast<int32_t>(i);
            if (index == view.selected)
                galaxy_draw_frame(
                    graphics, viewer, graphics.selection_rings[size][size_class], p.view_x, p.view_y
                );
            if (index == view.hovered)
                galaxy_draw_frame(
                    graphics, viewer, graphics.passover_rings[size][size_class], p.view_x, p.view_y
                );
            galaxy_draw_frame(
                graphics,
                viewer,
                formats::gaf::frame_at(graphics.planet_sequences[i][size], p.frame),
                p.view_x,
                p.view_y
            );
            if (scale != 0 || index == view.selected)
                present::blit_surface_keyed(
                    viewer,
                    graphics.labels[i].surface,
                    p.view_x + k_label_x,
                    (p.radius + k_label_drop) * (scale + 1) / screens::kMapUnitsPerPixelAtLevel0 +
                        p.view_y,
                    k_label_key
                );
        }
    }
    if (view.loading && !view.show_map_while_loading && graphics.map_static != nullptr)
        galaxy_draw_frame(
            graphics, viewer, formats::gaf::frame_at(graphics.map_static, view.static_frame), 0, 0
        );
}

std::size_t galaxy_count_drawn_planets(
    GalaxyGraphics& graphics, const screens::GalaxyView& view, const Surface& viewer
) {
    if (view.scale < 0 || view.scale >= screens::kGalaxyScaleCount)
        return 0;
    const auto size = static_cast<std::size_t>(view.scale);
    std::size_t drawn = 0;
    for (std::size_t i = 0; i < view.planet_count; ++i) {
        const screens::GalaxyPlanet& p = view.planets[i];
        if (!p.visible || p.view_x < 0 || p.view_x >= viewer.width || p.view_y < 0 ||
            p.view_y >= viewer.height)
            continue;
        const formats::gaf::Frame* frame =
            formats::gaf::frame_at(graphics.planet_sequences[i][size], p.frame);
        const formats::gaf::RenderedFrame* image =
            frame != nullptr ? decoded_frame(graphics, frame) : nullptr;
        if (image == nullptr || image->origin_x < 0 || image->origin_x >= image->width ||
            image->origin_y < 0 || image->origin_y >= image->height)
            continue;
        const uint8_t anchor = image->pixels[static_cast<std::size_t>(
            image->origin_y * image->width + image->origin_x
        )];
        if (anchor != image->transparency_index &&
            viewer.pixels[viewer.pitch * p.view_y + p.view_x] == anchor)
            ++drawn;
    }
    return drawn;
}

} // namespace oa::boneyards::console
