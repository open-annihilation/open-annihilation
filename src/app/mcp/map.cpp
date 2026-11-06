// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// map.info, map.visibility and map.radar (map.hpp). The match view copies
// the map, one player's sight grids and that player's radar and sonar
// contacts. These tools are what a caller receives. Another player's grids
// and contacts are refused unless the call asks for full awareness and the
// match is not shared.
#include "map.hpp"

#include "oa/formats/json.hpp"
#include "oa/formats/png.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::mcp {
namespace {

namespace json = oa::formats::json;

constexpr std::string_view kInfoSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"cell\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8192}}}";

constexpr std::string_view kVisibilitySchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"layer\"],"
    "\"properties\":{"
    "\"player\":{\"type\":\"string\"},"
    "\"layer\":{\"type\":\"string\",\"enum\":[\"sight\",\"explored\",\"radar\",\"sonar\","
    "\"jammed\"]},"
    "\"cell\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8192},"
    "\"format\":{\"type\":\"string\",\"enum\":[\"png\",\"rle\"]},"
    "\"awareness\":{\"type\":\"string\",\"enum\":[\"player\",\"full\"]}}}";

constexpr std::string_view kRadarSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"player\":{\"type\":\"string\"},"
    "\"awareness\":{\"type\":\"string\",\"enum\":[\"player\",\"full\"]}}}";

/// The most cells a PNG of a visibility grid may hold.
constexpr int64_t kPngCells = 256LL * 256LL;

/// One plot, in map pixels. The height grid uses this cell when a call omits one.
constexpr int32_t kPlotCell = 16;

/// A failed tool answer.
///
/// @param why the failure, as the caller reads it
/// @return the answer
ToolReply fail(std::string why) {
    ToolReply reply;
    reply.failed = true;
    reply.text = std::move(why);
    return reply;
}

/// A finished tool answer.
///
/// @param text the JSON object
/// @return the answer
ToolReply ok(std::string text) {
    ToolReply reply;
    reply.text = std::move(text);
    return reply;
}

/// The running match, as the host published it for this call.
struct Bound {
    const oa::app::MatchView* table{}; ///< the published view
    uint32_t tick{};                   ///< the match's tick
    uint8_t viewer{};                  ///< the local player
    bool full_allowed{};               ///< full awareness is allowed
};

/// Binds a call to the published view.
///
/// @param host the view
/// @param[out] bound the binding
/// @return the failure, or empty when a match is running
std::string bind_match(const UnitHost* host, Bound& bound) {
    if (host == nullptr || host->table.active == nullptr ||
        !host->table.active(host->table.context))
        return "no match is running";
    bound.table = &host->table;
    if (host->table.tick != nullptr)
        bound.tick = host->table.tick(host->table.context);
    if (host->table.viewer != nullptr)
        bound.viewer = host->table.viewer(host->table.context);
    if (host->table.full_awareness_allowed != nullptr)
        bound.full_allowed = host->table.full_awareness_allowed(host->table.context);
    return {};
}

/// Reads the call's awareness.
///
/// @param args the arguments
/// @param allowed whether the match allows full awareness
/// @param[out] full set when the call asks for full awareness
/// @return the failure, or empty
std::string awareness_failure(const json::Json& args, bool allowed, bool& full) {
    full = false;
    const json::Json* value = args.find("awareness");
    if (value == nullptr)
        return {};
    const std::string* text = value->string();
    if (text == nullptr || (*text != "player" && *text != "full"))
        return "awareness must be player or full";
    if (*text == "full") {
        if (!allowed)
            return "full awareness is only available in a single-player match";
        full = true;
    }
    return {};
}

/// Folds one ASCII letter.
///
/// @param byte the byte
/// @return the folded byte
unsigned char fold_ascii(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

/// Compares two words without regard to ASCII case.
///
/// @param first one word
/// @param second the other
/// @return true when they are the same word
bool same_word(std::string_view first, std::string_view second) {
    if (first.size() != second.size())
        return false;
    for (std::size_t cursor = 0; cursor < first.size(); ++cursor) {
        const unsigned char one = fold_ascii(static_cast<unsigned char>(first[cursor]));
        const unsigned char other = fold_ascii(static_cast<unsigned char>(second[cursor]));
        if (one != other)
            return false;
    }
    return true;
}

/// The arguments, or a failure when the call has none.
///
/// @param call the call
/// @param[out] args the arguments
/// @return the failure, or empty
std::string arguments_of(const ToolCall& call, const json::Json*& args) {
    if (call.arguments == nullptr)
        return "arguments must be an object";
    args = call.arguments;
    return {};
}

/// Reads a whole number that fits in 32 bits.
///
/// @param value the JSON number
/// @return the number, or nothing when it does not fit
std::optional<int32_t> fit_int32(const json::Json& value) {
    const std::optional<int64_t> number = value.integer();
    if (!number)
        return std::nullopt;
    if (*number < 1 || *number > 8192)
        return std::nullopt;
    return static_cast<int32_t>(*number);
}

/// Which player a call names.
struct PlayerPick {
    uint8_t index{};  ///< the player, 0 to 9
    bool mine = true; ///< the local player, including when the call names that number
};

/// Reads the call's player. An omitted player is the local one.
///
/// @param args the arguments
/// @param viewer the local player
/// @param[out] pick the player
/// @return the failure, or empty
std::string read_player(const json::Json& args, uint8_t viewer, PlayerPick& pick) {
    pick.index = viewer;
    pick.mine = true;
    const json::Json* value = args.find("player");
    if (value == nullptr)
        return {};
    const std::string* text = value->string();
    if (text == nullptr)
        return "player must be me or a player from 0 to 9";
    if (same_word(*text, "me") || text->empty())
        return {};
    if (text->size() == 1 && (*text)[0] >= '0' && (*text)[0] <= '9') {
        pick.index = static_cast<uint8_t>((*text)[0] - '0');
        pick.mine = pick.index == viewer;
        return {};
    }
    return "player must be me or a player from 0 to 9";
}

/// Refuses another player's view when full awareness was not asked for and allowed.
///
/// @param pick the player
/// @param full whether the call asked for full awareness
/// @return the failure, or empty
std::string other_player_failure(const PlayerPick& pick, bool full) {
    if (!pick.mine && !full)
        return "another player's view needs full awareness";
    return {};
}

/// Reads a cell size. An omitted cell is the default.
///
/// @param args the arguments
/// @param fallback the cell used when the call omits it
/// @param[out] cell the cell size, in map pixels
/// @return the failure, or empty
std::string read_cell(const json::Json& args, int32_t fallback, int32_t& cell) {
    cell = fallback;
    const json::Json* value = args.find("cell");
    if (value == nullptr)
        return {};
    const std::optional<int32_t> number = fit_int32(*value);
    if (!number)
        return "cell must be a whole number of map pixels from 1 to 8192";
    cell = *number;
    return {};
}

/// Writes a run-length grid, row by row. Each run is a count and a 0 or 1.
///
/// @param writer the JSON being written
/// @param cells the grid
void write_runs(json::JsonWriter& writer, const std::vector<uint8_t>& cells) {
    writer.begin_array();
    std::size_t index = 0;
    while (index < cells.size()) {
        const int64_t value = cells[index] != 0 ? 1 : 0;
        std::size_t count = 1;
        while (index + count < cells.size() && (cells[index + count] != 0 ? 1 : 0) == value)
            ++count;
        writer.begin_array();
        writer.integer(static_cast<int64_t>(count));
        writer.integer(value);
        writer.end_array();
        index += count;
    }
    writer.end_array();
}

/// Encodes a visibility grid as a grey PNG, 255 where the cell is on.
///
/// @param grid the grid
/// @param[out] png the file
/// @return false when the grid cannot be encoded
bool encode_png(const oa::app::VisibilityGrid& grid, std::vector<uint8_t>& png) {
    if (grid.width <= 0 || grid.height <= 0)
        return false;
    const auto area = static_cast<std::size_t>(grid.width) * static_cast<std::size_t>(grid.height);
    if (grid.cells.size() != area)
        return false;
    std::vector<uint8_t> rows(area);
    for (std::size_t index = 0; index < area; ++index)
        rows[index] = grid.cells[index] != 0 ? 255 : 0;
    oa::formats::png::Header header;
    header.width = static_cast<uint32_t>(grid.width);
    header.height = static_cast<uint32_t>(grid.height);
    header.bit_depth = 8;
    header.color_type = oa::formats::png::ColorType::grey;
    header.interlace = oa::formats::png::Interlace::none;
    oa::formats::png::Image image;
    image.header = header;
    image.rows = rows;
    return oa::formats::png::write_stored(image, &png);
}

/// Answers map.info.
///
/// @param context the UnitHost
/// @param call the arguments
/// @return the map
ToolReply read_info(void* context, const ToolCall& call) {
    const json::Json* args = nullptr;
    if (const std::string why = arguments_of(call, args); !why.empty())
        return fail(why);
    Bound bound;
    if (const std::string why = bind_match(static_cast<const UnitHost*>(context), bound);
        !why.empty())
        return fail(why);
    if (bound.table->map_info == nullptr)
        return fail("no match is running");
    int32_t cell = kPlotCell;
    if (const std::string why = read_cell(*args, cell, cell); !why.empty())
        return fail(why);
    oa::app::MapInfo info;
    if (!bound.table->map_info(bound.table->context, cell, &info))
        return fail("the map could not be read at that cell");
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("tick");
    writer.integer(static_cast<int64_t>(bound.tick));
    writer.key("name");
    writer.string(info.name);
    writer.key("width");
    writer.integer(info.width);
    writer.key("height");
    writer.integer(info.height);
    writer.key("plots_x");
    writer.integer(info.plots_x);
    writer.key("plots_z");
    writer.integer(info.plots_z);
    writer.key("sea");
    writer.integer(info.sea);
    writer.key("cell");
    writer.integer(info.cell);
    writer.key("cells_x");
    writer.integer(info.cells_x);
    writer.key("cells_z");
    writer.integer(info.cells_z);
    writer.key("heights");
    writer.begin_array();
    for (const uint8_t sample : info.heights)
        writer.integer(sample);
    writer.end_array();
    writer.key("metal");
    writer.begin_array();
    for (const oa::app::MetalSpot& spot : info.metal) {
        writer.begin_object();
        writer.key("x");
        writer.integer(spot.x);
        writer.key("z");
        writer.integer(spot.z);
        writer.key("amount");
        writer.integer(spot.amount);
        writer.end_object();
    }
    writer.end_array();
    writer.key("features");
    writer.begin_array();
    for (const oa::app::MapFeature& feature : info.features) {
        writer.begin_object();
        writer.key("name");
        writer.string(feature.name);
        writer.key("x");
        writer.integer(feature.x);
        writer.key("z");
        writer.integer(feature.z);
        writer.key("footprint_x");
        writer.integer(feature.footprint_x);
        writer.key("footprint_z");
        writer.integer(feature.footprint_z);
        writer.end_object();
    }
    writer.end_array();
    writer.key("features_truncated");
    writer.boolean(info.features_truncated);
    writer.end_object();
    return ok(writer.text());
}

/// The layer a call names.
///
/// @param text the layer's name
/// @param[out] layer the layer
/// @return false when the name is not a layer
bool layer_named(std::string_view text, oa::app::VisibilityLayer& layer) {
    if (text == "sight")
        layer = oa::app::VisibilityLayer::sight;
    else if (text == "explored")
        layer = oa::app::VisibilityLayer::explored;
    else if (text == "radar")
        layer = oa::app::VisibilityLayer::radar;
    else if (text == "sonar")
        layer = oa::app::VisibilityLayer::sonar;
    else if (text == "jammed")
        layer = oa::app::VisibilityLayer::jammed;
    else
        return false;
    return true;
}

/// Answers map.visibility.
///
/// @param context the UnitHost
/// @param call the arguments
/// @return the grid
ToolReply read_visibility(void* context, const ToolCall& call) {
    const json::Json* args = nullptr;
    if (const std::string why = arguments_of(call, args); !why.empty())
        return fail(why);
    Bound bound;
    if (const std::string why = bind_match(static_cast<const UnitHost*>(context), bound);
        !why.empty())
        return fail(why);
    if (bound.table->visibility == nullptr)
        return fail("no match is running");
    bool full = false;
    if (const std::string why = awareness_failure(*args, bound.full_allowed, full); !why.empty())
        return fail(why);
    PlayerPick pick;
    if (const std::string why = read_player(*args, bound.viewer, pick); !why.empty())
        return fail(why);
    if (const std::string why = other_player_failure(pick, full); !why.empty())
        return fail(why);
    const json::Json* layer_value = args->find("layer");
    const std::string* layer_text = layer_value != nullptr ? layer_value->string() : nullptr;
    oa::app::VisibilityLayer layer = oa::app::VisibilityLayer::sight;
    if (layer_text == nullptr || !layer_named(*layer_text, layer))
        return fail("layer must be sight, explored, radar, sonar or jammed");
    int32_t cell = 32;
    if (const std::string why = read_cell(*args, cell, cell); !why.empty())
        return fail(why);
    bool as_png = false;
    if (const json::Json* format = args->find("format")) {
        const std::string* text = format->string();
        if (text == nullptr || (*text != "png" && *text != "rle"))
            return fail("format must be png or rle");
        as_png = *text == "png";
    }
    oa::app::VisibilityGrid grid;
    if (!bound.table->visibility(bound.table->context, pick.index, layer, cell, &grid))
        return fail("the visibility grid could not be read at that cell");
    const auto area = static_cast<int64_t>(grid.width) * static_cast<int64_t>(grid.height);
    if (grid.width <= 0 || grid.height <= 0 || grid.cells.size() != static_cast<std::size_t>(area))
        return fail("the visibility grid could not be read at that cell");
    std::vector<uint8_t> png;
    if (as_png) {
        if (area > kPngCells)
            return fail("png needs a coarser cell");
        if (!encode_png(grid, png))
            return fail("the visibility grid could not be read at that cell");
    }
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("tick");
    writer.integer(static_cast<int64_t>(bound.tick));
    writer.key("player");
    writer.integer(pick.index);
    writer.key("layer");
    writer.string(*layer_text);
    writer.key("cell");
    writer.integer(grid.cell);
    writer.key("w");
    writer.integer(grid.width);
    writer.key("h");
    writer.integer(grid.height);
    if (as_png) {
        writer.key("png");
        writer.string(json::base64_text(png));
    } else {
        writer.key("rle");
        write_runs(writer, grid.cells);
    }
    if (!grid.minimap.empty() && grid.minimap.size() == grid.cells.size()) {
        writer.key("minimap");
        write_runs(writer, grid.minimap);
    }
    writer.end_object();
    ToolReply reply = ok(writer.text());
    reply.png = std::move(png);
    return reply;
}

/// Answers map.radar.
///
/// @param context the UnitHost
/// @param call the arguments
/// @return the contacts
ToolReply read_radar(void* context, const ToolCall& call) {
    const json::Json* args = nullptr;
    if (const std::string why = arguments_of(call, args); !why.empty())
        return fail(why);
    Bound bound;
    if (const std::string why = bind_match(static_cast<const UnitHost*>(context), bound);
        !why.empty())
        return fail(why);
    if (bound.table->contacts == nullptr)
        return fail("no match is running");
    bool full = false;
    if (const std::string why = awareness_failure(*args, bound.full_allowed, full); !why.empty())
        return fail(why);
    PlayerPick pick;
    if (const std::string why = read_player(*args, bound.viewer, pick); !why.empty())
        return fail(why);
    if (const std::string why = other_player_failure(pick, full); !why.empty())
        return fail(why);
    std::vector<oa::app::RadarContact> contacts;
    bool kept = false;
    if (!bound.table->contacts(bound.table->context, pick.index, &contacts, &kept))
        return fail("no match is running");
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("tick");
    writer.integer(static_cast<int64_t>(bound.tick));
    writer.key("player");
    writer.integer(pick.index);
    writer.key("kept");
    writer.boolean(kept);
    writer.key("contacts");
    writer.begin_array();
    for (const oa::app::RadarContact& contact : contacts) {
        writer.begin_object();
        writer.key("x");
        writer.integer(contact.x);
        writer.key("y");
        writer.integer(contact.y);
        writer.key("z");
        writer.integer(contact.z);
        writer.key("radar");
        writer.boolean(contact.radar);
        writer.key("sonar");
        writer.boolean(contact.sonar);
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    return ok(writer.text());
}

} // namespace

void add_map_tools(Session& session, UnitHost& host) {
    Tool info;
    info.name = "map.info";
    info.description =
        "Reads the map: its name, its size in map pixels, the sea level, the height of each "
        "cell, the metal in those cells and the features. cell is the cell size in map pixels, "
        "16 when omitted, one plot. A cell that would make the height grid too large is refused.";
    info.input_schema = std::string(kInfoSchema);
    info.reads = true;
    info.context = &host;
    info.call = read_info;
    session.add_tool(std::move(info));

    Tool visibility;
    visibility.name = "map.visibility";
    visibility.description =
        "Reads one player's sight, explored, radar, sonar or jammed grid. cell is the cell "
        "size in map pixels, 32 when omitted, one sight cell, which is the cell the minimap's "
        "fog is drawn from. format is rle or png. player is me or a player from 0 to 9. "
        "Another player's grid needs awareness full, and only in a single-player match. "
        "For sight and explored, minimap is the minimap's own fog for the same cells.";
    visibility.input_schema = std::string(kVisibilitySchema);
    visibility.reads = true;
    visibility.context = &host;
    visibility.call = read_visibility;
    session.add_tool(std::move(visibility));

    Tool radar;
    radar.name = "map.radar";
    radar.description =
        "Reads the radar and sonar contacts the player has: each contact's position in map "
        "pixels, and whether it is radar, sonar or both. A contact has no identity. player "
        "is me or a player from 0 to 9. Another player's contacts need awareness full, and "
        "only in a single-player match. kept is false when that player's picture is not kept, "
        "and the contacts are then empty.";
    radar.input_schema = std::string(kRadarSchema);
    radar.reads = true;
    radar.context = &host;
    radar.call = read_radar;
    session.add_tool(std::move(radar));
}

} // namespace oa::app::mcp
