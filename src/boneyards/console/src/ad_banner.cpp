// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Banner strip. With no ad server, or when its list cannot be fetched, the three local sample
// frames rotate every 900 pulses, as they always have. Otherwise each banner from ads.def is
// shown for its own interval, and a click opens its link.
#include "ad_banner.hpp"

#include "console_state.hpp"
#include "widgets.hpp"

#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/registration.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/png.hpp"
#include "oa/platform/files.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::boneyards::console {

struct ParsedSlot {
    int32_t id{};
    int32_t pulses{};
    int32_t weight{};
    int32_t unique{};
    char file[160]{};
    char link[512]{};
    char relative[160]{};
};

struct UniqueSlot {
    Handle image{};
    int32_t id{};
    int32_t pulses{};
    char link[512]{};
};

struct BannerReel {
    int32_t parsed_count{};
    int32_t unique_count{};
    int32_t show_count{};
    int32_t shown{};
    int32_t show_at{};
    ParsedSlot parsed[k_ad_banner_limit]{};
    UniqueSlot unique[k_ad_banner_limit]{};
    int32_t show[k_ad_banner_show_limit]{};
};

namespace {

namespace fs = std::filesystem;

constexpr int32_t k_banner_count = 3;
constexpr int32_t k_wait_catalogue = 1;
constexpr int32_t k_wait_png = 2;
constexpr int32_t k_unique_pending = -1;
constexpr int32_t k_unique_failed = -2;
constexpr char k_reserved[] = "{}[]<>:=%";
constexpr char k_catalogue_path[] = "boneyards/ta/ads/ads.def";
constexpr char k_catalogue_suffix[] = "/boneyards/ta/ads/ads.def";
constexpr char k_download_mark[] = "0BY";
const char* const k_banner_name = "Ad_Banner";
const char* const k_banner_gaf = "Ad_Banners.Gaf";
const char* const k_banner_sequence = "Ad_Sample";

AdBanner* as_banner(Window* window) {
    return reinterpret_cast<AdBanner*>(window);
}

bool copy_fit(char* out, std::size_t capacity, const char* text) {
    if (out == nullptr || capacity == 0)
        return false;
    if (text == nullptr) {
        out[0] = '\0';
        return true;
    }
    std::size_t count = 0;
    while (text[count] != '\0')
        ++count;
    if (count + 1 > capacity)
        return false;
    std::memcpy(out, text, count + 1);
    return true;
}

bool append_text(char* out, std::size_t capacity, const char* text) {
    if (out == nullptr || capacity == 0)
        return false;
    std::size_t count = 0;
    while (count < capacity && out[count] != '\0')
        ++count;
    if (count >= capacity)
        return false;
    if (text == nullptr)
        return true;
    std::size_t index = 0;
    while (text[index] != '\0') {
        if (count + 1 >= capacity) {
            out[count] = '\0';
            return false;
        }
        out[count] = text[index];
        ++count;
        ++index;
    }
    out[count] = '\0';
    return true;
}

bool append_port(char* out, std::size_t capacity, int32_t port) {
    char digits[6];
    int32_t count = 0;
    int32_t value = port > 0 ? port : 0;
    do {
        digits[count] = static_cast<char>('0' + (value % 10));
        ++count;
        value /= 10;
    } while (value > 0 && count < 6);
    while (count > 0) {
        --count;
        const char one[2] = {digits[count], '\0'};
        if (!append_text(out, capacity, one))
            return false;
    }
    return true;
}

bool starts_ignore_case(const char* text, const char* prefix) {
    if (text == nullptr || prefix == nullptr)
        return false;
    for (std::size_t index = 0; prefix[index] != '\0'; ++index) {
        char c = text[index];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != prefix[index])
            return false;
    }
    return true;
}

// Host and port of an ad_server url, written as http://host:port. The path on the url is ignored.
bool banner_origin(const char* url, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0 || url == nullptr)
        return false;
    out[0] = '\0';
    const char* cursor = url;
    if (starts_ignore_case(cursor, "https://"))
        cursor += 8;
    else if (starts_ignore_case(cursor, "http://"))
        cursor += 7;
    const char* host = cursor;
    while (*cursor != '\0' && *cursor != ':' && *cursor != '/' && *cursor != '?' && *cursor != '#')
        ++cursor;
    const std::size_t host_length = static_cast<std::size_t>(cursor - host);
    if (host_length == 0 || host_length >= 256)
        return false;
    int32_t port = 80;
    if (*cursor == ':') {
        ++cursor;
        if (*cursor < '0' || *cursor > '9')
            return false;
        int32_t value = 0;
        while (*cursor >= '0' && *cursor <= '9') {
            value = value * 10 + (*cursor - '0');
            if (value > 65535)
                return false;
            ++cursor;
        }
        if (value < 1)
            return false;
        port = value;
    }
    char host_text[256];
    std::memcpy(host_text, host, host_length);
    host_text[host_length] = '\0';
    if (!append_text(out, capacity, "http://") || !append_text(out, capacity, host_text) ||
        !append_text(out, capacity, ":"))
        return false;
    return append_port(out, capacity, port);
}

bool plain_token(std::string_view text) {
    if (text.empty())
        return false;
    for (const char c : text) {
        if (c == '\0')
            return false;
        for (const char reserved : std::string_view{k_reserved}) {
            if (c == reserved)
                return false;
        }
    }
    return true;
}

bool decode_value(std::string_view raw, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return false;
    std::size_t count = 0;
    for (std::size_t index = 0; index < raw.size(); ++index) {
        char byte = raw[index];
        if (byte == '%') {
            if (index + 1 >= raw.size())
                return false;
            ++index;
            const char digit = raw[index];
            if (digit < '0' || digit > '8')
                byte = static_cast<char>(0xff);
            else
                byte = k_reserved[digit - '0'];
        }
        if (count + 1 >= capacity)
            return false;
        out[count] = byte;
        ++count;
    }
    out[count] = '\0';
    return true;
}

bool parse_i32(std::string_view text, int32_t* out) {
    if (text.empty() || out == nullptr)
        return false;
    std::size_t index = 0;
    int32_t sign = 1;
    if (text[0] == '+' || text[0] == '-') {
        if (text[0] == '-')
            sign = -1;
        index = 1;
        if (index >= text.size())
            return false;
    }
    int64_t total = 0;
    for (; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9')
            return false;
        total = total * 10 + static_cast<int64_t>(text[index] - '0');
        if (total > static_cast<int64_t>(INT32_MAX) + 1)
            return false;
    }
    if (sign < 0)
        total = -total;
    if (total > INT32_MAX || total < INT32_MIN)
        return false;
    *out = static_cast<int32_t>(total);
    return true;
}

struct Field {
    char type{};
    char key[32]{};
    char value[512]{};
};

bool parse_field(std::string_view text, Field* field, std::size_t* taken) {
    if (field == nullptr || taken == nullptr || text.size() < 5 || text[0] != '<')
        return false;
    const char type = text[1];
    if ((type != 'I' && type != 'S' && type != 'U') || text[2] != ':')
        return false;
    const std::size_t eq = text.find('=', 3);
    if (eq == std::string_view::npos || eq <= 3)
        return false;
    const std::string_view key = text.substr(3, eq - 3);
    if (!plain_token(key) || key.size() >= sizeof field->key)
        return false;
    const std::size_t gt = text.find('>', eq + 1);
    if (gt == std::string_view::npos || gt <= eq + 1)
        return false;
    if (!decode_value(text.substr(eq + 1, gt - (eq + 1)), field->value, sizeof field->value))
        return false;
    std::memcpy(field->key, key.data(), key.size());
    field->key[key.size()] = '\0';
    field->type = type;
    *taken = gt + 1;
    return true;
}

bool same_key(const char* key, const char* name) {
    return std::strcmp(key, name) == 0;
}

bool parse_record(std::string_view text, ParsedBanner* out) {
    if (out == nullptr || text.size() < 4 || text[0] != '{' || text[1] != '[' || text.back() != '}')
        return false;
    const std::size_t close = text.find(']');
    if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != '}')
        return false;
    const std::size_t name_end = text.find('<', 2);
    if (name_end == std::string_view::npos || name_end <= 2)
        return false;
    if (text.substr(2, name_end - 2) != "Ads")
        return false;
    *out = {};
    out->weight = 1;
    out->pulses = k_ad_banner_period;
    bool saw_command = false;
    bool saw_file = false;
    char seen[16][32]{};
    int32_t seen_count = 0;
    std::string_view cursor = text.substr(name_end, close - name_end);
    while (!cursor.empty()) {
        Field field{};
        std::size_t taken = 0;
        if (!parse_field(cursor, &field, &taken) || taken > cursor.size())
            return false;
        for (int32_t seen_index = 0; seen_index < seen_count; ++seen_index) {
            if (same_key(seen[seen_index], field.key))
                return false;
        }
        if (seen_count >= 16)
            return false;
        std::memcpy(seen[seen_count], field.key, sizeof field.key);
        ++seen_count;
        if (same_key(field.key, "command")) {
            if (field.type != 'S' || !same_key(field.value, "banner"))
                return false;
            saw_command = true;
        } else if (same_key(field.key, "file")) {
            if (field.type != 'S' || field.value[0] == '\0')
                return false;
            if (!copy_fit(out->file, sizeof out->file, field.value))
                return false;
            saw_file = true;
        } else if (same_key(field.key, "link")) {
            if (field.type != 'S' || !copy_fit(out->link, sizeof out->link, field.value))
                return false;
        } else if (
            same_key(field.key, "id") || same_key(field.key, "seconds") ||
            same_key(field.key, "weight")
        ) {
            if (field.type != 'I' && field.type != 'U')
                return false;
            int32_t value = 0;
            if (!parse_i32(field.value, &value))
                return false;
            if (same_key(field.key, "id"))
                out->id = value;
            else if (same_key(field.key, "seconds")) {
                out->seconds = value;
                out->pulses = banner_interval_pulses(value);
            } else
                out->weight = value;
        }
        cursor.remove_prefix(taken);
    }
    return saw_command && saw_file;
}

void relative_of(const char* file, char* out, std::size_t capacity) {
    const char* start = file != nullptr ? file : "";
    while (*start == '/')
        ++start;
    if (!copy_fit(out, capacity, start) && capacity > 0)
        out[0] = '\0';
}

uint8_t nearest_index(const Palette& palette, int32_t red, int32_t green, int32_t blue) {
    int32_t best = -1;
    uint8_t best_index = 0;
    for (int32_t index = 0; index < OA_PALETTE_COLORS; ++index) {
        const PaletteEntry& entry = palette.entries[index];
        const int32_t dr = static_cast<int32_t>(entry.r) - red;
        const int32_t dg = static_cast<int32_t>(entry.g) - green;
        const int32_t db = static_cast<int32_t>(entry.b) - blue;
        const int32_t distance = dr * dr + dg * dg + db * db;
        if (best < 0 || distance < best) {
            best = distance;
            best_index = static_cast<uint8_t>(index);
        }
    }
    return best_index;
}

bool read_disk(const char* path, std::vector<uint8_t>* out) {
    if (path == nullptr || path[0] == '\0' || out == nullptr)
        return false;
    std::ifstream in(oa::platform::path_from_utf8(path), std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    constexpr std::streamoff k_byte_cap = 4 * 1024 * 1024;
    if (length < 0 || length > k_byte_cap)
        return false;
    in.seekg(0, std::ios::beg);
    out->assign(static_cast<std::size_t>(length), 0);
    if (length > 0)
        in.read(reinterpret_cast<char*>(out->data()), length);
    return static_cast<bool>(in);
}

void write_cache(const char* relative, const std::vector<uint8_t>& bytes) {
    if (relative == nullptr || bytes.empty())
        return;
    const std::string place = rover_session_write_path(relative);
    if (place.empty())
        return;
    const fs::path path = oa::platform::path_from_utf8(place);
    std::error_code error;
    if (fs::is_regular_file(path, error))
        return;
    const fs::path parent = path.parent_path();
    if (!parent.empty())
        fs::create_directories(parent, error);
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return;
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
}

bool take_png(const char* relative, std::vector<uint8_t>* out) {
    if (relative == nullptr || relative[0] == '\0' || out == nullptr)
        return false;
    const std::string place = rover_session_write_path(relative);
    std::error_code error;
    const bool had =
        !place.empty() && fs::is_regular_file(oa::platform::path_from_utf8(place), error);
    if (!rover_session_read_file(relative, out) || out->empty())
        return false;
    if (!had)
        write_cache(relative, *out);
    return true;
}

void release_reel(AdBanner* banner) {
    BannerReel* reel = banner->reel;
    if (reel == nullptr)
        return;
    for (int32_t index = 0; index < reel->unique_count; ++index) {
        image_release(reel->unique[index].image);
        reel->unique[index].image = k_no_handle;
    }
    delete reel;
    banner->reel = nullptr;
}

void restore_gaf(AdBanner* banner) {
    window_set_state_image(&banner->base, 1, banner->banners[0]);
    banner->next = 0;
    banner->countdown = k_ad_banner_period;
}

void banner_clear(AdBanner* banner) {
    release_reel(banner);
    banner->remote = false;
    banner->fetching = false;
    banner->gave_up = false;
    banner->waiting_kind = 0;
    banner->waiting_url[0] = '\0';
    banner->tried[0] = '\0';
}

bool map_into(AdBanner* banner, int32_t index, const std::vector<uint8_t>& bytes) {
    BannerReel* reel = banner->reel;
    ParsedSlot& slot = reel->parsed[index];
    std::vector<uint8_t> indices(
        static_cast<std::size_t>(k_ad_banner_width) * static_cast<std::size_t>(k_ad_banner_height)
    );
    const Palette* palette = banner->frame != nullptr && banner->frame->owner != nullptr
                                 ? banner->frame->owner->config.palette
                                 : nullptr;
    if (!banner_map_png(bytes, palette, indices.data()) ||
        reel->unique_count >= k_ad_banner_limit) {
        slot.unique = k_unique_failed;
        return false;
    }
    const Handle image = image_create_blank(
        k_ad_banner_width, k_ad_banner_height, resource::console_key, resource::console_key
    );
    Surface surface{};
    if (image < 0 || !image_surface(image, &surface) || surface.pixels == nullptr ||
        surface.pitch < k_ad_banner_width || surface.height < k_ad_banner_height) {
        if (image >= 0)
            image_release(image);
        slot.unique = k_unique_failed;
        return false;
    }
    for (int32_t row = 0; row < k_ad_banner_height; ++row) {
        std::memcpy(
            surface.pixels +
                static_cast<std::size_t>(row) * static_cast<std::size_t>(surface.pitch),
            indices.data() +
                static_cast<std::size_t>(row) * static_cast<std::size_t>(k_ad_banner_width),
            static_cast<std::size_t>(k_ad_banner_width)
        );
    }
    const int32_t slot_index = reel->unique_count;
    UniqueSlot& unique = reel->unique[slot_index];
    unique.image = image;
    unique.id = slot.id;
    unique.pulses = slot.pulses;
    copy_fit(unique.link, sizeof unique.link, slot.link);
    slot.unique = slot_index;
    ++reel->unique_count;
    return true;
}

bool start_http(AdBanner* banner, const char* url, int32_t kind) {
    if (banner->frame == nullptr || banner->frame->owner == nullptr || url == nullptr)
        return false;
    BoneyardsService& service = banner->frame->owner->config.service;
    if (service.fetch_file == nullptr)
        return false;
    char dest[520];
    dest[0] = '\0';
    const ServiceResult result = service.fetch_file(service.user, url, dest, sizeof dest);
    if (result != ServiceResult::pending)
        return false;
    if (!copy_fit(banner->waiting_url, sizeof banner->waiting_url, url))
        return false;
    banner->fetching = true;
    banner->waiting_kind = kind;
    return true;
}

/// Copies the interface language code. False when it is empty or does not fit.
bool interface_language_code(char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return false;
    const std::string_view tag = oa::data::languages::interface_language().tag;
    if (tag.empty() || tag.size() + 1 > capacity)
        return false;
    std::memcpy(out, tag.data(), tag.size());
    out[tag.size()] = '\0';
    return true;
}

/// Sends one ad_event. A missing language code sends nothing.
void report_ad(int32_t banner_id, int32_t kind) {
    char lang[64];
    if (!interface_language_code(lang, sizeof lang))
        return;
    rover_session_report_ad_event(banner_id, kind, lang);
}

/// Sends one impression for the banner now showing in a rotation slot.
void report_showing(AdBanner* banner, int32_t slot) {
    if (banner == nullptr || banner->reel == nullptr || slot < 0 ||
        slot >= banner->reel->unique_count)
        return;
    report_ad(banner->reel->unique[slot].id, kAdEventImpression);
}

bool publish(AdBanner* banner) {
    BannerReel* reel = banner->reel;
    int32_t weights[k_ad_banner_limit];
    int32_t map[k_ad_banner_limit];
    int32_t loaded = 0;
    for (int32_t index = 0; index < reel->parsed_count; ++index) {
        if (reel->parsed[index].unique < 0)
            continue;
        weights[loaded] = reel->parsed[index].weight;
        map[loaded] = reel->parsed[index].unique;
        ++loaded;
    }
    if (loaded == 0) {
        release_reel(banner);
        banner->remote = false;
        banner->fetching = false;
        banner->gave_up = true;
        return false;
    }
    int32_t order[k_ad_banner_show_limit];
    int32_t order_count = 0;
    expand_banner_order(weights, loaded, order, k_ad_banner_show_limit, &order_count);
    if (order_count < 1) {
        release_reel(banner);
        banner->gave_up = true;
        return false;
    }
    reel->show_count = order_count;
    for (int32_t index = 0; index < order_count; ++index)
        reel->show[index] = map[order[index]];
    reel->shown = 0;
    reel->show_at = order_count > 1 ? 1 : 0;
    const int32_t first_slot = reel->show[0];
    const UniqueSlot& first = reel->unique[first_slot];
    window_set_state_image(&banner->base, 1, first.image);
    banner->countdown = first.pulses;
    banner->remote = true;
    banner->fetching = false;
    banner->gave_up = false;
    report_showing(banner, first_slot);
    return true;
}

bool fetch_pending(AdBanner* banner) {
    BannerReel* reel = banner->reel;
    if (reel == nullptr)
        return false;
    for (int32_t index = 0; index < reel->parsed_count; ++index) {
        ParsedSlot& slot = reel->parsed[index];
        if (slot.unique != k_unique_pending)
            continue;
        std::vector<uint8_t> bytes;
        if (slot.relative[0] != '\0' && take_png(slot.relative, &bytes)) {
            map_into(banner, index, bytes);
            continue;
        }
        char url[640];
        if (banner_file_url(banner->tried, slot.file, url, sizeof url) &&
            start_http(banner, url, k_wait_png))
            return false;
        slot.unique = k_unique_failed;
    }
    return publish(banner);
}

bool apply_catalogue(AdBanner* banner, const std::vector<uint8_t>& bytes) {
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ParsedBanner parsed[k_ad_banner_limit];
    int32_t count = 0;
    if (bytes.empty() || !parse_ads_def(text, parsed, k_ad_banner_limit, &count) || count < 1) {
        banner->gave_up = true;
        banner->fetching = false;
        return false;
    }
    auto* reel = new (std::nothrow) BannerReel{};
    if (reel == nullptr) {
        banner->gave_up = true;
        return false;
    }
    banner->reel = reel;
    reel->parsed_count = count;
    for (int32_t index = 0; index < count; ++index) {
        ParsedSlot& slot = reel->parsed[index];
        slot.id = parsed[index].id;
        slot.pulses = parsed[index].pulses;
        slot.weight = parsed[index].weight;
        slot.unique = k_unique_pending;
        copy_fit(slot.file, sizeof slot.file, parsed[index].file);
        copy_fit(slot.link, sizeof slot.link, parsed[index].link);
        // A file field that names another host is not a path under the player's folder.
        if (std::strstr(parsed[index].file, "://") != nullptr) {
            slot.unique = k_unique_failed;
            slot.relative[0] = '\0';
            continue;
        }
        relative_of(parsed[index].file, slot.relative, sizeof slot.relative);
    }
    return fetch_pending(banner);
}

bool banner_load(AdBanner* banner, const char* params) {
    std::vector<uint8_t> bytes;
    if (rover_session_read_file(k_catalogue_path, &bytes))
        return apply_catalogue(banner, bytes);
    char url[640];
    char lang[64];
    const char* language = interface_language_code(lang, sizeof lang) ? lang : nullptr;
    if (!banner_list_url(banner->tried, params, language, url, sizeof url) ||
        !start_http(banner, url, k_wait_catalogue)) {
        banner->gave_up = true;
        return false;
    }
    return false;
}

bool banner_consider(AdBanner* banner) {
    if (banner == nullptr || banner->frame == nullptr)
        return false;
    char url[sizeof banner->tried];
    char params[260];
    url[0] = '\0';
    params[0] = '\0';
    int32_t send_id = 0;
    const bool active = rover_session_ad_server(url, sizeof url, &send_id, params, sizeof params);
    if (!active) {
        if (!banner->remote && banner->reel == nullptr && !banner->fetching && !banner->gave_up &&
            banner->tried[0] == '\0')
            return false;
        const bool was_remote = banner->remote;
        banner_clear(banner);
        if (was_remote)
            restore_gaf(banner);
        return was_remote;
    }
    if (banner->tried[0] != '\0' && std::strcmp(banner->tried, url) == 0)
        return false;
    const bool was_remote = banner->remote;
    banner_clear(banner);
    if (was_remote)
        restore_gaf(banner);
    if (!copy_fit(banner->tried, sizeof banner->tried, url)) {
        banner->gave_up = true;
        return was_remote;
    }
    return banner_load(banner, params) || was_remote;
}

void banner_server_tick(AdBanner* banner) {
    BannerReel* reel = banner->reel;
    if (reel == nullptr || reel->show_count < 1)
        return;
    const int32_t remaining = banner->countdown--;
    if (remaining != 0)
        return;
    reel->shown = reel->show_at;
    ++reel->show_at;
    if (reel->show_at >= reel->show_count)
        reel->show_at = 0;
    const int32_t slot = reel->show[reel->shown];
    const UniqueSlot& unique = reel->unique[slot];
    window_set_state_image(&banner->base, 1, unique.image);
    banner->countdown = unique.pulses;
    report_showing(banner, slot);
}

void ad_banner_create(Window* self) {
    AdBanner* banner = as_banner(self);
    for (int32_t index = 0; index < k_banner_count; ++index) {
        banner->banners[index] = console_images_load(
            banner->images, k_banner_gaf, k_banner_sequence, index, resource::console_key
        );
    }
    window_set_state_image(self, 1, banner->banners[0]);
    banner->next = 0;
    banner->shown = 0;
    banner->countdown = k_ad_banner_period;
    banner_consider(banner);
}

void ad_banner_update(Window* self) {
    if (!window_is_shown(self))
        return;
    AdBanner* banner = as_banner(self);
    const bool was_remote = banner->remote;
    const bool changed = banner_consider(banner);
    if (banner->remote) {
        if (was_remote && !changed)
            banner_server_tick(banner);
        return;
    }
    if (was_remote || changed)
        return;
    const int32_t remaining = banner->countdown--;
    if (remaining != 0)
        return;
    banner->shown = banner->next;
    window_set_state_image(self, 1, banner->banners[banner->shown]);
    banner->countdown = k_ad_banner_period;
    banner->next = (banner->next + 1) % k_banner_count;
}

void ad_banner_destroy(Window* self) {
    AdBanner* banner = as_banner(self);
    release_reel(banner);
    for (Handle& image : banner->banners) {
        image_release(image);
        image = k_no_handle;
    }
    console_window_free(self);
}

bool ad_banner_press(Window* self, const Message*) {
    AdBanner* banner = as_banner(self);
    if (banner == nullptr || !banner->remote || banner->reel == nullptr ||
        banner->reel->show_count < 1)
        return false;
    const int32_t which = banner->reel->show[banner->reel->shown];
    if (which < 0 || which >= banner->reel->unique_count)
        return false;
    const UniqueSlot& unique = banner->reel->unique[which];
    if (unique.link[0] == '\0' || !screens::registration_browser_available())
        return false;
    report_ad(unique.id, kAdEventClick);
    screens::registration_open_address(unique.link);
    return true;
}

WindowOps make_banner_ops() {
    WindowOps ops = console_panel_ops();
    ops.destroy = ad_banner_destroy;
    ops.on_bounds = ad_banner_create;
    ops.pulse = ad_banner_update;
    ops.left_up = ad_banner_press;
    return ops;
}

const WindowOps k_banner_ops = make_banner_ops();

} // namespace

int32_t banner_interval_pulses(int32_t seconds) {
    if (seconds < 1)
        return k_ad_banner_period;
    const int64_t pulses = static_cast<int64_t>(seconds) * 30;
    if (pulses > static_cast<int64_t>(INT32_MAX))
        return INT32_MAX;
    return static_cast<int32_t>(pulses);
}

bool parse_ads_def(std::string_view text, ParsedBanner* out, int32_t capacity, int32_t* count) {
    if (count == nullptr || capacity < 0 || (capacity > 0 && out == nullptr))
        return false;
    *count = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        if (text[index] != '{') {
            ++index;
            continue;
        }
        const std::size_t end = text.find('}', index);
        if (end == std::string_view::npos)
            return false;
        ParsedBanner banner{};
        if (!parse_record(text.substr(index, end - index + 1), &banner))
            return false;
        if (*count >= capacity)
            return false;
        out[*count] = banner;
        *count += 1;
        index = end + 1;
    }
    return true;
}

void expand_banner_order(
    const int32_t* weights,
    int32_t count,
    int32_t* order,
    int32_t order_capacity,
    int32_t* order_count
) {
    if (order_count == nullptr)
        return;
    *order_count = 0;
    if (weights == nullptr || order == nullptr || count < 1 || order_capacity < 1)
        return;
    const int32_t used = count > k_ad_banner_limit ? k_ad_banner_limit : count;
    int32_t remaining_weight[k_ad_banner_limit];
    for (int32_t index = 0; index < used; ++index) {
        int32_t weight = weights[index];
        if (weight < 1)
            weight = 1;
        if (weight > k_ad_banner_show_limit)
            weight = k_ad_banner_show_limit;
        remaining_weight[index] = weight;
    }
    while (*order_count < order_capacity) {
        bool any = false;
        for (int32_t index = 0; index < used; ++index) {
            if (remaining_weight[index] < 1)
                continue;
            order[*order_count] = index;
            *order_count += 1;
            --remaining_weight[index];
            any = true;
            if (*order_count >= order_capacity)
                break;
        }
        if (!any)
            break;
    }
}

namespace {

/// Copies the query of an ad_server url, without the '?'. An absent query is empty.
bool url_query(const char* url, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return false;
    out[0] = '\0';
    if (url == nullptr)
        return true;
    const char* query = std::strchr(url, '?');
    if (query == nullptr || query[1] == '\0')
        return true;
    ++query;
    const char* end = std::strchr(query, '#');
    const std::size_t length =
        end != nullptr ? static_cast<std::size_t>(end - query) : std::strlen(query);
    if (length + 1 > capacity)
        return false;
    std::memcpy(out, query, length);
    out[length] = '\0';
    return true;
}

} // namespace

bool banner_list_url(
    const char* url, const char* params, const char* lang, char* out, std::size_t capacity
) {
    if (!banner_origin(url, out, capacity) || !append_text(out, capacity, k_catalogue_suffix))
        return false;
    char query[640];
    if (!url_query(url, query, sizeof query))
        return false;
    const bool bare_ampersand = (params != nullptr && params[0] == '&') && query[0] == '\0' &&
                                (lang == nullptr || lang[0] == '\0');
    if (bare_ampersand)
        return append_text(out, capacity, params);
    if (params != nullptr && params[0] != '\0') {
        const char* extra = params;
        if (extra[0] == '?' || extra[0] == '&')
            ++extra;
        if (extra[0] != '\0') {
            if (query[0] != '\0' && !append_text(query, sizeof query, "&"))
                return false;
            if (!append_text(query, sizeof query, extra))
                return false;
        }
    }
    if (lang != nullptr && lang[0] != '\0') {
        if (query[0] != '\0' && !append_text(query, sizeof query, "&"))
            return false;
        if (!append_text(query, sizeof query, "lang=") || !append_text(query, sizeof query, lang))
            return false;
    }
    if (query[0] == '\0')
        return true;
    if (!append_text(out, capacity, "?"))
        return false;
    return append_text(out, capacity, query);
}

bool banner_file_url(const char* url, const char* file, char* out, std::size_t capacity) {
    if (file == nullptr || file[0] == '\0' || !banner_origin(url, out, capacity))
        return false;
    if (file[0] != '/' && !append_text(out, capacity, "/"))
        return false;
    return append_text(out, capacity, file);
}

bool banner_map_png(std::span<const uint8_t> bytes, const oa::Palette* palette, uint8_t* indices) {
    if (indices == nullptr || !formats::png::has_signature(bytes))
        return false;
    const formats::png::Messages messages{};
    formats::png::Info info;
    if (!formats::png::read_info(bytes, messages, &info))
        return false;
    const formats::png::Header header = formats::png::header_of(info, messages);
    if (header.width != static_cast<uint32_t>(k_ad_banner_width) ||
        header.height != static_cast<uint32_t>(k_ad_banner_height))
        return false;
    const bool indexed = header.color_type == formats::png::ColorType::palette;
    const bool grey = header.color_type == formats::png::ColorType::grey ||
                      header.color_type == formats::png::ColorType::grey_alpha;
    const bool colour = header.color_type == formats::png::ColorType::rgb ||
                        header.color_type == formats::png::ColorType::rgb_alpha;
    if ((!indexed && !grey && !colour) || palette == nullptr)
        return false;
    constexpr formats::png::Transforms transforms{true, true};
    const std::size_t row_length = formats::png::row_bytes(header, transforms);
    const uint32_t channels = formats::png::channel_count(header.color_type);
    if (channels == 0 || row_length < static_cast<std::size_t>(header.width) * channels ||
        row_length > 8192)
        return false;
    std::vector<uint8_t> rows(row_length * header.height);
    if (formats::png::read_image(bytes, info, transforms, messages, rows) ==
        formats::png::Progress::none)
        return false;
    const int32_t grey_top = header.bit_depth < 8 ? (1 << header.bit_depth) - 1 : 255;
    // One lobby index per file-palette entry. A file painted in the lobby palette therefore keeps
    // each colour, whichever index the file used for it.
    uint8_t table[256]{};
    if (indexed) {
        const int32_t entries = static_cast<int32_t>(info.palette_entries);
        const int32_t count = entries > 256 ? 256 : entries;
        for (int32_t entry = 0; entry < count; ++entry) {
            const formats::png::Rgb& swatch = info.palette[entry];
            table[entry] = nearest_index(
                *palette,
                static_cast<int32_t>(swatch.r),
                static_cast<int32_t>(swatch.g),
                static_cast<int32_t>(swatch.b)
            );
        }
    }
    for (int32_t y = 0; y < k_ad_banner_height; ++y) {
        const uint8_t* row = rows.data() + static_cast<std::size_t>(y) * row_length;
        for (int32_t x = 0; x < k_ad_banner_width; ++x) {
            const uint8_t* sample = row + static_cast<std::size_t>(x) * channels;
            uint8_t index = 0;
            if (colour) {
                index = nearest_index(
                    *palette,
                    static_cast<int32_t>(sample[0]),
                    static_cast<int32_t>(sample[1]),
                    static_cast<int32_t>(sample[2])
                );
            } else if (indexed) {
                index = table[sample[0]];
            } else {
                // A 1, 2 or 4-bit grey sample arrives unscaled; spread it over 0 to 255.
                const int32_t level = static_cast<int32_t>(sample[0]) * 255 / grey_top;
                index = nearest_index(*palette, level, level, level);
            }
            indices
                [static_cast<std::size_t>(y) * static_cast<std::size_t>(k_ad_banner_width) +
                 static_cast<std::size_t>(x)] = index;
        }
    }
    return true;
}

void ad_banner_shown_text(const AdBanner* banner, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';
    if (banner == nullptr)
        return;
    if (banner->remote && banner->reel != nullptr && banner->reel->show_count > 0) {
        const int32_t slot = banner->reel->show[banner->reel->shown];
        if (slot >= 0 && slot < banner->reel->unique_count) {
            std::snprintf(out, capacity, "%d", banner->reel->unique[slot].id);
            return;
        }
    }
    std::snprintf(out, capacity, "sample %d", banner->shown);
}

Window* make_ad_banner(MainFrame* frame, ConsoleImages* images, Window* parent) {
    auto* banner =
        reinterpret_cast<AdBanner*>(console_window_alloc(sizeof(AdBanner), &k_banner_ops));
    banner->frame = frame;
    banner->images = images;
    window_create(
        &banner->base,
        k_banner_name,
        main_frame_layout::banner_x,
        main_frame_layout::banner_y,
        main_frame_layout::banner_width,
        main_frame_layout::banner_height,
        parent,
        true
    );
    return &banner->base;
}

bool ad_banner_note_download(
    Window* banner_window, const char* url, const char* path, int32_t status
) {
    if (banner_window == nullptr)
        return false;
    AdBanner* banner = as_banner(banner_window);
    if (!banner->fetching || banner->waiting_url[0] == '\0' || url == nullptr ||
        std::strcmp(url, banner->waiting_url) != 0)
        return false;
    const int32_t kind = banner->waiting_kind;
    banner->fetching = false;
    banner->waiting_kind = 0;
    banner->waiting_url[0] = '\0';
    std::vector<uint8_t> bytes;
    if (status != kFetchFailed && path != nullptr && path[0] != '\0')
        read_disk(path, &bytes);
    if (path != nullptr && std::strstr(path, k_download_mark) != nullptr)
        std::remove(path);
    if (kind == k_wait_catalogue) {
        if (bytes.empty()) {
            banner->gave_up = true;
            return true;
        }
        apply_catalogue(banner, bytes);
        return true;
    }
    if (banner->reel != nullptr) {
        for (int32_t index = 0; index < banner->reel->parsed_count; ++index) {
            ParsedSlot& slot = banner->reel->parsed[index];
            if (slot.unique != k_unique_pending)
                continue;
            if (bytes.empty() || !map_into(banner, index, bytes))
                slot.unique = k_unique_failed;
            break;
        }
        fetch_pending(banner);
    }
    return true;
}

} // namespace oa::boneyards::console
