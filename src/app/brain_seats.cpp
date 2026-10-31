// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reads one installed brain and turns a seat's choice into the spec the
// simulation seats. The origin record is the installer's, and is skipped.

#include "oa/app/brain_seats.hpp"

#include "oa/base/sha256.hpp"
#include "oa/core/game_state.h"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace oabrain = oa::formats::oabrain;
namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;

constexpr std::string_view origin_file_name = ".oa-origin.yaml";

bool folder_name(std::string_view id) {
    if (id.empty() || id == "." || id == "..")
        return false;
    return id.find('/') == std::string_view::npos && id.find('\\') == std::string_view::npos;
}

bool read_exact(const fs::path& path, std::vector<uint8_t>& bytes, std::string& error) {
    bytes.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "the package folder could not be read";
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    if (length < 0) {
        error = "the package folder could not be read";
        return false;
    }
    in.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(length));
    if (length > 0) {
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
        if (!in) {
            bytes.clear();
            error = "the package folder could not be read";
            return false;
        }
    }
    return true;
}

bool listed_paths(const fs::path& folder, std::vector<std::string>& paths, std::string& error) {
    paths.clear();
    std::error_code exists;
    if (!fs::is_directory(folder, exists)) {
        error = "the package folder is missing";
        return false;
    }
    uintmax_t total = 0;
    try {
        const fs::recursive_directory_iterator end;
        fs::recursive_directory_iterator cursor{folder};
        for (; cursor != end; ++cursor) {
            const fs::directory_entry& entry = *cursor;
            std::error_code status_error;
            const fs::file_status status = entry.symlink_status(status_error);
            if (status_error) {
                error = "the package folder could not be read";
                return false;
            }
            if (fs::is_symlink(status)) {
                error = "the package holds a link";
                return false;
            }
            if (fs::is_directory(status))
                continue;
            if (!fs::is_regular_file(status)) {
                error = "the package holds a file that is not a regular file";
                return false;
            }
            std::error_code relative_error;
            const fs::path relative = fs::relative(entry.path(), folder, relative_error);
            if (relative_error) {
                error = "the package folder could not be read";
                return false;
            }
            const std::string generic = oa::platform::path_to_generic_utf8(relative);
            if (generic.empty() || generic == ".." || generic.starts_with("../") ||
                generic.find(':') != std::string::npos) {
                error = "the package holds a path outside its folder";
                return false;
            }
            std::error_code size_error;
            const uintmax_t size = entry.file_size(size_error);
            if (size_error) {
                error = "the package folder could not be read";
                return false;
            }
            if (size > installed_package_bytes || total > installed_package_bytes - size) {
                error = "the package is larger than 128 MiB";
                return false;
            }
            total += size;
            paths.push_back(generic);
        }
    } catch (const std::exception&) {
        paths.clear();
        error = "the package folder could not be read";
        return false;
    }
    std::sort(paths.begin(), paths.end());
    return true;
}

int32_t income_of(oabrain::Difficulty difficulty) {
    switch (difficulty) {
    case oabrain::Difficulty::easy:
        return OA_DIFFICULTY_EASY;
    case oabrain::Difficulty::medium:
        return OA_DIFFICULTY_MEDIUM;
    case oabrain::Difficulty::hard:
        return OA_DIFFICULTY_HARD;
    }
    return OA_DIFFICULTY_MEDIUM;
}

oa::sim::brains::OptionKind kind_of(oamut::OptionType type) {
    if (type == oamut::OptionType::number)
        return oa::sim::brains::OptionKind::number;
    if (type == oamut::OptionType::choice)
        return oa::sim::brains::OptionKind::choice;
    return oa::sim::brains::OptionKind::toggle;
}

bool lies_on_step(
    const oamod::Number& value, const oamod::Number& minimum, const oamod::Number& step
) {
    int64_t value_i = 0;
    int64_t minimum_i = 0;
    int64_t step_i = 0;
    if (oamod::integer_value(value, value_i) && oamod::integer_value(minimum, minimum_i) &&
        oamod::integer_value(step, step_i) && step_i > 0)
        return (value_i - minimum_i) % step_i == 0;
    const double step_count = oamod::number_to_double(step);
    if (step_count == 0.0)
        return false;
    const double taken =
        (oamod::number_to_double(value) - oamod::number_to_double(minimum)) / step_count;
    const double nearest = std::round(taken);
    return std::fabs(taken - nearest) <= 1e-8;
}

const oamut::Option* option_named(const oabrain::Manifest& manifest, std::string_view id) {
    for (const oamut::Option& option : manifest.options) {
        if (option.id == id)
            return &option;
    }
    return nullptr;
}

bool player_text(const BrainSeatChoice& choice, std::string_view id, std::string& text) {
    bool found = false;
    for (const auto& named : choice.options) {
        if (named.first == id) {
            found = true;
            text = named.second;
        }
    }
    return found;
}

bool apply_text(
    const oamut::Option& option, std::string_view text, ResolvedOption& made, std::string& error
) {
    if (option.type == oamut::OptionType::toggle) {
        if (text != "true" && text != "false") {
            error = option.id + ": " + std::string(text) + " is not true or false";
            return false;
        }
        made.toggle = text == "true";
        made.text.clear();
        return true;
    }
    if (option.type == oamut::OptionType::choice) {
        oamod::Number parsed{};
        const bool parsed_ok = oamod::parse_number(text, parsed) == oamod::NumberStatus::ok;
        for (const oamut::ChoiceValue& listed : option.values) {
            const bool matches =
                listed.number
                    ? parsed_ok && oamod::compare_numbers(parsed, listed.number_value) == 0
                    : listed.text == text;
            if (!matches)
                continue;
            if (listed.number) {
                made.number = oamod::number_to_double(listed.number_value);
                made.text.clear();
            } else {
                made.text = listed.text;
                made.number = 0.0;
            }
            return true;
        }
        std::string listed_text;
        for (const oamut::ChoiceValue& listed : option.values) {
            if (!listed_text.empty())
                listed_text += ", ";
            listed_text += listed.text;
        }
        error = option.id + ": " + std::string(text) + " is not one of " + listed_text;
        return false;
    }
    oamod::Number parsed{};
    if (oamod::parse_number(text, parsed) != oamod::NumberStatus::ok) {
        error = option.id + ": " + std::string(text) + " is not a number";
        return false;
    }
    if (oamod::compare_numbers(parsed, option.minimum) < 0 ||
        oamod::compare_numbers(parsed, option.maximum) > 0) {
        error = option.id + ": " + std::string(text) + " is outside " +
                oamod::canonical_number_text(option.minimum) + " to " +
                oamod::canonical_number_text(option.maximum);
        return false;
    }
    if (!lies_on_step(parsed, option.minimum, option.step)) {
        error = option.id + ": " + std::string(text) + " does not lie on a step of " +
                oamod::canonical_number_text(option.step) + " from " +
                oamod::canonical_number_text(option.minimum);
        return false;
    }
    made.number = oamod::number_to_double(parsed);
    made.text.clear();
    return true;
}

void apply_default(const oamut::Option& option, ResolvedOption& made) {
    made.kind = kind_of(option.type);
    made.toggle = false;
    made.number = 0.0;
    made.text.clear();
    if (option.type == oamut::OptionType::toggle) {
        made.toggle = option.toggle_default;
        return;
    }
    if (option.type == oamut::OptionType::number) {
        made.number = oamod::number_to_double(option.number_default);
        return;
    }
    if (option.values.empty())
        return;
    const std::size_t index =
        option.default_index < option.values.size() ? option.default_index : 0;
    const oamut::ChoiceValue& listed = option.values[index];
    if (listed.number) {
        made.number = oamod::number_to_double(listed.number_value);
        return;
    }
    made.text = listed.text;
}

} // namespace

std::optional<InstalledBrain>
read_installed_brain(const fs::path& brains_folder, std::string_view id, std::string& error) {
    if (!folder_name(id)) {
        error = "the brain id is not a folder name";
        return std::nullopt;
    }
    const fs::path folder = brains_folder / oa::platform::path_from_utf8(id);
    std::vector<std::string> paths;
    if (!listed_paths(folder, paths, error))
        return std::nullopt;

    const auto manifest_at =
        std::find(paths.begin(), paths.end(), std::string(oabrain::manifest_file));
    if (manifest_at == paths.end()) {
        error = "oabrain.yaml is missing";
        return std::nullopt;
    }
    std::vector<uint8_t> manifest_bytes;
    if (!read_exact(folder / oa::platform::path_from_utf8(*manifest_at), manifest_bytes, error))
        return std::nullopt;
    std::vector<oabrain::Problem> problems;
    oabrain::Manifest manifest;
    if (!oabrain::read_manifest(manifest_bytes, manifest, problems)) {
        error = problems.empty() ? "oabrain.yaml could not be read"
                                 : oabrain::describe(problems.front());
        return std::nullopt;
    }
    if (manifest.id != id) {
        error = "the manifest id does not match the folder";
        return std::nullopt;
    }

    InstalledBrain installed;
    installed.folder = folder;
    installed.manifest = std::move(manifest);
    installed.files.reserve(paths.size());
    for (const std::string& path : paths) {
        if (path == origin_file_name)
            continue;
        if (oabrain::file_role(path, installed.manifest) == oabrain::FileRole::outside) {
            error = path + " is not a file a brain may hold";
            return std::nullopt;
        }
        std::vector<uint8_t> bytes;
        if (!read_exact(folder / oa::platform::path_from_utf8(path), bytes, error))
            return std::nullopt;
        installed.files.emplace_back(path, std::move(bytes));
    }
    return installed;
}

bool resolve_seat(
    const InstalledBrain& installed,
    const BrainSeatChoice& choice,
    ResolvedSeat& out,
    std::string& error
) {
    for (const auto& named : choice.options) {
        if (named.first.empty()) {
            error = "an option has no id";
            return false;
        }
        if (option_named(installed.manifest, named.first) == nullptr) {
            error = "unknown option " + named.first;
            return false;
        }
    }

    const oabrain::Level* level = nullptr;
    if (choice.level.empty()) {
        if (installed.manifest.levels.empty()) {
            error = "the brain has no level";
            return false;
        }
        level = &installed.manifest.levels.front();
    } else {
        level = oabrain::find_level(installed.manifest, choice.level);
        if (level == nullptr) {
            error = "unknown level " + choice.level;
            return false;
        }
    }

    ResolvedSeat resolved;
    resolved.level = level->id;
    resolved.difficulty = income_of(level->difficulty);
    resolved.version = oamut::version_text(installed.manifest.version);
    resolved.full_awareness = installed.manifest.full_awareness;
    resolved.options.reserve(installed.manifest.options.size());
    for (const oamut::Option& option : installed.manifest.options) {
        ResolvedOption made;
        made.id = option.id;
        apply_default(option, made);
        std::string text;
        if (player_text(choice, option.id, text) && !apply_text(option, text, made, error))
            return false;
        resolved.options.push_back(std::move(made));
    }
    resolved.networks.reserve(installed.manifest.networks.size());
    for (const oabrain::Network& network : installed.manifest.networks) {
        if (!oa::base::sha256::parse_hex(network.sha256)) {
            error = "networks." + network.id + ".sha256 is not a digest";
            return false;
        }
        resolved.networks.push_back({network.id, network.file, network.sha256});
    }
    out = std::move(resolved);
    return true;
}

oa::sim::brains::SeatSpec seat_spec(
    uint8_t player,
    const InstalledBrain& installed,
    const ResolvedSeat& resolved,
    std::vector<oa::sim::brains::PackageFile>& files,
    std::vector<oa::sim::brains::OptionValue>& options,
    std::vector<oa::sim::brains::NetworkFile>& networks
) {
    files.clear();
    options.clear();
    networks.clear();
    files.reserve(installed.files.size());
    for (const auto& held : installed.files)
        files.push_back({held.first, held.second});
    options.reserve(resolved.options.size());
    for (const ResolvedOption& option : resolved.options)
        options.push_back({option.id, option.kind, option.toggle, option.number, option.text});
    networks.reserve(resolved.networks.size());
    for (const ResolvedNetwork& network : resolved.networks) {
        oa::base::sha256::Digest digest{};
        if (const auto parsed = oa::base::sha256::parse_hex(network.sha256))
            digest = *parsed;
        networks.push_back({network.id, network.path, digest});
    }

    oa::sim::brains::SeatSpec spec;
    spec.player = player;
    spec.id = installed.manifest.id;
    spec.name = installed.manifest.name;
    spec.version = resolved.version;
    spec.entry = installed.manifest.entry;
    spec.level = resolved.level;
    spec.difficulty = resolved.difficulty;
    spec.full_awareness = resolved.full_awareness;
    spec.options = options;
    spec.files = files;
    spec.networks = networks;
    spec.bank = oa::data::script_host::Bank{
        oa::data::script_host::default_tick_allowance,
        oa::data::script_host::default_bank_ticks,
        oa::data::script_host::default_call_time_limit
    };
    return spec;
}

namespace {

constexpr std::size_t log_bytes = std::size_t{1} << 20;
constexpr std::string_view log_full = "the log is full";

std::string replace_token(std::string_view text, std::string_view token, std::string_view value) {
    std::string out;
    out.reserve(text.size() + value.size());
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t at = text.find(token, start);
        if (at == std::string_view::npos) {
            out.append(text.substr(start));
            break;
        }
        out.append(text.substr(start, at - start));
        out.append(value);
        start = at + token.size();
    }
    return out;
}

std::string_view cut_to_character(std::string_view text, std::size_t limit) {
    if (text.size() <= limit)
        return text;
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
        --end;
    return text.substr(0, end);
}

} // namespace

std::string brain_notice(std::string_view words, std::string_view brain, std::string_view reason) {
    const std::string_view pattern = oa::data::languages::interface_text(words);
    const std::string_view cut = cut_to_character(reason, 160);
    return replace_token(replace_token(pattern, "{brain}", brain), "{reason}", cut);
}

void BrainLog::begin(
    const std::filesystem::path& data_directory,
    uint8_t player,
    std::string_view name,
    std::string_view version,
    std::string_view id,
    std::string_view level,
    uint32_t seed
) {
    out_.reset();
    path_.clear();
    bytes_ = 0;
    full_ = false;
    failed_ = false;
    reported_ = false;
    player_ = player;
    name_ = std::string(name);
    try {
        path_ = data_directory / "logs" / "brains" /
                ("seat-" + std::to_string(static_cast<int>(player) + 1) + ".log");
        std::filesystem::create_directories(path_.parent_path());
        out_ = std::make_unique<std::ofstream>(path_, std::ios::binary | std::ios::trunc);
        if (!*out_) {
            failed_ = true;
            report_unwritable();
            return;
        }
        std::string header;
        header += name;
        header += ' ';
        header += version;
        header += " (";
        header += id;
        header += "), level ";
        header += level;
        header += ", seat ";
        header += std::to_string(static_cast<int>(player) + 1);
        header += ", match seed ";
        header += std::to_string(seed);
        write_line(header);
    } catch (const std::exception&) {
        failed_ = true;
        report_unwritable();
    }
}

void BrainLog::line(uint32_t tick, std::string_view text) {
    std::string row = "tick ";
    row += std::to_string(tick);
    row += ": ";
    row += text;
    write_line(row);
}

void BrainLog::stop(uint32_t tick, const oa::data::script_host::ScriptError& error) {
    std::string row = "tick ";
    row += std::to_string(tick);
    row += ": stopped: ";
    row += oa::data::script_host::describe(error);
    write_line(row);
    std::string_view rest = error.traceback;
    while (!rest.empty()) {
        const std::size_t at = rest.find('\n');
        const std::string_view piece = at == std::string_view::npos ? rest : rest.substr(0, at);
        if (!piece.empty())
            write_line(piece);
        if (at == std::string_view::npos)
            break;
        rest.remove_prefix(at + 1);
    }
}

void BrainLog::stop_text(uint32_t tick, std::string_view reason) {
    std::string row = "tick ";
    row += std::to_string(tick);
    row += ": stopped: ";
    row += reason;
    write_line(row);
}

void BrainLog::fail(uint8_t player) {
    player_ = player;
    failed_ = true;
    out_.reset();
    report_unwritable();
}

const std::filesystem::path& BrainLog::path() const noexcept {
    return path_;
}

std::string_view BrainLog::name() const noexcept {
    return name_;
}

bool BrainLog::open() const noexcept {
    return out_ != nullptr && out_->is_open() && !failed_ && !full_;
}

void BrainLog::write_line(std::string_view text) {
    if (failed_ || full_ || out_ == nullptr || !out_->is_open())
        return;
    std::ofstream& out = *out_;
    const std::size_t closing = log_full.size() + 1;
    if (bytes_ >= log_bytes || closing > log_bytes - bytes_) {
        full_ = true;
        return;
    }
    const std::size_t room = log_bytes - closing - bytes_;
    const std::size_t need = text.size() + 1;
    if (need > room) {
        out.write(log_full.data(), static_cast<std::streamsize>(log_full.size()));
        out.put('\n');
        bytes_ += closing;
        full_ = true;
        out.flush();
        if (!out)
            report_unwritable();
        return;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.put('\n');
    bytes_ += need;
    if (!out) {
        report_unwritable();
        return;
    }
    out.flush();
}

void BrainLog::report_unwritable() {
    if (reported_)
        return;
    reported_ = true;
    failed_ = true;
    std::cerr << "open-annihilation: brains: seat " << static_cast<int>(player_)
              << ": the log could not be written\n";
}

} // namespace oa::app
