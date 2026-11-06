// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Face map: the GDI faces the game names and the bundled fonts that
// stand in for them.

#include <cctype>

#include "oa/platform/fonts.hpp"

namespace oa::platform {
namespace {

constexpr const char* k_liberation_licence = "liberation/LICENSE";
constexpr const char* k_liberation_sans_regular = "liberation/LiberationSans-Regular.ttf";
constexpr const char* k_liberation_sans_bold = "liberation/LiberationSans-Bold.ttf";
constexpr const char* k_liberation_sans_italic = "liberation/LiberationSans-Italic.ttf";
constexpr const char* k_liberation_sans_bold_italic = "liberation/LiberationSans-BoldItalic.ttf";
constexpr const char* k_liberation_serif_regular = "liberation/LiberationSerif-Regular.ttf";
constexpr const char* k_liberation_serif_bold = "liberation/LiberationSerif-Bold.ttf";
constexpr const char* k_liberation_serif_italic = "liberation/LiberationSerif-Italic.ttf";
constexpr const char* k_liberation_serif_bold_italic = "liberation/LiberationSerif-BoldItalic.ttf";
constexpr const char* k_saira_licence = "saira-extracondensed/OFL.txt";
constexpr const char* k_saira_extra_bold = "saira-extracondensed/SairaExtraCondensed-ExtraBold.ttf";

constexpr const char* k_bundled_files[] = {
    k_liberation_sans_regular,
    k_liberation_sans_bold,
    k_liberation_sans_italic,
    k_liberation_sans_bold_italic,
    k_liberation_serif_regular,
    k_liberation_serif_bold,
    k_liberation_serif_italic,
    k_liberation_serif_bold_italic,
    k_liberation_licence,
    k_saira_extra_bold,
    k_saira_licence,
};

// Style slots, indexed by (bold ? 1 : 0) | (italic ? 2 : 0).
constexpr int32_t k_style_bold = 1;
constexpr int32_t k_style_italic = 2;
constexpr int32_t k_style_count = 4;

struct FontFamily {
    const char* name{};
    const char* files[k_style_count]{}; // null: the family lacks that style
};

// Metric-compatible with Arial: the same advances and vertical extents.
constexpr FontFamily k_liberation_sans = {
    "Liberation Sans",
    {k_liberation_sans_regular,
     k_liberation_sans_bold,
     k_liberation_sans_italic,
     k_liberation_sans_bold_italic},
};

// A condensed heavy grotesque of Haettenschweiler's proportions, set at the
// same em; its one weight stands in for Haettenschweiler's only style.
constexpr FontFamily k_saira_extra_condensed_extra_bold = {
    "Saira ExtraCondensed ExtraBold",
    {k_saira_extra_bold, nullptr, nullptr, nullptr},
};

// The serif of the pages' Times New Roman. Its own vertical metrics are
// kept: the pages do not pin Times' device rows.
constexpr FontFamily k_liberation_serif = {
    "Liberation Serif",
    {k_liberation_serif_regular,
     k_liberation_serif_bold,
     k_liberation_serif_italic,
     k_liberation_serif_bold_italic},
};

// tmAscent and tmDescent GDI reports for the Windows faces at 8..32
// pixels per em, which it takes from their VDMX records (Arial 5.01 and
// Haettenschweiler 0.80); the stand-ins have no VDMX and their hinted
// extents differ by a pixel at some sizes.
constexpr int32_t k_vdmx_first_em = 8;
constexpr int32_t k_vdmx_count = 25;

constexpr uint8_t k_arial_regular_rows[k_vdmx_count][2] = {
    {8, 2},  {9, 3},  {10, 3}, {11, 3}, {12, 3}, {13, 3}, {13, 3}, {14, 3}, {15, 3},
    {15, 4}, {17, 4}, {18, 4}, {19, 4}, {19, 5}, {20, 5}, {21, 5}, {21, 6}, {23, 5},
    {25, 6}, {26, 6}, {26, 6}, {27, 6}, {28, 7}, {28, 7}, {29, 7},
};
constexpr uint8_t k_arial_bold_rows[k_vdmx_count][2] = {
    {8, 2},  {9, 2},  {10, 2}, {11, 3}, {12, 3}, {13, 3}, {13, 3}, {14, 4}, {15, 4},
    {15, 4}, {17, 4}, {18, 4}, {19, 5}, {19, 5}, {21, 5}, {22, 5}, {23, 6}, {24, 6},
    {24, 6}, {25, 7}, {26, 7}, {27, 7}, {28, 7}, {29, 7}, {30, 7},
};
constexpr uint8_t k_arial_italic_rows[k_vdmx_count][2] = {
    {8, 3},  {9, 3},  {10, 3}, {11, 3}, {12, 3}, {13, 3}, {13, 3}, {14, 3}, {16, 3},
    {16, 4}, {17, 5}, {18, 5}, {19, 5}, {19, 5}, {20, 6}, {22, 6}, {22, 6}, {23, 6},
    {24, 6}, {25, 6}, {25, 6}, {27, 7}, {28, 7}, {29, 7}, {29, 8},
};
constexpr uint8_t k_arial_bold_italic_rows[k_vdmx_count][2] = {
    {8, 2},  {9, 2},  {9, 2},  {11, 2}, {12, 3}, {13, 3}, {13, 3}, {14, 3}, {15, 3},
    {15, 4}, {17, 4}, {18, 5}, {19, 5}, {19, 5}, {20, 5}, {21, 5}, {22, 6}, {23, 5},
    {24, 6}, {26, 6}, {26, 6}, {27, 7}, {27, 7}, {28, 7}, {29, 7},
};
constexpr uint8_t k_haettenschweiler_rows[k_vdmx_count][2] = {
    {10, 4}, {10, 4}, {11, 4}, {12, 4}, {12, 4}, {13, 4}, {14, 4}, {15, 4}, {15, 4},
    {16, 4}, {17, 4}, {18, 4}, {19, 4}, {20, 4}, {20, 4}, {21, 4}, {23, 4}, {24, 4},
    {24, 4}, {25, 4}, {26, 4}, {26, 5}, {28, 5}, {29, 5}, {29, 5},
};

// Outside the table GDI scales the OS/2 Windows ascent and descent.
constexpr int32_t k_arial_units_per_em = 2048;
constexpr int32_t k_arial_win_ascent = 1854;
constexpr int32_t k_arial_win_descent = 434;
constexpr int32_t k_haettenschweiler_units_per_em = 1000;
constexpr int32_t k_haettenschweiler_win_ascent = 920;
constexpr int32_t k_haettenschweiler_win_descent = 120;

constexpr LineMetricsTable arial_table(const uint8_t (*rows)[2]) {
    return LineMetricsTable{
        k_arial_units_per_em,
        k_arial_win_ascent,
        k_arial_win_descent,
        k_vdmx_first_em,
        k_vdmx_count,
        rows,
    };
}

constexpr LineMetricsTable k_arial_regular = arial_table(k_arial_regular_rows);
constexpr LineMetricsTable k_arial_bold = arial_table(k_arial_bold_rows);
constexpr LineMetricsTable k_arial_italic = arial_table(k_arial_italic_rows);
constexpr LineMetricsTable k_arial_bold_italic = arial_table(k_arial_bold_italic_rows);
constexpr LineMetricsTable k_haettenschweiler = {
    k_haettenschweiler_units_per_em,
    k_haettenschweiler_win_ascent,
    k_haettenschweiler_win_descent,
    k_vdmx_first_em,
    k_vdmx_count,
    k_haettenschweiler_rows,
};

struct FaceEntry {
    const char* face{};
    const FontFamily* family{};
    const LineMetricsTable* line_metrics[k_style_count]{}; // null: the family's own
};

// ARIAL is the markup default and the NDA headings' face; Haettenschweiler
// the contents-list placeholder's; MS Sans Serif the dialog templates'. Each
// bundled family also answers to its own name.
constexpr FaceEntry k_face_map[] = {
    {"Arial",
     &k_liberation_sans,
     {&k_arial_regular, &k_arial_bold, &k_arial_italic, &k_arial_bold_italic}},
    {"Haettenschweiler",
     &k_saira_extra_condensed_extra_bold,
     {&k_haettenschweiler, &k_haettenschweiler, &k_haettenschweiler, &k_haettenschweiler}},
    {"MS Sans Serif", &k_liberation_sans, {}},
    {"Liberation Sans", &k_liberation_sans, {}},
    {"Times New Roman", &k_liberation_serif, {}},
    {"Times", &k_liberation_serif, {}},
    {"Liberation Serif", &k_liberation_serif, {}},
    {"Tinos", &k_liberation_serif, {}},
    {"Saira ExtraCondensed ExtraBold", &k_saira_extra_condensed_extra_bold, {}},
};

// Case-insensitive over the characters a LOGFONT face name holds. The loop
// stops at the first difference, so it never reads past either terminator.
bool same_face(const char* a, const char* b) {
    for (int32_t i = 0; i < k_face_name_chars; ++i) {
        const int ca = std::toupper(static_cast<unsigned char>(a[i]));
        const int cb = std::toupper(static_cast<unsigned char>(b[i]));
        if (ca != cb)
            return false;
        if (ca == 0)
            return true;
    }
    return true;
}

} // namespace

bool face_resolve(const char* face, int32_t weight, bool italic, FaceMatch* out) {
    if (face == nullptr)
        return false;
    for (const FaceEntry& entry : k_face_map) {
        if (!same_face(face, entry.face))
            continue;
        const bool bold = weight >= k_weight_bold_threshold;
        const int32_t style = (bold ? k_style_bold : 0) | (italic ? k_style_italic : 0);
        // The nearest style the family has; what it lacks is simulated.
        // Every family has a regular file.
        const int32_t candidates[] = {style, style & ~k_style_bold, style & ~k_style_italic, 0};
        int32_t chosen = 0;
        for (const int32_t candidate : candidates)
            if (entry.family->files[candidate] != nullptr) {
                chosen = candidate;
                break;
            }
        *out = FaceMatch{
            entry.face,
            entry.family->name,
            entry.family->files[chosen],
            bold && (chosen & k_style_bold) == 0,
            italic && (chosen & k_style_italic) == 0,
            entry.line_metrics[style],
        };
        return true;
    }
    return false;
}

int32_t bundled_font_files(const char* const** out) {
    *out = k_bundled_files;
    return static_cast<int32_t>(sizeof k_bundled_files / sizeof k_bundled_files[0]);
}

} // namespace oa::platform
