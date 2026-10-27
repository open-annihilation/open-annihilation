// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keys overlay: which actions a context lists, how a numbered family
// collapses, where the panel sits, and the names automation uses.
//
// oa-ui-key-editor-overlay-test

#include "oa/data/input_actions/actions.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/layout.hpp"
#include "oa/ui/key_editor/overlay.hpp"
#include "screen_fixture.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace editor = oa::ui::key_editor;
namespace keys = oa::ui::key_bindings;
namespace kit = oa::ui::kit;
namespace acts = oa::data::input_actions;
using key_editor_fixture::binding_of;
using key_editor_fixture::fonts;

/// The bit of a context or a condition.
///
/// @param names the registry's names, in bit order
/// @param name the one to find
/// @return the bit, or 0 when the name is absent
uint32_t bit_of(const std::vector<std::string_view>& names, std::string_view name) {
    for (std::size_t index = 0; index < names.size() && index < 32; ++index) {
        if (names[index] == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// The context bit.
///
/// @param name the context id
/// @return the bit
uint32_t context_bit(std::string_view name) {
    std::vector<std::string_view> names;
    for (const acts::Context& context : acts::contexts())
        names.push_back(context.name);
    return bit_of(names, name);
}

/// The condition bit.
///
/// @param name the condition
/// @return the bit
uint32_t condition_bit(std::string_view name) {
    const auto table = acts::conditions();
    return bit_of(std::vector<std::string_view>(table.begin(), table.end()), name);
}

/// The bindings of OA default, with the hacks that are on and one own layout.
struct ResolvedFixture {
    keys::KeyboardMap keyboard = keys::KeyboardMap::us();

    /// Resolves OA default.
    ///
    /// @param hacks the hacks that are on
    /// @param own the player's own changes, or null
    /// @return the bindings
    keys::Resolved resolve(std::vector<std::string> hacks, const keys::Layout* own) const {
        keys::ResolveInput input;
        input.base = keys::Base::oa_default;
        input.hacks_on = std::move(hacks);
        input.own = own;
        input.keyboard = &keyboard;
        input.platform = {};
        return keys::resolve(input);
    }
};

/// Fills the overlay's inputs from a resolution.
///
/// @param resolved the bindings
/// @param active the context bits
/// @param conditions the condition bits
/// @return the inputs
editor::OverlayInputs inputs_for(keys::Resolved resolved, uint32_t active, uint32_t conditions) {
    editor::OverlayInputs inputs;
    inputs.resolved = std::move(resolved);
    inputs.contexts.active = active;
    inputs.contexts.conditions = conditions;
    inputs.keyboard = keys::KeyboardMap::us();
    inputs.layout_name = "OA default";
    inputs.changes = 0;
    return inputs;
}

/// Finds a line by its name.
///
/// @param groups the groups
/// @param name the line
/// @return the line, or null
const editor::OverlayLine*
find_line(const std::vector<editor::OverlayGroup>& groups, std::string_view name) {
    for (const editor::OverlayGroup& group : groups) {
        for (const editor::OverlayLine& line : group.lines) {
            if (line.name == name)
                return &line;
        }
    }
    return nullptr;
}

/// Finds a group's index by its title.
///
/// @param groups the groups
/// @param title the title
/// @return the index, or the size when it is absent
std::size_t group_index(const std::vector<editor::OverlayGroup>& groups, std::string_view title) {
    for (std::size_t index = 0; index < groups.size(); ++index) {
        if (groups[index].title == title)
            return index;
    }
    return groups.size();
}

/// Reports whether an automation name is words joined by dots.
///
/// @param name the name
/// @return true when it is legal
bool name_ok(std::string_view name) {
    if (name.empty() || name.size() > 100 || name.front() == '.' || name.back() == '.')
        return false;
    bool dotted = false;
    for (const char byte : name) {
        const bool word =
            (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-';
        if (byte == '.') {
            if (dotted)
                return false;
            dotted = true;
            continue;
        }
        if (!word)
            return false;
        dotted = false;
    }
    return true;
}

/// The control of a name.
///
/// @param list the display list
/// @param name the automation name
/// @return the control, or null
const kit::Control* control_named(const kit::DisplayList& list, std::string_view name) {
    for (const kit::Control& control : list.controls) {
        if (control.name == name)
            return &control;
    }
    return nullptr;
}

/// Playing lists Attack as A and collapses Assign group into one range.
void test_playing() {
    const ResolvedFixture fixture;
    const uint32_t active = context_bit("global") | context_bit("match") | context_bit("play");
    const editor::OverlayInputs inputs = inputs_for(fixture.resolve({}, nullptr), active, 0);
    OA_CHECK(editor::overlay_title(inputs) == "Keys while playing");
    const std::vector<editor::OverlayGroup> groups = editor::overlay_groups(inputs);
    const editor::OverlayLine* attack = find_line(groups, "Attack");
    OA_CHECK(attack != nullptr);
    if (attack != nullptr)
        OA_CHECK(attack->chord == "A");
    const editor::OverlayLine* assign = find_line(groups, "Assign group");
    OA_CHECK(assign != nullptr);
    if (assign != nullptr) {
        OA_CHECK(assign->chord.find("…") != std::string::npos);
        OA_CHECK(assign->chord.find('1') != std::string::npos);
        OA_CHECK(assign->chord.find('9') != std::string::npos);
    }
}

/// A broken digit run does not collapse into one 1…9 line.
void test_broken_run() {
    keys::Layout own;
    own.base = keys::Base::oa_default;
    own.bindings.push_back(binding_of("group.assign-5", "Q"));
    const ResolvedFixture fixture;
    const uint32_t active = context_bit("global") | context_bit("match") | context_bit("play");
    const editor::OverlayInputs inputs = inputs_for(fixture.resolve({}, &own), active, 0);
    const std::vector<editor::OverlayGroup> groups = editor::overlay_groups(inputs);
    bool full_range = false;
    bool broken = false;
    for (const editor::OverlayGroup& group : groups) {
        for (const editor::OverlayLine& line : group.lines) {
            if (line.chord.find('1') != std::string::npos &&
                line.chord.find('9') != std::string::npos &&
                line.chord.find("…") != std::string::npos)
                full_range = true;
            if (line.chord == "Q")
                broken = true;
        }
    }
    OA_CHECK(!full_range);
    OA_CHECK(broken);
}

/// Watching lists Speed up and does not list Attack.
void test_watching() {
    const ResolvedFixture fixture;
    const uint32_t active = context_bit("global") | context_bit("match") | context_bit("watch");
    const editor::OverlayInputs inputs = inputs_for(fixture.resolve({}, nullptr), active, 0);
    OA_CHECK(editor::overlay_title(inputs) == "Keys while watching");
    const std::vector<editor::OverlayGroup> groups = editor::overlay_groups(inputs);
    OA_CHECK(find_line(groups, "Speed up") != nullptr);
    OA_CHECK(find_line(groups, "Attack") == nullptr);
}

/// Placing lists Rotate under This mod, and the title says so.
void test_placing() {
    const ResolvedFixture fixture;
    const uint32_t active =
        context_bit("global") | context_bit("match") | context_bit("play") | context_bit("placing");
    const editor::OverlayInputs inputs = inputs_for(
        fixture.resolve({"ui.build-preview", "units.build-rotation"}, nullptr), active, 0
    );
    OA_CHECK(editor::overlay_title(inputs) == "Keys while placing a building");
    const std::vector<editor::OverlayGroup> groups = editor::overlay_groups(inputs);
    const std::size_t mod = group_index(groups, "This mod");
    OA_CHECK(mod < groups.size());
    if (mod < groups.size()) {
        bool rotate = false;
        for (const editor::OverlayLine& line : groups[mod].lines) {
            if (line.name == "Rotate")
                rotate = true;
        }
        OA_CHECK(rotate);
    }
}

/// Hack actions sit under This mod, before Game.
void test_this_mod() {
    const ResolvedFixture fixture;
    const uint32_t active = context_bit("global") | context_bit("match") | context_bit("play");
    const uint32_t conditions = condition_bit("no-wheel-zoom");
    const editor::OverlayInputs inputs =
        inputs_for(fixture.resolve({"ui.megamap", "ui.whiteboard"}, nullptr), active, conditions);
    const std::vector<editor::OverlayGroup> groups = editor::overlay_groups(inputs);
    const std::size_t mod = group_index(groups, "This mod");
    const std::size_t game = group_index(groups, "Game");
    OA_CHECK(mod < groups.size());
    OA_CHECK(game < groups.size());
    OA_CHECK(mod < game);
    if (mod < groups.size()) {
        OA_CHECK(find_line(groups, "Megamap") != nullptr);
        const editor::OverlayLine* draw = find_line(groups, "Draw");
        OA_CHECK(draw != nullptr);
        if (draw != nullptr)
            OA_CHECK(draw->aside == "held");
    }
}

/// The On line carries the latches the caller already formatted.
void test_on_line() {
    const ResolvedFixture fixture;
    const uint32_t active = context_bit("global") | context_bit("match") | context_bit("play");
    editor::OverlayInputs inputs = inputs_for(fixture.resolve({}, nullptr), active, 0);
    inputs.on.push_back("QUEUE · latched");
    const std::vector<editor::OverlayGroup> groups = editor::overlay_groups(inputs);
    const editor::OverlayRoom room{{0, 0, 640, 480}, 128, kit::SizeClass::compact};
    const editor::OverlayLayout laid = editor::overlay_layout(inputs, groups, room, fonts(), 0);
    const kit::Control* list = control_named(laid.list, editor::name_keys_overlay_list);
    OA_CHECK(list != nullptr);
    if (list != nullptr) {
        OA_CHECK(list->text.find("On:") != std::string::npos);
        OA_CHECK(list->text.find("QUEUE · latched") != std::string::npos);
        OA_CHECK(list->text.find("Keys while playing") != std::string::npos);
    }
}

/// The panel sits inside the battlefield, to the right of a 128-point column.
///
/// @param width the battlefield's width
/// @param height the battlefield's height
/// @param size_class the class
/// @param want_width the panel's width at that class
void test_one_class(int32_t width, int32_t height, kit::SizeClass size_class, int32_t want_width) {
    const ResolvedFixture fixture;
    const uint32_t active = context_bit("global") | context_bit("match") | context_bit("play");
    const editor::OverlayInputs inputs = inputs_for(fixture.resolve({}, nullptr), active, 0);
    const std::vector<editor::OverlayGroup> groups = editor::overlay_groups(inputs);
    const editor::OverlayRoom room{{0, 0, width, height}, 128, size_class};
    const editor::OverlayLayout laid = editor::overlay_layout(inputs, groups, room, fonts(), 0);
    OA_CHECK(laid.panel.x >= 128);
    OA_CHECK(kit::wholly_in(laid.panel, room.battlefield));
    OA_CHECK(laid.panel.width == want_width);
    OA_CHECK(control_named(laid.list, editor::name_keys_overlay_edit) != nullptr);
    OA_CHECK(control_named(laid.list, editor::name_keys_overlay_close) != nullptr);
    const editor::OverlayLayout scrolled =
        editor::overlay_layout(inputs, groups, room, fonts(), 100000);
    OA_CHECK(scrolled.scroll == scrolled.scroll_max);
    OA_CHECK(scrolled.scroll >= 0);
}

/// The names automation uses are the ones the screen publishes, and they are legal.
void test_names() {
    OA_CHECK(editor::name_keys_overlay == "keys-overlay");
    OA_CHECK(editor::name_keys_overlay_edit == "keys-overlay.edit");
    OA_CHECK(editor::name_keys_overlay_close == "keys-overlay.close");
    OA_CHECK(editor::name_keys_overlay_list == "keys-overlay.list");
    OA_CHECK(name_ok(editor::name_keys_overlay));
    OA_CHECK(name_ok(editor::name_keys_overlay_edit));
    OA_CHECK(name_ok(editor::name_keys_overlay_close));
    OA_CHECK(name_ok(editor::name_keys_overlay_list));
}

} // namespace

int main() {
    test_playing();
    test_broken_run();
    test_watching();
    test_placing();
    test_this_mod();
    test_on_line();
    test_one_class(640, 480, kit::SizeClass::compact, 240);
    test_one_class(1280, 720, kit::SizeClass::regular, 420);
    test_one_class(1920, 1080, kit::SizeClass::large, 520);
    test_names();
    return oa::test::check_exit_status();
}
