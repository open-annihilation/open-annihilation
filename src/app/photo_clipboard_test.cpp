// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A saved photo offered through the clipboard hook: the type for each
// system, the picture's pixels, the size caps, and the times nothing is
// offered.
#include "photo_clipboard.hpp"
#include "photo_writer.hpp"

#include "oa/formats/png.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace {

using oa::app::PhotoClipboardImage;
using oa::app::PhotoClipboardSystem;
using oa::app::PhotoCopyReport;
using oa::app::PhotoCopyResult;

constexpr std::array<uint8_t, 12> kPixels = {
    255,
    0,
    0,
    0,
    255,
    0,
    0,
    0,
    255,
    255,
    255,
    0,
};

struct Caught {
    int offers = 0;
    bool take = true;
    std::string mime;
    std::vector<uint8_t> bytes;
};

Caught caught;

/// The hook. It keeps the bytes for this call and says whether they were taken.
///
/// @param mime_type the type offered
/// @param bytes the picture
/// @param byte_count how many bytes
/// @return whether the stand-in clipboard took them
bool catch_offer(const char* mime_type, const uint8_t* bytes, std::size_t byte_count) {
    ++caught.offers;
    caught.mime = mime_type != nullptr ? mime_type : "";
    if (bytes != nullptr)
        caught.bytes.assign(bytes, bytes + byte_count);
    else
        caught.bytes.clear();
    return caught.take;
}

void reset_offer() {
    caught = {};
    oa::app::clear_remembered_photo();
    oa::app::set_photo_clipboard_hook(catch_offer);
}

bool same_text(const char* left, const char* right) {
    if (left == nullptr || right == nullptr)
        return left == right;
    return std::strcmp(left, right) == 0;
}

bool png_is_pixels(std::span<const uint8_t> file) {
    oa::formats::png::Messages messages{};
    oa::formats::png::Info info;
    if (!oa::formats::png::read_info(file, messages, &info))
        return false;
    if (info.header.width != 2 || info.header.height != 2)
        return false;
    if (info.header.color_type != oa::formats::png::ColorType::rgb)
        return false;
    std::vector<uint8_t> rows(kPixels.size());
    const oa::formats::png::Progress progress =
        oa::formats::png::read_image(file, info, {}, messages, rows);
    return progress == oa::formats::png::Progress::end &&
           std::memcmp(rows.data(), kPixels.data(), kPixels.size()) == 0;
}

bool bmp_is_pixels(std::span<const uint8_t> file) {
    SDL_IOStream* stream = SDL_IOFromConstMem(file.data(), file.size());
    if (stream == nullptr)
        return false;
    SDL_Surface* loaded = SDL_LoadBMP_IO(stream, true);
    if (loaded == nullptr)
        return false;
    SDL_Surface* picture = loaded;
    if (loaded->format != SDL_PIXELFORMAT_RGB24) {
        picture = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGB24);
        SDL_DestroySurface(loaded);
        if (picture == nullptr)
            return false;
    }
    const bool locked = SDL_MUSTLOCK(picture);
    if (locked && !SDL_LockSurface(picture)) {
        SDL_DestroySurface(picture);
        return false;
    }
    bool matches = picture->w == 2 && picture->h == 2 && picture->pixels != nullptr;
    if (matches) {
        const auto* rows = static_cast<const uint8_t*>(picture->pixels);
        const auto pitch = static_cast<std::size_t>(picture->pitch);
        for (int row = 0; row < 2 && matches; ++row) {
            matches = std::memcmp(
                          rows + pitch * static_cast<std::size_t>(row),
                          kPixels.data() + static_cast<std::size_t>(row) * 6U,
                          6
                      ) == 0;
        }
    }
    if (locked)
        SDL_UnlockSurface(picture);
    SDL_DestroySurface(picture);
    return matches;
}

PhotoClipboardImage prepare(PhotoClipboardSystem host, std::span<const uint8_t> png = {}) {
    return oa::app::make_photo_clipboard(kPixels, 2, 2, png, host);
}

void the_system_and_the_switch() {
    OA_CHECK(same_text(oa::app::photo_clipboard_mime(PhotoClipboardSystem::macos), "image/png"));
    OA_CHECK(same_text(oa::app::photo_clipboard_mime(PhotoClipboardSystem::linux), "image/png"));
    OA_CHECK(same_text(oa::app::photo_clipboard_mime(PhotoClipboardSystem::windows), "image/bmp"));
    OA_CHECK(oa::app::photo_clipboard_mime(PhotoClipboardSystem::text_only) == nullptr);
    OA_CHECK(!oa::app::photo_clipboard_has_image(PhotoClipboardSystem::text_only));
    OA_CHECK(oa::app::photo_clipboard_has_image(PhotoClipboardSystem::windows));
#if defined(_WIN32)
    OA_CHECK(oa::app::this_photo_clipboard_system() == PhotoClipboardSystem::windows);
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    OA_CHECK(oa::app::this_photo_clipboard_system() == PhotoClipboardSystem::text_only);
#else
    OA_CHECK(oa::app::this_photo_clipboard_system() == PhotoClipboardSystem::macos);
#endif
#else
    OA_CHECK(oa::app::this_photo_clipboard_system() == PhotoClipboardSystem::linux);
#endif
    OA_CHECK(oa::app::photo_copy_on(""));
    OA_CHECK(oa::app::photo_copy_on("1"));
    OA_CHECK(!oa::app::photo_copy_on("0"));
}

void the_prepared_picture() {
    const PhotoClipboardImage mac = prepare(PhotoClipboardSystem::macos);
    OA_CHECK(same_text(mac.mime, "image/png"));
    OA_CHECK(mac.refusal.empty());
    OA_CHECK(png_is_pixels(mac.bytes));

    const PhotoClipboardImage reused = prepare(PhotoClipboardSystem::linux, mac.bytes);
    OA_CHECK(reused.bytes == mac.bytes);
    OA_CHECK(png_is_pixels(reused.bytes));

    const PhotoClipboardImage windows = prepare(PhotoClipboardSystem::windows);
    OA_CHECK(same_text(windows.mime, "image/bmp"));
    OA_CHECK(windows.refusal.empty());
    OA_CHECK(bmp_is_pixels(windows.bytes));

    const PhotoClipboardImage text = prepare(PhotoClipboardSystem::text_only);
    OA_CHECK(text.bytes.empty());
    OA_CHECK(text.mime == nullptr);
    OA_CHECK(text.refusal.empty());
}

void the_offer_and_the_copy_again() {
    reset_offer();
    const PhotoClipboardSystem host = oa::app::this_photo_clipboard_system();
    const char* mime = oa::app::photo_clipboard_mime(host);
    if (mime == nullptr) {
        OA_CHECK(host == PhotoClipboardSystem::text_only);
        return;
    }
    PhotoCopyReport report =
        oa::app::offer_photo_clipboard("Shot.png", prepare(host), true, false, true);
    OA_CHECK(report.result == PhotoCopyResult::copied);
    OA_CHECK(caught.offers == 1);
    OA_CHECK(caught.mime == mime);
    if (host == PhotoClipboardSystem::windows)
        OA_CHECK(bmp_is_pixels(caught.bytes));
    else
        OA_CHECK(png_is_pixels(caught.bytes));
    OA_CHECK(oa::app::remembered_photo_exists());
    OA_CHECK(oa::app::remembered_photo_name() == "Shot.png");
    const std::string saved = oa::app::photo_saved_line("Saved Shot.png", report);
    OA_CHECK(saved == "Saved Shot.png  Copied");

    const std::vector<uint8_t> first = caught.bytes;
    report = oa::app::copy_remembered_photo(true, false, true);
    OA_CHECK(report.result == PhotoCopyResult::copied);
    OA_CHECK(caught.offers == 2);
    OA_CHECK(caught.bytes == first);
    OA_CHECK(caught.mime == mime);
}

void nothing_offered_when_copying_is_off() {
    reset_offer();
    const PhotoClipboardSystem host = oa::app::this_photo_clipboard_system();
    PhotoCopyReport report =
        oa::app::offer_photo_clipboard("Shot.png", prepare(host), false, false, true);
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);
    OA_CHECK(oa::app::photo_saved_line("Saved Shot.png", report) == "Saved Shot.png");
    report = oa::app::copy_remembered_photo(false, false, true);
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);
}

void nothing_offered_in_game_mode() {
    reset_offer();
    const PhotoClipboardSystem host = oa::app::this_photo_clipboard_system();
    PhotoCopyReport report =
        oa::app::offer_photo_clipboard("Shot.png", prepare(host), true, true, true);
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);
    report = oa::app::copy_remembered_photo(true, true, true);
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);
}

void nothing_offered_without_an_image_clipboard() {
    reset_offer();
    PhotoCopyReport report = oa::app::offer_photo_clipboard(
        "Shot.png", prepare(PhotoClipboardSystem::macos), true, false, false
    );
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);
    report = oa::app::copy_remembered_photo(true, false, false);
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);

    reset_offer();
    report = oa::app::offer_photo_clipboard(
        "Shot.png", prepare(PhotoClipboardSystem::text_only), true, false, false
    );
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);
}

void a_side_above_the_limit_is_not_copied() {
    reset_offer();
    OA_CHECK(oa::app::photo_copy_refusal(8193, 1, 0) == oa::app::kPhotoCopySide);
    OA_CHECK(oa::app::photo_copy_refusal(1, 8193, 0) == oa::app::kPhotoCopySide);
    OA_CHECK(oa::app::photo_copy_refusal(oa::app::kPhotoCopySideLimit, 1, 0).empty());
    PhotoClipboardImage wide =
        oa::app::make_photo_clipboard({}, 8193, 1, {}, PhotoClipboardSystem::macos);
    OA_CHECK(wide.bytes.empty());
    OA_CHECK(wide.refusal == oa::app::kPhotoCopySide);
    PhotoCopyReport report =
        oa::app::offer_photo_clipboard("Shot.png", std::move(wide), true, false, true);
    OA_CHECK(report.result == PhotoCopyResult::refused);
    OA_CHECK(caught.offers == 0);
    const std::string saved = oa::app::photo_saved_line("Saved Shot.png", report);
    OA_CHECK(saved.starts_with("Saved Shot.png  "));
    OA_CHECK(saved.find("8192") != std::string::npos);
    report = oa::app::copy_remembered_photo(true, false, true);
    OA_CHECK(report.result == PhotoCopyResult::refused);
    OA_CHECK(caught.offers == 0);
}

void a_picture_above_the_byte_limit_is_not_copied() {
    reset_offer();
    const std::size_t limit = oa::app::kPhotoCopyByteLimit;
    OA_CHECK(oa::app::photo_copy_refusal(2, 2, limit).empty());
    OA_CHECK(oa::app::photo_copy_refusal(2, 2, limit + 1) == oa::app::kPhotoCopyBytes);
    OA_CHECK(oa::app::photo_copy_refusal(8193, 1, limit + 1) == oa::app::kPhotoCopySide);
    PhotoClipboardImage huge =
        oa::app::make_photo_clipboard({}, 8192, 8192, {}, PhotoClipboardSystem::windows);
    OA_CHECK(huge.bytes.empty());
    OA_CHECK(huge.refusal == oa::app::kPhotoCopyBytes);
    const PhotoCopyReport report =
        oa::app::offer_photo_clipboard("Shot.png", std::move(huge), true, false, true);
    OA_CHECK(report.result == PhotoCopyResult::refused);
    OA_CHECK(caught.offers == 0);
    const std::string saved = oa::app::photo_saved_line("Saved Shot.png", report);
    OA_CHECK(saved.find("128") != std::string::npos);
}

void a_clipboard_that_does_not_take_the_photo() {
    reset_offer();
    caught.take = false;
    const PhotoClipboardSystem host = oa::app::this_photo_clipboard_system();
    const PhotoCopyReport report =
        oa::app::offer_photo_clipboard("Shot.png", prepare(host), true, false, true);
    OA_CHECK(report.result == PhotoCopyResult::refused);
    OA_CHECK(report.reason == oa::app::kPhotoCopyRefused);
    OA_CHECK(caught.offers == 1);
    const std::string saved = oa::app::photo_saved_line("Saved Shot.png", report);
    OA_CHECK(saved.find("clipboard") != std::string::npos);
}

void copying_again_with_nothing_remembered() {
    reset_offer();
    const PhotoCopyReport report = oa::app::copy_remembered_photo(true, false, true);
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);
    OA_CHECK(!oa::app::remembered_photo_exists());
}

/// An oversized photo is skipped, and the saved line stays the saved message.
///
/// @param copy_on the switch
/// @param game_mode Steam's Game Mode
/// @param image_clipboard this system offers an image
void oversized_photo_is_skipped(bool copy_on, bool game_mode, bool image_clipboard) {
    reset_offer();
    PhotoClipboardImage wide =
        oa::app::make_photo_clipboard({}, 8193, 1, {}, PhotoClipboardSystem::macos);
    OA_CHECK(wide.refusal == oa::app::kPhotoCopySide);
    PhotoCopyReport report = oa::app::offer_photo_clipboard(
        "Shot.png", std::move(wide), copy_on, game_mode, image_clipboard
    );
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);
    OA_CHECK(oa::app::photo_saved_line("Saved Shot.png", report) == "Saved Shot.png");
    report = oa::app::copy_remembered_photo(copy_on, game_mode, image_clipboard);
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);
    OA_CHECK(oa::app::photo_saved_line("Saved Shot.png", report) == "Saved Shot.png");

    reset_offer();
    PhotoClipboardImage huge =
        oa::app::make_photo_clipboard({}, 8192, 8192, {}, PhotoClipboardSystem::windows);
    OA_CHECK(huge.refusal == oa::app::kPhotoCopyBytes);
    report = oa::app::offer_photo_clipboard(
        "Shot.png", std::move(huge), copy_on, game_mode, image_clipboard
    );
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);
    OA_CHECK(oa::app::photo_saved_line("Saved Shot.png", report) == "Saved Shot.png");
    report = oa::app::copy_remembered_photo(copy_on, game_mode, image_clipboard);
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(caught.offers == 0);
    OA_CHECK(oa::app::photo_saved_line("Saved Shot.png", report) == "Saved Shot.png");
}

void an_oversized_photo_is_skipped_before_it_is_refused() {
    oversized_photo_is_skipped(false, false, true);
    oversized_photo_is_skipped(true, true, true);
    oversized_photo_is_skipped(true, false, false);

    const PhotoClipboardImage text_wide =
        oa::app::make_photo_clipboard({}, 8193, 1, {}, PhotoClipboardSystem::text_only);
    OA_CHECK(text_wide.bytes.empty());
    OA_CHECK(text_wide.mime == nullptr);
    OA_CHECK(text_wide.refusal.empty());
    const PhotoClipboardImage text_huge =
        oa::app::make_photo_clipboard({}, 8192, 8192, {}, PhotoClipboardSystem::text_only);
    OA_CHECK(text_huge.bytes.empty());
    OA_CHECK(text_huge.refusal.empty());

    reset_offer();
    PhotoCopyReport report = oa::app::offer_photo_clipboard(
        "Shot.png",
        oa::app::make_photo_clipboard({}, 8193, 1, {}, PhotoClipboardSystem::text_only),
        true,
        false,
        false
    );
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(oa::app::photo_saved_line("Saved Shot.png", report) == "Saved Shot.png");
    report = oa::app::copy_remembered_photo(true, false, false);
    OA_CHECK(report.result == PhotoCopyResult::skipped);
    OA_CHECK(oa::app::photo_saved_line("Saved Shot.png", report) == "Saved Shot.png");
    OA_CHECK(caught.offers == 0);
}

/// Counts one wake for each photo the writer accepted.
std::atomic<int> writer_wakes{0};

/// The writer's wake for this test.
void count_writer_wake() noexcept {
    writer_wakes.fetch_add(1);
}

void the_writer_wakes_once_for_each_photo() {
    writer_wakes.store(0);
    oa::app::set_photo_writer_wake(count_writer_wake);
    const std::filesystem::path folder = oa::test::make_scratch_directory("oa-p06-photo-wake");
    std::error_code remove_error;

    {
        oa::app::PhotoWriter writer;
        oa::app::PhotoWriter::Picture refused;
        refused.width = 0;
        refused.height = 0;
        refused.file_name = "Refused.png";
        refused.path = folder / refused.file_name;
        OA_CHECK(writer.enqueue(std::move(refused)));

        oa::app::PhotoWriter::Picture saved;
        saved.rgb = {1, 2, 3};
        saved.width = 1;
        saved.height = 1;
        saved.file_name = "Saved.png";
        saved.path = folder / saved.file_name;
        OA_CHECK(writer.enqueue(std::move(saved)));
        writer.finish();

        OA_CHECK(writer_wakes.load() == 2);
        const std::optional<oa::app::PhotoWriter::Notice> first = writer.poll();
        const std::optional<oa::app::PhotoWriter::Notice> second = writer.poll();
        OA_CHECK(first.has_value());
        OA_CHECK(first.has_value() && !first->saved);
        OA_CHECK(second.has_value());
        OA_CHECK(second.has_value() && second->saved);
        OA_CHECK(!writer.poll().has_value());
    }

    oa::app::set_photo_writer_wake(nullptr);
    std::filesystem::remove_all(folder, remove_error);
}

} // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL video did not start\n");
        return 1;
    }
    the_system_and_the_switch();
    the_prepared_picture();
    the_offer_and_the_copy_again();
    nothing_offered_when_copying_is_off();
    nothing_offered_in_game_mode();
    nothing_offered_without_an_image_clipboard();
    a_side_above_the_limit_is_not_copied();
    a_picture_above_the_byte_limit_is_not_copied();
    a_clipboard_that_does_not_take_the_photo();
    copying_again_with_nothing_remembered();
    an_oversized_photo_is_skipped_before_it_is_refused();
    the_writer_wakes_once_for_each_photo();
    oa::app::set_photo_clipboard_hook(nullptr);
    oa::app::clear_remembered_photo();
    SDL_Quit();
    return oa::test::check_exit_status();
}
