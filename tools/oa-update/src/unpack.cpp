// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/unpack.hpp"

#include "oa/formats/zip.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/update_helper/paths.hpp"

#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace oa::update_helper {
namespace {

namespace fs = std::filesystem;
namespace zip = oa::formats::zip;

void set_error(std::string* reason, std::string message) {
    if (reason != nullptr)
        *reason = std::move(message);
}

struct FileSource {
    std::ifstream in;

    bool read_at(uint64_t offset, std::span<uint8_t> bytes) {
        if (bytes.empty())
            return true;
        if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
            return false;
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset));
        if (!in)
            return false;
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<std::streamsize>(in.gcount()) ==
               static_cast<std::streamsize>(bytes.size());
    }
};

bool read_hook(void* context, uint64_t offset, std::span<uint8_t> bytes) {
    return static_cast<FileSource*>(context)->read_at(offset, bytes);
}

bool write_hook(void* context, std::span<const uint8_t> bytes) {
    auto* out = static_cast<std::ofstream*>(context);
    if (bytes.empty())
        return true;
    out->write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    return static_cast<bool>(*out);
}

bool inside_top(std::string_view name, std::string_view top) {
    if (top.empty() || name.empty())
        return false;
    const std::string prefix = std::string(top) + "/";
    return name == top || name == prefix || name.starts_with(prefix);
}

bool recorded_refused(const zip::StreamEntry& entry) {
    if (entry.recorded_name.empty())
        return false;
    if (entry.recorded_name.find('\\') != std::string::npos)
        return true;
    return !zip::name_is_safe(entry.recorded_name);
}

bool stays_in_stage(const fs::path& stage, const fs::path& dest) {
    const fs::path relative = dest.lexically_normal().lexically_relative(stage.lexically_normal());
    const std::string text = utf8_of(relative);
    if (text.empty() || text == ".." || text.starts_with("../") || text.starts_with("..\\"))
        return false;
    return true;
}

bool executable_name(std::string_view name) {
    const auto slash = name.rfind('/');
    const std::string_view base = slash == std::string_view::npos ? name : name.substr(slash + 1);
    return base == "open-annihilation" || base == "oa-intro" || base == "oa-tool" ||
           base == "oa-update";
}

void remove_stage(const fs::path& stage) {
    std::error_code io;
    fs::remove_all(stage, io);
}

} // namespace

std::string top_folder_name(std::string_view version, std::string_view package) {
    return std::string("open-annihilation-") + std::string(version) + "-" + std::string(package);
}

bool unpack_package(
    const fs::path& archive, const fs::path& stage, std::string_view top, std::string* error
) {
    std::error_code size_io;
    const auto size = fs::file_size(archive, size_io);
    if (size_io) {
        set_error(error, "the package could not be read");
        return false;
    }
    FileSource source;
    source.in.open(archive, std::ios::binary);
    if (!source.in) {
        set_error(error, "the package could not be read");
        return false;
    }
    zip::SourceHooks hooks;
    hooks.context = &source;
    hooks.read_at = read_hook;
    zip::StreamDirectory directory;
    zip::ZipError zip_error;
    if (!zip::read_stream_directory(
            hooks, static_cast<uint64_t>(size), zip::StreamLimits{}, directory, zip_error
        )) {
        set_error(
            error,
            std::string("the package was refused: ") + zip::zip_status_message(zip_error.status)
        );
        return false;
    }

    for (const zip::StreamEntry& entry : directory.entries) {
        if (entry.symbolic_link || entry.special_file || recorded_refused(entry) ||
            !inside_top(entry.name, top)) {
            set_error(error, "the package holds a name that is not under its top folder");
            return false;
        }
    }

    remove_stage(stage);
    std::error_code io;
    fs::create_directories(stage, io);
    if (io) {
        set_error(error, "the staging folder could not be made");
        return false;
    }

    for (const zip::StreamEntry& entry : directory.entries) {
        const fs::path dest = stage / path_from(entry.name);
        if (!stays_in_stage(stage, dest)) {
            set_error(error, "the package holds a name that is not under its top folder");
            remove_stage(stage);
            return false;
        }
        if (entry.directory) {
            fs::create_directories(dest, io);
            if (io) {
                set_error(error, "the package could not be unpacked");
                remove_stage(stage);
                return false;
            }
            continue;
        }
        fs::create_directories(dest.parent_path(), io);
        if (io) {
            set_error(error, "the package could not be unpacked");
            remove_stage(stage);
            return false;
        }
        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
        if (!out) {
            set_error(error, "the package could not be unpacked");
            remove_stage(stage);
            return false;
        }
        // Windows will not remove the stage while this file is open.
        const auto drop_stage = [&]() {
            out.close();
            remove_stage(stage);
        };
        zip::EntryStream stream;
        if (!stream.open(hooks, static_cast<uint64_t>(size), entry, zip_error)) {
            set_error(error, "the package could not be unpacked");
            drop_stage();
            return false;
        }
        zip::SinkHooks sink;
        sink.context = &out;
        sink.write = write_hook;
        while (true) {
            const zip::StreamStep stepped = stream.step(sink, 64 * 1024, zip_error);
            if (stepped == zip::StreamStep::done)
                break;
            if (stepped == zip::StreamStep::failed) {
                set_error(error, "the package could not be unpacked");
                drop_stage();
                return false;
            }
        }
        out.close();
#if !defined(_WIN32)
        if (executable_name(entry.name)) {
            fs::permissions(dest, fs::perms::owner_exec, fs::perm_options::add, io);
            if (io) {
                set_error(error, "the package could not be unpacked");
                remove_stage(stage);
                return false;
            }
        }
#endif
    }
    return true;
}

} // namespace oa::update_helper
