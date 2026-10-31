// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The text of one --mutator value, and the installed package it names.
#include "oa/data/mutators/choice.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace {

namespace fs = std::filesystem;
namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;

/// Stores a refusal and returns nothing.
///
/// @param error where the refusal is stored; null discards it
/// @param message the refusal
/// @return nothing
std::optional<MutatorChoice> refuse_choice(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
    return std::nullopt;
}

/// Stores a refusal and returns nothing.
///
/// @param error where the refusal is stored; null discards it
/// @param message the refusal
/// @return nothing
std::optional<ResolvedMutator> refuse_resolved(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
    return std::nullopt;
}

/// Quotes a player's text.
///
/// @param text the text
/// @return the text in double quotes
std::string in_quotes(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

/// Reads the options clause of a choice.
///
/// @param options_text `name=value` pairs joined by `,`
/// @param[in,out] choice receives the pairs
/// @param[out] error the refusal, when a pair is not one
/// @return true when every pair was read
bool read_options(std::string_view options_text, MutatorChoice& choice, std::string* error) {
    std::size_t start = 0;
    while (start <= options_text.size()) {
        if (start == options_text.size())
            return refuse_choice(error, "an option is not name=value").has_value();
        const std::size_t comma = options_text.find(',', start);
        const std::string_view piece = options_text.substr(
            start, comma == std::string_view::npos ? std::string_view::npos : comma - start
        );
        const std::size_t equals = piece.find('=');
        if (piece.empty() || equals == std::string_view::npos ||
            piece.find('=', equals + 1) != std::string_view::npos) {
            return refuse_choice(error, "an option is not name=value").has_value();
        }
        const std::string_view name = piece.substr(0, equals);
        const std::string_view value = piece.substr(equals + 1);
        if (name.empty())
            return refuse_choice(error, "an option needs a name").has_value();
        if (value.empty())
            return refuse_choice(error, "an option needs a value").has_value();
        if (!oamut::option_id(name) || !oamut::choice_value_text(value))
            return refuse_choice(error, "an option is not name=value").has_value();
        for (const auto& held : choice.options) {
            if (held.first == name)
                return refuse_choice(error, "option " + std::string(name) + " is repeated")
                    .has_value();
        }
        choice.options.emplace_back(std::string(name), std::string(value));
        if (comma == std::string_view::npos)
            return true;
        start = comma + 1;
    }
    return true;
}

/// Bytes of a drive letter and its colon, as `C:` in `C:/Mods/pack`.
constexpr std::size_t drive_bytes = 2;

/// Tells whether a choice starts with a drive letter, a colon and a slash or
/// a backslash, as `C:/Mods/pack` does.
///
/// That colon is part of the path on every platform.
///
/// @param text the whole choice
/// @return true when the text starts with a drive
bool starts_with_drive(std::string_view text) {
    if (text.size() <= drive_bytes)
        return false;
    const char letter = text[0];
    const bool is_letter = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
    return is_letter && text[1] == ':' && (text[2] == '\\' || text[2] == '/');
}

/// The first colon after any drive whose remainder begins with an option
/// name and `=`.
///
/// An empty name still counts, so `:=4` is an options clause and not part of
/// the target. A name that is not an option id does not, so a path may hold
/// `=` without being read as options.
///
/// @param text the whole choice
/// @return the colon's index, or `npos` when the text names no options
std::size_t options_colon(std::string_view text) {
    const std::size_t first = starts_with_drive(text) ? drive_bytes : 0;
    for (std::size_t index = first; index < text.size(); ++index) {
        if (text[index] != ':')
            continue;
        const std::string_view remainder = text.substr(index + 1);
        const std::size_t equals = remainder.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::size_t comma = remainder.find(',');
        if (comma != std::string_view::npos && comma < equals)
            continue;
        const std::string_view name = remainder.substr(0, equals);
        if (!name.empty() && !oamut::option_id(name))
            continue;
        return index;
    }
    return std::string_view::npos;
}

/// Tells whether a target names a folder or a file that is already there.
///
/// @param target the player's text
/// @return true when that path is a directory or a regular file
bool existing_entry(std::string_view target) {
    std::error_code failure;
    const fs::file_status found = fs::status(oa::platform::path_from_utf8(target), failure);
    return !failure && (fs::is_directory(found) || fs::is_regular_file(found));
}

/// An unsigned decimal, most significant digit first, with no leading zero.
struct Magnitude {
    /// The digits. "0" when the number is zero.
    std::string digits{"0"};
};

/// The digits of a significand scaled by a power of ten.
///
/// @param significand the digits
/// @param extra_zeros how many zeros follow them
/// @return the scaled magnitude
Magnitude magnitude_of(uint64_t significand, int32_t extra_zeros) {
    Magnitude magnitude;
    if (significand == 0)
        return magnitude;
    magnitude.digits = std::to_string(significand);
    if (extra_zeros > 0)
        magnitude.digits.append(static_cast<std::size_t>(extra_zeros), '0');
    return magnitude;
}

/// Compares two magnitudes.
///
/// @param left the first
/// @param right the second
/// @return negative, zero or positive as `left` is below, equal to or above `right`
int compare_magnitude(const Magnitude& left, const Magnitude& right) {
    if (left.digits.size() != right.digits.size())
        return left.digits.size() < right.digits.size() ? -1 : 1;
    if (left.digits == right.digits)
        return 0;
    return left.digits < right.digits ? -1 : 1;
}

/// Adds two magnitudes.
///
/// @param left the first
/// @param right the second
/// @return the sum
Magnitude add_magnitude(const Magnitude& left, const Magnitude& right) {
    const std::string& first = left.digits;
    const std::string& second = right.digits;
    std::string summed;
    int carry = 0;
    std::size_t left_at = first.size();
    std::size_t right_at = second.size();
    while (left_at > 0 || right_at > 0 || carry != 0) {
        int digit = carry;
        if (left_at > 0)
            digit += first[--left_at] - '0';
        if (right_at > 0)
            digit += second[--right_at] - '0';
        summed.push_back(static_cast<char>('0' + digit % 10));
        carry = digit / 10;
    }
    std::reverse(summed.begin(), summed.end());
    return Magnitude{std::move(summed)};
}

/// Subtracts the second magnitude from the first, which is at least as large.
///
/// @param left the larger magnitude
/// @param right the smaller magnitude
/// @return the difference
Magnitude subtract_magnitude(const Magnitude& left, const Magnitude& right) {
    const std::string& first = left.digits;
    const std::string& second = right.digits;
    std::string difference(first.size(), '0');
    int borrow = 0;
    for (std::size_t index = 0; index < first.size(); ++index) {
        const std::size_t at = first.size() - 1 - index;
        int digit = (first[at] - '0') - borrow;
        const int take = index < second.size() ? second[second.size() - 1 - index] - '0' : 0;
        if (digit < take) {
            digit += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        difference[at] = static_cast<char>('0' + (digit - take));
    }
    const std::size_t start = difference.find_first_not_of('0');
    if (start == std::string::npos)
        return Magnitude{};
    return Magnitude{difference.substr(start)};
}

/// A manifest number scaled to one power of ten.
struct ScaledNumber {
    /// True when the number is below zero.
    bool negative{false};
    /// The absolute value, in units of that power of ten.
    Magnitude magnitude{};
};

/// Scales a number to `floor_exponent`.
///
/// @param number the number
/// @param floor_exponent the power of ten, at most the number's own
/// @return the scaled number
ScaledNumber scale_to(const oamod::Number& number, int32_t floor_exponent) {
    ScaledNumber scaled;
    scaled.negative = number.negative && number.significand != 0;
    const int32_t extra = number.exponent - floor_exponent;
    scaled.magnitude = magnitude_of(number.significand, extra > 0 ? extra : 0);
    return scaled;
}

/// The positive difference of two scaled numbers, the first at least the second.
///
/// @param high the larger number
/// @param low the smaller number
/// @return the difference
Magnitude positive_difference(const ScaledNumber& high, const ScaledNumber& low) {
    if (high.negative == low.negative) {
        if (!high.negative)
            return subtract_magnitude(high.magnitude, low.magnitude);
        return subtract_magnitude(low.magnitude, high.magnitude);
    }
    return add_magnitude(high.magnitude, low.magnitude);
}

/// Tells whether a span is a whole number of steps, including none.
///
/// A number option's range holds at most 1000 steps, so a value inside the
/// range needs no more subtractions than that. The arithmetic is decimal, so
/// a step of 1000 refuses 1234 and accepts 2000.
///
/// @param span the distance above the minimum
/// @param step one step, above zero
/// @return true when `span` is a multiple of `step`
bool whole_steps(Magnitude span, const Magnitude& step) {
    if (step.digits == "0")
        return false;
    for (int taken = 0; taken <= 1000; ++taken) {
        const int order = compare_magnitude(span, step);
        if (order < 0)
            return span.digits == "0";
        if (order == 0)
            return true;
        span = subtract_magnitude(span, step);
    }
    return false;
}

/// Tells whether a number lies on a step from a minimum.
///
/// @param value the number
/// @param minimum the first step
/// @param step the distance between steps
/// @return true when `value` is `minimum` plus a whole number of steps
bool lies_on_step(
    const oamod::Number& value, const oamod::Number& minimum, const oamod::Number& step
) {
    int32_t floor = value.exponent;
    if (minimum.exponent < floor)
        floor = minimum.exponent;
    if (step.exponent < floor)
        floor = step.exponent;
    const ScaledNumber scaled_value = scale_to(value, floor);
    const ScaledNumber scaled_minimum = scale_to(minimum, floor);
    const ScaledNumber scaled_step = scale_to(step, floor);
    return whole_steps(positive_difference(scaled_value, scaled_minimum), scaled_step.magnitude);
}

/// The refusal for one option, `id: name: ...`.
///
/// @param id the mutator's id
/// @param name the option's id
/// @param text the player's text
/// @param reason what is wrong with it
/// @return the refusal
std::string option_refusal(
    std::string_view id, std::string_view name, std::string_view text, std::string_view reason
) {
    return std::string(id) + ": " + std::string(name) + ": " + std::string(text) + " " +
           std::string(reason);
}

/// Writes a player's value over one option.
///
/// @param id the mutator's id
/// @param option the manifest option
/// @param text the player's text
/// @param[in,out] made the default, replaced when the text is that option's kind
/// @param[out] error the refusal, when the text is not
/// @return true when the value was written
bool apply_player_value(
    std::string_view id,
    const oamut::Option& option,
    std::string_view text,
    LoadoutOption& made,
    std::string* error
) {
    if (option.type == oamut::OptionType::toggle) {
        if (text != "true" && text != "false") {
            if (error != nullptr)
                *error = option_refusal(id, option.id, text, "is not true or false");
            return false;
        }
        made.toggle = text == "true";
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
            made.value = listed;
            return true;
        }
        std::string listed_text;
        for (const oamut::ChoiceValue& listed : option.values) {
            if (!listed_text.empty())
                listed_text += ", ";
            listed_text += listed.text;
        }
        if (error != nullptr)
            *error = option_refusal(id, option.id, text, "is not one of " + listed_text);
        return false;
    }
    oamod::Number parsed{};
    if (oamod::parse_number(text, parsed) != oamod::NumberStatus::ok) {
        if (error != nullptr)
            *error = option_refusal(id, option.id, text, "is not a number");
        return false;
    }
    if (oamod::compare_numbers(parsed, option.minimum) < 0 ||
        oamod::compare_numbers(parsed, option.maximum) > 0) {
        if (error != nullptr) {
            *error = option_refusal(
                id,
                option.id,
                text,
                "is outside " + oamod::canonical_number_text(option.minimum) + " to " +
                    oamod::canonical_number_text(option.maximum)
            );
        }
        return false;
    }
    if (!lies_on_step(parsed, option.minimum, option.step)) {
        if (error != nullptr) {
            *error = option_refusal(
                id,
                option.id,
                text,
                "does not lie on a step of " + oamod::canonical_number_text(option.step) +
                    " from " + oamod::canonical_number_text(option.minimum)
            );
        }
        return false;
    }
    made.value.number = true;
    made.value.number_value = parsed;
    made.value.text = oamod::canonical_number_text(parsed);
    return true;
}

/// Types every option, defaults first and the player's values over them.
///
/// @param manifest the manifest
/// @param named the player's settings
/// @param[out] typed the options, in the manifest's order
/// @param[out] error the refusal, when a name or a value is not one of the manifest
/// @return true when every named option was typed
bool type_options(
    const oamut::Manifest& manifest,
    const std::vector<std::pair<std::string, std::string>>& named,
    std::vector<LoadoutOption>& typed,
    std::string* error
) {
    for (const auto& one : named) {
        bool known = false;
        for (const oamut::Option& option : manifest.options) {
            if (option.id == one.first)
                known = true;
        }
        if (!known) {
            if (error != nullptr)
                *error = manifest.id + " has no option " + one.first;
            return false;
        }
    }
    typed.clear();
    typed.reserve(manifest.options.size());
    for (const oamut::Option& option : manifest.options) {
        LoadoutOption made = default_option(option);
        const std::string* text = nullptr;
        for (const auto& one : named) {
            if (one.first == option.id)
                text = &one.second;
        }
        if (text != nullptr && !apply_player_value(manifest.id, option, *text, made, error))
            return false;
        typed.push_back(std::move(made));
    }
    return true;
}

/// A path's filename in UTF-8, with ASCII letters lowered.
///
/// @param path the path
/// @return the filename
std::string folded_filename(const fs::path& path) {
    const std::u8string text = path.filename().generic_u8string();
    std::string folded;
    folded.reserve(text.size());
    for (const char8_t byte : text) {
        const auto raw = static_cast<unsigned char>(byte);
        const unsigned char lower =
            raw >= 'A' && raw <= 'Z' ? static_cast<unsigned char>(raw - 'A' + 'a') : raw;
        folded.push_back(static_cast<char>(lower));
    }
    return folded;
}

/// How a bounded read ended.
enum class Bound : uint8_t {
    read,       ///< the file was read, and it fits
    too_large,  ///< the file is larger than the bound
    unreadable, ///< the file did not open
};

/// Reads a file up to one byte past a bound.
///
/// @param file the file
/// @param most the most bytes that still count as read
/// @param[out] bytes the file, when it fits
/// @return how the read ended
Bound read_bounded(const fs::path& file, std::size_t most, std::vector<uint8_t>& bytes) {
    std::error_code size_error;
    const uintmax_t size = fs::file_size(file, size_error);
    if (!size_error && size > most)
        return Bound::too_large;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return Bound::unreadable;
    bytes.clear();
    for (std::istreambuf_iterator<char> at{in}, stop; at != stop && bytes.size() <= most; ++at) {
        bytes.push_back(static_cast<uint8_t>(static_cast<unsigned char>(*at)));
    }
    if (!in && !in.eof())
        return Bound::unreadable;
    if (bytes.size() > most)
        return Bound::too_large;
    return Bound::read;
}

/// Finds `oamut.yaml` in a folder, without ASCII case, skipping links.
///
/// The folder itself is not followed when it is a link.
///
/// @param folder the package folder
/// @return the manifest file, or nothing when the folder has none
std::optional<fs::path> find_manifest(const fs::path& folder) {
    std::error_code status_error;
    const fs::file_status status = fs::symlink_status(folder, status_error);
    if (status_error || fs::is_symlink(status) || !fs::is_directory(status))
        return std::nullopt;
    std::error_code list_error;
    fs::directory_iterator cursor(folder, list_error);
    if (list_error)
        return std::nullopt;
    const fs::directory_iterator end;
    for (; cursor != end; cursor.increment(list_error)) {
        if (list_error)
            return std::nullopt;
        std::error_code entry_error;
        const fs::file_status entry_status = fs::symlink_status(cursor->path(), entry_error);
        if (entry_error || fs::is_symlink(entry_status) || !fs::is_regular_file(entry_status))
            continue;
        if (folded_filename(cursor->path()) == oamut::manifest_file)
            return cursor->path();
    }
    return std::nullopt;
}

/// Reads a package folder's manifest.
///
/// @param folder the package folder
/// @param shown the player's text for the folder, quoted in a refusal
/// @param[out] manifest the manifest
/// @param[out] error the refusal, when the folder is not a package
/// @return true when the manifest was read
bool read_package(
    const fs::path& folder, std::string_view shown, oamut::Manifest& manifest, std::string* error
) {
    const std::optional<fs::path> manifest_path = find_manifest(folder);
    if (!manifest_path) {
        if (error != nullptr)
            *error = in_quotes(shown) + " is not a mutator package";
        return false;
    }
    std::vector<uint8_t> bytes;
    const Bound ended = read_bounded(*manifest_path, oamod::max_input_bytes, bytes);
    if (ended == Bound::too_large) {
        if (error != nullptr)
            *error = in_quotes(shown) + ": oamut.yaml is larger than 256 KiB";
        return false;
    }
    if (ended != Bound::read) {
        if (error != nullptr)
            *error = in_quotes(shown) + " is not a mutator package";
        return false;
    }
    std::vector<oamut::Problem> problems;
    if (!oamut::read_manifest(bytes, manifest, problems)) {
        if (error != nullptr) {
            *error = problems.empty() ? in_quotes(shown) + " is not a mutator package"
                                      : in_quotes(shown) + ": " + oamut::describe(problems.front());
        }
        return false;
    }
    return true;
}

} // namespace

std::optional<MutatorChoice> parse_choice(std::string_view text, std::string* error) {
    if (text.empty())
        return refuse_choice(error, "a mutator needs an id or a path");
    std::string_view target = text;
    std::string_view options_text;
    const std::size_t colon = options_colon(text);
    if (colon != std::string_view::npos) {
        target = text.substr(0, colon);
        options_text = text.substr(colon + 1);
    }
    if (target.empty())
        return refuse_choice(error, "a mutator needs an id or a path");
    // A drive's colon is part of the path, so only a later one needs the
    // entry to be there.
    const std::size_t after_drive = starts_with_drive(target) ? drive_bytes : 0;
    const bool colon_in_target = target.find(':', after_drive) != std::string_view::npos;
    if (colon_in_target && !existing_entry(target))
        return refuse_choice(error, in_quotes(target) + " is not an id or a path");
    MutatorChoice choice;
    choice.target = std::string(target);
    const bool slash =
        target.find('/') != std::string_view::npos || target.find('\\') != std::string_view::npos;
    choice.by_path = slash || colon_in_target;
    if (!options_text.empty() && !read_options(options_text, choice, error))
        return std::nullopt;
    return choice;
}

std::string format_choice(const MutatorChoice& choice) {
    std::string text = choice.target;
    if (choice.options.empty())
        return text;
    text.push_back(':');
    for (std::size_t index = 0; index < choice.options.size(); ++index) {
        if (index != 0)
            text.push_back(',');
        text += choice.options[index].first;
        text.push_back('=');
        text += choice.options[index].second;
    }
    return text;
}

std::optional<ResolvedMutator> resolve_installed(
    const MutatorChoice& choice,
    const oa::data::mutator_library::InstalledMutator& installed,
    std::string* error,
    FolderDigests* digests
) {
    ResolvedMutator resolved;
    resolved.choice = choice;
    resolved.package_folder = installed.folder;
    resolved.manifest = installed.manifest;
    if (!installed.package_sha256.empty()) {
        const std::optional<base::sha256::Digest> digest =
            base::sha256::parse_hex(installed.package_sha256);
        if (!digest) {
            return refuse_resolved(
                error,
                in_quotes(choice.target) +
                    " records a package SHA-256 that is not 64 hexadecimal digits"
            );
        }
        resolved.package = *digest;
    } else {
        std::string digest_error;
        const std::optional<base::sha256::Digest> digest =
            digests != nullptr ? digests->digest(resolved.package_folder, &digest_error)
                               : folder_digest(resolved.package_folder, &digest_error);
        if (!digest) {
            return refuse_resolved(
                error,
                digest_error.empty() ? in_quotes(choice.target) + " is not a mutator package"
                                     : digest_error
            );
        }
        resolved.package = *digest;
        resolved.folder = true;
    }
    if (!type_options(resolved.manifest, choice.options, resolved.options, error))
        return std::nullopt;
    return resolved;
}

std::optional<ResolvedMutator>
resolve_choice(const MutatorChoice& choice, const fs::path& mutators_folder, std::string* error) {
    // A path is the package folder itself, so the folder is not listed for it.
    if (choice.by_path)
        return resolve_choice(choice, oa::data::mutator_library::Listing{}, error);
    return resolve_choice(
        choice, oa::data::mutator_library::list_installed(mutators_folder), error
    );
}

std::optional<ResolvedMutator> resolve_choice(
    const MutatorChoice& choice,
    const oa::data::mutator_library::Listing& listing,
    std::string* error
) {
    if (choice.by_path) {
        ResolvedMutator resolved;
        resolved.choice = choice;
        resolved.package_folder = oa::platform::path_from_utf8(choice.target);
        if (!read_package(resolved.package_folder, choice.target, resolved.manifest, error))
            return std::nullopt;
        std::string digest_error;
        const std::optional<base::sha256::Digest> digest =
            folder_digest(resolved.package_folder, &digest_error);
        if (!digest) {
            return refuse_resolved(
                error,
                digest_error.empty() ? in_quotes(choice.target) + " is not a mutator package"
                                     : digest_error
            );
        }
        resolved.package = *digest;
        resolved.folder = true;
        if (!type_options(resolved.manifest, choice.options, resolved.options, error))
            return std::nullopt;
        return resolved;
    }
    const oa::data::mutator_library::InstalledMutator* installed =
        oa::data::mutator_library::find(listing, choice.target);
    if (installed == nullptr)
        return refuse_resolved(error, in_quotes(choice.target) + " is not an installed mutator");
    return resolve_installed(choice, *installed, error);
}

} // namespace oa::data::mutators
