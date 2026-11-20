// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The modern text an extension reads, and the focused field it hands the
// engine: the message log's face at a text size, a face's rows at that
// size, the text settings written and read back, and the field's place
// through a stub.
#include "oa/app/extension.hpp"
#include "text_input_area.hpp"

#include "oa/base/threads.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/present/text_style.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/engine_settings.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <vector>

namespace {

namespace settings = oa::ui::engine_settings;

// A face's value never moves: a face added later takes the next one, whatever
// place the chain looks in it.
static_assert(static_cast<int>(oa::app::ModernTextFace::sans_bold) == 0);
static_assert(static_cast<int>(oa::app::ModernTextFace::sans) == 1);
static_assert(static_cast<int>(oa::app::ModernTextFace::endonyms) == 2);
static_assert(static_cast<int>(oa::app::ModernTextFace::emoji) == 3);
static_assert(static_cast<int>(oa::app::ModernTextFace::endonyms_bengali) == 4);
static_assert(oa::app::modern_text_face_count == 5);

/// What the stub was handed.
struct Recorded {
    int calls{};
    const void* context_seen{};
    bool cleared{};
    oa::app::TextInputArea area{};
    bool accept{true};
};

/// Records the area and returns the record's accept flag.
///
/// @param context the Recorded
/// @param area the area the entry handed over; null clears it
/// @return Recorded::accept
bool record_area(void* context, const oa::app::TextInputArea* area) {
    auto& recorded = *static_cast<Recorded*>(context);
    ++recorded.calls;
    recorded.context_seen = context;
    recorded.cleared = area == nullptr;
    if (area != nullptr)
        recorded.area = *area;
    return recorded.accept;
}

/// Returns a stub that records into `recorded`.
///
/// @param recorded the record
/// @return the target
oa::app::TextInputAreaTarget stub_target(Recorded& recorded) {
    oa::app::TextInputAreaTarget target{};
    target.context = &recorded;
    target.set_area = record_area;
    return target;
}

/// Rows of ink a drawn line stands.
int32_t ink_height(const oa::app::ModernTextPixels& line) {
    int32_t first = line.height;
    int32_t end = 0;
    for (int32_t row = 0; row < line.height; ++row) {
        for (int32_t column = 0; column < line.width; ++column) {
            if (line.coverage[static_cast<std::size_t>(row * line.width + column)] == 0)
                continue;
            first = std::min(first, row);
            end = std::max(end, row + 1);
        }
    }
    return std::max(end - first, 0);
}

/// The message log's face at the text sizes the game offers.
void message_log_face_follows_the_text_size() {
    const oa::app::ModernTextSize full = oa::app::message_log_text_size(100, 1, false);
    OA_CHECK(full.bold && full.pixel_size == 14 && full.least_cjk_pixel_size == 0);
    const oa::app::ModernTextSize smaller = oa::app::message_log_text_size(80, 1, true);
    OA_CHECK(smaller.bold && smaller.pixel_size == 11 && smaller.least_cjk_pixel_size == 12);
    const oa::app::ModernTextSize scaled = oa::app::message_log_text_size(150, 2, false);
    OA_CHECK(scaled.bold && scaled.pixel_size == 42 && scaled.least_cjk_pixel_size == 0);
    const oa::app::ModernTextSize held = oa::app::message_log_text_size(1000, 1, false);
    OA_CHECK(held.pixel_size == 42 && held.least_cjk_pixel_size == 0);
    const oa::app::ModernTextSize tiny = oa::app::message_log_text_size(50, 1, true);
    OA_CHECK(tiny.bold && tiny.pixel_size == 7 && tiny.least_cjk_pixel_size == 12);
}

/// One face of a chain, or null when the chain has no such entry.
const oa::app::ModernFaceMetrics*
chain_face(const oa::app::ModernTextChain& chain, oa::app::ModernTextFace face) {
    for (int32_t index = 0; index < chain.count; ++index)
        if (chain.faces[static_cast<std::size_t>(index)].face == face)
            return &chain.faces[static_cast<std::size_t>(index)];
    return nullptr;
}

/// The chain's rows are the greatest rows of its faces, the Bengali endonym
/// face apart: its descent is deeper than the others', and it never changes
/// the rows of a line.
void chain_rows_are_its_faces(const oa::app::ModernTextChain& chain) {
    int32_t ascent = 0;
    int32_t descent = 0;
    for (int32_t index = 0; index < chain.count; ++index) {
        const oa::app::ModernFaceMetrics& face = chain.faces[static_cast<std::size_t>(index)];
        if (face.face == oa::app::ModernTextFace::endonyms_bengali)
            continue;
        ascent = std::max(ascent, face.ascent);
        descent = std::max(descent, face.descent);
    }
    OA_CHECK(chain.ascent == ascent && chain.descent == descent);
}

/// The faces at the message log's size, in fallback order, and a regular line.
void chain_at_the_message_log_size() {
    const oa::app::ModernTextSize log = oa::app::message_log_text_size(100, 1, false);
    const auto chain = oa::app::modern_text_chain(log);
    OA_CHECK(chain.has_value());
    if (!chain)
        return;
    OA_CHECK(chain->count == oa::app::modern_text_face_count);
    OA_CHECK(chain->faces[0].face == oa::app::ModernTextFace::sans_bold);
    OA_CHECK(chain->faces[1].face == oa::app::ModernTextFace::sans);
    OA_CHECK(chain->faces[2].face == oa::app::ModernTextFace::endonyms);
    OA_CHECK(chain->faces[3].face == oa::app::ModernTextFace::endonyms_bengali);
    OA_CHECK(chain->faces[4].face == oa::app::ModernTextFace::emoji);
    OA_CHECK(chain->faces[0].pixel_size == 14 && chain->faces[1].pixel_size == 14);
    // The Bengali endonym face is drawn at the sans faces' size.
    OA_CHECK(chain->faces[2].pixel_size == 12 && chain->faces[3].pixel_size == 14);
    OA_CHECK(chain->faces[4].pixel_size == 12);
    // Rows as the fonts report them at this size. The line is the endonym
    // face's, which keeps the rows of the face it was cut from, and the
    // Bengali endonym face does not change it.
    OA_CHECK(chain->faces[0].ascent == 13 && chain->faces[0].descent == 4);
    OA_CHECK(chain->faces[2].ascent == 14 && chain->faces[2].descent == 4);
    OA_CHECK(chain->faces[4].ascent == 12 && chain->faces[4].descent == 3);
    OA_CHECK(chain->ascent == 14 && chain->descent == 4);
    chain_rows_are_its_faces(*chain);

    oa::app::ModernTextSize regular = log;
    regular.bold = false;
    const auto plain = oa::app::modern_text_chain(regular);
    OA_CHECK(plain.has_value());
    if (plain) {
        OA_CHECK(plain->count == 4);
        OA_CHECK(plain->faces[0].face == oa::app::ModernTextFace::sans);
        OA_CHECK(plain->faces[1].face == oa::app::ModernTextFace::endonyms);
        OA_CHECK(plain->faces[2].face == oa::app::ModernTextFace::endonyms_bengali);
        OA_CHECK(plain->faces[3].face == oa::app::ModernTextFace::emoji);
        OA_CHECK(plain->faces[4].pixel_size == 0 && plain->faces[4].ascent == 0);
        OA_CHECK(plain->faces[0].pixel_size == 14);
        OA_CHECK(plain->faces[0].ascent == 13 && plain->faces[0].descent == 4);
        OA_CHECK(plain->faces[1].pixel_size == 12 && plain->faces[2].pixel_size == 14);
        OA_CHECK(plain->faces[3].pixel_size == 12);
        OA_CHECK(plain->ascent == 14 && plain->descent == 4);
        chain_rows_are_its_faces(*plain);
    }

    const oa::app::ModernTextSize cjk = oa::app::message_log_text_size(80, 1, true);
    const auto held = oa::app::modern_text_chain(cjk);
    OA_CHECK(held.has_value());
    if (held) {
        const oa::app::ModernFaceMetrics* sans =
            chain_face(*held, oa::app::ModernTextFace::sans_bold);
        const oa::app::ModernFaceMetrics* ideograph =
            chain_face(*held, oa::app::ModernTextFace::endonyms);
        const oa::app::ModernFaceMetrics* emoji = chain_face(*held, oa::app::ModernTextFace::emoji);
        OA_CHECK(sans && sans->pixel_size == 11 && sans->ascent == 11 && sans->descent == 3);
        OA_CHECK(ideograph && ideograph->pixel_size == 12);
        OA_CHECK(ideograph && ideograph->ascent == 14 && ideograph->descent == 4);
        OA_CHECK(emoji && emoji->pixel_size == 11);
        OA_CHECK(held->ascent == 14 && held->descent == 4);
        chain_rows_are_its_faces(*held);
    }

    oa::app::ModernTextSize none{};
    none.pixel_size = 0;
    OA_CHECK(!oa::app::modern_text_chain(none));
    none.pixel_size = 14;
    none.least_cjk_pixel_size = -1;
    OA_CHECK(!oa::app::modern_text_chain(none));
}

/// A mixed line comes from the bold sans, the endonym face and emoji, in that
/// order, and Bengali's letters from the Bengali endonym face.
void layout_follows_the_chain() {
    const oa::app::ModernTextSize log = oa::app::message_log_text_size(100, 1, false);
    const auto placed = oa::app::modern_text_layout("A\xE4\xB8\xAD\xF0\x9F\x9A\x80", log);
    OA_CHECK(placed && placed->size() == 3);
    if (placed && placed->size() == 3) {
        OA_CHECK((*placed)[0].character == U'A');
        OA_CHECK((*placed)[0].face == oa::app::ModernTextFace::sans_bold);
        OA_CHECK((*placed)[0].pen == 0 && (*placed)[0].advance > 0);
        OA_CHECK((*placed)[1].character == 0x4E2D);
        OA_CHECK((*placed)[1].face == oa::app::ModernTextFace::endonyms);
        OA_CHECK((*placed)[1].pen == (*placed)[0].advance);
        OA_CHECK((*placed)[2].character == 0x1F680);
        OA_CHECK((*placed)[2].face == oa::app::ModernTextFace::emoji);
        OA_CHECK((*placed)[2].pen == (*placed)[1].pen + (*placed)[1].advance);
    }
    // "বা": BA and the vowel sign AA, one syllable.
    const auto bengali = oa::app::modern_text_layout("\xE0\xA6\xAC\xE0\xA6\xBE", log);
    OA_CHECK(bengali && bengali->size() == 2);
    if (bengali && bengali->size() == 2) {
        OA_CHECK((*bengali)[0].character == 0x09AC && (*bengali)[1].character == 0x09BE);
        OA_CHECK((*bengali)[0].face == oa::app::ModernTextFace::endonyms_bengali);
        OA_CHECK((*bengali)[1].face == oa::app::ModernTextFace::endonyms_bengali);
    }
    oa::app::ModernTextSize regular = log;
    regular.bold = false;
    const auto letter = oa::app::modern_text_layout("A", regular);
    OA_CHECK(letter && letter->size() == 1);
    if (letter && letter->size() == 1)
        OA_CHECK((*letter)[0].face == oa::app::ModernTextFace::sans);
    OA_CHECK(!oa::app::modern_text_layout("\x80", log));
    OA_CHECK(!oa::app::modern_text_layout("\xC0\xAF", log));
    OA_CHECK(!oa::app::modern_text_pixels("\x80", log));
}

/// H at the message log's size is one bit a pixel and ten rows of ink.
void capital_matches_the_message_log() {
    const oa::app::ModernTextSize log = oa::app::message_log_text_size(100, 1, false);
    const auto chain = oa::app::modern_text_chain(log);
    const auto drawn = oa::app::modern_text_pixels("H", log);
    OA_CHECK(chain.has_value() && drawn.has_value());
    if (!chain || !drawn)
        return;
    OA_CHECK(drawn->height == chain->ascent + chain->descent);
    OA_CHECK(drawn->baseline == chain->ascent);
    OA_CHECK(
        drawn->coverage.size() ==
        static_cast<std::size_t>(drawn->width) * static_cast<std::size_t>(drawn->height)
    );
    OA_CHECK(std::all_of(drawn->coverage.begin(), drawn->coverage.end(), [](uint8_t coverage) {
        return coverage == 0 || coverage == 255;
    }));
    OA_CHECK(ink_height(*drawn) == 10);
}

/// Lines several threads draw at once: Latin, Greek, Cyrillic, CJK and an
/// emoji, so every face is used.
constexpr std::array<std::string_view, 4> kThreadLines{
    "Commanders queued",
    "\xCE\xA9\xCE\xBC\xCE\xAD\xCE\xB3\xCE\xB1 \xD0\x9A\xD0\xB8\xD1\x80",
    "\xE6\x9D\xB1\xE4\xBA\xAC",
    "Hi \xF0\x9F\x99\x82",
};

/// What one drawing thread is given and counts.
struct DrawingThread {
    int first{};
    const std::vector<std::optional<oa::app::ModernTextPixels>>* alone{};
    std::atomic<int>* mismatches{};
};

/// Lays out and draws the lines over and over, counting any that differ from
/// the same line drawn by one thread alone.
void draw_lines(void* argument) {
    constexpr int kRounds = 25;
    const auto& thread = *static_cast<const DrawingThread*>(argument);
    const oa::app::ModernTextSize log = oa::app::message_log_text_size(100, 1, false);
    for (int round = 0; round < kRounds; ++round) {
        const auto index = static_cast<std::size_t>(thread.first + round) % kThreadLines.size();
        const auto drawn = oa::app::modern_text_pixels(kThreadLines[index], log);
        const auto glyphs = oa::app::modern_text_layout(kThreadLines[index], log);
        const auto& expected = (*thread.alone)[index];
        const bool same =
            glyphs.has_value() && drawn.has_value() == expected.has_value() &&
            (!drawn || (drawn->width == expected->width && drawn->height == expected->height &&
                        drawn->coverage == expected->coverage));
        if (!same)
            thread.mismatches->fetch_add(1);
    }
}

/// Calls from several threads at once take turns with the faces: each line
/// lays out and draws as one thread alone draws it.
void threads_take_turns() {
    const oa::app::ModernTextSize log = oa::app::message_log_text_size(100, 1, false);
    std::vector<std::optional<oa::app::ModernTextPixels>> alone;
    for (const std::string_view line : kThreadLines)
        alone.push_back(oa::app::modern_text_pixels(line, log));
    OA_CHECK(std::all_of(alone.begin(), alone.end(), [](const auto& line) {
        return line.has_value();
    }));
    constexpr int kThreads = 4;
    std::atomic<int> mismatches{0};
    std::array<DrawingThread, kThreads> work{};
    std::array<oa::base::threads::Thread, kThreads> threads{};
    int started = 0;
    for (int index = 0; index < kThreads; ++index) {
        work[static_cast<std::size_t>(index)] = DrawingThread{index, &alone, &mismatches};
        if (oa::base::threads::start_thread(
                threads[static_cast<std::size_t>(index)],
                draw_lines,
                &work[static_cast<std::size_t>(index)]
            ))
            ++started;
    }
    for (auto& thread : threads)
        oa::base::threads::join_thread(thread);
    OA_CHECK(started == kThreads);
    OA_CHECK(mismatches.load() == 0);
}

/// A named preferences file's defaults, and a size written and read back.
void settings_read_back() {
    const settings::EngineSettings named = settings::default_settings({});
    OA_CHECK(!named.modern_fonts);
    OA_CHECK(named.text_size == oa::present::default_text_size);
    const oa::app::GameTextPreferences from_defaults =
        oa::app::game_text_preferences_of(named.modern_fonts, named.text_size);
    OA_CHECK(!from_defaults.modern_fonts && from_defaults.text_size == 80);

    settings::EngineSettings chosen = named;
    chosen.modern_fonts = true;
    chosen.text_size = 150;
    oa::platform::preferences::Values values;
    settings::write_settings(values, named, chosen, named, false);
    const settings::EngineSettings read = settings::read_settings(values, {}, false);
    OA_CHECK(read.modern_fonts && read.text_size == 150);
    const oa::app::GameTextPreferences back =
        oa::app::game_text_preferences_of(read.modern_fonts, read.text_size);
    OA_CHECK(back.modern_fonts && back.text_size == 150);
    OA_CHECK(oa::app::game_text_preferences_of(true, 10).text_size == 50);
    OA_CHECK(oa::app::game_text_preferences_of(false, 400).text_size == 300);
    OA_CHECK(oa::app::game_text_preferences_of(true, 80).text_size == 80);
}

/// The field reaches the stub in the canvas's own pixels, and an empty one clears it.
void field_reaches_the_stub() {
    Recorded recorded{};
    const oa::app::TextField field{10, 20, 100, 16, 6};
    OA_CHECK(oa::app::set_text_input_area_with(&field, nullptr, stub_target(recorded)));
    OA_CHECK(recorded.calls == 1 && recorded.context_seen == &recorded && !recorded.cleared);
    OA_CHECK(recorded.area.x == 10 && recorded.area.y == 20);
    OA_CHECK(recorded.area.width == 100 && recorded.area.height == 16);
    OA_CHECK(recorded.area.cursor == 6);

    const oa::app::TextField point{10, 20, 1, 1, 0};
    recorded = {};
    OA_CHECK(oa::app::set_text_input_area_with(&point, nullptr, stub_target(recorded)));
    OA_CHECK(recorded.area.width == 1 && recorded.area.height == 1 && recorded.area.cursor == 0);

    const oa::app::TextField past{10, 20, 100, 16, 200};
    recorded = {};
    OA_CHECK(oa::app::set_text_input_area_with(&past, nullptr, stub_target(recorded)));
    OA_CHECK(recorded.area.cursor == 200 && recorded.area.width == 100);

    recorded = {};
    OA_CHECK(oa::app::set_text_input_area_with(nullptr, nullptr, stub_target(recorded)));
    OA_CHECK(recorded.calls == 1 && recorded.cleared);

    const oa::app::TextField flat{10, 20, 0, 16, 0};
    recorded = {};
    OA_CHECK(oa::app::set_text_input_area_with(&flat, nullptr, stub_target(recorded)));
    OA_CHECK(recorded.calls == 1 && recorded.cleared);
    const oa::app::TextField thin{10, 20, 100, 0, 0};
    recorded = {};
    OA_CHECK(oa::app::set_text_input_area_with(&thin, nullptr, stub_target(recorded)));
    OA_CHECK(recorded.cleared);
    const oa::app::TextField negative{10, 20, -1, 16, 0};
    recorded = {};
    OA_CHECK(oa::app::set_text_input_area_with(&negative, nullptr, stub_target(recorded)));
    OA_CHECK(recorded.cleared);

    recorded = {};
    oa::app::TextInputAreaTarget missing = stub_target(recorded);
    missing.set_area = nullptr;
    OA_CHECK(!oa::app::set_text_input_area_with(&field, nullptr, missing));
    OA_CHECK(recorded.calls == 0);

    recorded = {};
    recorded.accept = false;
    OA_CHECK(!oa::app::set_text_input_area_with(&field, nullptr, stub_target(recorded)));
    OA_CHECK(recorded.calls == 1 && recorded.area.cursor == 6);
}

} // namespace

int main() {
    message_log_face_follows_the_text_size();
    chain_at_the_message_log_size();
    layout_follows_the_chain();
    capital_matches_the_message_log();
    threads_take_turns();
    settings_read_back();
    field_reaches_the_stub();
    return oa::test::check_exit_status();
}
