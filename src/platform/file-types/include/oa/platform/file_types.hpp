// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Makes the running copy of the game the opener of the eight file types for the user who runs
// it, with the game's icon on those files where the system takes one: the registry under
// HKEY_CURRENT_USER on Windows, and on Linux the MIME package, desktop entries and icons in
// the user's XDG data folder, with the system's database tools run after a change. Seven are
// files the game installs or adds; the eighth, a launch file, starts the game in its battle
// room (-c FILE). macOS and iOS declare the types in the bundle's Info.plist. On macOS a
// switch chooses whether this copy is the default opener of each of the eight, and only a
// switch that changed is asked; a start never replaces a choice made in Finder. iOS does
// nothing at run time. Each registration writes only what differs from what is there. On
// Windows the installer also associates launch files for every user of the computer; a
// withdrawal of launch files hides that association from the user who runs it, under
// HKEY_CURRENT_USER, and never writes the computer's own key.
#pragma once

#include <stdint.h>

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::platform::file_types {

/// The mod package's file name extension, without its dot; matched without case.
inline constexpr std::string_view mod_extension = "oamod";
/// The MIME type of a mod package.
inline constexpr std::string_view mod_mime_type = "application/x-oamod";
/// What the system calls a mod package in its file managers.
inline constexpr std::string_view mod_type_name = "Open Annihilation mod";
/// The Windows program identifier the mod package's extension names, one of the eight file types.
inline constexpr std::string_view mod_program_id = "OpenAnnihilation.Mod";
/// A language pack's file name extension, without its dot; matched without case.
inline constexpr std::string_view language_extension = "oalang";
/// The MIME type of a language pack.
inline constexpr std::string_view language_mime_type = "application/x-oalang";
/// What the system calls a language pack in its file managers.
inline constexpr std::string_view language_type_name = "Open Annihilation language pack";
/// The Windows program identifier a language pack's extension names.
inline constexpr std::string_view language_program_id = "OpenAnnihilation.Language";
/// A map pack's file name extension, without its dot; matched without case.
inline constexpr std::string_view map_extension = "oamap";
/// The MIME type of a map pack.
inline constexpr std::string_view map_mime_type = "application/x-oamap";
/// What the system calls a map pack in its file managers.
inline constexpr std::string_view map_type_name = "Open Annihilation map pack";
/// The Windows program identifier a map pack's extension names.
inline constexpr std::string_view map_program_id = "OpenAnnihilation.MapPack";
/// A mutator package's file name extension, without its dot; matched without case.
inline constexpr std::string_view mutator_extension = "oamut";
/// The MIME type of a mutator package.
inline constexpr std::string_view mutator_mime_type = "application/x-oamut";
/// What the system calls a mutator package in its file managers.
inline constexpr std::string_view mutator_type_name = "Open Annihilation mutator";
/// The Windows program identifier a mutator package's extension names.
inline constexpr std::string_view mutator_program_id = "OpenAnnihilation.Mutator";
/// A brain package's file name extension, without its dot; matched without case.
inline constexpr std::string_view brain_extension = "oabrain";
/// The MIME type of a brain package.
inline constexpr std::string_view brain_mime_type = "application/x-oabrain";
/// What the system calls a brain package in its file managers.
inline constexpr std::string_view brain_type_name = "Open Annihilation brain";
/// The Windows program identifier a brain package's extension names.
inline constexpr std::string_view brain_program_id = "OpenAnnihilation.Brain";
/// A key layout package's file name extension, without its dot; matched without case.
inline constexpr std::string_view keys_extension = "oakeys";
/// The MIME type of a key layout package.
inline constexpr std::string_view keys_mime_type = "application/x-oakeys";
/// What the system calls a key layout package in its file managers.
inline constexpr std::string_view keys_type_name = "Open Annihilation key layout";
/// The Windows program identifier a key layout package's extension names.
inline constexpr std::string_view keys_program_id = "OpenAnnihilation.KeyLayout";
/// A registry file's extension, without its dot; matched without case. A .oareg file.
inline constexpr std::string_view registry_extension = "oareg";
/// The MIME type of a .oareg registry file.
inline constexpr std::string_view registry_mime_type = "application/x-oareg";
/// What the system calls a .oareg registry file in its file managers.
inline constexpr std::string_view registry_type_name = "Open Annihilation registry";
/// The Windows program identifier a .oareg file's extension names.
inline constexpr std::string_view registry_program_id = "OpenAnnihilation.Registry";
/// A launch file's extension, without its dot; matched without case. A .oalaunch file.
inline constexpr std::string_view launch_extension = "oalaunch";
/// The MIME type of a .oalaunch launch file.
inline constexpr std::string_view launch_mime_type = "application/x-oalaunch";
/// What the system calls a .oalaunch launch file in its file managers.
inline constexpr std::string_view launch_type_name = "Open Annihilation launch file";
/// The Windows program identifier a .oalaunch file's extension names.
inline constexpr std::string_view launch_program_id = "OpenAnnihilation.Launch";
/// What a launch file's text begins with, after an optional UTF-8 byte order mark; the MIME
/// package's magic, so that a launch with another name is known by its content.
inline constexpr std::string_view launch_magic = "oalaunch:";
/// The name of the desktop entry, MIME package and program icon on Linux, without their
/// extensions; also the macOS and iOS bundle identifier.
inline constexpr std::string_view desktop_id = "net.coreprime.open-annihilation";
/// The name of the desktop entry that opens launch files on Linux, without its extension.
inline constexpr std::string_view launch_desktop_id = "net.coreprime.open-annihilation.launch";
/// The command-line option the registrations use to open one of the seven files the game
/// installs or adds.
inline constexpr std::string_view open_option = "--open";
/// The command-line option the registrations use to open a launch file: the game starts with
/// it as its launch.
inline constexpr std::string_view launch_option = "-c";
/// The most milliseconds a database tool may run before it is stopped.
inline constexpr uint32_t default_tool_time_limit_ms = 5000;

/// What the game does with a file of one of the types it opens.
enum class Opening : uint8_t {
    /// installs it, or adds the registry it names, once the main menu shows: --open FILE on
    /// Windows, a bare argument on Linux
    install,
    /// takes it as the launch, which opens its battle room: -c FILE
    launch,
};

/// One of the eight file types the game opens.
struct FileType {
    std::string_view extension{};        ///< the file name extension, without its dot
    std::string_view mime_type{};        ///< the MIME type
    std::string_view type_name{};        ///< what file managers call it
    std::string_view program_id{};       ///< the Windows program identifier
    std::string_view parent_mime_type{}; ///< the MIME type it is a kind of
    Opening opening{Opening::install};   ///< what the game does with such a file
};

/// The eight file types, in the order a registration writes them: the six packages, each a
/// kind of zip, a .oareg registry file and a .oalaunch launch file, each a kind of plain text.
inline constexpr std::array<FileType, 8> file_types{{
    {mod_extension, mod_mime_type, mod_type_name, mod_program_id, "application/zip"},
    {language_extension,
     language_mime_type,
     language_type_name,
     language_program_id,
     "application/zip"},
    {map_extension, map_mime_type, map_type_name, map_program_id, "application/zip"},
    {mutator_extension,
     mutator_mime_type,
     mutator_type_name,
     mutator_program_id,
     "application/zip"},
    {brain_extension, brain_mime_type, brain_type_name, brain_program_id, "application/zip"},
    {keys_extension, keys_mime_type, keys_type_name, keys_program_id, "application/zip"},
    {registry_extension, registry_mime_type, registry_type_name, registry_program_id, "text/plain"},
    {launch_extension,
     launch_mime_type,
     launch_type_name,
     launch_program_id,
     "text/plain",
     Opening::launch},
}};

/// One file type as macOS names it, and the content type a switch that is off hands back to.
/// The identifiers are the ones the bundle exports. The bundle declares each package and the
/// registry file as public.data, so a browser does not unpack a package; a launch file is
/// plain text. Handing a type off gives it to the default opener of a zip archive, or of
/// plain text for a registry or launch file.
struct ExportedContentType {
    std::string_view extension{};     ///< the file name extension, without its dot
    std::string_view content_type{};  ///< the exported type identifier
    std::string_view hands_back_to{}; ///< the content type Off asks for an opener
};

/// The eight exported content types, in the same order as file_types.
inline constexpr std::array<ExportedContentType, 8> exported_content_types{{
    {mod_extension, "net.coreprime.open-annihilation.oamod", "public.zip-archive"},
    {language_extension, "net.coreprime.open-annihilation.oalang", "public.zip-archive"},
    {map_extension, "net.coreprime.open-annihilation.oamap", "public.zip-archive"},
    {mutator_extension, "net.coreprime.open-annihilation.oamut", "public.zip-archive"},
    {brain_extension, "net.coreprime.open-annihilation.oabrain", "public.zip-archive"},
    {keys_extension, "net.coreprime.open-annihilation.oakeys", "public.zip-archive"},
    {registry_extension, "net.coreprime.open-annihilation.oareg", "public.plain-text"},
    {launch_extension, "net.coreprime.open-annihilation.oalaunch", "public.plain-text"},
}};

/// Which of the eight file types a registration or a withdrawal acts on. Each is on unless a
/// caller turns it off, so a call that names no set acts on all eight. Settings has a switch
/// for each of the eight. `keys` is last, so an aggregate that names the earlier fields
/// keeps them and takes the layout on.
struct TypeSet {
    bool mod{true};      ///< a mod package, .oamod
    bool language{true}; ///< a language pack, .oalang
    bool map{true};      ///< a map pack, .oamap
    bool registry{true}; ///< a registry file, .oareg
    bool launch{true};   ///< a launch file, .oalaunch
    bool mutator{true};  ///< a mutator package, .oamut
    bool brain{true};    ///< a brain package, .oabrain
    bool keys{true};     ///< a key layout package, .oakeys

    /// Reports whether the set holds the file type whose extension is `extension`.
    ///
    /// @param extension the extension, without its dot
    /// @return true when that type is in the set
    [[nodiscard]] constexpr bool contains(std::string_view extension) const noexcept {
        if (extension == mod_extension)
            return mod;
        if (extension == language_extension)
            return language;
        if (extension == map_extension)
            return map;
        if (extension == mutator_extension)
            return mutator;
        if (extension == brain_extension)
            return brain;
        if (extension == keys_extension)
            return keys;
        if (extension == registry_extension)
            return registry;
        if (extension == launch_extension)
            return launch;
        return false;
    }

    /// Reports whether the set holds one of the seven files the game installs or adds.
    [[nodiscard]] constexpr bool installs_any() const noexcept {
        return mod || language || map || mutator || brain || keys || registry;
    }

    /// Reports whether the set holds none of the eight.
    [[nodiscard]] constexpr bool none() const noexcept { return !installs_any() && !launch; }

    /// Reports whether the set holds all eight.
    [[nodiscard]] constexpr bool all() const noexcept {
        return mod && language && map && registry && launch && mutator && brain && keys;
    }

    /// Returns the eight types this set does not hold.
    [[nodiscard]] constexpr TypeSet complement() const noexcept {
        return TypeSet{!mod, !language, !map, !registry, !launch, !mutator, !brain, !keys};
    }
};

/// The file types the Windows installer associates with the game for everyone who uses the
/// computer: launch files alone. A withdrawal hides that association from the user who runs it
/// (user_override_step).
inline constexpr TypeSet installer_types{false, false, false, false, true, false, false, false};

/// The (Default) of an extension's key that hides the association every user shares from one
/// user: an empty program identifier, which names no program. Windows reads the user's own
/// classes over the computer's, so the type then has no opener for that user.
inline constexpr std::string_view user_override_program_id = "";

/// What a withdrawal of one of installer_types does about the association every user of the
/// computer shares.
enum class UserOverrideStep : uint8_t {
    /// nothing: that association is not the game's, and the user has no override
    none,
    /// write the override: that association is the game's and the user has no (Default) of
    /// their own
    write,
    /// leave the override: it is there and that association is still the game's
    keep,
    /// remove the override: that association is no longer the game's, so the override would
    /// hide another program's or nothing
    remove,
};

/// Decides what a withdrawal of one of installer_types does about the association every user of
/// the computer shares, from the extension's (Default) under the computer's classes and under
/// the user's own, read after the user's registration was withdrawn. The override is the user's
/// (Default) holding user_override_program_id. A (Default) of the user's own that names a
/// program, the player's choice or another program's, is never written over or removed.
///
/// @param program_id the type's program identifier, as the installer writes it
/// @param machine_default the computer's (Default) for the extension; nullopt when missing
/// @param user_default the user's (Default) for the extension; nullopt when missing
/// @return what to do
[[nodiscard]] UserOverrideStep user_override_step(
    std::string_view program_id,
    std::optional<std::string_view> machine_default,
    std::optional<std::string_view> user_default
) noexcept;

/// A root of the Windows registry.
enum class RegistryRoot : uint8_t {
    current_user,  ///< HKEY_CURRENT_USER
    local_machine, ///< HKEY_LOCAL_MACHINE
};

/// Where a registration writes, and what it runs after a change; tests name scratch ones.
struct Places {
    /// The XDG data folder; empty for $XDG_DATA_HOME when it is absolute, else
    /// $HOME/.local/share.
    std::filesystem::path data_home{};
    /// Windows: the key under HKEY_CURRENT_USER that holds the file types.
    std::string classes_key{"Software\\Classes"};
    /// Windows: the root of machine_classes_key. The game only reads there; a test names
    /// HKEY_CURRENT_USER and a scratch key.
    RegistryRoot machine_classes_root{RegistryRoot::local_machine};
    /// Windows: the key that holds the file types every user of the computer shares, which the
    /// installer writes and the game only reads.
    std::string machine_classes_key{"Software\\Classes"};
    /// XDG: run the database tools (update-mime-database, update-desktop-database and, where
    /// the user's icon cache exists, gtk-update-icon-cache) whose run is due: after a change,
    /// and until a run succeeds.
    bool run_tools{true};
    /// Windows: tell the shell that a file association changed, after a change.
    bool notify_shell{true};
    /// XDG: the most milliseconds a database tool may run before it is stopped.
    uint32_t tool_time_limit_ms{default_tool_time_limit_ms};
};

/// What a registration did.
struct Registration {
    bool supported{}; ///< this system registers at run time (Windows, Linux)
    bool changed{};   ///< something was written
    /// What was written, skipped or run, one line each, for the log.
    std::vector<std::string> lines{};
    std::string error{}; ///< why it stopped, for the log; empty when it did not
};

/// Returns the program's path as the system would start it again: the executable's own path
/// on Windows (the module's file name) and on Linux (/proc/self/exe).
///
/// @return the path; nullopt where it is unknown, where the executable was deleted or replaced
///         since it started, and on macOS and iOS, which register through the bundle
[[nodiscard]] std::optional<std::filesystem::path> running_executable();

/// Registers `executable` as the opener of the file types in `types` for the user who runs it,
/// writing only what differs from what is there, with `icon_png` as the program's and the
/// files' icon where the system takes an icon file (XDG). On Linux the MIME package and the
/// two desktop entries then list exactly those types, a type left out loses its mimetype icon,
/// the desktop entry of the seven is removed with none of them, the launch file's entry
/// without it, and the MIME package with none of the eight. A Windows copy
/// started from the temporary folder, as one started from inside a zip is, and a Linux copy
/// under $TMPDIR or /tmp, inside a Flatpak or a Snap, or whose path holds a control character
/// or is not UTF-8, register nothing and say so in a line. macOS and iOS register through the
/// bundle's Info.plist: there supported is false and nothing is done. Never throws.
///
/// @param executable the program, absolute, as running_executable gives it
/// @param icon_png the icon, a PNG file's bytes; empty writes no icon
/// @param places where it writes; the defaults are the user's own
/// @param types the file types to register; all eight when omitted
/// @return what was done
[[nodiscard]] Registration register_file_types(
    const std::filesystem::path& executable,
    std::span<const uint8_t> icon_png,
    const Places& places = {},
    TypeSet types = {}
);

/// Withdraws the registration of the file types in `types` for the user who runs it. On
/// Windows it removes each type's program-identifier key and that program identifier's value
/// under OpenWithProgids, and the extension's (Default) and Content Type values only while
/// they still name this game's program identifier and MIME type. Another program's values,
/// and the player's own choice of opener, stay. While the installer's association for every
/// user names the game, a launch file's withdrawal also writes the user's override, which hides
/// that association from this user alone (withdraw_windows). On Linux it takes those types out of the MIME
/// package and the desktop entries' MimeType lists, removes their mimetype icons, and removes
/// a desktop entry or the MIME package once it lists no type. The program's own icon
/// stays. A missing value is already withdrawn. The same skips as a registration apply on
/// Linux (a Flatpak, a Snap). macOS and iOS withdraw nothing at run time: there supported is
/// false. On macOS the switches call apply_opener_changes, which changes the default opener
/// and leaves the declaration. The shell, or the database tools, are told as a write tells
/// them. Never throws.
///
/// @param types the file types to withdraw
/// @param places where it writes; the defaults are the user's own
/// @return what was done
[[nodiscard]] Registration withdraw_file_types(TypeSet types, const Places& places = {});

#ifndef _WIN32
/// Registers `executable` as the opener of the file types in `types` in an XDG data folder:
/// the MIME package (mime_package), the desktop entry of the seven the game installs or adds
/// (desktop_entry), the launch file's desktop entry (launch_desktop_entry) and the icon as the
/// program's and each included type's, 256 pixels square, in the hicolor theme. A type left
/// out loses its mimetype icon. A desktop entry that would list no type, and the MIME package
/// with none of the eight, are removed instead of written empty, and the program's icon stays.
/// Each file is written
/// only when its bytes differ, through a temporary file renamed over it. After a change it
/// runs update-mime-database for a new MIME package, update-desktop-database for a new desktop
/// entry, and gtk-update-icon-cache for a new icon where the user's hicolor folder holds an
/// icon cache, without a shell, each stopped after places.tool_time_limit_ms; a tool that is
/// not installed is no error. A run that succeeds leaves a stamp file in the folder the tool
/// works on (a dot, desktop_id and ".updated"), which a change to its files removes first;
/// while a tool's stamp is missing, each later start runs it again. Built on every system but
/// Windows, so that its tests run there too; register_file_types calls it on Linux only.
/// Never throws.
///
/// @param executable the program, absolute
/// @param icon_png the icon, a PNG file's bytes; empty writes no icon
/// @param places where it writes, and whether and how long it runs the tools
/// @param types the file types to register; all eight when omitted
/// @return what was done
[[nodiscard]] Registration register_xdg(
    const std::filesystem::path& executable,
    std::span<const uint8_t> icon_png,
    const Places& places,
    TypeSet types = {}
);

/// Withdraws the file types in `types` from an XDG data folder: each is taken out of the MIME
/// package and the desktop entries, its mimetype icon is removed, and a desktop entry or the
/// MIME package is removed once it lists no type. The program's icon stays. A file that
/// is already absent is left so. The stamp rules are the ones a write uses. Built on every
/// system but Windows, so that its tests run there too. Never throws.
///
/// @param types the file types to withdraw
/// @param places where it writes, and whether and how long it runs the tools
/// @return what was done
[[nodiscard]] Registration withdraw_xdg(TypeSet types, const Places& places);
#else
/// Registers `executable` as the opener of the file types in `types` in the registry under
/// HKEY_CURRENT_USER\<places.classes_key>: each extension's program identifier and MIME type,
/// the program identifier's name, its icon (the executable's first icon, default_icon) and
/// its open command (open_command, with --open, or -c for a launch file). Each value is
/// written only when it is missing or differs,
/// and the shell is told once after any write. A type left out is not written and not removed;
/// withdraw_windows removes one. The extension's (Default) written over the user's override
/// (user_override_program_id) takes the place of that override. A choice of opener the player
/// made in Windows is never touched. Uses only calls Windows XP has. Never throws.
///
/// @param executable the program, absolute
/// @param places where it writes, and whether it tells the shell
/// @param types the file types to register; all eight when omitted
/// @return what was done
[[nodiscard]] Registration
register_windows(const std::filesystem::path& executable, const Places& places, TypeSet types = {});

/// Withdraws the file types in `types` from the registry under
/// HKEY_CURRENT_USER\<places.classes_key>. For each type it deletes the program-identifier
/// key, leaf first, and that program identifier's value under the extension's OpenWithProgids.
/// The extension's (Default) value is deleted only while its text is this game's program
/// identifier, and its Content Type value only while its text is this game's MIME type. Any
/// other value, including another program's OpenWithProgids value and the player's UserChoice,
/// is left as it is. A missing value is already withdrawn. For a type of installer_types it
/// then reads the extension's (Default) under places.machine_classes_key, and writes or removes
/// the user's override as user_override_step says, so that an association the installer made
/// for every user no longer opens the type for this one. Nothing under the machine's key is
/// ever written. The shell is told once after any removal or write, as a registration tells
/// it. Uses only calls Windows XP has. Never throws.
///
/// @param types the file types to withdraw
/// @param places where it writes, and whether it tells the shell
/// @return what was done
[[nodiscard]] Registration withdraw_windows(TypeSet types, const Places& places);
#endif

/// Returns the desktop entry that opens the seven file types the game installs or adds that
/// are in `types` with `executable`, the file as a bare argument, with no entry in the
/// application menus. A type left out is absent from MimeType; a launch file is never in it.
///
/// @param executable the program, absolute; its UTF-8 spelling is written
/// @param types the file types the entry opens; all seven when omitted
/// @return the entry's text, UTF-8; empty when the path cannot be written in an entry
///         (desktop_exec_argument)
[[nodiscard]] std::string
desktop_entry(const std::filesystem::path& executable, TypeSet types = {});

/// Returns the desktop entry that opens a launch file with `executable` as `-c FILE`, with
/// no entry in the application menus. Its MimeType is the launch file's alone.
///
/// @param executable the program, absolute; its UTF-8 spelling is written
/// @return the entry's text, UTF-8; empty when the path cannot be written in an entry
///         (desktop_exec_argument)
[[nodiscard]] std::string launch_desktop_entry(const std::filesystem::path& executable);

/// Returns the shared-mime-info package that names the file types in `types`, each a kind of
/// its parent: the six packages a kind of zip, a .oareg file and a .oalaunch file each a
/// kind of plain text. A launch file is also known by its content, a text that begins with
/// launch_magic, whatever its name. A type left out has no mime-type element.
///
/// @param types the file types the package names; all eight when omitted
/// @return the package's XML text, UTF-8
[[nodiscard]] std::string mime_package(TypeSet types = {});

/// Returns a path as one argument of a desktop entry's Exec line: in double quotes, with ",
/// `, $ and \ preceded by a backslash, every backslash then doubled by the entry's own escape
/// rule, and each % written %%.
///
/// @param path_utf8 the path, UTF-8
/// @return the argument; empty when the path holds a control character or is not UTF-8,
///         which an entry cannot hold
[[nodiscard]] std::string desktop_exec_argument(std::string_view path_utf8);

/// Returns the Windows command that opens a file with `executable`: "<executable>" --open
/// "%1" for the seven the game installs or adds, "<executable>" -c "%1" for a launch file.
///
/// @param executable the program, absolute
/// @param opening what the game does with the file
/// @return the command, UTF-8
[[nodiscard]] std::string
open_command(const std::filesystem::path& executable, Opening opening = Opening::install);

/// Returns the Windows icon location of `executable`'s first icon: <executable>,0, which the
/// system splits at its last comma.
///
/// @param executable the program, absolute
/// @return the location, UTF-8
[[nodiscard]] std::string default_icon(const std::filesystem::path& executable);

/// The program that opens a content type: its bundle identifier, and the file URL of that
/// copy where the system names one.
struct Handler {
    std::string bundle_id{};       ///< the opener's bundle identifier; empty when none is set
    std::string application_url{}; ///< the opener's file URL; empty when the system names none
};

/// The two questions a default-opener change asks. A test fills both; macOS fills them from
/// the system. A null function is a system that has no such question.
struct HandlerCalls {
    void* context{}; ///< handed to both functions
    /// The default opener of `content_type`. Sets `failure` and returns an empty handler when
    /// the question fails.
    Handler (*default_handler)(
        void* context, std::string_view content_type, std::string& failure
    ){};
    /// Makes `handler` the default opener of `content_type`. Returns false and sets `failure`
    /// when it does not.
    bool (*set_handler)(
        void* context, std::string_view content_type, const Handler& handler, std::string& failure
    ){};
};

/// One switch that moved: the extension, and whether this copy should open it.
struct OpenerChange {
    std::string_view extension{}; ///< one of the eight, without its dot
    bool open{}; ///< true: this copy; false: the opener of the type it hands back to
};

/// What the system reports for the eight types.
struct OpenerReport {
    /// On when this copy's bundle identifier is the default opener. A type whose question
    /// failed keeps the value `fallback` held.
    TypeSet open{};
    std::string error{}; ///< the questions that failed, for the log; empty when none did
};

/// Reports whether System's switches choose a default opener on this system: macOS does,
/// and a start does not apply them. Windows and Linux register instead, and iOS does neither.
[[nodiscard]] bool default_opener_is_a_setting() noexcept;

/// Returns the calls the system itself answers. Both functions are null where this system
/// has no default-opener question (everywhere but macOS).
[[nodiscard]] HandlerCalls system_handler_calls();

/// Returns this copy of the program, as a bundle. Both strings are empty when it is not an
/// application bundle, and everywhere but macOS.
[[nodiscard]] Handler running_application();

/// Reports which of the eight this copy opens by default, asking `calls`. A null
/// default_handler leaves `fallback` as it was and records no error. Never throws.
///
/// @param calls the questions; a test's, or system_handler_calls
/// @param bundle_id this copy's bundle identifier
/// @param fallback what a failed question leaves that switch showing
/// @return the eight types, and any question that failed
[[nodiscard]] OpenerReport
reported_openers(const HandlerCalls& calls, std::string_view bundle_id, TypeSet fallback);

/// The extensions, each with its dot, in the order the switches are listed, that `asked` turned
/// off and `open` still has this copy opening. Empty when none stayed.
///
/// @param asked what OK asked
/// @param open what the system reports now
/// @return those extensions
[[nodiscard]] std::vector<std::string> extensions_still_opened(TypeSet asked, TypeSet open);

/// Asks `calls` to make each change in `changes`. A switch that is on sets `application`. A
/// switch that is off asks for the default opener of the content type that file hands back
/// to, and sets that opener on the file's own content type. A change whose question fails is
/// skipped; the others are still asked. Never throws.
///
/// @param calls the questions; a test's, or system_handler_calls
/// @param application this copy, as running_application gives it
/// @param changes the switches that moved
/// @return what was asked; supported is false when `calls` cannot set an opener
[[nodiscard]] Registration apply_opener_changes(
    const HandlerCalls& calls, const Handler& application, std::span<const OpenerChange> changes
);

} // namespace oa::platform::file_types
