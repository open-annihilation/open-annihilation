// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "shipped_archive.hpp"

#include "oa/formats/hpi.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"

#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>

namespace oa::boneyards {

bool read_shipped_archive(const char* path, std::vector<uint8_t>& out) {
    static std::mutex gate;
    static std::optional<oa::HpiArchive> archive;
    static bool opened = false;
    std::lock_guard<std::mutex> held(gate);
    if (!opened) {
        opened = true;
        try {
            const std::string directory = oa::platform::program_directory();
            if (!directory.empty()) {
                const std::filesystem::path where =
                    oa::platform::path_from_utf8(directory) / "coreprime.ccx";
                std::error_code error;
                if (std::filesystem::is_regular_file(where, error) && !error) {
                    auto loaded = oa::open_hpi_file(where);
                    if (loaded.ok() && loaded.value.has_value())
                        archive = std::move(*loaded.value);
                }
            }
        } catch (const std::exception&) {
            archive.reset();
        }
    }
    if (!archive.has_value() || path == nullptr)
        return false;
    try {
        auto read = archive->read(path);
        if (!read.ok() || !read.value.has_value())
            return false;
        out = std::move(*read.value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace oa::boneyards
