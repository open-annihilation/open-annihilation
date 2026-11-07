// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The clipboard image is prepared here, on the thread that encodes the
// photo. Offering it is separate, and runs on the main thread.
#include "photo_clipboard.hpp"

#include "oa/formats/png.hpp"

#include <SDL3/SDL.h>

#include <cstring>
#include <memory>
#include <utility>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace oa::app {
namespace {

PhotoClipboardHooks clipboard_hook = nullptr;

/// One picture's bytes. The remembered photo and the clipboard share it.
using PhotoBytes = std::shared_ptr<const std::vector<uint8_t>>;

/// The last photo the main thread can offer again.
struct RememberedPhoto {
    std::string file_name;
    std::string mime;
    PhotoBytes bytes;
    std::string refusal;
    bool held = false;
};

RememberedPhoto remembered_photo;

/// The photo this process offered, until the system's cleanup runs.
struct OfferedPhoto {
    std::string mime;
    PhotoBytes bytes;
    bool live = false;
};

std::shared_ptr<OfferedPhoto> offered_photo;

/// The bytes the system holds. The shared picture lives until cleanup.
struct ClipboardPayload {
    std::shared_ptr<OfferedPhoto> offered;
};

/// Hands the payload's bytes to the system. They stay until cleanup.
///
/// @param userdata the payload
/// @param mime_type the type the system asked for
/// @param size set to the number of bytes
/// @return the bytes, or null when the type is not the one offered
const void* SDLCALL clipboard_data(void* userdata, const char* mime_type, size_t* size) {
    if (size != nullptr)
        *size = 0;
    auto* payload = static_cast<ClipboardPayload*>(userdata);
    if (payload == nullptr || mime_type == nullptr || size == nullptr)
        return nullptr;
    if (payload->offered == nullptr || payload->offered->bytes == nullptr)
        return nullptr;
    if (payload->offered->mime != mime_type)
        return nullptr;
    *size = payload->offered->bytes->size();
    return payload->offered->bytes->data();
}

/// Frees the payload the system was holding.
///
/// @param userdata the payload
void SDLCALL clipboard_cleanup(void* userdata) {
    auto* payload = static_cast<ClipboardPayload*>(userdata);
    if (payload != nullptr && payload->offered != nullptr)
        payload->offered->live = false;
    delete payload;
}

/// Encodes 8-bit RGB as a BMP file.
///
/// @param rgb the picture, top row first
/// @param width its width
/// @param height its height
/// @param[out] file the BMP
/// @return false when the picture cannot be encoded
bool encode_bmp(
    std::span<const uint8_t> rgb, uint32_t width, uint32_t height, std::vector<uint8_t>* file
) {
    SDL_Surface* surface =
        SDL_CreateSurface(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGB24);
    if (surface == nullptr)
        return false;
    if (SDL_MUSTLOCK(surface) && !SDL_LockSurface(surface)) {
        SDL_DestroySurface(surface);
        return false;
    }
    auto* pixels = static_cast<uint8_t*>(surface->pixels);
    const auto pitch = static_cast<std::size_t>(surface->pitch);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 3U;
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(pixels + pitch * row, rgb.data() + row_bytes * row, row_bytes);
    }
    if (SDL_MUSTLOCK(surface))
        SDL_UnlockSurface(surface);

    SDL_IOStream* stream = SDL_IOFromDynamicMem();
    if (stream == nullptr || !SDL_SaveBMP_IO(surface, stream, false)) {
        if (stream != nullptr)
            SDL_CloseIO(stream);
        SDL_DestroySurface(surface);
        return false;
    }
    const Sint64 length = SDL_GetIOSize(stream);
    bool placed = length > 0 && SDL_SeekIO(stream, 0, SDL_IO_SEEK_SET) >= 0;
    if (placed) {
        file->resize(static_cast<std::size_t>(length));
        placed = SDL_ReadIO(stream, file->data(), file->size()) == file->size();
    }
    SDL_CloseIO(stream);
    SDL_DestroySurface(surface);
    if (!placed)
        file->clear();
    return placed;
}

/// Encodes 8-bit RGB as a PNG file.
///
/// @param rgb the picture
/// @param width its width
/// @param height its height
/// @param[out] file the PNG
/// @return false when the picture cannot be encoded
bool encode_png(
    std::span<const uint8_t> rgb, uint32_t width, uint32_t height, std::vector<uint8_t>* file
) {
    oa::formats::png::Header header;
    header.width = width;
    header.height = height;
    header.bit_depth = 8;
    header.color_type = oa::formats::png::ColorType::rgb;
    header.interlace = oa::formats::png::Interlace::none;
    oa::formats::png::Image image;
    image.header = header;
    image.rows = rgb;
    return oa::formats::png::write(image, file);
}

/// How many bytes a BMP of this size takes, including row padding.
///
/// @param width the photo's width
/// @param height the photo's height
/// @return an upper bound on the file
uint64_t bmp_byte_bound(uint32_t width, uint32_t height) noexcept {
    const uint64_t stride = (static_cast<uint64_t>(width) * 3U + 3U) & ~uint64_t{3U};
    return stride * height + 128U;
}

/// Offers bytes the hook or the system keeps.
///
/// The hook sees the remembered bytes for this call only. The system shares
/// that same buffer, freed from the cleanup callback and not before.
///
/// @param mime the offered type
/// @param bytes the picture
/// @return true when the clipboard took them
bool hand_to_clipboard(const char* mime, const PhotoBytes& bytes) {
    if (mime == nullptr || bytes == nullptr || bytes->empty())
        return false;
    if (clipboard_hook != nullptr)
        return clipboard_hook(mime, bytes->data(), bytes->size());
    if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0)
        return false;
    if (offered_photo == nullptr)
        offered_photo = std::make_shared<OfferedPhoto>();
    offered_photo->mime = mime;
    offered_photo->bytes = bytes;
    offered_photo->live = false;
    auto* payload = new ClipboardPayload;
    payload->offered = offered_photo;
    const char* types[] = {payload->offered->mime.c_str()};
    // A failure after the system has stored the payload leaves it for the
    // cleanup callback. Freeing it here would free it before that callback.
    const bool took = SDL_SetClipboardData(clipboard_data, clipboard_cleanup, payload, types, 1);
    if (took)
        offered_photo->live = true;
    return took;
}

/// Remembers a prepared image and offers it when the switch allows.
///
/// @param file_name the saved file's name
/// @param image the prepared image
/// @param copy_on the switch is on
/// @param game_mode the run is in Steam's Game Mode
/// @param image_clipboard this system offers an image
/// @return what the offer did
PhotoCopyReport offer_prepared(
    std::string file_name,
    PhotoClipboardImage image,
    bool copy_on,
    bool game_mode,
    bool image_clipboard
) {
    remembered_photo.file_name = std::move(file_name);
    remembered_photo.held = true;
    remembered_photo.bytes.reset();
    remembered_photo.mime.clear();
    remembered_photo.refusal = std::string(image.refusal);
    if (image.mime != nullptr)
        remembered_photo.mime = image.mime;
    if (!image.bytes.empty())
        remembered_photo.bytes =
            std::make_shared<const std::vector<uint8_t>>(std::move(image.bytes));

    PhotoCopyReport report;
    // A skip is silent, including when the photo is above a copy limit.
    if (!copy_on || game_mode || !image_clipboard) {
        report.result = PhotoCopyResult::skipped;
        return report;
    }
    if (!remembered_photo.refusal.empty()) {
        report.result = PhotoCopyResult::refused;
        report.reason = remembered_photo.refusal;
        return report;
    }
    if (remembered_photo.bytes == nullptr || remembered_photo.bytes->empty() ||
        remembered_photo.mime.empty()) {
        report.result = PhotoCopyResult::skipped;
        return report;
    }
    if (!hand_to_clipboard(remembered_photo.mime.c_str(), remembered_photo.bytes)) {
        report.result = PhotoCopyResult::refused;
        report.reason = std::string(kPhotoCopyRefused);
        return report;
    }
    report.result = PhotoCopyResult::copied;
    return report;
}

} // namespace

PhotoClipboardSystem this_photo_clipboard_system() noexcept {
#if defined(_WIN32)
    return PhotoClipboardSystem::windows;
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    return PhotoClipboardSystem::text_only;
#else
    return PhotoClipboardSystem::macos;
#endif
#else
    return PhotoClipboardSystem::linux;
#endif
}

const char* photo_clipboard_mime(PhotoClipboardSystem host) noexcept {
    switch (host) {
    case PhotoClipboardSystem::macos:
    case PhotoClipboardSystem::linux:
        return "image/png";
    case PhotoClipboardSystem::windows:
        return "image/bmp";
    case PhotoClipboardSystem::text_only:
        return nullptr;
    }
    return nullptr;
}

bool photo_clipboard_has_image(PhotoClipboardSystem host) noexcept {
    return photo_clipboard_mime(host) != nullptr;
}

std::string_view photo_copy_refusal(uint32_t width, uint32_t height, std::size_t bytes) noexcept {
    if (width > kPhotoCopySideLimit || height > kPhotoCopySideLimit)
        return kPhotoCopySide;
    if (bytes > kPhotoCopyByteLimit)
        return kPhotoCopyBytes;
    return {};
}

bool photo_copy_on(std::string_view stored) noexcept {
    return stored != "0";
}

PhotoClipboardImage make_photo_clipboard(
    std::span<const uint8_t> rgb,
    uint32_t width,
    uint32_t height,
    std::span<const uint8_t> png,
    PhotoClipboardSystem host
) {
    PhotoClipboardImage image;
    // Where the clipboard holds text, nothing is prepared and nothing is
    // named, even when a side is above the limit.
    const char* mime = photo_clipboard_mime(host);
    if (mime == nullptr)
        return image;
    const std::string_view side = photo_copy_refusal(width, height, 0);
    if (!side.empty()) {
        image.refusal = side;
        return image;
    }

    if (host == PhotoClipboardSystem::windows) {
        if (bmp_byte_bound(width, height) > kPhotoCopyByteLimit) {
            image.refusal = kPhotoCopyBytes;
            return image;
        }
    } else if (png.size() > kPhotoCopyByteLimit) {
        image.refusal = kPhotoCopyBytes;
        return image;
    }

    const uint64_t need = static_cast<uint64_t>(width) * height * 3U;
    if (width == 0 || height == 0 || need != rgb.size()) {
        image.refusal = kPhotoCopyRefused;
        return image;
    }

    std::vector<uint8_t> encoded;
    if (host == PhotoClipboardSystem::windows) {
        if (!encode_bmp(rgb, width, height, &encoded)) {
            image.refusal = kPhotoCopyRefused;
            return image;
        }
    } else if (!png.empty()) {
        encoded.assign(png.begin(), png.end());
    } else if (!encode_png(rgb, width, height, &encoded)) {
        image.refusal = kPhotoCopyRefused;
        return image;
    }
    const std::string_view sized = photo_copy_refusal(width, height, encoded.size());
    if (!sized.empty()) {
        image.refusal = sized;
        return image;
    }
    image.bytes = std::move(encoded);
    image.mime = mime;
    return image;
}

void set_photo_clipboard_hook(PhotoClipboardHooks hook) noexcept {
    clipboard_hook = hook;
}

PhotoCopyReport offer_photo_clipboard(
    std::string file_name,
    PhotoClipboardImage image,
    bool copy_on,
    bool game_mode,
    bool image_clipboard
) {
    return offer_prepared(
        std::move(file_name), std::move(image), copy_on, game_mode, image_clipboard
    );
}

PhotoCopyReport copy_remembered_photo(bool copy_on, bool game_mode, bool image_clipboard) {
    PhotoCopyReport report;
    if (!remembered_photo.held)
        return report;
    if (!copy_on || game_mode || !image_clipboard) {
        report.result = PhotoCopyResult::skipped;
        return report;
    }
    if (!remembered_photo.refusal.empty()) {
        report.result = PhotoCopyResult::refused;
        report.reason = remembered_photo.refusal;
        return report;
    }
    if (remembered_photo.bytes == nullptr || remembered_photo.bytes->empty() ||
        remembered_photo.mime.empty()) {
        report.result = PhotoCopyResult::skipped;
        return report;
    }
    if (!hand_to_clipboard(remembered_photo.mime.c_str(), remembered_photo.bytes)) {
        report.result = PhotoCopyResult::refused;
        report.reason = std::string(kPhotoCopyRefused);
        return report;
    }
    report.result = PhotoCopyResult::copied;
    return report;
}

bool remembered_photo_exists() noexcept {
    return remembered_photo.held;
}

std::string_view remembered_photo_name() noexcept {
    return remembered_photo.file_name;
}

void clear_remembered_photo() noexcept {
    remembered_photo = {};
}

void keep_offered_photo_after_quit() noexcept {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    // Cocoa serves the photo from this process until something asks for it.
    // One read while the video is still up leaves the picture on the
    // clipboard after the game quits. Windows already keeps its own copy.
    if (offered_photo == nullptr || !offered_photo->live || offered_photo->mime.empty())
        return;
    if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0)
        return;
    size_t byte_count = 0;
    void* data = SDL_GetClipboardData(offered_photo->mime.c_str(), &byte_count);
    SDL_free(data);
#else
    return;
#endif
}

std::string photo_saved_line(std::string_view saved, const PhotoCopyReport& report) {
    std::string line(saved);
    if (report.result == PhotoCopyResult::copied) {
        line += "  ";
        line += kPhotoCopied;
        return line;
    }
    if (report.result == PhotoCopyResult::refused && !report.reason.empty()) {
        line += "  ";
        std::string sentence(kPhotoNotCopied);
        const std::size_t at = sentence.find("{reason}");
        if (at != std::string::npos)
            sentence.replace(at, std::string("{reason}").size(), report.reason);
        line += sentence;
    }
    return line;
}

} // namespace oa::app
