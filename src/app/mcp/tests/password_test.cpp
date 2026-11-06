// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A masked field's text stays out of the screen answers. ui.tree reports
// how many characters the field holds. ui.screen, ui.wait and the screen
// object a click answers with are the same screen object, which does not
// carry the text either.
#include "endpoint.hpp"
#include "oa/app/automation/view.hpp"
#include "oa/formats/json.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::automation {

// The screen answers share a translation unit with the endpoint's request
// handlers. This check never calls a handler. These stand in so it links
// without the endpoint.
void Answer::refuse(std::string_view, std::string_view, std::string_view) {
}

uint32_t Endpoint::tick() const noexcept {
    return 0;
}

Answer Endpoint::begin_answer(int64_t) const {
    return {};
}

void Endpoint::answer_held(Answer&) {
}

} // namespace oa::app::automation

namespace {

namespace json = oa::formats::json;
namespace automation = oa::app::automation;

using oa::app::AutomationControl;
using oa::app::AutomationControlKind;
using oa::app::AutomationHost;
using oa::app::CheckHost;

struct Fixture {
    std::vector<AutomationControl> controls;
};

void list_controls(void* context, std::vector<AutomationControl>* out) {
    *out = static_cast<Fixture*>(context)->controls;
}

oa::app::ScreenId shown_screen(void*) {
    // The main menu is the first built-in screen. Staying on it keeps the
    // answer off the match and the multiplayer screens.
    return 0;
}

uint8_t frontend_state(void*) {
    return 0;
}

uint32_t no_window(void*) {
    return 0;
}

void cursor_at(void*, int32_t* x, int32_t* y) {
    *x = 0;
    *y = 0;
}

/// One text field. A masked field still holds `secret` on the control, so
/// a writer that copies `text` would leak it.
AutomationControl field(const char* name, const char* secret, int32_t length, bool masked) {
    AutomationControl control;
    control.name = name;
    control.kind = AutomationControlKind::text_field;
    control.window = "Login";
    control.x = 16;
    control.y = 24;
    control.width = 80;
    control.height = 14;
    control.visible = true;
    control.enabled = true;
    control.text = secret != nullptr ? secret : "";
    control.masked = masked;
    control.text_length = length;
    return control;
}

std::optional<json::Json> parsed(std::string_view text) {
    json::JsonError failure;
    std::optional<json::Json> value = json::parse_json(text, failure);
    OA_CHECK(value.has_value());
    return value;
}

/// A leaf of that name. A group has a name and no text.
const json::Json* leaf_named(const json::Json& node, std::string_view name) {
    const json::Json* text = node.find("text");
    const json::Json* own = node.find("name");
    if (text != nullptr && own != nullptr && own->string() != nullptr && *own->string() == name)
        return &node;
    const json::Json* children = node.find("children");
    if (children == nullptr)
        return nullptr;
    for (const json::Json& child : children->elements()) {
        if (const json::Json* found = leaf_named(child, name))
            return found;
    }
    return nullptr;
}

const json::Json* tree_leaf(const json::Json& tree, std::string_view name) {
    const json::Json* roots = tree.find("roots");
    if (roots == nullptr)
        return nullptr;
    for (const json::Json& root : roots->elements()) {
        if (const json::Json* found = leaf_named(root, name))
            return found;
    }
    return nullptr;
}

/// A click's answer: the control it pressed, then the screen object.
std::string click_answer(std::string_view screen) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("clicked");
    writer.string("Password");
    writer.key("screen");
    writer.raw(screen);
    writer.end_object();
    return writer.text();
}

/// ui.wait's answer: it met, then the same screen object.
std::string wait_answer(std::string_view screen) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("ok");
    writer.boolean(true);
    writer.key("screen");
    writer.raw(screen);
    writer.end_object();
    return writer.text();
}

bool carries(std::string_view answer, std::string_view secret) {
    return !secret.empty() && answer.find(secret) != std::string_view::npos;
}

void check_absent(std::string_view answer, std::string_view secret, const char* where) {
    OA_CHECK(!carries(answer, secret));
    if (carries(answer, secret))
        std::fprintf(stderr, "a masked field's text is in %s\n", where);
}

void check_length(const json::Json& tree, const char* name, int64_t length) {
    const json::Json* leaf = tree_leaf(tree, name);
    OA_CHECK(leaf != nullptr);
    if (leaf == nullptr)
        return;
    const json::Json* text = leaf->find("text");
    OA_CHECK(text != nullptr && text->string() != nullptr && text->string()->empty());
    const json::Json* reported = leaf->find("length");
    OA_CHECK(reported != nullptr && reported->integer().value_or(-1) == length);
}

void check_masked_answers() {
    const std::string login_secret = std::string("ridge") + "-secret";
    const std::string battle_secret = std::string("blaze") + "-secret";
    const std::string frequency_secret = std::string("freq") + "-secret";

    Fixture fixture;
    fixture.controls.push_back(field("Password", login_secret.c_str(), 11, true));
    fixture.controls.push_back(field("Battle password", battle_secret.c_str(), 11, true));
    fixture.controls.push_back(field("Frequency password", frequency_secret.c_str(), 11, true));
    fixture.controls.push_back(field("Join password", "", 0, true));
    fixture.controls.push_back(field("Username", "oa_check", 0, false));

    CheckHost check{};
    check.context = &fixture;
    check.screen = shown_screen;
    check.frontend_state = frontend_state;
    check.window_id = no_window;
    check.cursor = cursor_at;

    AutomationHost host{};
    host.context = &fixture;
    host.controls = list_controls;

    const std::string tree_text = automation::tree_object(check, host);
    const std::string screen_text = automation::screen_object(check, host);
    const std::string click_text = click_answer(screen_text);
    const std::string wait_text = wait_answer(screen_text);

    const std::string secrets[] = {login_secret, battle_secret, frequency_secret};
    const std::string answers[] = {tree_text, screen_text, click_text, wait_text};
    const char* places[] = {"ui.tree", "ui.screen", "a click", "ui.wait"};
    for (const std::string& secret : secrets) {
        for (int place = 0; place < 4; ++place)
            check_absent(answers[place], secret, places[place]);
    }

    const std::optional<json::Json> tree = parsed(tree_text);
    if (!tree)
        return;
    check_length(*tree, "Password", 11);
    check_length(*tree, "Battle password", 11);
    check_length(*tree, "Frequency password", 11);
    check_length(*tree, "Join password", 0);

    const json::Json* name = tree_leaf(*tree, "Username");
    OA_CHECK(name != nullptr);
    if (name != nullptr) {
        const json::Json* text = name->find("text");
        OA_CHECK(text != nullptr && text->string() != nullptr && *text->string() == "oa_check");
        OA_CHECK(name->find("length") == nullptr);
    }
    OA_CHECK(tree_text.find("oa_check") != std::string::npos);
}

} // namespace

int main() {
    check_masked_answers();
    return oa::test::check_exit_status();
}
