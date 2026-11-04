// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The side prompt. After the dossier loads, a registered member with no side
// for this sector chooses Arm or Core before the Main Frame takes input.
// The last sector's side is named and preselected. EXIT is the other way out.
// The box is laid out in gui/side_prompt.gui on the message-box picture's
// own size, centred on the console. A cover over the console shows the
// console's green, so the login console's controls do not show around it.
#include "oa/boneyards/console/feature_consoles.hpp"

#include "console_layer.hpp"
#include "console_state.hpp"
#include "oa/boneyards/console/gui_screen.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "oa/boneyards/utf8.hpp"

#include <cstdlib>
#include <cstring>

namespace oa::boneyards::console {
namespace {

constexpr const char* kPromptPath = "gui/side_prompt.gui";
constexpr const char* kNoPrevious = "Choose Arm or Core for this sector.";
constexpr const char* kPreviousArm =
    "You chose Arm in the last sector. Choose a side for this one.";
constexpr const char* kPreviousCore =
    "You chose Core in the last sector. Choose a side for this one.";
// Bit 31 of a user id is a guest, the same bit the session keeps a local name for.
constexpr uint32_t kGuestUserBit = 0x80000000U;

struct SidePrompt {
    GuiScreen* screen = nullptr;
    StatusWidget* owner = nullptr;
    bool waiting = false;
    bool leave = false;
};

SidePrompt* g_prompt = nullptr;
bool g_gave_up = false;
StatusWidget* g_gave_up_owner = nullptr;

const char* sentence_for(char previous) {
    if (previous == 'A')
        return kPreviousArm;
    if (previous == 'C')
        return kPreviousCore;
    return kNoPrevious;
}

void set_message(GuiScreen* screen, char previous) {
    const GuiControl* message = gui_screen_find(screen, "Message");
    if (message == nullptr || message->window == nullptr || message->window->ops == nullptr ||
        message->window->ops->set_caption == nullptr)
        return;
    char shown[160];
    utf8::copy(shown, sizeof shown, shown_text(sentence_for(previous)), 0);
    message->window->ops->set_caption(message->window, shown, text_style::wrap);
}

Window* control_window(GuiScreen* screen, const char* name) {
    const GuiControl* control = gui_screen_find(screen, name);
    return control != nullptr ? control->window : nullptr;
}

void preselect(GuiScreen* screen, char previous) {
    Window* arm = control_window(screen, "Arm");
    Window* core = control_window(screen, "Core");
    if (arm != nullptr && core != nullptr) {
        screens::lobby_radio_add_member(reinterpret_cast<screens::LobbyRadio*>(arm), core);
        screens::lobby_radio_add_member(reinterpret_cast<screens::LobbyRadio*>(core), arm);
        window_set_state(arm, button_state::unpressed);
        window_set_state(core, button_state::unpressed);
    }
    if (previous == 'A' && arm != nullptr)
        window_set_state(arm, button_state::pressed);
    else if (previous == 'C' && core != nullptr)
        window_set_state(core, button_state::pressed);
}

// Draws the console's own picture, the one its green layer shows, on the
// cover, so the login console's controls and logo are hidden and the
// console looks empty around the box. The picture is shared, so the cover
// follows the layer as it changes. Without a layer the cover keeps its
// plain green.
void cover_login(GuiScreen* screen, const MainFrame* frame) {
    Window* cover = control_window(screen, "Cover");
    if (cover == nullptr || frame->console == nullptr)
        return;
    const Window& layer = frame->console->base;
    const int32_t state = layer.states.current;
    if (layer.states.images == nullptr || state < 0 || state >= layer.states.count ||
        layer.states.images[state] < 0)
        return;
    window_set_state_image(cover, cover->states.current, layer.states.images[state]);
}

const char* chosen_side(GuiScreen* screen) {
    Window* arm = control_window(screen, "Arm");
    Window* core = control_window(screen, "Core");
    const bool arm_on = arm != nullptr && arm->states.current == button_state::pressed;
    const bool core_on = core != nullptr && core->states.current == button_state::pressed;
    if (core_on)
        return "CORE";
    if (arm_on)
        return "ARM";
    return nullptr;
}

bool member_needs_side(const StatusWidget* owner) {
    const screens::BoneyardsScreens* navigation = owner->navigation;
    if (navigation == nullptr || navigation->allegiance != 'U')
        return false;
    if (navigation->login.guest_value == 1)
        return false;
    if ((navigation->session.self_user_id & kGuestUserBit) != 0)
        return false;
    return true;
}

void prompt_destroyed(void* user) {
    auto* prompt = static_cast<SidePrompt*>(user);
    if (g_prompt == prompt)
        g_prompt = nullptr;
    std::free(prompt);
}

void dismiss_prompt(StatusWidget* owner) {
    if (g_prompt == nullptr || g_prompt->owner != owner)
        return;
    GuiScreen* screen = g_prompt->screen;
    g_prompt->screen = nullptr;
    g_gave_up = false;
    g_gave_up_owner = nullptr;
    gui_screen_destroy(screen);
}

void on_refreshed(void* context, bool ok, const screens::ProfileFields* fields) {
    auto* prompt = static_cast<SidePrompt*>(context);
    if (g_prompt != prompt)
        return;
    prompt->waiting = false;
    if (!ok || fields == nullptr || fields->get_string == nullptr || prompt->owner == nullptr ||
        prompt->owner->navigation == nullptr)
        return;
    char letters[16] = "";
    if (!fields->get_string(fields->user, "ALLEGIANCE", letters, sizeof letters))
        return;
    char side = '\0';
    if (letters[0] == 'A' || letters[0] == 'a')
        side = 'A';
    else if (letters[0] == 'C' || letters[0] == 'c')
        side = 'C';
    if (side == '\0')
        return;
    prompt->owner->navigation->allegiance = side;
}

void on_submitted(void* context, bool ok) {
    auto* prompt = static_cast<SidePrompt*>(context);
    if (g_prompt != prompt)
        return;
    if (!ok || prompt->owner == nullptr || prompt->owner->navigation == nullptr) {
        prompt->waiting = false;
        return;
    }
    const BoneyardsService& service = prompt->owner->navigation->service;
    if (service.request_profile == nullptr) {
        prompt->waiting = false;
        return;
    }
    const uint32_t user_id = prompt->owner->navigation->session.self_user_id;
    const ServiceResult asked =
        service.request_profile(service.user, user_id, on_refreshed, nullptr, prompt);
    if (asked != ServiceResult::ok && asked != ServiceResult::pending)
        prompt->waiting = false;
}

void prompt_fired(void* user, Window* source) {
    auto* prompt = static_cast<SidePrompt*>(user);
    if (prompt->screen == nullptr || source == nullptr)
        return;
    const char* name = gui_screen_fired_name(prompt->screen);
    if (std::strcmp(name, "EXIT") == 0) {
        prompt->leave = true;
        return;
    }
    if (std::strcmp(name, "Choose") != 0 || prompt->waiting)
        return;
    const char* side = chosen_side(prompt->screen);
    if (side == nullptr || prompt->owner == nullptr || prompt->owner->navigation == nullptr)
        return;
    const BoneyardsService& service = prompt->owner->navigation->service;
    if (service.submit_profile == nullptr)
        return;
    ProfileEdits edits{};
    edits.count = 1;
    edits.fields[0].key = "ALLEGIANCE";
    utf8::copy(edits.fields[0].value, sizeof edits.fields[0].value, side, 0);
    prompt->waiting = true;
    const uint32_t user_id = prompt->owner->navigation->session.self_user_id;
    const ServiceResult sent =
        service.submit_profile(service.user, user_id, &edits, on_submitted, prompt);
    if (sent != ServiceResult::ok && sent != ServiceResult::pending)
        prompt->waiting = false;
}

void open_prompt(StatusWidget* owner) {
    if (g_prompt != nullptr || owner->system.main_frame == nullptr)
        return;
    if (g_gave_up && g_gave_up_owner == owner)
        return;
    GuiScreen* screen = gui_screen_load(kPromptPath, nullptr);
    if (screen == nullptr) {
        g_gave_up = true;
        g_gave_up_owner = owner;
        return;
    }
    auto* prompt = static_cast<SidePrompt*>(std::calloc(1, sizeof(SidePrompt)));
    if (prompt == nullptr)
        std::abort();
    prompt->screen = screen;
    prompt->owner = owner;
    g_prompt = prompt;
    g_gave_up = false;
    g_gave_up_owner = nullptr;
    const char previous =
        owner->navigation != nullptr ? owner->navigation->allegiance_previous : '\0';
    set_message(screen, previous);
    preselect(screen, previous);
    cover_login(screen, owner->system.main_frame);
    gui_screen_set_user(screen, prompt, prompt_fired, prompt_destroyed);
    Window* root = gui_screen_root(screen);
    window_set_bounds(
        root, root->x, root->y, root->width, root->height, &owner->system.main_frame->base, true
    );
    root->focusable = 1;
    desktop_set_focus(&owner->desktop, root);
}

} // namespace

bool side_prompt_holds_login(StatusWidget* owner) {
    if (owner == nullptr || owner->navigation == nullptr)
        return false;
    if (g_prompt != nullptr && g_prompt->owner == owner && g_prompt->leave) {
        dismiss_prompt(owner);
        console_reset_to_main_menu(owner);
        return true;
    }
    if (!member_needs_side(owner)) {
        dismiss_prompt(owner);
        return false;
    }
    open_prompt(owner);
    return true;
}

} // namespace oa::boneyards::console
