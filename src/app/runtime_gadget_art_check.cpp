// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A download button's picture is read for the match that is playing. Three
// skirmishes of one run: the button has no picture, then the picture from a
// file written into the mod folder, then none once that file is gone.
#include "oa/app/runtime.hpp"

#include "oa/data/defs/layout.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/hud/order_panel.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

namespace oa::app {
namespace {

/// Throws the check's failure, naming what went wrong.
///
/// @param what the failure, without the check's name
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("gadget art per match check: " + what);
}

/// Writes bytes into the mod folder, making the parent folder when it is missing.
///
/// @param path the file, under the mod folder
/// @param bytes the bytes to write
/// @param size how many bytes
void write_bytes(const fs::path& path, const uint8_t* bytes, std::size_t size) {
    std::error_code io_error;
    fs::create_directories(path.parent_path(), io_error);
    if (io_error)
        fail("could not make " + oa::platform::path_to_generic_utf8(path.parent_path()));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        fail("could not write " + oa::platform::path_to_generic_utf8(path));
    if (size != 0)
        out.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(size));
    if (!out)
        fail("could not write " + oa::platform::path_to_generic_utf8(path));
}

/// Removes the download file and the button art the check wrote, and the
/// folders it made when they are empty, however the check ends.
class WrittenGadgetFiles {
  public:

    WrittenGadgetFiles(
        fs::path download, fs::path art, bool made_download_folder, bool made_anims_folder
    )
        : download_(std::move(download)), art_(std::move(art)),
          made_download_folder_(made_download_folder), made_anims_folder_(made_anims_folder) {}

    WrittenGadgetFiles(const WrittenGadgetFiles&) = delete;
    WrittenGadgetFiles& operator=(const WrittenGadgetFiles&) = delete;

    ~WrittenGadgetFiles() {
        std::error_code ignored;
        fs::remove(download_, ignored);
        fs::remove(art_, ignored);
        if (made_download_folder_ && fs::is_empty(download_.parent_path(), ignored))
            fs::remove(download_.parent_path(), ignored);
        if (made_anims_folder_ && fs::is_empty(art_.parent_path(), ignored))
            fs::remove(art_.parent_path(), ignored);
    }

  private:

    fs::path download_;
    fs::path art_;
    bool made_download_folder_;
    bool made_anims_folder_;
};

} // namespace

void Runtime::check_gadget_art_per_match() {
    namespace hud = oa::ui::hud;
    if (options_.mod_dir.empty())
        fail("needs --mod-dir");
    const fs::path download = options_.mod_dir / "download" / "OAGADGETCHECK.TDF";
    const fs::path art = options_.mod_dir / "anims" / "ARMPW_gadget.gaf";
    std::error_code io_error;
    if (fs::exists(download, io_error) || fs::exists(art, io_error))
        fail("needs a mod folder with no download/OAGADGETCHECK.TDF or anims/ARMPW_gadget.gaf");
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
    std::error_code folder_error;
    const bool made_download_folder = !fs::exists(download.parent_path(), folder_error);
    const bool made_anims_folder = !fs::exists(art.parent_path(), folder_error);
    const WrittenGadgetFiles cleanup(download, art, made_download_folder, made_anims_folder);

    // ARMDL.GUI is build page 2. A download entry's MENU is that page plus
    // one, and BUTTON 2 is the empty slot after ARMWAR and ARMFLEA (gadget 6).
    constexpr std::string_view kDownload = "[MENUENTRY1]\n"
                                           "{\n"
                                           "UNITMENU=ARMLAB;\n"
                                           "MENU=3;\n"
                                           "BUTTON=2;\n"
                                           "UNITNAME=ARMPW;\n"
                                           "}\n";
    write_bytes(download, reinterpret_cast<const uint8_t*>(kDownload.data()), kDownload.size());
    assets_.mark_loose_shadows();

    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
    const auto type_of = [&](const char* name) {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0 || type >= spawn_types_.size())
            fail(std::string("lacks ") + name);
        return type;
    };
    bool running = true;
    const auto send = [&](SDL_EventType type, uint8_t button, float x, float y) {
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
            event.button.button = button;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        dispatch_event(event, running);
    };
    const auto click = [&](float x, float y) {
        send(SDL_EVENT_MOUSE_MOTION, 0, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, x, y);
    };
    const auto leave_for_menu = [&] {
        while (oa::ui::frontend_dialogs::dialog_count() != 0)
            oa::ui::frontend_dialogs::close_dialog();
        if (match_)
            leave_match();
        load(Screen::main_menu);
    };
    const auto play = [&](bool expect_picture, const char* which) {
        start_benchmark_skirmish();
        apply_output_mode();
        const auto lab = type_of("ARMLAB");
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
            fail(std::string(which) + " found no local commander");
        const auto* placed = place_finished_structure(lab, commander);
        if (placed == nullptr)
            fail(std::string(which) + " could not place ARMLAB");
        const auto factory = placed->unit_index;
        center_camera_on_unit(factory);
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        const auto on_screen = project_match_point(viewport, slots[factory].unit->position);
        click(static_cast<float>(on_screen.x), static_cast<float>(on_screen.y));
        if (selected_match_unit_ != factory)
            fail(std::string(which) + " did not select the lab");
        if (match_hud_panel_ != oa::data::defs::gui_path("ARMLAB1.GUI") || match_build_page_ != 1)
            fail(std::string(which) + " did not open the lab's build page");
        if (!match_hud_)
            fail(std::string(which) + " has no order panel");
        std::size_t next_button = 0;
        bool found_next = false;
        for (std::size_t gadget_index = 1; gadget_index < match_hud_->layout.gadgets.size();
             ++gadget_index) {
            if (match_hud_->layout.gadgets[gadget_index].common.name == "ARMNEXT") {
                next_button = gadget_index;
                found_next = true;
                break;
            }
        }
        if (!found_next)
            fail(std::string(which) + " found no ARMNEXT button");
        const auto next_point = hud_gadget_centre(next_button);
        click(static_cast<float>(next_point.x), static_cast<float>(next_point.y));
        if (match_hud_panel_ != oa::data::defs::gui_path("ARMDL.GUI") || match_build_page_ != 2)
            fail(std::string(which) + " did not open the download page");
        if (match_hud_->layout.gadgets.size() <= 6)
            fail(std::string(which) + " download page has no empty slot");
        const auto& gadget = match_hud_->layout.gadgets[6];
        const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        if (gadget.common.name != "ARMPW" || !gadget.common.gaf_file || button == nullptr ||
            button->grayed_out ||
            static_cast<uint8_t>(gadget.common.common_attributes) != hud::kCommonUnitButton)
            fail(std::string(which) + " did not link ARMPW into the empty download slot");
        const bool drawn = gaf_sequence(match_hud_->sprites, "ARMPW") != nullptr;
        if (drawn != expect_picture) {
            fail(
                std::string(which) +
                (expect_picture ? " did not draw ARMPW's picture" : " drew ARMPW's picture")
            );
        }
        leave_for_menu();
    };

    play(false, "the first match");

    const auto picture = assets_.read("anims/ARMLAB1.GAF");
    if (picture.bytes.empty())
        fail("anims/ARMLAB1.GAF was empty");
    write_bytes(art, picture.bytes.data(), picture.bytes.size());
    assets_.mark_loose_shadows();
    play(true, "the second match");

    io_error.clear();
    if (!fs::remove(art, io_error) || io_error)
        fail("could not remove anims/ARMPW_gadget.gaf");
    assets_.mark_loose_shadows();
    play(false, "the third match");

    std::cout << "gadget art per match check: passed\n";
}

} // namespace oa::app
