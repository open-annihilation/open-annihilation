// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The overlay of the turned view, the canvas and the operations on the
// picture under it, adds to a picture exactly what the painters paint on it:
// blend_rect's blends and lay_text's shadow, outline, letters and reduction
// to the palette, in any order and overlapping. Each tile lists every
// operation that reaches it, in order.
#include "renderer_live_overlay.hpp"
#include "oa/present/game_text.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"

#include <algorithm>
#include <random>
#include <vector>

namespace {
using namespace oa::app;
namespace overlay = renderer_live_overlay;
namespace present = oa::present;
constexpr int32_t width = 61;
constexpr int32_t height = 37;

/// A palette of 256 random colours.
std::vector<uint8_t> random_palette(std::mt19937& random) {
    std::vector<uint8_t> palette(256 * overlay::palette_entry_bytes);
    for (auto& byte : palette)
        byte = static_cast<uint8_t>(random());
    return palette;
}

/// The lowest colour no entry of the palette holds, as the canvas's key is.
std::array<uint8_t, 3> key_outside(const std::vector<uint8_t>& palette) {
    for (uint32_t colour = 0;; ++colour) {
        const std::array<uint8_t, 3> key{
            static_cast<uint8_t>(colour >> 16),
            static_cast<uint8_t>(colour >> 8),
            static_cast<uint8_t>(colour)
        };
        bool held = false;
        for (std::size_t at = 0; at + 2 < palette.size(); at += overlay::palette_entry_bytes)
            held = held || std::equal(key.begin(), key.end(), palette.begin() + std::ptrdiff_t(at));
        if (!held)
            return key;
    }
}

/// A line of text: a random mask, with or without its outline and shadow.
present::TextLayers random_line(std::mt19937& random) {
    present::TextMask mask;
    mask.width = 3 + static_cast<int32_t>(random() % 14);
    mask.height = 3 + static_cast<int32_t>(random() % 9);
    mask.baseline = mask.height - 1;
    mask.advance = mask.width + 1;
    mask.alpha.resize(static_cast<std::size_t>(mask.width * mask.height));
    for (auto& alpha : mask.alpha) {
        const auto pick = random() % 4;
        alpha = pick == 0 ? 0 : pick == 1 ? 255 : static_cast<uint8_t>(random());
    }
    mask.character_ends = {mask.advance};
    const present::TextStyle style{
        true, random() % 4 != 0, random() % 4 != 0, false, present::default_text_size
    };
    return present::build_text_layers(mask, style, 1 + static_cast<int32_t>(random() % 2));
}

/// Paints the same random strokes on a picture and on the overlay: lines
/// of text, blends and opaque rectangles in palette colours, at random
/// places, the overlay taking the operations its key leaves to the picture.
/// Composing it over the picture as it was must give the picture painted.
void paints_as_the_painters() {
    std::mt19937 random(20261031);
    // Rounds compared and operations they composed, so that the check is
    // known to have held text and blends to the painters.
    int compared = 0;
    std::size_t composed_operations = 0;
    for (int round = 0; round < 60; ++round) {
        const auto palette = random_palette(random);
        const auto key = key_outside(palette);
        std::vector<uint8_t> picture(static_cast<std::size_t>(width * height) * 3);
        for (auto& byte : picture)
            byte = static_cast<uint8_t>(random());
        const auto drawn = picture;
        oa::ui::frontend_renderer::Surface painted{width, height, picture};
        std::vector<uint8_t> canvas(picture.size());
        for (std::size_t at = 0; at < canvas.size(); at += 3)
            std::copy(key.begin(), key.end(), canvas.begin() + std::ptrdiff_t(at));
        std::vector<overlay::Operation> operations;
        uint32_t line = 0;
        // A blend of the canvas's own pixel that makes the key would read as
        // the picture's: the runtime reads the picture back then.
        bool keyed_by_blend = false;
        const int strokes = 1 + static_cast<int>(random() % 12);
        for (int stroke = 0; stroke < strokes; ++stroke) {
            const int32_t x = static_cast<int32_t>(random() % (width + 8)) - 4;
            const int32_t y = static_cast<int32_t>(random() % (height + 8)) - 4;
            const auto kind = random() % 3;
            if (kind == 0) {
                const auto layers = random_line(random);
                const std::array<uint8_t, 3> colour{
                    static_cast<uint8_t>(random()),
                    static_cast<uint8_t>(random()),
                    static_cast<uint8_t>(random())
                };
                auto on_picture = present::rgb_canvas(painted.rgb, width, height, palette);
                present::lay_text(on_picture, layers, x, y, colour);
                ++line;

                struct Recording {
                    std::vector<overlay::Operation>* operations;
                    uint32_t line;
                } recording{&operations, line};

                auto on_canvas = present::rgb_canvas(canvas, width, height, palette);
                on_canvas.see_through = key;
                on_canvas.world_user = &recording;
                on_canvas.world_run = [](void* user, const present::TextWorldRun& run) {
                    auto& held = *static_cast<Recording*>(user);
                    const auto kind =
                        run.layer == present::TextWorldLayer::shadow    ? overlay::Kind::shadow
                        : run.layer == present::TextWorldLayer::outline ? overlay::Kind::outline
                                                                        : overlay::Kind::letter;
                    held.operations->push_back(
                        {run.x, run.y, run.width, 1, kind, run.color, run.coverage, held.line}
                    );
                };
                present::lay_text(on_canvas, layers, x, y, colour);
                continue;
            }
            const int32_t w = 1 + static_cast<int32_t>(random() % 30);
            const int32_t h = 1 + static_cast<int32_t>(random() % 20);
            const auto entry = (random() % 256) * overlay::palette_entry_bytes;
            const std::array<uint8_t, 3> colour{
                palette[entry], palette[entry + 1], palette[entry + 2]
            };
            if (kind == 1) {
                const uint32_t opacity = random() % 300;
                oa::ui::frontend_renderer::blend_rect(painted, x, y, w, h, colour, opacity);
                const overlay::Operation blend{x, y, w, h, overlay::Kind::blend, colour, opacity};
                // The canvas's own pixels are blended where they are; the
                // key's are left to the picture.
                for (int32_t row = std::max(y, 0); row < std::min(y + h, height); ++row)
                    for (int32_t column = std::max(x, 0); column < std::min(x + w, width);
                         ++column) {
                        uint8_t* at = canvas.data() + std::size_t(row * width + column) * 3;
                        if (std::equal(key.begin(), key.end(), at))
                            continue;
                        const auto made = overlay::apply(blend, {at[0], at[1], at[2]});
                        std::copy(made.begin(), made.end(), at);
                        keyed_by_blend = keyed_by_blend || made == key;
                    }
                operations.push_back(blend);
                continue;
            }
            for (int32_t row = std::max(y, 0); row < std::min(y + h, height); ++row)
                for (int32_t column = std::max(x, 0); column < std::min(x + w, width); ++column) {
                    const std::size_t at = std::size_t(row * width + column) * 3;
                    std::copy(
                        colour.begin(), colour.end(), painted.rgb.begin() + std::ptrdiff_t(at)
                    );
                    std::copy(colour.begin(), colour.end(), canvas.begin() + std::ptrdiff_t(at));
                }
        }
        if (keyed_by_blend)
            continue;
        auto composed = drawn;
        overlay::compose(composed, width, height, canvas, key, operations, palette);
        OA_CHECK(composed == painted.rgb);
        ++compared;
        composed_operations += operations.size();
    }
    OA_CHECK(compared >= 50 && composed_operations >= 1000);
}

/// Without a palette the letters keep their blended colours, as on a canvas
/// with none.
void no_palette_keeps_colours() {
    const overlay::Operation letter{0, 0, 1, 1, overlay::Kind::letter, {200, 100, 0}, 128, 1};
    std::vector<uint8_t> picture{10, 20, 30};
    const std::vector<uint8_t> canvas{1, 2, 3};
    overlay::compose(picture, 1, 1, canvas, {1, 2, 3}, {&letter, 1}, {});
    const auto expected = present::blend_color({10, 20, 30}, {200, 100, 0}, 128);
    OA_CHECK(std::equal(expected.begin(), expected.end(), picture.begin()));
}

/// Each tile lists, in order, every operation reaching it, cut to the
/// picture; an operation outside it is left out.
void tiles_list_operations() {
    std::mt19937 random(7);
    std::vector<overlay::Operation> operations;
    for (int index = 0; index < 400; ++index)
        operations.push_back(
            {static_cast<int32_t>(random() % 200) - 40,
             static_cast<int32_t>(random() % 140) - 30,
             static_cast<int32_t>(random() % 50),
             static_cast<int32_t>(random() % 30),
             overlay::Kind::blend,
             {},
             static_cast<uint32_t>(index),
             0}
        );
    constexpr uint32_t picture_width = 150;
    constexpr uint32_t picture_height = 90;
    overlay::Packed packed;
    overlay::pack(operations, picture_width, picture_height, packed);
    OA_CHECK(packed.tiles_across == (picture_width + overlay::tile_edge - 1) / overlay::tile_edge);
    OA_CHECK(packed.tiles_down == (picture_height + overlay::tile_edge - 1) / overlay::tile_edge);
    // The operations kept, in order: those with a pixel in the picture.
    std::vector<uint32_t> kept;
    for (uint32_t index = 0; index < operations.size(); ++index) {
        const auto& operation = operations[index];
        if (operation.width > 0 && operation.height > 0 && operation.x + operation.width > 0 &&
            operation.y + operation.height > 0 && operation.x < int32_t(picture_width) &&
            operation.y < int32_t(picture_height))
            kept.push_back(index);
    }
    OA_CHECK(packed.count == kept.size());
    OA_CHECK(packed.operations.size() == kept.size() * overlay::texels_per_operation * 4);
    const auto word = [&](uint32_t operation, uint32_t at) {
        return packed.operations[std::size_t(operation) * overlay::texels_per_operation * 4 + at];
    };
    for (uint32_t at = 0; at < packed.count; ++at) {
        const auto& operation = operations[kept[at]];
        OA_CHECK(word(at, 0) == uint32_t(std::max(operation.x, 0)));
        OA_CHECK(word(at, 1) == uint32_t(std::max(operation.y, 0)));
        OA_CHECK(
            word(at, 2) ==
            uint32_t(std::min<int32_t>(operation.x + operation.width, picture_width) - 1)
        );
        OA_CHECK(
            word(at, 3) ==
            uint32_t(std::min<int32_t>(operation.y + operation.height, picture_height) - 1)
        );
        OA_CHECK(word(at, 6) == operation.amount);
    }
    const uint32_t tiles = packed.tiles_across * packed.tiles_down;
    for (uint32_t tile = 0; tile < tiles; ++tile) {
        const uint32_t left = tile % packed.tiles_across * overlay::tile_edge;
        const uint32_t top = tile / packed.tiles_across * overlay::tile_edge;
        std::vector<uint32_t> expected;
        for (uint32_t at = 0; at < packed.count; ++at)
            if (word(at, 0) < left + overlay::tile_edge && word(at, 2) >= left &&
                word(at, 1) < top + overlay::tile_edge && word(at, 3) >= top)
                expected.push_back(at);
        const std::vector<uint32_t> listed(
            packed.lists.begin() + packed.lists[tile], packed.lists.begin() + packed.lists[tile + 1]
        );
        OA_CHECK(listed == expected);
    }
    OA_CHECK(packed.lists[tiles] == packed.lists.size());
    // A picture out of range lays out nothing.
    overlay::pack(operations, 0, picture_height, packed);
    OA_CHECK(packed.count == 0 && packed.lists.empty() && packed.tiles_across == 0);
}

} // namespace

int main() {
    paints_as_the_painters();
    no_palette_keeps_colours();
    tiles_list_operations();
    return oa::test::check_exit_status();
}
