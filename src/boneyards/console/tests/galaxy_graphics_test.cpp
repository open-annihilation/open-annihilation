// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// War-map graphics and the war-map drawer: synthetic cases, and with --data
// over an install's war map (the alpha_map.gwm of the multiplayer install
// OA_LAUNCHER_GAME_DIR names, else the BYPacific_map.gwm of OA_GAME_DIR's
// BY1117a) loaded through oa-rover's sector-map slots, with the graphics and
// fonts of its by.ccx.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "galaxy_graphics.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/present/surface.hpp"
#include "oa/rover/platform.h"
#include "oa/platform/system.hpp"
#include "oa/platform/files.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;
namespace fs = std::filesystem;
using screens::GalaxyPlanet;
using screens::GalaxyView;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr int32_t kViewerWidth = screens::kGalaxyViewerWidth;
constexpr int32_t kViewerHeight = screens::kGalaxyViewerHeight;
constexpr uint8_t kFill = 0xfe;
constexpr const char* kAllegiances = "CCCCIIIAAACCCCCCAAAAAAAAC";
constexpr std::size_t kPlanetCount = 25;
// The system object's first font: BYHatFontSmall87 tinted 0x54.
constexpr uint8_t kLabelTint = 0x54;

struct Store {
    oa::AssetStore assets;
    std::vector<std::string> reads;
};

// Lower case with forward slashes, as the asset store names files.
std::string asset_path(const char* path) {
    std::string out(path);
    for (char& c : out)
        c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool read_asset(void* user, const char* path, std::vector<uint8_t>* out) {
    auto* store = static_cast<Store*>(user);
    store->reads.emplace_back(path);
    try {
        *out = store->assets.read(asset_path(path)).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

uint8_t* read_for_toolkit(void* user, const char* path, size_t* size) {
    std::vector<uint8_t> bytes;
    try {
        bytes = static_cast<Store*>(user)->assets.read(asset_path(path)).bytes;
    } catch (const std::exception&) {
        return nullptr;
    }
    auto* out = static_cast<uint8_t*>(std::malloc(bytes.size() + 1));
    if (out == nullptr)
        return nullptr;
    std::memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size();
    return out;
}

bool read_nothing(void*, const char*, std::vector<uint8_t>*) {
    return false;
}

struct Canvas {
    oa::present::SurfaceBuffer buffer;

    Canvas(int32_t width, int32_t height) : buffer(oa::present::create_surface(width, height)) {
        std::fill(buffer.pixels.begin(), buffer.pixels.end(), kFill);
    }

    Surface* surface() { return &buffer.surface; }

    uint8_t at(int32_t x, int32_t y) const {
        return buffer.pixels[static_cast<std::size_t>(y * buffer.surface.pitch + x)];
    }

    // Pixels other than the fill, as (x, y, colour) triples.
    std::vector<std::array<int32_t, 3>> lit() const {
        std::vector<std::array<int32_t, 3>> out;
        for (int32_t y = 0; y < buffer.surface.height; ++y)
            for (int32_t x = 0; x < buffer.surface.width; ++x)
                if (at(x, y) != kFill)
                    out.push_back({x, y, at(x, y)});
        return out;
    }
};

using Lit = std::vector<std::array<int32_t, 3>>;

// Lays `frame` into `pixels` (a viewer-sized image) at (x, y) less its
// origin, its key left out: the expected result of drawing it.
void lay_frame(
    std::vector<uint8_t>& pixels, const oa::formats::gaf::Frame* frame, int32_t x, int32_t y
) {
    if (frame == nullptr)
        return;
    const oa::formats::gaf::RenderResult rendered = oa::formats::gaf::render_normal(*frame);
    if (!rendered.ok())
        return;
    const oa::formats::gaf::RenderedFrame& image = *rendered.frame;
    for (int32_t row = 0; row < image.height; ++row) {
        for (int32_t col = 0; col < image.width; ++col) {
            const uint8_t source = image.pixels[static_cast<std::size_t>(row * image.width + col)];
            const int32_t tx = x - image.origin_x + col;
            const int32_t ty = y - image.origin_y + row;
            if (source != image.transparency_index && tx >= 0 && tx < kViewerWidth && ty >= 0 &&
                ty < kViewerHeight)
                pixels[static_cast<std::size_t>(ty * kViewerWidth + tx)] = source;
        }
    }
}

void lay_surface(std::vector<uint8_t>& pixels, const Surface& label, int32_t x, int32_t y) {
    for (int32_t row = 0; row < label.height; ++row) {
        for (int32_t col = 0; col < label.width; ++col) {
            const uint8_t source = label.pixels[row * label.pitch + col];
            const int32_t tx = x + col;
            const int32_t ty = y + row;
            if (source != 9 && tx >= 0 && tx < kViewerWidth && ty >= 0 && ty < kViewerHeight)
                pixels[static_cast<std::size_t>(ty * kViewerWidth + tx)] = source;
        }
    }
}

std::vector<uint8_t> draw_view(GalaxyGraphics& graphics, const GalaxyView& view) {
    Canvas viewer(kViewerWidth, kViewerHeight);
    galaxy_draw(graphics, view, viewer.surface());
    return viewer.buffer.pixels;
}

// The install the --data run found; the snapshots take the frontend palette
// from its totala1.hpi.
fs::path g_install_root;

// With OA_SNAPSHOT_DIR set, writes `image` as a PPM in the frontend
// palette, the 768 bytes that end frontendx.pcx.
void write_snapshot(const Surface& image, const char* name) {
    const auto dir_value = oa::platform::environment_value("OA_SNAPSHOT_DIR");
    const char* dir = dir_value ? dir_value->c_str() : nullptr;
    if (dir == nullptr)
        return;
    constexpr long kPaletteBytes = 256 * 3;
    std::array<uint8_t, kPaletteBytes> palette{};
    const std::vector<uint8_t> frontend = oa::test::netgame::read_archived(
        g_install_root, oa::test::netgame::kBaseArchive, "bitmaps/frontendx.pcx"
    );
    if (frontend.size() < palette.size())
        return;
    std::copy(frontend.end() - kPaletteBytes, frontend.end(), palette.begin());
    std::FILE* out = oa::platform::open_file(fs::path(dir) / name, "wb");
    if (out == nullptr)
        return;
    std::fprintf(out, "P6\n%d %d\n255\n", image.width, image.height);
    for (int32_t y = 0; y < image.height; ++y)
        for (int32_t x = 0; x < image.width; ++x) {
            const auto index = static_cast<std::size_t>(image.pixels[y * image.pitch + x]);
            std::fwrite(&palette[index * 3], 1, 3, out);
        }
    std::fclose(out);
}

// A viewer-sized image drawn by `paint` over 0.
template <typename Paint>
void snapshot_image(const char* name, Paint paint) {
    oa::present::SurfaceBuffer image = oa::present::create_surface(kViewerWidth, kViewerHeight);
    paint(&image.surface);
    write_snapshot(image.surface, name);
}

// The viewer drawn from `view` over 0, as the map panel clears it.
void snapshot(GalaxyGraphics& graphics, const GalaxyView& view, const char* name) {
    snapshot_image(name, [&](Surface* viewer) { galaxy_draw(graphics, view, viewer); });
}

void test_missing() {
    const FileReader files{nullptr, read_nothing};
    GalaxyGraphics graphics;
    CHECK(!galaxy_load_graphics(graphics, files)); // missing static map
    CHECK(!galaxy_load_auras(graphics, 'X', files));
    CHECK(!galaxy_load_auras(graphics, 'a', files));
    CHECK(!graphics.auras[1].has_value());
    CHECK(!galaxy_load_rings(graphics, nullptr, files));
}

// Lines walk the longer axis from its lower end, truncating the other
// coordinate towards zero, and leave the far end out (galaxy_draw_line).
void test_lines() {
    const auto line = [](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
        Canvas canvas(12, 8);
        galaxy_draw_line(canvas.surface(), x0, y0, x1, y1, 0x42);
        return canvas.lit();
    };
    // Rows 0.3 x truncated: 0 for x 0..3, 1 for 4..6, 2 for 7..9.
    const Lit shallow = {
        {0, 0, 0x42},
        {1, 0, 0x42},
        {2, 0, 0x42},
        {3, 0, 0x42},
        {4, 1, 0x42},
        {5, 1, 0x42},
        {6, 1, 0x42},
        {7, 2, 0x42},
        {8, 2, 0x42},
        {9, 2, 0x42}
    };
    CHECK(line(0, 0, 10, 3) == shallow);
    CHECK(line(10, 3, 0, 0) == shallow); // either direction, from the lower end
    const Lit steep = {{2, 0, 0x42}, {1, 1, 0x42}, {1, 2, 0x42}, {0, 3, 0x42}, {0, 4, 0x42}};
    CHECK(line(2, 0, 0, 5) == steep);
    CHECK(line(3, 3, 3, 3).empty());
    // From x = -5, y falls 0.4 a step: about 0, -0.4 and -0.8 at x 0..2,
    // which truncation keeps on row 0 where rounding down would not.
    CHECK(line(-5, 2, 5, -2) == (Lit{{0, 0, 0x42}, {1, 0, 0x42}, {2, 0, 0x42}}));
    CHECK(line(-2, 1, 3, 1) == (Lit{{0, 1, 0x42}, {1, 1, 0x42}, {2, 1, 0x42}}));
}

// Stars light their shape in the grey ramp 81..95: the core at the star's
// dim plus the layer's, the halo four steps dimmer, both held at 95
// (galaxy_draw_star).
void test_stars() {
    const auto draw = [](screens::StarKind kind, int32_t variant, int32_t dim, int32_t layer_dim) {
        Canvas canvas(8, 8);
        screens::GalaxyStar star{};
        star.kind = kind;
        star.variant = variant;
        star.dim = dim;
        star.visible = true;
        star.view_x = 2;
        star.view_y = 2;
        galaxy_draw_star(canvas.surface(), star, layer_dim);
        return canvas.lit();
    };
    using screens::StarKind;
    constexpr int32_t core = 84; // dim 1 + layer 2
    constexpr int32_t halo = 88;
    CHECK(draw(StarKind::point, 0, 1, 2) == (Lit{{2, 2, core}}));
    CHECK(draw(StarKind::pair, 0, 1, 2) == (Lit{{2, 2, halo}, {3, 2, core}}));
    CHECK(draw(StarKind::pair, 1, 1, 2) == (Lit{{2, 2, core}, {3, 2, halo}}));
    CHECK(draw(StarKind::pair, 2, 1, 2) == (Lit{{2, 2, halo}, {2, 3, core}}));
    CHECK(draw(StarKind::pair, 3, 1, 2) == (Lit{{2, 2, core}, {2, 3, halo}}));
    CHECK(draw(StarKind::pair, 4, 1, 2).empty());
    CHECK(
        draw(StarKind::square, 0, 1, 2) ==
        (Lit{{2, 2, halo}, {3, 2, core}, {2, 3, core}, {3, 3, halo}})
    );
    CHECK(
        draw(StarKind::square, 1, 1, 2) ==
        (Lit{{2, 2, core}, {3, 2, halo}, {2, 3, halo}, {3, 3, core}})
    );
    CHECK(draw(StarKind::square, 2, 1, 2).empty());
    CHECK(
        draw(StarKind::cross, 0, 1, 2) ==
        (Lit{{3, 2, halo}, {2, 3, halo}, {3, 3, core}, {4, 3, halo}, {3, 4, halo}})
    );
    CHECK(draw(StarKind::point, 0, 12, 4) == (Lit{{2, 2, 95}}));
    CHECK(draw(StarKind::pair, 0, 8, 2) == (Lit{{2, 2, 95}, {3, 2, 91}}));
    CHECK(draw(StarKind::none, 0, 0, 0).empty());

    Canvas canvas(8, 8);
    screens::GalaxyStar star{};
    star.kind = StarKind::cross;
    star.view_x = -1;
    star.view_y = -1;
    galaxy_draw_star(canvas.surface(), star, 0);
    CHECK(canvas.lit().empty()); // not visible
    star.visible = true;
    galaxy_draw_star(canvas.surface(), star, 0);
    CHECK(canvas.lit() == (Lit{{0, 0, 81}, {1, 0, 85}, {0, 1, 85}})); // the rest is off the surface
}

// An uncompressed frame of a synthetic GAF.
struct RawFrame {
    uint16_t width;
    uint16_t height;
    int16_t origin_x;
    int16_t origin_y;
    uint8_t key;
    std::vector<uint8_t> pixels;
};

void put16(std::vector<uint8_t>& out, std::size_t at, uint16_t value) {
    out[at] = static_cast<uint8_t>(value);
    out[at + 1] = static_cast<uint8_t>(value >> 8);
}

void put32(std::vector<uint8_t>& out, std::size_t at, uint32_t value) {
    put16(out, at, static_cast<uint16_t>(value));
    put16(out, at + 2, static_cast<uint16_t>(value >> 16));
}

// A GAF with one sequence named `name` holding `frames`: the header, the
// sequence pointer, the sequence header and frame list, then each frame
// record followed by its pixels.
std::vector<uint8_t> build_gaf(const char* name, const std::vector<RawFrame>& frames) {
    constexpr std::size_t kSequenceAt = 16;
    constexpr std::size_t kFrameListAt = kSequenceAt + 40;
    std::vector<uint8_t> out(kFrameListAt + frames.size() * 8, 0);
    put32(out, 0, oa::formats::gaf::version_ta);
    put32(out, 4, 1);
    put32(out, 12, kSequenceAt);
    put16(out, kSequenceAt, static_cast<uint16_t>(frames.size()));
    std::memcpy(&out[kSequenceAt + 8], name, std::strlen(name));
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const RawFrame& frame = frames[i];
        const std::size_t at = out.size();
        put32(out, kFrameListAt + i * 8, static_cast<uint32_t>(at));
        put32(out, kFrameListAt + i * 8 + 4, 10);
        out.resize(at + 24, 0);
        put16(out, at, frame.width);
        put16(out, at + 2, frame.height);
        put16(out, at + 4, static_cast<uint16_t>(frame.origin_x));
        put16(out, at + 6, static_cast<uint16_t>(frame.origin_y));
        out[at + 8] = frame.key;
        put32(out, at + 16, static_cast<uint32_t>(out.size()));
        out.insert(out.end(), frame.pixels.begin(), frame.pixels.end());
    }
    return out;
}

// A frame whose pixel (col, row) is base + row * 16 + col, but for the
// key at (key_col, key_row) when that is inside it.
RawFrame patterned(
    uint16_t width,
    uint16_t height,
    int16_t origin_x,
    int16_t origin_y,
    uint8_t base,
    int32_t key_col = -1,
    int32_t key_row = -1
) {
    RawFrame frame{width, height, origin_x, origin_y, 0xff, {}};
    for (int32_t row = 0; row < height; ++row)
        for (int32_t col = 0; col < width; ++col)
            frame.pixels.push_back(
                col == key_col && row == key_row ? frame.key
                                                 : static_cast<uint8_t>(base + row * 16 + col)
            );
    return frame;
}

struct Synthetic {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    std::vector<std::string> reads;
};

bool read_synthetic(void* user, const char* path, std::vector<uint8_t>* out) {
    auto* synthetic = static_cast<Synthetic*>(user);
    synthetic->reads.emplace_back(path);
    for (const auto& [name, bytes] : synthetic->files) {
        if (name == path) {
            *out = bytes;
            return true;
        }
    }
    return false;
}

bool inside(const Rect32& clip, int32_t x, int32_t y) {
    return x >= clip.x1 && x <= clip.x2 && y >= clip.y1 && y <= clip.y2;
}

void put(Canvas& canvas, int32_t x, int32_t y, uint8_t color) {
    canvas.buffer.pixels[static_cast<std::size_t>(y * canvas.buffer.surface.pitch + x)] = color;
}

// Lays `frame` with its top-left corner at (x, y) into `canvas`, key pixels
// left out, inside the inclusive rectangle `clip`: the expected result of
// the Recon image draws.
void lay_top_left(Canvas& canvas, const RawFrame& frame, int32_t x, int32_t y, const Rect32& clip) {
    for (int32_t row = 0; row < frame.height; ++row) {
        for (int32_t col = 0; col < frame.width; ++col) {
            const uint8_t source = frame.pixels[static_cast<std::size_t>(row * frame.width + col)];
            if (source != frame.key && inside(clip, x + col, y + row))
                put(canvas, x + col, y + row, source);
        }
    }
}

screens::GalaxyPlanet pan_planet(const char* gaf, const char* sequence) {
    screens::GalaxyPlanet planet{};
    std::snprintf(planet.pan_gaf, sizeof planet.pan_gaf, "anims\\%s", gaf);
    std::snprintf(planet.pan_seq, sizeof planet.pan_seq, "%s", sequence);
    return planet;
}

bool same_rect(const Rect32& a, const Rect32& b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

// A two-frame strip, 5 + 4 pixels wide, whose frames carry origins the
// Recon draw leaves out: loading once, the strip table, the frame under the
// scroll and the one under the window's end drawn straight after it (also
// across the wrap), the clip, and the Map_Static fallback.
void test_synthetic_panorama() {
    const RawFrame left = patterned(5, 3, 7, 9, 0x20);
    const RawFrame right = patterned(4, 3, -3, 2, 0x60, 1, 1);
    const RawFrame one = patterned(1, 1, 0, 0, 0x10);
    Synthetic synthetic;
    synthetic.files.emplace_back("anims/pans.gaf", build_gaf("TestPan", {left, right}));
    synthetic.files.emplace_back("anims/other.gaf", build_gaf("Else", {left}));
    const std::vector<RawFrame> ones(screens::kMaxReconPanFrames + 1, one);
    synthetic.files.emplace_back("anims/long.gaf", build_gaf("LongPan", ones));
    const FileReader files{&synthetic, read_synthetic};
    auto graphics = std::make_unique<GalaxyGraphics>();

    const screens::GalaxyPlanet planet = pan_planet("Pans.GAF", "testpan");
    CHECK(galaxy_load_panorama(*graphics, 3, planet, files));
    CHECK(synthetic.reads == std::vector<std::string>{"anims/pans.gaf"});
    const oa::formats::gaf::Sequence* sequence = graphics->pan_sequences[3];
    CHECK(sequence != nullptr && sequence->name == "TestPan" && graphics->pans[3].has_value());
    const PanoramaStrip& strip = graphics->pan_strips[3];
    CHECK(strip.frame_count == 2 && strip.total == 9);
    CHECK(strip.offsets[0] == 0 && strip.offsets[1] == 5);
    // Kept, not read again.
    CHECK(galaxy_load_panorama(*graphics, 3, pan_planet("none.gaf", "x"), files));
    CHECK(synthetic.reads.size() == 1);
    CHECK(sequence->frames[0].origin_x == 7 && sequence->frames[1].origin_y == 2);

    CHECK(!galaxy_load_panorama(*graphics, 4, pan_planet("none.gaf", "TestPan"), files));
    CHECK(!graphics->pans[4].has_value() && graphics->pan_sequences[4] == nullptr);
    // An archive without the sequence stays loaded and is read again on
    // the next open, as the sequence pointer alone guards the load.
    CHECK(!galaxy_load_panorama(*graphics, 5, pan_planet("other.gaf", "TestPan"), files));
    CHECK(graphics->pans[5].has_value() && graphics->pan_sequences[5] == nullptr);
    const std::size_t reads = synthetic.reads.size();
    CHECK(!galaxy_load_panorama(*graphics, 5, pan_planet("other.gaf", "TestPan"), files));
    CHECK(synthetic.reads.size() == reads + 1);
    CHECK(!galaxy_load_panorama(*graphics, screens::kMaxGalaxyPlanets, planet, files));
    CHECK(galaxy_load_panorama(*graphics, 6, pan_planet("long.gaf", "LongPan"), files));
    constexpr auto kLongest = static_cast<int32_t>(screens::kMaxReconPanFrames);
    CHECK(graphics->pan_strips[6].frame_count == kLongest);
    CHECK(graphics->pan_strips[6].total == kLongest);

    constexpr int32_t kWidth = 32;
    constexpr int32_t kHeight = 24;
    const Rect32 scan{0xD, 0x10, kWidth - 1, kHeight - 1}; // k_recon_scan_clip on this canvas
    const Rect32 own{1, 2, 3, 4};
    const auto draw = [&](std::size_t index, int32_t scroll, int32_t end, const Rect32& clip) {
        Canvas canvas(kWidth, kHeight);
        canvas.surface()->clip = own;
        galaxy_draw_panorama(*graphics, index, canvas.surface(), scroll, end, clip);
        CHECK(same_rect(canvas.surface()->clip, own));
        return canvas.buffer.pixels;
    };
    const auto expect = [&](std::initializer_list<std::pair<const RawFrame*, int32_t>> placed,
                            const Rect32& clip) {
        Canvas canvas(kWidth, kHeight);
        for (const auto& [frame, x] : placed)
            lay_top_left(canvas, *frame, x, 0x10, clip);
        return canvas.buffer.pixels;
    };
    // Inside the first frame: it alone, its corner at 0 - scroll + 0xD.
    CHECK(draw(3, 0, 4, k_recon_scan_clip) == expect({{&left, 0xD}}, scan));
    // The window's end in the next frame: that frame follows at +5.
    CHECK(draw(3, 1, 5, k_recon_scan_clip) == expect({{&left, 0xC}, {&right, 0x11}}, scan));
    CHECK(draw(3, 5, 8, k_recon_scan_clip) == expect({{&right, 0xD}}, scan));
    // Across the wrap: the last frame's last column at 0xD and the first
    // frame's first column at 0xE.
    const std::vector<uint8_t> wrapped = draw(3, 8, 3, k_recon_scan_clip);
    CHECK(wrapped == expect({{&right, 0xA}, {&left, 0xE}}, scan));
    CHECK(wrapped[0x10 * kWidth + 0xD] == 0x63 && wrapped[0x10 * kWidth + 0xE] == 0x20);
    const Rect32 narrow{0xD, 0x10, 0xF, 0x11};
    CHECK(draw(3, 8, 3, narrow) == expect({{&right, 0xA}, {&left, 0xE}}, narrow));
    const Rect32 all{0, 0, kWidth - 1, kHeight - 1};
    const std::vector<uint8_t> unclipped = draw(3, 8, 3, Rect32{-4, -4, 100, 100});
    CHECK(unclipped == expect({{&right, 0xA}, {&left, 0xE}}, all));
    CHECK(unclipped[0x11 * kWidth + 0xB] == kFill); // the key pixel is left out
    const std::vector<uint8_t> blank(static_cast<std::size_t>(kWidth * kHeight), kFill);
    CHECK(draw(4, 0, 4, all) == blank && draw(5, 0, 4, all) == blank);

    // Without a panorama: the Map_Static frame at (0, 0) less its origin.
    const RawFrame first_static = patterned(30, 22, 2, 1, 0x80);
    const RawFrame second_static = patterned(30, 22, 2, 1, 0x90);
    const oa::formats::gaf::ParseResult parsed =
        oa::formats::gaf::parse(build_gaf("Static", {first_static, second_static}));
    CHECK(parsed.ok());
    const auto draw_static = [&](int32_t frame) {
        Canvas canvas(kWidth, kHeight);
        canvas.surface()->clip = own;
        galaxy_draw_map_static(*graphics, frame, canvas.surface(), k_recon_scan_clip);
        CHECK(same_rect(canvas.surface()->clip, own));
        return canvas.buffer.pixels;
    };
    CHECK(draw_static(0) == blank); // no static loaded
    graphics->map_archive = *parsed.archive;
    graphics->map_static = galaxy_find_sequence(*graphics->map_archive, "static");
    graphics->map_static_frames = 2;
    Canvas second(kWidth, kHeight);
    lay_top_left(second, second_static, -2, -1, scan);
    CHECK(draw_static(1) == second.buffer.pixels && second.buffer.pixels != blank);
    CHECK(draw_static(2) == blank);

    // Releasing drops the archives and their decoded frames.
    CHECK(!graphics->decoded.empty());
    galaxy_release_panoramas(*graphics);
    for (std::size_t i = 0; i < screens::kMaxGalaxyPlanets; ++i)
        CHECK(
            !graphics->pans[i].has_value() && graphics->pan_sequences[i] == nullptr &&
            graphics->pan_strips[i].frame_count == 0
        );
    CHECK(graphics->decoded.size() == 1); // the static frame's
    CHECK(draw(3, 0, 4, all) == blank);
}

/// Checks that the install's war-map graphics loaded: the static frames, auras, rings, capitol
/// markers and planets, with the auras read independent, Core, then Arm.
///
/// @param store asset store the graphics were read through, with its read log
/// @param graphics war-map graphics loaded from the install
void test_installed(Store& store, GalaxyGraphics& graphics) {
    CHECK(graphics.map_static != nullptr && graphics.map_static_frames == 3);
    for (std::size_t side = 0; side < k_aura_sides; ++side) {
        CHECK(graphics.auras[side].has_value());
        for (const auto* sequence : graphics.aura_sequences[side])
            CHECK(sequence != nullptr);
    }
    CHECK(graphics.aura_sequences[0][0]->name == "25_L");
    for (const auto& ring : graphics.selection_rings)
        CHECK(ring[2] != nullptr);
    for (const auto& ring : graphics.passover_rings)
        CHECK(ring[2] != nullptr);
    CHECK(graphics.capitol_arm[11] != nullptr && graphics.capitol_core[0] != nullptr);
    CHECK(graphics.planets.has_value());
    // The independent, Core then Arm auras load in that order.
    const auto first = std::find(store.reads.begin(), store.reads.end(), "anims/planetauras.gaf");
    const auto core =
        std::find(store.reads.begin(), store.reads.end(), "anims/planetaurascore.gaf");
    const auto arm = std::find(store.reads.begin(), store.reads.end(), "anims/planetaurasarm.gaf");
    CHECK(first < core && core < arm && arm != store.reads.end());
}

// While the map loads the viewer shows the current static frame, whose
// 498 x 328 pixels have no transparent ones; GalaxyView.show_map_while_loading keeps it off.
void test_static(GalaxyGraphics& graphics) {
    auto view = std::make_unique<GalaxyView>();
    screens::galaxy_init(view.get());
    for (int32_t frame = 0; frame < graphics.map_static_frames; ++frame) {
        view->static_frame = frame;
        const std::vector<uint8_t> drawn = draw_view(graphics, *view);
        const oa::formats::gaf::RenderResult expected = oa::formats::gaf::render_normal(
            graphics.map_static->frames[static_cast<std::size_t>(frame)]
        );
        CHECK(
            expected.ok() && expected.frame->width == kViewerWidth &&
            expected.frame->height == kViewerHeight && drawn == expected.frame->pixels
        );
        CHECK(std::find(drawn.begin(), drawn.end(), 9) == drawn.end());
    }
    const std::vector<uint8_t> blank(static_cast<std::size_t>(kViewerWidth * kViewerHeight), kFill);
    view->show_map_while_loading = true;
    CHECK(draw_view(graphics, *view) == blank);
    view->show_map_while_loading = false;
    view->loading = false;
    CHECK(draw_view(graphics, *view) == blank); // no static and no map data

    // Frames draw at the position less their origin, key pixels left out.
    const oa::formats::gaf::Frame* ring = graphics.passover_rings[3][0];
    std::vector<uint8_t> expected(static_cast<std::size_t>(kViewerWidth * kViewerHeight), kFill);
    lay_frame(expected, ring, 100, 100);
    Canvas target(kViewerWidth, kViewerHeight);
    galaxy_draw_frame(graphics, target.surface(), ring, 100, 100);
    CHECK(target.buffer.pixels == expected && expected != blank);
}

// The planet's own drawing, alone on the viewer: its aura by current side
// at its activity frame, the capitol marker by initial side, the rings
// when selected or hovered, its sprite, then the label zoomed in or
// selected; all of scale * 3 + size class.
std::vector<uint8_t>
expected_planet(const GalaxyGraphics& graphics, const GalaxyView& view, std::size_t index) {
    std::vector<uint8_t> pixels(static_cast<std::size_t>(kViewerWidth * kViewerHeight), kFill);
    const GalaxyPlanet& p = view.planets[index];
    const auto scale = static_cast<std::size_t>(view.scale);
    const auto size_class = static_cast<std::size_t>(p.size_class);
    const std::size_t variant = scale * 3 + size_class;
    const auto side_of = [](char side) -> int32_t {
        switch (side) {
        case 'A':
        case 'a':
            return static_cast<int32_t>(AuraSide::arm);
        case 'C':
        case 'c':
            return static_cast<int32_t>(AuraSide::core);
        case 'I':
        case 'i':
            return static_cast<int32_t>(AuraSide::independent);
        default:
            return -1;
        }
    };
    const int32_t side = side_of(p.side);
    if (side >= 0)
        lay_frame(
            pixels,
            oa::formats::gaf::frame_at(
                graphics.aura_sequences[static_cast<std::size_t>(side)][variant], p.activity
            ),
            p.view_x,
            p.view_y
        );
    if (p.capitol) {
        const int32_t initial = side_of(p.initial_side);
        if (initial == static_cast<int32_t>(AuraSide::arm))
            lay_frame(pixels, graphics.capitol_arm[variant], p.view_x, p.view_y);
        else if (initial == static_cast<int32_t>(AuraSide::core))
            lay_frame(pixels, graphics.capitol_core[variant], p.view_x, p.view_y);
    }
    const auto self = static_cast<int32_t>(index);
    if (view.selected == self)
        lay_frame(pixels, graphics.selection_rings[scale][size_class], p.view_x, p.view_y);
    if (view.hovered == self)
        lay_frame(pixels, graphics.passover_rings[scale][size_class], p.view_x, p.view_y);
    lay_frame(
        pixels,
        oa::formats::gaf::frame_at(graphics.planet_sequences[index][scale], p.frame),
        p.view_x,
        p.view_y
    );
    if (view.scale != 0 || view.selected == self)
        lay_surface(
            pixels,
            graphics.labels[index].surface,
            p.view_x + 4,
            (p.radius + 6) * (view.scale + 1) / 4 + p.view_y
        );
    return pixels;
}

// A copy of the view showing only planet `index`: no stars, no connectors
// and every other planet out of view.
std::unique_ptr<GalaxyView> only_planet(const GalaxyView& view, std::size_t index) {
    auto alone = std::make_unique<GalaxyView>(view);
    for (screens::GalaxyStarLayer& layer : alone->star_layers)
        layer.star_count = 0;
    alone->connector_count = 0;
    for (std::size_t i = 0; i < alone->planet_count; ++i)
        alone->planets[i].visible = i == index && alone->planets[i].visible;
    return alone;
}

bool same_as_alone(GalaxyGraphics& graphics, const GalaxyView& view, std::size_t index) {
    return draw_view(graphics, view) == expected_planet(graphics, view, index);
}

void test_loaded_map(GalaxyGraphics& graphics, const char* path, Toolkit* toolkit) {
    auto view = std::make_unique<GalaxyView>();
    screens::galaxy_init(view.get());
    const BoneyardsService service = rover_session_service();
    CHECK(galaxy_view_load(view.get(), graphics, service, path));
    CHECK(view->planet_count == kPlanetCount && view->sector_id == 45);

    // Still loading: the static, not the map.
    const oa::formats::gaf::RenderResult noise =
        oa::formats::gaf::render_normal(graphics.map_static->frames[0]);
    CHECK(noise.ok() && draw_view(graphics, *view) == noise.frame->pixels);

    screens::galaxy_apply_allegiances(view.get(), kAllegiances);
    CHECK(!view->loading && view->has_data && view->scale == 0);
    const std::vector<uint8_t> map = draw_view(graphics, *view);
    const auto unchanged = static_cast<std::size_t>(std::count(map.begin(), map.end(), kFill));
    std::size_t as_static = 0;
    for (std::size_t i = 0; i < map.size(); ++i)
        as_static += map[i] == noise.frame->pixels[i] ? 1 : 0;
    CHECK(unchanged > map.size() * 3 / 4); // stars, links and planets on the cleared viewer
    CHECK(as_static < map.size() / 8);
    snapshot(graphics, *view, "galaxy_level0.ppm");

    // Sector 45's star layers: 100-unit cells (5% of 2000), 20 x 14 of
    // them, every spot inside the image; windows at the corner.
    const screens::GalaxyStarLayer& near = view->star_layers[0];
    const screens::GalaxyStarLayer& far = view->star_layers[2];
    CHECK(near.star_count == 280 && far.star_count == 280);
    CHECK(
        near.stars[0].x == 86 && near.stars[0].y == 74 &&
        near.stars[0].kind == screens::StarKind::point
    );
    CHECK(
        far.stars[0].x == 93 && far.stars[0].y == 89 && far.stars[0].kind == screens::StarKind::pair
    );

    // Every visible planet shows its sprite at its anchor, drawn last.
    std::size_t visible = 0;
    std::size_t anchored = 0;
    for (std::size_t i = 0; i < view->planet_count; ++i) {
        const GalaxyPlanet& p = view->planets[i];
        if (!p.visible)
            continue;
        ++visible;
        const oa::formats::gaf::RenderResult sprite = oa::formats::gaf::render_normal(
            *oa::formats::gaf::frame_at(graphics.planet_sequences[i][0], p.frame)
        );
        const oa::formats::gaf::RenderedFrame& image = *sprite.frame;
        const uint8_t anchor =
            image.pixels[static_cast<std::size_t>(image.origin_y * image.width + image.origin_x)];
        if (anchor != image.transparency_index &&
            map[static_cast<std::size_t>(p.view_y * kViewerWidth + p.view_x)] == anchor)
            ++anchored;
    }
    CHECK(visible == kPlanetCount && anchored == visible);

    // Each visible connector crosses its midpoint (or a pixel beside it on
    // the shorter axis) in its colour.
    std::size_t links = 0;
    std::size_t crossed = 0;
    for (std::size_t i = 0; i < view->connector_count; ++i) {
        const screens::GalaxyConnector& c = view->connectors[i];
        if (!c.visible)
            continue;
        ++links;
        const int32_t mx = (c.view_ax + c.view_bx) / 2;
        const int32_t my = (c.view_ay + c.view_by) / 2;
        const bool steep = std::abs(c.view_by - c.view_ay) >= std::abs(c.view_bx - c.view_ax);
        bool found = false;
        for (int32_t d = -1; d <= 1 && !found; ++d) {
            const int32_t x = steep ? mx + d : mx;
            const int32_t y = steep ? my : my + d;
            found = map[static_cast<std::size_t>(y * kViewerWidth + x)] ==
                    static_cast<uint8_t>(c.color);
        }
        crossed += found ? 1 : 0;
    }
    CHECK(links == view->connector_count && crossed == links);

    // Planet by planet: the aura follows the current side and the capitol
    // marker the initial one; rings on select and hover; labels only
    // zoomed in or selected.
    std::size_t capitols = 0;
    for (std::size_t i = 0; i < view->planet_count; ++i) {
        auto alone = only_planet(*view, i);
        CHECK(same_as_alone(graphics, *alone, i));
        capitols += alone->planets[i].capitol ? 1 : 0;
    }
    CHECK(capitols > 0);
    std::size_t index = 0;
    while (index < view->planet_count && !view->planets[index].capitol)
        ++index;
    auto alone = only_planet(*view, index);
    GalaxyPlanet& planet = alone->planets[index];
    const std::vector<uint8_t> as_loaded = draw_view(graphics, *alone);
    for (const char side : {'A', 'c', 'I', 'x'}) {
        planet.side = side;
        CHECK(same_as_alone(graphics, *alone, index));
    }
    planet.side = 'A';
    const std::vector<uint8_t> arm = draw_view(graphics, *alone);
    planet.side = 'C';
    CHECK(draw_view(graphics, *alone) != arm);
    planet.capitol = false;
    const std::vector<uint8_t> plain = draw_view(graphics, *alone);
    CHECK(plain != as_loaded && same_as_alone(graphics, *alone, index));
    planet.capitol = true;
    for (const char initial : {'A', 'C', 'I'}) {
        planet.initial_side = initial;
        CHECK(same_as_alone(graphics, *alone, index));
    }
    CHECK(draw_view(graphics, *alone) == plain); // an independent capitol has no marker
    planet.activity = 2;
    CHECK(same_as_alone(graphics, *alone, index));
    planet.activity = 9; // past the aura's four frames
    CHECK(same_as_alone(graphics, *alone, index));
    planet.activity = 0;

    alone->selected = static_cast<int32_t>(index);
    const std::vector<uint8_t> selected = draw_view(graphics, *alone);
    CHECK(selected != plain && same_as_alone(graphics, *alone, index));
    alone->selected = screens::kNoPlanet;
    alone->hovered = static_cast<int32_t>(index);
    CHECK(draw_view(graphics, *alone) != plain && same_as_alone(graphics, *alone, index));
    alone->hovered = screens::kNoPlanet;

    // Labels: the planet's name in the tinted font, three rows taller than
    // the font, drawn only zoomed in or selected.
    const Surface& label = graphics.labels[index].surface;
    int32_t width = 0;
    int32_t height = 0;
    toolkit_measure_text(toolkit, graphics.system_fonts->small_light, planet.name, &width, &height);
    CHECK(label.width == width && label.height == height + 3 && width > 0);
    bool tinted = true;
    for (int32_t y = 0; y < label.height; ++y)
        for (int32_t x = 0; x < label.width; ++x)
            tinted = tinted && (label.pixels[y * label.pitch + x] == 9 ||
                                label.pixels[y * label.pitch + x] == kLabelTint);
    CHECK(tinted);
    const auto label_ink = [&](const GalaxyView& shown) {
        const std::vector<uint8_t> drawn = draw_view(graphics, shown);
        const GalaxyPlanet& p = shown.planets[index];
        const int32_t left = p.view_x + 4;
        const int32_t top = (p.radius + 6) * (shown.scale + 1) / 4 + p.view_y;
        std::size_t ink = 0;
        for (int32_t y = 0; y < label.height; ++y)
            for (int32_t x = 0; x < label.width; ++x)
                if (label.pixels[y * label.pitch + x] == kLabelTint && left + x < kViewerWidth &&
                    top + y < kViewerHeight &&
                    drawn[static_cast<std::size_t>((top + y) * kViewerWidth + left + x)] ==
                        kLabelTint)
                    ++ink;
        return ink;
    };
    CHECK(label_ink(*alone) == 0);
    alone->selected = static_cast<int32_t>(index);
    CHECK(label_ink(*alone) > 20);
    auto zoomed = std::make_unique<GalaxyView>(*view);
    screens::galaxy_set_scale(zoomed.get(), 1);
    CHECK(zoomed->scale == 1 && zoomed->planets[index].visible);
    auto zoomed_alone = only_planet(*zoomed, index);
    CHECK(same_as_alone(graphics, *zoomed_alone, index) && label_ink(*zoomed_alone) > 20);
    zoomed->selected = static_cast<int32_t>(index);
    zoomed->hovered = static_cast<int32_t>(index == 0 ? 1 : 0);
    snapshot(graphics, *zoomed, "galaxy_level1_selected.ppm");

    // The star layers under zoom and scroll, as galaxy_scale_star_layer places them.
    CHECK(near.stars[0].view_x == 21 && near.stars[0].view_y == 17 && near.stars[0].visible);
    CHECK(far.stars[0].view_x == 23 && far.stars[0].view_y == 20);
    screens::galaxy_set_scale(view.get(), 2);
    CHECK(view->view_width == 664 && view->view_height == 437 && view->scroll_x == 0);
    CHECK(near.stars[0].view_x == 29 && near.stars[0].view_y == 23 && near.stars[0].visible);
    screens::galaxy_scroll(view.get(), 400, 300, true);
    CHECK(near.stars[0].view_x == -24 && near.stars[0].view_y == -14 && !near.stars[0].visible);
    CHECK(far.stars[0].view_x == 23 && far.stars[0].view_y == 20 && far.stars[0].visible);

    // Only the star layers: every visible point of the farthest layer
    // (drawn last) shows its core shade.
    auto stars = std::make_unique<GalaxyView>(*view);
    stars->planet_count = 0;
    stars->connector_count = 0;
    const std::vector<uint8_t> sky = draw_view(graphics, *stars);
    std::size_t points = 0;
    std::size_t shown = 0;
    for (int32_t i = 0; i < far.star_count; ++i) {
        const screens::GalaxyStar& s = far.stars[i];
        if (!s.visible || s.kind != screens::StarKind::point || s.view_x < 0 || s.view_y < 0 ||
            s.view_x >= kViewerWidth || s.view_y >= kViewerHeight)
            continue;
        ++points;
        const int32_t dim = std::min(s.dim + far.dim, 14);
        shown +=
            sky[static_cast<std::size_t>(s.view_y * kViewerWidth + s.view_x)] == 81 + dim ? 1 : 0;
    }
    CHECK(points > 100 && shown == points);
}

// The frame's pixel at (col, row).
uint8_t pixel_of(const oa::formats::gaf::RenderedFrame& image, int32_t col, int32_t row) {
    return image.pixels[static_cast<std::size_t>(row * image.width + col)];
}

// Lays rendered `image` with its top-left corner at (x, y) into `canvas`
// inside the inclusive `clip`, key pixels left out.
void lay_rendered(
    Canvas& canvas,
    const oa::formats::gaf::RenderedFrame& image,
    int32_t x,
    int32_t y,
    const Rect32& clip
) {
    for (int32_t row = 0; row < image.height; ++row)
        for (int32_t col = 0; col < image.width; ++col) {
            const uint8_t source = pixel_of(image, col, row);
            if (source != image.transparency_index && inside(clip, x + col, y + row))
                put(canvas, x + col, y + row, source);
        }
}

// The pan strips of the war map's planets, through the installed
// archives in the game's mount order and through by.ccx alone.
void test_install_panoramas(
    GalaxyGraphics& graphics, const FileReader& by_only, const fs::path& root, const char* path
) {
    auto view = std::make_unique<GalaxyView>();
    screens::galaxy_init(view.get());
    CHECK(galaxy_view_load(view.get(), graphics, rover_session_service(), path));
    Store full{oa::AssetStore{root}, {}};
    (void)full.assets.discover("31");
    const FileReader files{&full, read_asset};
    const auto find = [&](const char* name) {
        for (std::size_t i = 0; i < view->planet_count; ++i)
            if (std::strcmp(view->planets[i].name, name) == 0)
                return i;
        return view->planet_count;
    };
    const std::size_t alpha = find("DP Alpha");
    const std::size_t slag = find("Slag");
    CHECK(alpha < view->planet_count && slag < view->planet_count);
    if (alpha >= view->planet_count || slag >= view->planet_count)
        return;

    // Every planet's pan sequence loads from the installed archives.
    std::size_t loaded = 0;
    for (std::size_t i = 0; i < view->planet_count; ++i)
        loaded += galaxy_load_panorama(graphics, i, view->planets[i], files) ? 1 : 0;
    CHECK(loaded == view->planet_count);

    // DP Alpha's MetalPan (Metalbrief.gaf, totala1.hpi): three 640-pixel
    // frames and a 520-pixel one, 122 rows each.
    const oa::formats::gaf::Sequence* metal = graphics.pan_sequences[alpha];
    const PanoramaStrip& strip = graphics.pan_strips[alpha];
    CHECK(metal != nullptr && metal->name == "MetalPan");
    CHECK(
        strip.frame_count == 4 && strip.offsets[1] == 640 && strip.offsets[2] == 1280 &&
        strip.offsets[3] == 1920 && strip.total == 2440
    );
    std::vector<oa::formats::gaf::RenderedFrame> frames;
    for (const oa::formats::gaf::Frame& frame : metal->frames)
        frames.push_back(*oa::formats::gaf::render_normal(frame).frame);
    CHECK(
        frames.size() == 4 && frames[0].height == 122 && frames[3].width == 520 &&
        frames[0].origin_x == 320 && frames[0].origin_y == 222
    );

    const Rect32 clip = k_recon_scan_clip;
    const auto draw = [&](std::size_t index, int32_t scroll, int32_t end) {
        Canvas canvas(kViewerWidth, kViewerHeight);
        galaxy_draw_panorama(graphics, index, canvas.surface(), scroll, end, clip);
        return canvas;
    };
    const auto expected = [&](std::initializer_list<std::pair<std::size_t, int32_t>> placed) {
        Canvas canvas(kViewerWidth, kViewerHeight);
        for (const auto& [frame, x] : placed)
            lay_rendered(canvas, frames[frame], x, 0x10, clip);
        return canvas.buffer.pixels;
    };
    // The Recon open's window, 0 to 0x1D6: frame 0 alone.
    CHECK(draw(alpha, 0, 0x1D6).buffer.pixels == expected({{0, 0xD}}));
    const auto snapshot_scan =
        [&](std::size_t index, int32_t scroll, int32_t end, const char* name) {
            snapshot_image(name, [&](Surface* image) {
                galaxy_draw_panorama(graphics, index, image, scroll, end, clip);
            });
        };
    snapshot_scan(alpha, 0, 0x1D6, "recon_scan_metal_open.ppm");
    // Frames 0 and 1 across the window.
    CHECK(
        draw(alpha, 400, 400 + 0x1D6).buffer.pixels == expected({{0, 0xD - 400}, {1, 0xD + 240}})
    );
    // One pixel before the wrap: the window ends at 469 of frame 0, which
    // follows frame 3; frame 3's last column at 0xD, frame 0's first at 0xE.
    const Canvas wrapped = draw(alpha, 2439, (2439 + 0x1D6) % 2440);
    CHECK(wrapped.buffer.pixels == expected({{3, 1920 - 2439 + 0xD}, {0, 0xE}}));
    std::size_t last_column = 0;
    std::size_t first_column = 0;
    std::size_t outside = 0;
    for (int32_t y = 0; y < kViewerHeight; ++y) {
        const bool scan_row = y >= 0x10 && y <= 0x89;
        if (scan_row) {
            last_column += wrapped.at(0xD, y) == pixel_of(frames[3], 519, y - 0x10) ? 1 : 0;
            first_column += wrapped.at(0xE, y) == pixel_of(frames[0], 0, y - 0x10) ? 1 : 0;
        }
        outside += wrapped.at(0xC, y) != kFill || wrapped.at(0x1E4, y) != kFill ? 1 : 0;
    }
    for (int32_t x = 0; x < kViewerWidth; ++x)
        outside += wrapped.at(x, 0xF) != kFill || wrapped.at(x, 0x8A) != kFill ? 1 : 0;
    CHECK(last_column == 122 && first_column == 122 && outside == 0);
    CHECK(wrapped.at(0x1E3, 0x89) == pixel_of(frames[0], 0x1E3 - 0xE, 0x79));
    snapshot_scan(alpha, 2439, (2439 + 0x1D6) % 2440, "recon_scan_metal_wrap.ppm");

    // With by.ccx alone, Slag's AcidPan resolves from BYPans.gaf there while
    // DP Alpha's Metalbrief.gaf does not, leaving it the Map_Static.
    Store* by_store = static_cast<Store*>(by_only.user);
    galaxy_release_panoramas(graphics);
    CHECK(galaxy_load_panorama(graphics, slag, view->planets[slag], by_only));
    CHECK(
        std::find(by_store->reads.begin(), by_store->reads.end(), "anims/bypans.gaf") !=
        by_store->reads.end()
    );
    const oa::formats::gaf::Sequence* acid = graphics.pan_sequences[slag];
    CHECK(acid != nullptr && acid->name == "AcidPan" && graphics.pan_strips[slag].total == 2440);
    snapshot_scan(slag, 1000, 1000 + 0x1D6, "recon_scan_acid.ppm");
    CHECK(!galaxy_load_panorama(graphics, alpha, view->planets[alpha], by_only));
    Canvas fallback(kViewerWidth, kViewerHeight);
    galaxy_draw_panorama(graphics, alpha, fallback.surface(), 0, 0x1D6, clip);
    CHECK(fallback.lit().empty());
    galaxy_draw_map_static(graphics, 2, fallback.surface(), clip);
    Canvas noise(kViewerWidth, kViewerHeight);
    lay_rendered(
        noise, *oa::formats::gaf::render_normal(graphics.map_static->frames[2]).frame, 0, 0, clip
    );
    CHECK(
        fallback.buffer.pixels == noise.buffer.pixels &&
        fallback.lit().size() == static_cast<std::size_t>(0x1D7 * 0x7A)
    );
    snapshot_scan(alpha, 0, 0x1D6, "recon_scan_metal_before.ppm"); // no panorama: nothing
    snapshot_image("recon_scan_static.ppm", [&](Surface* image) {
        galaxy_draw_map_static(graphics, 2, image, clip);
    });
    galaxy_release_panoramas(graphics);
}

} // namespace

int main(int argc, char** argv) {
    if (!oa::test::game_data_requested(argc, argv)) {
        test_missing();
        test_lines();
        test_stars();
        test_synthetic_panorama();
        if (g_failures != 0) {
            std::fprintf(stderr, "%d failure(s)\n", g_failures);
            return 1;
        }
        std::printf("galaxy graphics tests passed\n");
        return 0;
    }

    struct Install {
        fs::path root;
        const char* map;
    };

    const Install installs[] = {
        {oa::boneyards_test::launcher_install(), "Boneyards\\alpha_map.gwm"},
        {oa::boneyards_test::by_install(), "Boneyards\\BYPacific_map.gwm"},
    };
    const Install* install = nullptr;
    for (const Install& candidate : installs) {
        std::string relative = candidate.map;
        relative[relative.find('\\')] = '/';
        if (!candidate.root.empty() && fs::exists(candidate.root / relative) &&
            fs::exists(candidate.root / "by.ccx")) {
            install = &candidate;
            break;
        }
    }
    if (install == nullptr)
        oa::test::skip_test(
            "the installed galaxy graphics tests",
            "neither OA_LAUNCHER_GAME_DIR nor OA_GAME_DIR names an install with a Boneyards war map"
        );
    g_install_root = install->root;
    rover_session_configure(install->root.string().c_str(), "");
    if (!rover_module_load()) {
        std::fprintf(stderr, "FAIL: the Rover table was not bound\n");
        return 1;
    }
    Store store{oa::AssetStore{install->root}, {}};
    store.assets.mount(install->root / "by.ccx");
    const FileReader files{&store, read_asset};
    auto graphics = std::make_unique<GalaxyGraphics>();
    CHECK(galaxy_load_graphics(*graphics, files));
    test_installed(store, *graphics);
    test_static(*graphics);

    const Palette palette{};
    ToolkitConfig config{};
    config.width = kViewerWidth;
    config.height = kViewerHeight;
    config.palette = &palette;
    config.files = ToolkitFiles{&store, read_for_toolkit};
    Toolkit* toolkit = toolkit_create(&config);
    screens::HatFonts fonts{};
    fonts.small = toolkit_default_font(toolkit);
    fonts.small_light = toolkit_font_tint(toolkit, fonts.small, kLabelTint);
    CHECK(fonts.small_light != k_no_handle);
    graphics->label_toolkit = toolkit;
    graphics->system_fonts = &fonts;
    test_loaded_map(*graphics, install->map, toolkit);
    toolkit_destroy(toolkit);
    graphics->label_toolkit = nullptr;
    if (install == &installs[0])
        test_install_panoramas(*graphics, files, install->root, install->map);

    rover_module_unload();
    rover_use_platform(nullptr);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("galaxy graphics tests passed (%s)\n", install->map);
    return 0;
}
