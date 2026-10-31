// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The macOS bundle route. The new bundle is checked against the one that is
// running, then moved into its place. When the player cannot make those
// moves, macOS asks for an administrator's password and a second copy of
// this helper makes them, acting as the player for everything else. This
// file is built on Apple only.

#include "oa/update_helper/routes.hpp"

#include "oa/update_helper/paths.hpp"

#import <AppKit/AppKit.h>
#include <Security/Security.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <grp.h>
#include <mach-o/dyld.h>
#include <memory>
#include <mutex>
#include <optional>
#include <pwd.h>
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
    int pid{0};
    std::string failure;
};

bool same_bundle(NSRunningApplication* application, NSString* resolved_bundle) {
    if (application == nil || resolved_bundle == nil)
        return false;
    NSURL* const bundle_url = application.bundleURL;
    if (bundle_url != nil) {
        NSString* const resolved = [bundle_url.path stringByResolvingSymlinksInPath];
        if (resolved != nil && [resolved isEqualToString:resolved_bundle])
            return true;
    }
    NSURL* const executable = application.executableURL;
    if (executable == nil)
        return false;
    NSString* const resolved_executable = [executable.path stringByResolvingSymlinksInPath];
    if (resolved_executable == nil)
        return false;
    NSString* const prefix = [resolved_bundle stringByAppendingString:@"/"];
    return [resolved_executable hasPrefix:prefix];
}

bool pid_already(const std::vector<int>& pids, int pid) {
    return std::find(pids.begin(), pids.end(), pid) != pids.end();
}

std::vector<int> pids_already_running(NSString* resolved_bundle) {
    std::vector<int> pids;
    NSArray<NSRunningApplication*>* const running =
        [[NSWorkspace sharedWorkspace] runningApplications];
    for (NSRunningApplication* application in running) {
        if (!same_bundle(application, resolved_bundle))
            continue;
        const pid_t raw = application.processIdentifier;
        if (raw > 0)
            pids.push_back(static_cast<int>(raw));
    }
    return pids;
}

int listed_pid(NSString* resolved_bundle, const std::vector<int>& already, int own_pid) {
    NSArray<NSRunningApplication*>* const running =
        [[NSWorkspace sharedWorkspace] runningApplications];
    for (NSRunningApplication* application in running) {
        if (!same_bundle(application, resolved_bundle))
            continue;
        const pid_t raw = application.processIdentifier;
        if (raw <= 0)
            continue;
        const int candidate = static_cast<int>(raw);
        if (candidate == own_pid || pid_already(already, candidate))
            continue;
        return candidate;
    }
    return 0;
}

/// True when the state file confirms `version`. An empty path or version,
/// or a file that cannot be read, is not confirmed.
bool state_confirms(const fs::path& state_file, std::string_view version) {
    if (state_file.empty() || version.empty())
        return false;
    std::ifstream in(state_file, std::ios::binary);
    if (!in)
        return false;
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (end < 0 || end > static_cast<std::streamoff>(oa::app::update::max_plan_bytes))
        return false;
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty()) {
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(end));
        if (!in)
            return false;
    }
    const std::optional<oa::app::update::State> state = oa::app::update::read_state(bytes, nullptr);
    return state.has_value() && state->confirmed && state->installed_version == version;
}

/// The signing team of this process. Empty for an ad hoc signature.
///
/// @return the team
std::string own_team() {
    SecCodeRef own = nullptr;
    if (SecCodeCopySelf(kSecCSDefaultFlags, &own) != errSecSuccess || own == nullptr)
        return {};
    SecStaticCodeRef disk_code = nullptr;
    const OSStatus made = SecCodeCopyStaticCode(own, kSecCSDefaultFlags, &disk_code);
    CFRelease(own);
    if (made != errSecSuccess || disk_code == nullptr)
        return {};
    std::string team = team_of(disk_code);
    CFRelease(disk_code);
    return team;
}

/// The bundle identifier in a bundle's Info.plist. Empty when it names none.
///
/// @param bundle the bundle
/// @return the identifier
std::string identifier_of(const fs::path& bundle) {
    @autoreleasepool {
        NSString* const path = [NSString stringWithUTF8String:utf8_of(bundle).c_str()];
        if (path == nil)
            return {};
        NSBundle* const loaded = [NSBundle bundleWithPath:path];
        NSString* const identifier = loaded == nil ? nil : loaded.bundleIdentifier;
        return identifier == nil ? std::string() : std::string(identifier.UTF8String);
    }
}

/// Gives every entry under the folder `folder` names the owner `user` and
/// group `group`, without following a link. A file with another name
/// elsewhere is left as it is: its other name is not part of the tree.
///
/// @param folder an open folder; it is closed
/// @param user the owner
/// @param group the group
/// @return true when every entry was given that owner
bool own_entries(int folder, uid_t user, gid_t group) {
    DIR* const listed = ::fdopendir(folder);
    if (listed == nullptr) {
        ::close(folder);
        return false;
    }
    bool owned = true;
    while (const dirent* const entry = ::readdir(listed)) {
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..")
            continue;
        struct stat info{};
        if (::fstatat(::dirfd(listed), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
            owned = false;
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            const int child = ::openat(
                ::dirfd(listed), entry->d_name, O_RDONLY | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC
            );
            if (child < 0 || ::fchown(child, user, group) != 0) {
                if (child >= 0)
                    ::close(child);
                owned = false;
                continue;
            }
            owned = own_entries(child, user, group) && owned;
            continue;
        }
        if (S_ISREG(info.st_mode) && info.st_nlink > 1)
            continue;
        if (::fchownat(::dirfd(listed), entry->d_name, user, group, AT_SYMLINK_NOFOLLOW) != 0)
            owned = false;
    }
    ::closedir(listed);
    return owned;
}

/// Gives the tree at `top` the owner `user` and group `group`, when `top`
/// is still what `was` describes.
///
/// @param top the moved tree
/// @param was what was at the tree's old place before the move
/// @param user the owner
/// @param group the group
/// @return true when the whole tree has that owner
bool own_tree(const fs::path& top, const struct stat& was, uid_t user, gid_t group) {
    const int opened = ::open(top.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (opened < 0)
        return false;
    struct stat now{};
    if (::fstat(opened, &now) != 0 || now.st_ino != was.st_ino || now.st_dev != was.st_dev ||
        ::fchown(opened, user, group) != 0) {
        ::close(opened);
        return false;
    }
    if (!S_ISDIR(now.st_mode)) {
        ::close(opened);
        return true;
    }
    return own_entries(opened, user, group);
}

} // namespace

std::vector<Operation>
macos_checked_restore(const oa::app::update::Plan& plan, bool test_key, std::string* why) {
    if (why != nullptr)
        why->clear();
    const fs::path install = path_from(plan.install);
    const fs::path updates = path_from(plan.updates);
    const fs::path kept = macos_kept_bundle(updates, install);
    std::error_code io;
    if (!fs::is_directory(install, io) || io || fs::is_symlink(fs::symlink_status(install))) {
        set_why(why, "the installed bundle is missing");
        return {};
    }
    if (!fs::is_directory(kept, io) || io || fs::is_symlink(fs::symlink_status(kept))) {
        set_why(why, "the kept bundle is missing");
        return {};
    }
    BundleSignature installed;
    std::string failure;
    if (!read_signature(install, installed, failure)) {
        set_why(why, "the installed bundle's signature is not valid");
        return {};
    }
    BundleSignature kept_signature;
    if (!read_signature(kept, kept_signature, failure)) {
        set_why(why, "the kept bundle's signature is not valid");
        return {};
    }
    const MacosDecision decision = macos_may_swap(installed, kept_signature, test_key);
    if (!decision.go) {
        set_why(why, "the kept bundle may not replace the installed one: " + decision.why);
        return {};
    }
    std::string volume_why;
    if (volume_of(install, updates, &volume_why) != MacosVolume::same) {
        set_why(why, volume_why.empty() ? "the kept bundle is on another volume" : volume_why);
        return {};
    }
    Operation aside;
    aside.kind = Operation::Kind::remove_tree;
    aside.path = install;
    Operation back;
    back.kind = Operation::Kind::rename;
    back.from = kept;
    back.to = install;
    return {std::move(aside), std::move(back)};
}

fs::path macos_helper_program() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(static_cast<std::size_t>(size) + 1U, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    char resolved[PATH_MAX];
    if (::realpath(buffer.data(), resolved) == nullptr)
        return path_from(buffer.data());
    return path_from(resolved);
}

ElevatedRelaunch relaunch_elevated_macos(
    std::string_view prompt,
    const fs::path& helper,
    const std::vector<std::string>& arguments,
    void (*waiting)(void* context),
    void* context
) {
    if (helper.empty())
        return macos_elevation_outcome(false, 1, {}, {});
    std::vector<std::string> command = macos_elevation_command(prompt, helper, arguments);
    int output[2] = {-1, -1};
    int errors[2] = {-1, -1};
    if (::pipe(output) != 0)
        return macos_elevation_outcome(false, 1, {}, {});
    if (::pipe(errors) != 0) {
        ::close(output[0]);
        ::close(output[1]);
        return macos_elevation_outcome(false, 1, {}, {});
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    posix_spawn_file_actions_addclose(&actions, errors[0]);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errors[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, output[1]);
    posix_spawn_file_actions_addclose(&actions, errors[1]);
    std::vector<char*> argv;
    argv.reserve(command.size() + 1);
    for (std::string& word : command)
        argv.push_back(word.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(output[1]);
    ::close(errors[1]);
    if (spawned != 0) {
        ::close(output[0]);
        ::close(errors[0]);
        return macos_elevation_outcome(false, 1, {}, {});
    }
    ::fcntl(output[0], F_SETFL, O_NONBLOCK);
    ::fcntl(errors[0], F_SETFL, O_NONBLOCK);
    std::string said;
    std::string complained;
    const auto drain = [](int from, std::string& into) {
        char buffer[512];
        for (;;) {
            const ssize_t got = ::read(from, buffer, sizeof buffer);
            if (got > 0) {
                if (into.size() < 65536)
                    into.append(buffer, static_cast<std::size_t>(got));
                continue;
            }
            if (got < 0 && errno == EINTR)
                continue;
            return;
        }
    };
    int status = 0;
    for (;;) {
        drain(output[0], said);
        drain(errors[0], complained);
        const pid_t ended = ::waitpid(pid, &status, WNOHANG);
        if (ended == pid)
            break;
        if (ended < 0 && errno != EINTR) {
            status = -1;
            break;
        }
        if (waiting != nullptr)
            waiting(context);
        ::usleep(50000);
    }
    drain(output[0], said);
    drain(errors[0], complained);
    ::close(output[0]);
    ::close(errors[0]);
    const int code = status >= 0 && WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return macos_elevation_outcome(true, code, said, complained);
}

bool act_as_player(
    const fs::path& updates, const fs::path& plan_file, ActingPlayer& player, std::string* why
) {
    player = {};
    if (::getuid() != 0) {
        player.user = static_cast<uint32_t>(::geteuid());
        player.group = static_cast<uint32_t>(::getegid());
        return true;
    }
    struct stat folder{};
    if (::lstat(updates.c_str(), &folder) != 0 || !S_ISDIR(folder.st_mode)) {
        set_why(why, "the updates folder could not be read");
        return false;
    }
    if (folder.st_uid == 0) {
        set_why(why, "the updates folder belongs to the administrator, not a player");
        return false;
    }
    struct stat plan{};
    if (::lstat(plan_file.c_str(), &plan) != 0 || !S_ISREG(plan.st_mode) ||
        plan.st_uid != folder.st_uid) {
        set_why(why, "the plan does not belong to the player who owns the updates folder");
        return false;
    }
    const passwd* const entry = ::getpwuid(folder.st_uid);
    const gid_t group = entry != nullptr ? entry->pw_gid : folder.st_gid;
    if (entry != nullptr && ::initgroups(entry->pw_name, static_cast<int>(group)) != 0) {
        set_why(why, "the player's groups could not be taken");
        return false;
    }
    if (::setegid(group) != 0 || ::seteuid(folder.st_uid) != 0) {
        set_why(why, "the player's user could not be taken");
        return false;
    }
    player.user = static_cast<uint32_t>(folder.st_uid);
    player.group = static_cast<uint32_t>(group);
    player.can_raise = true;
    return true;
}

bool read_install_owner(RaisedMoves& moves) {
    struct stat info{};
    if (::lstat(moves.install.c_str(), &info) != 0)
        return false;
    moves.install_user = static_cast<uint32_t>(info.st_uid);
    moves.install_group = static_cast<uint32_t>(info.st_gid);
    return true;
}

bool macos_bundle_is_ours(
    const fs::path& bundle, const fs::path& identifier, bool test_key, std::string* why
) {
    BundleSignature signature;
    std::string failure;
    if (!read_signature(bundle, signature, failure) || !signature.valid) {
        set_why(why, "a bundle's signature is not valid");
        return false;
    }
    const std::string team = own_team();
    const bool ad_hoc = team.empty() && signature.team.empty();
    if (team.empty() ? !(test_key && ad_hoc) : signature.team != team) {
        set_why(why, "a bundle is not signed by the team that signed this helper");
        return false;
    }
    const std::string named = identifier_of(bundle);
    if (named.empty() || named != identifier_of(identifier)) {
        set_why(why, "a bundle is not this game");
        return false;
    }
    return true;
}

bool raised_rename(void* context, const fs::path& from, const fs::path& to) {
    auto* const moves = static_cast<RaisedMoves*>(context);
    struct stat was{};
    const bool known = ::lstat(from.c_str(), &was) == 0;
    const bool raise = moves->player.can_raise;
    if (raise && ::seteuid(0) != 0)
        return false;
    const bool moved = ::rename(from.c_str(), to.c_str()) == 0;
    bool owned = true;
    if (moved && raise && known) {
        const MovedOwner owner = moved_tree_owner(moves->updates, moves->install, to);
        if (owner == MovedOwner::player)
            owned = own_tree(to, was, moves->player.user, moves->player.group);
        else if (owner == MovedOwner::install)
            owned = own_tree(to, was, moves->install_user, moves->install_group);
    }
    // Every other file this copy writes must be the player's. A copy that
    // cannot act as the player again stops here; the journal it leaves is
    // undone by the next run.
    if (raise && ::seteuid(static_cast<uid_t>(moves->player.user)) != 0)
        std::abort();
    if (moves->log != nullptr && !owned)
        moves->log->line("a moved bundle kept its owner: " + utf8_of(to));
    return moved;
}

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
    const fs::path& state_file,
    std::string_view expect_version,
    std::string* error,
    int* pid
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
        if (!from_version.empty()) {
            const std::string from(from_version);
            NSString* const flag = [NSString stringWithUTF8String:"--updated-from"];
            NSString* const version = [NSString stringWithUTF8String:from.c_str()];
            if (flag == nullptr || version == nullptr) {
                set_why(error, "the new version could not be started");
                return false;
            }
            [words addObject:flag];
            [words addObject:version];
        }

        // Stay out of the Dock. The wait below also accepts a game the system
        // already lists, and a state file that becomes confirmed, when the
        // launch report does not arrive in time.
        NSApplication* const helper_app = [NSApplication sharedApplication];
        [helper_app setActivationPolicy:NSApplicationActivationPolicyAccessory];

        NSString* const resolved = [path stringByResolvingSymlinksInPath];
        if (resolved == nullptr) {
            set_why(error, "the new version could not be started");
            return false;
        }
        const std::vector<int> already = pids_already_running(resolved);
        const int own_pid = static_cast<int>(::getpid());
        const bool already_confirmed = state_confirms(state_file, expect_version);

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
                   if (result->started)
                       result->pid = static_cast<int>(application.processIdentifier);
                   if (!result->started && open_error != nil &&
                       open_error.localizedDescription != nil)
                       result->failure = open_error.localizedDescription.UTF8String;
                   result->finished = true;
               }];

        NSDate* const deadline = [NSDate dateWithTimeIntervalSinceNow:30.0];
        while (true) {
            MacosLaunchSeen seen;
            {
                std::lock_guard<std::mutex> guard(result->mutex);
                seen.callback_finished = result->finished;
                seen.callback_started = result->started;
                seen.callback_pid = result->pid == own_pid ? 0 : result->pid;
            }
            const int fresh = listed_pid(resolved, already, own_pid);
            if (fresh > 0) {
                seen.application_listed = true;
                seen.listed_pid = fresh;
            }
            seen.version_confirmed =
                !already_confirmed && state_confirms(state_file, expect_version);
            const MacosLaunchDecision decision = macos_launch_decision(seen);
            if (decision.launch == MacosLaunch::started) {
                if (pid != nullptr)
                    *pid = decision.pid;
                return true;
            }
            if (decision.launch == MacosLaunch::failed) {
                std::string failure;
                {
                    std::lock_guard<std::mutex> guard(result->mutex);
                    failure = result->failure;
                }
                set_why(error, failure.empty() ? "the new version could not be started" : failure);
                return false;
            }
            if ([deadline timeIntervalSinceNow] <= 0.0)
                break;
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                     beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }
        set_why(error, "the new version did not start");
        return false;
    }
}

} // namespace oa::update_helper
