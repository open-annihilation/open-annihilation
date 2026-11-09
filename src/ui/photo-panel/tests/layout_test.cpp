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
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/photo_panel/layout.hpp"
#include "block_font.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
        const char* look[] = {
            "photo.look",
            "photo.vignette",
            "photo.roundness",
            "photo.grain",
            "photo.grain-size",
            "photo.frame",
            "photo.selection",
            "photo.health",
            "photo.guides",
        };
        names.insert(names.end(), std::begin(look), std::end(look));
    }
    names.push_back("photo.section.capture");
    if (model.capture_open) {
        const char* capture[] = {
            "photo.size",
            "photo.smooth",
            "photo.copy-to-clipboard",
            "photo.player-names",
            "photo.take",
            "photo.copy"
        };
        names.insert(names.end(), std::begin(capture), std::end(capture));
    }
    return names;
}

/// Captions the way fill_size_choices builds them, for a 640×480 battlefield.
/// Classic writes "x", so both looks measure the same words.
void apply_size_copy(panel::Model& model, bool shared) {
    std::array<panel::ChoiceStop, 4> sizes{};
    std::array<panel::ChoiceStop, 3> smooth{};
    const int factors[] = {1, 2, 3, 4};
    const int32_t widths[] = {640, 1280, 1920, 2560};
    const int32_t heights[] = {480, 960, 1440, 1920};
    for (int index = 0; index < 4; ++index) {
        sizes[static_cast<std::size_t>(index)].factor = factors[index];
        sizes[static_cast<std::size_t>(index)].width = widths[index];
        sizes[static_cast<std::size_t>(index)].height = heights[index];
        if (shared && index > 0)
            sizes[static_cast<std::size_t>(index)].lock = "Shared game";
    }
    const int smooth_factors[] = {1, 2, 4};
    for (int index = 0; index < 3; ++index) {
        smooth[static_cast<std::size_t>(index)].factor = smooth_factors[index];
        if (shared && index == 2)
            smooth[static_cast<std::size_t>(index)].lock = "Shared game";
    }
    panel::ChoiceWords words;
    words.times = "x";
    const panel::ChoiceCopy copy = panel::choice_copy(sizes, smooth, 0, 0, words);
    model.size_captions = copy.size_captions;
    model.smooth_captions = copy.smooth_captions;
    model.size_label = copy.size_label;
    model.smooth_label = copy.smooth_label;
    model.size_value = copy.size_value;
    model.size_reason = copy.reason;
    for (int index = 0; index < 4; ++index)
        model.size_locks[static_cast<std::size_t>(index)] =
            sizes[static_cast<std::size_t>(index)].lock;
    for (int index = 0; index < 3; ++index)
        model.smooth_locks[static_cast<std::size_t>(index)] =
            smooth[static_cast<std::size_t>(index)].lock;
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
    const bool shared = visit == panel::Visit::shared;
    apply_size_copy(model, shared);
    if (shared) {
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
    const char* arctic{};
    const char* bleach{};
    const char* caption{};
    const char* dusk{};
    const char* frame{};
    const char* grain{};
    const char* noir{};
    const char* roundness{};
    const char* rate{};
    const char* thin{};
    const char* vignette{};
    const char* wide{};
    const char* none{};
    const char* photo_size{};
    const char* grain_size{};
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
         "Neigen",
         "Arktis",
         "Bleich",
         "Textleiste",
         "Abendlicht",
         "Rahmen",
         "Korn",
         "Schwarzweiß",
         "Rundung",
         "Die Vorschau senkt die Bildrate.",
         "Dünner Rand",
         "Vignette",
         "Breiter Rand",
         "Keine",
         "Größe",
         "Korngröße"},
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
         "Incliner",
         "Arctique",
         "Délavé",
         "Bandeau",
         "Crépuscule",
         "Cadre",
         "Grain",
         "Noir",
         "Rondeur",
         "L'aperçu baisse le nombre d'images.",
         "Bord fin",
         "Vignettage",
         "Large marge",
         "Aucun",
         "Taille",
         "Taille du grain"},
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
         "Inclina",
         "Ártico",
         "Lavado",
         "Franja",
         "Anochecer",
         "Marco",
         "Grano",
         "Noir",
         "Redondez",
         "La vista previa baja los fotogramas.",
         "Borde fino",
         "Viñeta",
         "Marco ancho",
         "Ninguno",
         "Tamaño",
         "Tamaño del grano"},
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
         "Inclina",
         "Artico",
         "Sbiadito",
         "Didascalia",
         "Tramonto",
         "Cornice",
         "Grana",
         "Noir",
         "Rotondità",
         "L'anteprima abbassa i fotogrammi.",
         "Bordo sottile",
         "Vignettatura",
         "Bordo largo",
         "Nessuno",
         "Dimensione",
         "Dimensione della grana"},
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
            model.preset_labels = {
                tongue.none, "1997", tongue.dusk, tongue.arctic, tongue.noir, tongue.bleach
            };
            model.grain_size_label = tongue.grain_size;
            model.size_label = tongue.photo_size;
            model.vignette_label = tongue.vignette;
            model.roundness_label = tongue.roundness;
            model.grain_label = tongue.grain;
            model.frame_label = tongue.frame;
            model.frame_value = tongue.wide;
            model.preview_note = tongue.rate;
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
            const kit::Control* rate = named(placed.list, "photo.look.rate");
            expect(rate != nullptr && !rate->focusable && rate->text == tongue.rate, what);
            const kit::Control* vignette = named(placed.list, "photo.vignette");
            expect(vignette != nullptr && vignette->rect.width >= 64, what);
            expect(label_beside(placed, "photo.vignette") == tongue.vignette, what);
            expect(label_beside(placed, "photo.grain") == tongue.grain, what);
            const kit::Control* grain_size = named(placed.list, "photo.grain-size");
            expect(grain_size != nullptr && grain_size->label == tongue.grain_size, what);
            expect(grain_size != nullptr && grain_size->label != tongue.photo_size, what);
            const kit::Control* photo_size = named(placed.list, "photo.size");
            expect(photo_size != nullptr && photo_size->label == tongue.photo_size, what);
            expect(label_beside(placed, "photo.grain-size") == tongue.grain_size, what);
        }
        // The chips and the grain's size keep their whole names at the larger
        // sizes too, where the faces are larger.
        const int32_t sized_widths[] = {1280, 1920};
        const int32_t sized_heights[] = {720, 1080};
        const kit::SizeClass sized_classes[] = {kit::SizeClass::regular, kit::SizeClass::large};
        for (int sized = 0; sized < 2; ++sized) {
            for (int sized_look = 0; sized_look < 2; ++sized_look) {
                panel::Model larger = model_for(
                    sized_widths[sized],
                    sized_heights[sized],
                    sized_classes[sized],
                    panel::Visit::skirmish
                );
                larger.preset_labels = {
                    tongue.none, "1997", tongue.dusk, tongue.arctic, tongue.noir, tongue.bleach
                };
                larger.grain_size_label = tongue.grain_size;
                larger.size_label = tongue.photo_size;
                larger.vignette_label = tongue.vignette;
                larger.roundness_label = tongue.roundness;
                larger.grain_label = tongue.grain;
                larger.frame_label = tongue.frame;
                larger.frame_value = tongue.caption;
                kit::Fonts larger_fonts = *looks[sized_look];
                larger_fonts.size_class = sized_classes[sized];
                char sized_what[64];
                std::snprintf(
                    sized_what,
                    sizeof sized_what,
                    "%s %d %s",
                    tongue.tag,
                    static_cast<int>(sized_widths[sized]),
                    look_names[sized_look]
                );
                check_layout(larger, larger_fonts, sized_what);
                const panel::Layout larger_placed = panel::layout_of(larger, larger_fonts);
                expect(
                    label_beside(larger_placed, "photo.grain-size") == tongue.grain_size, sized_what
                );
            }
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
        const bool readout = control.name == "photo.clock" || control.name == "photo.file" ||
                             control.name == "photo.size.reason";
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
    for (const char* name : {"photo.clock", "photo.file", "photo.size.reason"}) {
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
            if (item.control != panel::kRun)
                continue;
            const auto* levels = std::get_if<kit::LevelsLook>(&item.look);
            if (levels == nullptr)
                continue;
            saw_mode = true;
            expect(levels->chosen == static_cast<std::size_t>(model.time_mode), what);
            expect(levels->offered == 3, what);
        }
        expect(saw_mode, what);
    }
    if (model.shown && model.look_open) {
        bool saw_preset = false;
        for (const kit::Item& item : placed.list.items) {
            if (item.control != panel::kPreset)
                continue;
            const auto* levels = std::get_if<kit::LevelsLook>(&item.look);
            if (levels == nullptr)
                continue;
            saw_preset = true;
            expect(levels->chosen == static_cast<std::size_t>(model.preset), what);
            expect(levels->offered == 6, what);
            expect(levels->captions.size() == model.preset_labels.size(), what);
            for (std::size_t index = 0; index < levels->captions.size(); ++index) {
                expect(levels->captions[index] == model.preset_labels[index], what);
                expect(levels->captions[index].find("\xE2\x80\xA6") == std::string::npos, what);
            }
        }
        const kit::Control* preset = named(placed.list, "photo.look");
        if (preset != nullptr && model.preset >= 0 &&
            static_cast<std::size_t>(model.preset) < model.preset_labels.size()) {
            expect(
                preset->text == model.preset_labels[static_cast<std::size_t>(model.preset)], what
            );
        }
        expect(saw_preset, what);
        expect(named(placed.list, "photo.vignette") != nullptr, what);
        expect(named(placed.list, "photo.frame") != nullptr, what);
        const kit::Control* rate = named(placed.list, "photo.look.rate");
        if (model.preview_note.empty())
            expect(rate == nullptr, what);
        else
            expect(rate != nullptr && !rate->focusable && rate->text == model.preview_note, what);
    }
    if (model.shown && model.capture_open) {
        const kit::Control* size = named(placed.list, "photo.size");
        const kit::Control* smooth = named(placed.list, "photo.smooth");
        expect(size != nullptr && size->focusable && size->kind == kit::ControlKind::levels, what);
        expect(
            smooth != nullptr && smooth->focusable && smooth->kind == kit::ControlKind::levels, what
        );
        expect(model.size_value == "Screen 640x480", what);
        expect(model.size_captions[1] == "2x", what);
        if (model.visit == panel::Visit::shared) {
            expect(model.size_reason == "2x, 3x, 4x and Smooth edges 4x: Shared game", what);
        }
        bool saw_size_label = false;
        bool saw_smooth_label = false;
        for (const kit::Item& item : placed.list.items) {
            if (item.role != kit::Role::text)
                continue;
            if (item.text == model.size_label)
                saw_size_label = true;
            if (item.text == model.smooth_label)
                saw_smooth_label = true;
        }
        expect(saw_size_label && saw_smooth_label, what);
        if (!model.size_reason.empty()) {
            const kit::Control* reason = named(placed.list, "photo.size.reason");
            expect(
                reason != nullptr && !reason->focusable && reason->text == model.size_reason, what
            );
            // The reason is drawn whole, over the rows it wraps to.
            std::string drawn;
            for (const kit::Item& item : placed.list.items) {
                if (reason == nullptr || item.role != kit::Role::text ||
                    !kit::wholly_in(item.rect, reason->rect))
                    continue;
                if (!drawn.empty())
                    drawn += ' ';
                drawn += item.text;
            }
            expect(drawn == model.size_reason, what);
        }
    }
    if (model.visit == panel::Visit::skirmish)
        expect(placed.chip_box[0].width > 0 && placed.chip_box[2].width > 0, what);
    if (model.visit == panel::Visit::replay)
        expect(placed.chip_box[0].width > 0 && placed.chip_box[1].width > 0, what);
    if (model.visit == panel::Visit::shared)
        expect(placed.chip_box[0].width > 0, what);
}

/// Tells whether two rectangles share any pixel.
bool overlaps(const kit::Rect& a, const kit::Rect& b) {
    return a.width > 0 && a.height > 0 && b.width > 0 && b.height > 0 && a.x < b.x + b.width &&
           b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

/// A control a finger presses. A readout is an area, and it is not.
bool finger_presses(const kit::Control& control) {
    return control.kind == kit::ControlKind::button || control.kind == kit::ControlKind::toggle ||
           control.kind == kit::ControlKind::slider || control.kind == kit::ControlKind::levels;
}

/// The part of a control the view shows. An empty clip is the whole rectangle.
kit::Rect visible_part(const kit::Control& control) {
    if (control.clip.width > 0 && control.clip.height > 0)
        return kit::intersect(control.rect, control.clip);
    return control.rect;
}

/// Tells whether each level of a strip is at least `least` wide and tall, on
/// one row or wrapped onto several.
bool strip_levels_fit(const panel::Layout& placed, const kit::Control& control, int32_t least) {
    for (const kit::Item& item : placed.list.items) {
        if (item.control != control.id || item.role != kit::Role::levels)
            continue;
        const auto* levels = std::get_if<kit::LevelsLook>(&item.look);
        if (levels == nullptr)
            return false;
        const auto count = static_cast<int32_t>(levels->captions.size());
        const int32_t columns =
            levels->columns > 0 && levels->columns < count ? levels->columns : count;
        const int32_t rows = (count + columns - 1) / std::max(columns, int32_t{1});
        const int32_t gaps = rows > 1 ? (rows - 1) * levels->row_gap : 0;
        const int32_t band = (control.rect.height - gaps) / std::max(rows, int32_t{1});
        return levels->level_width >= least && band >= least;
    }
    return false;
}

/// Tells whether a slider's knob is drawn at its row's middle, within a point.
bool knob_centred(const panel::Layout& placed, const kit::Control& control) {
    for (const kit::Item& item : placed.list.items) {
        if (item.control != control.id || item.role != kit::Role::slider)
            continue;
        const int32_t knob = item.rect.y + kit::compact_metrics.knob_height / 2;
        const int32_t middle = control.rect.y + control.rect.height / 2;
        return std::abs(knob - middle) <= 1;
    }
    return false;
}

/// Tells whether the scrolling view shows some row. A keyboard layout has none.
bool view_shows_a_row(const panel::Layout& placed) {
    if (placed.rows.view.height <= 0 || placed.rows.content_height <= 0)
        return true;
    for (const kit::Control& control : placed.list.controls) {
        if (control.group < 0)
            continue;
        const kit::Rect part = visible_part(control);
        if (part.width > 0 && part.height > 0)
            return true;
    }
    return false;
}

/// Checks one touch layout: every pressable control is a finger's row. The
/// part the view shows lies inside the window and clear of the chips and the
/// key line. A row scrolled out of the view is still listed.
void check_touch_frame(
    const panel::Model& model, const kit::Fonts& fonts, std::vector<std::string>& saw
) {
    const panel::Layout placed = panel::layout_of(model, fonts);
    const kit::Rect window{0, 0, model.width, model.height};
    const int32_t row = std::max(model.touch_row, int32_t{44});
    expect(kit::wholly_in(placed.panel, window), "touch panel in the window");
    expect(placed.panel.y >= 72, "touch panel starts under the chips");
    expect(view_shows_a_row(placed), "the scrolling view shows a row");
    const kit::Control* leave = named(placed.list, "photo.leave");
    const kit::Control* take = named(placed.list, "photo.take");
    expect(leave != nullptr && take != nullptr, "leave and take photo stay put");
    for (const kit::Control& control : placed.list.controls) {
        const bool pinned = control.name == "photo.leave" || control.name == "photo.take" ||
                            control.name == "photo.hint" || control.name == "photo.saved";
        if (!pinned && placed.rows.view.height > 0) {
            expect(control.group == 0, "a scrolling row is in the rows' area");
            expect(
                control.clip.x == placed.rows.view.x && control.clip.y == placed.rows.view.y &&
                    control.clip.width == placed.rows.view.width &&
                    control.clip.height == placed.rows.view.height,
                "a scrolling row is clipped to the view"
            );
        }
        if (!finger_presses(control))
            continue;
        saw.push_back(control.name);
        expect(control.rect.height >= row, "a finger's control is at least 44 points");
        const kit::Rect shown = visible_part(control);
        if (shown.width <= 0 || shown.height <= 0)
            continue;
        expect(kit::wholly_in(shown, placed.panel), "a finger's control lies in the panel");
        expect(kit::wholly_in(shown, window), "a finger's control lies in the window");
        expect(!overlaps(shown, placed.chips), "chips do not cover a control");
        expect(!overlaps(shown, placed.keyline), "the key line does not cover a control");
        for (const kit::Rect& chip : placed.chip_box)
            expect(!overlaps(shown, chip), "a chip does not cover a control");
        if (control.name == "photo.zoom")
            expect(control.rect.width >= 64, "the zoom track stays wide enough");
        if (control.kind == kit::ControlKind::levels)
            expect(
                strip_levels_fit(placed, control, row), "each level of a strip is a finger's target"
            );
        if (control.kind == kit::ControlKind::slider)
            expect(knob_centred(placed, control), "a finger's slider sits at its row's middle");
    }
}

/// The caption bar on a touch layout. Raised over the bar, the key line ends
/// before the panel; the notice sits above the bar and the key line, under the
/// chips, with a key line or without one.
void check_touch_caption(const kit::Fonts& fonts) {
    const int32_t windows[][2] = {{852, 393}, {667, 375}, {1180, 820}};
    for (const int32_t* window : windows) {
        for (const int32_t row : {44, 66}) {
            for (int with_keys = 0; with_keys < 2; ++with_keys) {
                panel::Model model = model_for(
                    window[0],
                    window[1],
                    kit::size_class_of(window[0], window[1]),
                    panel::Visit::skirmish
                );
                model.touch = true;
                model.touch_row = row;
                model.caption_clear = 30;
                model.toast = true;
                model.key_line =
                    with_keys ? "A Take photo · B Leave · X Freeze · Y Hide panel" : "";
                const panel::Layout placed = panel::layout_of(model, fonts);
                const int32_t bar = window[1] - model.caption_clear;
                expect(placed.toast.width > 0, "a touch notice shows");
                expect(placed.toast.y >= 72, "a touch notice stays under the chips");
                expect(
                    placed.toast.y + placed.toast.height <= bar, "a touch notice clears the bar"
                );
                if (with_keys) {
                    expect(
                        placed.keyline.y + placed.keyline.height <= bar,
                        "the touch key line clears the bar"
                    );
                    expect(
                        placed.keyline.x + placed.keyline.width <= placed.panel.x,
                        "the raised touch key line ends before the panel"
                    );
                    expect(
                        placed.toast.y + placed.toast.height <= placed.keyline.y,
                        "a touch notice clears the key line"
                    );
                }
                std::vector<std::string> saw;
                check_touch_frame(model, fonts, saw);
            }
        }
    }
}

/// The phone's panel at 852x393 and 667x375, at Standard, Large and Larger.
void check_touch_layout(const kit::Fonts& fonts) {
    const int32_t windows[][2] = {{852, 393}, {667, 375}};
    const int32_t rows[] = {44, 55, 66};
    std::vector<std::string> saw;
    for (const int32_t* window : windows) {
        const int32_t width = window[0];
        const int32_t height = window[1];
        for (const int32_t row : rows) {
            for (int section = 0; section < 4; ++section) {
                for (int with_keys = 0; with_keys < 2; ++with_keys) {
                    panel::Model model = model_for(
                        width, height, kit::size_class_of(width, height), panel::Visit::skirmish
                    );
                    model.touch = true;
                    model.touch_row = row;
                    model.camera_open = section == 0;
                    model.time_open = section == 1;
                    model.look_open = section == 2;
                    model.capture_open = section == 3;
                    model.key_line =
                        with_keys ? "A Take photo · B Leave · X Freeze · Y Hide panel" : "";
                    model.scroll = 1000000;
                    const int32_t max_scroll = panel::layout_of(model, fonts).scroll;
                    for (int32_t scroll = 0; scroll <= max_scroll; scroll += row) {
                        model.scroll = scroll;
                        check_touch_frame(model, fonts, saw);
                    }
                    if (max_scroll % row != 0) {
                        model.scroll = max_scroll;
                        check_touch_frame(model, fonts, saw);
                    }
                }
            }
            panel::Model hidden =
                model_for(width, height, kit::size_class_of(width, height), panel::Visit::skirmish);
            hidden.touch = true;
            hidden.touch_row = row;
            hidden.shown = false;
            hidden.hint_label = "Show panel";
            const panel::Layout hint = panel::layout_of(hidden, fonts);
            const kit::Control* show = named(hint.list, "photo.hint");
            expect(show != nullptr && show->rect.height >= row, "the hint is a finger's control");
            expect(
                kit::wholly_in(hint.hint, kit::Rect{0, 0, width, height}),
                "the hint is in the window"
            );
            if (show != nullptr)
                saw.emplace_back(show->name);
        }
    }
    for (const char* name :
         {"photo.leave",
          "photo.hint",
          "photo.section.camera",
          "photo.section.time",
          "photo.section.look",
          "photo.section.capture",
          "photo.zoom",
          "photo.time-mode",
          "photo.look",
          "photo.vignette",
          "photo.roundness",
          "photo.grain",
          "photo.grain-size",
          "photo.frame",
          "photo.selection",
          "photo.size",
          "photo.smooth",
          "photo.take",
          "photo.copy"}) {
        const bool found = std::find(saw.begin(), saw.end(), name) != saw.end();
        if (!found)
            std::fprintf(stderr, "layout: touch never showed %s\n", name);
        expect(found, "touch shows every pressable control");
    }

    // Every offset the rows can settle at shows a row, including half a row:
    // with Camera open, and with Photo open under the longest size reason,
    // which wraps over several rows of the scroll area.
    const int32_t samples[][3] = {{852, 393, 44}, {852, 393, 66}, {667, 375, 66}, {1180, 820, 66}};
    for (const int32_t* sample : samples) {
        for (const bool capture : {false, true}) {
            panel::Model model = model_for(
                sample[0],
                sample[1],
                kit::size_class_of(sample[0], sample[1]),
                panel::Visit::skirmish
            );
            model.touch = true;
            model.touch_row = sample[2];
            model.camera_open = !capture;
            model.time_open = false;
            model.look_open = false;
            model.capture_open = capture;
            if (capture) {
                model.size_reason = "2x, 3x, 4x, Smooth edges 2x and Smooth edges 4x: The 3D view "
                                    "draws at most 2730 pixels a side at this anti-aliasing";
                for (std::size_t stop = 1; stop < 4; ++stop)
                    model.size_locks[stop] = "The 3D view draws at most 2730 pixels a side";
            }
            model.key_line = "A Take photo · B Leave · X Freeze · Y Hide panel";
            model.scroll = 1000000;
            const int32_t limit = panel::layout_of(model, fonts).scroll;
            for (int32_t scroll = 0; scroll <= limit; ++scroll) {
                model.scroll = scroll;
                const panel::Layout placed = panel::layout_of(model, fonts);
                if (!view_shows_a_row(placed))
                    std::fprintf(stderr, "layout: empty view at scroll %d\n", scroll);
                expect(view_shows_a_row(placed), "no scroll position shows an empty view");
                if (scroll != sample[2] / 2 || limit < sample[2] / 2)
                    continue;
                const kit::Point under{
                    placed.rows.view.x + placed.rows.view.width / 2, placed.rows.view.y + 2
                };
                const kit::Control* hit =
                    kit::control_of(placed.list, kit::hit(placed.list, under));
                expect(
                    hit != nullptr && hit->name != "photo.leave" && hit->name != "photo.take",
                    "a point on a cut row is that row"
                );
            }
            if (capture) {
                std::vector<std::string> shown;
                model.scroll = 0;
                check_touch_frame(model, fonts, shown);
                const panel::Layout reasoned = panel::layout_of(model, fonts);
                const kit::Control* reason = named(reasoned.list, "photo.size.reason");
                expect(
                    reason != nullptr && reason->text == model.size_reason && !reason->focusable,
                    "the wrapped size reason is one quiet control on touch"
                );
            }
        }
    }

    panel::Model mouse = model_for(852, 393, kit::size_class_of(852, 393), panel::Visit::skirmish);
    const panel::Layout mouse_placed = panel::layout_of(mouse, fonts);
    const kit::Control* short_row = named(mouse_placed.list, "photo.leave");
    expect(short_row != nullptr && short_row->rect.height < 44, "a mouse row stays under 44");
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

    // The longest reasons the game writes: a shared match in Off, and the
    // 3D view's limit on every larger size. They wrap, and are drawn whole.
    for (int size = 0; size < 3; ++size) {
        for (int which_look = 0; which_look < 2; ++which_look) {
            for (const char* reason :
                 {"2x, 3x and 4x: Shared game. Smooth edges 2x and 4x: Off and Basic draw "
                  "whole pixels",
                  "2x, 3x, 4x, Smooth edges 2x and Smooth edges 4x: The 3D view draws at most "
                  "2730 pixels a side at this anti-aliasing"}) {
                panel::Model wrapped =
                    model_for(widths[size], heights[size], classes[size], panel::Visit::skirmish);
                wrapped.size_reason = reason;
                kit::Fonts fonts = *looks[which_look];
                fonts.size_class = classes[size];
                char what[160];
                std::snprintf(
                    what,
                    sizeof what,
                    "long reason %dx%d %s",
                    widths[size],
                    heights[size],
                    look_names[which_look]
                );
                check_layout(wrapped, fonts, what);
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

    // A caption bar owns the bottom of the picture. The key line sits above
    // it, and the panel stops above it, shown or hidden.
    const auto clears_caption = [](panel::Model model, const kit::Fonts& fonts, const char* what) {
        const panel::Layout placed = panel::layout_of(model, fonts);
        const int32_t limit = model.height - model.caption_clear;
        expect(placed.keyline.y + placed.keyline.height <= limit, what);
        if (model.shown)
            expect(placed.panel.y + placed.panel.height <= limit, what);
        else
            expect(placed.panel.width == 0, what);
    };
    panel::Model captioned = model_for(1280, 720, kit::SizeClass::regular, panel::Visit::skirmish);
    captioned.caption_clear = 46;
    clears_caption(captioned, classic, "key line clears the caption");
    captioned.shown = false;
    clears_caption(captioned, classic, "hidden key line clears the caption");
    panel::Model compact_caption =
        model_for(640, 480, kit::SizeClass::compact, panel::Visit::shared);
    compact_caption.caption_clear = 30;
    compact_caption.preview_note = "The preview lowers the frame rate.";
    clears_caption(compact_caption, classic, "compact key line clears the caption");
    compact_caption.shown = false;
    clears_caption(compact_caption, classic, "compact hidden key line clears the caption");
    panel::Model tall_caption =
        model_for(1920, 1080, kit::SizeClass::large, panel::Visit::skirmish);
    tall_caption.caption_clear = 69;
    clears_caption(tall_caption, classic, "large key line clears the caption");
    tall_caption.shown = false;
    clears_caption(tall_caption, classic, "large hidden key line clears the caption");

    // Under a crop the bar lies at the crop's bottom. A 3:2 crop at 640x480
    // leaves 27 rows under it, too few for the key line, which then sits
    // above the 27-row bar, shown or hidden. Every control stays inside the
    // panel in both looks and with German names, the panel growing over the
    // bar's right end when its rows do not fit above the bar.
    kit::Fonts compact_classic = classic;
    compact_classic.size_class = kit::SizeClass::compact;
    kit::Fonts compact_modern = modern;
    compact_modern.size_class = kit::SizeClass::compact;
    const std::array<std::array<const char*, 6>, 2> grade_names{{
        {"None", "1997", "Dusk", "Arctic", "Noir", "Bleach"},
        {"Keine", "1997", "Abendlicht", "Arktis", "Schwarzwei\xC3\x9F", "Bleich"},
    }};
    for (const std::array<const char*, 6>& names : grade_names) {
        for (const kit::Fonts* look : {&compact_classic, &compact_modern}) {
            panel::Model three_two =
                model_for(640, 480, kit::SizeClass::compact, panel::Visit::shared);
            three_two.guide = panel::Guide::crop_3_2;
            three_two.caption_clear = 27 + 27;
            three_two.preview_note = "The preview lowers the frame rate.";
            three_two.preset_labels = {names[0], names[1], names[2], names[3], names[4], names[5]};
            check_layout(three_two, *look, "3:2 caption bar");
            const panel::Layout open = panel::layout_of(three_two, *look);
            expect(open.keyline.y + open.keyline.height <= 480 - 54, "3:2 key line clears the bar");
            three_two.shown = false;
            clears_caption(three_two, *look, "3:2 hidden key line clears the bar");
        }
    }
    // A 16:9 crop at 640x480 leaves 60 rows under it, and the key line stays
    // there, under the crop and out of the photo.
    panel::Model sixteen_nine =
        model_for(640, 480, kit::SizeClass::compact, panel::Visit::skirmish);
    sixteen_nine.guide = panel::Guide::crop_16_9;
    sixteen_nine.caption_clear = 60 + 23;
    for (const bool shown : {true, false}) {
        sixteen_nine.shown = shown;
        const panel::Layout under = panel::layout_of(sixteen_nine, compact_classic);
        expect(under.keyline.y >= 60 + 360, "16:9 key line stays under the crop");
        expect(under.keyline.y + under.keyline.height <= 480, "16:9 key line in the window");
    }
    // With a caption bar the toast sits above both the key line and the bar.
    const std::array<std::pair<panel::Guide, int32_t>, 3> toast_cases{{
        {panel::Guide::off, 30},
        {panel::Guide::crop_3_2, 54},
        {panel::Guide::crop_16_9, 83},
    }};
    for (const std::pair<panel::Guide, int32_t>& toast_case : toast_cases) {
        panel::Model toasted = model_for(640, 480, kit::SizeClass::compact, panel::Visit::skirmish);
        toasted.guide = toast_case.first;
        toasted.caption_clear = toast_case.second;
        toasted.toast = true;
        const panel::Layout over = panel::layout_of(toasted, compact_classic);
        expect(over.toast.width > 0, "captioned toast");
        expect(over.toast.y + over.toast.height <= over.keyline.y, "toast over the key line");
        expect(over.toast.y + over.toast.height <= 480 - toast_case.second, "toast over the bar");
    }
    panel::Model toasted_large =
        model_for(1920, 1080, kit::SizeClass::large, panel::Visit::skirmish);
    toasted_large.caption_clear = 69;
    toasted_large.toast = true;
    const panel::Layout large_toast = panel::layout_of(toasted_large, classic);
    expect(
        large_toast.toast.y + large_toast.toast.height <= large_toast.keyline.y,
        "large toast over the key line"
    );
    expect(large_toast.toast.y + large_toast.toast.height <= 1080 - 69, "large toast over the bar");
    // Without a caption bar the toast keeps its place.
    panel::Model plain_toast = model_for(640, 480, kit::SizeClass::compact, panel::Visit::skirmish);
    plain_toast.toast = true;
    expect(panel::layout_of(plain_toast, classic).toast.y == 480 - 116, "toast place unchanged");

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

    check_touch_layout(classic);
    check_touch_caption(classic);

    return oa::test::check_exit_status();
}
