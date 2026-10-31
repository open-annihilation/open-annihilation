// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Emits HPI listings and effective-file winners for comparison with an
// independent HPI reader. A format cross-check only.
#include "oa/formats/hpi.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>

namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

uint64_t fnv(const std::vector<uint8_t>& bytes) {
    uint64_t hash = kFnvOffset;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
    return hash;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
    });
    return text;
}

int list(const char* path) {
    const auto opened = oa::open_hpi_file(path);
    if (!opened.ok()) {
        std::cerr << "oa-hpi-inspect: " << path << ": " << opened.error.message << '\n';
        return 1;
    }
    const oa::HpiArchive& archive = *opened.value;
    for (const auto& entry : archive.entries()) {
        const auto node = archive.lookup(entry.path);
        std::cout << entry.path << '\t' << entry.size << '\t';
        const auto bytes =
            node && archive.nodes()[*node].size == entry.size
                ? archive.read_node(*node)
                : oa::base::bytes::Decoded<std::vector<uint8_t>>(oa::base::bytes::DecodeError{
                      oa::base::bytes::DecodeCode::not_found, 0, "unreachable"
                  });
        if (bytes.ok())
            std::cout << std::hex << fnv(*bytes.value) << std::dec;
        else
            std::cout << bytes.error.message;
        std::cout << '\n';
    }
    return 0;
}

int winners(const char* root, const char* version) {
    oa::AssetStore store(root);
    for (const auto& outcome : store.discover(version))
        std::cerr << (outcome.mounted ? "mounted " : "skipped ")
                  << oa::platform::path_to_utf8(outcome.path.filename())
                  << (outcome.error.empty() ? "" : " (" + outcome.error + ")") << '\n';
    std::map<std::string, std::string> result;
    for (std::size_t mount = 0; mount < store.mount_paths().size(); ++mount)
        for (const auto& entry : store.mounted(mount).entries()) {
            const auto key = lower(entry.path);
            if (result.contains(key))
                continue;
            for (std::size_t m = 0; m < store.mount_paths().size(); ++m) {
                const auto node = store.mounted(m).lookup(entry.path);
                if (node && !store.mounted(m).nodes()[*node].directory()) {
                    result[key] = oa::platform::path_to_utf8(store.mount_paths()[m].filename());
                    break;
                }
            }
        }
    for (const auto& [path, source] : result)
        std::cout << path << '\t' << source << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "list" && argc == 3)
            return list(argv[2]);
        if (mode == "winners" && (argc == 3 || argc == 4))
            return winners(argv[2], argc == 4 ? argv[3] : "31");
        std::cerr << "usage: oa-hpi-inspect list ARCHIVE | winners GAME_DIR [VERSION]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "oa-hpi-inspect: " << error.what() << '\n';
        return 1;
    }
}
