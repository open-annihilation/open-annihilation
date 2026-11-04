// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Opens coreprime.ccx with the engine's archive reader, parses the animation
// and checks each sequence's frame count and size. The new screens' layouts
// are packed under gui/. The registration frame's pixels are compared with
// tools/qr.py's matrix for the address the archive was built with. The
// profile copied beside the archive leaves installation-archives unset.
// The install's by.ccx is a .ccx, so the engine mounts it with that group,
// and naming it would change the mod's sim hash. The two archives share no
// path, so the new screens still come from the mod's archive.
//
// Usage: oa-coreprime-ccx-check ARCHIVE PYTHON QR_PY URL
// The archive carries no war map.

#include "oa/formats/gaf.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#else
#include <stdio.h>
#endif

namespace {

constexpr int kQrScale = 3;
constexpr int kQrQuiet = 4;
constexpr uint8_t kQrBorder = 90;
constexpr uint8_t kQrLight = 81;
constexpr uint8_t kQrDark = 0;
constexpr uint8_t kTransparency = 9;

struct Expect {
    const char* name;
    int frames;
    int width;
    int height;
};

constexpr Expect kSequences[] = {
    {"Scanner", 8, 72, 72},
    {"Locator_Icons", 10, 11, 11},
    {"Queue_Meter", 11, 104, 9},
    {"QR_Register", 1, 113, 113},
};

// Reads a whole file. `opened` is false when the file cannot be read.
std::vector<uint8_t> read_bytes(const std::filesystem::path& path, bool& opened) {
    std::ifstream in(path, std::ios::binary);
    opened = static_cast<bool>(in);
    if (!opened)
        return {};
    in.seekg(0, std::ios::end);
    const auto length = in.tellg();
    in.seekg(0);
    if (length < 0) {
        opened = false;
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty())
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        opened = false;
        return {};
    }
    return bytes;
}

std::string read_text(const std::filesystem::path& path, bool& opened) {
    const std::vector<uint8_t> bytes = read_bytes(path, opened);
    return std::string(bytes.begin(), bytes.end());
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

#ifdef _WIN32

std::string quote_arg(const char* text) {
    std::string out = "\"";
    for (const char* at = text; *at != '\0'; ++at) {
        if (*at == '"' || *at == '\\')
            out.push_back('\\');
        out.push_back(*at);
    }
    out.push_back('"');
    return out;
}

// Runs qr.py and appends its stdout to `out`. The command goes through the
// shell, so each argument is quoted. cmd.exe drops the first and last quote of
// a line that starts with one, so the whole line is quoted once more.
bool run_qr(const char* python, const char* script, const char* url, std::string& out) {
    const std::string command = quote_arg(python) + " " + quote_arg(script) + " --matrix " +
                                quote_arg(url) + " --qr-version 3";
    const std::string line = "\"" + command + "\"";
    FILE* pipe = _popen(line.c_str(), "r");
    if (pipe == nullptr)
        return false;
    char buffer[4096];
    out.clear();
    while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) {
        out.append(buffer);
        if (out.size() > 1024U * 1024U) {
            _pclose(pipe);
            return false;
        }
    }
    const int status = _pclose(pipe);
    return status == 0;
}

#else

// Runs qr.py with the address as one argument and appends its stdout to `out`.
bool run_qr(const char* python, const char* script, const char* url, std::string& out) {
    int pipe_fd[2];
    if (pipe(pipe_fd) != 0)
        return false;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        return false;
    }
    posix_spawn_file_actions_adddup2(&actions, pipe_fd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipe_fd[0]);
    posix_spawn_file_actions_addclose(&actions, pipe_fd[1]);
    const char* args[] = {python, script, "--matrix", url, "--qr-version", "3", nullptr};
    pid_t pid = 0;
    const int spawned =
        posix_spawnp(&pid, python, &actions, nullptr, const_cast<char* const*>(args), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipe_fd[1]);
    if (spawned != 0) {
        close(pipe_fd[0]);
        return false;
    }
    out.clear();
    char buffer[4096];
    bool overflow = false;
    for (;;) {
        const ssize_t count = read(pipe_fd[0], buffer, sizeof buffer);
        if (count < 0) {
            overflow = true;
            break;
        }
        if (count == 0)
            break;
        out.append(buffer, buffer + count);
        if (out.size() > 1024U * 1024U) {
            overflow = true;
            break;
        }
    }
    close(pipe_fd[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return false;
    return !overflow && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#endif

std::vector<std::string> matrix_rows(const std::string& text) {
    std::vector<std::string> rows;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            rows.push_back(std::move(line));
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return rows;
}

const oa::formats::gaf::Sequence*
find_sequence(const oa::formats::gaf::Archive& archive, const char* name) {
    for (const oa::formats::gaf::Sequence& sequence : archive.sequences) {
        if (sequence.name == name)
            return &sequence;
    }
    return nullptr;
}

bool has_entry(const oa::HpiArchive& archive, const char* path) {
    for (const oa::ArchiveEntry& entry : archive.entries()) {
        if (entry.path == path)
            return true;
    }
    return false;
}

void check_qr_pixels(
    const std::vector<std::string>& rows, const std::vector<uint8_t>& pixels, int width, int height
) {
    OA_CHECK(!rows.empty());
    if (rows.empty())
        return;
    const int modules = static_cast<int>(rows.size());
    bool square = true;
    for (const std::string& row : rows) {
        if (static_cast<int>(row.size()) != modules) {
            square = false;
            break;
        }
        for (char cell : row) {
            if (cell != '0' && cell != '1')
                square = false;
        }
    }
    OA_CHECK(square);
    if (!square)
        return;
    const int side = (modules + 2 * kQrQuiet) * kQrScale + 2;
    OA_CHECK(width == side && height == side);
    if (width != side || height != side)
        return;
    OA_CHECK(static_cast<int>(pixels.size()) == side * side);
    if (static_cast<int>(pixels.size()) != side * side)
        return;
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            uint8_t expect = kQrLight;
            if (x == 0 || y == 0 || x == side - 1 || y == side - 1) {
                expect = kQrBorder;
            } else {
                const int module_x = (x - 1) / kQrScale - kQrQuiet;
                const int module_y = (y - 1) / kQrScale - kQrQuiet;
                if (module_x >= 0 && module_y >= 0 && module_x < modules && module_y < modules &&
                    rows[static_cast<std::size_t>(module_y)][static_cast<std::size_t>(module_x)] ==
                        '1')
                    expect = kQrDark;
            }
            if (pixels[static_cast<std::size_t>(y * side + x)] != expect) {
                std::fprintf(
                    stderr,
                    "registration pixel (%d,%d) is %u, expected %u\n",
                    x,
                    y,
                    pixels[static_cast<std::size_t>(y * side + x)],
                    expect
                );
                OA_CHECK(false);
                return;
            }
        }
    }
}

void check_sequence(const oa::formats::gaf::Sequence& sequence, const Expect& expect) {
    OA_CHECK(static_cast<int>(sequence.frames.size()) == expect.frames);
    if (static_cast<int>(sequence.frames.size()) != expect.frames)
        return;
    for (const oa::formats::gaf::Frame& frame : sequence.frames) {
        OA_CHECK(frame.width == expect.width);
        OA_CHECK(frame.height == expect.height);
        OA_CHECK(frame.transparency_index == kTransparency);
        OA_CHECK(!frame.compressed);
        OA_CHECK(frame.layer_count == 0);
        OA_CHECK(
            frame.pixels.size() ==
            static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height)
        );
    }
}

} // namespace

int main(int argc, char** argv) {
    OA_CHECK(argc == 5);
    if (argc != 5) {
        std::fprintf(stderr, "usage: oa-coreprime-ccx-check ARCHIVE PYTHON QR_PY URL\n");
        return oa::test::check_exit_status();
    }

    auto opened = oa::open_hpi_file(std::filesystem::path(argv[1]));
    OA_CHECK(opened.ok());
    if (!opened.ok()) {
        std::fprintf(
            stderr, "archive: %s\n", opened.error.message != nullptr ? opened.error.message : ""
        );
        return oa::test::check_exit_status();
    }
    const oa::HpiArchive& archive = *opened.value;

    constexpr const char* kGaf = "anims/BY_New.gaf";
    constexpr const char* kReadme = "README.md";
    constexpr const char* kMap = "boneyards/alpha_map.gwm";
    constexpr const char* kRegister = "gui/register_unavailable.gui";
    constexpr const char* kQuickPlay = "gui/quickplay.gui";
    constexpr const char* kMatchmaking = "gui/matchmaking.gui";
    constexpr const char* kRegistration = "gui/registration.gui";
    constexpr const char* kLocator = "gui/locator.gui";
    OA_CHECK(has_entry(archive, kGaf));
    OA_CHECK(has_entry(archive, kReadme));
    OA_CHECK(has_entry(archive, kRegister));
    OA_CHECK(has_entry(archive, kQuickPlay));
    OA_CHECK(has_entry(archive, kMatchmaking));
    OA_CHECK(has_entry(archive, kRegistration));
    OA_CHECK(has_entry(archive, kLocator));
    OA_CHECK(!has_entry(archive, "gui/sample.gui"));
    OA_CHECK(!has_entry(archive, "gui/broken.gui"));
    OA_CHECK(!has_entry(archive, kMap));
    // The animation and the readme, and every screen layout under gui/.
    // Counting the layouts keeps this check true as new screens arrive; each
    // one is also named above. No war map is packed.
    int expected_entries = 2;
    for (const auto& entry : archive.entries()) {
        if (entry.path.rfind("gui/", 0) == 0)
            ++expected_entries;
        const bool war_map =
            entry.path.size() >= 4 && entry.path.compare(entry.path.size() - 4, 4, ".gwm") == 0;
        OA_CHECK(!war_map);
    }
    OA_CHECK(static_cast<int>(archive.entries().size()) == expected_entries);

    auto readme = archive.read(kReadme);
    OA_CHECK(readme.ok());
    if (readme.ok())
        OA_CHECK(!readme.value->empty());

    auto gaf_bytes = archive.read(kGaf);
    OA_CHECK(gaf_bytes.ok());
    if (!gaf_bytes.ok())
        return oa::test::check_exit_status();
    const auto parsed = oa::formats::gaf::parse(*gaf_bytes.value);
    OA_CHECK(parsed.ok());
    if (!parsed.ok()) {
        std::fprintf(stderr, "animation: %s\n", parsed.error ? parsed.error->message.c_str() : "");
        return oa::test::check_exit_status();
    }
    const oa::formats::gaf::Archive& animation = *parsed.archive;
    OA_CHECK(animation.sequences.size() == sizeof kSequences / sizeof kSequences[0]);

    const oa::formats::gaf::Sequence* registration = nullptr;
    for (const Expect& expect : kSequences) {
        const oa::formats::gaf::Sequence* sequence = find_sequence(animation, expect.name);
        OA_CHECK(sequence != nullptr);
        if (sequence == nullptr)
            continue;
        check_sequence(*sequence, expect);
        if (std::strcmp(expect.name, "QR_Register") == 0)
            registration = sequence;
    }

    std::string matrix_text;
    const bool qr_ran = run_qr(argv[2], argv[3], argv[4], matrix_text);
    OA_CHECK(qr_ran);
    if (qr_ran && registration != nullptr && registration->frames.size() == 1) {
        const oa::formats::gaf::Frame& frame = registration->frames[0];
        check_qr_pixels(matrix_rows(matrix_text), frame.pixels, frame.width, frame.height);
    }

    bool profile_opened = false;
    const std::filesystem::path profile_path =
        std::filesystem::path(argv[1]).parent_path() / "oamod.yaml";
    const std::string profile = read_text(profile_path, profile_opened);
    OA_CHECK(profile_opened);
    if (profile_opened) {
        OA_CHECK(contains(profile, "oamod: 1"));
        OA_CHECK(contains(profile, "id: boneyards"));
        OA_CHECK(!contains(profile, "installation-archives"));
    }
    return oa::test::check_exit_status();
}
