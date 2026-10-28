// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library screen on the OA layer (library_menu.hpp), library, which the
// main menu's LIBRARY opens (Runtime::push_library_menu_screen). It shows
// the Library's model (oa/ui/library/library.hpp) drawn with the game's art:
// the tabs Active, Mods, Maps, Updates and Registries; the languages are on
// Settings › Language & Text.
//
// Browsing, the search, the filters, the tags and the selection change the
// model as the Library's own screen changes it. Every other press is the
// Library's own action (Runtime::library_act), so that what GET, UPDATE,
// UNINSTALL, ROLL BACK, PLAY NOW, the switches, CHANGE…, the expansions'
// plates and CLOSE do and ask stays the Library's.

#include "library_host.hpp"
#include "library_menu.hpp"
#include "menu_paint.hpp"
#include "oa_layer.hpp"
#include "runtime_game_menu_art.hpp"

#include "oa/app/runtime.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/screen.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/library/uninstall.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace library = oa::ui::library;
namespace words = oa::ui::library::words;
using menu_paint::worded;

/// The tabs the screen shows, top to bottom. The languages are on
/// Settings › Language & Text.
constexpr std::array<library::Tab, 5> shown_tabs{
    library::Tab::active, library::Tab::mods, library::Tab::maps, library::Tab::updates,
    library::Tab::registries
};

/// The filters, in the order their plates show.
constexpr std::array<library::Filter, library::filter_total> shown_filters{
    library::Filter::all, library::Filter::newest, library::Filter::installed,
    library::Filter::updates
};

/// How many rows past each end of the view have their pictures asked for.
constexpr std::size_t picture_margin = 4;

/// Returns a word fit for an automation name: lower case letters, digits
/// and hyphens.
///
/// @param text the word as given
/// @return the word; "item" for one with none of them
std::string word_of(std::string_view text) {
    std::string word;
    for (const char character : text) {
        if (character >= 'A' && character <= 'Z')
            word += static_cast<char>(character - 'A' + 'a');
        else if ((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9'))
            word += character;
        else if (!word.empty() && word.back() != '-')
            word += '-';
    }
    while (!word.empty() && word.back() == '-')
        word.pop_back();
    return word.empty() ? std::string("item") : word;
}

/// Returns a text in capitals, its ASCII letters raised.
///
/// @param text the text
/// @return the text in capitals
std::string capitals(std::string text) {
    for (char& character : text)
        if (character >= 'a' && character <= 'z')
            character = static_cast<char>(character - 'a' + 'A');
    return text;
}

/// Returns a badge as the screen draws it.
///
/// @param badge the badge; null for none
/// @return the picture; empty for none
LibraryMenuPicture picture_of(const library::Badge* badge) {
    if (badge == nullptr || badge->width == 0 || badge->height == 0 ||
        badge->rgba.size() < static_cast<std::size_t>(badge->width) * badge->height * 4)
        return {};
    return {badge->rgba.data(), badge->width, badge->height};
}

/// Returns a minimap as the screen draws it.
///
/// @param map the minimap; null for none
/// @return the picture; empty for none
LibraryMenuPicture picture_of(const library::MapPicture* map) {
    if (map == nullptr || map->width == 0 || map->height == 0 ||
        map->rgba.size() < static_cast<std::size_t>(map->width) * map->height * 4)
        return {};
    return {map->rgba.data(), map->width, map->height};
}

/// Returns an entry's badge: its own, its listing's or its installed copy's.
///
/// @param entry the entry
/// @return the badge; null for none
const library::Badge* badge_of(const library::Entry& entry) {
    if (entry.badge != nullptr)
        return entry.badge;
    if (entry.installed != nullptr && entry.installed->badge)
        return &*entry.installed->badge;
    if (entry.listing != nullptr && entry.listing->badge)
        return &*entry.listing->badge;
    return nullptr;
}

/// Returns an entry's picture: its badge, or for a map pack without one
/// its first decoded minimap.
///
/// @param entry the entry
/// @return the picture; empty for none
LibraryMenuPicture entry_picture(const library::Entry& entry) {
    if (const library::Badge* badge = badge_of(entry))
        return picture_of(badge);
    if (entry.id.kind == library::Kind::map_pack && entry.listing != nullptr)
        for (const library::MapLine& map : entry.listing->maps)
            if (map.preview)
                return picture_of(&*map.preview);
    return {};
}

/// Returns an action's word, for automation.
///
/// @param action the action
/// @return the word
std::string_view action_word(library::Action action) noexcept {
    switch (action) {
    case library::Action::get:
        return "get";
    case library::Action::update:
        return "update";
    case library::Action::update_all:
        return "update-all";
    case library::Action::cancel:
        return "cancel";
    case library::Action::retry:
        return "retry";
    case library::Action::roll_back:
        return "roll-back";
    case library::Action::play_now:
        return "play-now";
    case library::Action::switch_off:
        return "switch-off";
    case library::Action::open_folder:
        return "open-folder";
    case library::Action::homepage:
        return "homepage";
    case library::Action::uninstall:
        return "uninstall";
    case library::Action::use_now:
        return "use-now";
    case library::Action::turn_on:
        return "turn-on";
    default:
        return "action";
    }
}

/// Joins two texts with a dot between them, leaving out an empty one.
///
/// @param left the first
/// @param right the second
/// @return the two
std::string joined(std::string_view left, std::string_view right) {
    if (left.empty())
        return std::string(right);
    if (right.empty())
        return std::string(left);
    return std::string(left) + " · " + std::string(right);
}

/// Returns a count of packages in words: "1 package", "7 packages".
///
/// @param text how texts are looked up
/// @param count the count
/// @return the words
std::string packages_text(const library::TextHooks& text, std::size_t count) {
    const std::string number = std::to_string(count);
    return library::filled(
        text, count == 1 ? words::registry_package_one : words::registry_package_many,
        {{"n", number}}
    );
}

/// Returns a registry's packages counted by kind, in the tabs' order and
/// leaving out a kind it has none of: "32 mods · 3334 map packs · 6 expansions".
///
/// @param registry the registry
/// @return the counts; empty when its catalogue lists nothing
std::string kinds_text(const library::Registry& registry) {
    const struct {
        std::size_t count;
        std::string_view one;
        std::string_view many;
    } kinds[] = {
        {registry.mods, "{n} mod", "{n} mods"},
        {registry.map_packs, "{n} map pack", "{n} map packs"},
        {registry.languages, "{n} language", "{n} languages"},
        {registry.expansions, "{n} expansion", "{n} expansions"},
    };
    std::string line;
    for (const auto& kind : kinds)
        if (kind.count != 0)
            line = joined(
                line,
                menu_paint::filled(
                    worded(kind.count == 1 ? kind.one : kind.many), "{n}", std::to_string(kind.count)
                )
            );
    return line;
}

} // namespace

/// What the Library screen reads and asks for. It keeps the
/// screen's facts until the model or a press moves them, and the entries,
/// filters, tags, registries and buttons each place of those facts names.
struct Runtime::LibraryMenuHost {
    /// Makes the host of a screen opening now.
    ///
    /// @param runtime the runtime
    explicit LibraryMenuHost(Runtime& runtime) : runtime_(runtime) {}

    /// Returns what the screen shows now, made again when the model or a
    /// press moved it.
    ///
    /// @return the facts
    [[nodiscard]] LibraryMenuFacts facts() {
        LibraryHost& host = runtime_.library_host();
        // The languages are not one of this screen's tabs.
        if (host.model.tab == library::Tab::languages)
            library::set_tab(host.model, library::Tab::active);
        const std::string language = worded("LIBRARY");
        // A switch of the mod made here ends the run: its line shows over
        // the screen as the window is kept for the next (keep_switch_picture).
        if (!built_ || host.model_revision != seen_model_ || revision_ != seen_revision_ ||
            language != seen_language_) {
            seen_model_ = host.model_revision;
            seen_revision_ = revision_;
            seen_language_ = language;
            built_ = true;
            made_ = make(host);
            ask_pictures();
        }
        made_.switching =
            runtime_.soft_restart_requested_ ? runtime_.restart_line_ : std::string();
        made_.question.reset();
        if (host.menu_question) {
            const oa::ui::kit::Question& asked = *host.menu_question;
            LibraryMenuQuestion& shown = made_.question.emplace();
            shown.title = asked.title;
            for (const oa::ui::kit::Paragraph& paragraph : asked.paragraphs)
                shown.paragraphs.push_back(paragraph.text);
            for (const oa::ui::kit::QuestionButton& button : asked.buttons)
                shown.buttons.push_back({button.id, button.caption, true});
            const auto place = [&shown](int32_t button) {
                return static_cast<std::size_t>(std::clamp<int32_t>(
                    button, 0, std::max(static_cast<int32_t>(shown.buttons.size()) - 1, 0)
                ));
            };
            shown.cancel = place(asked.cancel_button);
            shown.primary = place(asked.primary_button);
            shown.marked = place(static_cast<int32_t>(asked.marked));
        }
        return made_;
    }

    /// Carries out a press.
    ///
    /// @param asked the press
    /// @return true when the Library closes
    bool press(const LibraryMenuPress& asked) {
        LibraryHost& host = runtime_.library_host();
        library::Library& model = host.model;
        ++revision_;
        library::ScreenResult result;
        switch (asked.command) {
        case LibraryMenuCommand::tab:
            if (asked.index < shown_tabs.size()) {
                library::set_tab(model, shown_tabs[asked.index]);
                library::close_details(model);
            }
            return false;
        case LibraryMenuCommand::filter:
            if (asked.index < shown_filters.size())
                library::set_filter(model, shown_filters[asked.index]);
            return false;
        case LibraryMenuCommand::tag:
            library::set_tag(model, asked.index < tags_.size() ? tags_[asked.index] : std::string());
            return false;
        case LibraryMenuCommand::search:
            library::set_query(model, asked.text);
            host.state.search = {model.query, model.query.size()};
            return false;
        case LibraryMenuCommand::select:
            if (asked.index < rows_.size())
                library::select(model, rows_[asked.index]);
            ask_pictures();
            return false;
        case LibraryMenuCommand::row_mark:
            if (asked.index >= rows_.size())
                return false;
            library::select(model, rows_[asked.index]);
            if (const library::Entry* entry = library::find_entry(model, rows_[asked.index])) {
                const std::vector<library::ActionButton> buttons = library::entry_actions(model, *entry);
                if (!buttons.empty() && buttons.front().enabled &&
                    (buttons.front().action == library::Action::get ||
                     buttons.front().action == library::Action::update)) {
                    result.action = buttons.front().action;
                    result.entry = entry->id;
                    return runtime_.library_act(result);
                }
            }
            return false;
        case LibraryMenuCommand::details_tag:
            if (asked.index < details_tags_.size())
                library::set_tag(model, details_tags_[asked.index]);
            return false;
        case LibraryMenuCommand::details_button:
            if (asked.index >= details_actions_.size())
                return false;
            result.action = details_actions_[asked.index];
            result.entry = details_entry_;
            result.registry = details_registry_;
            return runtime_.library_act(result);
        case LibraryMenuCommand::left_button:
            if (asked.index >= left_actions_.size())
                return false;
            result.action = left_actions_[asked.index];
            return runtime_.library_act(result);
        case LibraryMenuCommand::refresh:
            if (!library::can_check_now(model))
                return false;
            result.action = library::Action::check_now;
            return runtime_.library_act(result);
        case LibraryMenuCommand::close:
            result.action = library::Action::close;
            return runtime_.library_act(result);
        case LibraryMenuCommand::change_base:
            result.action = library::Action::change_base_game;
            return runtime_.library_act(result);
        case LibraryMenuCommand::language:
            runtime_.settings_menu_first_category_ =
                std::string(oa::ui::engine_settings::page_word(oa::ui::engine_settings::Page::language));
            result.action = library::Action::settings;
            return runtime_.library_act(result);
        case LibraryMenuCommand::expansion:
            return press_expansion(asked.index);
        case LibraryMenuCommand::choose_mod:
            library::set_tab(model, library::Tab::mods);
            library::close_details(model);
            return false;
        case LibraryMenuCommand::no_mod:
            result.action = library::Action::switch_off;
            for (const library::Entry& entry : model.entries)
                if (entry.id.kind == library::Kind::mod && entry.installed != nullptr &&
                    entry.installed->playing) {
                    result.entry = entry.id;
                    break;
                }
            return runtime_.library_act(result);
        case LibraryMenuCommand::browse_maps:
            library::set_tab(model, library::Tab::maps);
            library::close_details(model);
            return false;
        case LibraryMenuCommand::chooser_mark:
            if (asked.index < model.chooser.rows.size())
                library::mark_base_game(model, model.chooser.rows[asked.index].id);
            return false;
        case LibraryMenuCommand::chooser_use: {
            const library::BaseUse used = library::use_marked_base_game(model);
            if (used.kind != library::BaseUseKind::needs_source)
                return false;
            result.action = library::Action::copy_base_game;
            result.base_game = used.id;
            return runtime_.library_act(result);
        }
        case LibraryMenuCommand::chooser_cancel:
            library::close_base_chooser(model);
            return false;
        case LibraryMenuCommand::registry_select:
            if (asked.index < registries_.size())
                registry_ = registries_[asked.index];
            return false;
        case LibraryMenuCommand::registry_switch:
            if (asked.index >= registries_.size())
                return false;
            result.action = library::Action::set_registry;
            result.registry = registries_[asked.index];
            result.registry_on = asked.on;
            return runtime_.library_act(result);
        case LibraryMenuCommand::registry_refresh:
            if (asked.index >= registries_.size())
                return false;
            result.action = library::Action::refresh_registry;
            result.registry = registries_[asked.index];
            return runtime_.library_act(result);
        case LibraryMenuCommand::add_registry:
            result.action = library::Action::add_registry;
            return runtime_.library_act(result);
        case LibraryMenuCommand::question_button: {
            // The question closes before its answer acts, which may ask another.
            const std::function<void(int32_t)> answer = std::move(host.menu_answer);
            host.menu_question.reset();
            host.menu_answer = {};
            if (answer)
                answer(static_cast<int32_t>(asked.index));
            return false;
        }
        }
        return false;
    }

    /// Keeps what the screen shows, and asks for the pictures it needs.
    ///
    /// @param view the rows and maps in view
    void viewed(const LibraryMenuView& view) {
        view_ = view;
        ask_pictures();
    }

    /// Does what the Library's leaving does.
    void closed() {
        // A question over the Library goes with it.
        LibraryHost& host = runtime_.library_host();
        host.menu_question.reset();
        host.menu_answer = {};
        runtime_.library_screen_closed();
    }

  private:

    /// Asks for the pictures of the rows in view and a few beyond, the
    /// selected one's first, and decodes a few minimaps of the maps and
    /// packs shown.
    void ask_pictures() {
        LibraryHost& host = runtime_.library_host();
        const library::Library& model = host.model;
        std::vector<std::size_t> places;
        if (model.selected)
            for (std::size_t place = 0; place < model.visible.size(); ++place)
                if (model.entries[model.visible[place]].id == *model.selected) {
                    places.push_back(place);
                    break;
                }
        const std::size_t first = view_.first_row > picture_margin ? view_.first_row - picture_margin : 0;
        const std::size_t end = std::min(view_.end_row + picture_margin, model.visible.size());
        for (std::size_t place = first; place < end; ++place)
            places.push_back(place);
        host.request_row_pictures(places);
        const std::size_t first_row = view_.first_row;
        const std::size_t end_row = view_.end_row;
        const std::size_t maps = view_.maps;
        host.pump_map_previews(
            runtime_,
            [maps](std::size_t number) { return number >= 1 && number <= maps; },
            [&model, first_row, end_row](const library::EntryId& id) {
                if (model.selected && *model.selected == id)
                    return true;
                for (std::size_t place = first_row; place < end_row && place < model.visible.size(); ++place)
                    if (model.entries[model.visible[place]].id == id)
                        return true;
                return false;
            }
        );
    }

    /// Presses an expansion's plate, as the Library's own does.
    ///
    /// @param index the plate, by its place
    /// @return true when the Library closes
    bool press_expansion(std::size_t index) {
        LibraryHost& host = runtime_.library_host();
        if (index >= expansions_.size())
            return false;
        const library::ContentPress pressed = library::press_expansion(host.model, expansions_[index]);
        ++host.model_revision;
        library::ScreenResult result;
        result.expansion = pressed.expansion;
        result.registry = pressed.registry;
        switch (pressed.kind) {
        case library::ContentPressKind::none:
        case library::ContentPressKind::redraw:
            return false;
        case library::ContentPressKind::ask:
            result.action = library::Action::ask_expansion_off;
            break;
        case library::ContentPressKind::add:
            result.action = library::Action::add_expansion;
            break;
        case library::ContentPressKind::download:
            result.action = library::Action::get;
            break;
        case library::ContentPressKind::cancel_download:
            result.action = library::Action::cancel;
            break;
        case library::ContentPressKind::retry_download:
            result.action = library::Action::retry;
            break;
        }
        return runtime_.library_act(result);
    }

    /// Makes the facts from the model.
    ///
    /// @param host the Library's host
    /// @return the facts
    LibraryMenuFacts make(LibraryHost& host) {
        library::Library& model = host.model;
        const library::TextHooks& text = model.text;
        LibraryMenuFacts made;
        rows_.clear();
        tags_.clear();
        details_tags_.clear();
        details_actions_.clear();
        details_entry_ = {};
        details_registry_.clear();
        left_actions_.clear();
        registries_.clear();
        expansions_.clear();

        for (std::size_t at = 0; at < shown_tabs.size(); ++at) {
            const library::Tab tab = shown_tabs[at];
            LibraryMenuTab shown;
            const std::string name(library::tab_text(tab, false, text));
            shown.id = word_of(library::tab_text(tab, false, {}));
            if (tab == library::Tab::active)
                shown.name = name;
            else
                shown.name = library::filled(
                    text, words::tab_with_count,
                    {{"tab", name}, {"count", std::to_string(library::tab_count(model, tab))}}
                );
            switch (tab) {
            case library::Tab::active:
                shown.icon = LibraryMenuIcon::active;
                break;
            case library::Tab::mods:
                shown.icon = LibraryMenuIcon::mods;
                shown.after_divider = true;
                break;
            case library::Tab::maps:
                shown.icon = LibraryMenuIcon::maps;
                break;
            case library::Tab::updates:
                shown.icon = LibraryMenuIcon::updates;
                shown.attention = library::update_count(model) > 0;
                break;
            default:
                shown.icon = LibraryMenuIcon::registries;
                shown.after_divider = true;
                break;
            }
            made.tabs.push_back(std::move(shown));
            if (tab == model.tab) {
                made.chosen = at;
                made.heading = capitals(name);
            }
        }
        std::size_t installed = 0;
        for (const library::Installed& package : model.inputs.installed)
            if (package.kind != library::Kind::language)
                ++installed;
        made.installed = menu_paint::filled(
            worded("Installed: {count}"), "{count}", packages_text(text, installed)
        );
        made.refresh_enabled = library::can_check_now(model);
        made.refreshing = model.inputs.checking;
        made.queue = library::queue_lines(model).first;
        const bool in_match = model.inputs.in_match;

        switch (model.tab) {
        case library::Tab::active:
            made.page = LibraryMenuPage::active;
            make_active(model, made);
            made.left_buttons.push_back(
                {"update-all", std::string(library::action_text(library::Action::update_all, text)),
                 library::update_count(model) > 0 && !in_match}
            );
            left_actions_.push_back(library::Action::update_all);
            break;
        case library::Tab::registries:
            made.page = LibraryMenuPage::registries;
            make_registries(host, made);
            made.left_buttons.push_back(
                {"add-registry", std::string(library::shown(text, words::registry_add)), !in_match}
            );
            left_actions_.push_back(library::Action::add_registry);
            break;
        default:
            made.page = LibraryMenuPage::list;
            make_list(model, made);
            made.left_buttons.push_back(
                {"update-all", std::string(library::action_text(library::Action::update_all, text)),
                 library::update_count(model) > 0 && !in_match}
            );
            left_actions_.push_back(library::Action::update_all);
            break;
        }
        return made;
    }

    /// Makes the facts of Mods, Maps or Updates: the search, the filters,
    /// the tags, the rows and the chosen row's details.
    ///
    /// @param model the Library
    /// @param[in,out] made the facts
    void make_list(const library::Library& model, LibraryMenuFacts& made) {
        const library::TextHooks& text = model.text;
        LibraryMenuList& list = made.list;
        list.search = model.query;
        switch (model.tab) {
        case library::Tab::maps:
            list.search_hint = std::string(library::shown(text, words::search_maps));
            break;
        case library::Tab::updates:
            list.search_hint = std::string(library::shown(text, words::search_updates));
            break;
        default:
            list.search_hint = std::string(library::shown(text, words::search_mods));
            break;
        }
        const library::Filter filter = library::shown_filter(model);
        for (const library::Filter shown : shown_filters)
            list.filters.push_back(
                {word_of(library::filter_text(shown, {})), library::filter_caption(model, shown),
                 shown == filter}
            );
        for (const auto& [tag, count] : library::tags_in_tab(model)) {
            const bool chosen = tag == model.tag;
            list.tag_chosen = list.tag_chosen || chosen;
            list.tags.push_back(
                {tag, library::filled(text, words::tag_with_count, {{"tag", tag}, {"count", std::to_string(count)}}),
                 chosen}
            );
            tags_.push_back(tag);
        }
        for (const std::size_t index : model.visible) {
            const library::Entry& entry = model.entries[index];
            LibraryMenuRow row;
            row.id = (entry.id.registry.empty() ? std::string() : word_of(entry.id.registry) + ".") +
                     word_of(entry.id.key);
            const std::string_view name = library::entry_name(entry);
            row.name = std::string(name);
            row.letters = library::registry_initials(name);
            row.picture = entry_picture(entry);
            const bool queued = entry.queued != nullptr &&
                                entry.queued->phase != library::QueueItem::Phase::installed;
            if (queued) {
                row.line = library::queue_status_text(*entry.queued, text);
                row.line_warns = true;
                if (entry.queued->phase == library::QueueItem::Phase::downloading &&
                    entry.queued->total_bytes > 0)
                    row.progress = static_cast<double>(entry.queued->done_bytes) /
                                   static_cast<double>(entry.queued->total_bytes);
            } else if (!entry.blocked.empty()) {
                row.line = entry.blocked;
                row.line_warns = true;
            } else {
                std::string summary(library::entry_summary(entry));
                if (summary.empty())
                    summary = entry.shown_summary;
                row.line = joined(library::byline(entry, text), summary);
            }
            if (entry.installed != nullptr)
                row.version = library::release_text(entry.installed->version, entry.installed->revision, text);
            else if (entry.listing != nullptr)
                row.version = library::release_text(entry.listing->version, entry.listing->revision, text);
            if (entry.update != nullptr && entry.update->size > 0)
                row.size = library::size_text(entry.update->size, text);
            else if (entry.listing != nullptr && entry.listing->size > 0)
                row.size = library::size_text(entry.listing->size, text);
            switch (entry.state) {
            case library::State::get:
                row.mark = LibraryMenuMark::get;
                break;
            case library::State::update:
                row.mark = LibraryMenuMark::update;
                break;
            case library::State::installed:
            case library::State::playing:
                row.mark = library::is_base_game(entry.id) ? LibraryMenuMark::none
                                                           : LibraryMenuMark::installed;
                break;
            }
            if (row.mark == LibraryMenuMark::get || row.mark == LibraryMenuMark::update) {
                const std::vector<library::ActionButton> buttons = library::entry_actions(model, entry);
                row.mark_pressable = !buttons.empty() && buttons.front().enabled &&
                                     (buttons.front().action == library::Action::get ||
                                      buttons.front().action == library::Action::update);
                row.dimmed = !entry.can_act;
            }
            if (model.selected && *model.selected == entry.id)
                list.selected = rows_.size();
            rows_.push_back(entry.id);
            list.rows.push_back(std::move(row));
        }
        if (!model.query.empty())
            list.empty = library::filled(text, words::empty_matches, {{"query", model.query}});
        else if (model.tab == library::Tab::updates)
            list.empty = std::string(library::shown(text, words::empty_updates));
        else
            list.empty = std::string(library::shown(text, words::empty_tab));
        if (const library::Entry* entry = library::selected_entry(model))
            made.details = entry_details(model, *entry, true);
    }

    /// Makes the details of an entry: its badge, name, status, summary,
    /// tags, facts, note and buttons.
    ///
    /// @param model the Library
    /// @param entry the entry
    /// @param pressable its buttons are the details' buttons
    /// @return the details
    LibraryMenuDetails entry_details(
        const library::Library& model, const library::Entry& entry, bool pressable
    ) {
        const library::TextHooks& text = model.text;
        LibraryMenuDetails details;
        const std::string_view name = library::entry_name(entry);
        details.title = std::string(name);
        details.letters = library::registry_initials(name);
        details.picture = entry_picture(entry);
        details.status = library::status_line(model, entry);
        details.status_warns = entry.state == library::State::update || !entry.blocked.empty() ||
                               (entry.queued != nullptr &&
                                entry.queued->phase == library::QueueItem::Phase::failed);
        details.summary = std::string(library::entry_summary(entry));
        if (details.summary.empty())
            details.summary = entry.shown_summary;
        if (entry.listing != nullptr)
            details.tags = entry.listing->tags;
        for (const library::Fact& fact : library::facts(model, entry))
            details.facts.push_back(
                {fact.long_label, fact.long_value, fact.mark == library::Mark::warn}
            );
        if (!pressable)
            return details;
        details.note = library::details_note(model);
        details_entry_ = entry.id;
        details_tags_ = details.tags;
        for (const library::ActionButton& button : library::entry_actions(model, entry)) {
            details.buttons.push_back(
                {std::string(action_word(button.action)),
                 button.text.empty() ? std::string(library::action_text(button.action, text)) : button.text,
                 button.enabled}
            );
            details_actions_.push_back(button.action);
        }
        return details;
    }

    /// Makes the facts of the Active tab: the five tiles, what DETAILS
    /// tells of each, and the base game chooser while it is open.
    ///
    /// @param model the Library
    /// @param[in,out] made the facts
    void make_active(const library::Library& model, LibraryMenuFacts& made) {
        const library::TextHooks& text = model.text;
        LibraryMenuActive& active = made.active;
        const library::ActiveContent content = library::active_content(model);
        const library::ActiveSetup setup = library::active_setup(model);

        active.base_letters = content.letters;
        active.base_picture = picture_of(content.badge);
        active.base_name = content.name;
        active.base_version = content.version_line;
        active.base_check = content.check_line;
        active.base_check_good = content.check_good;
        active.change_enabled = content.change_enabled;

        active.language_name = setup.language_name;
        if (setup.language_badge != nullptr) {
            active.language_picture = picture_of(setup.language_badge);
            active.language_letters = library::registry_initials(setup.language_name);
        }
        active.language_line = !setup.language_note.empty()
                                   ? setup.language_note
                                   : joined(setup.language_version, setup.language_coverage);

        active.expansions_heading = content.expansions_heading;
        for (const library::ExpansionPill& pill : content.pills) {
            LibraryMenuExpansion shown;
            shown.id = pill.id;
            shown.name = pill.text;
            shown.on = pill.face == library::ExpansionFace::on;
            shown.enabled = pill.arrow ? pill.arrow_enabled : pill.enabled;
            shown.locked = pill.locked;
            if (pill.arrow) {
                switch (*pill.arrow) {
                case library::ExpansionArrow::get:
                    shown.arrow = LibraryMenuExpansionArrow::get;
                    break;
                case library::ExpansionArrow::cancel:
                    shown.arrow = LibraryMenuExpansionArrow::cancel;
                    break;
                case library::ExpansionArrow::retry:
                    shown.arrow = LibraryMenuExpansionArrow::retry;
                    break;
                }
            }
            active.expansions.push_back(std::move(shown));
            expansions_.push_back(pill.id);
        }
        active.expansions_hint = content.hint;
        active.expansions_lock = content.lock;

        const library::Entry* playing = nullptr;
        for (const library::Entry& entry : model.entries)
            if (entry.id.kind == library::Kind::mod && entry.installed != nullptr &&
                entry.installed->playing) {
                playing = &entry;
                break;
            }
        if (setup.mod_name.empty()) {
            active.mod_name = setup.base_title;
            active.mod_line = setup.base_look;
        } else {
            active.mod_name = setup.mod_name;
            active.mod_line = joined(setup.mod_release, setup.mod_author);
        }
        active.mod_letters = library::registry_initials(active.mod_name);
        active.mod_picture = picture_of(setup.mod_badge);
        active.mod_state = setup.mod_state;
        active.no_mod_enabled = playing != nullptr && library::can_switch(model);

        active.maps_heading = setup.maps.empty()
                                  ? std::string(library::shown(text, words::active_heading_maps))
                                  : joined(library::shown(text, words::active_heading_maps),
                                           std::to_string(setup.maps.size()));
        for (const library::ActiveMap& map : setup.maps)
            active.maps.push_back({map.name, picture_of(map.preview)});
        active.maps_empty = std::string(library::shown(text, words::active_no_maps));

        // What DETAILS tells of each tile, in LibraryMenuTile's order.
        active.details.resize(library_menu_tile_count);
        LibraryMenuDetails& base = active.details[static_cast<std::size_t>(LibraryMenuTile::base)];
        base.title = content.name;
        base.letters = content.letters;
        base.picture = active.base_picture;
        base.status = content.version_line;
        base.summary = worded(
            "The game Open Annihilation plays, from the files you added. CHANGE… chooses another "
            "one you added, or adds one."
        );
        if (!content.check_line.empty())
            base.facts.push_back({worded("Files"), content.check_line, !content.check_good});
        base.facts.push_back(
            {worded("Expansions"),
             std::to_string(std::count_if(content.pills.begin(), content.pills.end(),
                                          [](const library::ExpansionPill& pill) {
                                              return pill.face == library::ExpansionFace::on;
                                          })) +
                 " / " + std::to_string(content.pills.size()),
             false}
        );

        LibraryMenuDetails& language = active.details[static_cast<std::size_t>(LibraryMenuTile::language)];
        language.title = setup.language_name;
        language.letters = active.language_letters;
        language.picture = active.language_picture;
        language.badge = setup.language_badge != nullptr;
        language.status = setup.language_english.empty() ? setup.language_note : setup.language_english;
        language.summary = worded(
            "The language the game shows its words in. Its SETTINGS button opens Settings › "
            "Language & Text, where the languages are chosen and got."
        );
        if (!setup.language_pack.empty())
            language.facts.push_back({worded("Pack"), setup.language_pack, false});
        if (!setup.language_version.empty())
            language.facts.push_back(
                {std::string(library::shown(text, words::fact_installed)), setup.language_version, false}
            );
        if (!setup.language_coverage.empty())
            language.facts.push_back(
                {std::string(library::shown(text, words::fact_coverage)), setup.language_coverage, false}
            );

        LibraryMenuDetails& expansions =
            active.details[static_cast<std::size_t>(LibraryMenuTile::expansions)];
        expansions.title = content.expansions_heading;
        expansions.badge = false;
        expansions.summary = content.hint.empty()
                                 ? worded("Press an expansion to turn it on or off. The change "
                                          "applies when the Library closes.")
                                 : content.hint;
        for (const library::ExpansionPill& pill : content.pills)
            expansions.facts.push_back(
                {pill.text,
                 worded(pill.face == library::ExpansionFace::on ? "On"
                        : pill.arrow                            ? "Not copied"
                                                                : "Off"),
                 pill.locked}
            );
        expansions.note = content.lock;

        LibraryMenuDetails& mod = active.details[static_cast<std::size_t>(LibraryMenuTile::mod)];
        if (playing != nullptr) {
            mod = entry_details(model, *playing, false);
            mod.note = library::playing_note(model, *playing);
        } else {
            mod.title = active.mod_name;
            mod.letters = active.mod_letters;
            mod.picture = active.mod_picture;
            mod.status = setup.mod_state;
            mod.summary = active.mod_line;
        }
        if (mod.summary.empty())
            mod.summary = worded("CHOOSE MOD shows the mods, and PLAY NOW there plays one.");

        LibraryMenuDetails& maps = active.details[static_cast<std::size_t>(LibraryMenuTile::maps)];
        maps.title = std::string(library::shown(text, words::active_heading_maps));
        maps.badge = false;
        maps.status = active.maps_heading;
        if (setup.maps.empty()) {
            maps.summary = active.maps_empty;
        } else {
            std::string names;
            for (std::size_t at = 0; at < setup.maps.size() && at < 12; ++at)
                names = names.empty() ? setup.maps[at].line : names + "\n" + setup.maps[at].line;
            maps.summary = names;
            maps.facts.push_back(
                {worded("Packs"), std::to_string(model.inputs.map_packs.size()), false}
            );
        }

        if (model.content.pending_base)
            active.note = std::string(library::shown(text, words::base_applies));
        else if (model.content.edited)
            active.note = worded("Changes apply when the Library closes.");

        const library::BaseChooser& chooser = model.chooser;
        active.chooser_open = chooser.open;
        if (chooser.open) {
            active.chooser_lead = std::string(library::shown(text, words::base_lead));
            for (const library::BaseGameRow& row : chooser.rows)
                active.chooser_rows.push_back(
                    {row.id, row.letters, picture_of(row.badge), joined(row.name, row.version),
                     row.state_text, row.detail, row.marked}
                );
            active.chooser_note = chooser.note;
            active.chooser_problem = chooser.problem;
        }
    }

    /// Makes the facts of the Registries tab: a card for each registry and
    /// the details of the one chosen.
    ///
    /// @param host the Library's host
    /// @param[in,out] made the facts
    void make_registries(LibraryHost& host, LibraryMenuFacts& made) {
        const library::Library& model = host.model;
        const library::TextHooks& text = model.text;
        LibraryMenuRegistries& shown = made.registries;
        const bool in_match = model.inputs.in_match;
        for (const library::Registry& registry : model.inputs.registries) {
            LibraryMenuRegistry card;
            card.id = registry.id;
            card.letters = library::registry_initials(registry.name);
            if (registry.shipped_logo)
                card.picture = picture_of(host.shipped_logo_badge());
            else if (registry.icon)
                card.picture = picture_of(&*registry.icon);
            card.title = registry.title.empty() ? registry.name : registry.title;
            card.pill = std::string(library::shown(
                text, registry.built_in ? words::registry_built_in : words::registry_not_reviewed
            ));
            card.pill_warns = !registry.built_in;
            card.address = registry.address;
            card.packages = packages_text(text, registry.packages);
            card.refreshed = library::registry_refresh_line(registry, text);
            card.refresh_warns = registry.unreachable;
            card.on = registry.on;
            card.refresh_enabled = registry.on && !registry.refreshing && !in_match;
            card.refreshing = registry.refreshing;

            LibraryMenuDetails& details = card.details;
            details.title = card.title;
            details.letters = card.letters;
            details.picture = card.picture;
            details.status = card.refreshed;
            details.status_warns = registry.unreachable;
            details.summary = registry.description;
            if (!registry.author.empty())
                details.facts.push_back(
                    {std::string(library::shown(text, words::fact_author)), registry.author, false}
                );
            details.facts.push_back({worded("Packages"), std::to_string(registry.packages), false});
            if (const std::string kinds = kinds_text(registry); !kinds.empty())
                details.facts.push_back({worded("Contents"), kinds, false});
            details.facts.push_back(
                {worded("Mirrors"),
                 library::filled(text, registry.mirrors == 1 ? words::registry_mirror_one
                                                             : words::registry_mirror_many,
                                 {{"n", std::to_string(registry.mirrors)}}),
                 false}
            );
            details.facts.push_back(
                {worded("Downloads"),
                 registry.downloads_on ? worded("On")
                 : registry.downloads_off_reason.empty() ? worded("Off")
                                                         : registry.downloads_off_reason,
                 !registry.downloads_on}
            );
            details.section = capitals(std::string(library::shown(
                text, registry.keys.empty()      ? words::registry_no_key
                      : registry.keys.size() > 1 ? words::registry_key_many
                                                 : words::registry_key_one
            )));
            for (const library::SigningKey& key : registry.keys)
                details.section_lines.push_back(key.fingerprint);
            if (!registry.keys.empty())
                details.section_note = std::string(library::shown(
                    text, registry.built_in ? words::registry_key_built_in : words::registry_key_added
                ));
            if (!registry.built_in) {
                details.buttons.push_back(
                    {"add-mirror", std::string(library::shown(text, words::registry_add_mirror)), !in_match}
                );
                details.buttons.push_back(
                    {"remove", std::string(library::shown(text, words::registry_remove)), true}
                );
            }
            if (registry.id == registry_)
                shown.selected = registries_.size();
            registries_.push_back(registry.id);
            shown.cards.push_back(std::move(card));
        }
        if (!shown.selected && !shown.cards.empty()) {
            shown.selected = 0;
            registry_ = registries_.front();
        }
        if (shown.selected) {
            const library::Registry& registry = model.inputs.registries[*shown.selected];
            details_registry_ = registry.id;
            if (!registry.built_in) {
                details_actions_.push_back(library::Action::add_mirror);
                details_actions_.push_back(library::Action::remove_registry);
            }
        }
        shown.add = std::string(library::shown(text, words::registry_add));
        shown.note = library::registry_note(model.inputs.registries, text);
        shown.notice = model.inputs.registries_notice;
    }

    Runtime& runtime_;                       ///< the runtime whose Library it shows
    LibraryMenuFacts made_{};                ///< the facts as last made
    bool built_{};                           ///< made_ has been made
    uint64_t seen_model_{};                  ///< the model's revision made_ was made at
    uint64_t revision_{};                    ///< counts the presses
    uint64_t seen_revision_{};               ///< the presses made_ was made after
    std::string seen_language_{};            ///< a word in the language made_ was made in
    LibraryMenuView view_{};                 ///< the rows and maps in view
    std::vector<library::EntryId> rows_{};   ///< the rows' entries, in the order shown
    std::vector<std::string> tags_{};        ///< TAGS' tags, in the order shown
    std::vector<std::string> details_tags_{}; ///< the details' tags
    std::vector<library::Action> details_actions_{}; ///< the details' buttons' actions
    library::EntryId details_entry_{};       ///< the entry the details tell of
    std::string details_registry_{};         ///< the registry the details tell of
    std::vector<library::Action> left_actions_{}; ///< the left buttons' actions
    std::vector<std::string> registries_{};  ///< the cards' registries, in the order shown
    std::string registry_{};                 ///< the registry chosen on the Registries tab
    std::vector<std::string> expansions_{};  ///< the expansions' plates' ids
};

bool Runtime::push_library_menu_screen() {
    std::optional<GameMenuArt> game_art = read_game_menu_art(assets_, "LIBRARY", "library");
    if (!game_art)
        return false;
    auto host = std::make_shared<LibraryMenuHost>(*this);
    LibraryMenuActions actions;
    // The Library goes with the main menu, as the Library's own screen does.
    actions.shows = [this] { return screen_ == Screen::main_menu; };
    actions.press = [host](const LibraryMenuPress& asked) { return host->press(asked); };
    actions.viewed = [host](const LibraryMenuView& view) { host->viewed(view); };
    actions.closed = [host] { host->closed(); };
    oa_layer().push(make_library_menu(
        library_screen_name, [host] { return host->facts(); }, std::move(actions), std::move(*game_art)
    ));
    return true;
}

} // namespace oa::app
