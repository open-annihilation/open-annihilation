// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The one text of a setup's computer seats. Compact JSON, members in byte
// order, empty members left out. A text that breaks a rule is nothing,
// never a fatal error.

#include "oa/data/brain_library/seats.hpp"

#include "oa/formats/json.hpp"
#include "oa/formats/oabrain/manifest.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::brain_library {
namespace {

/// The most bytes of a seat text, checked before it is parsed.
constexpr std::size_t most_text_bytes = std::size_t{16} * 1024;

/// The most options one seat names.
constexpr std::size_t most_options = 16;

/// The most bytes of a version.
constexpr std::size_t most_version_bytes = 32;

/// The most bytes of an option value.
constexpr std::size_t most_value_bytes = 64;

/// The bytes of a package SHA-256 written in hex.
constexpr std::size_t sha256_hex_bytes = 64;

/// The seat members this text knows, in the order they are written.
enum class Field : uint8_t { brain, level, name, options, package_sha256, place, version };

/// Tells whether a present name is one a manifest may show.
///
/// One to 64 bytes, and no control character. An empty name is left out by
/// the writer, so a present empty name is not this text.
///
/// @param text the name
/// @return true when it may be stored
bool display_name(std::string_view text) noexcept {
    if (text.empty() || text.size() > oa::formats::oabrain::most_name_bytes)
        return false;
    for (const char byte : text) {
        const auto value = static_cast<unsigned char>(byte);
        if (value < 0x20 || value == 0x7f)
            return false;
    }
    return true;
}

/// Tells whether a byte is a lower-case hex digit.
///
/// @param byte the byte
/// @return true when it is
bool lower_hex(char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
}

/// Tells whether text is 64 lower-case hex digits.
///
/// @param text the text
/// @return true when it is
bool package_sha256(std::string_view text) noexcept {
    if (text.size() != sha256_hex_bytes)
        return false;
    for (const char byte : text)
        if (!lower_hex(byte))
            return false;
    return true;
}

/// The known member of that name, or nothing.
///
/// @param name the member's name
/// @param[out] field the member, when it is known
/// @return true when the name is a known member
bool known_field(std::string_view name, Field& field) noexcept {
    if (name == "brain") {
        field = Field::brain;
        return true;
    }
    if (name == "level") {
        field = Field::level;
        return true;
    }
    if (name == "name") {
        field = Field::name;
        return true;
    }
    if (name == "options") {
        field = Field::options;
        return true;
    }
    if (name == "package_sha256") {
        field = Field::package_sha256;
        return true;
    }
    if (name == "place") {
        field = Field::place;
        return true;
    }
    if (name == "version") {
        field = Field::version;
        return true;
    }
    return false;
}

/// Reads one seat object.
///
/// @param value the object
/// @param[out] seat the seat
/// @return true when every rule held
bool read_seat(const oa::formats::json::Json& value, Seat& seat) {
    if (value.type() != oa::formats::json::JsonType::object)
        return false;
    const auto names = value.names();
    const auto values = value.values();
    if (names.size() != values.size())
        return false;
    bool seen[7] = {};
    bool has_place = false;
    for (std::size_t index = 0; index < names.size(); ++index) {
        Field field{};
        if (!known_field(names[index], field))
            continue;
        const auto seen_at = static_cast<std::size_t>(field);
        if (seen[seen_at])
            return false;
        seen[seen_at] = true;
        const oa::formats::json::Json& member = values[index];
        switch (field) {
        case Field::brain: {
            const std::string* text = member.string();
            if (text == nullptr || !oa::formats::oabrain::brain_id(*text))
                return false;
            seat.brain = *text;
            break;
        }
        case Field::level: {
            const std::string* text = member.string();
            if (text == nullptr || !oa::formats::oabrain::brain_id(*text))
                return false;
            seat.level = *text;
            break;
        }
        case Field::name: {
            const std::string* text = member.string();
            if (text == nullptr || !display_name(*text))
                return false;
            seat.name = *text;
            break;
        }
        case Field::package_sha256: {
            const std::string* text = member.string();
            if (text == nullptr || !package_sha256(*text))
                return false;
            seat.package_sha256 = *text;
            break;
        }
        case Field::version: {
            const std::string* text = member.string();
            if (text == nullptr || text->size() > most_version_bytes)
                return false;
            seat.version = *text;
            break;
        }
        case Field::place: {
            const std::optional<int64_t> place = member.integer();
            if (!place || *place < 0 || *place > 9)
                return false;
            seat.place = static_cast<int32_t>(*place);
            has_place = true;
            break;
        }
        case Field::options: {
            if (member.type() != oa::formats::json::JsonType::object)
                return false;
            const auto option_names = member.names();
            const auto option_values = member.values();
            if (option_names.size() != option_values.size() || option_names.size() > most_options)
                return false;
            seat.options.reserve(option_names.size());
            for (std::size_t option = 0; option < option_names.size(); ++option) {
                const std::string& id = option_names[option];
                if (!oa::formats::oabrain::brain_id(id))
                    return false;
                for (const auto& earlier : seat.options)
                    if (earlier.first == id)
                        return false;
                const std::string* option_text = option_values[option].string();
                if (option_text == nullptr || option_text->size() > most_value_bytes)
                    return false;
                seat.options.emplace_back(id, *option_text);
            }
            break;
        }
        }
    }
    return has_place;
}

/// Writes one string member when it is not empty.
///
/// @param[in,out] writer the text
/// @param name the member's name
/// @param text the value
void write_text(
    oa::formats::json::JsonWriter& writer, std::string_view name, std::string_view text
) {
    if (text.empty())
        return;
    writer.key(name);
    writer.string(text);
}

/// Writes one seat, members in byte order.
///
/// @param[in,out] writer the text
/// @param seat the seat
void write_seat(oa::formats::json::JsonWriter& writer, const Seat& seat) {
    writer.begin_object();
    write_text(writer, "brain", seat.brain);
    write_text(writer, "level", seat.level);
    write_text(writer, "name", seat.name);
    if (!seat.options.empty()) {
        writer.key("options");
        writer.begin_object();
        for (const auto& option : seat.options) {
            writer.key(option.first);
            writer.string(option.second);
        }
        writer.end_object();
    }
    write_text(writer, "package_sha256", seat.package_sha256);
    writer.key("place");
    writer.integer(seat.place);
    write_text(writer, "version", seat.version);
    writer.end_object();
}

} // namespace

std::string seats_text(const Seats& seats) {
    std::vector<const Seat*> order;
    order.reserve(seats.seats.size());
    for (const Seat& seat : seats.seats)
        order.push_back(&seat);
    std::stable_sort(order.begin(), order.end(), [](const Seat* left, const Seat* right) {
        return left->place < right->place;
    });
    oa::formats::json::JsonWriter writer;
    writer.begin_object();
    writer.key("seats");
    writer.begin_array();
    for (const Seat* seat : order)
        write_seat(writer, *seat);
    writer.end_array();
    writer.end_object();
    return writer.text();
}

std::optional<Seats> parse_seats(std::string_view text) {
    if (text.size() > most_text_bytes)
        return std::nullopt;
    try {
        oa::formats::json::JsonError error;
        const std::optional<oa::formats::json::Json> parsed =
            oa::formats::json::parse_json(text, error);
        if (!parsed || parsed->type() != oa::formats::json::JsonType::object)
            return std::nullopt;
        const auto names = parsed->names();
        const auto values = parsed->values();
        if (names.size() != values.size())
            return std::nullopt;
        const oa::formats::json::Json* seats = nullptr;
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (names[index] != "seats")
                continue;
            if (seats != nullptr)
                return std::nullopt;
            seats = &values[index];
        }
        if (seats == nullptr || seats->type() != oa::formats::json::JsonType::array)
            return std::nullopt;
        const auto elements = seats->elements();
        if (elements.size() > most_seats)
            return std::nullopt;
        Seats read;
        read.seats.reserve(elements.size());
        bool used[most_seats] = {};
        for (const oa::formats::json::Json& element : elements) {
            Seat seat;
            if (!read_seat(element, seat))
                return std::nullopt;
            const auto place = static_cast<std::size_t>(seat.place);
            if (place >= most_seats || used[place])
                return std::nullopt;
            used[place] = true;
            read.seats.push_back(std::move(seat));
        }
        return read;
    } catch (const std::exception&) {
        return std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

std::size_t default_level(std::span<const Difficulty> levels, Difficulty wanted) noexcept {
    if (levels.empty())
        return 0;
    for (std::size_t index = 0; index < levels.size(); ++index)
        if (levels[index] == wanted)
            return index;
    return levels.size() - 1;
}

} // namespace oa::data::brain_library
