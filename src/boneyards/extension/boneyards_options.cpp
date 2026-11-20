// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards' long options, their usage text and checks, and the "-c"
// and "-y" switches (boneyards_options.hpp). "-c" is taken when it names
// the online module, and left when it names a launch file.
#include "boneyards_options.hpp"

#include "oa/app/app.hpp"
#include "oa/app/netgame/extension_api.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oa::app {
namespace {

namespace cl = oa::app::command_line;

/// Reads a planet area id given to a long option.
///
/// Anything else throws std::runtime_error "<option> expects a planet id from
/// 1 through 2147483647".
///
/// @param option Option name, for the error text.
/// @param text Option value; the whole text must be a decimal number.
/// @return The id, 1 through 2147483647.
[[nodiscard]] int32_t parse_area_id(std::string_view option, std::string_view text) {
    int32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value <= 0)
        throw std::runtime_error(
            std::string(option) + " expects a planet id from 1 through 2147483647"
        );
    return value;
}

/// Reads the Main Frame console that --boneyards-open names.
///
/// Any other name throws std::runtime_error.
///
/// @param text Console name, one of kBoneyardsConsoleNames.
/// @return The console.
[[nodiscard]] BoneyardsConsole parse_console(std::string_view text) {
    for (std::size_t i = 0; i < std::size(kBoneyardsConsoleNames); ++i)
        if (text == kBoneyardsConsoleNames[i])
            return static_cast<BoneyardsConsole>(i);
    throw std::runtime_error(
        "--boneyards-open expects recon, chat, battle, news, ladders, profile or help"
    );
}

/// Returns the BoneyardsOptionContext behind an extension context pointer.
///
/// @param context The extension table's context.
/// @return The context it points to.
BoneyardsOptionContext& context_of(void* context) {
    return *static_cast<BoneyardsOptionContext*>(context);
}

/// Takes one of the Boneyards' long options (Extension::take_option).
///
/// --check-boneyards-login, --enter-boneyards and --check-boneyards-unit-limit
/// imply a headless check that skips the intro; --check-boneyards-launch,
/// --check-boneyards-features, --check-boneyards-overlay and
/// --check-boneyards-entry skip the intro of an unattended run.
/// --check-strip-buttons and --check-boneyards-menu-modes are headless checks
/// that skip the intro.
/// --check-boneyards-shortcut and --check-boneyards-mod-entry skip the intro
/// and are neither a headless check nor an unattended run. A missing or
/// malformed value throws
/// std::runtime_error.
///
/// @param context The BoneyardsOptionContext whose options receive the value.
/// @param name Option name with its dashes.
/// @param values Arguments that follow the option.
/// @param[in,out] effects Gains the option_effect bits the option implies.
/// @return False when the option is not the Boneyards'.
bool take_option(void* context, const char* name, const OptionValues& values, uint32_t& effects) {
    auto& result = context_of(context).options;
    const std::string_view argument(name);
    auto value = [&values]() -> std::string_view { return values.next(values.arguments); };
    if (argument == "--check-boneyards-login") {
        result.check_boneyards_login = true;
        effects |= option_effect::headless_check | option_effect::skip_intro;
    } else if (argument == "--boneyards-server")
        result.boneyards_server = std::filesystem::path(value());
    else if (argument == "--boneyards-replay")
        result.boneyards_replay = std::filesystem::path(value());
    else if (argument == "--enter-boneyards") {
        result.enter_boneyards_ticks = parse_count(value());
        effects |= option_effect::headless_check | option_effect::skip_intro;
    } else if (argument == "--boneyards-login")
        result.boneyards_login = true;
    else if (argument == "--boneyards-frames")
        result.boneyards_frames = std::filesystem::path(value());
    else if (argument == "--boneyards-planets")
        result.boneyards_planets = parse_count(value());
    else if (argument == "--boneyards-pick")
        result.boneyards_pick = parse_area_id(argument, value());
    else if (argument == "--boneyards-open")
        result.boneyards_open = parse_console(value());
    else if (argument == "--boneyards-chat")
        result.boneyards_chat = parse_area_id(argument, value());
    else if (argument == "--boneyards-say")
        result.boneyards_say = std::string(value());
    else if (argument == "--boneyards-create")
        result.boneyards_create = std::string(value());
    else if (argument == "--check-boneyards-unit-limit") {
        int32_t limit = 0;
        const std::string_view text = value();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), limit);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || limit < 21 ||
            limit > 500)
            throw std::runtime_error(
                "--check-boneyards-unit-limit expects a unit limit from 21 through 500"
            );
        result.check_boneyards_unit_limit = limit;
        effects |= option_effect::headless_check | option_effect::skip_intro;
    } else if (argument == "--check-boneyards-launch") {
        const std::string role(value());
        if (role != "host" && role != "join")
            throw std::runtime_error("--check-boneyards-launch expects host or join");
        result.boneyards_launch = role == "host" ? LaunchCheckRole::host : LaunchCheckRole::join;
        effects |= option_effect::skip_intro | option_effect::unattended;
    } else if (argument == "--boneyards-launch-for-fun") {
        result.launch_for_fun = true;
    } else if (argument == "--check-boneyards-features") {
        const std::string role(value());
        if (role != "beta" && role != "player")
            throw std::runtime_error("--check-boneyards-features expects beta or player");
        result.boneyards_features =
            role == "beta" ? FeaturesCheckRole::beta : FeaturesCheckRole::player;
        effects |= option_effect::skip_intro | option_effect::unattended;
    } else if (argument == "--check-boneyards-overlay") {
        const std::string tier(value());
        if (tier != "basic" && tier != "full")
            throw std::runtime_error("--check-boneyards-overlay expects basic or full");
        result.boneyards_overlay =
            tier == "basic" ? OverlayCheckTier::basic : OverlayCheckTier::full;
        effects |= option_effect::skip_intro | option_effect::unattended;
    } else if (argument == "--check-boneyards-entry") {
        const std::string mode(value());
        if (mode != "install" && mode != "update")
            throw std::runtime_error("--check-boneyards-entry expects install or update");
        result.boneyards_entry =
            mode == "install" ? BoneyardsEntryCheck::install : BoneyardsEntryCheck::update;
        effects |= option_effect::skip_intro | option_effect::unattended;
    } else if (argument == "--check-strip-buttons") {
        result.check_strip_buttons = true;
        effects |= option_effect::headless_check | option_effect::skip_intro;
    } else if (argument == "--check-boneyards-menu-modes") {
        const std::string mode(value());
        if (mode != "strip" && mode != "button")
            throw std::runtime_error("--check-boneyards-menu-modes expects strip or button");
        result.check_menu_modes = mode == "strip" ? MenuModesCheck::strip : MenuModesCheck::button;
        effects |= option_effect::headless_check | option_effect::skip_intro;
    } else if (argument == "--check-boneyards-shortcut") {
        result.check_boneyards_shortcut = true;
        effects |= option_effect::skip_intro;
    } else if (argument == "--check-boneyards-mod-entry") {
        result.check_boneyards_mod_entry = true;
        effects |= option_effect::skip_intro;
    } else
        return false;
    return true;
}

/// Checks that the Boneyards options taken appear only with the options they need (Extension::check_options).
///
/// --boneyards-pick, --boneyards-open and --boneyards-chat need --enter-boneyards
/// and --boneyards-login; --boneyards-chat excludes --boneyards-pick and
/// --boneyards-open; --boneyards-say needs --boneyards-open chat or
/// --boneyards-chat; --boneyards-create needs --boneyards-chat;
/// --check-boneyards-launch and --check-boneyards-features each need
/// --boneyards-replay and exclude --enter-boneyards, and they exclude each
/// other. A refused combination throws std::runtime_error naming
/// it.
///
/// @param context The BoneyardsOptionContext holding the options.
void check_options(void* context) {
    const auto& result = context_of(context).options;
    const bool logs_in = result.enter_boneyards_ticks && result.boneyards_login;
    if (result.boneyards_pick && !logs_in)
        throw std::runtime_error("--boneyards-pick needs --enter-boneyards and --boneyards-login");
    if (result.boneyards_open && !logs_in)
        throw std::runtime_error("--boneyards-open needs --enter-boneyards and --boneyards-login");
    if (result.boneyards_chat && !logs_in)
        throw std::runtime_error("--boneyards-chat needs --enter-boneyards and --boneyards-login");
    if (result.boneyards_chat && (result.boneyards_pick || result.boneyards_open))
        throw std::runtime_error(
            "--boneyards-chat picks the planet and opens CHAT itself: drop --boneyards-pick and "
            "--boneyards-open"
        );
    if (!result.boneyards_say.empty() && result.boneyards_open != BoneyardsConsole::chat &&
        !result.boneyards_chat)
        throw std::runtime_error("--boneyards-say needs --boneyards-open chat or --boneyards-chat");
    if (!result.boneyards_create.empty() && !result.boneyards_chat)
        throw std::runtime_error("--boneyards-create needs --boneyards-chat");
    if (result.boneyards_launch && result.boneyards_replay.empty())
        throw std::runtime_error("--check-boneyards-launch needs --boneyards-replay");
    if (result.boneyards_launch && result.enter_boneyards_ticks)
        throw std::runtime_error(
            "--check-boneyards-launch plays in the running game: drop --enter-boneyards"
        );
    if (result.boneyards_features && result.boneyards_replay.empty())
        throw std::runtime_error("--check-boneyards-features needs --boneyards-replay");
    if (result.boneyards_features && result.enter_boneyards_ticks)
        throw std::runtime_error(
            "--check-boneyards-features plays in the running game: drop --enter-boneyards"
        );
    if (result.boneyards_features && (result.boneyards_launch || result.check_boneyards_login ||
                                      result.check_boneyards_mod_entry))
        throw std::runtime_error(
            "--check-boneyards-features plays on its own: drop the other checks"
        );
    if (result.check_boneyards_unit_limit &&
        (result.check_boneyards_login || result.enter_boneyards_ticks || result.boneyards_launch ||
         result.boneyards_features || result.boneyards_overlay ||
         result.boneyards_entry != BoneyardsEntryCheck::none || result.check_boneyards_shortcut ||
         result.check_boneyards_mod_entry))
        throw std::runtime_error(
            "--check-boneyards-unit-limit applies the Profile on its own: drop the other checks"
        );
    if (result.boneyards_overlay &&
        (result.check_boneyards_login || result.enter_boneyards_ticks || result.boneyards_launch ||
         result.boneyards_features || result.check_boneyards_unit_limit ||
         result.boneyards_entry != BoneyardsEntryCheck::none || result.check_boneyards_shortcut ||
         result.check_boneyards_mod_entry))
        throw std::runtime_error(
            "--check-boneyards-overlay shows the main menu on its own: drop the other checks"
        );
    if (result.boneyards_entry != BoneyardsEntryCheck::none &&
        (result.check_boneyards_login || result.enter_boneyards_ticks || result.boneyards_launch ||
         result.boneyards_features || result.check_boneyards_unit_limit ||
         result.boneyards_overlay || result.check_boneyards_shortcut ||
         result.check_boneyards_mod_entry))
        throw std::runtime_error("--check-boneyards-entry runs on its own: drop the other checks");
    if (result.check_strip_buttons &&
        (result.check_boneyards_login || result.enter_boneyards_ticks || result.boneyards_launch ||
         result.boneyards_features || result.check_boneyards_unit_limit ||
         result.boneyards_overlay || result.boneyards_entry != BoneyardsEntryCheck::none ||
         result.check_boneyards_shortcut || result.check_boneyards_mod_entry))
        throw std::runtime_error("--check-strip-buttons runs on its own: drop the other checks");
    if (result.check_menu_modes &&
        (result.check_boneyards_login || result.enter_boneyards_ticks || result.boneyards_launch ||
         result.boneyards_features || result.check_boneyards_unit_limit ||
         result.boneyards_overlay || result.boneyards_entry != BoneyardsEntryCheck::none ||
         result.check_boneyards_shortcut || result.check_strip_buttons))
        throw std::runtime_error(
            "--check-boneyards-menu-modes runs on its own: drop the other checks"
        );
    if (result.check_boneyards_shortcut &&
        (result.check_boneyards_login || result.enter_boneyards_ticks || result.boneyards_launch ||
         result.boneyards_features || result.check_boneyards_unit_limit ||
         result.boneyards_overlay || result.boneyards_entry != BoneyardsEntryCheck::none ||
         result.check_strip_buttons || result.check_boneyards_mod_entry))
        throw std::runtime_error(
            "--check-boneyards-shortcut runs on its own: drop the other checks"
        );
    if (result.check_boneyards_mod_entry &&
        (result.check_boneyards_login || result.enter_boneyards_ticks || result.boneyards_launch ||
         result.boneyards_features || result.check_boneyards_unit_limit ||
         result.boneyards_overlay || result.boneyards_entry != BoneyardsEntryCheck::none ||
         result.check_strip_buttons || result.check_boneyards_shortcut))
        throw std::runtime_error(
            "--check-boneyards-mod-entry runs on its own: drop the other checks"
        );
}

/// Tells whether `name` is 3.1c's online module rather than a launch file.
///
/// The online module is a name such as conline.cfg. A launch file is a path
/// (a slash, a backslash, a colon, a space or a tab), a name ending in
/// .oalaunch or .ini in either case, "-" for a launch read from standard
/// input, or a name that begins with "-". An empty name is not a module.
///
/// @param name The word "-c" carries, attached or as the next token.
/// @return Whether the Boneyards records it as the online module.
[[nodiscard]] bool names_online_module(std::string_view name) noexcept {
    if (name.empty() || name.front() == '-')
        return false;
    for (const char character : name) {
        if (character == '/' || character == '\\' || character == ':' || character == ' ' ||
            character == '\t')
            return false;
    }
    const auto ends_with = [name](std::string_view suffix) {
        if (name.size() < suffix.size())
            return false;
        for (std::size_t index = 0; index < suffix.size(); ++index) {
            char character = name[name.size() - suffix.size() + index];
            if (character >= 'A' && character <= 'Z')
                character = static_cast<char>(character - 'A' + 'a');
            if (character != suffix[index])
                return false;
        }
        return true;
    };
    return !ends_with(".oalaunch") && !ends_with(".ini");
}

/// Records `module` as the online module and clears the launch block.
///
/// An empty name records nothing and clears nothing. The name is cut to 63
/// characters.
///
/// @param[in,out] options the options that record the module
/// @param module the online module's name; may be null
void take_online_module(BoneyardsOptions& options, const char* module) noexcept {
    if (module == nullptr || *module == '\0')
        return;
    oa::app::netgame::extension_api::launch_block() = {};
    options.online_module.assign(module, std::min(std::strlen(module), cl::kTextBytes - 1));
}

/// Takes "-c" when it names the online module, and "-y" (SwitchHandler::take).
///
/// The intro is skipped for both. "-c" with no word skips the intro and
/// clears nothing. "-c <module>" clears the launch block and records the
/// module (take_online_module) when the word is the online module
/// (names_online_module); a launch file is declined and its word is not
/// taken, so the handler that follows reads it. The attached text is that
/// word; otherwise the next token is looked at and taken only when it is
/// the module. With no way to look ahead, the next token is taken, which
/// is the older reading. "-y" asks network play's main menu for its setup
/// step, which is the Boneyards' entry (register_boneyards_console_screens).
///
/// @param context The BoneyardsOptionContext whose options note the switch.
/// @param letter lower-case switch letter
/// @param arguments the text attached to the letter and the tokens after it; "-y" takes none
/// @param[out] effects gains switch_effect_skip_intro when the switch is taken
/// @return 1 for an online-module "-c" and for "-y", else 0
int take_switch(
    void* context, char letter, const cl::SwitchArguments* arguments, uint32_t* effects
) noexcept {
    if (letter == 'c') {
        auto& recorded = context_of(context).options;
        const bool attached = arguments->attached != nullptr && *arguments->attached != '\0';
        if (!attached && arguments->peek_token == nullptr) {
            take_online_module(recorded, arguments->next_token(arguments->tokens));
            *effects |= cl::switch_effect_skip_intro;
            return 1;
        }
        const char* word =
            attached ? arguments->attached : arguments->peek_token(arguments->tokens);
        if (word != nullptr && *word != '\0' && !names_online_module(word))
            return 0;
        if (!attached && word != nullptr && *word != '\0')
            word = arguments->next_token(arguments->tokens);
        take_online_module(recorded, word);
        *effects |= cl::switch_effect_skip_intro;
        return 1;
    }
    if (letter != 'y')
        return 0;
    *effects |= cl::switch_effect_skip_intro;
    context_of(context).options.setup_requested = true;
    oa::app::netgame::extension_api::request_setup();
    return 1;
}

/// Returns the handler of the online-module "-c" and of "-y" (Extension::switch_handler).
///
/// @param context The BoneyardsOptionContext holding the handler.
/// @return The handler.
const cl::SwitchHandler* switch_handler(void* context) {
    return &context_of(context).switches;
}

/// Returns the Boneyards' part of the usage text (Extension::text).
///
/// @param context Extension context (unused).
/// @param which Text asked for.
/// @return The check options' usage; null for every other text.
const char* text(void* /*context*/, ExtensionText which) {
    if (which != ExtensionText::usage_checks)
        return nullptr;
    return "[--check-boneyards-login] [--boneyards-server SERVER_LIST.byd|HOST] "
           "[--boneyards-replay FILE.replay] "
           "[--enter-boneyards TICKS [--boneyards-login] [--boneyards-frames DIR] "
           "[--boneyards-planets N] [--boneyards-pick AREA] [--boneyards-open CONSOLE] "
           "[--boneyards-chat AREA] [--boneyards-say TEXT] "
           "[--boneyards-create NAME]] [--check-boneyards-launch host|join] "
           "[--boneyards-launch-for-fun] "
           "[--check-boneyards-features beta|player] "
           "[--check-boneyards-unit-limit N] "
           "[--check-boneyards-overlay basic|full] "
           "[--check-boneyards-entry install|update] "
           "[--check-boneyards-shortcut] "
           "[--check-boneyards-mod-entry] "
           "[--check-strip-buttons] "
           "[--check-boneyards-menu-modes strip|button] ";
}

} // namespace

BoneyardsOptionContext& boneyards_option_context() {
    static BoneyardsOptionContext context{};
    return context;
}

void fill_boneyards_option_hooks(Extension& table, BoneyardsOptionContext& context) {
    context.switches = cl::SwitchHandler{&context, take_switch, nullptr};
    table.context = &context;
    table.take_option = take_option;
    table.check_options = check_options;
    table.switch_handler = switch_handler;
    table.text = text;
}

} // namespace oa::app
