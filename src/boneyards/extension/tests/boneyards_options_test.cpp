// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards' long options through oa-game's command line: its checks'
// options and their cross-checks, with the options the engine keeps, none of
// them without the Boneyards' extension; "-c", which skips the intro and
// with an online module named clears the launch block, and which leaves a
// launch file for the handler that follows; and "-y", which skips the intro and
// asks network play's main menu for its setup step
// (oa::app::netgame::extension_api::request_setup), which network play's launch,
// bound to a record, keeps.
#include "oa/app/app.hpp"
#include "boneyards_options.hpp"

#include "oa/app/command_line.hpp"
#include "oa/app/netgame/extension_api.hpp"
#include "oa/test/netgame_launch_record.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

struct Parsed {
    oa::app::BoneyardsOptions boneyards;
    oa::app::Options app;
};

// Each parse starts from a fresh Boneyards option context, as a start does.
Parsed parse(const std::vector<const char*>& arguments, bool extended = true) {
    std::vector<std::string> storage{"open-annihilation"};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : storage)
        argv.push_back(argument.data());
    oa::app::BoneyardsOptionContext context{};
    oa::app::Extension table{};
    if (extended)
        oa::app::fill_boneyards_option_hooks(table, context);
    auto app = oa::app::parse_options(static_cast<int>(argv.size()), argv.data(), table);
    return {context.options, std::move(app)};
}

// The message parse_options rejects the arguments with, or "" when it takes them.
std::string rejection(const std::vector<const char*>& arguments, bool extended = true) {
    try {
        (void)parse(arguments, extended);
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

/// Checks "-y": the Boneyards takes it, the intro is skipped and network
/// play's main menu is asked for its setup step (LaunchSwitches::netsetup);
/// without the Boneyards' extension it is refused.
void check_setup_switch() {
    oa::test::netgame::LaunchRecord record{};
    oa::test::netgame::bind_launch_record(&record);
    const auto setup = parse({"-y"});
    expect(setup.app.launch.skip_intro == 1, "-y skips the intro");
    expect(setup.boneyards.setup_requested, "-y is noted in the Boneyards' options");
    expect(record.switches.netsetup == 1, "-y asks network play's main menu for its setup step");
    record = {};
    const auto upper = parse({"-Y"});
    expect(upper.app.launch.skip_intro == 1 && record.switches.netsetup == 1, "-Y is -y");
    record = {};
    const auto plain = parse({"--headless-check"});
    expect(plain.app.launch.skip_intro == 0, "no skip without -y");
    expect(
        !plain.boneyards.setup_requested && record.switches.netsetup == 0, "no setup without -y"
    );
    expect(rejection({"-y"}, false) == "-y is not handled by this build", "-y is the Boneyards'");
    expect(record.switches.netsetup == 0, "a refused -y asks for nothing");
    oa::test::netgame::bind_launch_record(nullptr);
}

/// Checks "-c": the Boneyards takes it and the intro is skipped; a module
/// named, attached or as the next token, clears the launch block and is
/// recorded, cut to 63 characters; without a module nothing is cleared;
/// without the Boneyards' extension it is refused.
void check_online_module_switch() {
    auto& block = oa::app::netgame::extension_api::launch_block();
    block = {};
    block.connection_type = 1;
    block.hosting = 1;
    std::snprintf(block.session_name, sizeof block.session_name, "%s", "Host");
    std::snprintf(block.address, sizeof block.address, "%s", "10.0.0.5");
    const auto module = parse({"-c", "conline.cfg"});
    expect(module.app.launch.skip_intro == 1, "-c skips the intro");
    expect(module.boneyards.online_module == "conline.cfg", "-c records its module");
    expect(
        block.connection_type == 0 && block.hosting == 0 && block.session_name[0] == '\0' &&
            block.address[0] == '\0',
        "-c with a module clears the launch block"
    );
    block.connection_type = 3;
    const auto attached = parse({"-cconline.cfg"});
    expect(
        attached.boneyards.online_module == "conline.cfg" && block.connection_type == 0,
        "an attached module clears the block too"
    );
    block.connection_type = 3;
    const auto bare = parse({"-c"});
    expect(
        bare.app.launch.skip_intro == 1 && bare.boneyards.online_module.empty() &&
            block.connection_type == 3,
        "-c alone skips the intro and clears nothing"
    );
    const std::string long_module(80, 'm');
    const auto cut = parse({"-c", long_module.c_str()});
    expect(
        cut.boneyards.online_module == std::string(63, 'm'), "the module is cut to 63 characters"
    );
    expect(
        rejection({"-c", "conline.cfg"}, false) == "-c is not handled by this build",
        "-c is the Boneyards'"
    );
    block = {};
}

/// A handler asked after the Boneyards, standing in for network play: it
/// records the "-c" word the Boneyards left it.
struct LaunchFollow {
    const oa::app::command_line::SwitchHandler* boneyards = nullptr;
    oa::app::command_line::SwitchHandler chain{};
    std::string path;
    bool took = false;
};

int follow_launch(
    void* raw,
    char letter,
    const oa::app::command_line::SwitchArguments* arguments,
    uint32_t* effects
) {
    auto& follow = *static_cast<LaunchFollow*>(raw);
    const auto* handler = follow.boneyards;
    if (handler->take != nullptr &&
        handler->take(handler->context, letter, arguments, effects) != 0)
        return 1;
    if (letter != 'c')
        return 0;
    follow.took = true;
    const char* path = arguments->attached != nullptr && *arguments->attached != '\0'
                           ? arguments->attached
                           : arguments->next_token(arguments->tokens);
    follow.path = path != nullptr ? path : "";
    *effects |= oa::app::command_line::switch_effect_skip_intro;
    return 1;
}

/// Checks that a launch file's "-c" is left whole for the next handler, and
/// that the online-module form is still recorded.
void check_launch_file_left() {
    oa::app::BoneyardsOptionContext context{};
    oa::app::Extension table{};
    oa::app::fill_boneyards_option_hooks(table, context);
    LaunchFollow follow;
    follow.boneyards = &context.switches;
    follow.chain = {&follow, follow_launch, nullptr};
    auto& block = oa::app::netgame::extension_api::launch_block();

    const auto run = [&](const std::vector<const char*>& words) {
        follow.path.clear();
        follow.took = false;
        context.options = {};
        block = {};
        block.connection_type = 3;
        oa::app::command_line::Switches switches{};
        const auto status = oa::app::command_line::parse(words, switches, &follow.chain);
        expect(status == oa::app::command_line::Status::run, "the chained switches run");
        return switches;
    };

    const auto spaced = run({"-c", "My Battle.oalaunch", "german"});
    expect(follow.took && follow.path == "My Battle.oalaunch", "a path with spaces is left whole");
    expect(context.options.online_module.empty(), "a spaced path names no online module");
    expect(block.connection_type == 3, "a spaced path clears no launch block");
    expect(
        std::strcmp(spaced.language, "german") == 0 && spaced.skip_intro == 1,
        "the word after a spaced path stays the language, and the intro is skipped"
    );

    const auto folder = run({"-c", "/Users/me/Launch Files/host.oalaunch"});
    expect(
        follow.took && follow.path == "/Users/me/Launch Files/host.oalaunch" &&
            folder.skip_intro == 1,
        "a folder whose name has a space is left whole"
    );
    expect(block.connection_type == 3, "a path clears no launch block");

    run({"-chost.oalaunch"});
    expect(follow.took && follow.path == "host.oalaunch", "an attached .oalaunch file is left");
    run({"-c", "HOST.OALAUNCH"});
    expect(follow.path == "HOST.OALAUNCH", "the suffix matches in either case");
    run({"-c", "-"});
    expect(follow.took && follow.path == "-", "a piped launch is left");
    run({"-c", "-t60", "german"});
    expect(
        follow.took && follow.path == "-t60" && context.options.online_module.empty(),
        "a word that begins with a dash is the launch file, not the online module"
    );
    run({"-c", "taforever.ini"});
    expect(follow.took && follow.path == "taforever.ini", "a settings file is left");
    run({"-c", "C:\\Program Files\\TA Forever\\totala.ini"});
    expect(
        follow.path == "C:\\Program Files\\TA Forever\\totala.ini",
        "a settings path with spaces is left"
    );

    const auto module = run({"-c", "conline.cfg", "german"});
    expect(
        !follow.took && context.options.online_module == "conline.cfg", "the online module is taken"
    );
    expect(block.connection_type == 0, "the online module clears the launch block");
    expect(
        std::strcmp(module.language, "german") == 0 && module.skip_intro == 1,
        "the word after the online module is the language"
    );
    run({"-cconline.cfg"});
    expect(
        !follow.took && context.options.online_module == "conline.cfg",
        "an attached online module is taken"
    );
    block = {};
}

} // namespace

int main() {
    check_setup_switch();
    check_online_module_switch();
    check_launch_file_left();
    const auto plain = parse({"--headless-check"});

    const auto replay = parse({"--boneyards-replay", "alpha.replay", "--check-boneyards-login"});
    expect(
        replay.boneyards.boneyards_replay == "alpha.replay", "--boneyards-replay takes its file"
    );
    expect(
        replay.boneyards.check_boneyards_login && replay.app.headless_check,
        "the login check runs headless"
    );
    expect(plain.boneyards.boneyards_replay.empty(), "no replay without the flag");
    expect(
        rejection({"--boneyards-replay"}) == "--boneyards-replay requires a value",
        "the replay needs a file"
    );

    const auto post_login = parse(
        {"--enter-boneyards",
         "400",
         "--boneyards-login",
         "--boneyards-frames",
         "frames",
         "--boneyards-planets",
         "25"}
    );
    expect(
        post_login.boneyards.enter_boneyards_ticks == 400u &&
            post_login.boneyards.boneyards_login && post_login.app.headless_check,
        "--boneyards-login logs in during the entry check"
    );
    expect(
        post_login.boneyards.boneyards_frames == "frames" &&
            post_login.boneyards.boneyards_planets == 25u,
        "the entry check takes a frame directory and a planet count"
    );
    expect(
        !plain.boneyards.boneyards_login && plain.boneyards.boneyards_frames.empty() &&
            !plain.boneyards.boneyards_planets,
        "no login, frames or planet count without the flags"
    );
    const auto screens = parse(
        {"--enter-boneyards",
         "400",
         "--boneyards-login",
         "--boneyards-pick",
         "1801",
         "--boneyards-open",
         "help"}
    );
    expect(
        screens.boneyards.boneyards_pick == 1801 &&
            screens.boneyards.boneyards_open == oa::app::BoneyardsConsole::help,
        "the entry check takes a planet and a console"
    );
    expect(
        parse({"--enter-boneyards", "1", "--boneyards-login", "--boneyards-open", "recon"})
                .boneyards.boneyards_open == oa::app::BoneyardsConsole::recon,
        "the first console name"
    );
    expect(
        !plain.boneyards.boneyards_pick && !plain.boneyards.boneyards_open,
        "no planet or console without the flags"
    );
    expect(
        rejection({"--enter-boneyards", "400", "--boneyards-pick", "1801"}) ==
            "--boneyards-pick needs --enter-boneyards and --boneyards-login",
        "a planet needs the login"
    );
    expect(
        rejection({"--boneyards-login", "--boneyards-open", "chat"}) ==
            "--boneyards-open needs --enter-boneyards and --boneyards-login",
        "a console needs the entry check"
    );
    expect(
        rejection({"--boneyards-pick", "0"}).find("--boneyards-pick") == 0, "planet ids start at 1"
    );
    expect(
        rejection({"--boneyards-pick", "18x"}).find("--boneyards-pick") == 0,
        "trailing text after a planet id is refused"
    );
    expect(
        rejection({"--boneyards-open", "locator"}).find("--boneyards-open") == 0,
        "Locator opens no console"
    );
    expect(
        parse({"--enter-boneyards",
               "400",
               "--boneyards-login",
               "--boneyards-pick",
               "1801",
               "--boneyards-open",
               "chat",
               "--boneyards-say",
               "+locate Ghost"})
                .boneyards.boneyards_say == "+locate Ghost",
        "the chat console takes a line to send"
    );
    expect(plain.boneyards.boneyards_say.empty(), "no line without the flag");
    const auto chat = parse(
        {"--enter-boneyards",
         "400",
         "--boneyards-login",
         "--boneyards-chat",
         "1801",
         "--boneyards-say",
         "hello from oa"}
    );
    expect(
        chat.boneyards.boneyards_chat == 1801 && chat.boneyards.boneyards_say == "hello from oa" &&
            !chat.boneyards.boneyards_pick && !chat.boneyards.boneyards_open,
        "the chat check takes a planet and a line"
    );
    expect(!plain.boneyards.boneyards_chat, "no chat check without the flag");
    const auto create = parse(
        {"--enter-boneyards",
         "400",
         "--boneyards-login",
         "--boneyards-chat",
         "1801",
         "--boneyards-create",
         "Forward Base"}
    );
    expect(
        create.boneyards.boneyards_create == "Forward Base",
        "the chat check takes a frequency to create"
    );
    expect(plain.boneyards.boneyards_create.empty(), "nothing to create without the flag");
    expect(
        rejection(
            {"--enter-boneyards",
             "400",
             "--boneyards-login",
             "--boneyards-pick",
             "1801",
             "--boneyards-open",
             "chat",
             "--boneyards-create",
             "Den"}
        ) == "--boneyards-create needs --boneyards-chat",
        "only the chat check creates a frequency"
    );
    expect(
        rejection({"--enter-boneyards", "400", "--boneyards-chat", "1801"}) ==
            "--boneyards-chat needs --enter-boneyards and --boneyards-login",
        "the chat check needs the login"
    );
    expect(
        rejection({"--enter-boneyards",
                   "400",
                   "--boneyards-login",
                   "--boneyards-chat",
                   "1801",
                   "--boneyards-open",
                   "chat"})
                .find("--boneyards-chat picks the planet") == 0,
        "the chat check opens its own console"
    );
    expect(
        rejection({"--boneyards-chat", "-3"}) ==
            "--boneyards-chat expects a planet id from 1 through 2147483647",
        "the chat check's planet id is checked"
    );
    expect(
        rejection(
            {"--enter-boneyards",
             "400",
             "--boneyards-login",
             "--boneyards-open",
             "help",
             "--boneyards-say",
             "hello"}
        ) == "--boneyards-say needs --boneyards-open chat or --boneyards-chat",
        "only the chat console takes a line"
    );

    // A launched battle played in the running game: unattended, without the
    // intro, under a replay and outside the entry check.
    const auto launch =
        parse({"--boneyards-replay", "host.replay", "--check-boneyards-launch", "host"});
    expect(
        launch.boneyards.boneyards_launch == oa::app::LaunchCheckRole::host &&
            launch.app.unattended && launch.app.skip_intro && !launch.app.headless_check,
        "the launch check plays the host in an unattended run"
    );
    expect(
        parse({"--boneyards-replay", "join.replay", "--check-boneyards-launch", "join"})
                .boneyards.boneyards_launch == oa::app::LaunchCheckRole::join,
        "the launch check plays the joiner"
    );
    expect(!plain.boneyards.boneyards_launch, "no launch check without the flag");
    expect(!launch.boneyards.launch_for_fun, "the launch check picks Galactic War");
    expect(
        parse({
                  "--boneyards-replay",
                  "host.replay",
                  "--check-boneyards-launch",
                  "host",
                  "--boneyards-launch-for-fun",
              })
            .boneyards.launch_for_fun,
        "the launch check can pick For Fun"
    );
    const auto features =
        parse({"--boneyards-replay", "beta.replay", "--check-boneyards-features", "beta"});
    expect(
        features.boneyards.boneyards_features == oa::app::FeaturesCheckRole::beta &&
            features.app.unattended && features.app.skip_intro && !features.app.headless_check,
        "the features check plays a Beta Tester in an unattended run"
    );
    expect(
        parse({"--boneyards-replay", "player.replay", "--check-boneyards-features", "player"})
                .boneyards.boneyards_features == oa::app::FeaturesCheckRole::player,
        "the features check plays an ordinary account"
    );
    expect(!plain.boneyards.boneyards_features, "no features check without the flag");
    expect(
        rejection({"--boneyards-replay", "a.replay", "--check-boneyards-features", "watch"}) ==
            "--check-boneyards-features expects beta or player",
        "the features check plays one of two accounts"
    );
    expect(
        rejection({"--check-boneyards-features", "beta"}) ==
            "--check-boneyards-features needs --boneyards-replay",
        "the features check needs a replay"
    );
    expect(
        rejection({"--boneyards-replay",
                   "a.replay",
                   "--check-boneyards-features",
                   "player",
                   "--enter-boneyards",
                   "10"})
                .find("--check-boneyards-features plays in the running game") == 0,
        "the features check is no entry check"
    );
    expect(
        rejection({"--boneyards-replay",
                   "a.replay",
                   "--check-boneyards-features",
                   "beta",
                   "--check-boneyards-launch",
                   "host"})
                .find("--check-boneyards-features plays on its own") == 0,
        "the features check plays on its own"
    );
    expect(
        rejection({"--boneyards-replay",
                   "a.replay",
                   "--check-boneyards-features",
                   "beta",
                   "--check-boneyards-login"})
                .find("--check-boneyards-features plays on its own") == 0,
        "the features check is not the login check"
    );
    expect(
        rejection({"--boneyards-replay",
                   "a.replay",
                   "--check-boneyards-unit-limit",
                   "333",
                   "--check-boneyards-features",
                   "player"})
                .find("--check-boneyards-unit-limit applies the Profile on its own") == 0,
        "the unit limit check excludes the features check"
    );
    expect(
        rejection({"--boneyards-replay", "a.replay", "--check-boneyards-launch", "watch"}) ==
            "--check-boneyards-launch expects host or join",
        "the launch check plays one of two sides"
    );
    expect(
        rejection({"--check-boneyards-launch", "host"}) ==
            "--check-boneyards-launch needs --boneyards-replay",
        "the launch check needs a replay"
    );
    expect(
        rejection({"--boneyards-replay",
                   "a.replay",
                   "--check-boneyards-launch",
                   "join",
                   "--enter-boneyards",
                   "10"})
                .find("--check-boneyards-launch plays in the running game") == 0,
        "the launch check is no entry check"
    );
    expect(
        parse({"--check-boneyards-unit-limit", "333"}).boneyards.check_boneyards_unit_limit == 333,
        "the unit limit check takes the limit APPLY stores"
    );
    expect(
        rejection({"--check-boneyards-unit-limit", "20"}) ==
            "--check-boneyards-unit-limit expects a unit limit from 21 through 500",
        "the unit limit check stays inside the Profile's range"
    );
    expect(
        rejection({"--check-boneyards-unit-limit", "333", "--enter-boneyards", "10"})
                .find("--check-boneyards-unit-limit applies the Profile on its own") == 0,
        "the unit limit check runs on its own"
    );
    const auto overlay = parse({"--check-boneyards-overlay", "basic"});
    expect(
        overlay.boneyards.boneyards_overlay == oa::app::OverlayCheckTier::basic &&
            overlay.app.unattended && overlay.app.skip_intro && !overlay.app.headless_check,
        "the overlay check shows the main menu unattended"
    );
    expect(
        parse({"--check-boneyards-overlay", "full"}).boneyards.boneyards_overlay ==
            oa::app::OverlayCheckTier::full,
        "the overlay check also runs at the full tier"
    );
    expect(!plain.boneyards.boneyards_overlay, "no overlay check without the flag");
    expect(
        rejection({"--check-boneyards-overlay", "watch"}) ==
            "--check-boneyards-overlay expects basic or full",
        "the overlay check names basic or full"
    );
    expect(
        rejection({"--check-boneyards-overlay", "basic", "--enter-boneyards", "10"})
                .find("--check-boneyards-overlay shows the main menu on its own") == 0,
        "the overlay check runs on its own"
    );
    const auto entry = parse({"--check-boneyards-entry", "install"});
    expect(
        entry.boneyards.boneyards_entry == oa::app::BoneyardsEntryCheck::install &&
            entry.app.unattended && entry.app.skip_intro && !entry.app.headless_check,
        "the entry check fetches unattended"
    );
    expect(
        parse({"--check-boneyards-entry", "update"}).boneyards.boneyards_entry ==
            oa::app::BoneyardsEntryCheck::update,
        "the entry check also declines an update"
    );
    expect(
        plain.boneyards.boneyards_entry == oa::app::BoneyardsEntryCheck::none,
        "no entry check without the flag"
    );
    expect(
        rejection({"--check-boneyards-entry", "watch"}) ==
            "--check-boneyards-entry expects install or update",
        "the entry check names install or update"
    );
    expect(
        rejection({"--check-boneyards-entry", "install", "--enter-boneyards", "10"})
                .find("--check-boneyards-entry runs on its own") == 0,
        "the entry check runs on its own"
    );

    const auto strip = parse({"--check-strip-buttons"});
    expect(
        strip.boneyards.check_strip_buttons && strip.app.headless_check && strip.app.skip_intro,
        "the strip check runs headless and skips the intro"
    );
    expect(!plain.boneyards.check_strip_buttons, "no strip check without the flag");
    expect(
        rejection({"--check-strip-buttons", "--enter-boneyards", "10"})
                .find("--check-strip-buttons runs on its own") == 0,
        "the strip check runs on its own"
    );

    // Without the Boneyards' extension its flags do not exist.
    for (const char* flag :
         {"--enter-boneyards",
          "--check-boneyards-login",
          "--boneyards-server",
          "--boneyards-replay",
          "--boneyards-login",
          "--check-boneyards-launch",
          "--boneyards-launch-for-fun",
          "--check-boneyards-features",
          "--check-boneyards-unit-limit",
          "--check-boneyards-overlay",
          "--check-boneyards-entry",
          "--check-strip-buttons"})
        expect(rejection({flag}, false) == std::string("unknown option: ") + flag, flag);
    return failures == 0 ? 0 : 1;
}
