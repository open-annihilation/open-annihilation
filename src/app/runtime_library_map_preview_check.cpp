// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-library-map-previews: the Library's Maps tab and the Active tab's
// map tiles decode a preview from each place one can come from. One installed
// pack has a preview picture, one installed pack has none so its terrain
// minimap is read, and one pack is only listed so its picture comes from the
// check's registry. Each picture is the size that place produces, and none
// of them is the empty well the Library draws while a picture is missing.
// The check's catalogue is served by the test fixture. No package file is
// committed.

#include "oa/app/content/service.hpp"
#include "oa/app/package_install/oamap.hpp"
#include "oa/app/package_install/origin.hpp"
#include "library_host.hpp"
#include "map_packs.hpp"
#include "oa_layer.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/registry/player_registries.hpp"
#include "oa/formats/png.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/formats/url.hpp"
#include "oa/netgame/http_fixture/server.hpp"
#include "oa/ui/library/library.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace sha = oa::base::sha256;
namespace signing = oa::base::signing;
namespace catalogue = oa::data::catalogue;
namespace registry = oa::data::registry;
namespace fixture = oa::netgame::http_fixture;
namespace library = oa::ui::library;

/// The check's registry.
constexpr std::string_view kRegistry = "m34-previews";
/// The key that signs its catalogue.
constexpr std::string_view kKeyId = "m34-previews-key";
/// The installed pack whose map has a preview picture.
constexpr std::string_view kPaintedId = "m34-painted";
/// That map's title.
constexpr std::string_view kPaintedTitle = "Painted Shore";
/// The installed pack whose map has no preview picture.
constexpr std::string_view kPlainId = "m34-plain";
/// That map's title.
constexpr std::string_view kPlainTitle = "Plain Shore";
/// The pack the catalogue lists and the player has not installed.
constexpr std::string_view kListedId = "m34-listed";
/// That map's title.
constexpr std::string_view kListedTitle = "Listed Shore";
/// How long the check waits for the catalogue and then for the pictures.
constexpr std::chrono::seconds kWait{20};

/// The side a Library thumbnail is fitted into, the preview cache's default.
constexpr int32_t kThumbnailSide = 128;

/// The server the check's catalogue and listed picture are served from.
fixture::Server& fixture_server() {
    static fixture::Server server;
    return server;
}

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("library map preview check: " + std::string(what));
}

/// Writes a text file whole.
///
/// @param file the file
/// @param text its text
void write_text(const fs::path& file, std::string_view text) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(out), "could not write " + path_to_utf8(file));
}

/// Writes a file's bytes.
///
/// @param file the file
/// @param bytes its bytes
void write_bytes(const fs::path& file, std::span<const uint8_t> bytes) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!bytes.empty())
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    require(static_cast<bool>(out), "could not write " + path_to_utf8(file));
}

/// Returns a JSON string, with quotes and backslashes escaped.
///
/// @param text the text
/// @return the quoted text
std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const char letter : text) {
        const auto byte = static_cast<unsigned char>(letter);
        if (letter == '"' || letter == '\\')
            out.push_back('\\');
        else if (byte < 0x20)
            continue;
        out.push_back(letter);
    }
    out.push_back('"');
    return out;
}

/// Returns 64 lower-case hex digits.
///
/// @param digest the digest
/// @return the text
std::string hex_of(const sha::Digest& digest) {
    const auto hex_chars = sha::to_hex(digest);
    return {hex_chars.begin(), hex_chars.end()};
}

/// A terrain file whose minimap is four pixels. The body is padding the
/// thumbnail reader does not ask for.
///
/// @return the file's bytes
std::vector<uint8_t> stand_in_terrain() {
    std::vector<uint8_t> terrain(4096, 0);
    const auto put = [&](std::size_t offset, uint32_t value) {
        terrain[offset] = static_cast<uint8_t>(value);
        terrain[offset + 1] = static_cast<uint8_t>(value >> 8);
        terrain[offset + 2] = static_cast<uint8_t>(value >> 16);
        terrain[offset + 3] = static_cast<uint8_t>(value >> 24);
    };
    using oa::formats::tnt::layout::header_bytes;
    using oa::formats::tnt::layout::minimap_header_bytes;
    put(0, 0x00002000U);
    put(4, 16);
    put(8, 16);
    put(0x28, static_cast<uint32_t>(header_bytes));
    put(0x2c, oa::formats::tnt::layout::minimap_present_flag);
    put(header_bytes, 2);
    put(header_bytes + 4, 2);
    terrain[header_bytes + minimap_header_bytes + 0] = 1;
    terrain[header_bytes + minimap_header_bytes + 1] = 2;
    terrain[header_bytes + minimap_header_bytes + 2] = 3;
    terrain[header_bytes + minimap_header_bytes + 3] = 4;
    return terrain;
}

/// Encodes a small opaque picture.
///
/// @param width its width
/// @param height its height
/// @param rows red, green and blue for each pixel, top row first
/// @return the picture's bytes
std::vector<uint8_t> encode_png(uint32_t width, uint32_t height, std::span<const uint8_t> rows) {
    oa::formats::png::Image image;
    image.header.width = width;
    image.header.height = height;
    image.header.bit_depth = 8;
    image.header.color_type = oa::formats::png::ColorType::rgb;
    image.rows = rows;
    std::vector<uint8_t> file;
    require(oa::formats::png::write(image, &file), "a preview picture did not encode");
    return file;
}

/// The rectangle a preview picture's map takes inside the Library's square
/// thumbnail, which keeps the map's own shape: its longer side fills the square.
///
/// @param width the picture's width
/// @param height the picture's height
/// @return the fitted width and height
std::pair<uint32_t, uint32_t> fitted_png(uint32_t width, uint32_t height) {
    const auto side = static_cast<uint32_t>(kThumbnailSide);
    if (width >= height)
        return {side, std::max(1U, height * side / width)};
    return {std::max(1U, width * side / height), side};
}

/// Writes one installed pack: its manifest, a terrain file, an empty scenario
/// file the manifest names, and a preview picture when `preview` is set.
///
/// @param maps the player's Maps folder
/// @param id the pack's id
/// @param title the map's title
/// @param stem the map's file name without its extension
/// @param preview the preview picture; empty for none
void write_pack(
    const fs::path& maps,
    std::string_view id,
    std::string_view title,
    std::string_view stem,
    std::span<const uint8_t> preview,
    const sha::Digest& package_digest
) {
    const fs::path folder = maps / std::string(id);
    std::string yaml = "oamap: 1\n";
    yaml += "id: \"";
    yaml += id;
    yaml += "\"\nname: \"";
    yaml += title;
    yaml += " pack\"\nversion: \"1.0\"\n";
    yaml += "author: {name: unknown}\n";
    yaml += "packaging: {revision: 1, date: 2026-10-12, packager: Open Annihilation}\n";
    yaml += "requires: {base: ta-3.1c}\nmaps:\n - stem: \"";
    yaml += stem;
    yaml += "\"\n   name: \"";
    yaml += title;
    yaml += "\"\n   players: 2\n   size: \"2x2\"\n";
    if (!preview.empty())
        yaml += "   preview: \"previews/shore.png\"\n";
    yaml += "   files:\n    - \"maps/";
    yaml += stem;
    yaml += ".ota\"\n    - \"maps/";
    yaml += stem;
    yaml += ".tnt\"\n";
    write_text(folder / "oamap.yaml", yaml);
    write_bytes(folder / "maps" / (std::string(stem) + ".ota"), {});
    write_bytes(folder / "maps" / (std::string(stem) + ".tnt"), stand_in_terrain());
    if (!preview.empty())
        write_bytes(folder / "previews" / "shore.png", preview);

    package_install::Origin origin;
    origin.kind = package_install::OriginKind::catalogue;
    origin.registry = std::string(kRegistry);
    origin.catalogue_id = std::string(id);
    origin.release = 1;
    origin.sha256 = package_digest;
    origin.installed = "2026-10-12";
    const std::string record = package_install::origin_text(origin);
    require(!record.empty(), "the pack's origin record is empty");
    write_text(folder / std::string(package_install::origin_file_name), record);
}

/// One catalogue map, as the check's catalogue lists it.
struct ListedMap {
    std::string_view id{};
    std::string_view title{};
    std::string_view stem{};
    std::string preview{}; ///< a picture reference; empty for none
};

/// Returns one package's JSON object.
///
/// @param map the map
/// @param file the package file's reference
/// @param digest the package file's SHA-256, as hex
/// @return the object, without a trailing comma
std::string package_json(const ListedMap& map, std::string_view file, std::string_view digest) {
    std::string json = "{\"kind\":\"oamap\",\"id\":";
    json += json_string(map.id);
    json += ",\"name\":";
    json += json_string(std::string(map.title) + " pack");
    json += ",\"release\":1,\"size\":1,\"sha256\":";
    json += json_string(digest);
    json += ",\"file\":";
    json += json_string(file);
    json += ",\"maps\":[{\"map\":";
    json += json_string(std::string(map.stem) + "@" + std::string(map.id));
    json += ",\"name\":";
    json += json_string(map.title);
    json += ",\"players\":2,\"size\":\"2x2\"";
    if (!map.preview.empty()) {
        json += ",\"preview\":";
        json += json_string(map.preview);
    }
    json += ",\"compatible\":{\"ta-3.1c\":true}}]}";
    return json;
}

/// Returns the installed map's picture, when the Library has decoded one.
///
/// @param model the Library
/// @param title the map's title
/// @return the picture; null when that map has none yet
const library::MapPicture*
installed_picture(const library::Library& model, std::string_view title) {
    for (const library::InstalledMaps& pack : model.inputs.map_packs)
        for (const library::MapLine& map : pack.maps)
            if (map.name == title && map.preview && !map.preview->rgba.empty())
                return &*map.preview;
    return nullptr;
}

/// Returns a listed map's picture, when the Library has decoded one.
///
/// @param model the Library
/// @param title the map's title
/// @return the picture; null when that map has none yet
const library::MapPicture* listed_picture(const library::Library& model, std::string_view title) {
    for (const library::Listing& listing : model.inputs.listings)
        for (const library::MapLine& map : listing.maps)
            if (map.name == title && map.preview && !map.preview->rgba.empty())
                return &*map.preview;
    return nullptr;
}

/// Tells whether a picture is the size a decoded thumbnail of that kind has.
///
/// @param picture the picture
/// @param width the expected width
/// @param height the expected height
/// @return true when every pixel is there and one is opaque
bool picture_is(const library::MapPicture* picture, uint32_t width, uint32_t height) {
    if (picture == nullptr || picture->width != width || picture->height != height)
        return false;
    const std::size_t bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    if (picture->rgba.size() != bytes)
        return false;
    for (std::size_t index = 3; index < picture->rgba.size(); index += 4)
        if (picture->rgba[index] == 255)
            return true;
    return false;
}

/// Tells whether a square thumbnail shows its map at the map's own shape:
/// every pixel outside the centred rectangle the map takes, a pixel clear of
/// its edge, is opaque black.
///
/// @param picture the thumbnail
/// @param shape the rectangle the map takes, from fitted_png
/// @return true when the picture is the square and its bars are black
bool keeps_shape(const library::MapPicture* picture, std::pair<uint32_t, uint32_t> shape) {
    const auto side = static_cast<uint32_t>(kThumbnailSide);
    if (!picture_is(picture, side, side))
        return false;
    const uint32_t left = (side - shape.first) / 2;
    const uint32_t top = (side - shape.second) / 2;
    for (uint32_t y = 0; y < side; ++y)
        for (uint32_t x = 0; x < side; ++x) {
            const bool near_map =
                x + 1 >= left && x <= left + shape.first && y + 1 >= top && y <= top + shape.second;
            if (near_map)
                continue;
            const std::size_t at = (static_cast<std::size_t>(y) * side + x) * 4U;
            if (picture->rgba[at] != 0 || picture->rgba[at + 1] != 0 ||
                picture->rgba[at + 2] != 0 || picture->rgba[at + 3] != 255)
                return false;
        }
    return true;
}

} // namespace

void Runtime::prepare_library_map_preview_check() {
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder().empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    require(options_.data_dir.has_value(), "the run has no --data-dir");

    const uint8_t painted_rows[] = {220, 30, 30, 20, 20, 180};
    const uint8_t listed_rows[] = {20, 180, 40, 40, 40, 200};
    const std::vector<uint8_t> painted = encode_png(2, 1, painted_rows);
    const std::vector<uint8_t> listed = encode_png(1, 2, listed_rows);
    const sha::Digest listed_digest = sha::digest_of(listed);
    const std::string listed_hex = hex_of(listed_digest);
    const std::string listed_preview = "/v1/i/" + listed_hex + ".png";

    const fs::path maps = user_folder() / std::string(package_install::oamap::maps_folder_name);
    std::error_code clear_error;
    fs::remove_all(maps, clear_error);
    const std::vector<uint8_t> package_bytes{0x6d};
    const sha::Digest package_digest = sha::digest_of(package_bytes);
    const std::string package_hex = hex_of(package_digest);
    write_pack(maps, kPaintedId, kPaintedTitle, "painted", painted, package_digest);
    write_pack(maps, kPlainId, kPlainTitle, "plain", {}, package_digest);
    const std::string package_file = "/v1/p/" + package_hex + ".oamap";

    std::string listen_error;
    require(fixture_server().start(&listen_error), "the server did not listen: " + listen_error);
    const std::string catalogue_url = fixture_server().url("/v1/catalogue.json");

    std::array<uint8_t, 32> seed{};
    seed.fill(0x34);
    signing::SecretKey secret{};
    signing::PublicKey public_key{};
    signing::key_pair_from_seed(seed, secret, public_key);
    seed.fill(0);
    const std::string key_text(signing::public_key_text(public_key).view());

    const ListedMap maps_listed[] = {
        {kPaintedId, kPaintedTitle, "painted", {}},
        {kPlainId, kPlainTitle, "plain", {}},
        {kListedId, kListedTitle, "listed", listed_preview},
    };
    std::string json = "{\"catalogue\":1,\"registry\":";
    json += json_string(kRegistry);
    json += ",\"sequence\":1,\"generated\":\"2026-10-01T00:00:00Z\","
            "\"expires\":\"2027-01-01T00:00:00Z\",\"keys\":[{\"id\":";
    json += json_string(kKeyId);
    json += ",\"public\":";
    json += json_string(key_text);
    json += "}],\"packages\":[";
    for (std::size_t index = 0; index < 3; ++index) {
        if (index != 0)
            json += ',';
        json += package_json(maps_listed[index], package_file, package_hex);
    }
    json += "]}";

    const std::vector<uint8_t> catalogue_bytes(json.begin(), json.end());
    const signing::Signature signature = signing::sign(secret, catalogue_bytes);
    secret.fill(0);
    const std::string signature_text =
        catalogue::signature_file_text(catalogue::SignatureLine{std::string(kKeyId), signature});
    fixture_server().serve_bytes("/v1/catalogue.json", catalogue_bytes, "application/json");
    fixture_server().serve_bytes(
        "/v1/catalogue.json.sig",
        std::vector<uint8_t>(signature_text.begin(), signature_text.end()),
        "text/plain"
    );
    fixture_server().serve_bytes(package_file, package_bytes, "application/octet-stream");
    fixture_server().serve_bytes(listed_preview, listed, "image/png");

    registry::PlayerRegistries list;
    // SDL caches this string for the process. The caller does not free it.
    if (const char* base = SDL_GetBasePath(); base != nullptr) {
        const fs::path builtins =
            path_from_utf8(base) / path_from_utf8(content::builtin_registries_folder_name);
        std::error_code error;
        if (fs::is_directory(builtins, error))
            for (const registry::Descriptor& built_in :
                 registry::read_builtin_registries(builtins, nullptr))
                list.disabled.push_back(built_in.id);
    }
    const std::optional<oa::formats::url::Url> parsed =
        oa::formats::url::parse_http_url(catalogue_url);
    require(parsed.has_value(), "the catalogue's address did not read");
    registry::AddedRegistry added;
    added.descriptor.id = std::string(kRegistry);
    added.descriptor.name = "Preview Check";
    added.descriptor.catalogue = *parsed;
    added.descriptor.keys.push_back(registry::RegistryKey{std::string(kKeyId), public_key});
    added.descriptor.mode = registry::DownloadMode::direct;
    added.added = "2026-10-12";
    added.enabled = true;
    list.registries.push_back(std::move(added));
    write_text(
        user_folder() / std::string(registry::player_registries_file),
        registry::player_registries_text(list)
    );
    std::cout << "library map preview check: registry " << kRegistry << " at " << catalogue_url
              << '\n';
}

void Runtime::check_library_map_previews() {
    require(sdl_.window != nullptr, "the check needs the window");
    map_packs().refresh();
    require(map_packs().maps().size() == 2, "the two installed packs were not read");
    bool painted_has_picture = false;
    bool plain_has_picture = false;
    for (const PackMap& map : map_packs().maps()) {
        if (map.title == kPaintedTitle)
            painted_has_picture = !map.preview.empty();
        if (map.title == kPlainTitle)
            plain_has_picture = map.preview.empty();
    }
    require(
        painted_has_picture && plain_has_picture,
        "the installed packs are not one with a picture and one without"
    );

    // Every thumbnail is the square; a picture keeps its map's shape inside it.
    const auto painted_shape = fitted_png(2, 1);
    const auto listed_shape = fitted_png(1, 2);
    const auto square = static_cast<uint32_t>(kThumbnailSide);

    bool running = true;
    require(
        SDL_SetWindowSize(sdl_.window, 1920, 1080) && SDL_SyncWindow(sdl_.window),
        "the window did not take 1920x1080"
    );
    apply_output_mode();
    const auto frame = [&] {
        run_frame(running);
        require(running, "a frame ended the run");
    };
    for (int index = 0; index < 8; ++index)
        frame();
    require(oa_layer().top() == nullptr, "a screen stayed over the main menu");

    content_service().refresh(content::RefreshReason::check_now, std::string(kRegistry));
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    const auto wait = [&](const auto& ready, std::string_view what) {
        while (!ready()) {
            require(
                std::chrono::steady_clock::now() < deadline, std::string(what) + " did not arrive"
            );
            frame();
        }
    };
    wait(
        [&] {
            const std::shared_ptr<const content::Snapshot> snapshot = content_service().snapshot();
            if (snapshot == nullptr)
                return false;
            bool listed = false;
            for (const content::Entry& entry : snapshot->entries)
                listed = listed || (entry.package != nullptr && entry.registry == kRegistry &&
                                    entry.package->id == kListedId);
            return listed;
        },
        "the catalogue"
    );

    open_library(library::Tab::maps);
    frame();
    require(
        oa_layer().find(library_screen_name) != nullptr &&
            library_host().model.tab == library::Tab::maps,
        "the Library did not open on Maps"
    );

    const auto named = [this](std::string_view name) {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return true;
        return false;
    };
    wait(
        [&] {
            return named("oa.library.row.m34-previews.m34-painted") &&
                   named("oa.library.row.m34-previews.m34-plain") &&
                   named("oa.library.row.m34-previews.m34-listed");
        },
        "the three map rows"
    );
    wait(
        [&] {
            const library::Library& model = library_host().model;
            return keeps_shape(installed_picture(model, kPaintedTitle), painted_shape) &&
                   picture_is(installed_picture(model, kPlainTitle), square, square) &&
                   keeps_shape(listed_picture(model, kListedTitle), listed_shape);
        },
        "the three map pictures"
    );
    std::cout << "library map preview check: Maps shows " << kPaintedTitle << " "
              << painted_shape.first << "x" << painted_shape.second << ", " << kPlainTitle << " "
              << square << "x" << square << ", " << kListedTitle << " " << listed_shape.first << "x"
              << listed_shape.second << ", each centred on black in its square\n";

    library::set_tab(library_host().model, library::Tab::active);
    ++library_host().model_revision;
    frame();
    require(library_host().model.tab == library::Tab::active, "the Library did not show Active");
    wait(
        [&] {
            if (!named("oa.library.active.map.1") || !named("oa.library.active.map.2"))
                return false;
            const library::ActiveSetup setup = library::active_setup(library_host().model);
            if (setup.maps.size() != 2)
                return false;
            bool painted = false;
            bool plain = false;
            for (const library::ActiveMap& tile : setup.maps) {
                if (tile.name == kPaintedTitle)
                    painted = keeps_shape(tile.preview, painted_shape);
                if (tile.name == kPlainTitle)
                    plain = picture_is(tile.preview, square, square);
            }
            return painted && plain;
        },
        "the Active tab's map tiles"
    );
    std::cout << "library map preview check: Active shows both installed maps\n";
}

} // namespace oa::app
