// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// This build's package id. A steam-deck directory is tried in a scratch
// folder. Nothing is fetched.

#include "oa/app/update/build.hpp"

#include "oa/app/version_line.hpp"
#include "oa/data/engine_feed/feed.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/test/check.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

#if defined(OA_WINDOWS_95)
constexpr bool kWindows95 = true;
#else
constexpr bool kWindows95 = false;
#endif

/// A scratch folder removed when the case ends.
struct Scratch {
    fs::path path;

    explicit Scratch(std::string_view name) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               (std::string("oa-v03-") + std::string(name) + "-" + std::to_string(ticks));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void package_ids() {
    const std::string_view platforms[] = {
        "macos-universal",
        "windows-x64",
        "windows-x86",
        "windows-arm64",
        "linux-x86_64",
        "linux-arm64",
        "linux-armhf",
        "ios",
        "android",
        "unknown",
    };
    for (const std::string_view platform : platforms) {
        OA_CHECK(update::package_id(platform, false, false) == platform);
        OA_CHECK(update::package_id(platform, true, true) == "windows-95");
    }
    OA_CHECK(update::package_id("linux-x86_64", false, true) == "steam-deck");
    OA_CHECK(update::package_id("linux-arm64", false, true) == "linux-arm64");
    OA_CHECK(update::package_id("macos-universal", false, true) == "macos-universal");
    OA_CHECK(update::package_id("ios", false, false) == "ios");
}

void this_build_matches_the_compiled_version() {
    const update::ThisBuild built = update::this_build({});
#if defined(OA_ENGINE_VERSION)
    const std::string_view compiled = OA_ENGINE_VERSION;
    const std::optional<oa::formats::oamod::EngineVersion> parsed =
        oa::formats::oamod::parse_engine_version(compiled);
    OA_CHECK(parsed.has_value());
    if (parsed) {
        OA_CHECK(built.version.major == parsed->major);
        OA_CHECK(built.version.minor == parsed->minor);
        OA_CHECK(built.version.patch == parsed->patch);
        OA_CHECK(built.version_text == oa::formats::oamod::engine_version_text(*parsed));
        const std::optional<int64_t> number = oa::data::engine_feed::release_number(*parsed);
        OA_CHECK(built.release == (number ? *number : 0));
    }
#else
    OA_CHECK(built.version_text.empty());
    OA_CHECK(built.release == 0);
#endif
#if defined(OA_ENGINE_VERSION_LABEL)
    OA_CHECK(built.development == (std::string_view(OA_ENGINE_VERSION_LABEL).size() != 0));
#else
    OA_CHECK(!built.development);
#endif
    OA_CHECK(
        built.package == update::package_id(oa::app::release_platform_name(), kWindows95, false)
    );
}

void steam_deck_folder_follows_the_platform() {
    Scratch scratch("deck");
    const fs::path folder = scratch.path / "steam-deck";
    fs::create_directory(folder);
    const update::ThisBuild deck = update::this_build(scratch.path);
    OA_CHECK(
        deck.package == update::package_id(oa::app::release_platform_name(), kWindows95, true)
    );
    fs::remove(folder);
    std::ofstream file(scratch.path / "steam-deck", std::ios::binary);
    file << "not a folder";
    file.close();
    const update::ThisBuild file_marker = update::this_build(scratch.path);
    OA_CHECK(
        file_marker.package ==
        update::package_id(oa::app::release_platform_name(), kWindows95, false)
    );
}

} // namespace

int main() {
    package_ids();
    this_build_matches_the_compiled_version();
    steam_deck_folder_follows_the_platform();
    return oa::test::check_exit_status();
}
