// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The photo panel at 640x480, 1280x720 and 1920x1080, in each size class and
// both looks, for a skirmish, a replay and a shared match. Every control
// lies inside the panel and the panel inside the window, including Large at
// 640x480. Tab follows the sections. A locked control carries its reason.
//
// oa-ui-photo-panel-layout-test

#include "oa/platform/text_font.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/photo_panel/layout.hpp"
#include "block_font.hpp"

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace panel = oa::ui::photo_panel;

/// Prints a failure and counts it.
void expect(bool ok, const char* what) {
    if (!ok)
        std::fprintf(stderr, "layout: %s\n", what);
    OA_CHECK(ok);
}

/// The control of that name, or null.
const kit::Control* named(const kit::DisplayList& list, std::string_view name) {
    for (const kit::Control& control : list.controls) {
        if (control.name == name)
            return &control;
    }
    return nullptr;
}

/// Tab's names, in order.
std::vector<std::string> tab_names(const panel::Layout& placed) {
    std::vector<std::string> names;
    for (const kit::ControlId id : placed.list.tab_order) {
        const kit::Control* control = kit::control_of(placed.list, id);
        names.push_back(control == nullptr ? std::string{} : control->name);
    }
    return names;
}

/// The names Tab should walk while these sections are open.
std::vector<std::string> expected_tab(const panel::Model& model) {
    if (!model.shown)
        return {"photo.hint"};
    std::vector<std::string> names{"photo.leave", "photo.section.camera"};
    if (model.camera_open) {
        const char* camera[] = {
            "photo.zoom", "photo.turn", "photo.tilt", "photo.classic", "photo.follow"
        };
        names.insert(names.end(), std::begin(camera), std::end(camera));
    }
    names.push_back("photo.section.time");
    if (model.time_open) {
        const char* time[] = {"photo.time-mode", "photo.speed", "photo.step", "photo.between"};
        names.insert(names.end(), std::begin(time), std::end(time));
    }
    names.push_back("photo.section.look");
    if (model.look_open) {
        const char* look[] = {"photo.selection", "photo.health", "photo.guides"};
        names.insert(names.end(), std::begin(look), std::end(look));
    }
    names.push_back("photo.section.capture");
    if (model.capture_open) {
        const char* capture[] = {
            "photo.copy-to-clipboard", "photo.player-names", "photo.take", "photo.copy"
        };
        names.insert(names.end(), std::begin(capture), std::end(capture));
    }
    return names;
}

/// A panel at one visit, with the locks that visit carries.
panel::Model
model_for(int32_t width, int32_t height, kit::SizeClass size_class, panel::Visit visit) {
    panel::Model model;
    model.width = width;
    model.height = height;
    model.size_class = size_class;
    model.visit = visit;
    model.zoom_value = "1.0  Medium";
    model.turn_value = "0";
    model.tilt_value = "0";
    model.turn_lock = "Needs Full acceleration";
    model.tilt_lock = "Needs Full acceleration";
    model.speed_value = "30%";
    model.between_value = "8";
    model.clock = "12:34";
    model.file_name = "Photo 2026-11-05 13.12.08 Canal Crossing.png";
    model.head = "F9 or Esc leaves";
    model.guide_value = "Off";
    model.hint_label = "H shows the panel";
    if (visit == panel::Visit::shared) {
        model.time_mode = 0;
        model.freeze_caption = "Hold picture";
        model.slow_lock = "Shared game";
        model.speed_lock = "Shared game";
        model.step_lock = "Shared game";
        model.between_lock = "Shared game";
        model.chips[0] = "Live";
        model.live_amber = true;
        model.alerts = "Commander is under attack  2 chat lines";
    } else if (visit == panel::Visit::replay) {
        model.freeze_caption = "Freeze";
        model.time_mode = 0;
        model.chips[0] = "Replay paused";
        model.chips[1] = "12:34";
    } else {
        model.freeze_caption = "Freeze";
        model.time_mode = 2;
        model.chips[0] = "Photo mode";
        model.chips[1] = "Frozen";
        model.chips[2] = "12:34";
    }
    return model;
}

/// A drawn line fits its box. The block face has no accented glyph, so a line
/// it cannot measure is left to the game font. An ASCII line that measures as
/// nothing still fails.
void expect_fits(const kit::Fonts& fonts, std::string_view text, int32_t width, const char* what) {
    if (text.empty() || width <= 0)
        return;
    const int32_t measured = kit::text_width(fonts, kit::FontRole::small, text);
    if (measured <= 0 && !kit::modern_look(fonts)) {
        for (const unsigned char byte : text) {
            if (byte >= 128)
                return;
        }
    }
    if (measured <= 0 || measured > width)
        std::fprintf(stderr, "layout: %s is %d points in a box of %d\n", what, measured, width);
    expect(measured > 0 && measured <= width, what);
}

/// Every drawn label, value and caption fits the box it is painted in, and a
/// slider's words do not run into its track.
void measure_drawn(const panel::Layout& placed, const kit::Fonts& fonts, const char* what) {
    for (const kit::Item& item : placed.list.items) {
        if (item.role == kit::Role::text)
            expect_fits(fonts, item.text, item.rect.width, what);
        else if (const auto* button = std::get_if<kit::ButtonLook>(&item.look))
            expect_fits(fonts, button->caption, item.rect.width, what);
        else if (const auto* toggle = std::get_if<kit::SwitchLook>(&item.look)) {
            const int32_t half = std::max(item.rect.width / 2, int32_t{1});
            expect_fits(fonts, toggle->off_caption, half, what);
            expect_fits(fonts, toggle->on_caption, half, what);
        } else if (const auto* levels = std::get_if<kit::LevelsLook>(&item.look)) {
            const int32_t room = std::max(levels->level_width, int32_t{1});
            for (const std::string& caption : levels->captions)
                expect_fits(fonts, caption, room, what);
        }
    }
    for (const kit::Control& control : placed.list.controls) {
        if (control.kind != kit::ControlKind::slider)
            continue;
        for (const kit::Item& item : placed.list.items) {
            if (item.role != kit::Role::text || item.rect.y != control.rect.y)
                continue;
            const int32_t item_right = item.rect.x + item.rect.width;
            const int32_t track_right = control.rect.x + control.rect.width;
            const bool overlaps = item.rect.x < track_right && item_right > control.rect.x;
            expect(!overlaps, what);
        }
    }
}

void check_layout(const panel::Model& model, const kit::Fonts& fonts, const char* what);

/// The words of one language, as the catalogue gives them. Hard-coded so this
/// test does not link the language catalogue.
struct Tongue {
    const char* tag{};
    const char* between{};
    const char* step{};
    const char* run{};
    const char* slow{};
    const char* hold{};
    const char* speed{};
    const char* shared{};
    const char* accel{};
    const char* card{};
    const char* copy_to{};
    const char* off{};
    const char* on{};
    const char* selection{};
    const char* turn{};
    const char* tilt{};
};

/// The label drawn to the left of a slider.
std::string label_beside(const panel::Layout& placed, std::string_view name) {
    const kit::Control* control = named(placed.list, name);
    if (control == nullptr)
        return {};
    for (const kit::Item& item : placed.list.items) {
        if (item.role == kit::Role::text && item.rect.y == control->rect.y &&
            item.rect.x < control->rect.x)
            return item.text;
    }
    return {};
}

/// The caption a button draws.
std::string button_caption(const panel::Layout& placed, std::string_view name) {
    const kit::Control* control = named(placed.list, name);
    if (control == nullptr)
        return {};
    for (const kit::Item& item : placed.list.items) {
        if (item.control != control->id)
            continue;
        if (const auto* look = std::get_if<kit::ButtonLook>(&item.look))
            return look->caption;
    }
    return {};
}

/// German, French, Spanish and Italian at Compact, where the long words broke.
void check_tongues(const kit::Fonts& block, const kit::Fonts& modern) {
    const Tongue tongues[] = {
        {"de",
         "Zwischen den Schritten",
         "Schritt",
         "Weiter",
         "Langsam",
         "Bild halten",
         "Tempo",
         "Gemeinsames Spiel",
         "Braucht volle Beschleunigung",
         "Die Grafikkarte zeichnet nicht weiter herausgezoomt als bis hierher.",
         "In die Zwischenablage kopieren",
         "Aus",
         "An",
         "Auswahl zeigen",
         "Drehen",
         "Neigen"},
        {"fr",
         "Entre les temps",
         "Pas",
         "Reprendre",
         "Lent",
         "Garder l'image",
         "Vitesse",
         "Partie partagée",
         "Requiert l'accélération complète",
         "La carte graphique ne dessine pas plus loin que ça.",
         "Copier dans le presse-papiers",
         "Arrêt",
         "Marche",
         "Afficher la sélection",
         "Tourner",
         "Incliner"},
        {"es",
         "Entre los instantes",
         "Paso",
         "Reanudar",
         "Lento",
         "Mantener la imagen",
         "Velocidad",
         "Partida compartida",
         "Necesita aceleración completa",
         "La tarjeta gráfica no dibuja más lejos que esto.",
         "Copiar al portapapeles",
         "Apagado",
         "Encendido",
         "Mostrar la selección",
         "Girar",
         "Inclina"},
        {"it",
         "Tra un tempo e l'altro",
         "Passo",
         "Riprendi",
         "Lento",
         "Tieni l'immagine",
         "Velocità",
         "Partita condivisa",
         "Richiede l'accelerazione completa",
         "La scheda grafica non disegna più lontano di così.",
         "Copia negli appunti",
         "Spento",
         "Acceso",
         "Mostra la selezione",
         "Ruota",
         "Inclina"},
    };
    const kit::Fonts* looks[] = {&block, &modern};
    const char* look_names[] = {"block", "modern"};
    for (const Tongue& tongue : tongues) {
        for (int which = 0; which < 2; ++which) {
            panel::Model model = model_for(640, 480, kit::SizeClass::compact, panel::Visit::shared);
            model.between_label = tongue.between;
            model.step_label = tongue.step;
            model.run_label = tongue.run;
            model.slow_label = tongue.slow;
            model.freeze_caption = tongue.hold;
            model.speed_label = tongue.speed;
            model.turn_label = tongue.turn;
            model.tilt_label = tongue.tilt;
            model.slow_lock = tongue.shared;
            model.speed_lock = tongue.shared;
            model.step_lock = tongue.shared;
            model.between_lock = tongue.shared;
            model.turn_lock = tongue.accel;
            model.tilt_lock = tongue.accel;
            model.zoom_note = tongue.card;
            model.zoom_value = "1.0  Nah";
            model.copy_switch_label = tongue.copy_to;
            model.off_word = tongue.off;
            model.on_word = tongue.on;
            model.selection_label = tongue.selection;
            kit::Fonts fonts = *looks[which];
            fonts.size_class = kit::SizeClass::compact;
            char what[64];
            std::snprintf(what, sizeof what, "%s compact %s", tongue.tag, look_names[which]);
            check_layout(model, fonts, what);
            const panel::Layout placed = panel::layout_of(model, fonts);
            expect(label_beside(placed, "photo.between") == tongue.between, what);
            expect(button_caption(placed, "photo.step") == tongue.step, what);
            const kit::Control* note = named(placed.list, "photo.zoom.note");
            const kit::Control* zoom = named(placed.list, "photo.zoom");
            expect(note != nullptr && note->text == tongue.card, what);
            expect(zoom != nullptr && zoom->rect.width >= 64, what);
            std::string drawn_value;
            if (zoom != nullptr) {
                for (const kit::Item& item : placed.list.items) {
                    if (item.role == kit::Role::text && item.rect.y == zoom->rect.y &&
                        item.rect.x > zoom->rect.x)
                        drawn_value = item.text;
                }
            }
            expect(drawn_value.find(tongue.card) == std::string::npos, what);
            const kit::Control* reason = named(placed.list, "photo.turn.reason");
            expect(reason != nullptr && reason->text == tongue.accel, what);
            const kit::Control* step = named(placed.list, "photo.step");
            expect(step != nullptr && step->text.find(tongue.shared) != std::string::npos, what);
        }
    }
}

/// Every control is named, inside the panel, and in the section order.
void check_layout(const panel::Model& model, const kit::Fonts& fonts, const char* what) {
    const panel::Layout placed = panel::layout_of(model, fonts);
    const kit::Rect window{0, 0, model.width, model.height};
    if (!model.shown) {
        expect(placed.panel.width == 0, what);
        expect(kit::wholly_in(placed.hint, window), what);
    } else {
        expect(placed.panel.width == panel::panel_width(model.size_class, model.width), what);
        expect(kit::wholly_in(placed.panel, window), what);
    }
    std::vector<std::string> seen;
    for (const kit::Control& control : placed.list.controls) {
        expect(!control.name.empty(), what);
        for (const std::string& earlier : seen)
            expect(earlier != control.name, what);
        seen.push_back(control.name);
        if (control.name == "photo.saved")
            expect(kit::wholly_in(control.rect, window), what);
        else if (model.shown)
            expect(kit::wholly_in(control.rect, placed.panel), what);
        else
            expect(kit::wholly_in(control.rect, window), what);
        const bool readout = control.name == "photo.clock" || control.name == "photo.size" ||
                             control.name == "photo.file";
        if (readout) {
            expect(control.kind == kit::ControlKind::area && !control.focusable, what);
            expect(!control.enabled, what);
        }
        if (control.name == "photo.section.camera")
            expect(control.expanded == model.camera_open, what);
        if (control.name == "photo.section.time")
            expect(control.expanded == model.time_open, what);
        if (control.name == "photo.section.look")
            expect(control.expanded == model.look_open, what);
        if (control.name == "photo.section.capture")
            expect(control.expanded == model.capture_open, what);
    }
    const std::vector<std::string> tabs = tab_names(placed);
    expect(tabs == expected_tab(model), what);
    for (const char* name : {"photo.clock", "photo.size", "photo.file"}) {
        if (std::find(seen.begin(), seen.end(), name) == seen.end())
            continue;
        expect(std::find(tabs.begin(), tabs.end(), name) == tabs.end(), what);
    }
    measure_drawn(placed, fonts, what);
    if (model.shown && model.camera_open) {
        const kit::Control* zoom = named(placed.list, "photo.zoom");
        const kit::Control* turn = named(placed.list, "photo.turn");
        const kit::Control* tilt = named(placed.list, "photo.tilt");
        expect(zoom != nullptr && zoom->rect.width >= 64, what);
        expect(
            turn != nullptr && !model.turn_lock.empty() &&
                turn->text.find(model.turn_lock) != std::string::npos,
            what
        );
        expect(
            tilt != nullptr && !model.tilt_lock.empty() &&
                tilt->text.find(model.tilt_lock) != std::string::npos,
            what
        );
        const kit::Control* turn_reason = named(placed.list, "photo.turn.reason");
        expect(
            turn_reason != nullptr && turn_reason->text == model.turn_lock &&
                !turn_reason->focusable,
            what
        );
    }
    if (model.visit == panel::Visit::shared && model.shown && model.time_open) {
        const kit::Control* mode = named(placed.list, "photo.time-mode");
        const kit::Control* speed = named(placed.list, "photo.speed");
        const kit::Control* step = named(placed.list, "photo.step");
        const kit::Control* between = named(placed.list, "photo.between");
        expect(
            mode != nullptr && !model.slow_lock.empty() &&
                mode->text.find(model.slow_lock) != std::string::npos,
            what
        );
        expect(speed != nullptr && speed->text.find(model.speed_lock) != std::string::npos, what);
        expect(step != nullptr && step->text.find(model.step_lock) != std::string::npos, what);
        expect(
            between != nullptr && between->text.find(model.between_lock) != std::string::npos, what
        );
        expect(placed.alerts.width > 0, what);
    }
    if (model.visit == panel::Visit::replay && model.shown && model.time_open) {
        const kit::Control* step = named(placed.list, "photo.step");
        expect(step != nullptr && step->text.find("Shared game") == std::string::npos, what);
    }
    if (model.shown && model.time_open) {
        bool saw_mode = false;
        for (const kit::Item& item : placed.list.items) {
            const auto* levels = std::get_if<kit::LevelsLook>(&item.look);
            if (levels == nullptr)
                continue;
            saw_mode = true;
            expect(levels->chosen == static_cast<std::size_t>(model.time_mode), what);
        }
        expect(saw_mode, what);
    }
    if (model.visit == panel::Visit::skirmish)
        expect(placed.chip_box[0].width > 0 && placed.chip_box[2].width > 0, what);
    if (model.visit == panel::Visit::replay)
        expect(placed.chip_box[0].width > 0 && placed.chip_box[1].width > 0, what);
    if (model.visit == panel::Visit::shared)
        expect(placed.chip_box[0].width > 0, what);
}

} // namespace

int main() {
    auto stack =
        oa::platform::text_font::FontStack::open(oa::platform::text_font::bundled_font_directory());
    if (!stack) {
        std::fprintf(stderr, "layout: the modern fonts did not open\n");
        return 1;
    }
    kit::Fonts classic = photo_panel_test::block_fonts();
    kit::Fonts modern = classic;
    modern.modern = &*stack;

    const int32_t widths[] = {640, 1280, 1920};
    const int32_t heights[] = {480, 720, 1080};
    const kit::SizeClass classes[] = {
        kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large
    };
    const char* class_names[] = {"compact", "regular", "large"};
    const panel::Visit visits[] = {
        panel::Visit::skirmish, panel::Visit::replay, panel::Visit::shared
    };
    const char* visit_names[] = {"skirmish", "replay", "shared"};
    const kit::Fonts* looks[] = {&classic, &modern};
    const char* look_names[] = {"classic", "modern"};

    for (int size = 0; size < 3; ++size) {
        for (int which_class = 0; which_class < 3; ++which_class) {
            for (int which_visit = 0; which_visit < 3; ++which_visit) {
                for (int which_look = 0; which_look < 2; ++which_look) {
                    panel::Model model = model_for(
                        widths[size], heights[size], classes[which_class], visits[which_visit]
                    );
                    kit::Fonts fonts = *looks[which_look];
                    fonts.size_class = classes[which_class];
                    char what[160];
                    std::snprintf(
                        what,
                        sizeof what,
                        "%s %dx%d %s %s",
                        visit_names[which_visit],
                        widths[size],
                        heights[size],
                        class_names[which_class],
                        look_names[which_look]
                    );
                    check_layout(model, fonts, what);
                }
            }
        }
    }

    panel::Model folded = model_for(1280, 720, kit::SizeClass::regular, panel::Visit::skirmish);
    folded.camera_open = false;
    folded.look_open = false;
    check_layout(folded, classic, "folded camera and look");
    expect(named(panel::layout_of(folded, classic).list, "photo.zoom") == nullptr, "folded zoom");
    expect(
        named(panel::layout_of(folded, classic).list, "photo.section.camera") != nullptr,
        "folded camera header"
    );

    panel::Model hidden = model_for(1280, 720, kit::SizeClass::regular, panel::Visit::skirmish);
    hidden.shown = false;
    check_layout(hidden, classic, "hidden");
    expect(named(panel::layout_of(hidden, classic).list, "photo.take") == nullptr, "hidden take");
    expect(named(panel::layout_of(hidden, classic).list, "photo.hint") != nullptr, "hidden hint");

    // The toast starts at the margin, so its right edge is past a band of its
    // width measured from the window's left. The capture check skips that edge.
    panel::Model toasting = model_for(640, 480, kit::SizeClass::compact, panel::Visit::skirmish);
    toasting.toast = true;
    const panel::Layout toast_layout = panel::layout_of(toasting, classic);
    const int toast_right = toast_layout.toast.x + toast_layout.toast.width - 1;
    const int toast_top = toast_layout.toast.y;
    expect(toast_layout.toast.width > 0, "toast place");
    expect(panel::covers_chrome(toast_right, toast_top, 640, 480), "toast right edge is chrome");
    expect(panel::covers_chrome(304, 364, 640, 480), "toast corner is chrome");
    expect(
        !panel::covers_chrome(toast_layout.toast.x + toast_layout.toast.width, toast_top, 640, 480),
        "past the toast is the battlefield"
    );

    const std::vector<float> stops = panel::zoom_stops(0.083F, 8.F);
    expect(!stops.empty() && stops.front() == 0.083F, "the far zoom is a stop");
    expect(!stops.empty() && stops.back() == 8.F, "the near zoom is a stop");
    bool one = false;
    bool below_one = false;
    float previous = 0.F;
    for (const float stop : stops) {
        expect(stop + 0.0001F >= previous, "zoom stops ascend");
        if (stop == 1.F)
            one = true;
        if (stop > 0.083F && stop < 1.F)
            below_one = true;
        previous = stop;
    }
    expect(one, "1.0 is a zoom stop");
    expect(below_one, "a stop lies between the whole map and 1");
    expect(panel::zoom_scale_key(0.2F) == "Whole map", "0.2 is Whole map");
    expect(panel::zoom_scale_key(0.5F) == "Far", "0.5 is Far");
    expect(panel::zoom_scale_key(1.F) == "Medium", "1.0 is Medium");
    expect(panel::zoom_scale_key(1.7F) == "Medium", "1.7 is Medium");
    expect(panel::zoom_scale_key(5.F) == "Near", "5.0 is Near");
    expect(panel::zoom_scale_key(8.F) == "Closest", "8 is Closest");
    const int32_t at_one = panel::zoom_stop_nearest(stops, 1.F);
    expect(
        at_one >= 0 && static_cast<std::size_t>(at_one) < stops.size() && stops[at_one] == 1.F,
        "the stop nearest 1.0 is 1.0"
    );
    const std::vector<float> classic_stops = panel::zoom_stops(2.F, 8.F);
    expect(
        !classic_stops.empty() && classic_stops.front() == 2.F && classic_stops.back() == 8.F,
        "a tier keeps its own limits"
    );
    for (const float stop : classic_stops)
        expect(stop + 0.0001F >= 2.F, "no stop lies past the graphics card");

    check_tongues(classic, modern);

    // With the values the zoom can show reserved, the track keeps its width.
    // The tenths' digits may differ by a point; the names by tens of points.
    for (const kit::Fonts* look : looks) {
        int32_t first_width = -1;
        for (const char* value : {"0.1  Whole map", "1.0  Medium", "8.0  Closest", "0.3  Far"}) {
            panel::Model steady =
                model_for(640, 480, kit::SizeClass::compact, panel::Visit::skirmish);
            steady.zoom_value = value;
            steady.zoom_reserve = {
                "8.0  Whole map", "8.0  Far", "8.0  Medium", "8.0  Near", "8.0  Closest"
            };
            kit::Fonts fonts = *look;
            fonts.size_class = kit::SizeClass::compact;
            check_layout(steady, fonts, "steady zoom");
            const panel::Layout steady_layout = panel::layout_of(steady, fonts);
            const kit::Control* zoom = named(steady_layout.list, "photo.zoom");
            expect(zoom != nullptr, "steady zoom slider");
            if (zoom == nullptr)
                continue;
            if (first_width < 0)
                first_width = zoom->rect.width;
            expect(std::abs(zoom->rect.width - first_width) <= 2, "the zoom track keeps its width");
        }
    }

    return oa::test::check_exit_status();
}
