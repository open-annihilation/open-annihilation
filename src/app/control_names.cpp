// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The names table for the original game's picture buttons, colour cells and
// sliders (control_names.hpp).
#include "oa/app/control_names.hpp"

#include "oa/data/languages/interface_text.hpp"

#include <cctype>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

struct Row {
    std::string gui;
    std::string gadget;
    std::string label;
    ControlValue value = ControlValue::none;
    ControlKey key = ControlKey::exact;
};

struct Table {
    std::vector<Row> rows;
    std::vector<ControlName> views;
};

/// Folds one ASCII letter. Any other byte is unchanged.
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

/// The file's name without its folder or its suffix.
std::string_view file_stem(std::string_view file) {
    if (const auto folder = file.find_last_of("/\\"); folder != std::string_view::npos)
        file.remove_prefix(folder + 1);
    if (const auto suffix = file.rfind('.'); suffix != std::string_view::npos)
        file = file.substr(0, suffix);
    return file;
}

bool same_file(std::string_view left, std::string_view right) {
    return same_text(file_stem(left), file_stem(right));
}

/// The family stem of a star or slot row: "Side*" and "SIDEx" both yield their prefix.
std::string_view family_stem(const Row& row) {
    std::string_view gadget = row.gadget;
    if (row.key == ControlKey::star && !gadget.empty() && gadget.back() == '*')
        gadget.remove_suffix(1);
    else if (
        row.key == ControlKey::slot && !gadget.empty() &&
        (gadget.back() == 'x' || gadget.back() == 'X')
    )
        gadget.remove_suffix(1);
    return gadget;
}

/// Reports whether `name` is `stem` plus one or more digits.
bool numbered(std::string_view name, std::string_view stem) {
    if (stem.empty() || name.size() <= stem.size())
        return false;
    if (!same_text(name.substr(0, stem.size()), stem))
        return false;
    for (std::size_t index = stem.size(); index < name.size(); ++index) {
        const char digit = name[index];
        if (digit < '0' || digit > '9')
            return false;
    }
    return true;
}

bool names_gadget(const Row& row, std::string_view gadget) {
    if (same_text(row.gadget, gadget))
        return true;
    if (row.key == ControlKey::exact || row.key == ControlKey::added)
        return false;
    return numbered(gadget, family_stem(row));
}

/// The side prefix of a HUD file: ARMGEN and ARMCOM1 are both Arm's.
std::string_view hud_prefix(std::string_view file) {
    const std::string_view stem = file_stem(file);
    if (stem.size() >= 3 && same_text(stem.substr(0, 3), "arm"))
        return "ARM";
    if (stem.size() >= 3 && same_text(stem.substr(0, 3), "cor"))
        return "COR";
    return {};
}

bool same_hud_family(std::string_view left, std::string_view right) {
    const std::string_view prefix = hud_prefix(left);
    return !prefix.empty() && same_text(prefix, hud_prefix(right));
}

/// Reports whether a gadget belongs to the HUD file's side.
bool gadget_of_side(std::string_view gadget, std::string_view file) {
    const std::string_view prefix = hud_prefix(file);
    return !prefix.empty() && gadget.size() >= prefix.size() &&
           same_text(gadget.substr(0, prefix.size()), prefix);
}

void add(
    std::vector<Row>& rows,
    std::string_view gui,
    std::string_view gadget,
    std::string_view label,
    ControlValue value,
    ControlKey key
) {
    Row row;
    row.gui.assign(gui);
    row.gadget.assign(gadget);
    row.label.assign(label);
    row.value = value;
    row.key = key;
    rows.push_back(std::move(row));
}

/// The order, tab and page buttons, one name for each side.
void add_hud(std::vector<Row>& rows) {
    struct Button {
        const char* suffix;
        const char* label;
    };

    static constexpr Button buttons[] = {
        {"BUILD", "Build"},
        {"ORDERS", "Orders"},
        {"MOVE", "Move"},
        {"STOP", "Stop"},
        {"PATROL", "Patrol"},
        {"ATTACK", "Attack"},
        {"DEFEND", "Guard"},
        {"FIREORD", "Fire state"},
        {"MOVEORD", "Move state"},
        {"ONOFF", "On or off"},
        {"CAPTURE", "Capture"},
        {"RECLAIM", "Reclaim"},
        {"REPAIR", "Repair"},
        {"LOAD", "Load"},
        {"UNLOAD", "Unload"},
        {"CLOAK", "Cloak"},
        {"BLAST", "D-gun"},
    };
    for (const Button& button : buttons) {
        add(rows,
            "ARMGEN.GUI",
            std::string("ARM") + button.suffix,
            button.label,
            ControlValue::none,
            ControlKey::exact);
        add(rows,
            "CORGEN.GUI",
            std::string("COR") + button.suffix,
            button.label,
            ControlValue::none,
            ControlKey::exact);
    }
    add(rows, "ARMCOM1.GUI", "ARMPREV", "Previous page", ControlValue::none, ControlKey::exact);
    add(rows, "ARMCOM1.GUI", "ARMNEXT", "Next page", ControlValue::none, ControlKey::exact);
    add(rows, "CORCOM1.GUI", "CORPREV", "Previous page", ControlValue::none, ControlKey::exact);
    add(rows, "CORCOM1.GUI", "CORNEXT", "Next page", ControlValue::none, ControlKey::exact);
}

std::vector<Row> make_rows() {
    std::vector<Row> rows;
    add(rows, "MAINMENU.GUI", "Credits", "Credits", ControlValue::none, ControlKey::exact);
    add(rows, "NEWCAMP.GUI", "Side0", "Arm", ControlValue::none, ControlKey::exact);
    add(rows, "NEWCAMP.GUI", "Side1", "Core", ControlValue::none, ControlKey::exact);
    add(rows, "NEWCAMP.GUI", "KNOB", "Campaigns", ControlValue::none, ControlKey::exact);
    add(rows, "NEWCAMP.GUI", "Difficulty", "Difficulty", ControlValue::caption, ControlKey::added);
    add(rows, "NEWGAME.GUI", "Side0", "Arm", ControlValue::none, ControlKey::exact);
    add(rows, "NEWGAME.GUI", "Side1", "Core", ControlValue::none, ControlKey::exact);
    add(rows, "NEWGAME.GUI", "CampaignKnob", "Campaigns", ControlValue::none, ControlKey::exact);
    add(rows, "NEWGAME.GUI", "MissionsKnob", "Missions", ControlValue::none, ControlKey::exact);
    add(rows, "NEWGAME.GUI", "Difficulty", "Difficulty", ControlValue::caption, ControlKey::added);
    add(rows, "LOADGAME.GUI", "RADAR", "Preview", ControlValue::none, ControlKey::exact);
    add(rows, "LOADGAME.GUI", "SLIDER", "Saved games", ControlValue::none, ControlKey::exact);
    add(rows, "SKIRMISH.GUI", "Side*", "Side", ControlValue::side, ControlKey::star);
    add(rows, "SKIRMISH.GUI", "Color*", "Colour", ControlValue::colour, ControlKey::star);
    add(rows, "SKIRMISH.GUI", "Allies*", "Team", ControlValue::alliance, ControlKey::star);
    add(rows, "SKIRMISH.GUI", "Metal*", "Starting metal", ControlValue::number, ControlKey::star);
    add(rows, "SKIRMISH.GUI", "Energy*", "Starting energy", ControlValue::number, ControlKey::star);
    add(
        rows, "SKIRMISH.GUI", "StartLocation", "Location", ControlValue::caption, ControlKey::exact
    );
    add(rows,
        "SKIRMISH.GUI",
        "CommanderDeath",
        "Commander",
        ControlValue::caption,
        ControlKey::exact);
    add(rows, "SKIRMISH.GUI", "Mapping", "Mapping", ControlValue::caption, ControlKey::exact);
    add(rows,
        "SKIRMISH.GUI",
        "LineOfSight",
        "Line of sight",
        ControlValue::caption,
        ControlKey::exact);
    add(rows, "SKIRMISH.GUI", "Difficulty", "Difficulty", ControlValue::caption, ControlKey::exact);
    add(rows, "SELMAP.GUI", "SLIDER", "Maps", ControlValue::none, ControlKey::exact);
    add(rows, "SELMAP.GUI", "MAPPIC", "Map", ControlValue::none, ControlKey::exact);
    for (const char* file : {"SOUNDS.GUI", "SOUNDSRT.GUI"}) {
        add(rows, file, "FXVOL", "Sound volume", ControlValue::number, ControlKey::exact);
        add(rows, file, "MODE", "Sound mode", ControlValue::caption, ControlKey::exact);
        add(rows, file, "SPEECH", "Unit sounds", ControlValue::caption, ControlKey::exact);
    }
    for (const char* file : {"VISUALS.GUI", "VISUALRT.GUI"}) {
        add(rows, file, "GAMMA", "Gamma", ControlValue::number, ControlKey::exact);
        add(rows, file, "SHADING", "Shading", ControlValue::caption, ControlKey::exact);
        add(rows, file, "ANTI", "Anti-aliasing", ControlValue::caption, ControlKey::exact);
        add(rows, file, "BSHADOWS", "Shadows", ControlValue::caption, ControlKey::exact);
    }
    for (const char* file : {"VISUALS.GUI", "SELVMODE.GUI"})
        add(rows, file, "VIDSLDR", "Screen size", ControlValue::number, ControlKey::exact);
    for (const char* file : {"SPEEDS.GUI", "SPEEDSRT.GUI"}) {
        add(rows, file, "GAME", "Game speed", ControlValue::number, ControlKey::exact);
        add(rows, file, "SCREEN", "Screen scroll", ControlValue::number, ControlKey::exact);
        add(rows, file, "TXTSCROL", "Text delay", ControlValue::number, ControlKey::exact);
        add(rows, file, "MAXLINES", "Text lines", ControlValue::number, ControlKey::exact);
        add(rows, file, "LEFTCLICK", "Interface", ControlValue::caption, ControlKey::exact);
        add(rows, file, "UNITCHAT", "Unit text acks", ControlValue::caption, ControlKey::exact);
    }
    for (const char* file : {"MUSIC.GUI", "MUSICRT.GUI"}) {
        add(rows, file, "MUSICVOL", "Music volume", ControlValue::number, ControlKey::exact);
        add(rows, file, "NOTRAK", "CD music", ControlValue::caption, ControlKey::exact);
        add(rows, file, "TRACKTYPE", "Track type", ControlValue::caption, ControlKey::exact);
        add(rows, file, "TRACKMODE", "Play mode", ControlValue::caption, ControlKey::exact);
        add(rows, file, "CDPREV", "Previous track", ControlValue::none, ControlKey::exact);
        add(rows, file, "CDSTOP", "Stop", ControlValue::none, ControlKey::exact);
        add(rows, file, "CDPLAY", "Play track", ControlValue::none, ControlKey::exact);
        add(rows, file, "CDNEXT", "Next track", ControlValue::none, ControlKey::exact);
    }
    add(rows, "LOUNGE2.GUI", "READYx", "Ready", ControlValue::none, ControlKey::slot);
    add(rows, "LOUNGE2.GUI", "LOGOx", "Colour", ControlValue::colour, ControlKey::slot);
    add(rows, "LOUNGE2.GUI", "SIDEx", "Side", ControlValue::side, ControlKey::slot);
    add(rows, "LOUNGE2.GUI", "ALLYx", "Alliance", ControlValue::none, ControlKey::slot);
    add(rows, "LOUNGE2.GUI", "TEAMICONSx", "Team", ControlValue::alliance, ControlKey::slot);
    add(rows, "LOUNGE2.GUI", "METAL", "Metal", ControlValue::number, ControlKey::exact);
    add(rows, "LOUNGE2.GUI", "ENERGY", "Energy", ControlValue::number, ControlKey::exact);
    add(rows, "LOUNGE2.GUI", "MAXUNITS", "Max units", ControlValue::number, ControlKey::exact);
    add(
        rows, "LOUNGE2.GUI", "COMMANDER", "Dead Commander", ControlValue::caption, ControlKey::exact
    );
    add(rows, "LOUNGE2.GUI", "MAPPING", "Mapping Mode", ControlValue::caption, ControlKey::exact);
    add(rows, "LOUNGE2.GUI", "LOSTYPE", "Line of Sight", ControlValue::caption, ControlKey::exact);
    add(rows, "LOUNGE2.GUI", "CHEATING", "Cheat Codes", ControlValue::caption, ControlKey::exact);
    add(rows, "LOUNGE2.GUI", "FIXEDLOC", "Location", ControlValue::caption, ControlKey::exact);
    add(rows, "LOUNGE2.GUI", "WATCHING", "Watching", ControlValue::caption, ControlKey::exact);
    add(rows, "LOUNGE2.GUI", "GAMEOPEN", "Game Status", ControlValue::caption, ControlKey::exact);
    add(rows, "LOUNGE2.GUI", "battlestart", "Start doors", ControlValue::doors, ControlKey::exact);
    add_hud(rows);
    return rows;
}

const Table& table() {
    static const Table built = [] {
        Table made;
        made.rows = make_rows();
        made.views.reserve(made.rows.size());
        for (const Row& row : made.rows)
            made.views.push_back(ControlName{row.gui, row.gadget, row.label, row.value, row.key});
        return made;
    }();
    return built;
}

/// The stored setting a knob stands for: knob / (range - 1) * maximum,
/// truncated toward zero, as the options screens read a bar.
int32_t setting_of(int32_t knob, int32_t range, int32_t maximum) {
    if (range < 2 || maximum <= 0 || knob <= 0)
        return 0;
    const double scaled =
        static_cast<double>(knob) / static_cast<double>(range - 1) * static_cast<double>(maximum);
    if (scaled <= 0.0)
        return 0;
    return static_cast<int32_t>(scaled);
}

struct Scale {
    const char* gadget = nullptr;
    int32_t maximum = 0;
    bool gamma = false;
};

// The greatest setting each options bar stores. Gamma is shown as a decimal
// rather than as this whole number.
constexpr Scale kScales[] = {
    {"FXVOL", 0x40, false},
    {"MUSICVOL", 0x40, false},
    {"GAMMA", 0x14, true},
    {"GAME", 0x15, false},
    {"SCREEN", 0x41, false},
    {"TXTSCROL", 0x28, false},
    {"MAXLINES", 0x1e, false},
};

const Scale* scale_for(std::string_view gadget) {
    for (const Scale& scale : kScales)
        if (same_text(scale.gadget, gadget))
            return &scale;
    return nullptr;
}

bool drawn_number(std::string_view text) {
    return !text.empty() && text != "#";
}

/// The catalogue's words for an English text. The view is copied at once:
/// the next lookup replaces it.
std::string spoken(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

std::string fill_token(std::string text, std::string_view token, std::string_view value) {
    for (std::size_t at = text.find(token); at != std::string::npos; at = text.find(token, at)) {
        text.replace(at, token.size(), value);
        at += value.size();
    }
    return text;
}

/// Arm, Core and Watcher, in the player's language. A side the catalogue
/// does not name is left as the screen wrote it.
std::string side_words(std::string_view name) {
    std::string_view key;
    if (same_text(name, "ARM") || same_text(name, "Arm"))
        key = "Arm";
    else if (same_text(name, "CORE") || same_text(name, "Core"))
        key = "Core";
    else if (same_text(name, "WATCHER") || same_text(name, "Watcher"))
        key = "Watcher";
    else
        return std::string(name);
    return spoken(key);
}

/// Frames 10 and 11 are no alliance: the cell is empty. Any other frame
/// through 9 is that alliance, counted from 1. The picture is the stored
/// alliance times two, plus one when the alliance has a single member.
std::string alliance_words(int frame) {
    if (frame < 0 || frame > 11)
        return {};
    if (frame == 10 || frame == 11)
        return spoken("None");
    return std::to_string(frame / 2 + 1);
}

std::string neighbour_text(const ControlReading& reading, std::string_view gadget) {
    if (drawn_number(reading.drawn))
        return reading.drawn;
    const std::string_view label = slider_value_label(gadget);
    if (label.empty())
        return {};
    for (const auto& neighbour : reading.neighbours) {
        if (!same_text(neighbour.first, label))
            continue;
        if (drawn_number(neighbour.second))
            return neighbour.second;
    }
    return {};
}

struct InstallNamer {
    InstallNamer() { set_control_namer(&apply_control_words); }
};

const InstallNamer kInstallNamer;

} // namespace

std::size_t control_name_count() {
    return table().views.size();
}

ControlName control_name_at(std::size_t index) {
    if (index >= table().views.size())
        return {};
    return table().views[index];
}

const ControlName* find_control_name(std::string_view gui, std::string_view gadget) {
    const Table& names = table();
    const ControlName* same_name = nullptr;
    for (std::size_t index = 0; index < names.rows.size(); ++index) {
        const Row& row = names.rows[index];
        if (!names_gadget(row, gadget))
            continue;
        if (same_file(row.gui, gui))
            return &names.views[index];
        if (row.key == ControlKey::exact && same_name == nullptr && same_text(row.gadget, gadget) &&
            same_hud_family(row.gui, gui) && gadget_of_side(gadget, gui))
            same_name = &names.views[index];
    }
    return same_name;
}

std::string_view slider_value_label(std::string_view gadget) {
    struct Pair {
        const char* bar;
        const char* label;
    };

    static constexpr Pair pairs[] = {
        {"VIDSLDR", "VIDVAL"},
        {"TXTSCROL", "TEXTSCROLLTEXT"},
        {"MAXLINES", "MAXLINESTEXT"},
        {"METAL", "METALTEXT"},
        {"ENERGY", "ENERGYTEXT"},
        {"MAXUNITS", "MAXUNITSTEXT"},
    };
    for (const Pair& pair : pairs)
        if (same_text(pair.bar, gadget))
            return pair.label;
    return {};
}

std::string colour_text(int colour, int player) {
    static constexpr std::string_view kNames[] = {
        "Blue",
        "Red",
        "White",
        "Green",
        "Navy",
        "Purple",
        "Yellow",
        "Black",
        "Light blue",
        "Tan",
    };
    constexpr int kColourCount = 10;
    if (colour < 0 || colour >= kColourCount || player <= 0)
        return {};
    const std::string colour_name(oa::data::languages::interface_text(kNames[colour]));
    const std::string pattern(oa::data::languages::interface_text("{colour}, player {number}"));
    return fill_token(
        fill_token(pattern, "{colour}", colour_name), "{number}", std::to_string(player)
    );
}

std::string gamma_text(int32_t setting) {
    if (setting < 0)
        setting = 0;
    const auto tenths =
        static_cast<int>(std::lround((static_cast<double>(setting) / 24.0 + 0.5) * 10.0));
    const int whole = tenths / 10;
    const int fraction = tenths % 10;
    return std::to_string(whole) + '.' + std::to_string(fraction);
}

std::string slider_setting_text(std::string_view gadget, int32_t knob, int32_t range) {
    const Scale* scale = scale_for(gadget);
    if (scale == nullptr)
        return {};
    const int32_t setting = setting_of(knob, range, scale->maximum);
    if (scale->gamma)
        return gamma_text(setting);
    return std::to_string(setting);
}

std::string shortcut_letter(int quick_key) {
    if (quick_key >= 'a' && quick_key <= 'z')
        return std::string(1, static_cast<char>(quick_key - 'a' + 'A'));
    if (quick_key >= 'A' && quick_key <= 'Z')
        return std::string(1, static_cast<char>(quick_key));
    return {};
}

void apply_control_words(AutomationControl& control, const ControlReading& reading) {
    const ControlName* named = find_control_name(reading.gui, control.name);
    const bool cycling = reading.stages > 1;
    const bool doors = named != nullptr && named->value == ControlValue::doors;
    if (doors) {
        control.label = spoken(named->label);
        if (reading.doors_known)
            control.value_text = spoken(reading.doors_closed ? "Closed" : "Opened");
    } else if (cycling && !reading.linked_label.empty()) {
        control.label = reading.linked_label;
        control.value_text = control.text;
    } else if (named != nullptr) {
        const bool own_caption =
            !control.text.empty() && named->value == ControlValue::none && !cycling;
        if (!own_caption) {
            control.label = spoken(named->label);
            switch (named->value) {
            case ControlValue::none:
            case ControlValue::doors:
                break;
            case ControlValue::caption:
                control.value_text = control.text;
                break;
            case ControlValue::number: {
                const std::string shown = neighbour_text(reading, control.name);
                if (drawn_number(shown))
                    control.value_text = shown;
                else if (!control.text.empty())
                    control.value_text = control.text;
                else if (reading.has_slider) {
                    const std::string setting =
                        slider_setting_text(control.name, reading.knob, reading.range);
                    if (!setting.empty())
                        control.value_text = setting;
                }
                break;
            }
            case ControlValue::colour:
                control.value_text = colour_text(reading.colour, reading.player);
                break;
            case ControlValue::side:
                if (!reading.side_name.empty())
                    control.value_text = side_words(reading.side_name);
                break;
            case ControlValue::alliance:
                control.value_text = alliance_words(reading.alliance_frame);
                break;
            }
        }
    }
    if (!reading.shortcut.empty())
        control.shortcut = reading.shortcut;
    else if (reading.publish_letter) {
        const std::string letter = shortcut_letter(reading.quick_key);
        if (!letter.empty())
            control.shortcut = letter;
    }
}

} // namespace oa::app
