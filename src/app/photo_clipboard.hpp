// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A saved photo offered to the clipboard. The picture is encoded on the
// photo writer's thread. The main thread offers those bytes, or a test's
// hook stands in for the clipboard.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// The preference that copies a saved photo. 0 is off, 1 is on. Absent is on.
inline constexpr std::string_view kPhotoCopyPreference = "open-annihilation.photo.copy";

/// Said with the saved message when the clipboard took the photo.
inline constexpr std::string_view kPhotoCopied = "Copied";

/// Said with the saved message when the photo was saved and not copied.
/// `{reason}` is one of the sentences below.
inline constexpr std::string_view kPhotoNotCopied = "Photo not copied: {reason}";

/// A side of the photo is above the copy limit.
inline constexpr std::string_view kPhotoCopySide = "a side is above 8192 pixels";

/// The copied picture is above the copy limit.
inline constexpr std::string_view kPhotoCopyBytes = "the photo is above 128 MB";

/// The clipboard did not take the bytes it was offered.
inline constexpr std::string_view kPhotoCopyRefused = "the clipboard did not take the photo";

/// An explicit Copy in Steam's Game Mode.
inline constexpr std::string_view kPhotoCopyGameMode = "not available in Game Mode";

/// An explicit Copy where the clipboard holds text only.
inline constexpr std::string_view kPhotoCopyTextOnly = "the clipboard holds text only";

/// The longest side a copied photo may have, in pixels.
inline constexpr uint32_t kPhotoCopySideLimit = 8192;

/// The most bytes a copied photo may hold.
inline constexpr std::size_t kPhotoCopyByteLimit = 128U * 1024U * 1024U;

/// Where a photo can be copied, and which image that clipboard takes.
enum class PhotoClipboardSystem : uint8_t {
    macos,     ///< image/png
    linux,     ///< image/png
    windows,   ///< image/bmp
    text_only, ///< the clipboard holds text, so a photo is not offered
};

/// The system this build copies a photo on.
///
/// @return macos, linux, windows or text_only
[[nodiscard]] PhotoClipboardSystem this_photo_clipboard_system() noexcept;

/// The image type a system is offered.
///
/// @param host the system
/// @return "image/png", "image/bmp", or null where the clipboard holds text
[[nodiscard]] const char* photo_clipboard_mime(PhotoClipboardSystem host) noexcept;

/// Tells whether a system is offered an image.
///
/// @param host the system
/// @return false where the clipboard holds text
[[nodiscard]] bool photo_clipboard_has_image(PhotoClipboardSystem host) noexcept;

/// Why a photo of this size is not copied. Empty when it may be copied.
///
/// A side above 8192 pixels is named before a picture above 128 MB.
///
/// @param width the photo's width
/// @param height the photo's height
/// @param bytes the copied picture's size
/// @return the English sentence, or empty
[[nodiscard]] std::string_view
photo_copy_refusal(uint32_t width, uint32_t height, std::size_t bytes) noexcept;

/// Reads open-annihilation.photo.copy. Only 0 is off. Absent is on.
///
/// @param stored the preference's text, or empty when the key is absent
/// @return true when a photo is copied
[[nodiscard]] bool photo_copy_on(std::string_view stored) noexcept;

/// The image prepared for one system's clipboard.
struct PhotoClipboardImage {
    std::vector<uint8_t> bytes; ///< the PNG or the BMP, empty when nothing is offered
    const char* mime = nullptr; ///< "image/png" or "image/bmp", or null
    std::string_view refusal{}; ///< why it was not prepared, or empty
};

/// Prepares the image a system is offered.
///
/// A PNG system is given `png` when that span is not empty, and otherwise
/// the PNG encoded from `rgb`. Windows is given a BMP encoded from `rgb`.
/// A side above 8192 pixels, or a picture above 128 MB, is refused before
/// the BMP is encoded. Where the clipboard holds text, this returns before
/// that check: an empty image and no refusal, so the copy is skipped.
///
/// @param rgb 8-bit RGB, row by row, top row first
/// @param width the photo's width
/// @param height the photo's height
/// @param png the PNG file already written, which a PNG clipboard reuses
/// @param host the system
/// @return the image, or a refusal
[[nodiscard]] PhotoClipboardImage make_photo_clipboard(
    std::span<const uint8_t> rgb,
    uint32_t width,
    uint32_t height,
    std::span<const uint8_t> png,
    PhotoClipboardSystem host
);

/// Answers whether the clipboard took the bytes. A test stands in for the
/// clipboard with this. The pointer is valid only for the call.
using PhotoClipboardHooks =
    bool (*)(const char* mime_type, const uint8_t* bytes, std::size_t byte_count);

/// Stands a hook in for the clipboard. Null uses the system's clipboard.
///
/// @param hook the hook, or null
void set_photo_clipboard_hook(PhotoClipboardHooks hook) noexcept;

/// What offering a photo to the clipboard did.
enum class PhotoCopyResult : uint8_t {
    skipped, ///< nothing was offered, and the status stays the saved message
    refused, ///< the photo was saved and the status says it was not copied
    copied,  ///< the clipboard took the photo
};

/// The outcome of one offer.
struct PhotoCopyReport {
    PhotoCopyResult result = PhotoCopyResult::skipped; ///< what the offer did
    std::string reason; ///< the English sentence when result is refused
};

/// Offers a prepared image, and remembers it so it can be copied again.
///
/// Nothing is offered when copying is off, in Steam's Game Mode, or where
/// the clipboard has no image, and that skip is silent even when the photo
/// is above a copy limit. A refusal is named only when a copy was wanted.
/// The bytes handed to the system are the remembered picture, and they stay
/// alive until the system finishes with them.
///
/// @param file_name the saved file's name, without a folder
/// @param image the prepared image
/// @param copy_on open-annihilation.photo.copy is on
/// @param game_mode the run is in Steam's Game Mode
/// @param image_clipboard this system offers an image
/// @return what the offer did
[[nodiscard]] PhotoCopyReport offer_photo_clipboard(
    std::string file_name,
    PhotoClipboardImage image,
    bool copy_on,
    bool game_mode,
    bool image_clipboard
);

/// Offers the last photo again.
///
/// Nothing is offered when no photo is remembered, when copying is off, in
/// Steam's Game Mode, or where the clipboard has no image. That skip comes
/// before a refusal, so an oversized photo stays silent there too.
///
/// @param copy_on open-annihilation.photo.copy is on
/// @param game_mode the run is in Steam's Game Mode
/// @param image_clipboard this system offers an image
/// @return what the offer did. Skipped when no photo is remembered
[[nodiscard]] PhotoCopyReport
copy_remembered_photo(bool copy_on, bool game_mode, bool image_clipboard);

/// Tells whether a photo is remembered for copying again.
///
/// @return true after a saved photo was prepared
[[nodiscard]] bool remembered_photo_exists() noexcept;

/// The remembered photo's file name.
///
/// Valid until the next offer. Empty when no photo is remembered.
///
/// @return the file name, without a folder
[[nodiscard]] std::string_view remembered_photo_name() noexcept;

/// Bytes of the picture still held for the clipboard, including when copying
/// is off. The memory guard counts these. Zero when nothing is held.
///
/// @return the bytes
[[nodiscard]] std::size_t remembered_photo_bytes() noexcept;

/// Forgets the remembered photo. A test starts from none.
void clear_remembered_photo() noexcept;

/// On macOS, reads the offered photo once so it stays after the game quits.
///
/// Does nothing unless this process's photo is still the clipboard's, and
/// nothing on the other systems. Call it while SDL's video is still up.
void keep_offered_photo_after_quit() noexcept;

/// The saved message with the copy note after it, in English.
///
/// A skipped copy is the saved message alone. A copy adds "Copied". A
/// refusal adds "Photo not copied" and the reason.
///
/// @param saved the saved message, already filled in
/// @param report what the offer did
/// @return the status line
[[nodiscard]] std::string photo_saved_line(std::string_view saved, const PhotoCopyReport& report);

} // namespace oa::app
