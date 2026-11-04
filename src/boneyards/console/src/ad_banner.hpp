// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Main Frame's banner strip: the local sample frames, or the ad server's list.
#pragma once

#include "oa/present/surface.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace oa::boneyards {
struct Window;
}

namespace oa::boneyards::console {

struct MainFrame;
struct ConsoleImages;

// The strip's place and the original rotation, 900 pulses.
inline constexpr int32_t k_ad_banner_width = 468;
inline constexpr int32_t k_ad_banner_height = 60;
inline constexpr int32_t k_ad_banner_period = 900;
// How many banners a list may name, and how many showings a weighted list expands to.
inline constexpr int32_t k_ad_banner_limit = 64;
inline constexpr int32_t k_ad_banner_show_limit = 256;

// One banner from ads.def, in file order. seconds is 0 when the record omitted it.
struct ParsedBanner {
    int32_t id{};
    int32_t seconds{};
    int32_t weight{};
    int32_t pulses{};
    char file[160]{};
    char link[512]{};
};

/// Reads an ads.def body.
///
/// An empty body is no banners and returns true with a count of 0. One bad record rejects the
/// whole file. At most `capacity` banners are accepted; a further one rejects the file.
///
/// @param text file bytes, not necessarily terminated
/// @param[out] out banners in file order
/// @param capacity entries available in out
/// @param[out] count banners written
/// @return false when the file is not a banner list
bool parse_ads_def(std::string_view text, ParsedBanner* out, int32_t capacity, int32_t* count);

/// Pulses a banner stays up. Fewer than 1 second, and a missing count, stay up for 900 pulses.
///
/// @param seconds seconds named by the record; less than 1 uses the original period
/// @return pulses, never more than a 32-bit count can hold
int32_t banner_interval_pulses(int32_t seconds);

/// Expands weights into a show order.
///
/// File order, one pass at a time, while any banner still has showings left. A weight below 1
/// counts as 1, and a weight above 256 counts as 256. The result stops at `order_capacity`.
///
/// @param weights one weight per banner, in file order
/// @param count banners
/// @param[out] order indices into the banner list
/// @param order_capacity entries available in order
/// @param[out] order_count entries written
void expand_banner_order(
    const int32_t* weights,
    int32_t count,
    int32_t* order,
    int32_t order_capacity,
    int32_t* order_count
);

/// Builds the catalogue URL for an ad_server url.
///
/// The host and port are taken from `url` (port 80 when it names none) and the path is always
/// `/boneyards/ta/ads/ads.def`. The scheme written is http. A query already on `url` is kept, then
/// `params`, then `lang` when a language code is given.
///
/// @param url ad_server url
/// @param params query text; null or empty adds none. A text that does not start with '?' or '&'
///     gains a leading '?' when the url has no query of its own. A text that starts with '&' and
///     is the only query is appended with that ampersand
/// @param lang interface language code appended as `lang=`; null or empty adds none
/// @param[out] out buffer
/// @param capacity bytes available in out, including the terminator
/// @return false when the url has no host, its port is not from 1 to 65535, or the result does not
///     fit
bool banner_list_url(
    const char* url, const char* params, const char* lang, char* out, std::size_t capacity
);

/// Builds the URL of one banner file on the same host.
///
/// @param url ad_server url
/// @param file path from the banner's file field, with or without a leading slash
/// @param[out] out buffer
/// @param capacity bytes available in out, including the terminator
/// @return false when the url cannot be used or the result does not fit
bool banner_file_url(const char* url, const char* file, char* out, std::size_t capacity);

/// Maps a banner PNG onto lobby-palette indices, top row first.
///
/// The picture must be 468 by 60. An indexed file's palette, a grey sample and a colour sample are
/// each matched to the nearest lobby colour. Alpha is ignored. Any of these pictures needs the
/// lobby palette.
///
/// @param bytes PNG file
/// @param palette lobby palette; required
/// @param[out] indices 468 by 60 indices, one row after another
/// @return false when the file is not that picture, or the palette is missing
bool banner_map_png(std::span<const uint8_t> bytes, const oa::Palette* palette, uint8_t* indices);

/// Creates the banner strip and parents it on the Main Frame.
///
/// @param frame Main Frame that owns the strip
/// @param images image source for the local sample frames
/// @param parent parent window, the Main Frame
/// @return the strip's window
Window* make_ad_banner(MainFrame* frame, ConsoleImages* images, Window* parent);

/// Claims a downloaded file when the strip asked for it.
///
/// The bytes are read and a temporary download is deleted. Any other url is left for the contents
/// console.
///
/// @param banner the strip's window
/// @param url url the fetch named
/// @param path file the fetch wrote; null when it failed before that
/// @param status fetch status; kFetchFailed when the fetch failed
/// @return true when this strip asked for the url
bool ad_banner_note_download(Window* banner, const char* url, const char* path, int32_t status);

} // namespace oa::boneyards::console
