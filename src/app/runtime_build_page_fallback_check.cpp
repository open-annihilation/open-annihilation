// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A builder whose second build page has no GUI file of its own. 3.1c draws
// such a page on the side's download page, with the download entries that
// name it; with that page unreadable too, the page is passed over, the match
// plays on and the mod's data fault is logged once, where an older build put
// up an alert.
#include "oa/app/runtime.hpp"

#include "oa/core/world.h"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/ui/hud/order_panel.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace oa::app {
namespace {

/// Throws the check's failure, naming what went wrong.
///
/// @param what the failure, without the check's name
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("build page fallback check: " + what);
}

/// Removes the files the check wrote, and the folders it made once they are
/// empty, however the check ends.
class WrittenFiles {
  public:

    WrittenFiles() = default;
    WrittenFiles(const WrittenFiles&) = delete;
    WrittenFiles& operator=(const WrittenFiles&) = delete;

    ~WrittenFiles() {
        std::error_code ignored;
        for (const auto& file : files_)
            fs::remove(file, ignored);
        for (auto folder = folders_.rbegin(); folder != folders_.rend(); ++folder)
            if (fs::is_empty(*folder, ignored))
                fs::remove(*folder, ignored);
    }

    /// Writes a file, remembering it and its folder when the check makes it.
    ///
    /// @param path the file, under the mod folder
    /// @param text the file's bytes; empty writes an empty file
    void write(const fs::path& path, std::string_view text) {
        std::error_code io_error;
        if (!fs::exists(path.parent_path(), io_error)) {
            fs::create_directories(path.parent_path(), io_error);
            if (io_error)
                fail("could not make " + oa::platform::path_to_generic_utf8(path.parent_path()));
            folders_.push_back(path.parent_path());
        }
        files_.push_back(path);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out && !text.empty())
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out)
            fail("could not write " + oa::platform::path_to_generic_utf8(path));
    }

  private:

    std::vector<fs::path> files_;
    std::vector<fs::path> folders_;
};

// The fixture builder: a construction kbot of the base game's model and
// script whose page 1 is its own file and whose page 2 is made only of
// download entries, the Advanced Kbot Lab and the Advanced Vehicle Plant. Its
// section ends with the Copyright line a unit needs to be played
// (unit_copyright).
constexpr std::string_view kUnitName = "OAPAGES";

constexpr std::string_view kUnitFile = "[UNITINFO]\n"
                                       "{\n"
                                       "UnitName=OAPAGES;\n"
                                       "Version=1;\n"
                                       "Side=ARM;\n"
                                       "Objectname=ARMCK;\n"
                                       "Designation=OA-PAGES;\n"
                                       "Name=Pages Check;\n"
                                       "Description=Build page fallback check;\n"
                                       "FootprintX=2;\n"
                                       "FootprintZ=2;\n"
                                       "BuildCostEnergy=100;\n"
                                       "BuildCostMetal=10;\n"
                                       "MaxDamage=700;\n"
                                       "MaxWaterDepth=25;\n"
                                       "MaxSlope=20;\n"
                                       "BuildTime=100;\n"
                                       "WorkerTime=80;\n"
                                       "Builddistance=40;\n"
                                       "BMcode=1;\n"
                                       "Builder=1;\n"
                                       "ThreeD=1;\n"
                                       "ZBuffer=1;\n"
                                       "SightDistance=235;\n"
                                       "SoundCategory=ARM_CKBOT;\n"
                                       "Category=ARM KBOT LEVEL1 CONSTR NOWEAPON NOTAIR NOTSUB;\n"
                                       "TEDClass=KBOT;\n"
                                       "canmove=1;\n"
                                       "canpatrol=1;\n"
                                       "canstop=1;\n"
                                       "canguard=1;\n"
                                       "MaxVelocity=0.8;\n"
                                       "BrakeRate=0.24;\n"
                                       "Acceleration=0.12;\n"
                                       "TurnRate=1020;\n"
                                       "SteeringMode=1;\n"
                                       "DefaultMissionType=Standby;\n"
                                       "maneuverleashlength=640;\n"
                                       "MovementClass=TANKSH2;\n"
                                       "Upright=1;\n";

// One gadget record of a unit's page.
std::string gadget_record(
    int index,
    int id,
    std::string_view name,
    int x,
    int y,
    int width,
    int height,
    int attributes,
    int common_attributes
) {
    return "[GADGET" + std::to_string(index) + "]\n{\n[COMMON]\n{\nid=" + std::to_string(id) +
           ";\nassoc=" + (common_attributes != 0 ? "77" : "0") + ";\nname=" + std::string(name) +
           ";\nxpos=" + std::to_string(x) + ";\nypos=" + std::to_string(y) +
           ";\nwidth=" + std::to_string(width) + ";\nheight=" + std::to_string(height) +
           ";\nattribs=" + std::to_string(attributes) +
           ";\ncolorf=15;\ncolorb=0;\ntexturenumber=0;\nfontnumber=0;\nactive=1;\n"
           "commonattribs=" +
           std::to_string(common_attributes) + ";\n}\nstatus=0;\ntext=;\nquickkey=0;\n}\n";
}

// The unit's page 1: the panel, PREV and NEXT, the ORDERS and BUILD tabs
// and a Solar Collector, as a construction kbot's page lays them out.
std::string page_file() {
    std::string text = "[GADGET0]\n{\n[COMMON]\n{\nid=0;\nassoc=0;\nname=HEADER;\nxpos=0;\n"
                       "ypos=128;\nwidth=128;\nheight=352;\nattribs=0;\ncolorf=0;\ncolorb=0;\n"
                       "texturenumber=0;\nfontnumber=0;\nactive=1;\ncommonattribs=0;\n}\n"
                       "totalgadgets=5;\n[VERSION]\n{\nmajor=1;\nminor=0;\nrevision=0;\n}\n"
                       "panel=ARMPAN;\n}\n";
    text += gadget_record(1, 1, "ARMPREV", 8, 222, 44, 16, 2, 0);
    text += gadget_record(2, 1, "ARMNEXT", 72, 222, 44, 16, 2, 0);
    text += gadget_record(3, 1, "ARMORDERS", 3, 4, 59, 19, 16, 0);
    text += gadget_record(4, 1, "ARMBUILD", 65, 4, 59, 19, 16, 0);
    text += gadget_record(5, 1, "ARMSOLAR", 0, 27, 64, 64, 32, 4);
    return text;
}

// Page 2 is the download entries' MENU 3: BUTTON 0 and 1, the download
// page's first two patches.
constexpr std::string_view kDownloadFile = "[MENUENTRY1]\n"
                                           "{\n"
                                           "UNITMENU=OAPAGES;\n"
                                           "MENU=3;\n"
                                           "BUTTON=0;\n"
                                           "UNITNAME=ARMALAB;\n"
                                           "}\n"
                                           "[MENUENTRY2]\n"
                                           "{\n"
                                           "UNITMENU=OAPAGES;\n"
                                           "MENU=3;\n"
                                           "BUTTON=1;\n"
                                           "UNITNAME=ARMAVP;\n"
                                           "}\n";

} // namespace

void Runtime::check_build_page_fallback() {
    namespace hud = oa::ui::hud;
    namespace defs = oa::data::defs;
    if (options_.mod_dir.empty())
        fail("needs --mod-dir");
    const fs::path unit_file =
        options_.mod_dir / defs::data_path(defs::DataDirectory::units, "OAPAGES.FBI");
    const fs::path own_page = options_.mod_dir / defs::gui_path("OAPAGES1.GUI");
    const fs::path download_file =
        options_.mod_dir / defs::data_path(defs::DataDirectory::download, "OAPAGES.TDF");
    const fs::path script_file = options_.mod_dir / "scripts" / "OAPAGES.COB";
    std::error_code io_error;
    for (const auto& file : {unit_file, own_page, download_file, script_file})
        if (fs::exists(file, io_error))
            fail(
                "needs a mod folder with no " +
                oa::platform::path_to_generic_utf8(file.lexically_relative(options_.mod_dir))
            );
    if (io_error)
        fail("could not look in the mod folder");
    // The check writes into the mod folder, never into the game folder it layers over.
    std::size_t roots_at_mod_dir = 0;
    for (const auto& root : assets_.loose_roots()) {
        std::error_code same_error;
        if (fs::equivalent(root, options_.mod_dir, same_error))
            ++roots_at_mod_dir;
    }
    std::error_code game_error;
    if (roots_at_mod_dir > 1 || (!options_.game_dir.empty() &&
                                 fs::equivalent(options_.game_dir, options_.mod_dir, game_error)))
        fail("needs a mod folder that is not the game folder");
    WrittenFiles written;
    written.write(
        unit_file, std::string(kUnitFile) + "Copyright=" + defs::unit_copyright + ";\n}\n"
    );
    written.write(own_page, page_file());
    written.write(download_file, kDownloadFile);
    // The Construction Kbot's script, so the unit plays as one.
    const auto script = assets_.read("scripts/ARMCK.COB").bytes;
    if (script.empty())
        fail("scripts/ARMCK.COB was empty");
    written.write(script_file, {reinterpret_cast<const char*>(script.data()), script.size()});
    assets_.mark_loose_shadows();

    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
    bool running = true;
    const auto send = [&](SDL_EventType type, float x, float y) {
        float window_x = 0.0F;
        float window_y = 0.0F;
        if (!frame_to_window(sdl_.renderer, x, y, &window_x, &window_y))
            fail(SDL_GetError());
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = window_x;
            event.motion.y = window_y;
        } else {
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        dispatch_event(event, running);
    };
    const auto click = [&](float x, float y) {
        send(SDL_EVENT_MOUSE_MOTION, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
    };
    const auto gadget_named = [&](std::string_view name) -> std::size_t {
        if (match_hud_)
            for (std::size_t index = 1; index < match_hud_->layout.gadgets.size(); ++index)
                if (match_hud_->layout.gadgets[index].common.name == name)
                    return index;
        fail("the side column shows no " + std::string(name));
    };
    const auto click_gadget = [&](std::string_view name) {
        const auto point = hud_gadget_centre(gadget_named(name));
        click(static_cast<float>(point.x), static_cast<float>(point.y));
    };

    start_benchmark_skirmish();
    apply_output_mode();
    const auto type_of = [&](const char* name) {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0 || type >= spawn_types_.size())
            fail(std::string("the match has no ") + name + " unit type");
        return type;
    };
    const auto pages = type_of(kUnitName.data());
    const auto lab = type_of("ARMALAB");
    const auto plant = type_of("ARMAVP");
    // Page 1 is its file; the download entries' MENU 3 raises the type's
    // first missing page to 3, as 3.1c raises it.
    const auto page_count = match_->state().unit_defs[pages].gui_page_count;
    if (page_count != 3)
        fail("OAPAGES's first missing page is " + std::to_string(page_count) + ", not 3");
    auto& slots = match_->world().slots;
    uint16_t commander = 0;
    for (const auto& slot : slots) {
        if (slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            commander = slot.unit_index;
            break;
        }
    }
    if (commander == 0)
        fail("found no local commander");
    const auto* placed = place_finished_structure(pages, commander);
    if (placed == nullptr)
        fail("could not place OAPAGES");
    const auto unit = placed->unit_index;
    const auto select = [&](uint16_t id) {
        center_camera_on_unit(id);
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        const auto on_screen = project_match_point(viewport, slots[id].unit->position);
        click(static_cast<float>(on_screen.x), static_cast<float>(on_screen.y));
        if (selected_match_unit_ != id)
            fail("did not select " + spawn_type_names_[slots[id].record.type_index]);
    };
    const auto own_page_path = defs::gui_path("OAPAGES1.GUI");
    const auto download_page = defs::gui_path(match_side_name_prefix() + "DL.GUI");
    const auto kept_page = [&] { return hud::build_page(slots[unit].unit->flags); };
    const auto on_own_page = [&](const std::string& after) {
        if (match_hud_panel_ != own_page_path || match_build_page_ != 1 || kept_page() != 1)
            fail(
                after + " showed " + match_hud_panel_ + " as page " +
                std::to_string(match_build_page_) + ", the unit keeping page " +
                std::to_string(kept_page()) + ", not OAPAGES1.GUI as page 1"
            );
    };
    const auto fault_lines = [&] {
        std::vector<std::string> lines;
        for (const auto& line : reported_data_faults_)
            if (line.find("OAPAGES (Pages Check) build page") != std::string::npos)
                lines.push_back(line);
        return lines;
    };

    select(unit);
    if (match_build_page_ == 0)
        click_gadget("ARMBUILD");
    on_own_page("selecting OAPAGES");

    // Page 2 has no file of its own: the side's download page stands in, its
    // first two patches the entries' units, live, and the T2 lab is placed
    // from it.
    click_gadget("ARMNEXT");
    if (match_hud_panel_ != download_page || match_build_page_ != 2)
        fail("NEXT showed " + match_hud_panel_ + ", not " + download_page + " as page 2");

    struct Linked {
        std::size_t slot;
        const char* name;
        uint16_t type;
    };

    for (const auto& linked : {Linked{4, "ARMALAB", lab}, Linked{5, "ARMAVP", plant}}) {
        if (match_hud_->layout.gadgets.size() <= linked.slot)
            fail("the download page has no patch " + std::to_string(linked.slot));
        const auto& gadget = match_hud_->layout.gadgets[linked.slot];
        const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        if (gadget.common.name != linked.name || button == nullptr || button->grayed_out ||
            static_cast<uint8_t>(gadget.common.common_attributes) != hud::kCommonUnitButton ||
            oa::sim::unit_spawn::find_type_index(spawn_type_names_, gadget.common.name) !=
                linked.type)
            fail(
                std::string("did not link a live ") + linked.name + " button into patch " +
                std::to_string(linked.slot)
            );
    }
    click_gadget("ARMALAB");
    if (match_command_ != MatchCommand::build || pending_build_type_ != lab)
        fail("a click on the download page's ARMALAB did not place the Advanced Kbot Lab");
    reset_match_command();
    match_command_ = MatchCommand::none;
    pending_build_type_ = 0;
    click_gadget("ARMNEXT");
    on_own_page("NEXT from page 2");
    if (!fault_lines().empty())
        fail("a page that showed was logged: " + fault_lines().front());

    // The download page cannot be read: an empty file in the mod folder
    // hides the game's. Page 2 is passed over, each way and by its key, and
    // the unit keeps the page shown.
    written.write(options_.mod_dir / download_page, {});
    assets_.mark_loose_shadows();
    click_gadget("ARMNEXT");
    on_own_page("NEXT past the unreadable page 2");
    click_gadget("ARMPREV");
    on_own_page("PREV past the unreadable page 2");
    show_match_page_by_key(2);
    on_own_page("the page key 2");
    // Selected again while another unit's page shows, a unit that keeps the
    // unreadable page shows the page after it, never the other unit's.
    select(commander);
    auto& kept = slots[unit].unit->flags;
    kept = hud::build_menu_select(kept, page_count, 2);
    select(unit);
    on_own_page("selecting OAPAGES, which kept page 2");
    if (status_.find("not available") != std::string::npos)
        fail("put up an alert: " + status_);
    // The match plays on.
    for (int tick = 0; tick < 30; ++tick)
        step_match_simulation();
    if (!match_ || match_finished_)
        fail("the match did not play on");

    const auto lines = fault_lines();
    if (lines.size() != 1)
        fail("the unreadable page was logged " + std::to_string(lines.size()) + " times, not once");
    const std::string& line = lines.front();
    for (const std::string& part :
         {std::string("OAPAGES (Pages Check) build page 2 is not shown"),
          defs::gui_path("OAPAGES2.GUI") + " is missing or empty",
          download_page + ", the side's download page that stands in for it, could not be read"})
        if (line.find(part) == std::string::npos)
            fail("the log line does not say " + part + ": " + line);
    std::cout << "build page fallback check: logged once: " << line << '\n';

    while (oa::ui::frontend_dialogs::dialog_count() != 0)
        oa::ui::frontend_dialogs::close_dialog();
    if (match_)
        leave_match();
    load(Screen::main_menu);
    std::cout << "build page fallback check: passed\n";
}

} // namespace oa::app
