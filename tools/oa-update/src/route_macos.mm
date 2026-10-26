// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The macOS bundle route. The new bundle is checked against the one that is
// running, then moved into its place. A folder the player cannot write is
// opened in Installer instead. This file is built on Apple only.

#include "oa/update_helper/routes.hpp"

#include "oa/update_helper/paths.hpp"

#import <AppKit/AppKit.h>
#include <Security/Security.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace oa::update_helper {
namespace {

namespace fs = std::filesystem;

void set_why(std::string* why, std::string message) {
    if (why != nullptr)
        *why = std::move(message);
}

std::string text_of(CFStringRef text) {
    if (text == nullptr)
        return {};
    const char* const direct = CFStringGetCStringPtr(text, kCFStringEncodingUTF8);
    if (direct != nullptr)
        return direct;
    const CFIndex length = CFStringGetLength(text);
    const CFIndex bytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string buffer(static_cast<std::size_t>(bytes), '\0');
    if (!CFStringGetCString(text, buffer.data(), bytes, kCFStringEncodingUTF8))
        return {};
    buffer.assign(buffer.c_str());
    return buffer;
}

std::string team_of(SecStaticCodeRef code) {
    CFDictionaryRef information = nullptr;
    const OSStatus copied =
        SecCodeCopySigningInformation(code, kSecCSSigningInformation, &information);
    if (copied != errSecSuccess || information == nullptr)
        return {};
    std::string team;
    const void* const value = CFDictionaryGetValue(information, kSecCodeInfoTeamIdentifier);
    if (value != nullptr && CFGetTypeID(value) == CFStringGetTypeID())
        team = text_of(static_cast<CFStringRef>(value));
    CFRelease(information);
    return team;
}

/// True when the system accepts the requirement text `notarized` and the
/// bundle meets it. A system that rejects the text is reported separately.
bool requirement_says_notarized(SecStaticCodeRef code, bool& rejected) {
    rejected = false;
    SecRequirementRef requirement = nullptr;
    const OSStatus created =
        SecRequirementCreateWithString(CFSTR("notarized"), kSecCSDefaultFlags, &requirement);
    if (created != errSecSuccess || requirement == nullptr) {
        rejected = true;
        return false;
    }
    const OSStatus checked = SecStaticCodeCheckValidity(code, kSecCSDefaultFlags, requirement);
    CFRelease(requirement);
    return checked == errSecSuccess;
}

/// Gatekeeper's own assessment, used when this macOS rejects the
/// notarization requirement text. The attribute on the bundle is not changed.
bool spctl_says_notarized(const fs::path& bundle) {
    int pipes[2] = {-1, -1};
    if (pipe(pipes) != 0)
        return false;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(pipes[0]);
        close(pipes[1]);
        return false;
    }
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);

    std::string program = "/usr/sbin/spctl";
    std::string assess = "--assess";
    std::string type_flag = "--type";
    std::string execute = "execute";
    std::string verbose = "-vv";
    std::string target = utf8_of(bundle);
    char* argv[] = {
        program.data(),
        assess.data(),
        type_flag.data(),
        execute.data(),
        verbose.data(),
        target.data(),
        nullptr
    };
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (spawned != 0) {
        close(pipes[0]);
        return false;
    }

    std::string output;
    char buffer[512];
    for (;;) {
        const ssize_t got = read(pipes[0], buffer, sizeof buffer);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (got == 0)
            break;
        if (output.size() < 65536)
            output.append(buffer, static_cast<std::size_t>(got));
    }
    close(pipes[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return output.find("source=Notarized Developer ID") != std::string::npos;
}

/// The signature of one bundle. `failure` is set when the signature cannot
/// be read at all; an invalid signature is still a reading.
bool read_signature(const fs::path& bundle, BundleSignature& signature, std::string& failure) {
    signature = {};
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(bundle.c_str()),
        static_cast<CFIndex>(std::strlen(bundle.c_str())),
        true
    );
    if (url == nullptr) {
        failure = "the new bundle's signature is not valid";
        return false;
    }
    SecStaticCodeRef code = nullptr;
    const OSStatus created = SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code);
    CFRelease(url);
    if (created != errSecSuccess || code == nullptr) {
        failure = "the new bundle's signature is not valid";
        return false;
    }
    const SecCSFlags flags =
        kSecCSCheckAllArchitectures | kSecCSStrictValidate | kSecCSCheckNestedCode;
    signature.valid = SecStaticCodeCheckValidity(code, flags, nullptr) == errSecSuccess;
    signature.team = team_of(code);
    if (signature.valid) {
        bool rejected = false;
        signature.notarized = requirement_says_notarized(code, rejected);
        if (rejected)
            signature.notarized = spctl_says_notarized(bundle);
    }
    CFRelease(code);
    return true;
}

MacosVolume volume_of(const fs::path& install, const fs::path& updates, std::string* why) {
    struct stat installed{};
    struct stat kept{};
    if (::stat(install.c_str(), &installed) != 0 || ::stat(updates.c_str(), &kept) != 0) {
        set_why(why, "the volumes could not be compared");
        return MacosVolume::exdev;
    }
    return installed.st_dev == kept.st_dev ? MacosVolume::same : MacosVolume::exdev;
}

struct StartResult {
    std::mutex mutex;
    bool finished{false};
    bool started{false};
    std::string failure;
};

} // namespace

std::vector<Operation> macos_checked_operations(
    const oa::app::update::Plan& plan, const fs::path& staged, bool test_key, std::string* why
) {
    if (why != nullptr)
        why->clear();
    if (plan.installer)
        return macos_bundle_operations(plan, staged, MacosVolume::same, why);

    fs::path application;
    if (!macos_staged_application(staged, application, why))
        return {};
    const fs::path install = path_from(plan.install);
    BundleSignature running;
    std::string failure;
    if (!read_signature(install, running, failure)) {
        set_why(why, "the installed bundle's signature is not valid");
        return {};
    }
    BundleSignature incoming;
    if (!read_signature(application, incoming, failure)) {
        set_why(why, failure.empty() ? "the new bundle's signature is not valid" : failure);
        return {};
    }
    const MacosDecision decision = macos_may_swap(running, incoming, test_key);
    if (!decision.go) {
        set_why(why, decision.why);
        return {};
    }
    std::string volume_why;
    const MacosVolume volume = volume_of(install, path_from(plan.updates), &volume_why);
    if (!volume_why.empty()) {
        set_why(why, volume_why);
        return {};
    }
    return macos_bundle_operations(plan, staged, volume, why);
}

bool start_macos_bundle(
    const fs::path& bundle,
    const std::vector<std::string>& arguments,
    std::string_view from_version,
    std::string* error
) {
    @autoreleasepool {
        if (bundle.empty()) {
            set_why(error, "the new version could not be started");
            return false;
        }
        NSString* const path = [NSString stringWithUTF8String:utf8_of(bundle).c_str()];
        if (path == nullptr) {
            set_why(error, "the new version could not be started");
            return false;
        }
        NSMutableArray<NSString*>* const words = [NSMutableArray array];
        for (const std::string& argument : arguments) {
            NSString* const word = [NSString stringWithUTF8String:argument.c_str()];
            if (word == nullptr) {
                set_why(error, "the new version could not be started");
                return false;
            }
            [words addObject:word];
        }
        const std::string from(from_version);
        NSString* const flag = [NSString stringWithUTF8String:"--updated-from"];
        NSString* const version = [NSString stringWithUTF8String:from.c_str()];
        if (flag == nullptr || version == nullptr) {
            set_why(error, "the new version could not be started");
            return false;
        }
        [words addObject:flag];
        [words addObject:version];

        NSWorkspaceOpenConfiguration* const configuration =
            [NSWorkspaceOpenConfiguration configuration];
        configuration.createsNewApplicationInstance = YES;
        configuration.arguments = words;

        auto result = std::make_shared<StartResult>();
        NSURL* const url = [NSURL fileURLWithPath:path];
        [[NSWorkspace sharedWorkspace]
            openApplicationAtURL:url
                   configuration:configuration
               completionHandler:^(NSRunningApplication* application, NSError* open_error) {
                   std::lock_guard<std::mutex> guard(result->mutex);
                   result->started =
                       application != nil && open_error == nil && application.processIdentifier > 0;
                   if (!result->started && open_error != nil &&
                       open_error.localizedDescription != nil)
                       result->failure = open_error.localizedDescription.UTF8String;
                   result->finished = true;
               }];

        NSDate* const deadline = [NSDate dateWithTimeIntervalSinceNow:30.0];
        bool finished = false;
        while (!finished && [deadline timeIntervalSinceNow] > 0.0) {
            {
                std::lock_guard<std::mutex> guard(result->mutex);
                finished = result->finished;
            }
            if (finished)
                break;
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }
        std::lock_guard<std::mutex> guard(result->mutex);
        if (!result->finished) {
            set_why(error, "the new version did not start");
            return false;
        }
        if (!result->started) {
            set_why(
                error,
                result->failure.empty() ? "the new version could not be started" : result->failure
            );
            return false;
        }
        return true;
    }
}

} // namespace oa::update_helper
