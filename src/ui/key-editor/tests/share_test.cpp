// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A share name becomes an id, a paste shows a clash and an unknown action
// before it writes the draft, a text that is no key layout is refused in the
// player's words, and an import moves a chord and records where the keys
// came from.

#include "oa/ui/key_editor/model.hpp"

#include "oa/formats/oakeys/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/legacy_import.hpp"

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace editor = oa::ui::key_editor;
namespace keys = oa::ui::key_bindings;
namespace oakeys = oa::formats::oakeys;

/// Views a text's bytes.
///
/// @param text the text
/// @return its bytes
std::span<const uint8_t> as_bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

/// Prints a line when a check is about to fail, then checks it.
///
/// @param condition the check
/// @param detail what failed
void expect(bool condition, const std::string& detail) {
    if (!condition)
        std::fprintf(stderr, "%s\n", detail.c_str());
    OA_CHECK(condition);
}

/// A chord the test expects to parse.
///
/// @param text the chord
/// @return the chord
keys::Chord parsed(std::string_view text) {
    keys::ChordError error{};
    const std::optional<keys::Chord> chord = keys::parse_chord(text, error);
    expect(chord.has_value(), std::string{text} + " did not parse");
    if (!chord)
        return {};
    return *chord;
}

/// OA default, with nothing of the player's own yet.
///
/// @return the inputs
editor::EditorInputs plain() {
    editor::EditorInputs inputs;
    inputs.base = keys::Base::oa_default;
    inputs.keyboard = keys::KeyboardMap::us();
    return inputs;
}

/// True when the details hold a chord with this text.
///
/// @param shown the details
/// @param text the canonical chord
/// @return true when one chord matches
bool holds(const editor::Details& shown, std::string_view text) {
    const keys::Chord wanted = parsed(text);
    for (const editor::ChordDetail& one : shown.chords) {
        if (keys::same_key(one.chord, wanted, nullptr))
            return true;
    }
    return false;
}

/// The share id, and a summary whose text is the text form.
void share_name_and_summary() {
    expect(
        editor::id_from_name("Grid, left hand") == std::optional<std::string>{"grid-left-hand"},
        "Grid, left hand is not grid-left-hand"
    );
    expect(!editor::id_from_name("OA default"), "OA default was accepted");
    expect(!editor::id_from_name("oa-default"), "oa-default was accepted");
    expect(!editor::id_from_name("classic"), "classic was accepted");
    expect(!editor::id_from_name("Own ridge"), "an own- id was accepted");
    expect(!editor::id_from_name("   "), "an empty id was accepted");

    editor::Editor model(plain());
    model.set_share_name("Grid, left hand");
    expect(model.set_chords("order.move", {parsed("@KeyQ")}), "order.move did not take KeyQ");
    expect(model.set_latch("order.queue-modifier", true), "latch did not turn on");
    const editor::ShareSummary summary = editor::share_summary(model);
    const oakeys::Manifest manifest = editor::share_manifest(model);
    expect(summary.id == "grid-left-hand", "the summary id is not grid-left-hand");
    expect(
        summary.text == oakeys::write_bindings_text(manifest),
        "the summary text is not the text form"
    );
    expect(
        summary.keys == manifest.bindings.size() && summary.keys >= 1, "the key count disagrees"
    );
    expect(summary.helpers == 1, "the helper count is not 1");
    expect(summary.base == model.layout_name(), "the base is not the layout's name");
    expect(summary.name == "Grid, left hand", "the summary dropped the name");
}

/// A paste keeps the clashing chord, lists it, and keeps an unknown action.
void paste_keeps_and_lists() {
    constexpr std::string_view text = "# oakeys bindings, base oa-default\n"
                                      "order.move: [\"@KeyQ\"]\n"
                                      "select.all: [Ctrl+B]\n"
                                      "camera.orbit-left: [Q]\n";
    editor::Editor model(plain());
    const editor::PastePreview preview = editor::paste_preview(model, as_bytes(text));
    expect(preview.rows.size() == 3, "the paste did not preview three rows");
    expect(preview.clashes == 1, "the paste did not preview one clash");
    expect(preview.unknown == 1, "the paste did not preview one unknown action");
    expect(preview.changes == 3, "APPLY would not count three changes");
    bool holder = false;
    for (const editor::PasteRow& row : preview.rows) {
        if (row.action == "select.all")
            holder = row.clash && row.holder == "select.builders";
        if (row.action == "camera.orbit-left")
            expect(row.unknown, "camera.orbit-left was not kept as unknown");
    }
    expect(holder, "Ctrl+B was not listed as select.builders' chord");

    editor::apply_paste(model, preview);
    expect(holds(model.details("order.move"), "@KeyQ"), "order.move did not gain KeyQ");
    expect(holds(model.details("select.all"), "Ctrl+A"), "select.all lost Ctrl+A");
    expect(holds(model.details("select.builders"), "Ctrl+B"), "select.builders lost Ctrl+B");
    expect(!model.can_save(), "SAVE was not held back");
    bool unknown_kept = false;
    for (const oakeys::BindingEntry& binding : model.result().manifest.bindings) {
        if (binding.action == "camera.orbit-left")
            unknown_kept = true;
    }
    expect(unknown_kept, "the unknown action was dropped");

    const std::optional<keys::Clash> clash = model.draft_clash("select.all");
    expect(clash.has_value(), "the card has no clash");
    if (clash)
        expect(editor::settle(model, *clash, editor::Settle::keep_here), "the card did not settle");
    expect(model.can_save(), "SAVE stayed disabled after the card");
    expect(holds(model.details("select.all"), "Ctrl+B"), "keeping it here did not move Ctrl+B");
    expect(!holds(model.details("select.builders"), "Ctrl+B"), "select.builders still has Ctrl+B");
}

/// Keeping the holder drops the paste's claim and lets SAVE proceed.
void paste_keep_holder() {
    constexpr std::string_view text = "select.all: [Ctrl+B]\n";
    editor::Editor model(plain());
    const editor::PastePreview preview = editor::paste_preview(model, as_bytes(text));
    editor::apply_paste(model, preview);
    const std::optional<keys::Clash> clash = model.draft_clash("select.builders");
    expect(clash.has_value(), "the holder's card has no clash");
    if (clash)
        expect(
            editor::settle(model, *clash, editor::Settle::keep_here),
            "keep here on the holder failed"
        );
    expect(
        holds(model.details("select.all"), "Ctrl+A"), "the holder did not keep Ctrl+B where it was"
    );
    expect(holds(model.details("select.builders"), "Ctrl+B"), "Ctrl+B left select.builders");
    expect(model.can_save(), "SAVE stayed disabled after keeping the holder");
}

/// A chord that does not parse is an error at its line and column. The other row stays.
void paste_error_keeps_its_place() {
    constexpr std::string_view text = "order.move: [\"@KeyQ\"]\n"
                                      "select.all: [\"Ctrl+\"]\n";
    editor::Editor model(plain());
    const editor::PastePreview preview = editor::paste_preview(model, as_bytes(text));
    expect(!preview.problems.empty(), "Ctrl+ was not an error");
    bool placed = false;
    bool move = false;
    for (const oakeys::Problem& problem : preview.problems) {
        if (problem.line > 0 && problem.column > 0)
            placed = true;
    }
    for (const editor::PasteRow& row : preview.rows) {
        if (row.action == "order.move")
            move = true;
        expect(row.action != "select.all", "the broken chord became a row");
    }
    expect(placed, "the error has no line and column");
    expect(move, "the good row was dropped");
}

/// A text that is no key layout is refused in the player's words, at the
/// line and column where reading stopped, and lists no row.
void paste_refuses_other_text() {
    constexpr std::string_view text = "Hello, these are my keys\n";
    editor::Editor model(plain());
    const editor::PastePreview preview = editor::paste_preview(model, as_bytes(text));
    expect(preview.rows.empty(), "a text that is no layout listed a row");
    expect(preview.changes == 0, "a text that is no layout counted a change");
    expect(preview.problems.size() == 1, "a text that is no layout did not give one problem");
    if (!preview.problems.empty()) {
        const oakeys::Problem& problem = preview.problems.front();
        expect(
            problem.message == editor::words::text_not_layout,
            "the refusal reads " + problem.message
        );
        expect(problem.line == 1 && problem.column > 0, "the refusal has no line and column");
    }
}

/// An import keeps only a differing key, moves F4, and records imported-from.
void import_moves_f4() {
    editor::Editor model(plain());
    keys::LegacyRead read;
    keys::LegacyKey same;
    same.action = "order.move";
    same.chords = {parsed("M")};
    same.line = 1;
    read.keys.push_back(std::move(same));
    keys::LegacyKey mapped;
    mapped.action = "megamap.toggle";
    mapped.chords = {parsed("F4")};
    mapped.source_line = "MegaMapKey=115";
    mapped.line = 2;
    read.keys.push_back(std::move(mapped));

    editor::ImportPreview preview = editor::import_preview(model, read, "tdraw", "Settings.ini");
    expect(preview.found == 2, "found is not 2");
    expect(preview.differ == 1, "a matching key was counted");
    expect(preview.rows.size() == 1, "the matching key was listed");
    if (!preview.rows.empty()) {
        expect(
            preview.rows.front().action == "megamap.toggle", "the differing key is not the megamap"
        );
        expect(
            preview.rows.front().loses.find("game.kill-board") != std::string::npos,
            "the kills board was not named, loses is " + preview.rows.front().loses
        );
    }
    preview.date = "2026-10-27";
    editor::apply_import(model, preview);
    expect(holds(model.details("megamap.toggle"), "F4"), "the megamap did not gain F4");
    expect(!holds(model.details("game.kill-board"), "F4"), "the kills board still has F4");
    expect(holds(model.details("game.kill-board"), "Ctrl+F4"), "Ctrl+F4 left the kills board");
    const editor::SaveResult saved = model.result();
    expect(saved.manifest.imported_from.has_value(), "imported-from was not recorded");
    if (saved.manifest.imported_from) {
        expect(saved.manifest.imported_from->tool == "tdraw", "the tool is not tdraw");
        expect(
            saved.manifest.imported_from->file == "Settings.ini", "the file is not Settings.ini"
        );
        expect(saved.manifest.imported_from->date == "2026-10-27", "the date was not kept");
    }
    bool wrote_f4 = false;
    for (const oakeys::BindingEntry& binding : saved.manifest.bindings) {
        if (binding.action == "megamap.toggle" && !binding.chords.empty() &&
            binding.chords.front().text == "F4")
            wrote_f4 = true;
    }
    expect(wrote_f4, "SAVE did not write megamap.toggle: [F4]");
    expect(model.can_save(), "moving F4 left a clash");
}

} // namespace

int main() {
    share_name_and_summary();
    paste_keeps_and_lists();
    paste_keep_holder();
    paste_error_keeps_its_place();
    paste_refuses_other_text();
    import_moves_f4();
    return 0;
}
