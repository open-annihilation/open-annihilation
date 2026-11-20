// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/gui_layout.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gui = oa::ui::gui_layout;

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

gui::ParseResult parse_text(std::string_view text, const gui::TranslationLookup& translation = {}) {
    return gui::parse(
        std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()), translation
    );
}

constexpr std::string_view main_menu_fixture = R"GUI(
[GADGET0]
{
  [COMMON] {
    id=0; assoc=205; name=Mainmenu.GUI; xpos=0; ypos=0; width=640; height=480;
    attribs=52685; colorf=52685; colorb=-129851; texturenumber=0;
    fontnumber=-51; active=1; commonattribs=-51; help=Original help text;
  }
  totalgadgets=99;
  [VERSION] { major=-51; minor=-51; revision=-51; }
  panel=; crdefault=; escdefault=; defaultfocus=SINGLE;
}
[GADGET1]
{
  [COMMON] {
    id=1; assoc=126; name=SINGLE; xpos=139; ypos=393; width=96; height=20;
    attribs=2; colorf=0; colorb=0; texturenumber=0; fontnumber=0;
    active=1
    commonattribs=54; help=Button help;
  }
  status=0; text=SINGLE; quickkey=83; grayedout=3; stages=0;
}
[GADGET2]
{
  [COMMON] {
    id=5; assoc=243; name=DebugString; xpos=320; ypos=300; width=300; height=0;
    attribs=80; colorf=15; colorb=0; texturenumber=101; fontnumber=0;
    active=0; commonattribs=-109; help=;
  }
  text=Debug Build; link=MORE;
}
)GUI";

void main_menu_fields_and_game_defaults() {
    std::vector<std::string> lookup_order;
    const auto result = parse_text(
        main_menu_fixture, [&lookup_order](std::string_view text) -> std::optional<std::string> {
            lookup_order.emplace_back(text);
            if (text.empty())
                return "Translated empty help";
            if (text == "SINGLE")
                return "Solo";
            return std::nullopt;
        }
    );
    require(result.ok(), result.error ? result.error->message : "main menu did not parse");
    require(result.layout->gadgets.size() == 3, "all top-level gadget sections were not loaded");

    const auto& root = result.layout->gadgets[0];
    const auto& panel = std::get<gui::PanelFields>(root.fields);
    require(
        root.common.type == gui::GadgetType::panel && root.common.association == 205 &&
            root.common.name == "Mainmenu.GUI" && root.common.width == 640 &&
            root.common.height == 480 && root.common.font_number == -51,
        "COMMON field widths or signed truncation disagreed"
    );
    require(
        root.common.background_color == static_cast<uint16_t>(static_cast<int32_t>(-129851)),
        "colors were not reduced to the game's unsigned 16-bit value"
    );
    require(
        root.common.source_help == "Original help text" &&
            root.common.runtime_help == "Translated empty help",
        "the empty help translation was not preserved"
    );
    require(
        panel.declared_total_gadgets == 99 && panel.loaded_total_gadgets == 2 &&
            panel.default_focus == "SINGLE" && panel.version.major == -51,
        "panel fields or parsed-count overwrite disagreed"
    );

    const auto& button = std::get<gui::ButtonFields>(result.layout->gadgets[1].fields);
    require(
        result.layout->gadgets[1].common.active == 1 &&
            result.layout->gadgets[1].common.common_attributes == 54,
        "missing semicolon at newline was not accepted like shipped GUI data"
    );
    require(
        button.source_text == "SINGLE" && button.text == "Solo" && button.quick_key == 83 &&
            button.grayed_out && button.stages == 0,
        "button translation, quick-key conversion, or bit field disagreed"
    );

    const auto& label = std::get<gui::LabelFields>(result.layout->gadgets[2].fields);
    require(
        label.text == "Debug Build" && label.link == "MORE", "label text/link fields disagreed"
    );
    require(
        lookup_order == std::vector<std::string>({"", "", "SINGLE", "", "Debug Build"}),
        "empty help lookup did not precede each gadget's subtype text lookup"
    );
}

std::string common(uint32_t type, std::string_view name) {
    return "[COMMON]{id=" + std::to_string(type) + ";assoc=0;name=" + std::string(name) +
           ";xpos=0;ypos=0;width=1;height=1;attribs=0;colorf=0;colorb=0;"
           "texturenumber=0;fontnumber=0;active=1;commonattribs=0;}";
}

void every_type_specific_loader() {
    std::string text;
    text += "[GADGET0]{" + common(0, "ROOT") + "totalgadgets=0;}";
    text += "[GADGET1]{" + common(2, "LIST") + "itemheight=-2;}";
    text += "[GADGET2]{" + common(3, "EDIT") + "maxchars=300;text=seed;}";
    text += "[GADGET3]{" + common(4, "SLIDE") +
            "range=-1;thick=65535;knobpos=4;knobsize=5;text=track;}";
    text += "[GADGET4]{" + common(6, "HOT") + "hotornot=2;}";
    text += "[GADGET5]{" + common(7, "FILE7") + "filename=corefont;}";
    text += "[GADGET6]{" + common(8, "FILE8") + "filename=armbutt;}";
    text += "[GADGET7]{" + common(10, "NULL") + "nuttin=-99;}";
    text += "[GADGET8]{" + common(12, "TYPE12") + "ignored=17;}";
    const auto result = parse_text(text);
    require(result.ok(), result.error ? result.error->message : "type fixture did not parse");
    require(
        std::get<gui::ListBoxFields>(result.layout->gadgets[1].fields).item_height == -2,
        "list-box item height disagreed"
    );
    require(
        std::get<gui::TextBoxFields>(result.layout->gadgets[2].fields).max_characters == 128,
        "text-box maximum was not clamped above 128"
    );
    const auto& slider = std::get<gui::ScrollBarFields>(result.layout->gadgets[3].fields);
    require(
        slider.range == -1 && slider.thickness == -1 && slider.knob_position == 4 &&
            slider.knob_size == 5 && slider.text == "track",
        "scroll-bar signed widths or text disagreed"
    );
    require(
        !std::get<gui::HotSurfaceFields>(result.layout->gadgets[4].fields).hot,
        "hotornot did not retain only its low bit"
    );
    require(
        std::get<gui::FileResourceFields>(result.layout->gadgets[5].fields).filename ==
                "corefont" &&
            std::get<gui::FileResourceFields>(result.layout->gadgets[6].fields).filename ==
                "armbutt",
        "shared type-7/type-8 filename behavior disagreed"
    );
    require(
        std::get<gui::NullResourceFields>(result.layout->gadgets[7].fields).nuttin == -99 &&
            std::holds_alternative<gui::CommonOnlyFields>(result.layout->gadgets[8].fields),
        "type-10 value or undispatched type-12 behavior disagreed"
    );
}

void malformed_and_bounded_inputs() {
    const std::vector<uint8_t> binary{0, 0xcd, 0x48, 0x45};
    const auto binary_result = gui::parse(binary);
    require(
        !binary_result.ok() && binary_result.error->code == gui::ErrorCode::binary_format,
        "compiled GUI input was not distinguished from the text format"
    );

    const auto malformed = parse_text("[GADGET0] { [COMMON] { id nope; } }");
    require(
        !malformed.ok() && malformed.error->code == gui::ErrorCode::malformed_syntax,
        "missing assignment separator was not rejected"
    );

    const auto translated_too_long = parse_text(
        "[GADGET0]{" + common(0, "ROOT") +
            "totalgadgets=1;}"
            "[GADGET1]{" +
            common(5, "LABEL") + "text=key;}",
        [](std::string_view) -> std::optional<std::string> {
            return std::string(gui::limit::text_bytes + 1, 'x');
        }
    );
    require(
        !translated_too_long.ok() &&
            translated_too_long.error->code == gui::ErrorCode::translated_text_limit,
        "oversized translated text was not bounded"
    );

    std::string too_many;
    for (std::size_t index = 0; index <= gui::limit::gadgets; ++index) {
        too_many +=
            "[GADGET" + std::to_string(index) + "]{" + common(index == 0 ? 0U : 12U, "G") + "}";
    }
    const auto gadget_limit = parse_text(too_many);
    require(
        !gadget_limit.ok() && gadget_limit.error->code == gui::ErrorCode::gadget_limit,
        "200-record backing allocation was not enforced before growth"
    );
}

gui::Gadget placed(gui::GadgetType type, int16_t x, int16_t y, int16_t width, int16_t height) {
    gui::Gadget gadget;
    gadget.common.type = type;
    gadget.common.x = x;
    gadget.common.y = y;
    gadget.common.width = width;
    gadget.common.height = height;
    return gadget;
}

void positioned_rectangle_adds_root_origin_only_for_nonzero_type() {
    const std::vector<gui::Gadget> gadgets{
        placed(gui::GadgetType::panel, 100, -20, 30, 10),
        placed(gui::GadgetType::button, 5, 7, 8, 2),
        placed(gui::GadgetType::panel, -4, 3, 0, 1),
    };
    const auto root = gui::gadget_rectangle(gadgets, 0);
    require(
        root == gui::Rect{100, -20, 129, -11},
        "the rectangle did not keep a type-zero origin or its inclusive edges"
    );
    const auto button = gui::gadget_rectangle(gadgets, 1);
    require(
        button == gui::Rect{105, -13, 112, -12},
        "the rectangle did not add the root record's stored origin"
    );
    const auto nested_panel = gui::gadget_rectangle(gadgets, 2);
    require(
        nested_panel == gui::Rect{-4, 3, -5, 3},
        "the rectangle added the root origin to a zero type byte"
    );
    require(!gui::gadget_rectangle(gadgets, 3), "an index past the records was not rejected");
    require(!gui::gadget_rectangle({}, 0), "an empty record span was not rejected");

    const std::vector<gui::Gadget> nonzero_root{placed(gui::GadgetType::button, 4, 5, 6, 7)};
    require(
        gui::gadget_rectangle(nonzero_root, 0) == gui::Rect{8, 10, 13, 16},
        "the rectangle did not add a nonzero index-zero origin twice"
    );

    const std::vector<gui::Gadget> negative_width{placed(gui::GadgetType::panel, 10, 10, -1, 5)};
    require(
        gui::gadget_rectangle(negative_width, 0) == gui::Rect{10, 10, 8, 14},
        "signed width was not preserved in the inclusive right edge"
    );
}

void shipped_score_eof_is_an_implicit_top_level_close() {
    const auto result = parse_text(
        "[GADGET0]{" + common(0, "ROOT") +
        "totalgadgets=1;}"
        "[GADGET1]{" +
        common(5, "LAST") + "text=BUTTON NAME;\n"
    );
    require(
        result.ok() && result.layout->gadgets.size() == 2 &&
            std::get<gui::LabelFields>(result.layout->gadgets[1].fields).text == "BUTTON NAME",
        "shipped SCORE.GUI final-section EOF behavior was not preserved"
    );
}

void stray_text_runs_on_into_the_next_field() {
    // A build page's button whose COMMON block ends "}1", as some mods'
    // pages do: the stray digit runs on into the next line's grayedout,
    // which is left unread, and the rest of the page loads.
    const auto stray = parse_text(
        "[GADGET0]\r\n{\r\n\t" + common(0, "HEADER") +
        "\r\n\ttotalgadgets=2;\r\n}\r\n"
        "[GADGET1]\r\n\t{\r\n\t[COMMON]\r\n\t\t{\r\n\t\tid=1;\r\n\t\tname=ARMVPE;\r\n"
        "\t\tcommonattribs=4;\r\n\t\t}1\r\n\tgrayedout=1;\r\n\tquickkey=118;\r\n\t}\r\n"
        "[GADGET2]\r\n{\r\n\t" +
        common(1, "ARMALAB") + "\r\n\tgrayedout=1;\r\n}\r\n"
    );
    require(stray.ok(), stray.error ? stray.error->message : "the stray digit was rejected");
    require(stray.layout->gadgets.size() == 3, "the gadgets after the stray digit were lost");
    const auto& stray_button = std::get<gui::ButtonFields>(stray.layout->gadgets[1].fields);
    require(
        stray.layout->gadgets[1].common.name == "ARMVPE" &&
            stray.layout->gadgets[1].common.common_attributes == 4 && !stray_button.grayed_out &&
            stray_button.quick_key == 118,
        "the stray digit did not take only the next field with it"
    );
    require(
        stray.layout->gadgets[2].common.name == "ARMALAB" &&
            std::get<gui::ButtonFields>(stray.layout->gadgets[2].fields).grayed_out,
        "the gadget after the stray digit did not load"
    );

    // A name whose '=' starts the next line is still that name.
    const auto split = parse_text(
        "[GADGET0]{" + common(0, "ROOT") + "totalgadgets=1;}[GADGET1]{" + common(1, "B") +
        "grayedout\r\n  =1;}"
    );
    require(
        split.ok() && std::get<gui::ButtonFields>(split.layout->gadgets[1].fields).grayed_out,
        "a name whose '=' starts the next line was not read"
    );

    // Comments between the stray text and the field are passed over.
    const auto commented = parse_text(
        "[GADGET0]{" + common(0, "ROOT") + "totalgadgets=1;}[GADGET1]{" + common(1, "B") +
        "1\n// stray=0;\n/* text=x; */ grayedout=1;\nquickkey=7;}"
    );
    require(
        commented.ok() &&
            !std::get<gui::ButtonFields>(commented.layout->gadgets[1].fields).grayed_out &&
            std::get<gui::ButtonFields>(commented.layout->gadgets[1].fields).quick_key == 7,
        "a comment after the stray text ended its run"
    );

    // Stray text with no field after it in its section is still refused.
    const auto unfinished = parse_text("[GADGET0]{" + common(0, "ROOT") + "totalgadgets=0;\n1\n}");
    require(
        !unfinished.ok() && unfinished.error->code == gui::ErrorCode::malformed_syntax,
        "stray text that ran into its section's end was accepted"
    );
}

void numbers_read_their_leading_digits() {
    // As the game reads a number: leading spaces, one sign and the digits
    // after it; what follows is ignored, no digits read 0, and a value past
    // 32 bits wraps before the field keeps its low bits.
    const auto result = parse_text(
        "[GADGET0]{[COMMON]{id=oops;xpos=+12;ypos= -3px;width=4294967297;height=0x40;"
        "commonattribs=O;}totalgadgets=1;}"
        "[GADGET1]{" +
        common(1, "PATCH") + "status=7th;grayedout=1.5;quickkey= 105;stages=-;}"
    );
    require(result.ok(), result.error ? result.error->message : "the numbers were rejected");
    const auto& root = result.layout->gadgets[0].common;
    require(
        root.type == gui::GadgetType::panel && root.x == 12 && root.y == -3 && root.width == 1 &&
            root.height == 0 && root.common_attributes == 0,
        "a number was not read from its leading digits"
    );
    const auto& button = std::get<gui::ButtonFields>(result.layout->gadgets[1].fields);
    require(
        button.status == 7 && button.grayed_out && button.quick_key == 105 && button.stages == 0,
        "a button's numbers were not read from their leading digits"
    );
}

void skin_tiles_end_flush_with_the_far_edges() {
    using Tile = gui::SkinTile;
    // 400x100 with 64-pixel tiles: seven columns, the last moved left to 336,
    // and a bottom row moved up to 36.
    const auto panel = gui::skin_tiles(400, 100, 64, 64);
    require(panel.size() == 14, "a 400x100 skin did not take seven columns of two rows");
    require(panel[0] == Tile{0, 0, 0}, "the top-left tile was not frame 0 at the corner");
    require(panel[1] == Tile{1, 64, 0}, "the second top tile was not a middle frame");
    require(panel[6] == Tile{2, 336, 0}, "the top-right tile did not end flush with the edge");
    require(panel[7] == Tile{6, 0, 36}, "the bottom row did not move up to end flush");
    require(panel[13] == Tile{8, 336, 36}, "the bottom-right tile was not frame 8 flush");

    // A whole number of tiles high: the last row stays a middle one.
    const auto whole = gui::skin_tiles(64, 128, 64, 64);
    require(
        whole == std::vector<Tile>{{2, 0, 0}, {5, 0, 64}},
        "a skin two tiles high did not keep middle frames on its last row"
    );

    // Narrower and shorter than a tile: one right-column tile moved up and left.
    require(
        gui::skin_tiles(40, 20, 64, 64) == std::vector<Tile>{{2, -24, -44}},
        "a skin smaller than a tile was not one moved top-right tile"
    );
    require(gui::skin_tiles(40, 0, 64, 64).empty(), "a skin with no height took tiles");
    require(gui::skin_tiles(40, 20, 0, 64).empty(), "a zero-width frame took tiles");
}

void button_captions_follow_their_alignment() {
    using Pen = gui::CaptionPen;
    namespace attribute = gui::attribute;
    // A build page's second unit button (64x64 at 64,155, attribs=32) and a
    // 14-pixel caption in a font 14 rows high: centred across, its line
    // ending 4 rows above the bottom edge.
    const gui::Rect build{64, 155, 127, 218};
    const auto pen = [&](uint32_t attributes, int32_t width, bool pressed) {
        return gui::button_caption_pen(build, attributes, width, 14, pressed);
    };
    require(pen(attribute::bottom_caption, 14, false) == Pen{89, 200}, "bottom caption misplaced");
    require(
        pen(attribute::bottom_caption, 14, true) == Pen{90, 201},
        "a pressed bottom caption did not move one pixel right and down"
    );
    require(pen(attribute::centered, 14, false) == Pen{89, 179}, "centred caption misplaced");
    require(pen(attribute::horizontal, 14, true) == Pen{68, 180}, "left caption misplaced");
    require(pen(attribute::right_aligned, 14, true) == Pen{110, 180}, "right caption misplaced");
    // A caption wider than the button: centred it starts left of the edge,
    // half the overflow rounded toward zero; aligned right it starts at it.
    require(pen(attribute::centered, 70, false) == Pen{62, 179}, "wide centred caption misplaced");
    require(
        pen(attribute::right_aligned, 70, false) == Pen{64, 179}, "wide right caption misplaced"
    );
    // The first alignment the attributes hold decides.
    require(
        pen(attribute::horizontal | attribute::bottom_caption, 14, false) == Pen{67, 179},
        "left alignment did not come before the bottom caption"
    );
    require(
        pen(attribute::centered | attribute::bottom_caption, 14, false) == Pen{89, 179},
        "centring did not come before the bottom caption"
    );
    require(!pen(0, 14, false), "a button without an alignment placed a caption");
    require(!pen(attribute::toggle, 14, false), "a toggle without an alignment placed a caption");
}

} // namespace

int main() {
    try {
        main_menu_fields_and_game_defaults();
        every_type_specific_loader();
        malformed_and_bounded_inputs();
        positioned_rectangle_adds_root_origin_only_for_nonzero_type();
        shipped_score_eof_is_an_implicit_top_level_close();
        stray_text_runs_on_into_the_next_field();
        numbers_read_their_leading_digits();
        skin_tiles_end_flush_with_the_far_edges();
        button_captions_follow_their_alignment();
    } catch (const std::exception& error) {
        std::cerr << "gui-layout test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "gui-layout tests passed\n";
    return 0;
}
