// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A unit's page whose picture has a single frame, pressed. 3.1c's frame
// lookup finds no frame past a sequence's last, and the button is drawn
// without a picture while the press is held; the match plays on, and the
// mod's data fault is logged once, naming the page, the unit and the GAF.
#include "oa/app/runtime.hpp"

#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/frontend_dialogs.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oa::app {
namespace {

/// Throws the check's failure, naming what went wrong.
///
/// @param what the failure, without the check's name
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("picture frames check: " + what);
}

/// Writes bytes into the mod folder, making the parent folder when it is missing.
///
/// @param path the file, under the mod folder
/// @param bytes the bytes to write
void write_bytes(const fs::path& path, std::span<const uint8_t> bytes) {
    std::error_code io_error;
    fs::create_directories(path.parent_path(), io_error);
    if (io_error)
        fail("could not make " + oa::platform::path_to_generic_utf8(path.parent_path()));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        fail("could not write " + oa::platform::path_to_generic_utf8(path));
    if (!bytes.empty())
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    if (!out)
        fail("could not write " + oa::platform::path_to_generic_utf8(path));
}

void put16(std::vector<uint8_t>& out, std::size_t at, uint16_t value) {
    out[at] = static_cast<uint8_t>(value);
    out[at + 1] = static_cast<uint8_t>(value >> 8);
}

void put32(std::vector<uint8_t>& out, std::size_t at, uint32_t value) {
    put16(out, at, static_cast<uint16_t>(value));
    put16(out, at + 2, static_cast<uint16_t>(value >> 16));
}

/// A GAF holding one sequence of one uncompressed frame, every pixel one
/// palette entry: the header, the sequence's place, the sequence header and
/// its frame list, then the frame's record and its pixels.
///
/// @param name the sequence's name, at most 31 characters
/// @param width the frame's width
/// @param height the frame's height
/// @param colour the palette entry every pixel holds
/// @return the file's bytes
std::vector<uint8_t>
one_frame_gaf(std::string_view name, uint16_t width, uint16_t height, uint8_t colour) {
    constexpr std::size_t kSequenceAt = 16;
    constexpr std::size_t kFrameListAt = kSequenceAt + 40;
    constexpr std::size_t kFrameAt = kFrameListAt + 8;
    constexpr std::size_t kPixelsAt = kFrameAt + 24;
    constexpr uint32_t kDuration = 10;
    std::vector<uint8_t> out(kPixelsAt, 0);
    put32(out, 0, oa::formats::gaf::version_ta);
    put32(out, 4, 1);
    put32(out, 12, kSequenceAt);
    put16(out, kSequenceAt, 1);
    std::memcpy(&out[kSequenceAt + 8], name.data(), std::min<std::size_t>(name.size(), 31));
    put32(out, kFrameListAt, kFrameAt);
    put32(out, kFrameListAt + 4, kDuration);
    put16(out, kFrameAt, width);
    put16(out, kFrameAt + 2, height);
    // Its transparent entry is one the frame does not hold.
    out[kFrameAt + 8] = static_cast<uint8_t>(colour + 1);
    put32(out, kFrameAt + 16, kPixelsAt);
    out.resize(kPixelsAt + static_cast<std::size_t>(width) * height, colour);
    return out;
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
        for (const auto& folder : folders_)
            if (fs::is_empty(folder, ignored))
                fs::remove(folder, ignored);
    }

    /// Writes a file, remembering it and its folder when the check makes it.
    void write(const fs::path& path, std::span<const uint8_t> bytes) {
        std::error_code exists_error;
        if (!fs::exists(path.parent_path(), exists_error))
            folders_.push_back(path.parent_path());
        files_.push_back(path);
        write_bytes(path, bytes);
    }

  private:

    std::vector<fs::path> files_;
    std::vector<fs::path> folders_;
};

// The fixture unit: a kbot of the base game's Peewee model that builds
// nothing and has one page of its own, its picture one frame of
// kPictureColour. Its section ends with the Copyright line a unit needs to be
// played (unit_copyright).
constexpr std::string_view kUnitName = "OAFRAME";
constexpr std::string_view kPicture = "OAFRAMEPIC";
constexpr uint8_t kPictureColour = 0xf9;
constexpr uint16_t kPictureSide = 64;

constexpr std::string_view kUnitFile = "[UNITINFO]\n"
                                       "{\n"
                                       "UnitName=OAFRAME;\n"
                                       "Version=1;\n"
                                       "Side=ARM;\n"
                                       "Objectname=ARMPW;\n"
                                       "Designation=OA-FRAME;\n"
                                       "Name=Frame Check;\n"
                                       "Description=Picture frame check;\n"
                                       "FootprintX=2;\n"
                                       "FootprintZ=2;\n"
                                       "BuildCostEnergy=100;\n"
                                       "BuildCostMetal=10;\n"
                                       "MaxDamage=250;\n"
                                       "MaxWaterDepth=12;\n"
                                       "MaxSlope=17;\n"
                                       "BuildTime=100;\n"
                                       "WorkerTime=0;\n"
                                       "BMcode=1;\n"
                                       "Builder=0;\n"
                                       "ThreeD=1;\n"
                                       "ZBuffer=1;\n"
                                       "SightDistance=280;\n"
                                       "SoundCategory=ARMPW;\n"
                                       "Category=ARM KBOT LEVEL1 NOTAIR NOTSUB;\n"
                                       "TEDClass=KBOT;\n"
                                       "canmove=1;\n"
                                       "canpatrol=1;\n"
                                       "canstop=1;\n"
                                       "canguard=1;\n"
                                       "MaxVelocity=1.8;\n"
                                       "BrakeRate=0.19;\n"
                                       "Acceleration=0.1;\n"
                                       "TurnRate=1120;\n"
                                       "SteeringMode=2;\n"
                                       "DefaultMissionType=Standby;\n"
                                       "maneuverleashlength=640;\n"
                                       "MovementClass=TANKSH2;\n"
                                       "Upright=1;\n";

// The unit's page 1: the panel, the ORDERS and BUILD tabs, and the picture,
// as a unit's own page lays them out.
constexpr std::string_view kPageFile = "[GADGET0]\n"
                                       "{\n"
                                       "[COMMON]\n"
                                       "{\n"
                                       "id=0;\n"
                                       "assoc=0;\n"
                                       "name=HEADER;\n"
                                       "xpos=0;\n"
                                       "ypos=128;\n"
                                       "width=128;\n"
                                       "height=352;\n"
                                       "attribs=0;\n"
                                       "colorf=0;\n"
                                       "colorb=0;\n"
                                       "texturenumber=0;\n"
                                       "fontnumber=0;\n"
                                       "active=1;\n"
                                       "commonattribs=0;\n"
                                       "}\n"
                                       "totalgadgets=4;\n"
                                       "[VERSION]\n"
                                       "{\n"
                                       "major=1;\n"
                                       "minor=0;\n"
                                       "revision=0;\n"
                                       "}\n"
                                       "panel=ARMPAN;\n"
                                       "}\n"
                                       "[GADGET1]\n"
                                       "{\n"
                                       "[COMMON]\n"
                                       "{\n"
                                       "id=1;\n"
                                       "assoc=0;\n"
                                       "name=ARMORDERS;\n"
                                       "xpos=3;\n"
                                       "ypos=4;\n"
                                       "width=59;\n"
                                       "height=19;\n"
                                       "attribs=16;\n"
                                       "colorf=15;\n"
                                       "colorb=0;\n"
                                       "texturenumber=0;\n"
                                       "fontnumber=0;\n"
                                       "active=1;\n"
                                       "commonattribs=0;\n"
                                       "}\n"
                                       "status=0;\n"
                                       "text=;\n"
                                       "quickkey=111;\n"
                                       "}\n"
                                       "[GADGET2]\n"
                                       "{\n"
                                       "[COMMON]\n"
                                       "{\n"
                                       "id=1;\n"
                                       "assoc=0;\n"
                                       "name=ARMBUILD;\n"
                                       "xpos=65;\n"
                                       "ypos=4;\n"
                                       "width=59;\n"
                                       "height=19;\n"
                                       "attribs=16;\n"
                                       "colorf=15;\n"
                                       "colorb=0;\n"
                                       "texturenumber=0;\n"
                                       "fontnumber=0;\n"
                                       "active=1;\n"
                                       "commonattribs=0;\n"
                                       "}\n"
                                       "status=0;\n"
                                       "text=;\n"
                                       "quickkey=98;\n"
                                       "}\n"
                                       "[GADGET3]\n"
                                       "{\n"
                                       "[COMMON]\n"
                                       "{\n"
                                       "id=1;\n"
                                       "assoc=77;\n"
                                       "name=OAFRAMEPIC;\n"
                                       "xpos=32;\n"
                                       "ypos=40;\n"
                                       "width=64;\n"
                                       "height=64;\n"
                                       "attribs=32;\n"
                                       "colorf=15;\n"
                                       "colorb=0;\n"
                                       "texturenumber=0;\n"
                                       "fontnumber=0;\n"
                                       "active=1;\n"
                                       "commonattribs=0;\n"
                                       "}\n"
                                       "status=0;\n"
                                       "text=;\n"
                                       "quickkey=0;\n"
                                       "}\n";

/// Returns a surface's pixel, or black outside it.
std::array<uint8_t, 3> pixel_at(std::span<const uint8_t> rgb, uint32_t width, int x, int y) {
    const auto offset =
        (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * std::size_t{3};
    if (x < 0 || y < 0 || offset + 3 > rgb.size())
        return {};
    return {rgb[offset], rgb[offset + 1], rgb[offset + 2]};
}

} // namespace

void Runtime::check_picture_frames() {
    if (options_.mod_dir.empty())
        fail("needs --mod-dir");
    const fs::path unit_file = options_.mod_dir / "units" / "OAFRAME.FBI";
    const fs::path page_file = options_.mod_dir / "guis" / "OAFRAME1.GUI";
    const fs::path art_file = options_.mod_dir / "anims" / "OAFRAME1.GAF";
    std::error_code io_error;
    for (const auto& file : {unit_file, page_file, art_file})
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
    const std::string unit_text =
        std::string(kUnitFile) + "Copyright=" + oa::data::defs::unit_copyright + ";\n}\n";
    written.write(
        unit_file, {reinterpret_cast<const uint8_t*>(unit_text.data()), unit_text.size()}
    );
    written.write(
        page_file, {reinterpret_cast<const uint8_t*>(kPageFile.data()), kPageFile.size()}
    );
    written.write(art_file, one_frame_gaf(kPicture, kPictureSide, kPictureSide, kPictureColour));
    assets_.mark_loose_shadows();

    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
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
    const auto gadget_named = [&](std::string_view name) -> std::size_t {
        if (match_hud_)
            for (std::size_t index = 1; index < match_hud_->layout.gadgets.size(); ++index)
                if (match_hud_->layout.gadgets[index].common.name == name)
                    return index;
        fail("the side column shows no " + std::string(name));
    };

    start_benchmark_skirmish();
    apply_output_mode();
    const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, kUnitName.data());
    if (type == 0 || type >= spawn_types_.size())
        fail("the match has no OAFRAME unit type");
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
    const auto* placed = place_finished_structure(type, commander);
    if (placed == nullptr)
        fail("could not place OAFRAME");
    const auto unit = placed->unit_index;
    center_camera_on_unit(unit);
    const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
    const auto on_screen = project_match_point(viewport, slots[unit].unit->position);
    click(static_cast<float>(on_screen.x), static_cast<float>(on_screen.y));
    if (selected_match_unit_ != unit)
        fail("did not select OAFRAME");
    // The unit's page shows once BUILD is pressed on the general page.
    const auto build = hud_gadget_centre(gadget_named("ARMBUILD"));
    click(static_cast<float>(build.x), static_cast<float>(build.y));
    if (match_hud_panel_ != oa::data::defs::gui_path("OAFRAME1.GUI"))
        fail("BUILD opened " + match_hud_panel_ + ", not OAFRAME's page");

    const auto picture = gadget_named(kPicture);
    const auto& gadget = match_hud_->layout.gadgets[picture].common;
    if (gadget.width != kPictureSide || gadget.height != kPictureSide)
        fail("the picture's button does not take its frame's size");
    const int sample_x = gadget.x + gadget.width / 2;
    const int sample_y = gadget.y + gadget.height / 2;
    const auto drawn = [&] {
        return pixel_at(match_hud_cpu_.rgb, match_hud_cpu_.width, sample_x, sample_y);
    };
    const auto& background = match_hud_->background;
    const auto under = pixel_at(background.rgb, background.width, sample_x, sample_y);
    const auto& palette = background.palette ? *background.palette : match_hud_->gui_palette;
    const std::size_t entry = static_cast<std::size_t>(kPictureColour) * 4U;
    const std::array<uint8_t, 3> frame_colour{
        palette[entry], palette[entry + 1], palette[entry + 2]
    };
    if (frame_colour == under)
        fail("the picture's colour is the panel's under it");
    const auto fault_lines = [&] {
        std::vector<std::string> lines;
        for (const auto& line : reported_data_faults_)
            if (line.find(kPicture) != std::string::npos)
                lines.push_back(line);
        return lines;
    };

    render_match_surface();
    if (drawn() != frame_colour)
        fail("the picture's frame was not drawn");
    if (!fault_lines().empty())
        fail("a picture drawn whole was logged");
    const auto centre = hud_gadget_centre(picture);
    for (int press = 1; press <= 2; ++press) {
        const std::string which = press == 1 ? "the first press" : "the second press";
        send(SDL_EVENT_MOUSE_MOTION, 0, static_cast<float>(centre.x), static_cast<float>(centre.y));
        send(
            SDL_EVENT_MOUSE_BUTTON_DOWN,
            SDL_BUTTON_LEFT,
            static_cast<float>(centre.x),
            static_cast<float>(centre.y)
        );
        // Held over several frames: each draws the button pressed, asking for
        // the frame after its only one.
        for (int frame = 0; frame < 3; ++frame) {
            render_match_surface();
            if (drawn() != under)
                fail(which + " drew a frame the picture does not have");
        }
        send(
            SDL_EVENT_MOUSE_BUTTON_UP,
            SDL_BUTTON_LEFT,
            static_cast<float>(centre.x),
            static_cast<float>(centre.y)
        );
        render_match_surface();
        if (drawn() != frame_colour)
            fail(which + ", let go, did not draw the picture's frame again");
        if (!match_ || match_hud_panel_ != oa::data::defs::gui_path("OAFRAME1.GUI"))
            fail(which + " left OAFRAME's page");
    }

    const auto lines = fault_lines();
    if (lines.size() != 1)
        fail("the missing frame was logged " + std::to_string(lines.size()) + " times, not once");
    const std::string& line = lines.front();
    for (const std::string_view part :
         {std::string_view("OAFRAME1.GUI"),
          std::string_view("(unit OAFRAME, Frame Check)"),
          std::string_view(
              "OAFRAMEPIC asks for frame index 1 of GAF sequence OAFRAMEPIC in "
              "anims/OAFRAME1.GAF, which has 1 frame; drawn without it"
          )})
        if (line.find(part) == std::string::npos)
            fail("the log line does not name " + std::string(part) + ": " + line);
    std::cout << "picture frames check: logged once: " << line << '\n';

    while (oa::ui::frontend_dialogs::dialog_count() != 0)
        oa::ui::frontend_dialogs::close_dialog();
    if (match_)
        leave_match();
    load(Screen::main_menu);
    std::cout << "picture frames check: passed\n";
}

} // namespace oa::app
