// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The first-run steps: their layout, their keys and pointer, and their drawing.
// Words are looked up as the screen is laid out. On the language step a built-in
// or installed language in the middle is read from the same catalogue, and the
// language the game shows stays as it was until the player chooses.

#include "oa/ui/first_run/first_run.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::ui::first_run {

void draw_sample(
    const kit::Canvas& canvas, const kit::Rect& inset, const Model& model, kit::SizeClass size_class
);

namespace {

using oa::data::languages::interface_text;

/// The room inside the setup chrome, from the window's top left.
struct Band {
    int32_t left{};
    int32_t top{};
    int32_t width{};
    int32_t height{};
};

/// Returns the band the step's own words share, under the header and above the footer.
///
/// @param width the window's width, in points
/// @param height the window's height, in points
/// @return the band
Band content_band(int32_t width, int32_t height) noexcept {
    const int32_t left = kit::compact_metrics.edge + kit::compact_metrics.padding;
    const int32_t top = kit::compact_metrics.edge + kit::compact_metrics.header_height + 1 +
                        kit::compact_metrics.padding;
    const int32_t bottom = height - kit::compact_metrics.edge - kit::compact_metrics.footer_height -
                           1 - kit::compact_metrics.padding;
    Band band;
    band.left = left;
    band.top = top;
    band.width = std::max(width - 2 * left, int32_t{0});
    band.height = std::max(bottom - top, int32_t{0});
    return band;
}

/// Looks a word up and keeps a string the look can own.
///
/// A language reads that word from the installed catalogue. Null reads the
/// language the game is showing.
///
/// @param english the catalogue text
/// @param language the language to read it in; null for the one in use
/// @return the words to show
std::string
show(std::string_view english, const oa::data::languages::Language* language = nullptr) {
    if (language != nullptr)
        return std::string(oa::data::languages::interface_text_in(english, *language));
    return std::string(interface_text(english));
}

/// One {place} of a pattern and the words it stands for.
struct Place {
    std::string_view name{};
    std::string value{};
};

/// Looks a pattern up and fills each of its {places} once.
///
/// @param english the catalogue text
/// @param places the places it names
/// @return the words to show
std::string filled(std::string_view english, std::initializer_list<Place> places) {
    std::string text = show(english);
    for (const Place& place : places) {
        const std::string token = "{" + std::string(place.name) + "}";
        const auto at = text.find(token);
        if (at != std::string::npos)
            text.replace(at, token.size(), place.value);
    }
    return text;
}

/// The language the language step's own words show in: the card in the middle
/// when the game can read it now. A pack that is not installed yet, and any
/// other step, leave the words in the language in use.
///
/// @param session the session
/// @return the language; null to keep the language in use
const oa::data::languages::Language* centred_language(const Session& session) noexcept {
    if (session.model.step != Step::language || session.carousel.selected < 0)
        return nullptr;
    const auto index = static_cast<std::size_t>(session.carousel.selected);
    if (index >= session.model.cards.size())
        return nullptr;
    const Card& card = session.model.cards[index];
    if (card.kind != CardKind::built_in && card.kind != CardKind::installed)
        return nullptr;
    const oa::data::languages::Language* centred = oa::data::languages::find_by_tag(card.tag);
    if (centred == nullptr || !oa::data::languages::playable(*centred))
        return nullptr;
    return centred;
}

/// Adds one line of text.
///
/// @param[in,out] list the display list
/// @param rect the line
/// @param text the words
/// @param font which font
/// @param ink the colour
void add_text(
    kit::DisplayList& list,
    const kit::Rect& rect,
    std::string text,
    kit::FontRole font,
    kit::Colour ink,
    kit::Align align = kit::Align::left
) {
    if (rect.width <= 0 || rect.height <= 0 || text.empty())
        return;
    kit::Item item;
    item.role = kit::Role::text;
    item.rect = rect;
    item.text = std::move(text);
    item.font = font;
    item.colour = ink;
    item.align = align;
    list.items.push_back(std::move(item));
}

/// Measures a line. A font that reports nothing falls back to the estimate,
/// so a paragraph still wraps when the game font has no glyphs.
///
/// @param fonts the fonts; null estimates
/// @param font which font
/// @return the measure
kit::Measure measure_of(const kit::Fonts* fonts, kit::FontRole font) {
    return [fonts, font](std::string_view words) {
        if (fonts == nullptr || words.empty())
            return kit::estimated_width(words);
        const int32_t measured = kit::text_width(*fonts, font, words);
        if (measured <= 0)
            return kit::estimated_width(words);
        return measured;
    };
}

/// Breaks a paragraph into lines and adds them. Returns the row under the last line.
///
/// @param[in,out] list the display list
/// @param fonts the fonts; null estimates
/// @param left the lines' left
/// @param top the first line's top
/// @param width the room
/// @param text the paragraph
/// @param font which font
/// @param ink the colour
/// @return the row under the last line
int32_t add_paragraph(
    kit::DisplayList& list,
    const kit::Fonts* fonts,
    int32_t left,
    int32_t top,
    int32_t width,
    std::string_view text,
    kit::FontRole font,
    kit::Colour ink,
    kit::Align align = kit::Align::left
) {
    const kit::Measure measure = measure_of(fonts, font);
    std::vector<std::string> lines = kit::wrap(text, std::max(width, int32_t{1}), measure);
    if (lines.empty() && !text.empty())
        lines.emplace_back(text);
    const int32_t line_height = font == kit::FontRole::small ? kit::compact_metrics.small_line
                                                             : kit::compact_metrics.regular_line;
    int32_t row = top;
    for (std::string& line : lines) {
        add_text(list, {left, row, width, line_height}, std::move(line), font, ink, align);
        row += line_height;
    }
    return row;
}

/// How tall a wrapped paragraph is.
///
/// @param fonts the fonts; null estimates
/// @param width the room
/// @param text the paragraph
/// @param font which font
/// @return the height, in points
int32_t paragraph_height(
    const kit::Fonts* fonts, int32_t width, std::string_view text, kit::FontRole font
) {
    const kit::Measure measure = measure_of(fonts, font);
    const std::vector<std::string> lines = kit::wrap(text, std::max(width, int32_t{1}), measure);
    const int32_t count = std::max(static_cast<int32_t>(lines.size()), int32_t{1});
    const int32_t line_height = font == kit::FontRole::small ? kit::compact_metrics.small_line
                                                             : kit::compact_metrics.regular_line;
    return count * line_height;
}

/// The height of the band a step's title is centred in.
constexpr int32_t kTitleHeight = 40;
/// The rows between the title's band and the line under it.
constexpr int32_t kTitleGap = 4;

/// Lays a step's title and the line under it out the same way on every
/// step: the title across the band in its prominent size, and the line in
/// the small font, centred under it, wrapped where the band is narrow.
///
/// @param[in,out] scene the scene
/// @param fonts the fonts; null estimates
/// @param band the room under the header
/// @param title the title
/// @param line the words under it
/// @return the row under the line
int32_t add_title(
    Scene& scene,
    const kit::Fonts* fonts,
    const Band& band,
    std::string title,
    std::string_view line
) {
    scene.title_text = std::move(title);
    scene.title = {band.left, band.top, band.width, kTitleHeight};
    return add_paragraph(
        scene.list,
        fonts,
        band.left,
        band.top + kTitleHeight + kTitleGap,
        band.width,
        line,
        kit::FontRole::small,
        kit::colour::hint,
        kit::Align::centre
    );
}

/// Adds a button the chrome does not draw, and remembers it for Tab.
///
/// @param[in,out] list the display list
/// @param[in,out] order Tab's order
/// @param rect the button
/// @param id its number
/// @param name its automation name
/// @param caption its words
/// @param enabled false while a press does not reach it
/// @param style which look
void add_button(
    kit::DisplayList& list,
    std::vector<kit::ControlId>& order,
    const kit::Rect& rect,
    kit::ControlId id,
    std::string name,
    std::string caption,
    bool enabled,
    kit::ButtonStyle style
) {
    kit::ButtonLook look;
    look.caption = caption;
    look.style = style;
    look.enabled = enabled;
    kit::Item item;
    item.role = kit::Role::button;
    item.rect = rect;
    item.text = caption;
    item.look = look;
    list.items.push_back(std::move(item));
    kit::Control control;
    control.id = id;
    control.rect = rect;
    control.enabled = enabled;
    control.name = std::move(name);
    control.kind = kit::ControlKind::button;
    control.focusable = true;
    control.text = std::move(caption);
    list.controls.push_back(std::move(control));
    order.push_back(id);
}

/// The step's word in an automation name.
///
/// @param step the step
/// @return language, online or buttons
std::string_view step_word(Step step) noexcept {
    return step_token(step);
}

/// An automation name, first-run.<step>.<control>.
///
/// @param step the step
/// @param control the control's word
/// @return the name
std::string control_name(Step step, std::string_view control) {
    std::string name = "first-run.";
    name += step_word(step);
    name += '.';
    name += control;
    return name;
}

/// A tag as an automation word: lower case, and anything else a hyphen.
///
/// @param tag the language's tag
/// @return the word
std::string tag_word(std::string_view tag) {
    std::string word;
    word.reserve(tag.size());
    for (const char letter : tag) {
        const auto unit = static_cast<unsigned char>(letter);
        if (unit >= 'A' && unit <= 'Z')
            word.push_back(static_cast<char>(unit - 'A' + 'a'));
        else if ((unit >= 'a' && unit <= 'z') || (unit >= '0' && unit <= '9'))
            word.push_back(static_cast<char>(unit));
        else
            word.push_back('-');
    }
    if (word.empty())
        word = "pack";
    return word;
}

/// The badge's letters: the tag's first two, in capitals.
///
/// @param tag the language's tag
/// @return one or two letters
std::string badge_letters(std::string_view tag) {
    std::string letters;
    for (const char letter : tag) {
        const auto unit = static_cast<unsigned char>(letter);
        if (unit >= 'a' && unit <= 'z')
            letters.push_back(static_cast<char>(unit - 'a' + 'A'));
        else if (unit >= 'A' && unit <= 'Z')
            letters.push_back(static_cast<char>(unit));
        if (letters.size() == 2)
            break;
    }
    if (letters.empty())
        letters = "EN";
    return letters;
}

/// The pill's colours.
///
/// @param card the card
/// @param active the card is the chosen one, and the step speaks its language
/// @return the availability
kit::LanguageAvailability availability_of(const Card& card, bool active) noexcept {
    if (active)
        return kit::LanguageAvailability::active;
    if (card.kind != CardKind::pack)
        return kit::LanguageAvailability::built_in;
    switch (card.pack) {
    case PackState::downloading:
        return kit::LanguageAvailability::downloading;
    case PackState::checking:
        return kit::LanguageAvailability::checking;
    case PackState::failed:
        return kit::LanguageAvailability::failed;
    case PackState::offline:
        return kit::LanguageAvailability::offline;
    case PackState::ready:
        return kit::LanguageAvailability::get;
    }
    return kit::LanguageAvailability::get;
}

/// The pill's words.
///
/// @param card the card
/// @param active the card is the chosen one, and the step speaks its language
/// @param language the language the step is speaking; null for the one in use
/// @return the words
std::string
pill_text(const Card& card, bool active, const oa::data::languages::Language* language) {
    if (active)
        return show("Active", language);
    if (card.kind == CardKind::built_in)
        return show("Built in", language);
    if (card.kind == CardKind::installed)
        return show("Installed", language);
    switch (card.pack) {
    case PackState::downloading:
        return show("Downloading", language);
    case PackState::checking:
        return show("Checking…", language);
    case PackState::failed:
        return show("Couldn't download", language);
    case PackState::offline:
        return show("Offline", language);
    case PackState::ready:
        break;
    }
    std::string pill = show("Get", language);
    if (!card.size_text.empty()) {
        pill += " · ";
        pill += card.size_text;
    }
    return pill;
}

/// The line under a card's names: a download's progress, or why it failed.
///
/// @param card the card
/// @param language the language the step is speaking; null for the one in use
/// @return the line; empty when the card has none
std::string facts_text(const Card& card, const oa::data::languages::Language* language) {
    if (card.kind == CardKind::pack && card.pack == PackState::downloading) {
        std::string line = show("Downloading", language);
        line += ' ';
        line += card.own_name;
        line += " · ";
        line += card.progress_done_text;
        line += ' ';
        line += show("of", language);
        line += ' ';
        line += card.progress_size_text;
        return line;
    }
    if (card.kind == CardKind::pack && card.pack == PackState::failed)
        return card.failure;
    return {};
}

/// Fills the setup chrome's look for a step.
///
/// @param session the session
/// @param width the window's width
/// @param height the window's height
/// @param back_enabled Back takes a press
/// @param next_enabled Next takes a press
/// @param language the language the language step is speaking; null on the other steps
/// @return the look
/// The page word beside the title: Setup before the game is mounted, Welcome after.
///
/// @param session the session
/// @param language the language the language step is speaking; null otherwise
/// @return the word
std::string page_word(const Session& session, const oa::data::languages::Language* language) {
    const bool setup_page =
        session.model.walk == Walk::setup &&
        (session.model.step == Step::language || session.model.step == Step::game ||
         session.model.step == Step::copying);
    return show(setup_page ? "Setup" : "Welcome", language);
}

kit::SetupChromeLook chrome_look(
    const Session& session,
    int32_t width,
    int32_t height,
    bool back_enabled,
    bool next_enabled,
    const oa::data::languages::Language* language
) {
    kit::SetupChromeLook look;
    look.width = width;
    look.height = height;
    look.page = page_word(session, language);
    // In a window the title bar already names the game.
    look.brand = !session.model.windowed;
    const StepPlace place = step_place(session.model.step, session.model.walk);
    look.step_name = show(place.name, language);
    look.step_pattern = language != nullptr ? show(kit::step_line_pattern, language)
                                            : std::string(interface_text(kit::step_line_pattern));
    look.step_index = place.index;
    look.step_count = place.count;
    // One word for the whole walk. The host chooses it. Copying does not
    // swap in Cancel: Escape still returns to the game step.
    look.skip = show(session.model.leave == Leave::quit ? "Quit" : "Skip", language);
    look.back = show("Back", language);
    look.next = show("Next", language);
    look.skip_enabled = true;
    look.back_enabled = back_enabled;
    look.next_enabled = next_enabled;
    const Step step = session.model.step;
    if (step == Step::game) {
        look.back_enabled = true;
        look.next = show("Copy the game", language);
        look.next_enabled =
            session.model.source >= 0 &&
            session.model.source < static_cast<int32_t>(session.model.sources.size());
    } else if (step == Step::copying) {
        look.back_enabled = false;
        if (session.model.copy.checked || session.model.copy.paused) {
            look.next = show("Continue", language);
            look.next_style = kit::ButtonStyle::accent;
            if (session.model.copy.checked && session.copy_continue_ms > 0) {
                const int32_t seconds = (session.copy_continue_ms + 999) / 1000;
                look.next += " · ";
                look.next += std::to_string(seconds);
            }
            look.next_enabled = true;
        } else {
            look.next = show("Pause", language);
            look.next_style = kit::ButtonStyle::plain;
            look.next_enabled = session.model.copy.running;
        }
    }
    return look;
}

/// Adds the chrome and its three buttons.
///
/// @param[in,out] scene the scene
/// @param[in,out] order Tab's order
/// @param session the session
/// @param width the window's width
/// @param height the window's height
/// @param back_enabled Back takes a press
/// @param next_enabled Next takes a press
/// @param language the language the language step is speaking; null on the other steps
void add_chrome(
    Scene& scene,
    std::vector<kit::ControlId>& order,
    const Session& session,
    int32_t width,
    int32_t height,
    bool back_enabled,
    bool next_enabled,
    const oa::data::languages::Language* language
) {
    const kit::SetupChromeLook look =
        chrome_look(session, width, height, back_enabled, next_enabled, language);
    kit::add_setup_chrome(scene.list, {0, 0}, look);
    const kit::SetupButtons buttons = kit::setup_buttons(nullptr, look);
    const Step step = session.model.step;
    kit::add_setup_controls(
        scene.list,
        {0, 0},
        buttons,
        look,
        skip_id,
        control_name(step, "skip"),
        back_id,
        control_name(step, "back"),
        next_id,
        control_name(step, "next")
    );
    order.push_back(skip_id);
    order.push_back(back_id);
    order.push_back(next_id);
}

/// One channel of a rule blended over the window face at half.
///
/// The same mix the painter uses at 128 of 256: the face keeps half, the
/// rule takes half, and a half is added before the divide.
///
/// @param under the window face's channel
/// @param over the rule's channel
/// @return the blended channel
uint8_t floor_channel(uint8_t under, uint8_t over) noexcept {
    constexpr uint32_t kOpaque = 256;
    constexpr uint32_t kShare = kOpaque / 2;
    const uint32_t painted = uint32_t{over} * kShare + kOpaque / 2;
    return static_cast<uint8_t>((uint32_t{under} * (kOpaque - kShare) + painted) / kOpaque);
}

/// The floor's ink: half the strength of a rule, over the window face.
///
/// @return the colour
kit::Colour floor_ink() noexcept {
    return {
        floor_channel(kit::colour::panel.r, kit::colour::rule.r),
        floor_channel(kit::colour::panel.g, kit::colour::rule.g),
        floor_channel(kit::colour::panel.b, kit::colour::rule.b),
    };
}

/// Draws the floor the language cards stand on: a grid, denser toward its top.
///
/// The lines are half a rule over the window face, at every size, so they
/// do not read as the edges of a table under the cards.
///
/// @param[in,out] list the display list
/// @param width the window's width
/// @param top the grid's top
/// @param bottom the grid's bottom
void add_floor(kit::DisplayList& list, int32_t width, int32_t top, int32_t bottom) {
    if (width <= 0 || bottom - top < 8)
        return;
    const kit::Colour ink = floor_ink();
    constexpr int32_t kRows = 5;
    for (int32_t row = 0; row <= kRows; ++row) {
        const int32_t y = top + (bottom - top) * row * row / (kRows * kRows);
        kit::Item rule;
        rule.role = kit::Role::rule;
        rule.rect = {0, y, width, 1};
        rule.colour = ink;
        list.items.push_back(std::move(rule));
    }
    constexpr int32_t kColumns = 12;
    for (int32_t column = 0; column <= kColumns; ++column) {
        const int32_t x = width * column / kColumns;
        kit::Item rule;
        rule.role = kit::Role::rule;
        rule.rect = {x, top, 1, bottom - top};
        rule.colour = ink;
        list.items.push_back(std::move(rule));
    }
}

/// The largest Library window that fits a room, at that window's own class.
struct SampleFit {
    kit::SizeClass size_class{kit::SizeClass::compact};
    int32_t width{};
    int32_t height{};
};

/// Picks a Library window that fits a room so the picture is not stretched.
///
/// @param room_width the room's width
/// @param room_height the room's height
/// @return the window
SampleFit fit_sample(int32_t room_width, int32_t room_height) {
    constexpr int32_t kChrome = 2 + 2 * kit::picture_well_inset;
    const kit::SizeClass order[] = {
        kit::SizeClass::large, kit::SizeClass::regular, kit::SizeClass::compact
    };
    for (const kit::SizeClass size_class : order) {
        const kit::Metrics& metrics = kit::metrics_of(size_class);
        if (metrics.dialog_width + kChrome <= room_width &&
            metrics.dialog_height + kChrome <= room_height)
            return {size_class, metrics.dialog_width, metrics.dialog_height};
    }
    return {
        kit::SizeClass::compact,
        std::max(room_width - kChrome, int32_t{1}),
        std::max(room_height - kChrome, int32_t{1}),
    };
}

/// The card the carousel is on or turning to.
///
/// @param session the session
/// @return its index; -1 when the carousel has no card
int32_t chosen_card(const Session& session) noexcept {
    const int32_t selected = session.carousel.selected;
    if (selected < 0 || static_cast<std::size_t>(selected) >= session.model.cards.size())
        return -1;
    return selected;
}

/// Moves a focus that rests on a language card to the chosen card, so the
/// focus ring marks the card the carousel turns to and no other. A focus on
/// an arrow or a footer button stays where it is.
///
/// @param[in,out] session the session
/// @return true when the focus moved
bool follow_chosen(Session& session) noexcept {
    if (session.model.step != Step::language)
        return false;
    const kit::ControlId focused = session.interaction.focused;
    if (focused < card_id || focused >= source_id)
        return false;
    const int32_t chosen = chosen_card(session);
    if (chosen < 0 || focused == card_id + chosen)
        return false;
    session.interaction.focused = card_id + chosen;
    return true;
}

/// Moves the carousel one card, wrapping from either end when it has more than one.
///
/// @param[in,out] model the carousel
/// @param next true moves to the next card, false to the previous
/// @return true when the selection changed
bool step_carousel(kit::CarouselModel& model, bool next) {
    if (model.count <= 1)
        return false;
    // The unwrapped index keeps the direction when two cards make both ways
    // the same length. carousel_go turns that into one step.
    const kit::CarouselCommand command{!next, next, false};
    return kit::carousel_command(model, command) != kit::CarouselEvent::none;
}

/// Lays the language step out.
///
/// @param[in,out] scene the scene
/// @param session the session
/// @param frame the room
/// @param fonts the fonts
void lay_language(
    Scene& scene, const Session& session, const kit::Frame& frame, const kit::Fonts* fonts
) {
    const int32_t width = frame.area.width;
    const int32_t height = frame.area.height;
    const bool busy = download_busy(session.model);
    const oa::data::languages::Language* const preview = centred_language(session);
    std::vector<kit::ControlId> order;
    add_chrome(scene, order, session, width, height, false, !busy, preview);

    const Band band = content_band(width, height);
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    int32_t row = add_title(
        scene,
        fonts,
        band,
        show("Welcome to Open Annihilation", preview),
        show("Choose your language.", preview)
    );
    row += 8;

    constexpr int32_t kArrow = 28;
    const std::string note = show(
        "Other languages download from Core Prime. The first download makes a random install ID, "
        "which a later step explains.",
        preview
    );
    const int32_t note_width = std::max(band.width, int32_t{1});
    const int32_t note_height = paragraph_height(fonts, note_width, note, kit::FontRole::small);
    std::string status;
    bool show_bar = false;
    bool show_action = false;
    bool action_cancel = false;
    if (session.carousel.selected >= 0 &&
        static_cast<std::size_t>(session.carousel.selected) < session.model.cards.size()) {
        const Card& chosen =
            session.model.cards[static_cast<std::size_t>(session.carousel.selected)];
        status = facts_text(chosen, preview);
        show_bar = chosen.kind == CardKind::pack && chosen.pack == PackState::downloading &&
                   chosen.progress_whole > 0;
        action_cancel = chosen.kind == CardKind::pack && (chosen.pack == PackState::downloading ||
                                                          chosen.pack == PackState::checking);
        show_action =
            action_cancel || (chosen.kind == CardKind::pack && chosen.pack == PackState::failed);
    }
    std::string hint;
    if (session.model.catalogue == CatalogueNote::unread ||
        session.model.catalogue == CatalogueNote::downloads_off) {
        hint =
            session.model.catalogue == CatalogueNote::downloads_off
                ? show("Downloads are off, so language packs stay offline.", preview)
                : show("The catalogue could not be read, so language packs stay offline.", preview);
    }
    int32_t under = 8 + note_height;
    if (!status.empty())
        under += kit::compact_metrics.small_line + 4;
    if (show_bar)
        under += metrics.progress_bar_height + 4;
    if (show_action)
        under += kit::compact_metrics.button_height + 4;
    if (!hint.empty())
        under += 4 + paragraph_height(fonts, note_width, hint, kit::FontRole::small);
    const int32_t footer_limit = height - kit::compact_metrics.edge -
                                 kit::compact_metrics.footer_height - 1 -
                                 kit::compact_metrics.padding;
    const kit::CarouselGeometry geometry = kit::carousel_fit(width, metrics);
    constexpr int32_t kFloor = 48;
    const int32_t band_height = geometry.card_height + kFloor;
    const int32_t carousel_height = band_height + kit::carousel_pager_band;
    int32_t band_y = height / 2 - band_height / 2;
    if (band_y < row)
        band_y = row;
    if (band_y + carousel_height + under > footer_limit)
        band_y = std::max(row, footer_limit - under - carousel_height);
    scene.carousel = {0, band_y, width, carousel_height};
    const int32_t arrow_y = band_y + band_height / 2 - kArrow / 2;
    scene.previous = {4, arrow_y, kArrow, kArrow};
    scene.next_arrow = {width - 4 - kArrow, arrow_y, kArrow, kArrow};
    add_floor(scene.list, width, band_y + band_height / 2, band_y + band_height);

    kit::CarouselLook carousel;
    carousel.card_width = geometry.card_width;
    carousel.card_height = geometry.card_height;
    carousel.step = geometry.step;
    carousel.centre_milli = kit::carousel_index_milli(session.carousel);
    carousel.reduced_motion = frame.reduced_motion;
    carousel.wrap = session.model.cards.size() > 1;
    // The chosen card alone is marked: the ring once it faces the player,
    // and the Active look while the step speaks its language, which is the
    // language in use or a built-in or installed language in the middle.
    // The language in use loses that look while another card is chosen.
    const int32_t chosen_index = chosen_card(session);
    carousel.chosen = chosen_index;
    const bool chosen_speaks =
        chosen_index >= 0 &&
        (session.model.cards[static_cast<std::size_t>(chosen_index)].current || preview != nullptr);
    carousel.cards.reserve(session.model.cards.size());
    for (std::size_t at = 0; at < session.model.cards.size(); ++at) {
        const Card& card = session.model.cards[at];
        const bool active = chosen_speaks && static_cast<int32_t>(at) == chosen_index;
        kit::CarouselCard face;
        face.letters = badge_letters(card.tag);
        face.own_name = card.own_name;
        face.english_name = card.english_name;
        face.facts = facts_text(card, preview);
        face.pill = pill_text(card, active, preview);
        face.availability = availability_of(card, active);
        face.disabled =
            busy && card.pack != PackState::downloading && card.pack != PackState::checking;
        carousel.cards.push_back(std::move(face));
    }
    kit::add_carousel(scene.list, scene.carousel, carousel);

    const kit::Rect places_band = kit::carousel_band(scene.carousel);
    scene.places = kit::carousel_places(
        places_band,
        geometry,
        static_cast<int32_t>(session.model.cards.size()),
        carousel.centre_milli,
        frame.reduced_motion,
        carousel.wrap
    );
    std::vector<kit::CarouselSlot> left_to_right = scene.places;
    std::sort(
        left_to_right.begin(),
        left_to_right.end(),
        [](const kit::CarouselSlot& left, const kit::CarouselSlot& right) {
            return left.face.x < right.face.x;
        }
    );
    const bool wraps = session.carousel.count > 1;
    const bool has_previous = wraps;
    const bool has_next = wraps;
    kit::add_carousel_control(
        scene.list,
        scene.previous,
        previous_id,
        control_name(Step::language, "previous"),
        show("Previous", preview),
        has_previous
    );
    order.push_back(previous_id);
    for (const kit::CarouselSlot& slot : left_to_right) {
        if (slot.index < 0 || static_cast<std::size_t>(slot.index) >= session.model.cards.size())
            continue;
        const Card& card = session.model.cards[static_cast<std::size_t>(slot.index)];
        const bool enabled = !busy;
        kit::add_carousel_control(
            scene.list,
            slot.face,
            card_id + slot.index,
            control_name(Step::language, "card." + tag_word(card.tag)),
            card.own_name,
            enabled
        );
        order.push_back(card_id + slot.index);
    }
    kit::add_carousel_control(
        scene.list,
        scene.next_arrow,
        next_card_id,
        control_name(Step::language, "next-card"),
        show("Next", preview),
        has_next
    );
    order.push_back(next_card_id);

    int32_t row_under = scene.carousel.y + scene.carousel.height + 8;
    if (!status.empty()) {
        add_text(
            scene.list,
            {band.left, row_under, band.width, kit::compact_metrics.small_line},
            status,
            kit::FontRole::small,
            kit::colour::text,
            kit::Align::centre
        );
        row_under += kit::compact_metrics.small_line + 4;
    }
    if (show_bar && session.carousel.selected >= 0 &&
        static_cast<std::size_t>(session.carousel.selected) < session.model.cards.size()) {
        const Card& chosen =
            session.model.cards[static_cast<std::size_t>(session.carousel.selected)];
        const int32_t bar_width = std::min(geometry.card_width, band.width);
        const kit::Rect bar{
            band.left + std::max((band.width - bar_width) / 2, int32_t{0}),
            row_under,
            bar_width,
            metrics.progress_bar_height
        };
        kit::Item progress;
        progress.role = kit::Role::progress;
        progress.rect = bar;
        progress.look = kit::ProgressLook{chosen.progress_done, chosen.progress_whole};
        scene.list.items.push_back(std::move(progress));
        row_under += metrics.progress_bar_height + 4;
    }
    if (show_action) {
        const std::string caption =
            action_cancel ? show("Cancel download", preview) : show("Retry", preview);
        const int32_t button_width = std::max(kit::compact_metrics.button_width, int32_t{120});
        const int32_t used = std::min(button_width, band.width);
        const kit::Rect button{
            band.left + std::max((band.width - used) / 2, int32_t{0}),
            row_under,
            used,
            kit::compact_metrics.button_height
        };
        add_button(
            scene.list,
            order,
            button,
            action_cancel ? cancel_id : retry_id,
            control_name(Step::language, action_cancel ? "cancel" : "retry"),
            caption,
            true,
            action_cancel ? kit::ButtonStyle::plain : kit::ButtonStyle::accent
        );
        row_under += kit::compact_metrics.button_height + 4;
    }
    const int32_t note_top = add_paragraph(
        scene.list,
        fonts,
        band.left,
        row_under,
        band.width,
        note,
        kit::FontRole::small,
        kit::colour::hint,
        kit::Align::centre
    );
    if (!hint.empty()) {
        add_paragraph(
            scene.list,
            fonts,
            band.left,
            note_top + 4,
            band.width,
            hint,
            kit::FontRole::small,
            kit::colour::quiet,
            kit::Align::centre
        );
    }
    scene.list.tab_order = std::move(order);
}

/// Lays the online-features step out.
///
/// @param[in,out] scene the scene
/// @param session the session
/// @param frame the room
/// @param fonts the fonts
void lay_online(
    Scene& scene, const Session& session, const kit::Frame& frame, const kit::Fonts* fonts
) {
    const int32_t width = frame.area.width;
    const int32_t height = frame.area.height;
    std::vector<kit::ControlId> order;
    add_chrome(scene, order, session, width, height, true, true, nullptr);
    const Band band = content_band(width, height);

    const std::string heading = show("Downloads, updates and a number only you hold");
    const std::string feature_packs =
        show("Get language packs, mods and map packs from Core Prime.");
    const std::string feature_lists =
        show("Lists of what is new are checked as the game starts and when the Library opens.");
    const std::string feature_id = show(
        "A random install ID, one per registry, is sent only when downloading from that registry."
    );
    const std::string panel_body = show(
        "A random 128-bit number, made the first time you download from a registry, not when the "
        "game starts. Each registry that asks has its own. Core Prime asks for one. It is sent "
        "only when asking that registry for a download. Turning it off forgets it, and downloads "
        "from that registry stop. Built-in languages and files you open keep working."
    );
    const std::string on_title = show("Turn on online features");
    const std::string on_hint = show(
        "Check for content updates stays at Automatically, and Core Prime's install ID stays on."
    );
    const std::string off_title = show("Not now");
    const std::string off_hint = show(
        "Check for content updates is set to Never, and Core Prime's install ID is turned off."
    );
    const int32_t card_height = kit::choice_card_height();
    constexpr int32_t kGap = 16;
    constexpr int32_t kChrome = 2 + 2 * kit::picture_well_inset;
    constexpr int32_t kTextFloor = 360;
    const auto column_height = [&](int32_t column) {
        const int32_t room = std::max(column, int32_t{1});
        const int32_t heading_h = paragraph_height(fonts, room, heading, kit::FontRole::regular);
        const int32_t packs_h = paragraph_height(fonts, room, feature_packs, kit::FontRole::small);
        const int32_t lists_h = paragraph_height(fonts, room, feature_lists, kit::FontRole::small);
        const int32_t id_h = paragraph_height(fonts, room, feature_id, kit::FontRole::small);
        const int32_t body_h = paragraph_height(
            fonts, std::max(column - 16, int32_t{1}), panel_body, kit::FontRole::small
        );
        const int32_t panel_h = 8 + kit::compact_metrics.regular_line + 4 + body_h + 8;
        // The card's own height holds one hint line. A wrapped hint grows it.
        const auto hint_extra = [&](std::string_view hint) {
            const int32_t hint_room = std::max(column - 30, int32_t{1});
            const int32_t tall = paragraph_height(fonts, hint_room, hint, kit::FontRole::small);
            return std::max(tall - kit::compact_metrics.small_line, int32_t{0});
        };
        return kit::compact_metrics.small_line + 4 + heading_h + 6 + packs_h + 2 + lists_h + 2 +
               id_h + 8 + panel_h + 8 + card_height * 2 + hint_extra(on_hint) +
               hint_extra(off_hint) + 8;
    };
    const auto place_well = [&](const kit::Rect& well_rect, const SampleFit& fitted) {
        kit::PictureWellLook well;
        well.stand_in = fonts == nullptr;
        kit::add_picture_well(scene.list, well_rect, well);
        const kit::PictureWellLayout placed = kit::picture_well_layout(well_rect, false);
        scene.well_inset = placed.inset;
        scene.sample_class = fitted.size_class;
        scene.sample = fonts != nullptr && placed.inset.width > 8 && placed.inset.height > 8;
    };

    bool beside = false;
    SampleFit fitted{};
    int32_t text_left = band.left;
    int32_t text_top = band.top;
    int32_t text_width = band.width;
    // At Compact the well is a strip of the sample's header. The text takes
    // that room, from the top of the band. Regular and Large keep the well.
    const bool show_well = frame.size_class != kit::SizeClass::compact;
    if (show_well && band.width >= kTextFloor + kGap + 200 && band.height >= 200) {
        const int32_t well_room = band.width - kTextFloor - kGap;
        fitted = fit_sample(well_room, band.height);
        const kit::Metrics& fitted_metrics = kit::metrics_of(fitted.size_class);
        const bool real_dialog = fitted.width == fitted_metrics.dialog_width &&
                                 fitted.height == fitted_metrics.dialog_height;
        const int32_t well_w = fitted.width + kChrome;
        const int32_t well_h = fitted.height + kChrome;
        const int32_t column = band.width - well_w - kGap;
        if (real_dialog && column >= 280 && well_h <= band.height &&
            column_height(column) <= band.height) {
            beside = true;
            place_well({band.left, band.top, well_w, well_h}, fitted);
            text_left = band.left + well_w + kGap;
            text_width = std::max(band.left + band.width - text_left, int32_t{1});
        }
    }
    if (show_well && !beside) {
        const int32_t words = column_height(std::max(band.width, int32_t{1}));
        int32_t room_h = std::max(band.height - words - 8, int32_t{72});
        const int32_t cap = std::max(band.height * 2 / 5, int32_t{72});
        if (room_h > cap)
            room_h = cap;
        fitted = fit_sample(band.width, room_h);
        const int32_t well_w = std::min(fitted.width + kChrome, std::max(band.width, int32_t{1}));
        const int32_t well_h = std::min(fitted.height + kChrome, std::max(room_h, int32_t{72}));
        const kit::Rect well_rect{
            band.left + std::max((band.width - well_w) / 2, int32_t{0}), band.top, well_w, well_h
        };
        place_well(well_rect, fitted);
        text_top = well_rect.y + well_rect.height + 8;
        text_width = std::max(band.width, int32_t{1});
    }

    int32_t row = text_top;
    add_text(
        scene.list,
        {text_left, row, text_width, kit::compact_metrics.small_line},
        show("ONLINE FEATURES"),
        kit::FontRole::small,
        kit::colour::quiet
    );
    row += kit::compact_metrics.small_line + 4;
    row = add_paragraph(
        scene.list,
        fonts,
        text_left,
        row,
        text_width,
        heading,
        kit::FontRole::regular,
        kit::colour::text
    );
    row += 6;
    const std::string features_text[] = {feature_packs, feature_lists, feature_id};
    for (const std::string& line : features_text) {
        row = add_paragraph(
            scene.list,
            fonts,
            text_left,
            row,
            text_width,
            line,
            kit::FontRole::small,
            kit::colour::text
        );
        row += 2;
    }
    row += 6;

    const int32_t panel_body_height = paragraph_height(
        fonts, std::max(text_width - 16, int32_t{1}), panel_body, kit::FontRole::small
    );
    const int32_t panel_height = 8 + kit::compact_metrics.regular_line + 4 + panel_body_height + 8;
    const kit::Rect panel{text_left, row, text_width, panel_height};
    kit::Item ground;
    ground.role = kit::Role::fill;
    ground.rect = panel;
    ground.colour = kit::colour::list;
    scene.list.items.push_back(ground);
    kit::Item edge;
    edge.role = kit::Role::outline;
    edge.rect = panel;
    edge.colour = kit::colour::control_border;
    scene.list.items.push_back(edge);
    add_text(
        scene.list,
        {panel.x + 8,
         panel.y + 6,
         std::max(panel.width - 16, int32_t{0}),
         kit::compact_metrics.regular_line},
        show("Your install ID"),
        kit::FontRole::regular,
        kit::colour::text
    );
    add_paragraph(
        scene.list,
        fonts,
        panel.x + 8,
        panel.y + 6 + kit::compact_metrics.regular_line + 2,
        std::max(panel.width - 16, int32_t{1}),
        panel_body,
        kit::FontRole::small,
        kit::colour::hint
    );
    row += panel_height + 8;

    const int32_t hint_room = std::max(text_width - 30, int32_t{1});
    const auto extra_for = [&](std::string_view hint) {
        return std::max(
            paragraph_height(fonts, hint_room, hint, kit::FontRole::small) -
                kit::compact_metrics.small_line,
            int32_t{0}
        );
    };
    const int32_t on_height = card_height + extra_for(on_hint);
    const int32_t off_height = card_height + extra_for(off_hint);
    const kit::Rect on_rect{text_left, row, text_width, on_height};
    const kit::Rect off_rect{text_left, row + on_height + 8, text_width, off_height};
    kit::ChoiceCardLook on_look;
    on_look.title = on_title;
    on_look.hint = on_hint;
    on_look.chosen = session.model.online_on;
    kit::ChoiceCardLook off_look;
    off_look.title = off_title;
    off_look.hint = off_hint;
    off_look.chosen = !session.model.online_on;
    kit::add_choice_card(scene.list, on_rect, on_look, on_id, control_name(Step::online, "on"));
    kit::add_choice_card(
        scene.list, off_rect, off_look, off_id, control_name(Step::online, "not-now")
    );
    order.insert(order.begin(), off_id);
    order.insert(order.begin(), on_id);
    scene.list.tab_order = std::move(order);
}

/// The short badge a source row shows.
///
/// @param mark the kind of place
/// @return the badge
std::string_view source_badge(SourceMark mark) noexcept {
    switch (mark) {
    case SourceMark::gog:
        return "GOG";
    case SourceMark::package:
        return "PKG";
    case SourceMark::demo:
        return "DEMO";
    case SourceMark::disc:
        return "DISC";
    case SourceMark::folder:
        return "FOLDER";
    }
    return "FOLDER";
}

/// The points between a list and its scroll bar, and between the copying
/// panel's edge and the bar inside it.
constexpr int32_t kBarGap = 4;
/// A scroll bar's well: the thumb and its border.
constexpr int32_t kWellWidth = kit::compact_metrics.scroll_thumb_width + 2;
/// How far a press beside the well still holds the scroll bar, either side.
constexpr int32_t kBarReach = 3;
/// The room kept above and below the row a list follows, so its edge and a
/// focus ring around it stay in view.
constexpr int32_t kFollowMargin = 4;

/// Returns where a list that does not fit scrolls, and how far.
///
/// The list keeps the offset the player left it at. Until the player
/// scrolls it, it moves the least from there that shows the row it follows
/// whole.
///
/// @param view what the list is seen through, in points
/// @param well the scroll bar's well, as high as the view or a little less
/// @param content_height the whole list's height, in points
/// @param scroll where the player left the list
/// @param follow_top the top of the row it follows before scrolling, in points
/// @param follow_height that row's height; 0 follows nothing
/// @param follow_last true when that row is the last
/// @return the scroll area
kit::ScrollArea list_area(
    const kit::Rect& view,
    const kit::Rect& well,
    int32_t content_height,
    const ListScroll& scroll,
    int32_t follow_top,
    int32_t follow_height,
    bool follow_last
) noexcept {
    kit::ScrollArea area;
    area.view = view;
    area.well = well;
    area.hit = {well.x - kBarReach, well.y, well.width + 2 * kBarReach, well.height};
    area.page_step = std::max(view.height - list_step, list_step);
    area.content_height = content_height;
    area.limit = kit::ScrollArea::limit_for(content_height, view.height);
    area.offset = std::clamp(scroll.offset, int32_t{0}, area.limit);
    if (!scroll.held && follow_height > 0)
        area.offset = area.offset_showing(follow_top, follow_height, follow_last);
    return area;
}

/// Adds a list's scroll bar.
///
/// @param[in,out] list the display list
/// @param area where the list scrolls
/// @param scroll the press on it, which lights it while it is held
void add_list_bar(kit::DisplayList& list, const kit::ScrollArea& area, const ListScroll& scroll) {
    kit::Item bar;
    bar.role = kit::Role::scroll_bar;
    bar.rect = area.well;
    bar.look = kit::ScrollBarLook{
        area.thumb(kit::compact_metrics.least_thumb_height), scroll.dragging && scroll.by_bar
    };
    list.items.push_back(std::move(bar));
}

/// Clips what was added to a display list since a point in it.
///
/// @param[in,out] list the display list
/// @param items_from the first item to clip
/// @param controls_from the first control to clip
/// @param clip what drawing and a press may reach
void clip_since(
    kit::DisplayList& list, std::size_t items_from, std::size_t controls_from, const kit::Rect& clip
) {
    for (std::size_t at = items_from; at < list.items.size(); ++at)
        list.items[at].clip = clip;
    for (std::size_t at = controls_from; at < list.controls.size(); ++at)
        list.controls[at].clip = clip;
}

/// Returns a footer button's width: button_width, or the caption with the
/// same inset the footer's buttons keep either side of the word. Without
/// the game's fonts, or with fonts that have no glyphs, the caption is
/// estimated as the footer's own buttons estimate theirs, so the words the
/// modern fonts draw stay inside the box.
///
/// @param fonts the fonts; null estimates the caption
/// @param caption the button's words
/// @return the width, in points
int32_t footer_button_width(const kit::Fonts* fonts, std::string_view caption) {
    const int32_t text = measure_of(fonts, kit::FontRole::small)(caption);
    return std::max(
        kit::compact_metrics.button_width, text + 2 * kit::compact_metrics.choice_text_inset
    );
}

/// A chip's width: the pill's own measure, or the estimate when the fonts are not there.
///
/// @param fonts the fonts; null estimates
/// @param text the chip's words
/// @param room the widest a chip may be
/// @return the width, in points
int32_t chip_width(const kit::Fonts* fonts, std::string_view text, int32_t room) {
    int32_t measured = 0;
    if (fonts != nullptr) {
        kit::PillLook look;
        look.text = std::string(text);
        measured = kit::pill_width(*fonts, look);
    }
    if (measured <= 0)
        measured = std::max(static_cast<int32_t>(text.size()) * 7 + 16, int32_t{48});
    return std::min(measured, std::max(room, int32_t{48}));
}

/// Lays the game step out: the sources that were found, and Locate another.
///
/// The column is as wide as Settings' panel. Each row holds its path and the bundles it has,
/// and the rows are separated by the panel's padding. Rows that do not fit
/// above Locate another… and the line under it scroll between the title and
/// Locate another…, with a scroll bar right of them.
///
/// @param[in,out] scene the scene
/// @param session the session
/// @param frame the room
/// @param fonts the fonts
void lay_game(
    Scene& scene, const Session& session, const kit::Frame& frame, const kit::Fonts* fonts
) {
    const int32_t width = frame.area.width;
    const int32_t height = frame.area.height;
    std::vector<kit::ControlId> order;
    add_chrome(scene, order, session, width, height, true, true, nullptr);
    const Band band = content_band(width, height);
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    int32_t column = std::min(band.width, metrics.dialog_width);
    const int32_t left = band.left + std::max((band.width - column) / 2, int32_t{0});
    // The choice card's text starts after the radio, and stops short of the card's right edge.
    constexpr int32_t kTextInset = 24;
    constexpr int32_t kTextEnd = 6;
    const int32_t text_left = left + kTextInset;

    int32_t row = add_title(
        scene,
        fonts,
        band,
        show("Locate Total Annihilation Game Files"),
        show("OA copies the game once. The installation is left as it is.")
    );
    row += metrics.padding;
    const int32_t list_top = row;

    constexpr int32_t kCardPad = 6;
    constexpr int32_t kChipGap = 6;
    const int32_t chip_h = kit::pill_height();
    const auto count = static_cast<int32_t>(session.model.sources.size());

    struct ChipSpot {
        std::string text{};
        int32_t x{};
        int32_t y{};
        int32_t width{};
    };

    /// One row's words and its chips, laid out for the text's width.
    struct CardPlan {
        std::string hint{};
        std::vector<ChipSpot> spots{};
        int32_t chip_block{};
        int32_t hint_h{};
        int32_t height{};
    };

    const auto plan_cards = [&](int32_t words_width) {
        std::vector<CardPlan> made;
        for (const GameSource& source : session.model.sources) {
            CardPlan plan;
            plan.hint = std::string(source_badge(source.mark));
            if (!source.detail.empty()) {
                plan.hint += " · ";
                plan.hint += source.detail;
            }
            if (!source.status.empty()) {
                plan.hint += " · ";
                plan.hint += source.status;
            }
            int32_t cursor = 0;
            int32_t chip_row = 0;
            for (const SourceChip& chip : source.chips) {
                if (!chip.held || chip.name.empty())
                    continue;
                const int32_t pill_w = chip_width(fonts, chip.name, words_width);
                if (cursor > 0 && cursor + pill_w > words_width) {
                    cursor = 0;
                    ++chip_row;
                }
                ChipSpot spot;
                spot.text = chip.name;
                spot.x = cursor;
                spot.y = chip_row * (chip_h + kChipGap);
                spot.width = pill_w;
                cursor += pill_w + kChipGap;
                plan.spots.push_back(std::move(spot));
            }
            const int32_t chip_rows = plan.spots.empty() ? 0 : chip_row + 1;
            plan.chip_block = chip_rows == 0 ? 0 : chip_rows * chip_h + (chip_rows - 1) * kChipGap;
            plan.hint_h = std::max(
                paragraph_height(fonts, words_width, plan.hint, kit::FontRole::small),
                metrics.small_line
            );
            const int32_t note_h =
                source.footnote.empty()
                    ? 0
                    : paragraph_height(fonts, words_width, source.footnote, kit::FontRole::small);
            int32_t card_h = kCardPad + metrics.regular_line + metrics.hint_gap + plan.hint_h;
            if (plan.chip_block > 0)
                card_h += metrics.hint_gap + plan.chip_block;
            if (note_h > 0)
                card_h += metrics.hint_gap + note_h;
            card_h += kCardPad;
            plan.height = std::max(card_h, kit::choice_card_height());
            made.push_back(std::move(plan));
        }
        return made;
    };
    const auto list_height = [&](const std::vector<CardPlan>& planned) {
        int32_t total = 0;
        for (const CardPlan& plan : planned)
            total += plan.height + metrics.padding;
        return std::max(total - metrics.padding, int32_t{0});
    };

    int32_t text_w = std::max(column - kTextInset - kTextEnd, int32_t{1});
    std::vector<CardPlan> plans = plan_cards(text_w);
    int32_t content_height = list_height(plans);

    // Under the rows: Locate another…, the line under it, and with a
    // controller the band's last line, which its hint takes.
    const std::string locate = show("Locate another…");
    const std::string copies = show("Copies go to Documents › Open Annihilation");
    const int32_t button_height = kit::compact_metrics.button_height;
    const int32_t under_rows =
        metrics.padding + button_height + 8 +
        paragraph_height(fonts, column, copies, kit::FontRole::small) +
        (session.model.controller ? kit::compact_metrics.small_line : int32_t{0});
    const int32_t room = std::max(band.top + band.height - under_rows - list_top, int32_t{0});
    const bool scrolls = count > 0 && content_height > room;
    if (scrolls && left + column + kBarGap + kWellWidth > band.left + band.width) {
        // No room right of the rows for the scroll bar, so the rows give it theirs.
        column = std::max(column - kBarGap - kWellWidth, int32_t{1});
        text_w = std::max(column - kTextInset - kTextEnd, int32_t{1});
        plans = plan_cards(text_w);
        content_height = list_height(plans);
    }

    std::vector<int32_t> tops;
    int32_t top = 0;
    for (const CardPlan& plan : plans) {
        tops.push_back(top);
        top += plan.height + metrics.padding;
    }
    kit::ScrollArea area;
    kit::Rect clip{};
    if (scrolls) {
        const kit::Rect view{left, list_top, column, room};
        const kit::Rect well{left + column + kBarGap, list_top, kWellWidth, room};
        // The list follows the place the focus is on, else the chosen one.
        int32_t followed = -1;
        const kit::ControlId focused = session.interaction.focused;
        if (focused >= source_id && focused < source_id + count)
            followed = focused - source_id;
        else if (session.model.source >= 0 && session.model.source < count)
            followed = session.model.source;
        int32_t follow_top = 0;
        int32_t follow_height = 0;
        if (followed >= 0) {
            const auto at = static_cast<std::size_t>(followed);
            follow_top = list_top + tops[at] - kFollowMargin;
            follow_height = plans[at].height + 2 * kFollowMargin;
        }
        area = list_area(
            view,
            well,
            content_height,
            session.source_list,
            follow_top,
            follow_height,
            followed == count - 1
        );
        scene.list_scroll = area;
        // The focus ring stands just outside a row, so the clip leaves it
        // room at the sides.
        clip = {
            left - kFollowMargin, list_top, column + 2 * kFollowMargin, std::max(room, int32_t{1})
        };
    }

    for (int32_t index = 0; index < count; ++index) {
        const auto at = static_cast<std::size_t>(index);
        const GameSource& source = session.model.sources[at];
        const CardPlan& plan = plans[at];
        const std::size_t items_from = scene.list.items.size();
        const std::size_t controls_from = scene.list.controls.size();
        kit::ChoiceCardLook card;
        card.title = source.title;
        card.hint = plan.hint;
        card.chosen = session.model.source == index;
        const kit::Rect rect = area.placed({left, list_top + tops[at], column, plan.height});
        const kit::ControlId id = source_id + index;
        kit::add_choice_card(
            scene.list, rect, card, id, control_name(Step::game, "source." + std::to_string(index))
        );
        order.insert(order.begin(), id);
        const int32_t chips_y = rect.y + kCardPad + metrics.regular_line + metrics.hint_gap +
                                plan.hint_h + metrics.hint_gap;
        for (const ChipSpot& spot : plan.spots) {
            kit::PillLook pill;
            pill.text = spot.text;
            pill.state = card.chosen ? kit::PillState::active : kit::PillState::neutral;
            kit::add_pill(
                scene.list,
                {text_left + spot.x, chips_y + spot.y, spot.width, chip_h},
                pill,
                kit::no_control,
                {}
            );
        }
        if (!source.footnote.empty()) {
            const int32_t note_y =
                plan.chip_block > 0 ? chips_y + plan.chip_block + metrics.hint_gap : chips_y;
            add_paragraph(
                scene.list,
                fonts,
                text_left,
                note_y,
                text_w,
                source.footnote,
                kit::FontRole::small,
                kit::colour::quiet
            );
        }
        if (scrolls)
            clip_since(scene.list, items_from, controls_from, clip);
    }
    if (scrolls)
        add_list_bar(scene.list, area, session.source_list);
    if (scrolls)
        row = list_top + room + metrics.padding;
    else if (count > 0)
        row = list_top + content_height + metrics.padding;

    const int32_t locate_w =
        std::min(footer_button_width(fonts, locate), std::max(column, int32_t{1}));
    const kit::Rect locate_rect{
        left + std::max((column - locate_w) / 2, int32_t{0}), row, locate_w, button_height
    };
    add_button(
        scene.list,
        order,
        locate_rect,
        locate_id,
        control_name(Step::game, "locate"),
        locate,
        true,
        kit::ButtonStyle::plain
    );
    row += button_height + 8;
    add_paragraph(
        scene.list,
        fonts,
        left,
        row,
        column,
        copies,
        kit::FontRole::small,
        kit::colour::quiet,
        kit::Align::centre
    );
    if (session.model.controller) {
        add_paragraph(
            scene.list,
            fonts,
            left,
            band.top + band.height - kit::compact_metrics.small_line,
            column,
            show("A continues. B goes back."),
            kit::FontRole::small,
            kit::colour::hint,
            kit::Align::centre
        );
    }
    scene.list.tab_order = std::move(order);
}

/// Lays the question a changed install asks over the copying step.
///
/// The step stays drawn under a dim. Only the question's buttons take the
/// keys and the pointer: the step's own controls and its list are left out
/// while it shows.
///
/// @param[in,out] scene the laid-out copying step
/// @param session the session
/// @param frame the room
/// @param fonts the fonts
void lay_changed_question(
    Scene& scene, const Session& session, const kit::Frame& frame, const kit::Fonts* fonts
) {
    kit::Question question = changed_question(session.model, frame.size_class);
    // The scene's focus ring marks the button the keys are on.
    question.marked = kit::no_control;
    const kit::PlacedQuestion placed = kit::place_question(question, fonts);
    const kit::DisplayList box = kit::question_list(question, fonts);
    const int32_t width = placed.width;
    const kit::Point at{
        std::max((scene.width - width) / 2, int32_t{0}),
        std::max((scene.height - placed.height) / 2, int32_t{0}),
    };
    scene.question = {at.x, at.y, width, placed.height};
    scene.list.controls.clear();
    scene.list.tab_order.clear();
    scene.list_scroll = {};
    scene.over_from = scene.list.items.size();
    kit::Item dim;
    dim.role = kit::Role::blend;
    dim.rect = {0, 0, scene.width, scene.height};
    dim.colour = kit::colour::backdrop;
    dim.opacity = kit::menu_backdrop_opacity;
    scene.list.items.push_back(dim);
    const auto renumbered = [](kit::ControlId id) {
        if (id == 0)
            return changed_choose_id;
        if (id == 1)
            return changed_leave_id;
        if (id == kit::notice_body)
            return changed_text_id;
        return kit::no_control;
    };
    const auto moved = [&at](kit::Rect rect) {
        if (rect.width > 0 && rect.height > 0)
            rect = {rect.x + at.x, rect.y + at.y, rect.width, rect.height};
        return rect;
    };
    // The question is drawn on a canvas placed at its corner, so its parts
    // keep its own points. Its controls are moved into the window's.
    scene.question_list.items = box.items;
    for (kit::Item& item : scene.question_list.items) {
        if (item.control != kit::no_control)
            item.control = renumbered(item.control);
    }
    for (kit::Control control : box.controls) {
        control.id = renumbered(control.id);
        if (control.id == kit::no_control)
            continue;
        control.rect = moved(control.rect);
        control.clip = moved(control.clip);
        control.name = control_name(
            Step::copying,
            control.id == changed_choose_id  ? "changed.choose"
            : control.id == changed_leave_id ? "changed.leave"
                                             : "changed.text"
        );
        scene.list.controls.push_back(std::move(control));
    }
    scene.list.tab_order = {changed_choose_id, changed_leave_id};
}

/// The copy row the copying step's list follows: the bundle being copied;
/// else the first that differs; else the first whose result is a note; else
/// the last that is done.
///
/// @param copy the copy
/// @return its index; -1 when no bundle has started
int32_t followed_bundle(const CopyView& copy) noexcept {
    const auto count = static_cast<int32_t>(copy.bundles.size());
    const auto first_where = [&](auto test) {
        for (int32_t index = 0; index < count; ++index) {
            if (test(copy.bundles[static_cast<std::size_t>(index)]))
                return index;
        }
        return int32_t{-1};
    };
    int32_t found =
        first_where([](const CopyBundle& bundle) { return bundle.phase == CopyPhase::copying; });
    if (found < 0)
        found =
            first_where([](const CopyBundle& bundle) { return bundle.phase == CopyPhase::differ; });
    if (found < 0)
        found = first_where([](const CopyBundle& bundle) {
            return bundle.phase == CopyPhase::done && bundle.note;
        });
    for (int32_t index = count - 1; found < 0 && index >= 0; --index) {
        if (copy.bundles[static_cast<std::size_t>(index)].phase == CopyPhase::done)
            found = index;
    }
    return found;
}

/// Lays the copying step out: the overall bar, one row a bundle, and the check.
///
/// The two lines at the band's foot keep their places: the note that a copy
/// carries on, then where the copies go, over the line the controller's hint
/// takes. The bundles' panel ends above them, and rows that do not fit
/// scroll inside it, with a scroll bar at its right.
///
/// @param[in,out] scene the scene
/// @param session the session
/// @param frame the room
/// @param fonts the fonts
void lay_copying(
    Scene& scene, const Session& session, const kit::Frame& frame, const kit::Fonts* fonts
) {
    const int32_t width = frame.area.width;
    const int32_t height = frame.area.height;
    std::vector<kit::ControlId> order;
    add_chrome(scene, order, session, width, height, false, true, nullptr);
    const Band band = content_band(width, height);
    // The column is the game step's: Settings' width, centred in the band.
    const kit::Metrics& metrics = kit::metrics_of(frame.size_class);
    const int32_t column = std::min(band.width, metrics.dialog_width);
    const int32_t left = band.left + std::max((band.width - column) / 2, int32_t{0});
    const CopyView& copy = session.model.copy;

    std::string subtitle;
    if (!copy.from.empty()) {
        subtitle = show("From {place}…");
        const std::string token = "{place}";
        const auto at = subtitle.find(token);
        if (at != std::string::npos)
            subtitle.replace(at, token.size(), copy.from);
        subtitle += " ";
        subtitle += show("Nothing is installed and your files are not changed.");
    } else {
        subtitle = show("Nothing is installed and your files are not changed.");
    }
    int32_t row = add_title(scene, fonts, band, show("Copying Total Annihilation"), subtitle);
    row += 8;
    if (!copy.overall.empty()) {
        add_text(
            scene.list,
            {left, row, column, kit::compact_metrics.regular_line},
            copy.overall,
            kit::FontRole::regular,
            kit::colour::text
        );
        row += kit::compact_metrics.regular_line;
    }
    if (!copy.remaining.empty()) {
        add_text(
            scene.list,
            {left, row, column, kit::compact_metrics.small_line},
            copy.remaining,
            kit::FontRole::small,
            kit::colour::quiet
        );
        row += kit::compact_metrics.small_line;
    }
    row += 4;
    kit::add_progress(scene.list, {left, row, column, 8}, copy.done, copy.whole);
    row += 12;
    if (copy.checked && !copy.verdict.empty()) {
        add_text(
            scene.list,
            {left, row, column, kit::compact_metrics.regular_line},
            copy.verdict,
            kit::FontRole::regular,
            kit::colour::text
        );
        row += kit::compact_metrics.regular_line;
    }
    row += 8;

    // The foot of the band, from the bottom up: the controller's hint, where
    // the copies go, and the note.
    const std::string note = show("If OA stops, the copy carries on from here next time.");
    const std::string destination = show("To Documents › Open Annihilation › Games and Expansions");
    const int32_t hint_top = band.top + band.height - kit::compact_metrics.small_line;
    const int32_t destination_top =
        hint_top - paragraph_height(fonts, column, destination, kit::FontRole::small);
    const int32_t note_top =
        destination_top - paragraph_height(fonts, column, note, kit::FontRole::small);

    if (!copy.bundles.empty()) {
        const int32_t pad = kit::compact_metrics.padding;
        // A row: the name, the file or the result, the bar, and the amount.
        constexpr int32_t kBarGapAbove = 4;
        constexpr int32_t kBarStep = 10;
        constexpr int32_t kRuleAbove = 8;
        constexpr int32_t kRuleStep = 9;
        const int32_t row_height = kit::compact_metrics.regular_line +
                                   kit::compact_metrics.small_line + kBarGapAbove + kBarStep +
                                   kit::compact_metrics.small_line;
        const auto count = static_cast<int32_t>(copy.bundles.size());
        const int32_t content_height =
            pad + count * row_height + (count - 1) * (kRuleAbove + kRuleStep) + pad;
        const int32_t room = std::max(note_top - 8 - row, int32_t{0});
        const bool scrolls = content_height > room;
        const int32_t panel_height = scrolls ? room : content_height;
        const kit::Rect panel_rect{left, row, column, panel_height};
        kit::Item panel;
        panel.role = kit::Role::fill;
        panel.rect = panel_rect;
        panel.colour = kit::colour::list;
        scene.list.items.push_back(panel);
        kit::Item edge;
        edge.role = kit::Role::bevel;
        edge.rect = panel_rect;
        edge.colour = kit::colour::edge_light;
        scene.list.items.push_back(edge);

        const int32_t inner_left = left + pad;
        int32_t inner_width = std::max(column - 2 * pad, int32_t{1});
        kit::ScrollArea area;
        // Inside the panel's raised edge.
        const kit::Rect clip{left + 1, row + 1, column - 2, std::max(panel_height - 2, int32_t{1})};
        if (scrolls) {
            const kit::Rect well{
                left + column - 1 - kBarGap - kWellWidth,
                row + 1 + kBarGap,
                kWellWidth,
                std::max(panel_height - 2 - 2 * kBarGap, int32_t{0})
            };
            inner_width = std::max(well.x - kBarGap - inner_left, int32_t{1});
            const int32_t followed = followed_bundle(copy);
            const int32_t follow_top =
                row + pad + followed * (row_height + kRuleAbove + kRuleStep) - kRuleAbove;
            area = list_area(
                panel_rect,
                well,
                content_height,
                session.copy_list,
                follow_top,
                followed < 0 ? 0 : row_height + 2 * kRuleAbove,
                followed == count - 1
            );
            scene.list_scroll = area;
        }
        const std::size_t rows_from = scene.list.items.size();

        int32_t y = row + pad - area.offset;
        for (int32_t index = 0; index < count; ++index) {
            const CopyBundle& bundle = copy.bundles[static_cast<std::size_t>(index)];
            add_text(
                scene.list,
                {inner_left, y, inner_width, kit::compact_metrics.regular_line},
                bundle.name,
                kit::FontRole::regular,
                kit::colour::text
            );
            y += kit::compact_metrics.regular_line;
            // The line under the name stays for the whole copy, so a bundle
            // that finishes does not pull the rows under it up. While the
            // bundle copies it is the file's name. Once the bundle is done
            // it is the result, and the file's name is not shown again.
            const bool copying_file = bundle.phase == CopyPhase::copying;
            const std::string& detail = copying_file ? bundle.file : bundle.amount;
            const kit::Colour detail_ink = copying_file                        ? kit::colour::hint
                                           : bundle.phase == CopyPhase::differ ? kit::colour::lock
                                           : bundle.note                       ? kit::colour::hint
                                                                               : kit::colour::quiet;
            if (!detail.empty()) {
                add_text(
                    scene.list,
                    {inner_left, y, inner_width, kit::compact_metrics.small_line},
                    detail,
                    kit::FontRole::small,
                    detail_ink
                );
            }
            y += kit::compact_metrics.small_line;
            y += kBarGapAbove;
            kit::add_progress(
                scene.list, {inner_left, y, inner_width, 6}, bundle.done, bundle.whole
            );
            y += kBarStep;
            // The amount under the bar is the live count while the file
            // copies. The same height stays once the result has moved up,
            // so the row does not change height.
            if (copying_file && !bundle.amount.empty()) {
                add_text(
                    scene.list,
                    {inner_left, y, inner_width, kit::compact_metrics.small_line},
                    bundle.amount,
                    kit::FontRole::small,
                    bundle.note ? kit::colour::hint : kit::colour::quiet
                );
            }
            y += kit::compact_metrics.small_line;
            if (index + 1 < count) {
                y += kRuleAbove;
                kit::Item rule;
                rule.role = kit::Role::rule;
                rule.rect = {inner_left, y, inner_width, 1};
                rule.colour = kit::colour::rule;
                scene.list.items.push_back(std::move(rule));
                y += kRuleStep;
            }
        }
        if (scrolls) {
            clip_since(scene.list, rows_from, scene.list.controls.size(), clip);
            add_list_bar(scene.list, area, session.copy_list);
        }
    }

    add_paragraph(
        scene.list,
        fonts,
        left,
        note_top,
        column,
        note,
        kit::FontRole::small,
        kit::colour::quiet,
        kit::Align::centre
    );
    if (session.model.controller) {
        add_paragraph(
            scene.list,
            fonts,
            left,
            hint_top,
            column,
            show("A continues. B goes back."),
            kit::FontRole::small,
            kit::colour::hint,
            kit::Align::centre
        );
    }
    add_paragraph(
        scene.list,
        fonts,
        left,
        destination_top,
        column,
        destination,
        kit::FontRole::small,
        kit::colour::quiet,
        kit::Align::centre
    );
    scene.list.tab_order = std::move(order);
    if (!copy.changed.empty())
        lay_changed_question(scene, session, frame, fonts);
}

/// How tall a coach card is for a body.
///
/// @param fonts the fonts; null estimates
/// @param width the card's width
/// @param body the body
/// @return the height, in points
int32_t coach_height(const kit::Fonts* fonts, int32_t width, std::string_view body) {
    const int32_t text =
        paragraph_height(fonts, std::max(width - 12, int32_t{1}), body, kit::FontRole::small);
    return 8 + kit::compact_metrics.regular_line + 4 + text + 8;
}

/// Lays the OA-buttons step out, over a clear layer.
///
/// @param[in,out] scene the scene
/// @param session the session
/// @param frame the room
/// @param fonts the fonts
void lay_buttons(
    Scene& scene, const Session& session, const kit::Frame& frame, const kit::Fonts* fonts
) {
    const int32_t width = frame.area.width;
    const int32_t height = frame.area.height;
    const kit::Rect room{0, 0, width, height};
    const StepPlace place = step_place(session.model.step, session.model.walk);
    kit::StepLook step;
    step.name = std::string(interface_text(place.name));
    step.pattern = std::string(interface_text(kit::step_line_pattern));
    step.index = place.index;
    step.count = place.count;
    const std::string kicker = kit::step_line(step);
    const std::string heading = show("Two buttons for everything Open Annihilation adds");
    const std::string paragraph = show(
        "The Library is where you get mods, map packs and languages. Settings is where you change "
        "how Open Annihilation looks and plays. Everything else on this menu is the game's own."
    );
    const int32_t button_height = kit::compact_metrics.button_height;
    const auto content_of = [&](int32_t panel_w) {
        const int32_t inner = std::max(panel_w - 20, int32_t{1});
        int32_t tall = 10;
        tall += kit::compact_metrics.small_line;
        tall += 6;
        tall += paragraph_height(fonts, inner, heading, kit::FontRole::regular);
        tall += 6;
        tall += paragraph_height(fonts, inner, paragraph, kit::FontRole::small);
        tall += 10 + button_height + 10;
        return tall;
    };
    int32_t panel_width = std::min(int32_t{360}, std::max(width / 3, int32_t{220}));
    int32_t panel_left = kit::compact_metrics.padding;
    int32_t panel_top = kit::compact_metrics.padding;
    int32_t panel_height = content_of(panel_width);
    int32_t limit = width - kit::compact_metrics.padding;
    const auto stop_before = [&](const kit::Rect& button) {
        if (button.width <= 0 || button.height <= 0)
            return;
        if (button.y + button.height <= panel_top || button.y >= panel_top + panel_height)
            return;
        limit = std::min(limit, button.x - 8);
    };
    stop_before(session.model.library_button);
    stop_before(session.model.settings_button);
    panel_width = std::min(panel_width, std::max(limit - panel_left, int32_t{0}));
    panel_height = content_of(std::max(panel_width, int32_t{1}));
    if (panel_width < 180) {
        panel_width = std::min(std::max(width - 2 * panel_left, int32_t{0}), int32_t{420});
        panel_height = content_of(std::max(panel_width, int32_t{1}));
        panel_top = std::max(height - panel_height - kit::compact_metrics.padding, int32_t{0});
    }
    const kit::Rect panel{panel_left, panel_top, panel_width, panel_height};

    const std::string library_body = show(
        "Get, update and switch mods, map packs and languages. The number counts every package you "
        "have added: mods, map packs, language packs and expansions."
    );
    const std::string settings_body = show(
        "Open Annihilation's own settings: controls, graphics, the interface's size, downloads and "
        "your files."
    );
    int32_t card_width = coach_card_width(frame.size_class);
    if (card_width > width - 16)
        card_width = std::max(width - 16, int32_t{80});
    const int32_t library_height = coach_height(fonts, card_width, library_body);
    const int32_t settings_height = coach_height(fonts, card_width, settings_body);
    const CoachCards coaches = place_coaches(
        room,
        panel,
        session.model.library_button,
        session.model.settings_button,
        card_width,
        library_height,
        settings_height
    );
    scene.coach_library = coaches.library;
    scene.coach_settings = coaches.settings;

    kit::Item ground;
    ground.role = kit::Role::fill;
    ground.rect = panel;
    ground.colour = kit::colour::panel;
    scene.list.items.push_back(std::move(ground));
    kit::Item edge;
    edge.role = kit::Role::outline;
    edge.rect = panel;
    edge.colour = kit::colour::accent;
    scene.list.items.push_back(std::move(edge));

    int32_t row = panel.y + 10;
    const int32_t text_width = std::max(panel.width - 20, int32_t{1});
    add_text(
        scene.list,
        {panel.x + 10, row, text_width, kit::compact_metrics.small_line},
        kicker,
        kit::FontRole::small,
        kit::colour::quiet
    );
    row += kit::compact_metrics.small_line;
    row += 6;
    row = add_paragraph(
        scene.list,
        fonts,
        panel.x + 10,
        row,
        text_width,
        heading,
        kit::FontRole::regular,
        kit::colour::text
    );
    row += 6;
    row = add_paragraph(
        scene.list,
        fonts,
        panel.x + 10,
        row,
        text_width,
        paragraph,
        kit::FontRole::small,
        kit::colour::hint
    );
    row += 10;

    const int32_t back_width = std::max(kit::compact_metrics.button_width, int32_t{72});
    const int32_t start_width = std::max(int32_t{140}, panel.width / 2);
    const kit::Rect back_rect{panel.x + 10, row, back_width, button_height};
    int32_t start_left = panel.x + panel.width - 10 - start_width;
    if (start_left < back_rect.x + back_rect.width + 8)
        start_left = back_rect.x + back_rect.width + 8;
    const int32_t start_w = std::max(panel.x + panel.width - 10 - start_left, int32_t{72});
    const kit::Rect start_rect{start_left, row, start_w, button_height};

    std::vector<kit::ControlId> order;
    add_button(
        scene.list,
        order,
        back_rect,
        back_id,
        control_name(Step::buttons, "back"),
        show("Back"),
        true,
        kit::ButtonStyle::plain
    );
    kit::CallToActionLook call;
    call.caption = show("Start playing");
    call.enabled = true;
    call.time_ms = session.pulse_ms;
    call.reduced_motion = frame.reduced_motion;
    kit::add_call_to_action(
        scene.list, start_rect, call, start_id, control_name(Step::buttons, "start")
    );
    order.push_back(start_id);

    kit::CoachMarkLook library_mark;
    library_mark.number = 1;
    library_mark.title = show("Library");
    library_mark.body = library_body;
    library_mark.target = session.model.library_button;
    kit::add_coach_mark(scene.list, coaches.library, library_mark);
    kit::CoachMarkLook settings_mark;
    settings_mark.number = 2;
    settings_mark.title = show("Settings");
    settings_mark.body = settings_body;
    settings_mark.target = session.model.settings_button;
    kit::add_coach_mark(scene.list, coaches.settings, settings_mark);
    scene.list.tab_order = std::move(order);
}

/// The list of the step on screen.
///
/// @param session the session
/// @return the game step's or the copying step's list; null on the other steps
ListScroll* list_of(Session& session) noexcept {
    if (session.model.step == Step::game)
        return &session.source_list;
    if (session.model.step == Step::copying)
        return &session.copy_list;
    return nullptr;
}

/// How far the step's list is moved up now. A list the player has scrolled
/// is where they left it, even when several turns or keys arrive before
/// the host lays the step out again.
///
/// @param session the session
/// @param scene the layout, with the list's scroll area
/// @return the offset, in points
int32_t list_offset(const Session& session, const Scene& scene) noexcept {
    const kit::ScrollArea& area = scene.list_scroll;
    const ListScroll* scroll = session.model.step == Step::game      ? &session.source_list
                               : session.model.step == Step::copying ? &session.copy_list
                                                                     : nullptr;
    if (scroll == nullptr || !scroll->held)
        return area.offset;
    return std::clamp(scroll->offset, int32_t{0}, std::max(area.limit, int32_t{0}));
}

/// Scrolls the step's list to an offset, and keeps it there.
///
/// @param[in,out] session the session
/// @param scene the layout, with the list's scroll area
/// @param offset how far the list is wanted moved up, in points
/// @return true when the list moved
bool scroll_list_to(Session& session, const Scene& scene, int32_t offset) noexcept {
    ListScroll* scroll = list_of(session);
    const kit::ScrollArea& area = scene.list_scroll;
    if (scroll == nullptr || area.limit <= 0)
        return false;
    const int32_t was = list_offset(session, scene);
    scroll->held = true;
    scroll->offset = std::clamp(offset, int32_t{0}, area.limit);
    return scroll->offset != was;
}

/// Takes a pointer on the step's list while the list scrolls.
///
/// A press on the scroll bar's thumb drags the thumb; a press on the bar
/// beside the thumb moves a page toward the press, then drags. A press on
/// the list where no control takes it drags the list. The press's moves and
/// its release go to the list until it is let go.
///
/// @param[in,out] session the session
/// @param scene the layout
/// @param kind where the finger is
/// @param at the finger, in the screen's points
/// @param[out] redraw true when the list or its bar changed
/// @return true when the list took the pointer
bool list_pointer(
    Session& session, const Scene& scene, PointerKind kind, kit::Point at, bool& redraw
) {
    redraw = false;
    ListScroll* scroll = list_of(session);
    if (scroll == nullptr)
        return false;
    const kit::ScrollArea& area = scene.list_scroll;
    constexpr int32_t kLeastThumb = kit::compact_metrics.least_thumb_height;
    if (scroll->dragging && kind == PointerKind::move) {
        int32_t wanted = scroll->press_offset - (at.y - scroll->press_y);
        if (scroll->by_bar) {
            kit::ScrollArea pressed = area;
            pressed.offset = std::clamp(scroll->press_offset, int32_t{0}, area.limit);
            const int32_t thumb_top = pressed.thumb(kLeastThumb).y + (at.y - scroll->press_y);
            wanted = area.offset_at(thumb_top, kLeastThumb);
        }
        redraw = scroll_list_to(session, scene, wanted);
        return true;
    }
    if (scroll->dragging) {
        redraw = scroll->by_bar;
        scroll->dragging = false;
        scroll->by_bar = false;
        if (kind == PointerKind::up)
            return true;
    }
    if (kind != PointerKind::down || area.limit <= 0)
        return false;
    kit::ScrollArea shown = area;
    shown.offset = list_offset(session, scene);
    if (kit::contains(area.hit, at)) {
        const kit::Rect thumb = shown.thumb(kLeastThumb);
        if (at.y < thumb.y)
            std::ignore = scroll_list_to(session, scene, shown.offset - area.page_step);
        else if (at.y >= thumb.y + thumb.height)
            std::ignore = scroll_list_to(session, scene, shown.offset + area.page_step);
        scroll->dragging = true;
        scroll->by_bar = true;
        scroll->press_y = at.y;
        scroll->press_offset = list_offset(session, scene);
        redraw = true;
        return true;
    }
    if (kit::contains(area.view, at) && kit::hit(scene.list, at) == kit::no_control) {
        scroll->dragging = true;
        scroll->by_bar = false;
        scroll->press_y = at.y;
        scroll->press_offset = shown.offset;
        return true;
    }
    return false;
}

/// Lets the game step's list follow a place again, from where it is now, so
/// it moves no more than it must to show that place.
///
/// @param[in,out] session the session
/// @param scene the layout the list is in now
void follow_places(Session& session, const Scene& scene) noexcept {
    session.source_list.offset = scene.list_scroll.offset;
    session.source_list.held = false;
}

/// Lets the game step's list follow again once the focus moves onto a place.
///
/// @param[in,out] session the session
/// @param scene the layout the list is in now
/// @param was the control the focus was on before
void follow_focused_place(Session& session, const Scene& scene, kit::ControlId was) noexcept {
    const kit::ControlId now = session.interaction.focused;
    const auto count = static_cast<kit::ControlId>(session.model.sources.size());
    if (session.model.step == Step::game && now != was && now >= source_id &&
        now < source_id + count)
        follow_places(session, scene);
}

/// What a control asks for.
///
/// @param[in,out] session the session
/// @param scene the layout
/// @param id the control
/// @return what the setup did
Outcome activate(Session& session, const Scene& scene, kit::ControlId id) {
    Outcome outcome;
    const kit::Control* control = kit::control_of(scene.list, id);
    if (control == nullptr || !control->enabled)
        return outcome;
    switch (id) {
    case skip_id:
        outcome.ask = session.model.leave == Leave::quit ? Ask::quit : Ask::skip;
        return outcome;
    case back_id:
        if (session.model.step != Step::language && session.model.step != Step::copying)
            outcome.ask = Ask::back;
        return outcome;
    case next_id:
    case start_id:
        if (session.model.step == Step::copying && session.model.copy.running &&
            !session.model.copy.checked)
            outcome.ask = Ask::pause_copy;
        else
            outcome.ask = Ask::advance;
        return outcome;
    case locate_id:
        outcome.ask = Ask::locate;
        return outcome;
    case changed_choose_id:
        outcome.ask = Ask::cancel_copy;
        return outcome;
    case changed_leave_id:
        outcome.ask = session.model.leave == Leave::quit ? Ask::quit : Ask::skip;
        return outcome;
    case on_id:
        session.model.online_on = true;
        session.interaction.focused = on_id;
        outcome.ask = Ask::choose_on;
        outcome.redraw = true;
        return outcome;
    case off_id:
        session.model.online_on = false;
        session.interaction.focused = off_id;
        outcome.ask = Ask::choose_off;
        outcome.redraw = true;
        return outcome;
    case cancel_id:
        outcome.ask = Ask::cancel_download;
        return outcome;
    case retry_id:
        outcome.ask = Ask::retry_download;
        return outcome;
    case previous_id:
        if (step_carousel(session.carousel, false)) {
            follow_chosen(session);
            outcome.redraw = true;
        }
        return outcome;
    case next_card_id:
        if (step_carousel(session.carousel, true)) {
            follow_chosen(session);
            outcome.redraw = true;
        }
        return outcome;
    default:
        break;
    }
    if (id >= source_id) {
        const int32_t index = id - source_id;
        if (index < 0 || index >= static_cast<int32_t>(session.model.sources.size()))
            return outcome;
        // The list follows the place chosen, even after the player scrolled it.
        if (session.source_list.held)
            outcome.redraw = true;
        follow_places(session, scene);
        if (session.model.source != index) {
            session.model.source = index;
            session.interaction.focused = id;
            outcome.redraw = true;
        }
        return outcome;
    }
    if (id >= card_id && id < source_id) {
        const int32_t index = id - card_id;
        if (index < 0 || index >= session.carousel.count)
            return outcome;
        if (index != session.carousel.selected) {
            kit::carousel_go(session.carousel, index);
            follow_chosen(session);
            outcome.redraw = true;
            return outcome;
        }
        if (!download_busy(session.model)) {
            outcome.ask = Ask::pick;
            outcome.index = index;
        }
        return outcome;
    }
    return outcome;
}

/// Dims the layer except two holes, and rings each hole.
///
/// @param canvas where it is drawn
/// @param layer the rectangle that dims
/// @param library the Library button's hole
/// @param settings the Settings button's hole
void dim_around(
    const kit::Canvas& canvas, const kit::Rect& layer, kit::Rect library, kit::Rect settings
) {
    if (layer.width <= 0 || layer.height <= 0)
        return;
    const kit::Rect holes[] = {library, settings};
    std::vector<int32_t> rows;
    rows.push_back(layer.y);
    rows.push_back(layer.y + layer.height);
    for (const kit::Rect& hole : holes) {
        const kit::Rect shared = kit::intersect(hole, layer);
        if (shared.width <= 0 || shared.height <= 0)
            continue;
        rows.push_back(shared.y);
        rows.push_back(shared.y + shared.height);
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());

    struct Span {
        int32_t begin{};
        int32_t end{};
    };

    for (std::size_t row_at = 0; row_at + 1 < rows.size(); ++row_at) {
        const int32_t top = rows[row_at];
        const int32_t bottom = rows[row_at + 1];
        if (bottom <= top)
            continue;
        std::vector<Span> open;
        for (const kit::Rect& hole : holes) {
            if (hole.width <= 0 || hole.height <= 0)
                continue;
            if (hole.y >= bottom || hole.y + hole.height <= top)
                continue;
            const int32_t left = std::max(hole.x, layer.x);
            const int32_t right = std::min(hole.x + hole.width, layer.x + layer.width);
            if (right > left)
                open.push_back(Span{left, right});
        }
        std::sort(open.begin(), open.end(), [](const Span& left, const Span& right) {
            return left.begin < right.begin;
        });
        std::vector<Span> merged;
        for (const Span& span : open) {
            if (merged.empty() || span.begin > merged.back().end)
                merged.push_back(span);
            else
                merged.back().end = std::max(merged.back().end, span.end);
        }
        int32_t cursor = layer.x;
        const int32_t limit = layer.x + layer.width;
        for (const Span& span : merged) {
            if (span.begin > cursor) {
                kit::blend_rect(
                    canvas,
                    {cursor, top, span.begin - cursor, bottom - top},
                    kit::colour::backdrop,
                    kit::spotlight_opacity
                );
            }
            cursor = std::max(cursor, span.end);
        }
        if (cursor < limit) {
            kit::blend_rect(
                canvas,
                {cursor, top, limit - cursor, bottom - top},
                kit::colour::backdrop,
                kit::spotlight_opacity
            );
        }
    }
    for (const kit::Rect& hole : holes) {
        if (hole.width > 0 && hole.height > 0)
            kit::outline_rect(canvas, hole, kit::colour::accent);
    }
}

/// True when the copy has been checked, matches, and is not still running.
bool copy_is_clear(const CopyView& copy) noexcept {
    if (!copy.checked || copy.running || copy.hashing || copy.paused || copy.verdict.empty())
        return false;
    for (const CopyBundle& bundle : copy.bundles) {
        if (bundle.phase == CopyPhase::differ)
            return false;
    }
    return true;
}

} // namespace

int32_t setup_scale_percent(
    int32_t room_width, int32_t room_height, int32_t least_percent, bool fractional
) noexcept {
    constexpr int64_t kWhole = 100;
    const int64_t across =
        int64_t{std::max(room_width, int32_t{0})} * kWhole / int64_t{setup_room.x};
    const int64_t down =
        int64_t{std::max(room_height, int32_t{0})} * kWhole / int64_t{setup_room.y};
    int64_t fitted = std::min(across, down);
    if (!fractional)
        fitted = fitted / kWhole * kWhole;
    const int64_t least = std::max(int64_t{least_percent}, kWhole);
    return static_cast<int32_t>(std::max(fitted, least));
}

kit::Frame setup_frame(
    int32_t room_width, int32_t room_height, int32_t least_percent, bool fractional
) noexcept {
    kit::Viewport room;
    room.width = std::max(room_width, int32_t{0});
    room.height = std::max(room_height, int32_t{0});
    room.scale_percent = setup_scale_percent(room.width, room.height, least_percent, fractional);
    return kit::frame_of(room);
}

void tick(Session& session, uint32_t dt_ms) noexcept {
    kit::carousel_tick(session.carousel, dt_ms);
    session.pulse_ms += dt_ms;
    if (session.model.step != Step::copying || session.copy_continue_stopped)
        return;
    const CopyView& copy = session.model.copy;
    if (copy.running || copy.hashing)
        session.copy_continue_armed = true;
    if (!copy_is_clear(copy)) {
        session.copy_continue_ms = -1;
        return;
    }
    if (!session.copy_continue_armed)
        return;
    if (session.copy_continue_ms < 0) {
        session.copy_continue_ms = copy_continue_wait_ms;
        return;
    }
    const uint32_t left = static_cast<uint32_t>(session.copy_continue_ms);
    const uint32_t gap = dt_ms > left ? left : dt_ms;
    session.copy_continue_ms -= static_cast<int32_t>(gap);
}

bool stop_copy_continue(Session& session) noexcept {
    if (session.model.step != Step::copying || session.copy_continue_ms < 0)
        return false;
    session.copy_continue_stopped = true;
    session.copy_continue_ms = -1;
    return true;
}

void reset_copy_continue(Session& session) noexcept {
    session.copy_continue_ms = -1;
    session.copy_continue_armed = false;
    session.copy_continue_stopped = false;
    session.copy_list = {};
}

void ensure_focus(Session& session, const kit::DisplayList& list) {
    const bool placed =
        session.focus_ready && kit::control_of(list, session.interaction.focused) != nullptr;
    if (placed) {
        // However the carousel moved, a focus on a card is on the chosen one.
        const kit::ControlId was = session.interaction.focused;
        if (follow_chosen(session) && kit::control_of(list, session.interaction.focused) == nullptr)
            session.interaction.focused = was;
        return;
    }
    session.focus_ready = true;
    session.interaction.focus_shown = true;
    kit::ControlId wanted = next_id;
    if (session.model.step == Step::language)
        wanted = card_id + session.carousel.selected;
    else if (session.model.step == Step::game)
        wanted = session.model.source >= 0 ? source_id + session.model.source : locate_id;
    else if (session.model.step == Step::online)
        wanted = session.model.online_on ? on_id : off_id;
    else if (session.model.step == Step::buttons)
        wanted = start_id;
    else
        wanted = next_id;
    if (kit::control_of(list, wanted) == nullptr && !list.tab_order.empty())
        wanted = list.tab_order.front();
    session.interaction.focused = wanted;
}

Scene lay_out(const Session& session, const kit::Frame& frame, const kit::Fonts* fonts) {
    Scene scene;
    scene.width = frame.area.width;
    scene.height = frame.area.height;
    if (frame.area.width <= 0 || frame.area.height <= 0)
        return scene;
    switch (session.model.step) {
    case Step::language:
        lay_language(scene, session, frame, fonts);
        break;
    case Step::game:
        lay_game(scene, session, frame, fonts);
        break;
    case Step::copying:
        lay_copying(scene, session, frame, fonts);
        break;
    case Step::online:
        lay_online(scene, session, frame, fonts);
        break;
    case Step::buttons:
        lay_buttons(scene, session, frame, fonts);
        break;
    }
    return scene;
}

Outcome pointer(
    Session& session,
    const Scene& scene,
    PointerKind kind,
    kit::Point at,
    int32_t finger_reach,
    uint8_t clicks
) {
    Outcome outcome;
    const bool stopped = kind != PointerKind::move && stop_copy_continue(session);
    bool list_moved = false;
    if (list_pointer(session, scene, kind, at, list_moved)) {
        outcome.redraw = list_moved || stopped;
        return outcome;
    }
    if (session.model.step == Step::language && !scene.places.empty()) {
        const kit::PointerPhase phase = kind == PointerKind::move   ? kit::PointerPhase::move
                                        : kind == PointerKind::down ? kit::PointerPhase::down
                                                                    : kit::PointerPhase::up;
        // Arrows are ordinary controls, so a press on either end wraps. The
        // carousel's own arrow handling stops at the first and last card.
        const kit::CarouselEvent event = kit::carousel_pointer(
            session.carousel, scene.places, {}, {}, session.drag, phase, at, clicks
        );
        if (kind == PointerKind::up && event != kit::CarouselEvent::none) {
            std::ignore = kit::pointer_up(session.interaction, scene.list, at);
            follow_chosen(session);
            outcome.redraw = true;
            if (event == kit::CarouselEvent::picked && !download_busy(session.model)) {
                outcome.ask = Ask::pick;
                outcome.index = session.carousel.selected;
            }
            return outcome;
        }
    }
    kit::PointerOutcome pressed{};
    switch (kind) {
    case PointerKind::move:
        pressed = kit::pointer_move(session.interaction, scene.list, at);
        break;
    case PointerKind::down:
        pressed = finger_reach > 0
                      ? kit::finger_down(session.interaction, scene.list, at, finger_reach)
                      : kit::pointer_down(session.interaction, scene.list, at);
        break;
    case PointerKind::up:
        pressed = kit::pointer_up(session.interaction, scene.list, at);
        break;
    }
    if (pressed.result == kit::PointerResult::activated)
        return activate(session, scene, pressed.control);
    if (pressed.result == kit::PointerResult::redraw || stopped)
        outcome.redraw = true;
    return outcome;
}

Outcome key(Session& session, const Scene& scene, kit::Key pressed) {
    Outcome outcome;
    if (stop_copy_continue(session))
        outcome.redraw = true;
    // On the last step both keys finish the walk, even when Back has the focus.
    if (session.model.step == Step::buttons &&
        (pressed == kit::Key::escape || pressed == kit::Key::enter)) {
        outcome.ask = Ask::advance;
        return outcome;
    }
    // A list that does not fit scrolls under the paging keys, and on the
    // copying step, whose list holds no control, under Up and Down too.
    if (list_of(session) != nullptr && scene.list_scroll.limit > 0) {
        const kit::ScrollArea& area = scene.list_scroll;
        const bool copying = session.model.step == Step::copying;
        const int32_t now = list_offset(session, scene);
        bool scrolls = true;
        int32_t wanted = now;
        if (pressed == kit::Key::page_up)
            wanted = now - area.page_step;
        else if (pressed == kit::Key::page_down)
            wanted = now + area.page_step;
        else if (pressed == kit::Key::home)
            wanted = 0;
        else if (pressed == kit::Key::end)
            wanted = area.limit;
        else if (copying && pressed == kit::Key::up)
            wanted = now - list_step;
        else if (copying && pressed == kit::Key::down)
            wanted = now + list_step;
        else
            scrolls = false;
        if (scrolls) {
            if (scroll_list_to(session, scene, wanted))
                outcome.redraw = true;
            return outcome;
        }
    }
    if (pressed == kit::Key::escape) {
        if (session.model.step == Step::copying && !session.model.copy.checked)
            outcome.ask = Ask::cancel_copy;
        else if (session.model.step != Step::language && session.model.step != Step::copying)
            outcome.ask = Ask::back;
        return outcome;
    }
    if (session.model.step == Step::game &&
        (pressed == kit::Key::up || pressed == kit::Key::down || pressed == kit::Key::left ||
         pressed == kit::Key::right)) {
        const int32_t count = static_cast<int32_t>(session.model.sources.size());
        if (count > 0) {
            const int32_t index = session.model.source < 0 ? 0 : session.model.source;
            const int32_t next = kit::choice_step(index, count, pressed);
            if (next != session.model.source)
                return activate(session, scene, source_id + next);
        }
    }
    const kit::ControlId focused = session.interaction.focused;
    const bool on_choice = focused == on_id || focused == off_id;
    if (session.model.step == Step::online && on_choice &&
        (pressed == kit::Key::up || pressed == kit::Key::down || pressed == kit::Key::left ||
         pressed == kit::Key::right)) {
        const int32_t index = session.model.online_on ? 0 : 1;
        const int32_t next = kit::choice_step(index, 2, pressed);
        if (next != index)
            return activate(session, scene, next == 0 ? on_id : off_id);
    }
    const kit::KeyOutcome taken = kit::key(session.interaction, scene.list, pressed);
    if (taken.result == kit::KeyResult::cancel) {
        if (session.model.step == Step::copying && !session.model.copy.checked)
            outcome.ask = Ask::cancel_copy;
        else if (session.model.step != Step::language && session.model.step != Step::copying)
            outcome.ask = Ask::back;
        return outcome;
    }
    if (taken.result == kit::KeyResult::redraw) {
        follow_focused_place(session, scene, focused);
        outcome.redraw = true;
        return outcome;
    }
    if (taken.result == kit::KeyResult::to_control) {
        if (pressed == kit::Key::left || pressed == kit::Key::right) {
            if (session.model.step == Step::language &&
                step_carousel(session.carousel, pressed == kit::Key::right)) {
                follow_chosen(session);
                outcome.redraw = true;
            }
            return outcome;
        }
        return activate(session, scene, taken.control);
    }
    if (taken.result == kit::KeyResult::accept)
        return activate(session, scene, session.interaction.focused);
    return outcome;
}

Outcome wheel(Session& session, const Scene& scene, float notches) {
    Outcome outcome;
    if (notches != 0.F && stop_copy_continue(session))
        outcome.redraw = true;
    if (ListScroll* scroll = list_of(session); scroll != nullptr) {
        const kit::ScrollArea& area = scene.list_scroll;
        if (area.limit > 0 && notches != 0.F) {
            const int32_t wanted = kit::wheel_offset(
                scroll->wheel, notches, list_step, list_offset(session, scene), area.limit
            );
            if (scroll_list_to(session, scene, wanted))
                outcome.redraw = true;
        }
        return outcome;
    }
    if (session.model.step != Step::language || session.carousel.count <= 1 || notches == 0.F)
        return outcome;
    // A turn away from the player is the previous card. One card does not move.
    if (step_carousel(session.carousel, notches < 0.F)) {
        follow_chosen(session);
        outcome.redraw = true;
    }
    return outcome;
}

void draw(const kit::Canvas& canvas, const Scene& scene, const Session& session) {
    kit::DisplayList list = scene.list;
    kit::mark_states(list, session.interaction);
    kit::DisplayList question = scene.question_list;
    kit::mark_states(question, session.interaction);
    const bool asking = scene.question.width > 0 && scene.question.height > 0;
    if (session.interaction.focus_shown) {
        if (const kit::Control* control = kit::control_of(list, session.interaction.focused)) {
            if (asking) {
                // The question's buttons are ringed on the question.
                kit::add_focus_ring(
                    question,
                    {control->rect.x - scene.question.x,
                     control->rect.y - scene.question.y,
                     control->rect.width,
                     control->rect.height},
                    true
                );
            } else {
                // A row of a list that scrolls rings only the part in view.
                const kit::Rect clip = control->clip;
                kit::add_focus_ring(list, control->rect, true);
                list.items.back().clip = clip;
            }
        }
    }
    if (session.model.step == Step::buttons) {
        const kit::Rect library = kit::grown(session.model.library_button, 4);
        const kit::Rect settings = kit::grown(session.model.settings_button, 4);
        dim_around(canvas, {0, 0, scene.width, scene.height}, library, settings);
    }
    // A question over the step is drawn last, over the step's title too.
    kit::DisplayList over;
    if (scene.over_from > 0 && scene.over_from <= list.items.size()) {
        over.items.assign(
            list.items.begin() + static_cast<std::ptrdiff_t>(scene.over_from), list.items.end()
        );
        list.items.resize(scene.over_from);
    }
    kit::paint(canvas, list);
    if (!scene.title_text.empty() && scene.title.width > 0 && scene.title.height > 0)
        kit::draw_prominent_line(canvas, scene.title_text, scene.title, kit::colour::text);
    if (scene.sample)
        draw_sample(canvas, scene.well_inset, session.model, scene.sample_class);
    kit::paint(canvas, over);
    if (asking)
        kit::paint(kit::placed_at(canvas, {scene.question.x, scene.question.y}), question);
}

kit::Question changed_question(const Model& model, kit::SizeClass size_class) {
    kit::Question question;
    question.word = "changed-install";
    question.size_class = size_class;
    question.title = show("Your Total Annihilation has been changed");
    question.paragraphs.push_back(
        {show(
             "Some of its files are not the size they are in Total Annihilation 3.1c and its "
             "expansions. A mod or another program may have changed them."
         ),
         false}
    );
    const std::vector<ChangedFile>& changed = model.copy.changed;
    const std::size_t named =
        changed.size() > changed_files_shown ? changed_files_shown - 1 : changed.size();
    for (std::size_t at = 0; at < named; ++at) {
        const ChangedFile& file = changed[at];
        question.paragraphs.push_back(
            {filled(
                 "{file}, from {bundle}, is {size} bytes. It should be {expected}.",
                 {{"file", file.name},
                  {"bundle", show(file.bundle)},
                  {"size", std::to_string(file.size)},
                  {"expected", std::to_string(file.expected)}}
             ),
             false}
        );
    }
    if (named < changed.size()) {
        question.paragraphs.push_back(
            {filled(
                 "And {n} more; the log has them all.",
                 {{"n", std::to_string(changed.size() - named)}}
             ),
             false}
        );
    }
    question.paragraphs.push_back(
        {show(
             "Open Annihilation needs a fresh Total Annihilation install, without mods, to set up "
             "its copy. Nothing has been copied. Choose another place to copy from."
         ),
         false}
    );
    question.buttons.push_back({show("Choose another place"), true, "choose"});
    const bool quits = model.leave == Leave::quit;
    question.buttons.push_back({show(quits ? "Quit" : "Skip"), false, quits ? "quit" : "skip"});
    question.cancel_button = 0;
    question.primary_button = 0;
    question.marked = 0;
    return question;
}

} // namespace oa::ui::first_run
