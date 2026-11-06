// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The map tools with a stand-in match view: the map, one player's sight
// grid as runs and as a PNG, and radar contacts that carry no identity.
// Another player's grid and contacts are refused unless the call asks for
// full awareness and the match allows it.
#include "map.hpp"

#include "oa/formats/json.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace json = oa::formats::json;
namespace mcp = oa::app::mcp;

/// The stand-in match the tools read.
struct Picture {
    bool running = true;
    bool allowed = true;
    uint32_t tick = 40;
    uint8_t viewer = 0;
    oa::app::MapInfo info{};
    oa::app::VisibilityGrid mine{};
    oa::app::VisibilityGrid theirs{};
    std::vector<oa::app::RadarContact> contacts;
    bool kept = true;
};

/// Parses one JSON text. A failure is a failed check and an empty result.
///
/// @param text the text
/// @return the value
std::optional<json::Json> parsed(std::string_view text) {
    json::JsonError failure;
    std::optional<json::Json> value = json::parse_json(text, failure);
    OA_CHECK(value.has_value());
    return value;
}

/// One request.
///
/// @param id the id, as JSON
/// @param method the method
/// @param params the params object, as JSON; empty omits params
/// @return the request
std::string request(std::string_view id, std::string_view method, std::string_view params = {}) {
    std::string text = "{\"jsonrpc\":\"2.0\",\"id\":";
    text.append(id);
    text.append(",\"method\":\"");
    text.append(method);
    text.push_back('"');
    if (!params.empty()) {
        text.append(",\"params\":");
        text.append(params);
    }
    text.push_back('}');
    return text;
}

bool picture_active(void* context) {
    return static_cast<Picture*>(context)->running;
}

uint32_t picture_tick(void* context) {
    return static_cast<Picture*>(context)->tick;
}

uint8_t picture_viewer(void* context) {
    return static_cast<Picture*>(context)->viewer;
}

bool picture_allowed(void* context) {
    return static_cast<Picture*>(context)->allowed;
}

bool picture_map(void* context, int32_t cell, oa::app::MapInfo* info) {
    *info = static_cast<Picture*>(context)->info;
    info->cell = cell;
    return true;
}

bool picture_visibility(
    void* context,
    uint8_t player,
    oa::app::VisibilityLayer /*layer*/,
    int32_t cell,
    oa::app::VisibilityGrid* grid
) {
    const Picture* picture = static_cast<const Picture*>(context);
    *grid = player == picture->viewer ? picture->mine : picture->theirs;
    grid->cell = cell;
    return true;
}

bool picture_contacts(
    void* context, uint8_t player, std::vector<oa::app::RadarContact>* listed, bool* kept
) {
    const Picture* picture = static_cast<const Picture*>(context);
    listed->clear();
    if (player == picture->viewer)
        *listed = picture->contacts;
    *kept = picture->kept;
    return true;
}

/// Binds the stand-in to a host.
///
/// @param picture the stand-in
/// @param[out] host the host the tools read
void bind_picture(Picture& picture, mcp::UnitHost& host) {
    host.table.context = &picture;
    host.table.active = picture_active;
    host.table.tick = picture_tick;
    host.table.viewer = picture_viewer;
    host.table.full_awareness_allowed = picture_allowed;
    host.table.map_info = picture_map;
    host.table.visibility = picture_visibility;
    host.table.contacts = picture_contacts;
}

/// A session that has finished initialize, with the map tools installed.
///
/// @param host the stand-in host
/// @return the session
mcp::Session open_session(mcp::UnitHost& host) {
    mcp::Session session(mcp::SessionConfig{});
    mcp::add_map_tools(session, host);
    (void)session.receive(request("1", "initialize"));
    (void)session.receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    return session;
}

/// The one answer a message produced, parsed.
///
/// @param session the session
/// @param message the message
/// @return the answer, or nothing when it was not one JSON object
std::optional<json::Json> only(mcp::Session& session, std::string_view message) {
    const std::vector<std::string> lines = session.receive(message);
    OA_CHECK(lines.size() == 1);
    if (lines.size() != 1)
        return std::nullopt;
    return parsed(lines[0]);
}

/// The text of a tool result.
///
/// @param answer the tools/call answer
/// @return the text, or nothing
std::optional<std::string> tool_text(const json::Json& answer) {
    const json::Json* result = answer.find("result");
    OA_CHECK(result != nullptr);
    if (result == nullptr)
        return std::nullopt;
    const json::Json* content = result->find("content");
    OA_CHECK(content != nullptr && !content->elements().empty());
    if (content == nullptr || content->elements().empty())
        return std::nullopt;
    const json::Json* text = content->elements().front().find("text");
    OA_CHECK(text != nullptr && text->string() != nullptr);
    if (text == nullptr || text->string() == nullptr)
        return std::nullopt;
    return *text->string();
}

/// The text of a successful tool result, parsed.
///
/// @param answer the tools/call answer
/// @return the text's JSON, or nothing
std::optional<json::Json> tool_body(const json::Json& answer) {
    const std::optional<std::string> text = tool_text(answer);
    if (!text)
        return std::nullopt;
    return parsed(*text);
}

/// Tells whether a tool result is a failure.
///
/// @param answer the tools/call answer
/// @return true when isError is set
bool tool_failed(const json::Json& answer) {
    const json::Json* result = answer.find("result");
    if (result == nullptr)
        return false;
    const json::Json* flag = result->find("isError");
    return flag != nullptr && flag->boolean().value_or(false);
}

/// A grid of two by two, on at the first cell and the last.
///
/// @param with_minimap copy the cells into the minimap reading
/// @return the grid
oa::app::VisibilityGrid sample_grid(bool with_minimap) {
    oa::app::VisibilityGrid grid;
    grid.width = 2;
    grid.height = 2;
    grid.cell = 32;
    grid.cells = {1, 0, 0, 1};
    if (with_minimap)
        grid.minimap = grid.cells;
    return grid;
}

/// The map, and a call while no match is running.
void check_info() {
    Picture picture;
    picture.info.name = "Coast";
    picture.info.width = 64;
    picture.info.height = 32;
    picture.info.plots_x = 4;
    picture.info.plots_z = 2;
    picture.info.sea = 80;
    picture.info.cells_x = 2;
    picture.info.cells_z = 2;
    picture.info.heights = {1, 2, 3, 4};
    picture.info.metal.push_back(oa::app::MetalSpot{1, 0, 12});
    oa::app::MapFeature tree;
    tree.name = "Tree";
    tree.x = 16;
    tree.z = 0;
    tree.footprint_x = 1;
    tree.footprint_z = 1;
    picture.info.features.push_back(tree);
    mcp::UnitHost host;
    bind_picture(picture, host);
    mcp::Session session = open_session(host);

    if (const std::optional<json::Json> answer =
            only(session, request("2", "tools/call", "{\"name\":\"map.info\"}"))) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* name = body->find("name");
            const json::Json* heights = body->find("heights");
            const json::Json* metal = body->find("metal");
            const json::Json* features = body->find("features");
            const json::Json* across = body->find("cells_x");
            const json::Json* down = body->find("cells_z");
            const json::Json* cell = body->find("cell");
            OA_CHECK(name != nullptr && name->string() && *name->string() == "Coast");
            OA_CHECK(cell != nullptr && cell->integer() && *cell->integer() == 16);
            OA_CHECK(across != nullptr && down != nullptr && heights != nullptr);
            if (across != nullptr && down != nullptr && across->integer() && down->integer() &&
                heights != nullptr)
                OA_CHECK(
                    heights->elements().size() ==
                    static_cast<std::size_t>(*across->integer() * *down->integer())
                );
            OA_CHECK(metal != nullptr && metal->elements().size() == 1);
            OA_CHECK(features != nullptr && features->elements().size() == 1);
            if (features != nullptr && !features->elements().empty()) {
                const json::Json* feature = features->elements().front().find("name");
                OA_CHECK(feature != nullptr && feature->string() && *feature->string() == "Tree");
            }
        }
    }

    picture.running = false;
    if (const std::optional<json::Json> answer =
            only(session, request("3", "tools/call", "{\"name\":\"map.info\"}"))) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(*text == "no match is running");
    }
}

/// Runs of the sample grid, a PNG, and the refusal of another player's view.
void check_visibility() {
    Picture picture;
    picture.mine = sample_grid(true);
    picture.theirs = sample_grid(false);
    picture.theirs.cells = {0, 0, 0, 1};
    mcp::UnitHost host;
    bind_picture(picture, host);
    mcp::Session session = open_session(host);

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "2",
                "tools/call",
                "{\"name\":\"map.visibility\",\"arguments\":{\"layer\":\"sight\"}}"
            )
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* runs = body->find("rle");
            const json::Json* fog = body->find("minimap");
            OA_CHECK(runs != nullptr && fog != nullptr);
            if (runs != nullptr && fog != nullptr) {
                OA_CHECK(runs->elements().size() == 3);
                OA_CHECK(fog->elements().size() == runs->elements().size());
                if (!runs->elements().empty()) {
                    const json::Json& first = runs->elements().front();
                    OA_CHECK(first.elements().size() == 2);
                    if (first.elements().size() == 2) {
                        OA_CHECK(
                            first.elements()[0].integer() && *first.elements()[0].integer() == 1
                        );
                        OA_CHECK(
                            first.elements()[1].integer() && *first.elements()[1].integer() == 1
                        );
                    }
                }
            }
        }
    }

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "3",
                "tools/call",
                "{\"name\":\"map.visibility\",\"arguments\":{\"layer\":\"sight\",\"format\":"
                "\"png\"}}"
            )
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* png = body->find("png");
            OA_CHECK(png != nullptr && png->string() && png->string()->starts_with("iVBORw0KGgo"));
        }
        const json::Json* result = answer->find("result");
        const json::Json* content = result != nullptr ? result->find("content") : nullptr;
        OA_CHECK(content != nullptr && content->elements().size() == 2);
        if (content != nullptr && content->elements().size() == 2) {
            const json::Json& image = content->elements()[1];
            const json::Json* kind = image.find("type");
            const json::Json* mime = image.find("mimeType");
            const json::Json* data = image.find("data");
            OA_CHECK(kind != nullptr && kind->string() && *kind->string() == "image");
            OA_CHECK(mime != nullptr && mime->string() && *mime->string() == "image/png");
            OA_CHECK(
                data != nullptr && data->string() && data->string()->starts_with("iVBORw0KGgo")
            );
        }
    }

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "4",
                "tools/call",
                "{\"name\":\"map.visibility\",\"arguments\":{\"layer\":\"sight\",\"player\":\"1\"}}"
            )
        )) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(*text == "another player's view needs full awareness");
    }

    picture.allowed = false;
    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "5",
                "tools/call",
                "{\"name\":\"map.visibility\",\"arguments\":"
                "{\"layer\":\"sight\",\"player\":\"1\",\"awareness\":\"full\"}}"
            )
        )) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(*text == "full awareness is only available in a single-player match");
    }

    picture.allowed = true;
    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "6",
                "tools/call",
                "{\"name\":\"map.visibility\",\"arguments\":"
                "{\"layer\":\"explored\",\"player\":\"1\",\"awareness\":\"full\"}}"
            )
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* player = body->find("player");
            const json::Json* runs = body->find("rle");
            OA_CHECK(player != nullptr && player->integer() && *player->integer() == 1);
            OA_CHECK(runs != nullptr && runs->elements().size() == 2);
            OA_CHECK(body->find("minimap") == nullptr);
        }
    }
}

/// Contacts carry a position and no identity, and another player is refused.
void check_radar() {
    Picture picture;
    oa::app::RadarContact contact;
    contact.x = 48;
    contact.y = 3;
    contact.z = 96;
    contact.radar = true;
    contact.sonar = false;
    picture.contacts.push_back(contact);
    mcp::UnitHost host;
    bind_picture(picture, host);
    mcp::Session session = open_session(host);

    if (const std::optional<json::Json> answer =
            only(session, request("2", "tools/call", "{\"name\":\"map.radar\"}"))) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* kept = body->find("kept");
            const json::Json* contacts = body->find("contacts");
            OA_CHECK(kept != nullptr && kept->boolean() && *kept->boolean());
            OA_CHECK(contacts != nullptr && contacts->elements().size() == 1);
            if (contacts != nullptr && !contacts->elements().empty()) {
                const json::Json& one = contacts->elements().front();
                OA_CHECK(one.find("id") == nullptr);
                OA_CHECK(one.find("type") == nullptr);
                OA_CHECK(one.find("owner") == nullptr);
                const json::Json* x = one.find("x");
                const json::Json* radar = one.find("radar");
                const json::Json* sonar = one.find("sonar");
                OA_CHECK(x != nullptr && x->integer() && *x->integer() == 48);
                OA_CHECK(radar != nullptr && radar->boolean() && *radar->boolean());
                OA_CHECK(sonar != nullptr && sonar->boolean() && !*sonar->boolean());
                OA_CHECK(one.names().size() == 5);
            }
        }
    }

    if (const std::optional<json::Json> answer = only(
            session,
            request("3", "tools/call", "{\"name\":\"map.radar\",\"arguments\":{\"player\":\"1\"}}")
        )) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(*text == "another player's view needs full awareness");
    }
}

} // namespace

int main() {
    check_info();
    check_visibility();
    check_radar();
    return oa::test::check_exit_status();
}
