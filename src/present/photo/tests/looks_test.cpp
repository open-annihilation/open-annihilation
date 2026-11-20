// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A fixed 64×36 picture through each preset, the vignette, the grain and
// each frame. The digest is pinned. One, two and eight threads, and no
// pool, write the same bytes. None and an amount of 0 leave the picture
// unchanged. The same seed writes the same grain. A picture drawn two to
// four times larger, with the grain size scaled by that factor, box-reduces
// to the picture with the look.
#include "oa/present/photo/looks.hpp"
#include "oa/present/photo/photo_sizes.hpp"

#include "oa/base/sha256.hpp"
#include "oa/platform/job_pool.hpp"
#include "oa/platform/text_font.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using oa::present::photo::Frame;
using oa::present::photo::Look;
using oa::present::photo::Picture;
using oa::present::photo::Preset;
using oa::present::photo::apply_look;
using oa::present::photo::look_idle;
using oa::present::photo::photo_grain_size;

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 36;

/// The picture every case starts from.
///
/// @return packed RGB
std::vector<uint8_t> source_picture() {
    std::vector<uint8_t> rgb(static_cast<std::size_t>(kWidth * kHeight * 3));
    for (int32_t row = 0; row < kHeight; ++row) {
        for (int32_t column = 0; column < kWidth; ++column) {
            uint8_t* pixel =
                rgb.data() +
                (static_cast<std::size_t>(row) * kWidth + static_cast<std::size_t>(column)) * 3U;
            pixel[0] = static_cast<uint8_t>((column * 3 + row * 5) & 255);
            pixel[1] = static_cast<uint8_t>((column * 7 + row) & 255);
            pixel[2] = static_cast<uint8_t>((column + row * 11) & 255);
        }
    }
    return rgb;
}

/// The digest of packed bytes, as lower-case hex.
///
/// @param bytes the bytes
/// @return 64 hex digits
std::string digest_of(const std::vector<uint8_t>& bytes) {
    const auto hex = oa::base::sha256::to_hex(oa::base::sha256::digest_of(bytes));
    return std::string(hex.data(), hex.size());
}

/// Applies a look on a pool and returns the picture.
///
/// @param look the look
/// @param pool the pool; null runs on the caller
/// @param fonts the caption faces; null draws a bar with no letters
/// @return packed RGB
std::vector<uint8_t> painted(
    const Look& look, oa::platform::job_pool::Pool* pool, oa::platform::text_font::FontStack* fonts
) {
    std::vector<uint8_t> rgb = source_picture();
    Picture picture;
    picture.bytes = rgb.data();
    picture.width = kWidth;
    picture.height = kHeight;
    picture.channels = 3;
    apply_look(picture, look, fonts, pool);
    return rgb;
}

/// The picture every case starts from, each pixel a factor-by-factor block,
/// as Off and Basic draw a larger photo.
///
/// @param factor the block's side
/// @return packed RGB, factor times the picture's width and height
std::vector<uint8_t> blocks_of(int32_t factor) {
    const std::vector<uint8_t> source = source_picture();
    const int32_t wide = kWidth * factor;
    std::vector<uint8_t> rgb(static_cast<std::size_t>(wide) * kHeight * factor * 3U);
    for (int32_t row = 0; row < kHeight * factor; ++row) {
        for (int32_t column = 0; column < wide; ++column) {
            const std::size_t from = (static_cast<std::size_t>(row / factor) * kWidth +
                                      static_cast<std::size_t>(column / factor)) *
                                     3U;
            const std::size_t to =
                (static_cast<std::size_t>(row) * wide + static_cast<std::size_t>(column)) * 3U;
            std::memcpy(rgb.data() + to, source.data() + from, 3U);
        }
    }
    return rgb;
}

/// Paints a look on a picture factor times the source, and box-reduces it.
///
/// @param look the look
/// @param factor the picture's size factor
/// @return the reduced picture, the source's size; empty when it did not reduce
std::vector<uint8_t> reduced_look(const Look& look, int32_t factor) {
    std::vector<uint8_t> rgb = blocks_of(factor);
    Picture picture;
    picture.bytes = rgb.data();
    picture.width = kWidth * factor;
    picture.height = kHeight * factor;
    picture.channels = 3;
    apply_look(picture, look, nullptr, nullptr);
    std::vector<uint8_t> reduced;
    int32_t width = 0;
    int32_t height = 0;
    if (!oa::present::photo::box_reduce(
            rgb.data(), rgb.size(), picture.width, picture.height, factor, reduced, width, height
        ) ||
        width != kWidth || height != kHeight)
        return {};
    return reduced;
}

/// Requires a pinned digest, and prints the one it measured when it differs.
///
/// @param name the case
/// @param got the measured digest
/// @param expect the pinned digest
void require_pin(const char* name, const std::string& got, const char* expect) {
    if (got != expect) {
        std::fprintf(stderr, "looks: %s GOT %s\n", name, got.c_str());
        ++failures;
    }
}

} // namespace

int main() {
    const std::vector<uint8_t> source = source_picture();
    Look idle;
    CHECK(look_idle(idle));
    CHECK(painted(idle, nullptr, nullptr) == source);

    Look round_only;
    round_only.roundness = 100;
    CHECK(look_idle(round_only));
    CHECK(painted(round_only, nullptr, nullptr) == source);

    Look size_only;
    size_only.grain_size = 16;
    CHECK(look_idle(size_only));
    CHECK(painted(size_only, nullptr, nullptr) == source);

    const Preset presets[] = {
        Preset::y1997, Preset::dusk, Preset::arctic, Preset::noir, Preset::bleach
    };
    const char* preset_names[] = {"1997", "dusk", "arctic", "noir", "bleach"};
    const char* preset_pins[] = {
        "cbc6974f0dbe8402026e4ff03bb4b54f9b58a1f197acc3d097401fe70bb1c08d",
        "118c42d87a6431e0bce516fc2886462dc5f380b259009672f1c80de9f94e1d87",
        "69d66a48014000955ae2b55b26f50ee88f3951588208643b12f8d5dbed5348c6",
        "985093ddc19c198e6712fce1c6489f489f0ef82759f9f1446b028e58c4ca0ad9",
        "697fb8bed00ce99d34ee886b6aa3212b437f053d27d002e65eeb25c9f3dfde64",
    };
    for (int which = 0; which < 5; ++which) {
        Look look;
        look.preset = presets[which];
        CHECK(!look_idle(look));
        const std::vector<uint8_t> graded = painted(look, nullptr, nullptr);
        CHECK(graded != source);
        require_pin(preset_names[which], digest_of(graded), preset_pins[which]);
    }

    Look vignette;
    vignette.vignette = 40;
    vignette.roundness = 50;
    const std::vector<uint8_t> vignetted = painted(vignette, nullptr, nullptr);
    CHECK(vignetted != source);
    require_pin(
        "vignette",
        digest_of(vignetted),
        "72ab9b2e1028319ca214c41a3c49bfa2702846a14551c180130f940f8305ab71"
    );

    Look square = vignette;
    square.roundness = 0;
    Look circle = vignette;
    circle.roundness = 100;
    CHECK(painted(square, nullptr, nullptr) != painted(circle, nullptr, nullptr));

    Look grain;
    grain.grain = 30;
    grain.grain_size = 3;
    grain.seed = 30;
    const std::vector<uint8_t> grained = painted(grain, nullptr, nullptr);
    CHECK(grained != source);
    require_pin(
        "grain",
        digest_of(grained),
        "e895333bfbf27ac639cdcc15f66c140501899eda86e40728869c5ebc217bf234"
    );
    CHECK(painted(grain, nullptr, nullptr) == grained);
    Look other_seed = grain;
    other_seed.seed = 31;
    CHECK(painted(other_seed, nullptr, nullptr) != grained);

    Look thin;
    thin.frame = Frame::thin;
    require_pin(
        "thin",
        digest_of(painted(thin, nullptr, nullptr)),
        "bf1a98464c7be3a1aed700d5b3960f4f111cc5351ca4e7169431190ab678bf53"
    );
    Look mat;
    mat.frame = Frame::mat;
    require_pin(
        "mat",
        digest_of(painted(mat, nullptr, nullptr)),
        "1ddc4ee9fa93980c2614a72d9cf779229fd979d8211503b833ab0317002a981a"
    );

    auto fonts =
        oa::platform::text_font::FontStack::open(oa::platform::text_font::bundled_font_directory());
    CHECK(fonts != nullptr);
    Look caption;
    caption.frame = Frame::caption;
    caption.caption = "Ridge · 00:00:01 · 2026-11-08";
    const std::vector<uint8_t> barred = painted(caption, nullptr, fonts.get());
    require_pin(
        "caption",
        digest_of(barred),
        "0c6555145a3d9e24b99f1cf622f50ff613f3a271b3c8d232fae611601427221b"
    );
    Look bare_bar = caption;
    bare_bar.caption.clear();
    const std::vector<uint8_t> no_letters = painted(bare_bar, nullptr, fonts.get());
    CHECK(no_letters != source);
    CHECK(no_letters != barred);

    Look combined;
    combined.preset = Preset::dusk;
    combined.vignette = 40;
    combined.roundness = 70;
    combined.grain = 25;
    combined.grain_size = 2;
    combined.seed = 30;
    combined.frame = Frame::caption;
    combined.caption = caption.caption;
    const std::vector<uint8_t> once = painted(combined, nullptr, fonts.get());
    oa::platform::job_pool::Pool one(1);
    oa::platform::job_pool::Pool two(2);
    oa::platform::job_pool::Pool eight(8);
    CHECK(painted(combined, &one, fonts.get()) == once);
    CHECK(painted(combined, &two, fonts.get()) == once);
    CHECK(painted(combined, &eight, fonts.get()) == once);
    require_pin(
        "combined",
        digest_of(once),
        "3df50ea5b473c2d33db5bb025821137e6cce66350282f164d02793c704edb4a4"
    );

    std::vector<uint8_t> rgba(static_cast<std::size_t>(kWidth * kHeight * 4));
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(kWidth * kHeight); ++pixel) {
        rgba[pixel * 4] = source[pixel * 3];
        rgba[pixel * 4 + 1] = source[pixel * 3 + 1];
        rgba[pixel * 4 + 2] = source[pixel * 3 + 2];
        rgba[pixel * 4 + 3] = 77;
    }
    Picture wide;
    wide.bytes = rgba.data();
    wide.width = kWidth;
    wide.height = kHeight;
    wide.channels = 4;
    Look dusk;
    dusk.preset = Preset::dusk;
    apply_look(wide, dusk, nullptr, nullptr);
    bool alpha_kept = true;
    for (std::size_t pixel = 0; pixel < static_cast<std::size_t>(kWidth * kHeight); ++pixel) {
        if (rgba[pixel * 4 + 3] != 77)
            alpha_kept = false;
    }
    CHECK(alpha_kept);

    const int32_t pad = 5;
    std::vector<uint8_t> padded(
        static_cast<std::size_t>(kHeight) * static_cast<std::size_t>(kWidth * 3 + pad), 0xAB
    );
    for (int32_t row = 0; row < kHeight; ++row) {
        std::memcpy(
            padded.data() +
                static_cast<std::size_t>(row) * static_cast<std::size_t>(kWidth * 3 + pad),
            source.data() + static_cast<std::size_t>(row) * kWidth * 3,
            static_cast<std::size_t>(kWidth) * 3U
        );
    }
    Picture strided;
    strided.bytes = padded.data();
    strided.width = kWidth;
    strided.height = kHeight;
    strided.stride = kWidth * 3 + pad;
    strided.channels = 3;
    apply_look(strided, dusk, nullptr, nullptr);
    bool pad_kept = true;
    for (int32_t row = 0; row < kHeight; ++row) {
        const uint8_t* tail =
            padded.data() +
            static_cast<std::size_t>(row) * static_cast<std::size_t>(kWidth * 3 + pad) +
            static_cast<std::size_t>(kWidth) * 3U;
        for (int32_t byte = 0; byte < pad; ++byte) {
            if (tail[byte] != 0xAB)
                pad_kept = false;
        }
    }
    CHECK(pad_kept);

    CHECK(std::string(oa::present::photo::preset_key(Preset::none)) == "none");
    CHECK(std::string(oa::present::photo::preset_key(Preset::y1997)) == "1997");
    CHECK(std::string(oa::present::photo::preset_key(Preset::dusk)) == "dusk");
    CHECK(std::string(oa::present::photo::preset_key(Preset::arctic)) == "arctic");
    CHECK(std::string(oa::present::photo::preset_key(Preset::noir)) == "noir");
    CHECK(std::string(oa::present::photo::preset_key(Preset::bleach)) == "bleach");
    CHECK(std::string(oa::present::photo::frame_key(Frame::none)) == "none");
    CHECK(std::string(oa::present::photo::frame_key(Frame::thin)) == "thin");
    CHECK(std::string(oa::present::photo::frame_key(Frame::mat)) == "mat");
    CHECK(std::string(oa::present::photo::frame_key(Frame::caption)) == "caption");
    CHECK(oa::present::photo::caption_bar_height(64, 36) == 8);
    CHECK(oa::present::photo::caption_bar_height(640, 480) == 30);
    CHECK(oa::present::photo::caption_bar_height(1280, 720) == 46);
    CHECK(oa::present::photo::caption_bar_height(1920, 1080) == 69);

    // Grain size is in pixels. A photo two to four times the battlefield
    // draws each grain that many times larger, so it covers the same part of
    // the map. Box-reduced, with a grade too, it is the preview's look.
    CHECK(photo_grain_size(2, 1) == 2);
    CHECK(photo_grain_size(3, 2) == 6);
    CHECK(photo_grain_size(16, 4) == 64);
    CHECK(photo_grain_size(0, 3) == 3);
    CHECK(photo_grain_size(5, 0) == 5);
    CHECK(photo_grain_size(1 << 30, 4) == 2147483647);
    Look large_grain;
    large_grain.preset = Preset::dusk;
    large_grain.grain = 60;
    large_grain.grain_size = 3;
    large_grain.seed = 4242;
    const std::vector<uint8_t> preview = painted(large_grain, nullptr, nullptr);
    for (const int32_t factor : {2, 3, 4}) {
        Look scaled = large_grain;
        scaled.grain_size = photo_grain_size(large_grain.grain_size, factor);
        CHECK(reduced_look(scaled, factor) == preview);
        // Left at the panel's size the grain is finer, and the reduced
        // photo is not the preview.
        const std::vector<uint8_t> unscaled = reduced_look(large_grain, factor);
        CHECK(!unscaled.empty() && unscaled != preview);
    }

    if (failures != 0) {
        std::fprintf(stderr, "looks: %d failed\n", failures);
        return 1;
    }
    std::printf("present-photo-looks: ok\n");
    return 0;
}
