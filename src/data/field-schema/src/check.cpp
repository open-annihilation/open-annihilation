// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Range checks and the values the loaders read, including a key a file leaves out.
#include "oa/data/field_schema.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace oa::data::field_schema {
namespace {

constexpr double k_fixed_edge = 32767.99998;
constexpr double k_huge_integer = 1e20;
constexpr double k_velocity_scale = 65536.0 / 30.0;
constexpr double k_acceleration_scale = 65536.0 / 900.0;
constexpr double k_turn_scale = 1.0 / 30.0;
constexpr double k_ticks_per_second = 30.0;
constexpr int32_t k_cloaker_distance = 80;

bool numeric_type(FieldType type) noexcept {
    return type == FieldType::integer || type == FieldType::fixed || type == FieldType::decimal ||
           type == FieldType::flag;
}

bool is_c_space(unsigned char character) noexcept {
    return character == ' ' || (character >= '\t' && character <= '\r');
}

void copy_key(char* out, std::size_t size, std::string_view key) noexcept {
    if (size == 0)
        return;
    const std::size_t count = key.size() < size - 1 ? key.size() : size - 1;
    if (count != 0)
        std::memcpy(out, key.data(), count);
    out[count] = '\0';
}

enum class IntScan : uint8_t { none, value, huge };

struct IntRead {
    IntScan kind{};
    double magnitude{};
};

// The digits parse_int would consume, as a magnitude rather than a wrapped 32-bit value.
IntRead scan_int(const char* text) noexcept {
    const auto* at = reinterpret_cast<const unsigned char*>(text);
    while (is_c_space(*at))
        ++at;
    const bool negative = *at == '-';
    if (*at == '-' || *at == '+')
        ++at;
    bool digits = false;
    bool huge = false;
    double magnitude = 0.0;
    while (*at >= '0' && *at <= '9') {
        digits = true;
        const double digit = static_cast<double>(*at - '0');
        if (!huge) {
            if (magnitude > (k_huge_integer - digit) / 10.0)
                huge = true;
            else
                magnitude = magnitude * 10.0 + digit;
        }
        ++at;
    }
    if (!digits)
        return {};
    if (huge)
        return {IntScan::huge, negative ? -k_huge_integer : k_huge_integer};
    return {IntScan::value, negative ? -magnitude : magnitude};
}

bool has_real_digits(const char* text) noexcept {
    const auto* at = reinterpret_cast<const unsigned char*>(text);
    while (is_c_space(*at))
        ++at;
    if (*at == '-' || *at == '+')
        ++at;
    bool digits = false;
    while (*at >= '0' && *at <= '9') {
        digits = true;
        ++at;
    }
    if (*at == '.') {
        ++at;
        while (*at >= '0' && *at <= '9') {
            digits = true;
            ++at;
        }
    }
    return digits;
}

int64_t cast_width(Storage storage, int32_t parsed) noexcept {
    switch (storage) {
    case Storage::int8:
        return static_cast<int8_t>(parsed);
    case Storage::uint8:
        return static_cast<uint8_t>(parsed);
    case Storage::int16:
        return static_cast<int16_t>(parsed);
    case Storage::uint16:
        return static_cast<uint16_t>(parsed);
    case Storage::uint32:
        return static_cast<uint32_t>(parsed);
    default:
        return parsed;
    }
}

void format_number(char* out, std::size_t size, double value) noexcept {
    if (std::isnan(value))
        std::snprintf(out, size, "NaN");
    else if (std::isinf(value))
        std::snprintf(out, size, "%s", value > 0.0 ? "infinity" : "-infinity");
    else if (std::trunc(value) == value && std::fabs(value) < 1e15)
        std::snprintf(out, size, "%.0f", value);
    else
        std::snprintf(out, size, "%.10g", value);
}

Check verdict_reason(Verdict verdict, const char* reason) {
    Check result;
    result.verdict = verdict;
    if (verdict != Verdict::ok)
        result.reason = reason;
    return result;
}

Check bounded(
    Verdict verdict,
    std::string_view key,
    double value,
    const char* relation,
    double limit,
    std::string_view why
) {
    char shown[64];
    char limit_text[64];
    char reason[768];
    format_number(shown, sizeof shown, value);
    format_number(limit_text, sizeof limit_text, limit);
    std::snprintf(
        reason,
        sizeof reason,
        "%.*s %s %s %s; %.*s",
        static_cast<int>(key.size()),
        key.data(),
        shown,
        relation,
        limit_text,
        static_cast<int>(why.size()),
        why.data()
    );
    return verdict_reason(verdict, reason);
}

Check outside_range(
    Verdict verdict,
    std::string_view key,
    double value,
    double low,
    double high,
    std::string_view why
) {
    char shown[64];
    char low_text[64];
    char high_text[64];
    char reason[768];
    format_number(shown, sizeof shown, value);
    format_number(low_text, sizeof low_text, low);
    format_number(high_text, sizeof high_text, high);
    std::snprintf(
        reason,
        sizeof reason,
        "%.*s %s is outside %s to %s; %.*s",
        static_cast<int>(key.size()),
        key.data(),
        shown,
        low_text,
        high_text,
        static_cast<int>(why.size()),
        why.data()
    );
    return verdict_reason(verdict, reason);
}

const char* forbidden_marker(std::string_view text) noexcept {
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == ';')
            return "';'";
        if (character == '\n' || character == '\r')
            return "a line break";
        if (character == '\0')
            return "a zero byte";
        if (index + 1 < text.size() && character == '/' &&
            (text[index + 1] == '/' || text[index + 1] == '*'))
            return text[index + 1] == '/' ? "'//'" : "'/*'";
    }
    return nullptr;
}

std::string printable_snippet(std::string_view text) {
    std::string snippet;
    for (char character : text) {
        if (snippet.size() >= 40) {
            snippet += "...";
            break;
        }
        const auto byte = static_cast<unsigned char>(character);
        snippet += (byte >= 32 && byte < 127) ? character : '?';
    }
    return snippet;
}

bool on_off_bounds(const FieldSpec& spec) noexcept {
    return spec.warn_below == 0.0 && spec.warn_above == 1.0;
}

bool outside_int(double value, double low, double high) noexcept {
    return value < low || value > high;
}

Check storage_warning(const FieldSpec& spec, std::string_view key, double value) {
    const std::string_view why = spec.why;
    switch (spec.storage) {
    case Storage::int8:
        if (outside_int(value, -128.0, 127.0))
            return outside_range(Verdict::warn, key, value, -128.0, 127.0, why);
        break;
    case Storage::uint8:
        if (outside_int(value, 0.0, 255.0))
            return outside_range(Verdict::warn, key, value, 0.0, 255.0, why);
        break;
    case Storage::int16:
        if (outside_int(value, -32768.0, 32767.0))
            return outside_range(Verdict::warn, key, value, -32768.0, 32767.0, why);
        break;
    case Storage::uint16:
        if (outside_int(value, 0.0, 65535.0))
            return outside_range(Verdict::warn, key, value, 0.0, 65535.0, why);
        break;
    case Storage::int32:
        if (outside_int(
                value,
                static_cast<double>(std::numeric_limits<int32_t>::min()),
                static_cast<double>(std::numeric_limits<int32_t>::max())
            ))
            return outside_range(
                Verdict::warn,
                key,
                value,
                static_cast<double>(std::numeric_limits<int32_t>::min()),
                static_cast<double>(std::numeric_limits<int32_t>::max()),
                why
            );
        break;
    case Storage::uint32:
        if (outside_int(value, 0.0, 4294967295.0))
            return outside_range(Verdict::warn, key, value, 0.0, 4294967295.0, why);
        break;
    case Storage::fixed32:
        if (value > k_fixed_edge || value < -k_fixed_edge)
            return outside_range(Verdict::warn, key, value, -k_fixed_edge, k_fixed_edge, why);
        break;
    case Storage::ticks16:
    case Storage::ticks32: {
        const double ticks = std::trunc(value * k_ticks_per_second);
        const double low = spec.storage == Storage::ticks16
                               ? -32768.0
                               : static_cast<double>(std::numeric_limits<int32_t>::min());
        const double high = spec.storage == Storage::ticks16
                                ? 32767.0
                                : static_cast<double>(std::numeric_limits<int32_t>::max());
        if (!std::isfinite(ticks) || ticks < low || ticks > high) {
            char shown[64];
            char tick_text[64];
            char low_text[64];
            char high_text[64];
            char reason[768];
            format_number(shown, sizeof shown, value);
            format_number(tick_text, sizeof tick_text, std::isfinite(ticks) ? ticks : value);
            format_number(low_text, sizeof low_text, low);
            format_number(high_text, sizeof high_text, high);
            std::snprintf(
                reason,
                sizeof reason,
                "%.*s %s makes %s ticks, outside %s to %s; %.*s",
                static_cast<int>(key.size()),
                key.data(),
                shown,
                std::isfinite(ticks) ? tick_text : "a non-finite count",
                low_text,
                high_text,
                static_cast<int>(why.size()),
                why.data()
            );
            return verdict_reason(Verdict::warn, reason);
        }
        break;
    }
    default:
        break;
    }
    return {};
}

Check scaled_warning(
    const FieldSpec& spec, std::string_view key, double value, double scale, double low, double high
) {
    const double scaled = value * scale;
    const double truncated = std::trunc(scaled);
    if (std::isfinite(scaled) && truncated >= low && truncated <= high)
        return {};
    char shown[64];
    char low_text[64];
    char high_text[64];
    char reason[768];
    format_number(shown, sizeof shown, value);
    format_number(low_text, sizeof low_text, low);
    format_number(high_text, sizeof high_text, high);
    std::snprintf(
        reason,
        sizeof reason,
        "%.*s %s scales outside %s to %s; %.*s",
        static_cast<int>(key.size()),
        key.data(),
        shown,
        low_text,
        high_text,
        static_cast<int>(spec.why.size()),
        spec.why.data()
    );
    return verdict_reason(Verdict::warn, reason);
}

Check weapon_scale_warning(const FieldSpec& spec, std::string_view key, double value) {
    if (spec.record != RecordKind::weapon)
        return {};
    if (spec.key == "weaponvelocity" || spec.key == "startvelocity")
        return scaled_warning(
            spec,
            key,
            value,
            k_velocity_scale,
            static_cast<double>(std::numeric_limits<int32_t>::min()),
            static_cast<double>(std::numeric_limits<int32_t>::max())
        );
    if (spec.key == "weaponacceleration")
        return scaled_warning(
            spec,
            key,
            value,
            k_acceleration_scale,
            static_cast<double>(std::numeric_limits<int32_t>::min()),
            static_cast<double>(std::numeric_limits<int32_t>::max())
        );
    if (spec.key == "turnrate")
        return scaled_warning(spec, key, value, k_turn_scale, -32768.0, 32767.0);
    return {};
}

int32_t whole_of_int(int32_t raw) noexcept {
    const double widened = static_cast<double>(static_cast<float>(raw));
    if (!std::isfinite(widened) || widened >= 9223372036854775808.0 ||
        widened < -9223372036854775808.0)
        return static_cast<int32_t>(static_cast<uint64_t>(std::numeric_limits<int64_t>::min()));
    const auto truncated = static_cast<int64_t>(std::trunc(widened));
    return static_cast<int32_t>(static_cast<uint64_t>(truncated));
}

double twice_fixed_number(const oa::formats::tdf::Block* block, const char* from) noexcept {
    const auto fixed = static_cast<uint32_t>(oa::formats::tdf::get_fixed(block, from, 0));
    const auto doubled = static_cast<int32_t>(fixed << 1);
    return static_cast<double>(doubled) / 65536.0;
}

/// The ceiling a weapon id is refused above, or the spec's own ceiling.
///
/// @param spec the field being judged
/// @param weapon_types the weapon types a weapon id is held to
/// @return weapon_types - 1 for a weapon id, otherwise the spec's refuse_above
double refused_above(const FieldSpec& spec, uint32_t weapon_types) noexcept {
    if (spec.record == RecordKind::weapon && spec.key == "id")
        return static_cast<double>(weapon_types) - 1.0;
    return spec.refuse_above;
}

} // namespace

Check check_number(RecordKind record, std::string_view key, double value, uint32_t weapon_types) {
    const FieldSpec* spec = find_field(record, key);
    if (spec == nullptr || !numeric_type(spec->type))
        return {};
    const std::string_view shown_key = spec->key.empty() ? key : spec->key;
    if (!std::isfinite(value)) {
        char reason[768];
        const char* which = std::isnan(value) ? "NaN" : "infinity";
        std::snprintf(
            reason,
            sizeof reason,
            "%.*s %s is not a finite number; a value that is not finite cannot be stored",
            static_cast<int>(shown_key.size()),
            shown_key.data(),
            which
        );
        return verdict_reason(Verdict::refuse, reason);
    }
    const double judged = (spec->type == FieldType::integer || spec->type == FieldType::flag)
                              ? std::trunc(value)
                              : value;
    if (judged < spec->refuse_below)
        return bounded(
            Verdict::refuse, shown_key, value, "is below", spec->refuse_below, spec->why
        );
    const double ceiling = refused_above(*spec, weapon_types);
    if (judged > ceiling)
        return bounded(Verdict::refuse, shown_key, value, "is above", ceiling, spec->why);
    if (on_off_bounds(*spec) && (judged < spec->warn_below || judged > spec->warn_above)) {
        char shown[64];
        char reason[768];
        format_number(shown, sizeof shown, value);
        std::snprintf(
            reason,
            sizeof reason,
            "%.*s %s is not 0 or 1; %.*s",
            static_cast<int>(shown_key.size()),
            shown_key.data(),
            shown,
            static_cast<int>(spec->why.size()),
            spec->why.data()
        );
        return verdict_reason(Verdict::warn, reason);
    }
    if (judged < spec->warn_below)
        return bounded(Verdict::warn, shown_key, value, "is below", spec->warn_below, spec->why);
    if (judged > spec->warn_above)
        return bounded(Verdict::warn, shown_key, value, "is above", spec->warn_above, spec->why);
    if (spec->record == RecordKind::weapon &&
        (spec->key == "weaponvelocity" || spec->key == "startvelocity" ||
         spec->key == "weaponacceleration" || spec->key == "turnrate"))
        return weapon_scale_warning(*spec, shown_key, judged);
    return storage_warning(*spec, shown_key, judged);
}

// Bytes of a number the loader's grammar consumes, or npos when it finds no digit.
// Integer and flag fields stop at the first non-digit, as parse_int does. Fixed
// and decimal fields follow parse_double, including an exponent.
std::size_t consumed_number(FieldType type, const char* text) noexcept {
    const auto* at = reinterpret_cast<const unsigned char*>(text);
    const auto* const start = at;
    while (is_c_space(*at))
        ++at;
    if (*at == '-' || *at == '+')
        ++at;
    bool digits = false;
    while (*at >= '0' && *at <= '9') {
        digits = true;
        ++at;
    }
    if (type == FieldType::integer || type == FieldType::flag) {
        // A fractional part is the whole number the loader keeps, not trailing
        // text: workertime 67.5 stays the integer 67.
        if (!digits)
            return std::string_view::npos;
        if (*at == '.') {
            ++at;
            while (*at >= '0' && *at <= '9')
                ++at;
        }
        return static_cast<std::size_t>(at - start);
    }
    if (*at == '.') {
        ++at;
        while (*at >= '0' && *at <= '9') {
            digits = true;
            ++at;
        }
    }
    if (!digits)
        return std::string_view::npos;
    if (*at == 'e' || *at == 'E' || *at == 'd' || *at == 'D') {
        const unsigned char* exponent = at + 1;
        if (*exponent == '-' || *exponent == '+')
            ++exponent;
        if (*exponent >= '0' && *exponent <= '9') {
            at = exponent;
            while (*at >= '0' && *at <= '9')
                ++at;
        }
    }
    return static_cast<std::size_t>(at - start);
}

Check not_a_number(std::string_view key, std::string_view text) {
    const std::string snippet = printable_snippet(text);
    char reason[768];
    std::snprintf(
        reason,
        sizeof reason,
        "%.*s: \"%s\" is not a number",
        static_cast<int>(key.size()),
        key.data(),
        snippet.c_str()
    );
    return verdict_reason(Verdict::refuse, reason);
}

// A parsed number, plus a warning when characters after it are what the loader skips.
Check judged_number(
    RecordKind record,
    std::string_view key,
    std::string_view shown_key,
    double value,
    std::string_view ignored,
    uint32_t weapon_types
) {
    Check judged = check_number(record, key, value, weapon_types);
    if (ignored.empty() || judged.verdict == Verdict::refuse)
        return judged;
    const std::string snippet = printable_snippet(ignored);
    char reason[768];
    std::snprintf(
        reason,
        sizeof reason,
        "%.*s ignores \"%s\"",
        static_cast<int>(shown_key.size()),
        shown_key.data(),
        snippet.c_str()
    );
    if (judged.verdict == Verdict::warn && !judged.reason.empty()) {
        judged.reason.push_back(';');
        judged.reason.push_back(' ');
        judged.reason += reason;
        return judged;
    }
    return verdict_reason(Verdict::warn, reason);
}

Check check_text(
    RecordKind record, std::string_view key, std::string_view text, uint32_t weapon_types
) {
    const FieldSpec* spec = find_field(record, key);
    if (spec == nullptr)
        return {};
    if (const char* marker = forbidden_marker(text)) {
        const std::string snippet = printable_snippet(text);
        const std::string_view shown_key = spec->key.empty() ? key : spec->key;
        char reason[768];
        std::snprintf(
            reason,
            sizeof reason,
            "%.*s \"%s\" contains %s; a text value cannot hold a semicolon, a line break, a zero "
            "byte, // or /*",
            static_cast<int>(shown_key.size()),
            shown_key.data(),
            snippet.c_str(),
            marker
        );
        return verdict_reason(Verdict::refuse, reason);
    }
    if (!numeric_type(spec->type))
        return {};
    const std::string owned(text);
    const std::size_t used = consumed_number(spec->type, owned.c_str());
    const std::string_view shown_key = spec->key.empty() ? key : spec->key;
    if (used == std::string_view::npos)
        return not_a_number(shown_key, text);
    const std::string_view ignored(owned.data() + used, owned.size() - used);
    if (spec->type == FieldType::integer || spec->type == FieldType::flag) {
        const IntRead scanned = scan_int(owned.c_str());
        return judged_number(record, key, shown_key, scanned.magnitude, ignored, weapon_types);
    }
    return judged_number(
        record, key, shown_key, oa::formats::tdf::parse_double(owned.c_str()), ignored, weapon_types
    );
}

Value read_value(const FieldSpec* spec, const char* text) {
    Value out;
    if (text == nullptr) {
        out.kind = Value::Kind::absent;
        return out;
    }
    if (spec == nullptr || spec->type == FieldType::text || spec->type == FieldType::name_ref ||
        spec->type == FieldType::token_list || spec->type == FieldType::yard_map) {
        out.kind = Value::Kind::text;
        out.text = text;
        return out;
    }
    if (spec->type == FieldType::flag) {
        out.kind = Value::Kind::boolean;
        if (scan_int(text).kind == IntScan::none)
            out.number = 0.0;
        else
            out.boolean = (oa::formats::tdf::parse_int(text) & 1) != 0;
        return out;
    }
    if (spec->type == FieldType::integer) {
        out.kind = Value::Kind::integer;
        if (scan_int(text).kind == IntScan::none) {
            out.number = 0.0;
            out.integer = 0;
        } else {
            out.integer = cast_width(spec->storage, oa::formats::tdf::parse_int(text));
        }
        return out;
    }
    out.kind = Value::Kind::number;
    out.number = has_real_digits(text) ? oa::formats::tdf::parse_double(text) : 0.0;
    return out;
}

Value default_value(RecordKind record, std::string_view key, const oa::formats::tdf::Block* block) {
    const FieldSpec* spec = find_field(record, key);
    if (spec == nullptr)
        return {};
    char from[64];
    copy_key(from, sizeof from, spec->default_from);
    double number = spec->default_number;
    std::string text(spec->default_text);
    switch (spec->default_rule) {
    case DefaultRule::constant:
        break;
    case DefaultRule::twice_fixed:
        number = twice_fixed_number(block, from);
        break;
    case DefaultRule::whole_of:
        number = static_cast<double>(whole_of_int(oa::formats::tdf::get_int(block, from, 0)));
        break;
    case DefaultRule::half_low_byte: {
        const FieldSpec* source = find_field(record, spec->default_from);
        const auto fallback =
            static_cast<int32_t>(source != nullptr ? source->default_number : 0.0);
        const auto read = static_cast<uint32_t>(oa::formats::tdf::get_int(block, from, fallback));
        number = static_cast<double>((read & 0xffu) >> 1);
        break;
    }
    case DefaultRule::text_of: {
        char buffer[256];
        oa::formats::tdf::get_string(block, from, buffer, sizeof buffer, "");
        text = buffer;
        break;
    }
    case DefaultRule::cloaker_distance:
        number = oa::formats::tdf::get_int(block, from, 0) > 0 ? k_cloaker_distance : 0;
        break;
    }
    Value out;
    switch (spec->type) {
    case FieldType::integer:
        out.kind = Value::Kind::integer;
        out.integer = static_cast<int64_t>(number);
        break;
    case FieldType::fixed:
    case FieldType::decimal:
        out.kind = Value::Kind::number;
        out.number = number;
        break;
    case FieldType::flag:
        out.kind = Value::Kind::boolean;
        out.boolean = (static_cast<int64_t>(number) & 1) != 0;
        break;
    default:
        out.kind = Value::Kind::text;
        out.text = std::move(text);
        break;
    }
    return out;
}

} // namespace oa::data::field_schema
