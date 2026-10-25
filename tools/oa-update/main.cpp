// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-update: wait for the game to leave, put a checked package in place and
// start the game again. It makes no request of its own.

#include "oa/update_helper/paths.hpp"
#include "oa/update_helper/run.hpp"

#include <SDL3/SDL_main.h>

#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#if __has_include("oa_engine_build.hpp")
#include "oa_engine_build.hpp"
#endif
#include "oa/app/version_line.hpp"

namespace {

void print_help() {
    std::cout << "oa-update --plan FILE\n"
                 "oa-update --recover --updates DIR\n"
                 "oa-update --version\n"
                 "oa-update --help\n";
#if OA_SELF_CHECKS_BUILT
    std::cout << "oa-update --plan FILE --feed-key ID=ed25519:BASE64\n";
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (oa::app::version_argument_present(argc, argv)) {
        std::cout << oa::app::running_engine_version_line() << '\n';
        return oa::update_helper::exit_swapped;
    }
    try {
        bool help = false;
        bool recover = false;
        std::string plan;
        std::string updates;
        std::vector<oa::update_helper::FeedKey> keys;
        for (int index = 1; index < argc; ++index) {
            if (argv[index] == nullptr)
                continue;
            const std::string_view argument(argv[index]);
            auto need_value = [&](std::string& dest) {
                if (index + 1 >= argc || argv[index + 1] == nullptr) {
                    std::cerr << "oa-update: " << argument << " needs a value\n";
                    return false;
                }
                ++index;
                dest = argv[index];
                return true;
            };
            if (argument == "--help") {
                help = true;
            } else if (argument == "--plan") {
                if (!plan.empty() || !need_value(plan))
                    return oa::update_helper::exit_usage;
            } else if (argument == "--recover") {
                recover = true;
            } else if (argument == "--updates") {
                if (!updates.empty() || !need_value(updates))
                    return oa::update_helper::exit_usage;
            } else if (argument == "--feed-key") {
#if OA_SELF_CHECKS_BUILT
                std::string text;
                if (!need_value(text))
                    return oa::update_helper::exit_usage;
                oa::update_helper::FeedKey key;
                std::string error;
                if (!oa::update_helper::parse_feed_key(text, key, &error)) {
                    std::cerr << "oa-update: " << error << '\n';
                    return oa::update_helper::exit_usage;
                }
                keys.push_back(std::move(key));
#else
                std::cerr << "oa-update: --feed-key is not in this build\n";
                return oa::update_helper::exit_usage;
#endif
            } else {
                std::cerr << "oa-update: unknown argument\n";
                return oa::update_helper::exit_usage;
            }
        }
        if (help) {
            print_help();
            return oa::update_helper::exit_swapped;
        }
        if (recover && !plan.empty()) {
            std::cerr << "oa-update: pass --plan, or --recover with --updates\n";
            return oa::update_helper::exit_usage;
        }
        if (recover) {
            if (updates.empty() || !keys.empty()) {
                std::cerr << "oa-update: --recover needs --updates\n";
                return oa::update_helper::exit_usage;
            }
            return oa::update_helper::recover_updates(oa::update_helper::path_from(updates));
        }
        if (plan.empty()) {
            std::cerr << "oa-update: pass --plan, or --recover with --updates\n";
            return oa::update_helper::exit_usage;
        }
        return oa::update_helper::run_plan(oa::update_helper::path_from(plan), keys);
    } catch (const std::exception& raised) {
        std::cerr << "oa-update: " << raised.what() << '\n';
        return oa::update_helper::exit_usage;
    }
}
