// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The names table covers the picture buttons, colour cells and sliders of
// the installed screens, and the values it speaks.
#include "oa/app/control_names.hpp"

#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/ui/gui_layout.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace gui = oa::ui::gui_layout;

char fold_byte(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return static_cast<char>(byte);
}

bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (fold_byte(a) != fold_byte(b))
            return false;
    }
    return true;
}

std::string folded(std::string_view text) {
    std::string out(text);
    for (char& character : out)
        character = fold_byte(static_cast<unsigned char>(character));
    return out;
}

/// The caption a button or a slider draws. A hot surface has none.
std::string caption_of(const gui::Gadget& gadget) {
    if (const auto* button = std::get_if<gui::ButtonFields>(&gadget.fields))
        return button->text;
    if (const auto* bar = std::get_if<gui::ScrollBarFields>(&gadget.fields))
        return bar->text;
    return {};
}

bool wordless(const gui::Gadget& gadget) {
    const gui::GadgetType type = gadget.common.type;
    if (type != gui::GadgetType::button && type != gui::GadgetType::scroll_bar &&
        type != gui::GadgetType::hot_surface)
        return false;
    return caption_of(gadget).empty();
}

bool has_gadget(const gui::Layout& layout, std::string_view name) {
    for (const gui::Gadget& gadget : layout.gadgets)
        if (same_text(gadget.common.name, name))
            return true;
    return false;
}

void words_for_colour_gamma_and_keys() {
    using namespace oa::app;
    OA_CHECK(colour_text(0, 1) == "Blue, player 1");
    OA_CHECK(colour_text(9, 10) == "Tan, player 10");
    OA_CHECK(colour_text(-1, 1).empty());
    OA_CHECK(colour_text(0, 0).empty());
    OA_CHECK(colour_text(10, 1).empty());
    OA_CHECK(gamma_text(12) == "1.0");
    OA_CHECK(gamma_text(0) == "0.5");
    OA_CHECK(shortcut_letter('e') == "E");
    OA_CHECK(shortcut_letter('S') == "S");
    OA_CHECK(shortcut_letter('|').empty());
    OA_CHECK(shortcut_letter(0).empty());
    OA_CHECK(shortcut_letter('0').empty());
    const ControlName* colour = find_control_name("SKIRMISH.GUI", "Color0");
    OA_CHECK(colour != nullptr && colour->label == "Colour");
    OA_CHECK(colour != nullptr && colour->value == ControlValue::colour);
    const ControlName* move = find_control_name("ARMCOM1.GUI", "ARMMOVE");
    OA_CHECK(move != nullptr && move->label == "Move");
    const ControlName* stop = find_control_name("ARMGEN.GUI", "ARMSTOP");
    OA_CHECK(stop != nullptr && stop->label == "Stop");
    const ControlName* metal = find_control_name("guis/SKIRMISH.GUI", "Metal3");
    OA_CHECK(metal != nullptr && metal->label == "Starting metal");
    OA_CHECK(metal != nullptr && metal->value == ControlValue::number);
    const ControlName* ready = find_control_name("LOUNGE2.GUI", "READY0");
    OA_CHECK(ready != nullptr && ready->label == "Ready");
    OA_CHECK(slider_value_label("VIDSLDR") == "VIDVAL");
    OA_CHECK(slider_setting_text("GAMMA", 12, 21) == "1.0");
    OA_CHECK(slider_setting_text("METAL", 5, 10).empty());
    OA_CHECK(find_control_name("SELPROV.GUI", "SLIDER") == nullptr);
    OA_CHECK(find_control_name("LOUNGE2.GUI", "MAPPING") != nullptr);
    OA_CHECK(find_control_name("LOUNGE2.GUI", "MAPPING")->label == "Mapping Mode");
    OA_CHECK(find_control_name("SKIRMISH.GUI", "Mapping")->label == "Mapping");
    const ControlName* difficulty = find_control_name("NEWCAMP.GUI", "Difficulty");
    OA_CHECK(difficulty != nullptr && difficulty->label == "Difficulty");
    OA_CHECK(difficulty != nullptr && difficulty->key == ControlKey::added);
    const ControlName* mode = find_control_name("SOUNDS.GUI", "MODE");
    OA_CHECK(mode != nullptr && mode->label == "Sound mode");
    OA_CHECK(mode != nullptr && mode->value == ControlValue::caption);
    const ControlName* doors = find_control_name("LOUNGE2.GUI", "battlestart");
    OA_CHECK(doors != nullptr && doors->label == "Start doors");
    OA_CHECK(doors != nullptr && doors->value == ControlValue::doors);
    const ControlName* allies = find_control_name("SKIRMISH.GUI", "Allies0");
    OA_CHECK(allies != nullptr && allies->value == ControlValue::alliance);
    const ControlName* teams = find_control_name("LOUNGE2.GUI", "TEAMICONS0");
    OA_CHECK(teams != nullptr && teams->value == ControlValue::alliance);

    AutomationControl sound;
    sound.name = "MODE";
    sound.text = "Mono";
    ControlReading reading;
    reading.gui = "SOUNDS.GUI";
    reading.stages = 2;
    apply_control_words(sound, reading);
    OA_CHECK(sound.label == "Sound mode");
    OA_CHECK(sound.value_text == "Mono");

    AutomationControl start;
    start.name = "battlestart";
    reading = {};
    reading.gui = "LOUNGE2.GUI";
    reading.doors_known = true;
    reading.doors_closed = true;
    apply_control_words(start, reading);
    OA_CHECK(start.label == "Start doors");
    OA_CHECK(start.value_text == "Closed");
    reading.doors_closed = false;
    apply_control_words(start, reading);
    OA_CHECK(start.value_text == "Opened");

    AutomationControl team;
    team.name = "Allies0";
    reading = {};
    reading.gui = "SKIRMISH.GUI";
    reading.alliance_frame = 10;
    apply_control_words(team, reading);
    OA_CHECK(team.label == "Team");
    OA_CHECK(team.value_text == "None");
    reading.alliance_frame = 11;
    apply_control_words(team, reading);
    OA_CHECK(team.value_text == "None");
    reading.alliance_frame = 0;
    apply_control_words(team, reading);
    OA_CHECK(team.value_text == "1");
    reading.alliance_frame = 3;
    apply_control_words(team, reading);
    OA_CHECK(team.value_text == "2");

    AutomationControl side;
    side.name = "Side0";
    reading = {};
    reading.gui = "SKIRMISH.GUI";
    reading.side_name = "ARM";
    apply_control_words(side, reading);
    OA_CHECK(side.label == "Side");
    OA_CHECK(side.value_text == "Arm");
}

void names_are_unique() {
    using oa::app::control_name_at;
    using oa::app::control_name_count;
    std::map<std::string, std::string> seen;
    for (std::size_t index = 0; index < control_name_count(); ++index) {
        const oa::app::ControlName row = control_name_at(index);
        const std::string key = folded(row.gui) + "\n" + folded(row.gadget);
        const auto [placed, inserted] = seen.emplace(key, std::string(row.label));
        if (!inserted) {
            std::cerr << "duplicate name " << row.gui << " " << row.gadget << '\n';
            OA_CHECK(inserted);
        }
    }
    OA_CHECK(!seen.empty());
}

const gui::Layout* load_gui(
    const oa::AssetStore& assets, std::string_view file, std::map<std::string, gui::Layout>& loaded
) {
    const std::string key = folded(file);
    const auto have = loaded.find(key);
    if (have != loaded.end())
        return &have->second;
    const std::vector<uint8_t> bytes = oa::test::read_game_file(assets, "guis/" + key);
    if (bytes.empty()) {
        std::cerr << "missing GUI " << file << '\n';
        OA_CHECK(!bytes.empty());
        return nullptr;
    }
    const gui::ParseResult parsed = gui::parse(bytes);
    if (!parsed.ok()) {
        std::cerr << "GUI did not parse " << file << '\n';
        OA_CHECK(parsed.ok());
        return nullptr;
    }
    const auto [placed, inserted] = loaded.emplace(key, *parsed.layout);
    (void)inserted;
    return &placed->second;
}

void entries_name_installed_gadgets(const oa::AssetStore& assets) {
    using oa::app::control_name_at;
    using oa::app::control_name_count;
    using oa::app::ControlKey;
    std::map<std::string, gui::Layout> loaded;
    for (std::size_t index = 0; index < control_name_count(); ++index) {
        const oa::app::ControlName row = control_name_at(index);
        const gui::Layout* layout = load_gui(assets, row.gui, loaded);
        if (layout == nullptr)
            continue;
        if (row.key == ControlKey::star || row.key == ControlKey::added)
            continue;
        const bool present = has_gadget(*layout, row.gadget);
        if (!present)
            std::cerr << "table names a missing gadget " << row.gui << " " << row.gadget << '\n';
        OA_CHECK(present);
    }
}

/// Screens whose picture buttons, sliders and hot surfaces without words
/// each need a row. The build page's unit pictures are named from the unit
/// when a match is showing them, so they are not rows.
constexpr std::string_view kScreens[] = {
    "MAINMENU.GUI",
    "NEWCAMP.GUI",
    "NEWGAME.GUI",
    "LOADGAME.GUI",
    "SKIRMISH.GUI",
    "SELMAP.GUI",
    "SOUNDS.GUI",
    "SOUNDSRT.GUI",
    "VISUALS.GUI",
    "VISUALRT.GUI",
    "SPEEDS.GUI",
    "SPEEDSRT.GUI",
    "MUSIC.GUI",
    "MUSICRT.GUI",
    "LOUNGE2.GUI",
    "SELVMODE.GUI",
    "ARMGEN.GUI",
    "CORGEN.GUI",
};

void wordless_gadgets_are_named(const oa::AssetStore& assets) {
    std::map<std::string, gui::Layout> loaded;
    for (const std::string_view file : kScreens) {
        const gui::Layout* layout = load_gui(assets, file, loaded);
        if (layout == nullptr)
            continue;
        for (const gui::Gadget& gadget : layout->gadgets) {
            if (!wordless(gadget))
                continue;
            const oa::app::ControlName* named =
                oa::app::find_control_name(file, gadget.common.name);
            if (named == nullptr)
                std::cerr << "wordless gadget has no name " << file << " " << gadget.common.name
                          << '\n';
            OA_CHECK(named != nullptr);
        }
    }
    const gui::Layout* arm = load_gui(assets, "ARMCOM1.GUI", loaded);
    const gui::Layout* core = load_gui(assets, "CORCOM1.GUI", loaded);
    OA_CHECK(arm != nullptr && has_gadget(*arm, "ARMPREV") && has_gadget(*arm, "ARMNEXT"));
    OA_CHECK(core != nullptr && has_gadget(*core, "CORPREV") && has_gadget(*core, "CORNEXT"));
    OA_CHECK(oa::app::find_control_name("ARMCOM1.GUI", "ARMPREV") != nullptr);
    OA_CHECK(oa::app::find_control_name("CORCOM1.GUI", "CORNEXT") != nullptr);
}

} // namespace

int main() {
    words_for_colour_gamma_and_keys();
    names_are_unique();
    if (oa::test::failed_checks() != 0)
        return oa::test::check_exit_status();
    const oa::AssetStore assets = oa::test::require_game_assets("control names");
    entries_name_installed_gadgets(assets);
    wordless_gadgets_are_named(assets);
    return oa::test::check_exit_status();
}
