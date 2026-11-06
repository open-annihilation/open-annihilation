// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// IMG loading (PNG through the native codec), pending-image bookkeeping and
// markup files.

#include <cstdio>
#include <cstring>

#include "internal.hpp"
#include "oa/formats/png.hpp"

namespace oa::boneyards::html {

using namespace detail;

namespace {

constexpr std::size_t k_notice_bytes = 1024;
// Name shown when the decoder has no file name.
constexpr const char* k_unnamed_file = "UNKNOWN (ERROR!)";

// Decoder notices name the image file they came from.
struct DecoderContext {
    const Environment* env;
    const char* file;
};

void forward_warning(void* user, const char* text) {
    const auto* context = static_cast<const DecoderContext*>(user);
    png_decoder_warning(*context->env, context->file, text);
}

void forward_error(void* user, const char* text) {
    const auto* context = static_cast<const DecoderContext*>(user);
    png_decoder_error(*context->env, context->file, text);
}

bool palette_starts_black(const formats::png::Info& info) {
    return info.has_palette && info.palette_entries > 0 && info.palette[0].r == 0 &&
           info.palette[0].g == 0 && info.palette[0].b == 0;
}

} // namespace

void png_decoder_warning(const Environment& env, const char* file, const char* text) {
    if (env.notices.show == nullptr)
        return;
    char notice[k_notice_bytes];
    std::snprintf(
        notice,
        sizeof notice,
        "%s: libpng warning: %s\n",
        file != nullptr ? file : k_unnamed_file,
        text
    );
    env.notices.show(env.notices.user, k_png_notice_title, notice);
}

void png_decoder_error(const Environment& env, const char* file, const char* text) {
    png_decoder_warning(env, file, text);
}

bool image_load_png(
    Document& doc, int32_t index, const char* path, int32_t width, int32_t height, const char* href
) {
    if (path == nullptr)
        return true;
    {
        Image& image = doc.images[index];
        if (href != nullptr)
            image.head.href = href;
        image.pixels.clear();
        image.row_bytes = 0;
        if (width > 0)
            image.width = width;
        if (height > 0)
            image.height = height;
        if (image.name != path) {
            std::memset(image.name, 0, sizeof image.name);
            std::snprintf(image.name, sizeof image.name, "%s", path);
        }
        image.name[k_image_name_bytes - 1] = '\0';
        image.raw_indices = false;
    }
    std::vector<uint8_t> bytes;
    if (doc.env.files.read == nullptr ||
        !doc.env.files.read(doc.env.files.user, doc.images[index].name, &bytes))
        return false;
    if (!formats::png::has_signature(bytes))
        return false;
    Image& image = doc.images[index];
    DecoderContext context{&doc.env, image.name};
    const formats::png::Messages messages{&context, forward_warning, forward_error};
    formats::png::Info info;
    if (!formats::png::read_info(bytes, messages, &info))
        return true;
    const formats::png::Header header = formats::png::header_of(info, messages);
    image.source_width = static_cast<int32_t>(header.width);
    image.source_height = static_cast<int32_t>(header.height);
    if (image.width < 1)
        image.width = image.source_width;
    if (image.height < 1)
        image.height = image.source_height;
    if (header.width > static_cast<uint32_t>(k_image_max_side) ||
        header.height > static_cast<uint32_t>(k_image_max_side))
        return true;
    image.raw_indices = palette_starts_black(info);
    constexpr formats::png::Transforms k_transforms{true, true};
    const std::size_t row_bytes = formats::png::row_bytes(header, k_transforms);
    image.row_bytes = static_cast<int32_t>(row_bytes);
    image.pixels.assign(row_bytes * header.height, 0);
    if (formats::png::read_image(bytes, info, k_transforms, messages, image.pixels) !=
        formats::png::Progress::none)
        image.loaded = true;
    return true;
}

bool document_pending_images(const Document& doc, std::vector<const char*>* names) {
    names->clear();
    for (const Image& image : doc.images)
        if (!image.loaded)
            names->push_back(image.name);
    return !names->empty();
}

void document_reload_image(Document& doc, const char* name, const char* path) {
    for (std::size_t i = 0; i < doc.images.size(); ++i)
        if (std::strcmp(doc.images[i].name, name) == 0)
            image_load_png(doc, static_cast<int32_t>(i), path, 0, 0, nullptr);
}

bool document_load_file(Document& doc, const char* path) {
    std::vector<uint8_t> bytes;
    if (doc.env.files.read == nullptr || !doc.env.files.read(doc.env.files.user, path, &bytes))
        return false;
    // Like 3.1c, the text ends at the file's first NUL.
    const auto* text = reinterpret_cast<const char*>(bytes.data());
    const std::size_t length = strnlen(text, bytes.size());
    return document_load(doc, std::string_view(text, length));
}

} // namespace oa::boneyards::html
