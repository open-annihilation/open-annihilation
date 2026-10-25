// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/start.hpp"

#include "oa/update_helper/paths.hpp"

#include <SDL3/SDL.h>

#include <utility>
#include <vector>

namespace oa::update_helper {
namespace {

void set_error(std::string* reason, std::string message) {
    if (reason != nullptr)
        *reason = std::move(message);
}

struct Argv {
    std::vector<std::string> storage;
    std::vector<const char*> pointers;
};

void fill_argv(
    Argv& argv,
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments,
    bool add_updated_from,
    std::string_view from_version
) {
    argv.storage.clear();
    argv.pointers.clear();
    argv.storage.push_back(utf8_of(program));
    argv.storage.insert(argv.storage.end(), arguments.begin(), arguments.end());
    if (add_updated_from) {
        argv.storage.emplace_back("--updated-from");
        argv.storage.emplace_back(from_version);
    }
    argv.pointers.reserve(argv.storage.size() + 1);
    for (const std::string& argument : argv.storage)
        argv.pointers.push_back(argument.c_str());
    argv.pointers.push_back(nullptr);
}

SDL_Process* spawn(Argv& argv, bool background, std::string* reason) {
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        set_error(reason, SDL_GetError());
        return nullptr;
    }
    // SDL reads the arguments as a mutable array but never writes them.
    void* const argument_list = const_cast<char**>(argv.pointers.data());
    static_cast<void>(
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argument_list)
    );
    if (background) {
        static_cast<void>(
            SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true)
        );
    }
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (process == nullptr) {
        const char* const program = argv.pointers.empty() ? "" : argv.pointers[0];
        set_error(reason, std::string("cannot start ") + program + ": " + SDL_GetError());
    }
    return process;
}

} // namespace

bool start_detached(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments,
    bool add_updated_from,
    std::string_view from_version,
    std::string* error
) {
    if (program.empty()) {
        set_error(error, "the program is empty");
        return false;
    }
    Argv argv;
    fill_argv(argv, program, arguments, add_updated_from, from_version);
    SDL_Process* process = spawn(argv, true, error);
    if (process == nullptr)
        return false;
    SDL_DestroyProcess(process);
    return true;
}

bool run_and_wait(
    const std::filesystem::path& program,
    const std::vector<std::string>& arguments,
    std::string* error
) {
    if (program.empty()) {
        set_error(error, "the program is empty");
        return false;
    }
    Argv argv;
    fill_argv(argv, program, arguments, false, {});
    SDL_Process* process = spawn(argv, false, error);
    if (process == nullptr)
        return false;
    int status = 0;
    const bool ended = SDL_WaitProcess(process, true, &status);
    SDL_DestroyProcess(process);
    if (!ended || status != 0) {
        set_error(error, "a step's program did not finish");
        return false;
    }
    return true;
}

} // namespace oa::update_helper
