// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Contents browser (table of contents, ladder pages, remote documents) and
// the Galactic War planet-information text.
#pragma once

#include "oa/boneyards/service.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

inline constexpr std::size_t kContentsTextCapacity = 0x100;
inline constexpr std::size_t kMaxContentsCategories = 16;
inline constexpr std::size_t kMaxContentsHeadlines = 24;

// One headline of a contents category.
struct ContentsHeadline {
    int32_t id;
    int32_t order; // sort key
    char title[kContentsTextCapacity];
    char locator[kContentsTextCapacity];
};

// One category of a contents list, with its headlines.
struct ContentsCategory {
    int32_t id;
    int32_t order; // sort key
    char name[kContentsTextCapacity];
    ContentsHeadline headlines[kMaxContentsHeadlines];
    std::size_t headline_count;
    bool expanded;
};

struct Contents {
    ContentsCategory categories[kMaxContentsCategories];
    std::size_t category_count;
};

// Fields of one table-of-contents record ("category" or "headline").
struct ContentsField {
    const char* kind;
    const char* id;
    const char* name;
    const char* order;
    const char* parent; // headline: owning category id
    const char* title;
    const char* locator;
};

/// Fills a category record from its decimal id and order strings and its name.
///
/// @param[out] category record to fill; cleared first
/// @param id category id as decimal text; null reads as 0
/// @param order sort key as decimal text; null reads as 0
/// @param name category name
void contents_init_category(
    ContentsCategory* category, const char* id, const char* order, const char* name
);

/// Inserts a category after every category whose order is not greater.
///
/// @param[in,out] contents table of contents
/// @param category record copied in
/// @return false when the table is full
bool contents_insert_category(Contents* contents, const ContentsCategory* category);

/// Inserts a headline after every headline whose order is not greater.
///
/// @param[in,out] category category receiving the headline
/// @param headline record copied in
/// @return false when the category is full
bool contents_insert_headline(ContentsCategory* category, const ContentsHeadline* headline);

/// Applies one "category" or "headline" record to the table of contents.
///
/// A headline joins the category whose id is its parent; unknown kinds and
/// headlines without their category are ignored.
///
/// @param[in,out] contents table of contents
/// @param field the record's fields
void contents_apply_field(Contents* contents, const ContentsField* field);

// One row of the contents list.
struct ContentsRow {
    bool is_category;
    std::size_t category;
    std::size_t headline;
};

/// Lays out the contents list: every category, followed by its headlines when expanded.
///
/// @param[in,out] contents table of contents; the first category may be expanded
/// @param select_first expand the first category and copy its first headline's locator
/// @param[out] rows row buffer
/// @param capacity rows `rows` holds; rows past it are counted but not written
/// @param[out] first_locator receives the first headline's locator with `select_first`; may be null
/// @param locator_capacity size of `first_locator` in bytes
/// @return number of rows in the layout
std::size_t contents_build_rows(
    Contents* contents,
    bool select_first,
    ContentsRow* rows,
    std::size_t capacity,
    char* first_locator,
    std::size_t locator_capacity
);

// Outcome of resolving a document locator.
enum class LocatorKind : int32_t {
    local,       // a local document; handled directly
    remote,      // fetched through the service
    unavailable, // "#Remote#" locator without a content server, or a fetch not started
};

/// Resolves a document locator and starts fetching a remote one.
///
/// "#Remote#/path" becomes http://host:port/path on the service's content
/// server of `server_type`; it and an http:// locator are fetched through
/// the service into a temporary file. Anything else is local.
///
/// @param service client service
/// @param server_type kContentServerNews or kContentServerLadders
/// @param[in,out] locator document locator; with `rewrite` replaced by the resolved URL
/// @param capacity size of `locator` in bytes
/// @param rewrite whether the resolved URL replaces the locator text
/// @return local, remote, or unavailable when the server names no host or the fetch did not start
LocatorKind resolve_document_locator(
    const BoneyardsService* service,
    int32_t server_type,
    char* locator,
    std::size_t capacity,
    bool rewrite
);

/// Builds the ladder page locator holding a 1-based ladder position.
///
/// The character before ".htm" becomes the page number (100 entries per
/// page).
///
/// @param base locator of the ladder's first page
/// @param position 1-based ladder position
/// @param[out] out page locator
/// @param capacity size of `out` in bytes
void ladder_page_locator(const char* base, int32_t position, char* out, std::size_t capacity);

/// Builds the locator of a ladder's name index from one of its page locators.
///
/// The page's number and ".htm" become the first letter of the name looked
/// for and ".byi".
///
/// @param page locator of a ladder page
/// @param first first letter of the name
/// @param[out] out index locator
/// @param capacity size of `out` in bytes
void ladder_index_locator(const char* page, char first, char* out, std::size_t capacity);

/// Finds a name's 1-based ladder position in a ladder's name index.
///
/// The index is a run of words, each name followed by its position; names
/// match without regard to case.
///
/// @param index the index's text
/// @param size bytes of `index`
/// @param name name looked for
/// @return the position, or -1 when the index does not list the name
int32_t ladder_index_position(const char* index, std::size_t size, const char* name);

/// Formats the notice shown when a ladder search finds nothing (Windows-1252 quotes).
///
/// @param user user name searched for
/// @param[out] out notice text
/// @param capacity size of `out` in bytes
void ladder_not_listed_message(const char* user, char* out, std::size_t capacity);

/// Maps a planet affiliation code to its label ("Arm", "Core", "Contested").
///
/// @param code affiliation letter A, C or I, either case
/// @return the label, or null for an unknown code
const char* planet_affiliation_label(char code);

inline constexpr std::size_t kMaxRequirementLines = 6;

/// Lists the lines of the notice that a Galactic War planet cannot be fought for.
///
/// @param requirement planet requirement code (0 disk, 1 CC, 2 BT, 3 map, 4 CC and map,
///     5 BT and map); another code lists no requirement
/// @param[out] lines receives up to kMaxRequirementLines static texts
/// @return the line count
std::size_t gw_requirement_lines(int32_t requirement, const char** lines);

} // namespace oa::boneyards::screens
