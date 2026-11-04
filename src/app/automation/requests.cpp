// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The requests the automation endpoint answers, and its work of each frame
// (requests.hpp).
#include "requests.hpp"

#include "controls.hpp"
#include "input.hpp"
#include "frames.hpp"
#include "serve_reports.hpp"

#include "oa/app/automation/tree.hpp"

#include "oa/app/app.hpp"
#include "oa/core/game_state.h"
#include "oa/app/frame_coordinates.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error "OA_ENGINE_VERSION names the engine's version, which hello gives"
#endif

namespace oa::app::automation {
namespace {

// The game's canvas: the 640x480 screen the menus are drawn on.
constexpr int kCanvasWidth = 640;
constexpr int kCanvasHeight = 480;
// Game ticks a second.
constexpr int64_t kTickRate = 30;

// Every operation the endpoint answers.
constexpr RequestKind kRequests[] = {
    {"hello", answer_hello},
    {"screen", answer_screen},
    {"prefs", answer_prefs},
    {"quit", answer_quit},
    {"controls", answer_controls},
    {"input", answer_input},
    {"frame", answer_frame},
    {"match", answer_match},
    {"room", answer_room},
    {"subscribe", answer_subscribe},
};

/// Has the game quit once the client's answer to quit has been written, as
/// the main menu's Exit does: a running match is left with no question
/// (AutomationHost::quit). Without that entry the game is sent the system's
/// quit, as the window's close button sends it.
///
/// @param[in,out] endpoint the endpoint
void quit_once_answered(Endpoint& endpoint, FrameStage /*stage*/) {
    if (!endpoint.take_quit())
        return;
    const AutomationHost& host = endpoint.automation_host();
    if (host.quit != nullptr) {
        host.quit(host.context);
        return;
    }
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    quit.quit.timestamp = SDL_GetTicksNS();
    (void)SDL_PushEvent(&quit);
}

// The work the endpoint does each time it is served, in order.
constexpr FrameWork kFrameWork[] = {
    quit_once_answered,
    answer_taken_input,
    serve_frame_request,
    send_frame_events,
};

/// Writes a rectangle as the protocol does: [x, y, width, height].
///
/// @param[in,out] json the JSON being written
/// @param rect the rectangle
void write_rect(JsonWriter& json, const SDL_Rect& rect) {
    json.begin_array();
    json.integer(rect.x);
    json.integer(rect.y);
    json.integer(rect.w);
    json.integer(rect.h);
    json.end_array();
}

/// Returns a path as UTF-8 text.
///
/// @param path the path
/// @return the text
std::string utf8_of(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace

std::span<const RequestKind> request_kinds() noexcept {
    return kRequests;
}

const RequestKind* find_request(std::string_view op) noexcept {
    for (const RequestKind& kind : kRequests)
        if (kind.op == op)
            return &kind;
    return nullptr;
}

std::span<const FrameWork> frame_work() noexcept {
    return kFrameWork;
}

void answer_hello(Endpoint& endpoint, const Request& /*request*/, Answer& answer) {
    const CheckHost& host = endpoint.check_host();
    SDL_Window* window = SDL_GetWindowFromID(host.window_id(host.context));
    SDL_Renderer* renderer = window != nullptr ? SDL_GetRenderer(window) : nullptr;
    // The window's size and the canvas's place in it, in the window's pixels.
    int window_width = 0;
    int window_height = 0;
    int canvas_width = kCanvasWidth;
    int canvas_height = kCanvasHeight;
    SDL_Rect canvas{0, 0, kCanvasWidth, kCanvasHeight};
    if (renderer != nullptr && SDL_GetRenderOutputSize(renderer, &window_width, &window_height)) {
        SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
        SDL_FRect reckoned{};
        if (SDL_GetRenderLogicalPresentation(renderer, &canvas_width, &canvas_height, &mode) &&
            mode != SDL_LOGICAL_PRESENTATION_DISABLED &&
            SDL_GetRenderLogicalPresentationRect(renderer, &reckoned)) {
            canvas = drawn_frame_rect(reckoned);
        } else {
            // Drawn at the window's size, as a match is.
            canvas_width = window_width;
            canvas_height = window_height;
            canvas = {0, 0, window_width, window_height};
        }
    }
    JsonWriter& json = answer.json;
    json.key("version");
    json.integer(automation_version);
    json.key("engine");
    json.begin_object();
    json.key("version");
    json.string(OA_ENGINE_VERSION);
    json.end_object();
    json.key("automation");
    json.integer(automation_version);
    json.key("window");
    json.begin_object();
    json.key("width");
    json.integer(window_width);
    json.key("height");
    json.integer(window_height);
    json.end_object();
    json.key("canvas");
    json.begin_object();
    json.key("width");
    json.integer(canvas_width);
    json.key("height");
    json.integer(canvas_height);
    json.key("rect");
    write_rect(json, canvas);
    json.end_object();
    json.key("tick_rate");
    json.integer(kTickRate);
    json.key("fixed_clock");
    json.boolean(endpoint.options().fixed_clock);
}

void answer_screen(Endpoint& endpoint, const Request& /*request*/, Answer& answer) {
    const CheckHost& host = endpoint.check_host();
    int32_t pointer_x = 0;
    int32_t pointer_y = 0;
    host.cursor(host.context, &pointer_x, &pointer_y);
    JsonWriter& json = answer.json;
    json.key("screen");
    json.string(screen_name(host.screen(host.context)));
    json.key("frontend_state");
    json.integer(host.frontend_state(host.context));
    std::vector<AutomationControl> controls;
    collect_controls(endpoint, controls);
    write_dialogs_and_focus(json, controls);
    json.key("pointer");
    json.begin_object();
    json.key("x");
    json.integer(pointer_x);
    json.key("y");
    json.integer(pointer_y);
    json.end_object();
    // Where the match's camera looks: the map pixel at the battlefield's
    // top-left corner.
    const AutomationHost& automation = endpoint.automation_host();
    const oa::Game* game = host.screen(host.context) == screen_id(Screen::match)
                               ? automation.match_game(automation.context)
                               : nullptr;
    json.key("camera");
    if (game != nullptr) {
        json.begin_object();
        json.key("x");
        json.integer(game->camera_x);
        json.key("y");
        json.integer(game->camera_y);
        json.end_object();
    } else {
        json.null();
    }
    // The gadget under the pointer. A listed control's rectangle wins, in
    // the order controls take the pointer; otherwise the gadget the game names.
    std::string game_hover;
    if (automation.hovered != nullptr)
        if (const char* name = automation.hovered(automation.context))
            game_hover = name;
    const std::string hovered = hovered_control(controls, pointer_x, pointer_y, game_hover);
    json.key("hovered");
    if (hovered.empty())
        json.null();
    else
        json.string(hovered);
}

void answer_prefs(Endpoint& endpoint, const Request& request, Answer& answer) {
    const Json* names = request.fields.find("names");
    if (names != nullptr && names->type() != JsonType::null) {
        bool listed = names->type() == JsonType::array;
        for (const Json& name : names->elements())
            listed = listed && name.string() != nullptr;
        if (!listed) {
            answer.refuse("bad_request", "names is a list of preference keys", "names");
            return;
        }
    }
    const AutomationHost& host = endpoint.automation_host();
    const auto* values = host.preferences(host.context);
    const auto* file = host.preferences_file(host.context);
    const auto wanted = [names](const std::string& key) {
        if (names == nullptr || names->type() == JsonType::null)
            return true;
        for (const Json& name : names->elements())
            if (*name.string() == key)
                return true;
        return false;
    };
    JsonWriter& json = answer.json;
    json.key("path");
    json.string(file != nullptr ? utf8_of(*file) : std::string());
    json.key("values");
    json.begin_object();
    if (values != nullptr) {
        for (const auto& [key, value] : *values) {
            if (!wanted(key))
                continue;
            json.key(key);
            json.string(value);
        }
    }
    json.end_object();
}

void answer_quit(Endpoint& endpoint, const Request& /*request*/, Answer& /*answer*/) {
    endpoint.request_quit();
}

} // namespace oa::app::automation
