// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A test expansion. Its id, English name, extends and needs are Cavedog
// units, so one manifest is valid against the built-in table. Its only file
// is four bytes the built-in table does not know. The game installs with the
// built-in table and refuses this package. Tests pass test_bundle() to the
// functions that take a table. folder_bundle() is the same expansion shaped
// as one with an archive beside a folder of files.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles.hpp"

#include <cstdint>
#include <span>
#include <string_view>

namespace oa::test::expansion_fixture {

/// The manifest a test package carries. `version` and `date` are quoted.
///
/// @return the YAML text
inline std::string_view manifest_text() noexcept {
    return "oaexp: 1\n"
           "id: cavedog-units\n"
           "name: Cavedog units\n"
           "version: \"1\"\n"
           "extends:\n"
           "  - totala\n"
           "needs:\n"
           "  - core-contingency\n"
           "author:\n"
           "  name: Cavedog\n"
           "  email: ridge@example.com\n"
           "packaging:\n"
           "  revision: 1\n"
           "  date: \"2026-10-13\"\n"
           "  packager: Core Prime\n";
}

/// The manifest as bytes.
///
/// @return the YAML bytes; they live as long as the program
inline std::span<const uint8_t> manifest_bytes() noexcept {
    const std::string_view text = manifest_text();
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

/// The one file the test table names, `abcd`.
///
/// @return those four bytes
inline std::span<const uint8_t> unit_bytes() noexcept {
    static constexpr uint8_t bytes[] = {'a', 'b', 'c', 'd'};
    return std::span<const uint8_t>(bytes, 4);
}

/// The test table. The pointer stays valid for the process.
///
/// @return the bundle
inline const oa::data::content_bundles::Bundle& test_bundle() {
    using oa::data::content_bundles::Bundle;
    using oa::data::content_bundles::ContentFile;
    using oa::data::content_bundles::Kind;
    static constexpr std::string_view id = "cavedog-units";
    static constexpr std::string_view english = "Cavedog units";
    static constexpr std::string_view extends_items[] = {"totala"};
    static constexpr std::string_view needs_items[] = {"core-contingency"};
    static constexpr std::string_view copy_name = "unit.fbi";
    static constexpr std::string_view source_names[] = {"unit.fbi"};
    static const auto digest = oa::base::sha256::digest_of(unit_bytes());
    static const oa::base::sha256::Digest hashes[] = {digest};
    static const ContentFile files[] = {
        {copy_name,
         std::span<const std::string_view>(source_names, 1),
         uint64_t{4},
         std::span<const oa::base::sha256::Digest>(hashes, 1)},
    };
    static const Bundle bundle{
        id,
        english,
        Kind::expansion,
        std::span<const std::string_view>(extends_items, 1),
        std::span<const std::string_view>(needs_items, 1),
        std::span<const ContentFile>(files, 1),
    };
    return bundle;
}

/// The second file folder_bundle() names, `efgh`, in the folder `Units`.
///
/// @return those four bytes
inline std::span<const uint8_t> folder_bytes() noexcept {
    static constexpr uint8_t bytes[] = {'e', 'f', 'g', 'h'};
    return std::span<const uint8_t>(bytes, 4);
}

/// The test table shaped as an archive beside a folder: `unit.fbi` holds
/// unit_bytes() and `Units/arm.fbi` holds folder_bytes(). The pointer stays
/// valid for the process.
///
/// @return the bundle
inline const oa::data::content_bundles::Bundle& folder_bundle() {
    using oa::data::content_bundles::Bundle;
    using oa::data::content_bundles::ContentFile;
    using oa::data::content_bundles::Kind;
    static constexpr std::string_view id = "cavedog-units";
    static constexpr std::string_view english = "Cavedog units";
    static constexpr std::string_view extends_items[] = {"totala"};
    static constexpr std::string_view needs_items[] = {"core-contingency"};
    static constexpr std::string_view unit_sources[] = {"unit.fbi"};
    static constexpr std::string_view folder_sources[] = {"Units/arm.fbi"};
    static const oa::base::sha256::Digest unit_hashes[] = {
        oa::base::sha256::digest_of(unit_bytes())
    };
    static const oa::base::sha256::Digest folder_hashes[] = {
        oa::base::sha256::digest_of(folder_bytes())
    };
    static const ContentFile files[] = {
        {"unit.fbi",
         std::span<const std::string_view>(unit_sources, 1),
         uint64_t{4},
         std::span<const oa::base::sha256::Digest>(unit_hashes, 1)},
        {"Units/arm.fbi",
         std::span<const std::string_view>(folder_sources, 1),
         uint64_t{4},
         std::span<const oa::base::sha256::Digest>(folder_hashes, 1)},
    };
    static const Bundle bundle{
        id,
        english,
        Kind::expansion,
        std::span<const std::string_view>(extends_items, 1),
        std::span<const std::string_view>(needs_items, 1),
        std::span<const ContentFile>(files, 2),
    };
    return bundle;
}

} // namespace oa::test::expansion_fixture
