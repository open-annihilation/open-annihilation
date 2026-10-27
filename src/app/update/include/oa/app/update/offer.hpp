// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The question a verified release asks: its words, whether the player has
// skipped that release, and the release number a skip remembers. Nothing
// here opens a window or fetches.
#pragma once

#include "oa/app/update/build.hpp"
#include "oa/data/engine_feed/feed.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/library/text.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::update {

namespace words {

/// The question's title. `{version}` is the release's version.
inline constexpr std::string_view is_out = "Open Annihilation {version} is out";
/// The size sentence. `{current}` is this build's version, `{size}` the package zip.
inline constexpr std::string_view you_have = "You have {current}. The update is {size}.";
/// The line above the release's notes. `{version}` is the release's version.
inline constexpr std::string_view new_in = "New in {version}:";
/// The sentence under the notes.
inline constexpr std::string_view stay = "Your saved games, settings and mods stay as they are.";
/// Skip this version. Escape does not answer it.
inline constexpr std::string_view skip = "SKIP THIS VERSION";
/// Later. Escape answers it, and nothing is written.
inline constexpr std::string_view later = "LATER";
/// Update, the primary button. It opens the release's page.
inline constexpr std::string_view update = "UPDATE";
/// What a Steam Deck in Game Mode shows instead of opening the page. `{page}` is the address.
inline constexpr std::string_view game_mode =
    "Open {page} in a browser on another computer, or in Desktop Mode.";

/// The page a release uses when it names none.
inline constexpr std::string_view releases_page =
    "https://github.com/open-annihilation/open-annihilation/releases";

} // namespace words

/// The words of one offer, in the language the hooks show.
struct OfferText {
    std::string title;                   ///< the title; empty when there is nothing to offer
    std::vector<std::string> paragraphs; ///< the size sentence, then the reassurance
    std::vector<std::string> notes;      ///< the release's notes, not looked up
};

/// Writes the question's words for one offer.
///
/// The title, the size sentence and the reassurance are looked up through
/// `text` and then filled. The notes are the release's own lines for
/// `language`, then its first subtag, then English, and are not looked up.
/// The size is the offered package's zip. An offer with no release, no
/// package, or way none writes nothing.
///
/// @param offer the offer
/// @param build this build, whose version the size sentence names
/// @param language the language tag the notes are chosen for
/// @param text how the words are looked up
/// @return the words
[[nodiscard]] OfferText offer_text(
    const oa::data::engine_feed::Offer& offer,
    const ThisBuild& build,
    std::string_view language,
    const oa::ui::library::TextHooks& text
);

/// Writes the notes the prompt and the Updated notice list.
///
/// Empty lines write nothing. Otherwise the heading is "New in {version}:",
/// looked up and filled, and each line follows on its own line with a bullet.
/// The bullet and the lines are not looked up.
///
/// @param version the release's version, which the heading names
/// @param lines the release's notes, as the feed wrote them
/// @param text how the heading is looked up
/// @return the heading and the lines, or empty
[[nodiscard]] std::string notes_paragraph(
    std::string_view version,
    const std::vector<std::string>& lines,
    const oa::ui::library::TextHooks& text
);

/// Tells whether the question should be asked.
///
/// Nothing is asked when the offer's way is none or it names no release.
/// An absent skip asks. A skip asks only when the offer's release number
/// is above the skipped number, so that release and every older one stay
/// quiet and a newer one asks.
///
/// @param offer the offer
/// @param skipped the highest release number the player skipped, or nothing
/// @return true when the question should be asked
[[nodiscard]] bool
offer_wanted(const oa::data::engine_feed::Offer& offer, std::optional<int64_t> skipped) noexcept;

/// Reads the highest release number the player skipped.
///
/// Absent, or any text that is not a whole number, means none.
///
/// @param values the preference values
/// @return the release number, or nothing
[[nodiscard]] std::optional<int64_t> read_skipped(const platform::preferences::Values& values);

/// Writes the highest release number the player skipped.
///
/// @param[in,out] values the preference values
/// @param release the release number
void write_skipped(platform::preferences::Values& values, int64_t release);

} // namespace oa::app::update
