// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The photo file name: refused characters become spaces, a collision is
// numbered, and a long path loses whole characters of the map.
#include "oa/present/photo/photo_file_name.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using oa::present::photo::LocalMoment;
using oa::present::photo::path_characters;
using oa::present::photo::photo_file_name;

constexpr LocalMoment kMoment{2026, 11, 5, 13, 12, 8};
constexpr std::size_t kRoom = 10000;

/// The name with the map left whole.
///
/// @param map the map
/// @param copy the photo of that second
/// @return the name
std::string named(std::string_view map, int copy) {
    return photo_file_name(kMoment, map, copy, 0, kRoom);
}

/// True when every byte of `text` begins a well-formed UTF-8 sequence.
///
/// @param text the name
/// @return true when no sequence is cut
bool whole_utf8(std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        const auto lead = static_cast<unsigned char>(text[at]);
        std::size_t need = 1;
        if (lead < 0x80)
            need = 1;
        else if ((lead & 0xe0) == 0xc0 && lead >= 0xc2)
            need = 2;
        else if ((lead & 0xf0) == 0xe0)
            need = 3;
        else if ((lead & 0xf8) == 0xf0 && lead <= 0xf4)
            need = 4;
        else
            return false;
        if (at + need > text.size())
            return false;
        for (std::size_t index = 1; index < need; ++index) {
            if ((static_cast<unsigned char>(text[at + index]) & 0xc0) != 0x80)
                return false;
        }
        at += need;
    }
    return true;
}

} // namespace

int main() {
    CHECK(named("Canal Crossing", 1) == "Photo 2026-11-05 13.12.08 Canal Crossing.png");
    CHECK(named("Canal Crossing", 2) == "Photo 2026-11-05 13.12.08 Canal Crossing 2.png");
    CHECK(named("Canal Crossing", 3) == "Photo 2026-11-05 13.12.08 Canal Crossing 3.png");
    CHECK(named("", 1) == "Photo 2026-11-05 13.12.08.png");
    CHECK(named("", 2) == "Photo 2026-11-05 13.12.08 2.png");

    CHECK(named("a<b>c:d\"e/f\\g|h?i*j", 1) == "Photo 2026-11-05 13.12.08 a b c d e f g h i j.png");
    CHECK(
        named(
            std::string(
                "a\x01"
                "b\x7f"
                "c"
            ),
            1
        ) == "Photo 2026-11-05 13.12.08 a b c.png"
    );

    CHECK(named("Canal.", 1) == "Photo 2026-11-05 13.12.08 Canal .png");
    CHECK(named("Canal ", 1) == "Photo 2026-11-05 13.12.08 Canal .png");
    CHECK(named("Canal...", 1) == std::string("Photo 2026-11-05 13.12.08 Canal") + "   " + ".png");

    const std::string cafe = named("Caf\xc3\xa9 Crossing", 1);
    CHECK(cafe == "Photo 2026-11-05 13.12.08 Caf\xc3\xa9 Crossing.png");
    CHECK(whole_utf8(cafe));

    // The budget is one separator plus the count of the name that should
    // remain. "é" is two UTF-8 bytes and one UTF-16 unit, so a budget taken
    // from the whole name drops a different number of marks on each rule.
    const std::string marks = "\xc3\xa9\xc3\xa9\xc3\xa9";
    const std::string whole = named(marks, 1);
    const std::string two_marks = "Photo 2026-11-05 13.12.08 \xc3\xa9\xc3\xa9.png";
    CHECK(whole_utf8(whole));
    const std::string shortened =
        photo_file_name(kMoment, marks, 1, 0, 1 + path_characters(two_marks));
    CHECK(shortened.ends_with(".png"));
    CHECK(whole_utf8(shortened));
    CHECK(shortened == two_marks);
    CHECK(path_characters(whole) == path_characters(shortened) + path_characters("\xc3\xa9"));

    const std::string grin = std::string("Go") + "\xf0\x9f\x98\x80";
    const std::string with_grin = named(grin, 1);
    const std::string grin_kept = "Photo 2026-11-05 13.12.08 Go.png";
    const std::string grin_cut =
        photo_file_name(kMoment, grin, 1, 0, 1 + path_characters(grin_kept));
    CHECK(grin_cut == grin_kept);
    CHECK(whole_utf8(grin_cut));
    CHECK(
        path_characters(with_grin) ==
        path_characters(grin_cut) + path_characters("\xf0\x9f\x98\x80")
    );

    const std::string crossi = "Photo 2026-11-05 13.12.08 Canal Crossi 2.png";
    const std::string tight =
        photo_file_name(kMoment, "Canal Crossing", 2, 0, 1 + path_characters(crossi));
    CHECK(tight.ends_with(" 2.png"));
    CHECK(whole_utf8(tight));
    CHECK(tight == crossi);

    const std::string bare = "Photo 2026-11-05 13.12.08 2.png";
    const std::string no_map =
        photo_file_name(kMoment, "Canal Crossing", 2, 8, 8 + 1 + path_characters(bare));
    CHECK(no_map == bare);
    CHECK(no_map.ends_with(" 2.png"));

    if (failures != 0) {
        std::fprintf(stderr, "photo names: %d failed\n", failures);
        return 1;
    }
    std::printf("photo names: ok\n");
    return 0;
}
