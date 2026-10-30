// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The update question's words, and which release a skip still asks about.
// The notes are the feed's own text. Nothing is fetched.

#include "oa/app/update/offer.hpp"

#include "oa/test/check.hpp"
#include "oa/ui/library/text.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace update = oa::app::update;
namespace feed = oa::data::engine_feed;
namespace library = oa::ui::library;

/// One release, its package and the notes the cases read.
struct Sample {
    feed::Feed catalog{};
    feed::Offer offer{};
    update::ThisBuild build{};
};

/// A release 999, version 9.9.9, with English and German notes.
Sample sample(bool with_notes) {
    Sample held;
    feed::Release release;
    release.release = 999;
    release.version_text = "9.9.9";
    release.page = "https://example.invalid/releases/9.9.9";
    if (with_notes) {
        feed::Notes english;
        english.language = "en";
        english.lines = {"Wider bars", "A second English line"};
        feed::Notes german;
        german.language = "de";
        german.lines = {"Breitere Balken", "Eine deutsche Zeile"};
        release.notes = {std::move(english), std::move(german)};
    }
    feed::Package package;
    package.id = "test";
    package.zip.size = 43011223;
    release.packages.push_back(std::move(package));
    held.catalog.releases.push_back(std::move(release));
    held.offer.release = &held.catalog.releases.front();
    held.offer.package = &held.catalog.releases.front().packages.front();
    held.offer.way = feed::Way::in_place;
    held.build.version_text = "0.8.6";
    held.build.release = 806;
    return held;
}

/// Wraps every looked-up pattern. The view stays valid until the next lookup,
/// and each result is copied before the next lookup.
std::string_view mark_word(void*, std::string_view english) {
    static std::string held;
    held = "«";
    held.append(english);
    held.append("»");
    return held;
}

void english_text() {
    const Sample held = sample(true);
    const update::OfferText text =
        update::offer_text(held.offer, held.build, "en", library::TextHooks{});
    const std::string size = library::size_text(held.offer.package->zip.size);
    OA_CHECK(text.title == "Open Annihilation 9.9.9 is out");
    OA_CHECK(text.paragraphs.size() == 2);
    OA_CHECK(text.paragraphs[0] == "You have 0.8.6. The update is " + size + ".");
    OA_CHECK(text.paragraphs[1] == "Your saved games, settings and mods stay as they are.");
    OA_CHECK(text.notes.size() == 2);
    OA_CHECK(text.notes[0] == "Wider bars");
    OA_CHECK(text.notes[1] == "A second English line");
}

void marked_words_leave_the_notes() {
    const Sample held = sample(true);
    const library::TextHooks hooks{nullptr, mark_word};
    const update::OfferText text = update::offer_text(held.offer, held.build, "en", hooks);
    // The pattern is looked up whole, then its places are filled, so the marks
    // stay and the version is the release's.
    OA_CHECK(text.title == "«Open Annihilation 9.9.9 is out»");
    const std::string size = library::size_text(held.offer.package->zip.size, hooks);
    OA_CHECK(text.paragraphs.size() == 2);
    OA_CHECK(text.paragraphs[0].find("«You have ") == 0);
    OA_CHECK(text.paragraphs[0].find("0.8.6") != std::string::npos);
    OA_CHECK(text.paragraphs[0].find(size) != std::string::npos);
    OA_CHECK(text.paragraphs[0].find("«") != std::string::npos);
    OA_CHECK(text.paragraphs[1] == "«Your saved games, settings and mods stay as they are.»");
    OA_CHECK(text.notes.size() == 2);
    OA_CHECK(text.notes[0] == "Wider bars");
    OA_CHECK(text.notes[1] == "A second English line");
    for (const std::string& line : text.notes)
        OA_CHECK(line.find("«") == std::string::npos);
}

void notes_follow_the_language() {
    const Sample held = sample(true);
    const library::TextHooks hooks{};
    const update::OfferText german = update::offer_text(held.offer, held.build, "de", hooks);
    OA_CHECK(german.notes.size() == 2);
    OA_CHECK(german.notes[0] == "Breitere Balken");
    OA_CHECK(german.notes[1] == "Eine deutsche Zeile");
    const update::OfferText french = update::offer_text(held.offer, held.build, "fr", hooks);
    OA_CHECK(french.notes.size() == 2);
    OA_CHECK(french.notes[0] == "Wider bars");
    OA_CHECK(french.notes[1] == "A second English line");
    const Sample bare = sample(false);
    const update::OfferText none = update::offer_text(bare.offer, bare.build, "de", hooks);
    OA_CHECK(none.notes.empty());
    OA_CHECK(none.title == "Open Annihilation 9.9.9 is out");
}

void the_package_size_is_the_library_size() {
    OA_CHECK(library::size_text(24539136) == "23.4 MB");
    Sample held = sample(true);
    held.catalog.releases.front().packages.front().zip.size = 24539136;
    const update::OfferText text =
        update::offer_text(held.offer, held.build, "en", library::TextHooks{});
    OA_CHECK(text.paragraphs.size() == 2);
    OA_CHECK(text.paragraphs[0] == "You have 0.8.6. The update is 23.4 MB.");
}

void notes_are_one_line_each() {
    const std::vector<std::string> lines = {"Wider bars", "A second English line"};
    const std::string prompt = update::notes_paragraph("9.9.9", lines, library::TextHooks{});
    OA_CHECK(prompt == "New in 9.9.9:\n• Wider bars\n• A second English line");
    const std::string updated = update::notes_paragraph("0.8.7", lines, library::TextHooks{});
    OA_CHECK(updated == "New in 0.8.7:\n• Wider bars\n• A second English line");
    OA_CHECK(update::notes_paragraph("9.9.9", {}, library::TextHooks{}).empty());
    const library::TextHooks hooks{nullptr, mark_word};
    const std::string marked = update::notes_paragraph("9.9.9", lines, hooks);
    OA_CHECK(marked == "«New in 9.9.9:»\n• Wider bars\n• A second English line");
}

void wanted_against_the_skip() {
    const Sample held = sample(true);
    OA_CHECK(update::offer_wanted(held.offer, std::nullopt));
    OA_CHECK(!update::offer_wanted(held.offer, 999));
    OA_CHECK(!update::offer_wanted(held.offer, 1000));
    OA_CHECK(update::offer_wanted(held.offer, 998));
    feed::Offer none = held.offer;
    none.way = feed::Way::none;
    OA_CHECK(!update::offer_wanted(none, std::nullopt));
    feed::Offer missing = held.offer;
    missing.release = nullptr;
    OA_CHECK(!update::offer_wanted(missing, std::nullopt));
}

void skipped_round_trip() {
    oa::platform::preferences::Values values;
    OA_CHECK(!update::read_skipped(values).has_value());
    update::write_skipped(values, 808);
    OA_CHECK(update::read_skipped(values) == 808);
    values["open-annihilation.update-skipped"] = "nope";
    OA_CHECK(!update::read_skipped(values).has_value());
    values["open-annihilation.update-skipped"] = "42abc";
    OA_CHECK(!update::read_skipped(values).has_value());
    values["open-annihilation.update-skipped"] = "";
    OA_CHECK(!update::read_skipped(values).has_value());
}

} // namespace

int main() {
    english_text();
    marked_words_leave_the_notes();
    notes_follow_the_language();
    the_package_size_is_the_library_size();
    notes_are_one_line_each();
    wanted_against_the_skip();
    skipped_round_trip();
    return oa::test::check_exit_status();
}
