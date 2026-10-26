// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/catalogue/catalogue.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/url.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::catalogue {
namespace {

namespace bundles = oa::data::content_bundles;
namespace json = formats::json;
namespace oabrain = formats::oabrain;
namespace oamod = formats::oamod;
namespace oamut = formats::oamut;
namespace sha = base::sha256;
namespace signing = base::signing;
namespace url = formats::url;

/// The most bytes one package may name. 2^32 itself is allowed.
constexpr int64_t max_package_bytes = int64_t{1} << 32;

/// The highest release number. 2^31-1.
constexpr int64_t max_release = 2147483647;

/// The most mirrors one catalogue names.
constexpr std::size_t max_mirrors = 8;

/// The most ids one supportedGames or requiredExpansions list names.
constexpr std::size_t max_content_ids = 16;

/// The most bytes of a map's title, and of a name under compatible.
constexpr std::size_t max_map_name_bytes = 64;
constexpr std::size_t max_compatible_name_bytes = 256;

/// The most digits kept from a coverage number's significand.
constexpr int max_coverage_digits = 18;

/// Sets `error` when the caller asked for one, and reports failure.
bool refuse(std::string* error, std::string message) {
    if (error)
        *error = std::move(message);
    return false;
}

/// The catalogue's bytes as text. An empty span is an empty view.
std::string_view bytes_as_text(std::span<const uint8_t> bytes) {
    if (bytes.empty())
        return {};
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// Reports whether every object in the tree names each of its members once.
bool members_unique(const json::Json& value) {
    if (value.type() == json::JsonType::array) {
        for (const json::Json& element : value.elements()) {
            if (!members_unique(element))
                return false;
        }
        return true;
    }
    if (value.type() != json::JsonType::object)
        return true;
    const auto names = value.names();
    for (std::size_t index = 0; index < names.size(); ++index) {
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (names[earlier] == names[index])
                return false;
        }
    }
    for (const json::Json& child : value.values()) {
        if (!members_unique(child))
            return false;
    }
    return true;
}

/// Reads `kind` as one of the package kinds.
bool parse_kind(std::string_view text, Kind& kind) noexcept {
    if (text == "oalang") {
        kind = Kind::oalang;
        return true;
    }
    if (text == "oamod") {
        kind = Kind::oamod;
        return true;
    }
    if (text == "oamap") {
        kind = Kind::oamap;
        return true;
    }
    if (text == "expansion") {
        kind = Kind::expansion;
        return true;
    }
    if (text == "oamut") {
        kind = Kind::oamut;
        return true;
    }
    if (text == "oabrain") {
        kind = Kind::oabrain;
        return true;
    }
    if (text == "oakeys") {
        kind = Kind::oakeys;
        return true;
    }
    return false;
}

/// Reports whether `text` is 64 lower-case hexadecimal digits.
bool lower_hex(std::string_view text) noexcept {
    if (text.size() != sha::hex_size)
        return false;
    for (const char value : text) {
        const bool digit = value >= '0' && value <= '9';
        const bool hex = value >= 'a' && value <= 'f';
        if (!digit && !hex)
            return false;
    }
    return true;
}

/// The last path segment of a reference.
std::string_view last_segment(std::string_view path) noexcept {
    const auto slash = path.rfind('/');
    if (slash == std::string_view::npos)
        return path;
    return path.substr(slash + 1);
}

/// Reports whether `file`'s last segment is the package's hash and kind.
bool file_named(std::string_view file, const sha::Digest& digest, Kind kind) {
    const auto hex = sha::to_hex(digest);
    std::string expected(hex.data(), hex.size());
    expected += '.';
    expected += package_extension(kind);
    return last_segment(file) == expected;
}

/// Keeps a text member when it is present and within the bounds.
bool optional_text(
    const json::Json& object,
    std::string_view name,
    std::size_t min_bytes,
    std::size_t max_bytes,
    std::string& out,
    std::string& reason
) {
    const json::Json* value = object.find(name);
    if (!value)
        return true;
    const std::string* text = value->string();
    if (!text || text->size() < min_bytes || text->size() > max_bytes) {
        reason = std::string(name) + " is not " + std::to_string(min_bytes) + " to " +
                 std::to_string(max_bytes) + " bytes";
        return false;
    }
    out = *text;
    return true;
}

/// Requires a text member within the bounds.
bool require_text(
    const json::Json& object,
    std::string_view name,
    std::size_t min_bytes,
    std::size_t max_bytes,
    std::string& out,
    std::string& reason
) {
    if (!object.find(name)) {
        reason = std::string(name) + " is missing";
        return false;
    }
    return optional_text(object, name, min_bytes, max_bytes, out, reason);
}

/// Reads a list of tags, or of language tags when `language` is set.
bool read_text_list(
    const json::Json& object,
    std::string_view name,
    std::size_t max_count,
    bool language,
    std::vector<std::string>& out,
    std::string& reason
) {
    const json::Json* value = object.find(name);
    if (!value)
        return true;
    if (value->type() != json::JsonType::array) {
        reason = std::string(name) + " is not a list";
        return false;
    }
    if (value->elements().size() > max_count) {
        reason = std::string(name) + " lists more than " + std::to_string(max_count);
        return false;
    }
    for (const json::Json& element : value->elements()) {
        const std::string* text = element.string();
        const bool ok =
            text && (language ? valid_package_key(Kind::oalang, *text) : oamod::tag_valid(*text));
        if (!ok) {
            reason = std::string(language ? "locale \"" : "tag \"");
            if (text)
                reason += *text;
            reason +=
                language ? std::string("\" is not a language tag") : std::string("\" is not a tag");
            return false;
        }
        out.push_back(*text);
    }
    return true;
}

/// Drops leading zeros, leaving one zero when the text is all zeros.
std::string strip_zeros(std::string text) {
    std::size_t start = 0;
    while (start + 1 < text.size() && text[start] == '0')
        ++start;
    if (start == 0)
        return text;
    return text.substr(start);
}

/// The digits of a number scaled to `floor`, with no sign and no leading zero.
std::string scaled_digits(const oamod::Number& number, int32_t floor) {
    if (number.significand == 0)
        return "0";
    std::string digits = std::to_string(number.significand);
    if (number.exponent > floor)
        digits.append(static_cast<std::size_t>(number.exponent - floor), '0');
    return digits;
}

/// Compares two unsigned decimal texts. Both have no leading zero.
bool digits_less(std::string_view first_digits, std::string_view second_digits) {
    if (first_digits.size() != second_digits.size())
        return first_digits.size() < second_digits.size();
    return first_digits < second_digits;
}

/// Adds two unsigned decimal texts.
std::string add_digits(std::string_view first_digits, std::string_view second_digits) {
    std::string sum(std::max(first_digits.size(), second_digits.size()) + 1, '0');
    int carry = 0;
    for (std::size_t slot = 0; slot < sum.size(); ++slot) {
        int value = carry;
        if (slot < first_digits.size())
            value += first_digits[first_digits.size() - 1 - slot] - '0';
        if (slot < second_digits.size())
            value += second_digits[second_digits.size() - 1 - slot] - '0';
        sum[sum.size() - 1 - slot] = static_cast<char>('0' + value % 10);
        carry = value / 10;
    }
    return strip_zeros(std::move(sum));
}

/// Subtracts two unsigned decimal texts. `minuend` is at least `subtrahend`.
std::string subtract_digits(std::string minuend, std::string_view subtrahend) {
    int borrow = 0;
    for (std::size_t slot = 0; slot < minuend.size(); ++slot) {
        const std::size_t from_end = minuend.size() - 1 - slot;
        int value = minuend[from_end] - '0' - borrow;
        if (slot < subtrahend.size())
            value -= subtrahend[subtrahend.size() - 1 - slot] - '0';
        if (value < 0) {
            value += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        minuend[from_end] = static_cast<char>('0' + value);
    }
    return strip_zeros(std::move(minuend));
}

/// The positive difference of two numbers, scaled to `floor`. `high` is at least `low`.
std::string
positive_difference(const oamod::Number& high, const oamod::Number& low, int32_t floor) {
    const bool high_negative = high.negative && high.significand != 0;
    const bool low_negative = low.negative && low.significand != 0;
    const std::string high_digits = scaled_digits(high, floor);
    const std::string low_digits = scaled_digits(low, floor);
    if (high_negative == low_negative) {
        if (!high_negative)
            return subtract_digits(high_digits, low_digits);
        return subtract_digits(low_digits, high_digits);
    }
    return add_digits(high_digits, low_digits);
}

/// Tells whether `span` is a whole number of `step`s, both unsigned decimals.
bool divides_exactly(std::string span, const std::string& step) {
    if (step == "0")
        return false;
    if (span == "0")
        return true;
    std::string remainder = "0";
    for (const char digit : span) {
        if (remainder == "0")
            remainder = std::string(1, digit);
        else
            remainder.push_back(digit);
        remainder = strip_zeros(std::move(remainder));
        while (!digits_less(remainder, step))
            remainder = subtract_digits(std::move(remainder), step);
    }
    return remainder == "0";
}

/// Reads a JSON number into a profile number and keeps its text.
bool read_json_number(const json::Json& value, oamod::Number& number, std::string& written) {
    const std::string* text = value.number_text();
    if (text == nullptr || oamod::parse_number(*text, number) != oamod::NumberStatus::ok)
        return false;
    written = *text;
    return true;
}

/// Reads `member` as mutator requirements. Absent is an empty list.
///
/// Each id is a mutator key, not `self_id`, and none is repeated. `version`
/// is optional and is a version range. When `prerequisites` is set, an id
/// it names is refused. Other members of an object are ignored. The member
/// is read only for a mutator; the caller skips it for every other kind.
bool read_mutator_objects(
    const json::Json& object,
    std::string_view member,
    std::size_t most,
    std::string_view self_id,
    const std::vector<MutatorRequirement>* prerequisites,
    std::vector<MutatorRequirement>& out,
    std::string& reason,
    std::string_view label = {}
) {
    // Refusals name the member as the catalogue writes it (`requires.mutators`).
    const std::string_view shown = label.empty() ? member : label;
    const json::Json* value = object.find(member);
    if (value == nullptr)
        return true;
    if (value->type() != json::JsonType::array) {
        reason = std::string(shown) + " is not a list";
        return false;
    }
    if (value->elements().size() > most) {
        reason = std::string(shown) + " lists more than " + std::to_string(most);
        return false;
    }
    std::vector<std::string> seen;
    for (std::size_t slot = 0; slot < value->elements().size(); ++slot) {
        const std::string where = std::string(shown) + "[" + std::to_string(slot) + "]";
        const json::Json& element = value->elements()[slot];
        if (element.type() != json::JsonType::object) {
            reason = where + " is not an object";
            return false;
        }
        const json::Json* id_value = element.find("id");
        const std::string* id_text = id_value == nullptr ? nullptr : id_value->string();
        if (id_text == nullptr || !valid_package_key(Kind::oamut, *id_text)) {
            reason = where + ".id is not a mutator key";
            return false;
        }
        if (*id_text == self_id) {
            reason = where + ".id is this mutator";
            return false;
        }
        bool repeated = false;
        for (const std::string& earlier : seen) {
            if (earlier == *id_text)
                repeated = true;
        }
        if (repeated) {
            reason = where + ".id is repeated";
            return false;
        }
        if (prerequisites != nullptr) {
            bool named = false;
            for (const MutatorRequirement& prerequisite : *prerequisites) {
                if (prerequisite.id == *id_text)
                    named = true;
            }
            if (named) {
                reason = where + ".id is a prerequisite";
                return false;
            }
        }
        MutatorRequirement requirement;
        requirement.id = *id_text;
        if (const json::Json* version = element.find("version")) {
            const std::string* text = version->string();
            std::optional<oamut::VersionRange> range;
            if (text != nullptr)
                range = oamut::parse_version_range(*text);
            if (!range) {
                reason = where + ".version is not a version range";
                return false;
            }
            requirement.range_text = *text;
            requirement.range = std::move(*range);
        }
        seen.push_back(requirement.id);
        out.push_back(std::move(requirement));
    }
    return true;
}

/// Reads `works_with`. Each entry is a condition, and `*` is the only entry when it appears.
bool read_works_with(const json::Json& object, Package& package, std::string& reason) {
    const json::Json* value = object.find("works_with");
    if (value == nullptr)
        return true;
    if (value->type() != json::JsonType::array) {
        reason = "works_with is not a list";
        return false;
    }
    if (value->elements().size() > oamut::most_works_with) {
        reason = "works_with lists more than " + std::to_string(oamut::most_works_with);
        return false;
    }
    for (std::size_t slot = 0; slot < value->elements().size(); ++slot) {
        const std::string where = "works_with[" + std::to_string(slot) + "]";
        const std::string* text = value->elements()[slot].string();
        std::optional<oamut::Condition> condition;
        if (text != nullptr)
            condition = oamut::parse_condition(*text);
        if (!condition) {
            reason = where + " is not a condition";
            return false;
        }
        if (condition->id == "*" && value->elements().size() != 1) {
            reason = where + " is not alone";
            return false;
        }
        package.works_with.push_back(*text);
    }
    return true;
}

/// Reads `clashes`. Absent stays absent. Any other value refuses the entry.
bool read_clashes(const json::Json& object, Package& package, std::string& reason) {
    const json::Json* value = object.find("clashes");
    if (value == nullptr)
        return true;
    const std::string* text = value->string();
    const std::optional<oamut::ClashMode> mode =
        text != nullptr ? oamut::parse_clash_mode(*text) : std::nullopt;
    if (!mode) {
        reason = "clashes must be add or replace";
        return false;
    }
    package.clashes = *mode;
    return true;
}

/// Reads `layers`. A known name is kept once. An unknown name is skipped.
bool read_layers(const json::Json& object, Package& package, std::string& reason) {
    constexpr std::size_t most_layers = 16;
    const json::Json* value = object.find("layers");
    if (value == nullptr)
        return true;
    if (value->type() != json::JsonType::array) {
        reason = "layers is not a list";
        return false;
    }
    if (value->elements().size() > most_layers) {
        reason = "layers lists more than " + std::to_string(most_layers);
        return false;
    }
    for (const json::Json& element : value->elements()) {
        const std::string* text = element.string();
        if (text == nullptr)
            continue;
        const std::optional<oamut::Layer> layer = oamut::parse_layer(*text);
        if (!layer)
            continue;
        bool seen = false;
        for (const oamut::Layer kept : package.layers) {
            if (kept == *layer)
                seen = true;
        }
        if (!seen)
            package.layers.push_back(*layer);
    }
    return true;
}

/// Reads one choice value. `where` names it in a refusal.
bool read_choice_value(
    const json::Json& element, std::string_view where, ChoiceValueText& out, std::string& reason
) {
    if (const std::string* written = element.number_text()) {
        oamod::Number number;
        if (oamod::parse_number(*written, number) != oamod::NumberStatus::ok) {
            reason = std::string(where) + " is not a number";
            return false;
        }
        out.number = true;
        out.text = *written;
        return true;
    }
    const std::string* text = element.string();
    if (text == nullptr || !oamut::choice_value_text(*text)) {
        reason = std::string(where) + " is not a number or 1 to " +
                 std::to_string(oamut::most_choice_value_bytes) + " letters, digits, _, - or .";
        return false;
    }
    out.text = *text;
    return true;
}

/// Reads a choice option's values, labels and default into `summary`.
bool read_choice_option(
    const json::Json& object, std::string_view where, OptionSummary& summary, std::string& reason
) {
    constexpr std::size_t most_label_bytes = 64;
    const json::Json* values = object.find("values");
    if (values == nullptr || values->type() != json::JsonType::array ||
        values->elements().size() < 2 || values->elements().size() > oamut::most_choice_values) {
        reason = std::string(where) + ".values is not 2 to " +
                 std::to_string(oamut::most_choice_values) + " values";
        return false;
    }
    bool saw = false;
    bool numbers = false;
    for (std::size_t slot = 0; slot < values->elements().size(); ++slot) {
        const std::string item = std::string(where) + ".values[" + std::to_string(slot) + "]";
        ChoiceValueText value;
        if (!read_choice_value(values->elements()[slot], item, value, reason))
            return false;
        if (saw && value.number != numbers) {
            reason = item + " is not the same kind of value";
            return false;
        }
        saw = true;
        numbers = value.number;
        summary.values.push_back(std::move(value));
    }
    if (const json::Json* labels = object.find("labels")) {
        if (labels->type() != json::JsonType::array ||
            labels->elements().size() != summary.values.size()) {
            reason = std::string(where) + ".labels is not one label for each value";
            return false;
        }
        for (std::size_t slot = 0; slot < labels->elements().size(); ++slot) {
            const std::string* text = labels->elements()[slot].string();
            if (text == nullptr || text->empty() || text->size() > most_label_bytes) {
                reason = std::string(where) + ".labels[" + std::to_string(slot) +
                         "] is not 1 to 64 bytes";
                return false;
            }
            summary.value_labels.push_back(*text);
        }
    }
    const json::Json* fallback = object.find("default");
    if (fallback == nullptr)
        return true;
    for (const ChoiceValueText& value : summary.values) {
        if (value.number) {
            oamod::Number listed;
            oamod::Number chosen;
            std::string chosen_text;
            if (oamod::parse_number(value.text, listed) == oamod::NumberStatus::ok &&
                read_json_number(*fallback, chosen, chosen_text) &&
                oamod::compare_numbers(listed, chosen) == 0) {
                summary.default_text = std::move(chosen_text);
                return true;
            }
        } else if (const std::string* text = fallback->string()) {
            if (*text == value.text) {
                summary.default_text = *text;
                return true;
            }
        }
    }
    reason = std::string(where) + ".default is not one of the values";
    return false;
}

/// Reads a number option's minimum, maximum, step and default.
bool read_number_option(
    const json::Json& object, std::string_view where, OptionSummary& summary, std::string& reason
) {
    const json::Json* minimum = object.find("min");
    const json::Json* maximum = object.find("max");
    const json::Json* step = object.find("step");
    oamod::Number minimum_number;
    oamod::Number maximum_number;
    oamod::Number step_number;
    if (minimum == nullptr || !read_json_number(*minimum, minimum_number, summary.minimum)) {
        reason = std::string(where) + ".min is not a number";
        return false;
    }
    if (maximum == nullptr || !read_json_number(*maximum, maximum_number, summary.maximum)) {
        reason = std::string(where) + ".max is not a number";
        return false;
    }
    if (step == nullptr || !read_json_number(*step, step_number, summary.step)) {
        reason = std::string(where) + ".step is not a number";
        return false;
    }
    if (oamod::compare_numbers(minimum_number, maximum_number) >= 0) {
        reason = std::string(where) + ".min is not below max";
        return false;
    }
    const oamod::Number zero = oamod::number_from_integer(0);
    if (oamod::compare_numbers(step_number, zero) <= 0) {
        reason = std::string(where) + ".step is not above zero";
        return false;
    }
    const json::Json* fallback = object.find("default");
    if (fallback == nullptr)
        return true;
    oamod::Number chosen;
    if (!read_json_number(*fallback, chosen, summary.default_text)) {
        reason = std::string(where) + ".default is not a number";
        return false;
    }
    if (oamod::compare_numbers(chosen, minimum_number) < 0 ||
        oamod::compare_numbers(chosen, maximum_number) > 0) {
        reason = std::string(where) + ".default is not on a step";
        return false;
    }
    int32_t floor = minimum_number.exponent;
    if (maximum_number.exponent < floor)
        floor = maximum_number.exponent;
    if (step_number.exponent < floor)
        floor = step_number.exponent;
    if (chosen.exponent < floor)
        floor = chosen.exponent;
    const std::string span = positive_difference(chosen, minimum_number, floor);
    const std::string step_digits = scaled_digits(step_number, floor);
    if (!divides_exactly(span, step_digits)) {
        reason = std::string(where) + ".default is not on a step";
        return false;
    }
    return true;
}

/// Reads one option of a known type. An unknown type is the caller's to skip.
bool read_known_option(
    const json::Json& object,
    std::string_view where,
    oamut::OptionType type,
    OptionSummary& summary,
    std::string& reason
) {
    constexpr std::size_t most_label_bytes = 64;
    summary.type = type;
    const json::Json* id_value = object.find("id");
    const std::string* id_text = id_value == nullptr ? nullptr : id_value->string();
    if (id_text == nullptr || !oamut::option_id(*id_text)) {
        if (id_text != nullptr && oamut::lua_reserved_word(*id_text))
            reason = std::string(where) + ".id is a word Lua reserves";
        else
            reason = std::string(where) + ".id is not an option id";
        return false;
    }
    summary.id = *id_text;
    const json::Json* label = object.find("label");
    const std::string* label_text = label == nullptr ? nullptr : label->string();
    if (label_text == nullptr || label_text->empty() || label_text->size() > most_label_bytes) {
        reason = std::string(where) + ".label is not 1 to 64 bytes";
        return false;
    }
    summary.label = *label_text;
    if (type == oamut::OptionType::toggle) {
        const json::Json* fallback = object.find("default");
        if (fallback == nullptr)
            return true;
        const std::optional<bool> flag = fallback->boolean();
        if (!flag) {
            reason = std::string(where) + ".default is not true or false";
            return false;
        }
        summary.default_text = *flag ? "true" : "false";
        return true;
    }
    if (type == oamut::OptionType::choice)
        return read_choice_option(object, where, summary, reason);
    return read_number_option(object, where, summary, reason);
}

/// Reads `options`. An option of an unknown type is skipped.
bool read_options(const json::Json& object, Package& package, std::string& reason) {
    const json::Json* value = object.find("options");
    if (value == nullptr)
        return true;
    if (value->type() != json::JsonType::array) {
        reason = "options is not a list";
        return false;
    }
    if (value->elements().size() > oamut::most_options) {
        reason = "options lists more than " + std::to_string(oamut::most_options);
        return false;
    }
    for (std::size_t slot = 0; slot < value->elements().size(); ++slot) {
        const std::string where = "options[" + std::to_string(slot) + "]";
        const json::Json& element = value->elements()[slot];
        if (element.type() != json::JsonType::object) {
            reason = where + " is not an object";
            return false;
        }
        const json::Json* type_value = element.find("type");
        const std::string* type_text = type_value == nullptr ? nullptr : type_value->string();
        oamut::OptionType type = oamut::OptionType::toggle;
        bool known = false;
        if (type_text != nullptr && *type_text == "toggle") {
            type = oamut::OptionType::toggle;
            known = true;
        } else if (type_text != nullptr && *type_text == "choice") {
            type = oamut::OptionType::choice;
            known = true;
        } else if (type_text != nullptr && *type_text == "number") {
            type = oamut::OptionType::number;
            known = true;
        }
        if (!known)
            continue;
        OptionSummary summary;
        if (!read_known_option(element, where, type, summary, reason))
            return false;
        package.options.push_back(std::move(summary));
    }
    return true;
}

/// Reads `works_with` and `options`, which a mutator and a brain both carry.
bool read_works_with_and_options(const json::Json& object, Package& package, std::string& reason) {
    if (!read_works_with(object, package, reason))
        return false;
    return read_options(object, package, reason);
}

/// Reads the mutator members other than `requires.mutators`, which `read_requires` reads.
bool read_mutator_facts(const json::Json& object, Package& package, std::string& reason) {
    if (!read_works_with_and_options(object, package, reason))
        return false;
    if (!read_mutator_objects(
            object,
            "conflicts_with",
            oamut::most_conflicts,
            package.id,
            &package.requires_mutators,
            package.conflicts_with,
            reason
        ))
        return false;
    if (!read_layers(object, package, reason))
        return false;
    return read_clashes(object, package, reason);
}

/// Reads `levels`, `full_awareness` and `network_bytes` on a brain.
///
/// A level whose difficulty this build does not know is skipped. An entry
/// left with no level is refused. Any other broken member names itself.
bool read_brain_facts(const json::Json& object, Package& package, std::string& reason) {
    constexpr uint64_t most_bytes = 67108864;
    static_assert(most_bytes == oabrain::most_network_bytes);
    const json::Json* levels = object.find("levels");
    if (levels == nullptr) {
        reason = "levels is missing";
        return false;
    }
    if (levels->type() != json::JsonType::array) {
        reason = "levels is not a list";
        return false;
    }
    if (levels->elements().empty()) {
        reason = "levels lists none";
        return false;
    }
    if (levels->elements().size() > oabrain::most_levels) {
        reason = "levels lists more than " + std::to_string(oabrain::most_levels);
        return false;
    }
    std::vector<std::string> seen_ids;
    for (std::size_t slot = 0; slot < levels->elements().size(); ++slot) {
        const std::string where = "levels[" + std::to_string(slot) + "]";
        const json::Json& element = levels->elements()[slot];
        if (element.type() != json::JsonType::object) {
            reason = where + " is not an object";
            return false;
        }
        const json::Json* id_value = element.find("id");
        const std::string* id_text = id_value == nullptr ? nullptr : id_value->string();
        if (id_text == nullptr || !oabrain::level_id(*id_text)) {
            reason = where + ".id is not a level id";
            return false;
        }
        for (const std::string& earlier : seen_ids) {
            if (earlier == *id_text) {
                reason = where + ".id is repeated";
                return false;
            }
        }
        seen_ids.push_back(*id_text);
        const json::Json* name_value = element.find("name");
        const std::string* name_text = name_value == nullptr ? nullptr : name_value->string();
        if (name_text == nullptr || name_text->empty() ||
            name_text->size() > oabrain::most_level_name_bytes) {
            reason = where + ".name is not 1 to 64 bytes";
            return false;
        }
        const json::Json* difficulty_value = element.find("difficulty");
        const std::string* difficulty_text =
            difficulty_value == nullptr ? nullptr : difficulty_value->string();
        const std::optional<oabrain::Difficulty> difficulty =
            difficulty_text == nullptr ? std::nullopt : oabrain::parse_difficulty(*difficulty_text);
        if (!difficulty)
            continue;
        BrainLevel kept;
        kept.id = *id_text;
        kept.name = *name_text;
        kept.difficulty = *difficulty;
        package.levels.push_back(std::move(kept));
    }
    if (package.levels.empty()) {
        reason = "levels names no level this build knows";
        return false;
    }
    if (const json::Json* awareness = object.find("full_awareness")) {
        const std::optional<bool> flag = awareness->boolean();
        if (!flag) {
            reason = "full_awareness is not true or false";
            return false;
        }
        package.full_awareness = *flag;
    }
    if (const json::Json* size_value = object.find("network_bytes")) {
        const std::optional<int64_t> number = size_value->integer();
        if (!number || *number < 0 || static_cast<uint64_t>(*number) > most_bytes) {
            reason = "network_bytes is not a whole number from 0 to " + std::to_string(most_bytes);
            return false;
        }
        package.network_bytes = static_cast<uint64_t>(*number);
    }
    return true;
}

/// Whether `name` is one of the six helpers a key layout may set.
bool known_helper(std::string_view name) noexcept {
    return name == "toggle" || name == "sticky-modifiers" || name == "hold-to-confirm" ||
           name == "pointer-keys" || name == "radial-orders" || name == "edge-scroll";
}

/// Whether every byte is an ASCII letter or digit.
bool letters_and_digits(std::string_view text) noexcept {
    if (text.empty())
        return false;
    for (const char byte : text) {
        const bool digit = byte >= '0' && byte <= '9';
        const bool capital = byte >= 'A' && byte <= 'Z';
        const bool small = byte >= 'a' && byte <= 'z';
        if (!digit && !capital && !small)
            return false;
    }
    return true;
}

/// Whether `name` is a pad control: a capital first letter, then letters and digits.
bool pad_name_ok(std::string_view name) noexcept {
    if (name.empty() || name.front() < 'A' || name.front() > 'Z')
        return false;
    return letters_and_digits(name);
}

/// Whether `text` is a trigger this build's catalogue can name.
///
/// A modifier on its own is not one. A string the chord grammar would not
/// read is not one, and the caller skips it. The check is the shape only:
/// this library does not link the key-binding parser.
bool readable_trigger(std::string_view text) noexcept {
    if (text == "Primary" || text == "Ctrl" || text == "Alt" || text == "Shift" || text == "Meta")
        return false;
    if (text.size() == 1) {
        const unsigned char byte = static_cast<unsigned char>(text[0]);
        return byte >= 0x21 && byte <= 0x7E;
    }
    if (!text.empty() && text.front() == '@')
        return letters_and_digits(text.substr(1));
    static constexpr std::string_view named[] = {
        "Escape",
        "Enter",
        "Tab",
        "Backspace",
        "Space",
        "Delete",
        "F1",
        "F2",
        "F3",
        "F4",
        "F5",
        "F6",
        "F7",
        "F8",
        "F9",
        "F10",
        "F11",
        "F12",
        "F13",
        "F14",
        "F15",
        "F16",
        "F17",
        "F18",
        "F19",
        "F20",
        "F21",
        "F22",
        "F23",
        "F24",
        "Insert",
        "Home",
        "End",
        "PageUp",
        "PageDown",
        "Up",
        "Down",
        "Left",
        "Right",
        "Numpad0",
        "Numpad1",
        "Numpad2",
        "Numpad3",
        "Numpad4",
        "Numpad5",
        "Numpad6",
        "Numpad7",
        "Numpad8",
        "Numpad9",
        "NumpadAdd",
        "NumpadSubtract",
        "NumpadMultiply",
        "NumpadDivide",
        "NumpadDecimal",
        "NumpadEnter",
        "PrintScreen",
        "Pause",
        "ContextMenu",
    };
    for (const std::string_view name : named) {
        if (text == name)
            return true;
    }
    static constexpr std::string_view mouse[] = {
        "MouseLeft",
        "MouseRight",
        "MouseMiddle",
        "MouseBack",
        "MouseForward",
        "Mouse6",
        "Mouse7",
        "Mouse8",
    };
    for (const std::string_view name : mouse) {
        if (text == name)
            return true;
    }
    if (text == "WheelUp" || text == "WheelDown" || text == "WheelRight" || text == "WheelLeft")
        return true;
    constexpr std::string_view pad = "Pad.";
    if (text.size() > pad.size() && text.substr(0, pad.size()) == pad)
        return pad_name_ok(text.substr(pad.size()));
    return false;
}

/// Reads the members only a key layout uses.
///
/// An unknown helper, and a trigger this build cannot read, are skipped.
/// The lists' limits apply before that skip. Any other broken member names
/// itself and leaves the entry out.
bool read_key_facts(const json::Json& object, Package& package, std::string& reason) {
    const json::Json* base = object.find("base");
    if (base == nullptr) {
        reason = "base is missing";
        return false;
    }
    const std::string* base_text = base->string();
    if (base_text == nullptr || (*base_text != "oa-default" && *base_text != "none")) {
        reason = "base is not oa-default or none";
        return false;
    }
    package.key_base = *base_text;

    package.keyboard = "any";
    if (const json::Json* keyboard = object.find("keyboard")) {
        const std::string* keyboard_text = keyboard->string();
        if (keyboard_text == nullptr || (*keyboard_text != "any" && *keyboard_text != "ansi" &&
                                         *keyboard_text != "iso" && *keyboard_text != "jis")) {
            reason = "keyboard is not any, ansi, iso or jis";
            return false;
        }
        package.keyboard = *keyboard_text;
    }
    if (const json::Json* pointer = object.find("pointer")) {
        const std::string* pointer_text = pointer->string();
        if (pointer_text == nullptr ||
            (*pointer_text != "right-click" && *pointer_text != "left-click")) {
            reason = "pointer is not right-click or left-click";
            return false;
        }
        package.pointer = *pointer_text;
    }
    if (const json::Json* helpers = object.find("helpers")) {
        if (helpers->type() != json::JsonType::array) {
            reason = "helpers is not a list";
            return false;
        }
        if (helpers->elements().size() > 6) {
            reason = "helpers lists more than 6";
            return false;
        }
        for (std::size_t slot = 0; slot < helpers->elements().size(); ++slot) {
            const std::string* helper_text = helpers->elements()[slot].string();
            if (helper_text == nullptr) {
                reason = "helpers[" + std::to_string(slot) + "] is not text";
                return false;
            }
            if (!known_helper(*helper_text))
                continue;
            for (const std::string& earlier : package.helpers) {
                if (earlier == *helper_text) {
                    reason = "helpers[" + std::to_string(slot) + "] is repeated";
                    return false;
                }
            }
            package.helpers.push_back(*helper_text);
        }
    }
    if (const json::Json* changes = object.find("changes")) {
        const std::optional<int64_t> number = changes->integer();
        if (!number || *number < 0 || *number > 2048) {
            reason = "changes is not a whole number from 0 to 2048";
            return false;
        }
        package.changes = static_cast<uint32_t>(*number);
    }
    if (const json::Json* triggers = object.find("triggers")) {
        if (triggers->type() != json::JsonType::array) {
            reason = "triggers is not a list";
            return false;
        }
        if (triggers->elements().size() > 128) {
            reason = "triggers lists more than 128";
            return false;
        }
        for (std::size_t slot = 0; slot < triggers->elements().size(); ++slot) {
            const std::string* trigger_text = triggers->elements()[slot].string();
            if (trigger_text == nullptr) {
                reason = "triggers[" + std::to_string(slot) + "] is not text";
                return false;
            }
            if (!readable_trigger(*trigger_text))
                continue;
            for (const std::string& earlier : package.triggers) {
                if (earlier == *trigger_text) {
                    reason = "triggers[" + std::to_string(slot) + "] is repeated";
                    return false;
                }
            }
            package.triggers.push_back(*trigger_text);
        }
    }
    if (const json::Json* preview = object.find("preview")) {
        const std::string* preview_text = preview->string();
        const bool png = preview_text && preview_text->size() >= 4 &&
                         preview_text->compare(preview_text->size() - 4, 4, ".png") == 0;
        if (preview_text == nullptr || !valid_reference(*preview_text) || !png) {
            reason = "preview is not a picture reference";
            return false;
        }
        package.preview = *preview_text;
    }
    return true;
}

/// Reads `requires` when the package names it.
bool read_requires(const json::Json& object, Package& package, std::string& reason) {
    const json::Json* value = object.find("requires");
    if (!value)
        return true;
    if (value->type() != json::JsonType::object) {
        reason = "requires is not an object";
        return false;
    }
    if (const json::Json* engine = value->find("engine")) {
        const std::string* text = engine->string();
        if (!text) {
            reason = "requires.engine is not text";
            return false;
        }
        std::string error;
        auto range = oamod::parse_engine_range(*text, &error);
        if (!range) {
            reason = "requires.engine " + error;
            return false;
        }
        package.requires_engine_text = *text;
        package.requires_engine = std::move(*range);
    }
    if (const json::Json* base = value->find("base")) {
        const std::string* text = base->string();
        if (!text) {
            reason = "requires.base is not text";
            return false;
        }
        package.requires_base = *text;
    }
    // Prerequisites are a mutator's. On every other kind the member is ignored.
    if (package.kind == Kind::oamut && !read_mutator_objects(
                                           *value,
                                           "mutators",
                                           oamut::most_required,
                                           package.id,
                                           nullptr,
                                           package.requires_mutators,
                                           reason,
                                           "requires.mutators"
                                       ))
        return false;
    return true;
}

/// Tells whether an expansion extends one of the games.
///
/// @param expansion the expansion
/// @param games the base game ids
/// @return true when it extends one of them
bool extends_one(const bundles::Bundle& expansion, const std::vector<std::string>& games) {
    for (const std::string_view game : expansion.extends) {
        for (const std::string& supported : games) {
            if (supported == game)
                return true;
        }
    }
    return false;
}

/// Reads supportedGames and requiredExpansions when the entry names them.
///
/// A missing key leaves the list empty. supportedGames must name at least
/// one base game. requiredExpansions may be empty. Each id is a bundle of
/// that kind, none repeated, at most 16. An expansion has to extend a game
/// the entry names, or, when it omits supportedGames, the games that key
/// means: both base games for a language pack, and totala otherwise.
///
/// @param object the package
/// @param[in,out] package the package being read; its kind and requires_base are set
/// @param[out] reason why the lists were refused
/// @return true when the lists passed, including when both keys are absent
bool read_content_lists(const json::Json& object, Package& package, std::string& reason) {
    const auto read_ids =
        [&](std::string_view key, bool allow_empty, std::vector<std::string>& out) -> bool {
        const json::Json* value = object.find(key);
        if (!value)
            return true;
        if (value->type() != json::JsonType::array) {
            reason = std::string(key) + " is not a list";
            return false;
        }
        if (value->elements().size() > max_content_ids) {
            reason = std::string(key) + " lists more than " + std::to_string(max_content_ids);
            return false;
        }
        if (value->elements().empty() && !allow_empty) {
            reason = std::string(key) + " lists none";
            return false;
        }
        for (const json::Json& element : value->elements()) {
            const std::string* text = element.string();
            if (!text || text->empty()) {
                reason = std::string(key) + " holds an id that is not text";
                return false;
            }
            for (const std::string& earlier : out) {
                if (earlier == *text) {
                    reason = std::string(key) + " repeats \"" + *text + "\"";
                    return false;
                }
            }
            out.push_back(*text);
        }
        return true;
    };

    std::vector<std::string> games;
    std::vector<std::string> expansions;
    const bool games_written = object.find("supportedGames") != nullptr;
    const bool expansions_written = object.find("requiredExpansions") != nullptr;
    if (!read_ids("supportedGames", false, games) ||
        !read_ids("requiredExpansions", true, expansions))
        return false;
    for (const std::string& id : games) {
        const bundles::Bundle* bundle = bundles::find_bundle(id);
        if (bundle == nullptr || bundle->kind != bundles::Kind::base_game) {
            reason = "supportedGames \"" + id + "\" is not a base game";
            return false;
        }
    }
    std::vector<std::string> extends_against = games;
    if (!games_written) {
        if (package.kind == Kind::oalang) {
            extends_against.emplace_back(bundles::default_base_game_id);
            extends_against.emplace_back("totala-demo");
        } else {
            // A mod or a map pack that omits the key runs with totala, which
            // is also what requires.base ta-3.1c means.
            extends_against.emplace_back(bundles::default_base_game_id);
        }
    }
    for (const std::string& id : expansions) {
        const bundles::Bundle* bundle = bundles::find_bundle(id);
        if (bundle == nullptr || bundle->kind != bundles::Kind::expansion) {
            reason = "requiredExpansions \"" + id + "\" is not an expansion";
            return false;
        }
        if (!extends_one(*bundle, extends_against)) {
            reason = "requiredExpansions \"" + id + "\" extends none of the supported games";
            return false;
        }
    }
    if (games_written)
        package.supported_games = std::move(games);
    if (expansions_written)
        package.required_expansions = std::move(expansions);
    return true;
}

/// Reports whether a hack id is 1 to 64 bytes of the hack-id alphabet.
bool valid_hack_id(std::string_view text) noexcept {
    if (text.empty() || text.size() > 64)
        return false;
    for (const char value : text) {
        const bool word = (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
        if (!word && value != '.' && value != '-')
            return false;
    }
    return true;
}

/// Reads `hacks` when the package names it.
bool read_hacks(const json::Json& object, Package& package, std::string& reason) {
    const json::Json* value = object.find("hacks");
    if (!value)
        return true;
    if (value->type() != json::JsonType::object) {
        reason = "hacks is not an object";
        return false;
    }
    const auto whole = [](const json::Json* field,
                          std::optional<int32_t>& out,
                          std::string& reason,
                          std::string_view name) {
        if (!field)
            return true;
        const auto number = field->integer();
        if (!number || *number < 0 || *number > max_release) {
            reason = std::string(name) + " is not a whole number";
            return false;
        }
        out = static_cast<int32_t>(*number);
        return true;
    };
    if (!whole(value->find("sim"), package.hacks_sim, reason, "hacks.sim") ||
        !whole(value->find("view"), package.hacks_view, reason, "hacks.view"))
        return false;
    const json::Json* ids = value->find("ids");
    if (!ids)
        return true;
    if (ids->type() != json::JsonType::array) {
        reason = "hacks.ids is not a list";
        return false;
    }
    if (ids->elements().size() > 256) {
        reason = "hacks.ids lists more than 256";
        return false;
    }
    for (const json::Json& element : ids->elements()) {
        const std::string* text = element.string();
        if (!text || !valid_hack_id(*text)) {
            reason = "hack id";
            if (text)
                reason += " \"" + *text + "\"";
            reason += " is not a hack id";
            return false;
        }
        package.hack_ids.push_back(*text);
    }
    return true;
}

/// Reads decimal digits into a coverage significand, stopping after 18 digits.
bool take_digits(std::string_view text, std::size_t& index, uint64_t& significand, int& digits) {
    const std::size_t start = index;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        if (digits >= max_coverage_digits)
            return false;
        significand = significand * 10 + static_cast<uint64_t>(text[index] - '0');
        ++digits;
        ++index;
    }
    return index > start;
}

/// Reads a coverage number from its JSON spelling. The value is from 0 to 1.
bool coverage_number(std::string_view text, double& out) {
    if (text.empty() || text.front() == '-' || text.front() == '+')
        return false;
    std::size_t index = 0;
    if (text[index] < '0' || text[index] > '9')
        return false;
    uint64_t significand = 0;
    int digits = 0;
    if (text[index] == '0')
        ++index;
    else if (!take_digits(text, index, significand, digits))
        return false;
    const int integer_digits = digits;
    int fraction_digits = 0;
    if (index < text.size() && text[index] == '.') {
        ++index;
        if (!take_digits(text, index, significand, digits))
            return false;
        fraction_digits = digits - integer_digits;
    }
    int exponent = 0;
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        int sign = 1;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
            if (text[index] == '-')
                sign = -1;
            ++index;
        }
        if (index >= text.size() || text[index] < '0' || text[index] > '9')
            return false;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            if (exponent > 10000)
                return false;
            exponent = exponent * 10 + (text[index] - '0');
            ++index;
        }
        exponent *= sign;
    }
    if (index != text.size())
        return false;
    if (significand == 0) {
        out = 0;
        return true;
    }
    const long long power = static_cast<long long>(exponent) - fraction_digits;
    if (power >= 0) {
        if (significand == 1 && power == 0) {
            out = 1;
            return true;
        }
        return false;
    }
    const long long denominator = -power;
    if (denominator > max_coverage_digits) {
        // At most 18 significand digits, so this is below 1.
        out = 0;
        if (denominator <= 350) {
            double value = static_cast<double>(significand);
            for (long long step = 0; step < denominator; ++step)
                value /= 10;
            out = value;
        }
        return true;
    }
    uint64_t scale = 1;
    for (long long step = 0; step < denominator; ++step)
        scale *= 10;
    if (significand > scale)
        return false;
    out = static_cast<double>(significand) / static_cast<double>(scale);
    return true;
}

/// Reads one side of a map's `<width>x<height>`. No leading zero, 1 to 256.
bool map_side(std::string_view text, int32_t& out) noexcept {
    if (text.empty() || text.size() > 3 || (text.size() > 1 && text.front() == '0'))
        return false;
    int value = 0;
    for (const char byte : text) {
        if (byte < '0' || byte > '9')
            return false;
        value = value * 10 + (byte - '0');
    }
    if (value < 1 || value > 256)
        return false;
    out = value;
    return true;
}

/// Reads a map's size, one `x` between the width and the height.
bool parse_map_size(std::string_view text, int32_t& width, int32_t& height) noexcept {
    const auto mark = text.find('x');
    if (mark == std::string_view::npos || text.find('x', mark + 1) != std::string_view::npos)
        return false;
    return map_side(text.substr(0, mark), width) && map_side(text.substr(mark + 1), height);
}

/// Reads one map. A map that names another package fails the whole entry.
bool read_map(
    const json::Json& value, std::string_view package_id, MapEntry& entry, std::string& reason
) {
    if (value.type() != json::JsonType::object) {
        reason = "map is not an object";
        return false;
    }
    const json::Json* map = value.find("map");
    const std::string* map_text = map ? map->string() : nullptr;
    const auto at = map_text ? map_text->find('@') : std::string::npos;
    const bool one_at = map_text && at != std::string::npos && at != 0 &&
                        map_text->find('@', at + 1) == std::string::npos;
    const std::string_view suffix =
        one_at ? std::string_view(*map_text).substr(at + 1) : std::string_view{};
    if (!one_at || suffix != package_id) {
        reason = "map names another package";
        return false;
    }
    entry.map = *map_text;
    entry.stem = map_text->substr(0, at);
    if (!require_text(value, "name", 1, max_map_name_bytes, entry.name, reason)) {
        reason = "map name is not 1 to 64 bytes";
        return false;
    }
    const json::Json* players = value.find("players");
    const auto player_count = players ? players->integer() : std::nullopt;
    if (!player_count || *player_count < 1 || *player_count > 10) {
        reason = "players is not 1 to 10";
        return false;
    }
    entry.players = static_cast<int32_t>(*player_count);
    const json::Json* size = value.find("size");
    const std::string* size_text = size ? size->string() : nullptr;
    if (!size_text || !parse_map_size(*size_text, entry.width, entry.height)) {
        reason = "map size is not width x height";
        return false;
    }
    if (const json::Json* preview = value.find("preview")) {
        const std::string* text = preview->string();
        const bool png =
            text && text->size() >= 4 && text->compare(text->size() - 4, 4, ".png") == 0;
        if (!text || !valid_reference(*text) || !png) {
            reason = "preview is not a picture reference";
            return false;
        }
        entry.preview = *text;
    }
    if (const json::Json* compatible = value.find("compatible")) {
        if (compatible->type() != json::JsonType::object) {
            reason = "compatible is not names and booleans";
            return false;
        }
        const auto names = compatible->names();
        const auto values = compatible->values();
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (names[index].empty() || names[index].size() > max_compatible_name_bytes) {
                reason = "compatible name is not 1 to 256 bytes";
                return false;
            }
            const auto flag = values[index].boolean();
            if (!flag) {
                reason = "compatible is not names and booleans";
                return false;
            }
            entry.compatible.emplace_back(names[index], *flag);
        }
    }
    return true;
}

/// Reads the fields only a language pack carries. Other kinds ignore them.
bool read_language(const json::Json& value, Package& package, std::string& reason) {
    if (const json::Json* coverage = value.find("coverage")) {
        const std::string* text = coverage->number_text();
        double number = 0;
        if (!text || !coverage_number(*text, number)) {
            reason = "coverage is not a number from 0 to 1";
            return false;
        }
        package.coverage = number;
    }
    if (!optional_text(value, "english_name", 1, 64, package.english_name, reason) ||
        !optional_text(value, "word", 1, 32, package.word, reason))
        return false;
    if (!read_text_list(value, "locales", 16, true, package.locales, reason) ||
        !read_text_list(value, "fallbacks", 8, true, package.fallbacks, reason))
        return false;
    if (const json::Json* needs = value.find("needs")) {
        const std::string* text = needs->string();
        if (!text || (*text != "game-fonts" && *text != "modern-fonts")) {
            reason = "needs is not game-fonts or modern-fonts";
            return false;
        }
        package.needs = *text;
    }
    return true;
}

/// Reads an expansion's id and the base games it extends.
///
/// The id is a built-in expansion. `extends` is that expansion's list, in
/// order. A missing list is the same refusal as a different one.
///
/// @param value the package object
/// @param[in,out] package the package; its id is already set
/// @param[out] reason the first broken rule
/// @return true when the id is an expansion and extends is its list
bool read_expansion_lists(const json::Json& value, Package& package, std::string& reason) {
    const bundles::Bundle* bundle = bundles::find_bundle(package.id);
    if (bundle == nullptr || bundle->kind != bundles::Kind::expansion) {
        reason = "id \"" + package.id + "\" is not an expansion";
        return false;
    }
    const json::Json* list = value.find("extends");
    if (list == nullptr || list->type() != json::JsonType::array ||
        list->elements().size() != bundle->extends.size()) {
        reason = "extends is not the expansion's list";
        return false;
    }
    for (std::size_t index = 0; index < bundle->extends.size(); ++index) {
        const std::string* text = list->elements()[index].string();
        if (text == nullptr || *text != bundle->extends[index]) {
            reason = "extends is not the expansion's list";
            return false;
        }
        package.extends.push_back(*text);
    }
    return true;
}

/// Reads one package. The first broken rule is the reason the entry is left out.
bool read_package(const json::Json& value, Package& package, std::string& reason) {
    if (value.type() != json::JsonType::object) {
        reason = "a package is not an object";
        return false;
    }
    const json::Json* kind_value = value.find("kind");
    const std::string* kind_text = kind_value ? kind_value->string() : nullptr;
    Kind kind = Kind::oamod;
    if (!kind_text || !parse_kind(*kind_text, kind)) {
        reason = "unknown kind";
        return false;
    }
    package.kind = kind;
    const json::Json* id_value = value.find("id");
    const std::string* id_text = id_value ? id_value->string() : nullptr;
    if (!id_text || !valid_package_key(kind, *id_text)) {
        const std::string shown = id_text ? *id_text : std::string{};
        if (id_text && id_text->find('@') != std::string::npos)
            reason = "id \"" + shown + "\" is not a package key";
        else if (kind == Kind::oalang)
            reason = "id \"" + shown + "\" is not a language tag";
        else
            reason = "id \"" + shown + "\" is not lower-case kebab-case";
        return false;
    }
    package.id = *id_text;
    if (!require_text(value, "name", 1, 64, package.name, reason))
        return false;
    const json::Json* release = value.find("release");
    const auto release_number = release ? release->integer() : std::nullopt;
    if (!release_number || *release_number < 1 || *release_number > max_release) {
        reason = "release is not a whole number from 1 to 2147483647";
        return false;
    }
    package.release = *release_number;
    const json::Json* size = value.find("size");
    const auto size_number = size ? size->integer() : std::nullopt;
    if (!size_number || *size_number < 1 || *size_number > max_package_bytes) {
        reason = "size is not a whole number of bytes from 1 to 4294967296";
        return false;
    }
    package.size = static_cast<uint64_t>(*size_number);
    const json::Json* hash = value.find("sha256");
    const std::string* hash_text = hash ? hash->string() : nullptr;
    if (!hash_text || !lower_hex(*hash_text)) {
        reason = "sha256 is not 64 lower-case hex digits";
        return false;
    }
    package.sha256 = *sha::parse_hex(*hash_text);
    const json::Json* file = value.find("file");
    const std::string* file_text = file ? file->string() : nullptr;
    if (!file_text || !valid_reference(*file_text)) {
        reason = "file is not a reference";
        return false;
    }
    if (!file_named(*file_text, package.sha256, kind)) {
        reason = "file not named by sha256";
        return false;
    }
    package.file = *file_text;
    if (kind == Kind::oamod || kind == Kind::expansion || kind == Kind::oamut ||
        kind == Kind::oabrain || kind == Kind::oakeys) {
        if (!require_text(value, "version", 1, 32, package.version, reason))
            return false;
    } else if (!optional_text(value, "version", 1, 32, package.version, reason)) {
        return false;
    }
    if ((kind == Kind::oamut || kind == Kind::oabrain || kind == Kind::oakeys) &&
        !oamut::parse_version(package.version)) {
        reason = "version is not a mutator version";
        return false;
    }
    const json::Json* revision = value.find("revision");
    if (kind == Kind::oamod || revision) {
        const auto number = revision ? revision->integer() : std::nullopt;
        if (!number || *number < 1 || *number > 65535) {
            reason = "revision is not a whole number from 1 to 65535";
            return false;
        }
        package.revision = static_cast<int32_t>(*number);
    }
    if (!optional_text(value, "publisher", 1, 64, package.publisher, reason) ||
        !optional_text(value, "author", 1, 64, package.author, reason) ||
        !optional_text(value, "summary", 0, 200, package.summary, reason) ||
        !optional_text(value, "notes", 0, 2000, package.notes, reason))
        return false;
    if (const json::Json* homepage = value.find("homepage")) {
        const std::string* text = homepage->string();
        if (!text || !oamod::homepage_valid(*text)) {
            reason = "homepage is not an address the game can open";
            return false;
        }
        package.homepage = *text;
    }
    if (!read_text_list(value, "tags", 16, false, package.tags, reason))
        return false;
    if (const json::Json* badge = value.find("badge")) {
        const std::string* text = badge->string();
        const bool png =
            text && text->size() >= 4 && text->compare(text->size() - 4, 4, ".png") == 0;
        if (!text || !valid_reference(*text) || !png) {
            reason = "badge is not a picture reference";
            return false;
        }
        package.badge = *text;
    }
    if (const json::Json* published = value.find("published")) {
        const std::string* text = published->string();
        if (!text || !parse_day(*text)) {
            reason = "published is not a day written YYYY-MM-DD";
            return false;
        }
        package.published = *text;
    }
    if (!read_requires(value, package, reason))
        return false;
    if (!read_content_lists(value, package, reason))
        return false;
    if (const json::Json* sim_hash = value.find("sim_hash")) {
        const std::string* text = sim_hash->string();
        const auto digest = text ? sha::parse_hex(*text) : std::nullopt;
        if (!digest) {
            reason = "sim_hash is not 64 hex digits";
            return false;
        }
        package.sim_hash = *digest;
    }
    if (!read_hacks(value, package, reason))
        return false;
    if (kind == Kind::oalang && !read_language(value, package, reason))
        return false;
    if (kind == Kind::oamap) {
        const json::Json* maps = value.find("maps");
        if (!maps || maps->type() != json::JsonType::array || maps->elements().empty()) {
            reason = "map pack names no map";
            return false;
        }
        for (const json::Json& map : maps->elements()) {
            MapEntry entry;
            if (!read_map(map, package.id, entry, reason))
                return false;
            package.maps.push_back(std::move(entry));
        }
    }
    if (kind == Kind::expansion && !read_expansion_lists(value, package, reason))
        return false;
    if (kind == Kind::oamut && !read_mutator_facts(value, package, reason))
        return false;
    if (kind == Kind::oabrain && (!read_works_with_and_options(value, package, reason) ||
                                  !read_brain_facts(value, package, reason)))
        return false;
    if (kind == Kind::oakeys &&
        (!read_works_with(value, package, reason) || !read_key_facts(value, package, reason)))
        return false;
    return true;
}

/// Keeps a skip reason until max_problems reasons are kept.
void keep_problem(Catalogue& catalogue, std::size_t index, std::string_view reason) {
    if (catalogue.problems.size() >= max_problems)
        return;
    catalogue.problems.push_back("packages[" + std::to_string(index) + "]: " + std::string(reason));
}

/// The package key folded to lower case, for the duplicate check.
std::string fold_key(std::string_view id) {
    std::string folded(id);
    for (char& value : folded) {
        if (value >= 'A' && value <= 'Z')
            value = static_cast<char>(value - 'A' + 'a');
    }
    return folded;
}

/// Reads the signing keys. Unknown members of a key object are ignored.
bool read_keys(
    const json::Json& array, std::vector<registry::RegistryKey>& out, std::string& error
) {
    if (array.elements().size() > registry::max_registry_keys) {
        error = "keys names more than 8";
        return false;
    }
    for (std::size_t index = 0; index < array.elements().size(); ++index) {
        const json::Json& value = array.elements()[index];
        const std::string where = "keys[" + std::to_string(index) + "]";
        if (value.type() != json::JsonType::object) {
            error = where + " is not an object";
            return false;
        }
        const json::Json* id_value = value.find("id");
        const json::Json* public_value = value.find("public");
        const std::string* id_text = id_value ? id_value->string() : nullptr;
        const std::string* public_text = public_value ? public_value->string() : nullptr;
        if (!id_text || !registry::valid_key_id(*id_text)) {
            error = where + " has no key id";
            return false;
        }
        const auto parsed = public_text ? signing::parse_public_key(*public_text) : std::nullopt;
        if (!parsed) {
            error = where + " is not a public key";
            return false;
        }
        for (const registry::RegistryKey& existing : out) {
            if (existing.id == *id_text) {
                error = where + " repeats an id";
                return false;
            }
            if (existing.key == *parsed) {
                error = where + " repeats a public key";
                return false;
            }
        }
        out.push_back(registry::RegistryKey{*id_text, *parsed});
    }
    return true;
}

/// Reads the mirror list. Each address is http or https and has no query.
bool read_mirrors(const json::Json& array, std::vector<url::Url>& out, std::string& error) {
    if (array.elements().size() > max_mirrors) {
        error = "mirrors names more than 8";
        return false;
    }
    for (std::size_t index = 0; index < array.elements().size(); ++index) {
        const std::string* text = array.elements()[index].string();
        const auto parsed = text ? url::parse_http_url(*text) : std::nullopt;
        if (!text || !parsed || url::has_query(*parsed)) {
            error = "mirrors[" + std::to_string(index) +
                    "] is not an http or https address without a query";
            return false;
        }
        out.push_back(*parsed);
    }
    return true;
}

/// Reads one UTC member into the text and the seconds.
bool read_time(
    const json::Json& object,
    std::string_view name,
    std::string& text_out,
    int64_t& seconds,
    std::string& error
) {
    const json::Json* value = object.find(name);
    const std::string* text = value ? value->string() : nullptr;
    const auto parsed = text ? parse_utc_time(*text) : std::nullopt;
    if (!parsed) {
        error = std::string(name) + " is not a UTC time";
        return false;
    }
    text_out = *text;
    seconds = *parsed;
    return true;
}

/// One package that passed its own rules, with its index in the array.
struct Kept {
    Package package;
    std::size_t index = 0;
};

} // namespace

bool read_catalogue(std::span<const uint8_t> bytes, Catalogue& out, std::string* error) {
    json::JsonError json_error;
    const auto root = json::parse_json(bytes_as_text(bytes), json_error);
    if (!root)
        return refuse(error, "the catalogue is not JSON");
    if (root->type() != json::JsonType::object)
        return refuse(error, "the catalogue is not a JSON object");
    if (!members_unique(*root))
        return refuse(error, "a member is named twice");

    Catalogue loaded;
    const json::Json* version = root->find("catalogue");
    const auto version_number = version ? version->integer() : std::nullopt;
    if (!version_number || *version_number != catalogue_version)
        return refuse(error, "catalogue is not 1");
    const json::Json* registry_value = root->find("registry");
    const std::string* registry_text = registry_value ? registry_value->string() : nullptr;
    if (!registry_text || !registry::valid_registry_id(*registry_text))
        return refuse(error, "registry is not a registry id");
    loaded.registry = *registry_text;
    const json::Json* sequence = root->find("sequence");
    if (!sequence)
        return refuse(error, "sequence is missing");
    const auto sequence_number = sequence->integer();
    if (!sequence_number || *sequence_number < 0)
        return refuse(error, "sequence is not a whole number at least 0");
    loaded.sequence = *sequence_number;
    std::string time_error;
    if (!read_time(*root, "generated", loaded.generated_text, loaded.generated, time_error) ||
        !read_time(*root, "expires", loaded.expires_text, loaded.expires, time_error))
        return refuse(error, std::move(time_error));
    if (loaded.expires <= loaded.generated)
        return refuse(error, "expires is not after generated");
    if (const json::Json* downloads = root->find("downloads")) {
        const std::string* text = downloads->string();
        const auto parsed = text ? url::parse_http_url(*text) : std::nullopt;
        if (!parsed)
            return refuse(error, "downloads is not an http or https address");
        loaded.downloads = url::url_text(*parsed);
    }
    if (const json::Json* keys = root->find("keys")) {
        if (keys->type() != json::JsonType::array)
            return refuse(error, "keys is not a list");
        std::string key_error;
        if (!read_keys(*keys, loaded.keys, key_error))
            return refuse(error, std::move(key_error));
    }
    if (const json::Json* mirrors = root->find("mirrors")) {
        if (mirrors->type() != json::JsonType::array)
            return refuse(error, "mirrors is not a list");
        std::string mirror_error;
        if (!read_mirrors(*mirrors, loaded.mirrors, mirror_error))
            return refuse(error, std::move(mirror_error));
    }
    if (const json::Json* icon = root->find("icon")) {
        const std::string* text = icon->string();
        const bool png =
            text && text->size() >= 4 && text->compare(text->size() - 4, 4, ".png") == 0;
        if (!text || !valid_reference(*text) || !png)
            return refuse(error, "icon is not a picture reference");
        loaded.icon = *text;
    }
    const json::Json* packages = root->find("packages");
    if (!packages || packages->type() != json::JsonType::array)
        return refuse(error, "packages is missing");
    if (packages->elements().size() > max_packages)
        return refuse(error, "packages lists more than 20000");

    std::vector<Kept> kept;
    kept.reserve(packages->elements().size());
    for (std::size_t index = 0; index < packages->elements().size(); ++index) {
        Package package;
        std::string reason;
        if (!read_package(packages->elements()[index], package, reason)) {
            keep_problem(loaded, index, reason);
            continue;
        }
        kept.push_back(Kept{std::move(package), index});
    }
    std::map<std::string, std::vector<std::size_t>> by_key;
    for (std::size_t index = 0; index < kept.size(); ++index)
        by_key[fold_key(kept[index].package.id)].push_back(index);
    std::vector<bool> drop(kept.size());
    for (const auto& [key, indexes] : by_key) {
        if (indexes.size() < 2)
            continue;
        for (const std::size_t index : indexes) {
            drop[index] = true;
            keep_problem(loaded, kept[index].index, "same key \"" + key + "\"");
        }
    }
    for (std::size_t index = 0; index < kept.size(); ++index) {
        if (!drop[index])
            loaded.packages.push_back(std::move(kept[index].package));
    }
    out = std::move(loaded);
    return true;
}

} // namespace oa::data::catalogue
