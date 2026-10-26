// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor's model over the Ridge fixture: category order, filters,
// search, removing and restoring chords, latch and hold, number keys,
// choosing and duplicating a layout, a Home clash the chosen layout did not
// steal, and a binding this version does not know.
//
// oa-ui-key-editor-model-test

#include "oa/data/input_actions/actions.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/key_editor/model.hpp"
#include "screen_fixture.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace editor = oa::ui::key_editor;
namespace keys = oa::ui::key_bindings;
namespace kit = oa::ui::kit;
namespace acts = oa::data::input_actions;
using key_editor_fixture::binding_of;
using key_editor_fixture::editor_fixture;
using key_editor_fixture::parsed_chord;

/// Prints a line when a check is about to fail, then checks it.
///
/// @param condition the check
/// @param detail what failed, for the line before OA_CHECK
void expect(bool condition, const std::string& detail) {
    if (!condition)
        std::fprintf(stderr, "%s\n", detail.c_str());
    OA_CHECK(condition);
}

/// Joins category or action ids with a comma.
///
/// @param ids the ids
/// @return the line
std::string joined_ids(const std::vector<std::string>& ids) {
    std::string line;
    for (const std::string& id : ids) {
        if (!line.empty())
            line += ", ";
        line += id;
    }
    return line;
}

/// The ids of a category list.
///
/// @param categories the categories
/// @return the ids, in order
std::vector<std::string> category_ids(const std::vector<editor::CategoryEntry>& categories) {
    std::vector<std::string> ids;
    ids.reserve(categories.size());
    for (const editor::CategoryEntry& entry : categories)
        ids.push_back(entry.id);
    return ids;
}

/// One category, or null.
///
/// @param categories the categories
/// @param id the category id
/// @return the entry
const editor::CategoryEntry* category_named(
    const std::vector<editor::CategoryEntry>& categories, std::string_view id
) {
    for (const editor::CategoryEntry& entry : categories) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

/// One row, or null.
///
/// @param rows the rows
/// @param id the action id
/// @return the row
const editor::Row* row_named(const std::vector<editor::Row>& rows, std::string_view id) {
    for (const editor::Row& row : rows) {
        if (row.id == id)
            return &row;
    }
    return nullptr;
}

/// True when every needle is among the chords.
///
/// @param chords the chords
/// @param needles the chords that must be there
/// @return true when each needle matches one chord
bool contains_chords(const std::vector<keys::Chord>& chords, const std::vector<keys::Chord>& needles) {
    for (const keys::Chord& needle : needles) {
        bool found = false;
        for (const keys::Chord& chord : chords) {
            if (chord == needle)
                found = true;
        }
        if (!found)
            return false;
    }
    return true;
}

/// True when a row list names an action.
///
/// @param rows the rows
/// @param id the action id
/// @return true when it is there
bool has_row(const std::vector<editor::Row>& rows, std::string_view id) {
    return row_named(rows, id) != nullptr;
}

/// True when the details include the chord a text parses to.
///
/// @param chords the details' chords
/// @param text the chord text
/// @return true when one matches
bool has_chord(const std::vector<editor::ChordDetail>& chords, std::string_view text) {
    const keys::Chord want = parsed_chord(text);
    for (const editor::ChordDetail& chord : chords) {
        if (chord.chord == want)
            return true;
    }
    return false;
}

/// The place of a chord in the details, or the chord count when it is absent.
///
/// @param details the details
/// @param text the chord text
/// @return the index
std::size_t chord_index(const editor::Details& details, std::string_view text) {
    const keys::Chord want = parsed_chord(text);
    for (std::size_t index = 0; index < details.chords.size(); ++index) {
        if (details.chords[index].chord == want)
            return index;
    }
    return details.chords.size();
}

/// The rows of every category, with no filter and no search.
///
/// @param model the editor
/// @return the rows
std::vector<editor::Row> every_row(const editor::Editor& model) {
    return model.rows("all", editor::Filter::all, "");
}

/// The fresh fixture's categories, letters and the category letter E.
void categories_and_letters() {
    const editor::Editor model(editor_fixture());
    const std::vector<editor::CategoryEntry> categories = model.categories();
    const std::vector<std::string> ids = category_ids(categories);
    const std::vector<std::string> expected = {
        "all",
        "orders",
        "mouse",
        "selection",
        "groups",
        "camera",
        "building",
        "game",
        "chat",
        "watching",
        "window",
        "menus",
        "heading.hacks",
        "ui.megamap",
        "heading.mod",
        "mod.categories",
        "mod.menus",
        "oa-screens",
    };
    expect(ids == expected, "categories: " + joined_ids(ids));
    expect(
        category_named(categories, "developer") == nullptr, "developer is listed while the mode is off"
    );
    expect(category_named(categories, "unknown") == nullptr, "unknown is listed on a fresh fixture");

    const editor::CategoryEntry* menus = category_named(categories, "menus");
    const editor::CategoryEntry* watching = category_named(categories, "watching");
    const editor::CategoryEntry* screens = category_named(categories, "oa-screens");
    const editor::CategoryEntry* megamap = category_named(categories, "ui.megamap");
    const editor::CategoryEntry* mod = category_named(categories, "heading.mod");
    expect(menus != nullptr && menus->letter == "U", "menus letter is not U");
    expect(watching != nullptr && watching->letter.empty(), "watching has a letter");
    expect(screens != nullptr && screens->letter.empty(), "oa-screens has a letter");
    expect(
        megamap != nullptr && megamap->title == "Megamap" && megamap->letter == "L",
        "ui.megamap is not Megamap / L"
    );
    expect(mod != nullptr && mod->title == "Ridge", "heading.mod is not titled Ridge");

    const std::vector<editor::Row> made = model.rows("mod.categories", editor::Filter::all, "");
    std::string made_ids;
    for (const editor::Row& row : made) {
        if (!made_ids.empty())
            made_ids += ", ";
        made_ids += row.id;
    }
    // Ctrl+Z is select.same-type, so a Z in mod.categories is dropped. E is free.
    expect(has_row(made, "select.category-e"), "mod.categories rows: " + made_ids);
    const std::vector<editor::Row> menus_rows = model.rows("mod.menus", editor::Filter::all, "");
    expect(has_row(menus_rows, "gui.ridge"), "mod.menus has no gui.ridge");

    const editor::FilterCounts counts = model.counts();
    const std::vector<editor::Row> all = every_row(model);
    expect(counts.all == all.size(), "counts().all is not the all-row count");
    expect(model.changes() == 1, "a fresh fixture does not count one own binding");
    expect(model.unsaved() == 0, "a fresh editor is not clean");
    expect(model.can_save(), "the Ridge fixture clashes");
}

/// Filters and a folded search.
void filters_and_search() {
    const editor::Editor model(editor_fixture());
    const std::vector<editor::Row> changed = model.rows("all", editor::Filter::changed, "");
    const std::vector<editor::Row> unbound = model.rows("all", editor::Filter::unbound, "");
    const std::vector<editor::Row> latchable = model.rows("all", editor::Filter::latchable, "");
    expect(has_row(changed, "select.all"), "changed does not include select.all");
    expect(!has_row(changed, "order.attack"), "changed includes the layout's Attack");
    const editor::Details select_all = model.details("select.all");
    expect(has_chord(select_all.chords, "Ctrl+B"), "select.all is not Ctrl+B");

    std::size_t empty = 0;
    for (const editor::Row& row : every_row(model)) {
        if (row.chords.empty())
            ++empty;
    }
    expect(unbound.size() == empty, "unbound is not every row with no chord");
    expect(model.counts().unbound == empty, "counts().unbound disagrees");
    for (const editor::Row& row : unbound)
        expect(row.chords.empty(), row.id + " is unbound with a chord");
    const std::vector<editor::Row> all_rows = every_row(model);
    const editor::Row* attack = row_named(all_rows, "order.attack");
    std::string attack_chords = "missing";
    if (attack != nullptr) {
        attack_chords.clear();
        for (const keys::Chord& chord : attack->chords) {
            if (!attack_chords.empty())
                attack_chords += ", ";
            attack_chords += keys::chord_text(chord);
        }
        if (attack_chords.empty())
            attack_chords = "none";
    }
    expect(attack != nullptr && !attack->chords.empty(), "Attack starts unbound (" + attack_chords + ")");

    expect(has_row(latchable, "order.queue-modifier"), "latchable omits order.queue-modifier");
    expect(model.counts().latchable == latchable.size(), "counts().latchable disagrees");

    for (const char* query : {"attack", "ATTACK", "Attack"}) {
        const std::vector<editor::Row> found = model.rows("all", editor::Filter::all, query);
        expect(has_row(found, "order.attack"), std::string(query) + " misses order.attack");
        expect(!has_row(found, "order.move"), std::string(query) + " includes order.move");
    }
}

/// Removing Attack's W marks it unbound and changed. A second remove does nothing.
void remove_attack() {
    editor::Editor model(editor_fixture());
    const editor::Details before = model.details("order.attack");
    const std::size_t index = chord_index(before, "W");
    expect(index < before.chords.size(), "Attack has no W");
    expect(model.remove_chord("order.attack", index), "remove_chord did not take W");
    const editor::Details after = model.details("order.attack");
    expect(!has_chord(after.chords, "W"), "Attack still has W");
    expect(after.chords.empty(), "Attack kept a chord");
    const std::vector<editor::Row> all_rows = every_row(model);
    const editor::Row* row = row_named(all_rows, "order.attack");
    expect(row != nullptr && row->mark == kit::KeyMark::unbound, "Attack's mark is not unbound");
    expect(
        has_row(model.rows("all", editor::Filter::changed, ""), "order.attack"),
        "unbound Attack is not changed"
    );
    expect(
        has_row(model.rows("all", editor::Filter::unbound, ""), "order.attack"),
        "Attack is not unbound"
    );
    expect(!model.remove_chord("order.attack", index), "a second remove succeeded");
    expect(model.unsaved() > 0, "removing a chord left the editor clean");
}

/// Resetting select.all drops the own binding.
void reset_select_all() {
    editor::Editor model(editor_fixture());
    expect(model.reset_action("select.all"), "reset_action did not drop select.all");
    expect(
        !has_row(model.rows("all", editor::Filter::changed, ""), "select.all"),
        "select.all stayed changed"
    );
    const editor::Details details = model.details("select.all");
    expect(has_chord(details.chords, "Ctrl+A"), "select.all did not return to Ctrl+A");
    expect(!has_chord(details.chords, "Ctrl+B"), "select.all kept Ctrl+B");
}

/// Escape stays on the two fixed actions. Settings' comma and focus-next's Tab come off
/// and reset puts them back.
void fixed_escape_and_removed_chords() {
    editor::Editor model(editor_fixture());
    const editor::Details chat = model.details("chat.cancel");
    const std::size_t chat_escape = chord_index(chat, "Escape");
    expect(chat_escape < chat.chords.size(), "chat.cancel has no Escape");
    expect(!model.remove_chord("chat.cancel", chat_escape), "chat.cancel's Escape came off");
    expect(has_chord(model.details("chat.cancel").chords, "Escape"), "chat.cancel lost Escape");

    const editor::Details screen = model.details("ui.screen-cancel");
    const std::size_t screen_escape = chord_index(screen, "Escape");
    expect(screen_escape < screen.chords.size(), "ui.screen-cancel has no Escape");
    expect(!model.remove_chord("ui.screen-cancel", screen_escape), "ui.screen-cancel's Escape came off");
    expect(
        has_chord(model.details("ui.screen-cancel").chords, "Escape"), "ui.screen-cancel lost Escape"
    );

    const editor::Details settings = model.details("ui.settings");
    std::size_t comma = chord_index(settings, "Primary+,");
    if (comma >= settings.chords.size() && settings.chords.size() == 1)
        comma = 0;
    expect(comma < settings.chords.size(), "ui.settings has no chord");
    expect(model.remove_chord("ui.settings", comma), "ui.settings's chord stayed");
    expect(
        !has_chord(model.details("ui.settings").chords, "Primary+,"), "ui.settings kept Primary+,"
    );
    expect(model.reset_action("ui.settings"), "reset did not restore ui.settings");
    expect(has_chord(model.details("ui.settings").chords, "Primary+,"), "ui.settings's chord did not return");

    const editor::Details next = model.details("ui.screen-focus-next");
    const std::size_t tab = chord_index(next, "Tab");
    expect(tab < next.chords.size(), "ui.screen-focus-next has no Tab");
    expect(model.remove_chord("ui.screen-focus-next", tab), "Tab stayed on ui.screen-focus-next");
    expect(!has_chord(model.details("ui.screen-focus-next").chords, "Tab"), "Tab remained");
    expect(model.reset_action("ui.screen-focus-next"), "reset did not restore Tab");
    expect(has_chord(model.details("ui.screen-focus-next").chords, "Tab"), "Tab did not return");
}

/// An editor opened with an empty Attack and one latch is already dirty in the summary
/// and clean as a save.
void opened_dirty_summary() {
    editor::EditorInputs inputs = editor_fixture();
    inputs.own.bindings.clear();
    keys::LayoutBinding empty;
    empty.action = "order.attack";
    inputs.own.bindings.push_back(std::move(empty));
    inputs.helpers.toggle = std::vector<std::string>{"order.queue-modifier"};
    const editor::Editor model(std::move(inputs));
    expect(model.changes() == 2, "an empty Attack plus one latch is not two changes");
    expect(model.unsaved() == 0, "that opening is unsaved");
}

/// Latch writes one id, and a second on does nothing. Hold to confirm lists every other
/// confirmable action and leaves self-destruct off. The registry's only confirmable
/// action is order.self-destruct, so that list is set and empty.
void latch_and_confirm() {
    editor::Editor model(editor_fixture());
    expect(model.set_latch("order.queue-modifier", true), "set_latch did not turn on");
    expect(!model.set_latch("order.queue-modifier", true), "set_latch turned on twice");
    const editor::SaveResult saved = model.result();
    expect(
        saved.manifest.helpers.toggle.has_value() && saved.manifest.helpers.toggle->size() == 1 &&
            (*saved.manifest.helpers.toggle)[0] == "order.queue-modifier",
        "toggle is not just order.queue-modifier"
    );
    expect(model.details("order.queue-modifier").latch_on, "Latch is off");

    editor::Editor confirm(editor_fixture());
    std::vector<std::string> expected;
    for (const acts::Action& action : acts::actions()) {
        if (action.confirmable && action.id != "order.self-destruct")
            expected.emplace_back(action.id);
    }
    expect(confirm.set_confirm("order.self-destruct", false), "set_confirm did not materialise");
    const auto held = confirm.result().manifest.helpers.hold_to_confirm;
    expect(held.has_value() && *held == expected, "hold_to_confirm is not every other confirmable");
    const editor::Details self = confirm.details("order.self-destruct");
    expect(self.confirm_offered, "self-destruct does not offer a hold");
    expect(!self.confirm_on, "self-destruct still confirms");
    if (held) {
        for (const std::string& id : *held)
            expect(confirm.details(id).confirm_on, id + " did not stay on");
    }
}

/// Number keys swap the group and page chords and are not an own change.
void number_keys_swap() {
    editor::Editor model(editor_fixture());
    const editor::Details group_before = model.details("group.select-1");
    const editor::Details page_before = model.details("build.page-1");
    std::vector<keys::Chord> group_chords;
    std::vector<keys::Chord> page_chords;
    for (const editor::ChordDetail& chord : group_before.chords)
        group_chords.push_back(chord.chord);
    for (const editor::ChordDetail& chord : page_before.chords)
        page_chords.push_back(chord.chord);
    const std::size_t changes = model.changes();
    model.set_number_keys(true);
    std::vector<keys::Chord> group_after;
    std::vector<keys::Chord> page_after;
    for (const editor::ChordDetail& chord : model.details("group.select-1").chords)
        group_after.push_back(chord.chord);
    for (const editor::ChordDetail& chord : model.details("build.page-1").chords)
        page_after.push_back(chord.chord);
    expect(group_after != group_chords, "group.select-1 did not change");
    expect(page_after != page_chords, "build.page-1 did not change");
    expect(contains_chords(group_after, page_chords), "group.select-1 did not gain the page chords");
    expect(contains_chords(page_after, group_chords), "build.page-1 did not gain the group chords");
    expect(model.number_keys(), "number keys stayed off");
    expect(model.changes() == changes, "number keys counted as an own change");
    expect(model.unsaved() > 0, "number keys left the editor clean");
}

/// Choosing a layout keeps or clears the draft, and an unknown id does nothing.
void choose_layouts() {
    editor::Editor unknown(editor_fixture());
    expect(!unknown.choose_layout("not-a-layout", editor::KeepChanges::keep), "an unknown layout was chosen");
    expect(unknown.layout_id() == "ridge-grid", "an unknown id changed the layout");
    expect(
        !unknown.choose_layout("ridge-grid", editor::KeepChanges::keep),
        "choosing the current layout with keep succeeded"
    );

    editor::Editor kept(editor_fixture());
    expect(kept.choose_layout("oa-default", editor::KeepChanges::keep), "oa-default was not chosen");
    expect(kept.layout_id() == "oa-default", "layout is not oa-default");
    expect(
        has_row(kept.rows("all", editor::Filter::changed, ""), "select.all"),
        "keep dropped the own binding"
    );
    expect(!kept.rename("Ridge renamed"), "oa-default was renamed");

    editor::EditorInputs inputs = editor_fixture();
    editor::LayoutSummary plain;
    plain.id = "ridge-plain";
    plain.name = "Ridge plain";
    plain.author = "Ridge";
    plain.version = "1.0";
    plain.layout.id = "ridge-plain";
    plain.layout.base = keys::Base::oa_default;
    inputs.installed.push_back(std::move(plain));
    editor::Editor aside(std::move(inputs));
    expect(aside.choose_layout("ridge-plain", editor::KeepChanges::aside), "ridge-plain was not chosen");
    expect(aside.layout_id() == "ridge-plain", "aside did not choose ridge-plain");
    expect(
        !has_row(aside.rows("all", editor::Filter::changed, ""), "select.all"),
        "aside kept the known own binding"
    );
}

/// Duplicate makes a layout and clears the known draft. An empty name and OA default's
/// rename do nothing.
void duplicate_and_rename() {
    editor::Editor model(editor_fixture());
    expect(!model.duplicate(""), "an empty name duplicated");
    expect(model.layout_id() == "ridge-grid", "a refused duplicate changed the layout");
    expect(model.duplicate("Ridge copy"), "duplicate did not make Ridge copy");
    expect(model.layout_id() == "ridge-copy", "the copy's id is not ridge-copy");
    expect(model.layout_name() == "Ridge copy", "the copy's name is not Ridge copy");
    expect(model.layout_author() == "You", "the copy's author is not You");
    expect(
        !has_row(model.rows("all", editor::Filter::changed, ""), "select.all"),
        "duplicate kept the known own binding"
    );
    const editor::SaveResult saved = model.result();
    bool found = false;
    for (const auto& manifest : saved.layouts) {
        if (manifest.id == "ridge-copy" && manifest.name == "Ridge copy" && manifest.author.name == "You")
            found = true;
    }
    expect(found, "result().layouts has no Ridge copy");

    expect(model.rename("Ridge renamed"), "rename did not change the copy");
    expect(model.layout_name() == "Ridge renamed", "layout_name is not Ridge renamed");
    expect(model.layout_id() == "ridge-copy", "rename changed the id");
    bool renamed = false;
    for (const auto& manifest : model.result().layouts) {
        if (manifest.id == "ridge-copy" && manifest.name == "Ridge renamed")
            renamed = true;
    }
    expect(renamed, "result().layouts did not rename the copy");
}

/// Home on order.move in the chosen layout meets camera.centre's base Home.
///
/// Writing Home on both order.move and camera.centre in one chosen file drops the later
/// line (earlier in the file wins), so camera.centre would lose Home and the pair would
/// not clash. The chosen layout names only order.move. The base keeps camera.centre.
void chosen_home_clash() {
    editor::EditorInputs inputs;
    inputs.base = keys::Base::oa_default;
    inputs.has_chosen = true;
    inputs.chosen.id = "home-pair";
    inputs.chosen.name = "Home pair";
    inputs.chosen.author = "Ridge";
    inputs.chosen.version = "1.0";
    inputs.chosen.layout.id = "home-pair";
    inputs.chosen.layout.base = keys::Base::oa_default;
    inputs.chosen.layout.bindings.push_back(binding_of("order.move", "Home"));
    inputs.keyboard = keys::KeyboardMap::us();
    inputs.platform = {};
    inputs.scheme = keys::PointerScheme::right_click;
    inputs.hold_to_confirm_setting = true;
    const editor::Editor model(std::move(inputs));

    const editor::Details move = model.details("order.move");
    const editor::Details centre = model.details("camera.centre");
    const std::size_t move_home = chord_index(move, "Home");
    const std::size_t centre_home = chord_index(centre, "Home");
    expect(move_home < move.chords.size(), "order.move has no Home");
    expect(centre_home < centre.chords.size(), "camera.centre has no Home");
    if (move_home < move.chords.size())
        expect(move.chords[move_home].step == keys::Step::layout, "order.move's Home is not the layout");
    if (centre_home < centre.chords.size())
        expect(centre.chords[centre_home].step == keys::Step::base, "camera.centre's Home is not the base");
    expect(model.result().manifest.bindings.empty(), "own stole a Home binding");

    const std::vector<editor::Row> clashing = model.rows("all", editor::Filter::clashes, "");
    if (model.clashes() != 1 || model.counts().clashes != 2) {
        std::string ids;
        for (const editor::Row& row : clashing) {
            if (!ids.empty())
                ids += ", ";
            ids += row.id;
        }
        std::fprintf(
            stderr,
            "home clash: pairs %zu, actions %zu, rows %s\n",
            model.clashes(),
            model.counts().clashes,
            ids.c_str()
        );
    }
    expect(model.clashes() == 1, "Home is not one clashing pair");
    expect(model.counts().clashes == 2, "the clash does not count both actions");
    expect(has_row(clashing, "order.move") && has_row(clashing, "camera.centre"), "the clash rows differ");
    expect(!model.can_save(), "a Home clash can save");
}

/// A name this version does not know is read only, written back, and kept by restore.
void unknown_binding() {
    editor::EditorInputs inputs = editor_fixture();
    inputs.own.bindings.push_back(binding_of("not.a.real.action", "K"));
    editor::Editor model(std::move(inputs));
    const std::vector<editor::CategoryEntry> categories = model.categories();
    expect(category_named(categories, "unknown") != nullptr, "unknown is missing");
    const std::vector<editor::Row> rows = model.rows("unknown", editor::Filter::all, "");
    const editor::Row* row = row_named(rows, "not.a.real.action");
    expect(row != nullptr && row->read_only, "the unknown row is missing or writable");
    expect(!model.remove_chord("not.a.real.action", 0), "remove_chord edited an unknown action");
    expect(!model.reset_action("not.a.real.action"), "reset_action edited an unknown action");
    expect(!model.set_latch("not.a.real.action", true), "set_latch edited an unknown action");
    expect(!model.set_confirm("not.a.real.action", false), "set_confirm edited an unknown action");

    bool written = false;
    for (const auto& binding : model.result().manifest.bindings) {
        if (binding.action == "not.a.real.action") {
            written = binding.chords.size() == 1 && binding.chords[0].text == "K";
        }
    }
    expect(written, "the manifest did not write not.a.real.action as K");

    model.set_number_keys(true);
    expect(model.set_latch("order.queue-modifier", true), "latch did not turn on before restore");
    model.restore_defaults();
    expect(!model.number_keys(), "restore did not put number keys back");
    expect(!model.details("order.queue-modifier").latch_on, "restore left Latch on");
    expect(
        !has_row(model.rows("all", editor::Filter::changed, ""), "select.all"),
        "restore kept select.all"
    );
    expect(
        has_row(model.rows("unknown", editor::Filter::all, ""), "not.a.real.action"),
        "restore dropped the unknown action"
    );
    bool kept = false;
    for (const auto& binding : model.result().manifest.bindings) {
        if (binding.action == "not.a.real.action")
            kept = true;
    }
    expect(kept, "restore dropped the unknown binding from the manifest");
    expect(model.changes() == 1, "restore did not leave only the unknown binding");
}

/// Developer keys appear only while developer mode is on.
void developer_category() {
    const editor::Editor off(editor_fixture());
    expect(off.rows("developer", editor::Filter::all, "").empty(), "developer rows exist while off");
    expect(category_named(off.categories(), "developer") == nullptr, "developer is listed while off");

    editor::EditorInputs inputs = editor_fixture();
    inputs.developer_mode = true;
    const editor::Editor on(std::move(inputs));
    const std::vector<editor::CategoryEntry> on_categories = on.categories();
    const editor::CategoryEntry* developer = category_named(on_categories, "developer");
    expect(developer != nullptr, "developer is missing while on");
    expect(
        developer != nullptr && developer->count == on.rows("developer", editor::Filter::all, "").size(),
        "developer count disagrees with its rows"
    );
}

} // namespace

int main() {
    categories_and_letters();
    filters_and_search();
    remove_attack();
    reset_select_all();
    fixed_escape_and_removed_chords();
    opened_dirty_summary();
    latch_and_confirm();
    number_keys_swap();
    choose_layouts();
    duplicate_and_rename();
    chosen_home_clash();
    unknown_binding();
    developer_category();
    return oa::test::failed_checks() == 0 ? 0 : 1;
}
