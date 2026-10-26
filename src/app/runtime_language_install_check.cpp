// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-language-install: the pseudo language pack installed through the
// game. A dropped file is asked about and installed, the same package is
// declined, and a catalogue revision is installed while Settings stay open
// and no prompt shows. The player's Languages folder is checked after each.
// A notice over Settings, whose drawing the layer keeps, is drawn again once
// the revision has replaced the shown language's words in place.

#include "mod_install_state.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/package_install/oalang.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/languages/language_pack.hpp"
#include "oa/formats/zip.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/engine_settings/prompt.hpp"
#include "oa/ui/kit/components_more.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {

namespace settings = oa::ui::engine_settings;
namespace install = oa::app::package_install;
namespace languages = oa::data::languages;

namespace {

/// The tag the pseudo pack installs.
constexpr std::string_view kTag = "en-XA";
/// The most frames a step of the check runs before it gives up.
constexpr int kMostFrames = 400;
/// Where the pointer rests while nothing is clicked: off every button.
constexpr oa::ui::display_layout::Point kRestingPointer{4, 240};
/// The text the pseudo pack gives Select Map.
constexpr std::string_view kSelectMap = "［测Select Map试］";

/// Throws when a step of the check fails.
///
/// @param ok the step passed
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("language install check: " + std::string(what));
}

/// Returns the SHA-256 of a file.
///
/// @param file the file
/// @return its digest
oa::base::sha256::Digest file_hash(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{in}, {}};
    return oa::base::sha256::digest_of(bytes);
}

/// Reads a file.
///
/// @param file the file
/// @return its bytes; empty when it cannot be read
std::vector<uint8_t> read_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>{in}, {}};
}

/// One file of a package.
struct Piece {
    std::string name{};
    std::vector<uint8_t> bytes{};
};

/// Reads every file under a directory.
///
/// @param root the directory
/// @return the files, named below the directory
std::vector<Piece> tree_of(const fs::path& root) {
    std::vector<Piece> pieces;
    std::error_code error;
    for (fs::recursive_directory_iterator at(root, error), end; !error && at != end;
         at.increment(error)) {
        if (!at->is_regular_file(error))
            continue;
        const std::string name = fs::relative(at->path(), root, error).generic_string();
        if (error || name.empty())
            continue;
        pieces.push_back({name, read_bytes(at->path())});
    }
    return pieces;
}

/// Finds a piece by its name.
///
/// @param pieces the pieces
/// @param name the name
/// @return the piece; null when it is missing
Piece* find_piece(std::vector<Piece>& pieces, std::string_view name) {
    for (Piece& piece : pieces)
        if (piece.name == name)
            return &piece;
    return nullptr;
}

/// Writes a stored archive.
///
/// @param file where it goes
/// @param pieces its files
void write_zip(const fs::path& file, const std::vector<Piece>& pieces) {
    std::vector<oa::formats::zip::NewEntry> entries;
    entries.reserve(pieces.size());
    for (const Piece& piece : pieces)
        entries.push_back({piece.name, piece.bytes});
    std::vector<uint8_t> archive;
    oa::formats::zip::ZipError error{};
    require(
        oa::formats::zip::write_archive(entries, archive, error),
        "a language pack could not be made"
    );
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size())
    );
    require(static_cast<bool>(out), "a language pack could not be written");
}

/// Builds the pseudo pack with a font and a packaging revision.
///
/// @param pseudo the pseudo-pack directory
/// @param file where the package goes
/// @param revision the packaging revision
void write_language_pack(const fs::path& pseudo, const fs::path& file, int64_t revision) {
    const fs::path font_file = oa::platform::text_font::bundled_font_directory() / "DejaVuSans.ttf";
    const std::vector<uint8_t> font = read_bytes(font_file);
    require(!font.empty(), "the bundled font could not be read");
    std::vector<Piece> pieces = tree_of(pseudo);
    Piece* manifest = find_piece(pieces, "language.yaml");
    require(manifest != nullptr, "the pseudo pack has no language.yaml");
    std::string yaml(manifest->bytes.begin(), manifest->bytes.end());
    yaml += "fonts:\n  - {file: Pseudo.ttf, role: letters}\n";
    yaml += "packaging: {revision: " + std::to_string(revision) +
            ", date: 2026-10-11, packager: Open Annihilation}\n";
    manifest->bytes.assign(yaml.begin(), yaml.end());
    pieces.push_back({"fonts/Pseudo.ttf", font});
    write_zip(file, pieces);
}

/// Returns the packaging revision a folder's language.yaml gives.
///
/// @param folder the installed folder
/// @return the revision; -1 when the folder holds no readable manifest
int64_t revision_in(const fs::path& folder) {
    const std::vector<uint8_t> bytes = read_bytes(folder / "language.yaml");
    languages::PackManifest manifest;
    if (bytes.empty() || !languages::read_manifest(bytes, manifest) || !manifest.packaging)
        return -1;
    return manifest.packaging->revision;
}

} // namespace

void Runtime::check_language_install() {
    require(options_.preferences_file.has_value(), "the check needs --preferences-file");
    require(!user_folder_.empty(), "the run has no folder of the player's own");
    require(sdl_.window != nullptr, "the check needs the SDL presenter");
    require(
        std::string_view(shown_language().tag) != kTag,
        "en-XA is already shown; the Languages folder was not empty at the start"
    );

    const fs::path pseudo = user_folder_.parent_path() / "pseudo-pack";
    const fs::path packages = user_folder_.parent_path() / "files";
    const fs::path folder =
        user_folder_ / std::string(install::oalang::languages_folder_name) / std::string(kTag);
    const fs::path manifest = folder / "language.yaml";
    require(fs::is_directory(pseudo), "the pseudo pack is not beside the player's folder");

    auto& state = mod_install_state();
    state.check_shows_prompts = true;
    load(Screen::main_menu);
    require(engine_settings_fonts() != nullptr, "the dialog's fonts did not load");
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);

    const auto until_prompt = [&](std::string_view what) {
        for (int frame = 0; frame < kMostFrames; ++frame) {
            tell_mod_installs();
            if (state.prompt != nullptr && (state.stage == ModInstallState::Stage::asking ||
                                            state.stage == ModInstallState::Stage::telling))
                return;
        }
        require(false, std::string("no prompt showed: ") + std::string(what));
    };
    const auto until_deleted = [&] {
        for (int frame = 0; frame < kMostFrames && state.discarder.busy(); ++frame)
            tell_mod_installs();
        require(!state.discarder.busy(), "the folders a change dropped were not deleted");
    };
    const auto click = [&](install::Answer answer) {
        require(state.prompt != nullptr, "no prompt to answer");
        const auto& prompt = state.prompt->question();
        const auto& answers = state.answers;
        const auto found = std::find(answers.begin(), answers.end(), answer);
        require(found != answers.end(), "the prompt has no such button");
        const auto button = static_cast<int32_t>(found - answers.begin());
        const auto* fonts = engine_settings_fonts();
        // Where the OA layer shows the prompt.
        const auto placed = state.prompt->placement(oa_layer().view());
        for (const auto& part : settings::prompt_layout(prompt, fonts))
            if (part.control == button) {
                // At the window pixel the layer shows the button's middle at.
                const auto pixel = layer_window_pixel(
                    placed, {part.rect.x + part.rect.width / 2, part.rect.y + part.rect.height / 2}
                );
                for (const SDL_EventType type :
                     {SDL_EVENT_MOUSE_MOTION,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      SDL_EVENT_MOUSE_BUTTON_UP}) {
                    SDL_Event event = window_pointer_event(
                        sdl_.window,
                        sdl_.renderer,
                        type,
                        pixel,
                        type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
                    );
                    bool running = true;
                    dispatch_event(event, running);
                    require(running, "a click on a prompt ended the run");
                }
                send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
                return;
            }
        require(false, "the prompt's button is not laid out");
    };

    const fs::path dropped = packages / "pseudo-r1.oalang";
    write_language_pack(pseudo, dropped, 1);
    install::post_opened_file(dropped);
    until_prompt("the dropped language pack");
    require(
        state.prompt != nullptr && state.prompt->question().title == "INSTALL LANGUAGE",
        "installing the language was not asked"
    );
    click(install::Answer::alongside);
    until_prompt("the language installed");
    require(
        state.prompt != nullptr && state.prompt->question().title == "LANGUAGE INSTALLED",
        "--open did not tell that the language pack was installed"
    );
    require(fs::is_regular_file(manifest), "Languages/en-XA/language.yaml was not installed");
    require(std::string_view(shown_language().tag) == kTag, "en-XA is not the language shown");
    require(
        std::string_view(game_translation("Select Map")) == kSelectMap,
        "Select Map was not translated"
    );
    require(modern_font_pack_faces() == 1, "the modern font stack does not hold one pack face");
    std::cout << "language install check: installed in " << path_to_utf8(folder) << '\n';
    std::error_code stamp_error;
    const auto stamp = fs::last_write_time(manifest, stamp_error);
    require(!stamp_error, "language.yaml has no modification time");
    click(install::Answer::ok);
    until_deleted();

    install::post_opened_file(dropped);
    until_prompt("the same language pack");
    require(
        state.prompt != nullptr && state.prompt->question().title == "REINSTALL LANGUAGE",
        "reinstalling the language was not asked"
    );
    click(install::Answer::cancel);
    require(state.prompt == nullptr, "Cancel did not close the question");
    require(
        std::string_view(shown_language().tag) == kTag, "cancelling changed the language shown"
    );
    const auto kept = fs::last_write_time(manifest, stamp_error);
    require(!stamp_error && kept == stamp, "cancelling changed the installed pack");

    const uint32_t prompts = state.prompts_shown;
    state.check_shows_prompts = false;
    open_engine_settings_from_menu();
    require(engine_settings_dialog() != nullptr, "Settings did not open");
    // A notice over Settings, which the layer keeps drawn while nothing it is
    // drawn from changes.
    oa::ui::kit::Notice told;
    told.title = "NOTICE";
    told.paragraphs.push_back(oa::ui::kit::Paragraph{"Select Map", false});
    told.ok_caption = "OK";
    oa_layer().push(
        std::make_unique<NoticeScreen>(
            oa_layer(), Screen::main_menu, std::move(told), NoticeScreen::Host{}
        )
    );
    const auto notice_frame = [&] {
        bool running = true;
        run_frame(running);
        require(running, "a frame with the notice ended the run");
    };
    notice_frame();
    notice_frame();
    const LayerScreen* notice = oa_layer().find(NoticeScreen::screen_name);
    require(notice != nullptr, "the notice did not open");
    const uint64_t kept_notice = oa_layer().kept_drawing(*notice);
    require(kept_notice != 0, "the layer keeps no drawing of the notice");
    const fs::path catalogue_file = packages / "pseudo-r2.oalang";
    write_language_pack(pseudo, catalogue_file, 2);
    install::Origin origin{};
    origin.kind = install::OriginKind::catalogue;
    origin.registry = "check";
    origin.catalogue_id = std::string(kTag);
    origin.release = 2;
    origin.sha256 = file_hash(catalogue_file);
    install::post_package_file(catalogue_file, origin);
    bool committed = false;
    for (int frame = 0; frame < kMostFrames; ++frame) {
        tell_mod_installs();
        require(
            state.prompt == nullptr && state.prompts_shown == prompts,
            "a catalogue pack showed a prompt"
        );
        require(engine_settings_dialog() != nullptr, "Settings closed during the install");
        if (revision_in(folder) == 2 && state.stage == ModInstallState::Stage::idle &&
            !state.unpacking) {
            committed = true;
            break;
        }
    }
    require(committed, "the catalogue revision was not installed");
    const auto record = install::read_origin(folder);
    require(
        record && record->kind == install::OriginKind::catalogue, "the origin is not a catalogue"
    );
    require(
        revision_in(folder / std::string(install::backup_folder_name)) == 1,
        ".backup does not hold revision 1"
    );
    // The pack was read again: the language shown is the same entry, and
    // its words were replaced in place.
    notice_frame();
    notice = oa_layer().find(NoticeScreen::screen_name);
    require(notice != nullptr, "the notice closed during the install");
    require(
        oa_layer().kept_drawing(*notice) != kept_notice,
        "the notice kept its drawing after its language's pack was read again"
    );
    oa_layer().close(notice);
    std::cout << "language install check: catalogue pack installed with no prompt\n";
    std::cout << "language install check: passed\n" << std::flush;
}

} // namespace oa::app
