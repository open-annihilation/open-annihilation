// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// System's seven file-type switches: each stands for its own type alone, so the launch file's
// switch, moved and kept with OK, registers and withdraws launch files alone and leaves the
// other six types as they were. On every system but Windows the XDG files are written in a
// scratch data folder, and on Windows the registry values under a scratch key of
// HKEY_CURRENT_USER, as OK writes them there; with the installer's association for every user
// under a second scratch key, Off hides it from this user and On puts this user's registration
// back. The macOS half asks a stand-in for the system's default-opener questions and changes
// only the launch file's opener. Nothing is registered for the user who runs it, no other
// program is started, and HKEY_LOCAL_MACHINE is neither read nor written.

#include "oa/ui/engine_settings/file_types.hpp"

#include "oa/platform/file_types.hpp"
#include "oa/ui/engine_settings.hpp"

#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <stdint.h>

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlwapi.h>
#else
#include <cstdlib>
#include <fstream>
#include <iterator>
#endif

namespace {

namespace fs = std::filesystem;
namespace settings = oa::ui::engine_settings;
namespace file_types = oa::platform::file_types;

/// One switch and the extension of the type it stands for.
struct SwitchCase {
    bool settings::EngineSettings::* member{}; ///< the switch
    std::string_view extension{};              ///< its type's extension, without its dot
};

/// The seven switches, in the order System lists them.
constexpr std::array<SwitchCase, 7> switch_cases{{
    {&settings::EngineSettings::file_type_oamod, file_types::mod_extension},
    {&settings::EngineSettings::file_type_oamap, file_types::map_extension},
    {&settings::EngineSettings::file_type_oalang, file_types::language_extension},
    {&settings::EngineSettings::file_type_oareg, file_types::registry_extension},
    {&settings::EngineSettings::file_type_oamut, file_types::mutator_extension},
    {&settings::EngineSettings::file_type_oabrain, file_types::brain_extension},
    {&settings::EngineSettings::file_type_oalaunch, file_types::launch_extension},
}};

/// Every switch on, as a first start and the defaults have them.
void every_switch_starts_on() {
    const settings::EngineSettings defaults{};
    OA_CHECK(defaults.file_type_oalaunch);
    OA_CHECK(settings::file_type_set(defaults).all());
    OA_CHECK(settings::file_type_set(defaults).complement().none());
    OA_CHECK(settings::moved_file_type_switches(defaults, defaults).empty());
}

/// Each switch off alone takes its own type out of the set and puts it, alone, in the set a
/// withdrawal acts on; moved, it asks for its own opener alone. Read back, the set gives the
/// same switches.
void each_switch_stands_for_its_own_type() {
    const settings::EngineSettings opened{};
    for (const SwitchCase& off : switch_cases) {
        settings::EngineSettings chosen = opened;
        chosen.*off.member = false;
        const file_types::TypeSet enabled = settings::file_type_set(chosen);
        const file_types::TypeSet withdrawn = enabled.complement();
        for (const file_types::FileType& type : file_types::file_types) {
            OA_CHECK(enabled.contains(type.extension) == (type.extension != off.extension));
            OA_CHECK(withdrawn.contains(type.extension) == (type.extension == off.extension));
        }
        const std::vector<file_types::OpenerChange> changes =
            settings::moved_file_type_switches(opened, chosen);
        OA_CHECK(changes.size() == 1);
        OA_CHECK(!changes.empty() && changes[0].extension == off.extension && !changes[0].open);
        const std::vector<file_types::OpenerChange> back =
            settings::moved_file_type_switches(chosen, opened);
        OA_CHECK(back.size() == 1 && back[0].extension == off.extension && back[0].open);

        settings::EngineSettings shown{};
        settings::set_file_type_switches(shown, enabled);
        OA_CHECK(shown == chosen);
    }
    // The launch file's switch is the last, and the order is System's.
    settings::EngineSettings none{};
    for (const SwitchCase& off : switch_cases)
        none.*off.member = false;
    const std::vector<file_types::OpenerChange> every =
        settings::moved_file_type_switches(settings::EngineSettings{}, none);
    OA_CHECK(every.size() == switch_cases.size());
    for (std::size_t index = 0; index < every.size() && index < switch_cases.size(); ++index)
        OA_CHECK(every[index].extension == switch_cases[index].extension);
    OA_CHECK(settings::file_type_set(none).none());
}

#ifndef _WIN32
/// Reads a whole file.
std::string read_file(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

/// Clears an environment variable for the rest of a case and puts it back afterwards.
class ClearedVariable {
  public:

    explicit ClearedVariable(const char* name) : name_(name) {
        if (const char* old = std::getenv(name))
            old_ = std::string(old);
        unsetenv(name);
    }

    ~ClearedVariable() {
        if (old_)
            setenv(name_.c_str(), old_->c_str(), 1);
    }

    ClearedVariable(const ClearedVariable&) = delete;
    ClearedVariable& operator=(const ClearedVariable&) = delete;

  private:

    std::string name_{};
    std::optional<std::string> old_{};
};

/// The launch file's switch off, then on again, in an XDG data folder, as OK does on Linux:
/// register the types left on, then withdraw the rest. The launch file's desktop entry, its icon
/// and its part of the MIME package go and come back; the desktop entry of the six, their icons
/// and the program's icon are never written or removed.
void the_launch_switch_registers_and_withdraws_launch_files_alone_in_xdg() {
    // A Flatpak or Snap registers nothing; this case is neither.
    const ClearedVariable flatpak("FLATPAK_ID");
    const ClearedVariable snap("SNAP");
    const fs::path root = oa::test::make_scratch_directory("oa-ui-engine-settings-file-types");
    const fs::path data = root / "data";
    file_types::Places places{};
    places.data_home = data;
    places.run_tools = false;
    const fs::path executable("/opt/Open Annihilation/open-annihilation");
    std::vector<uint8_t> icon(300);
    for (std::size_t index = 0; index < icon.size(); ++index)
        icon[index] = static_cast<uint8_t>(index + 7U);

    const fs::path package = data / "mime/packages/net.coreprime.open-annihilation.xml";
    const fs::path launch_entry =
        data / "applications/net.coreprime.open-annihilation.launch.desktop";
    const fs::path launch_icon =
        data / "icons/hicolor/256x256/mimetypes/application-x-oalaunch.png";
    // The files of the other six, and the program's icon.
    const std::array<fs::path, 8> others{
        data / "applications/net.coreprime.open-annihilation.desktop",
        data / "icons/hicolor/256x256/mimetypes/application-x-oamod.png",
        data / "icons/hicolor/256x256/mimetypes/application-x-oamap.png",
        data / "icons/hicolor/256x256/mimetypes/application-x-oalang.png",
        data / "icons/hicolor/256x256/mimetypes/application-x-oamut.png",
        data / "icons/hicolor/256x256/mimetypes/application-x-oabrain.png",
        data / "icons/hicolor/256x256/mimetypes/application-x-oareg.png",
        data / "icons/hicolor/256x256/apps/net.coreprime.open-annihilation.png",
    };

    // A start with every switch on registers the seven.
    const settings::EngineSettings opened{};
    file_types::Registration done =
        file_types::register_xdg(executable, icon, places, settings::file_type_set(opened));
    OA_CHECK(done.supported && done.changed && done.error.empty());
    OA_CHECK(read_file(launch_entry) == file_types::launch_desktop_entry(executable));
    OA_CHECK(fs::exists(launch_icon));
    std::map<fs::path, std::string> before;
    for (const fs::path& file : others) {
        OA_CHECK(fs::exists(file));
        before[file] = read_file(file);
    }
    // No line of a later step names a file of the other six, or the program's icon.
    const auto lines_leave_the_others = [&others](const file_types::Registration& step) {
        for (const std::string& line : step.lines)
            for (const fs::path& file : others)
                if (line.find(file.string()) != std::string::npos)
                    return false;
        return true;
    };

    // OK with the launch file's switch off.
    settings::EngineSettings chosen = opened;
    chosen.file_type_oalaunch = false;
    const file_types::TypeSet without_launch = settings::file_type_set(chosen);
    done = file_types::register_xdg(executable, icon, places, without_launch);
    OA_CHECK(done.supported && done.error.empty());
    file_types::Registration withdrawn =
        file_types::withdraw_xdg(without_launch.complement(), places);
    OA_CHECK(withdrawn.supported && withdrawn.error.empty());
    OA_CHECK(done.changed || withdrawn.changed);
    OA_CHECK(!fs::exists(launch_entry));
    OA_CHECK(!fs::exists(launch_icon));
    OA_CHECK(read_file(package) == file_types::mime_package(without_launch));
    OA_CHECK(read_file(package).find("application/x-oalaunch") == std::string::npos);
    OA_CHECK(read_file(package).find("application/x-oareg") != std::string::npos);
    for (const fs::path& file : others)
        OA_CHECK(fs::exists(file) && read_file(file) == before[file]);
    OA_CHECK(lines_leave_the_others(done) && lines_leave_the_others(withdrawn));

    // OK with it on again: the launch file's entry, icon and part of the package come back.
    const file_types::TypeSet with_launch = settings::file_type_set(opened);
    done = file_types::register_xdg(executable, icon, places, with_launch);
    OA_CHECK(done.supported && done.changed && done.error.empty());
    withdrawn = file_types::withdraw_xdg(with_launch.complement(), places);
    OA_CHECK(withdrawn.supported && !withdrawn.changed && withdrawn.error.empty());
    OA_CHECK(read_file(launch_entry) == file_types::launch_desktop_entry(executable));
    OA_CHECK(fs::exists(launch_icon));
    OA_CHECK(read_file(package) == file_types::mime_package(with_launch));
    for (const fs::path& file : others)
        OA_CHECK(fs::exists(file) && read_file(file) == before[file]);
    OA_CHECK(lines_leave_the_others(done) && lines_leave_the_others(withdrawn));

    // With the other six off, the launch file's switch alone keeps launch files registered.
    settings::EngineSettings launch_only{};
    launch_only.file_type_oamod = false;
    launch_only.file_type_oamap = false;
    launch_only.file_type_oalang = false;
    launch_only.file_type_oareg = false;
    launch_only.file_type_oamut = false;
    launch_only.file_type_oabrain = false;
    const file_types::TypeSet only = settings::file_type_set(launch_only);
    done = file_types::register_xdg(executable, icon, places, only);
    OA_CHECK(done.error.empty());
    withdrawn = file_types::withdraw_xdg(only.complement(), places);
    OA_CHECK(withdrawn.error.empty());
    OA_CHECK(read_file(launch_entry) == file_types::launch_desktop_entry(executable));
    OA_CHECK(read_file(package) == file_types::mime_package(only));
    OA_CHECK(!fs::exists(others[0]));

    std::error_code error;
    fs::remove_all(root, error);
}
#else
/// Reports whether a log line names launch files, and no other type.
///
/// @param line the line
/// @return true when the line is about the launch file's type
bool names_launch_files(const std::string& line) {
    return line.find("oalaunch") != std::string::npos ||
           line.find("OpenAnnihilation.Launch") != std::string::npos;
}

/// Reads a string value under HKEY_CURRENT_USER.
std::optional<std::wstring> read_value(const std::wstring& key_name, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key_name.c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return std::nullopt;
    DWORD type = 0;
    DWORD size = 0;
    std::optional<std::wstring> value;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        std::vector<wchar_t> text(size / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(
                key, name, nullptr, &type, reinterpret_cast<BYTE*>(text.data()), &size
            ) == ERROR_SUCCESS)
            value = std::wstring(text.data());
    }
    RegCloseKey(key);
    return value;
}

/// Reports whether a key is there.
bool key_exists(const std::wstring& key_name) {
    HKEY opened = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key_name.c_str(), 0, KEY_QUERY_VALUE, &opened) !=
        ERROR_SUCCESS)
        return false;
    RegCloseKey(opened);
    return true;
}

/// Reports whether the six types other than launch files keep their six values each, as a
/// registration of the seven wrote them.
bool other_install_types_registered(const std::wstring& classes, const fs::path& executable) {
    bool kept = true;
    for (const file_types::FileType& type : file_types::file_types) {
        if (type.extension == file_types::launch_extension)
            continue;
        const std::wstring extension(type.extension.begin(), type.extension.end());
        const std::wstring program_id(type.program_id.begin(), type.program_id.end());
        const std::wstring mime(type.mime_type.begin(), type.mime_type.end());
        const std::wstring command = L"\"" + executable.wstring() + L"\" --open \"%1\"";
        kept = kept && read_value(classes + L"\\." + extension, nullptr) == program_id;
        kept = kept && read_value(classes + L"\\." + extension, L"Content Type") == mime;
        kept = kept && read_value(
                           classes + L"\\." + extension + L"\\OpenWithProgids", program_id.c_str()
                       ) == L"";
        kept = kept && key_exists(classes + L"\\" + program_id);
        kept = kept && read_value(classes + L"\\" + program_id + L"\\DefaultIcon", nullptr) ==
                           executable.wstring() + L",0";
        kept =
            kept && read_value(classes + L"\\" + program_id + L"\\shell\\open\\command", nullptr) ==
                        command;
    }
    return kept;
}

/// The launch file's switch off, then on again, in the registry, as OK does on Windows:
/// register the types left on, then withdraw the rest. The launch file's program identifier and
/// its extension's values go and come back; the other six's values are never written or
/// removed.
void the_launch_switch_registers_and_withdraws_launch_files_alone_in_the_registry() {
    const std::wstring parent = L"Software\\OpenAnnihilationSettingsFileTypesTest";
    const std::wstring scratch = parent + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                 std::to_wstring(GetTickCount());
    const std::wstring classes = scratch + L"\\Classes";
    file_types::Places places{};
    places.classes_key = fs::path(classes).string();
    // The computer's classes stand in as an empty scratch key, as on a computer with no
    // installed copy, whatever this one holds.
    places.machine_classes_root = file_types::RegistryRoot::current_user;
    places.machine_classes_key = fs::path(scratch + L"\\Machine").string();
    places.notify_shell = false;
    const fs::path executable(L"C:\\Games\\Open Annihilation\\open-annihilation.exe");

    // A start with every switch on registers the seven.
    const settings::EngineSettings opened{};
    file_types::Registration done =
        file_types::register_windows(executable, places, settings::file_type_set(opened));
    OA_CHECK(done.supported && done.changed && done.error.empty());
    OA_CHECK(read_value(classes + L"\\.oalaunch", nullptr) == L"OpenAnnihilation.Launch");
    OA_CHECK(other_install_types_registered(classes, executable));

    // OK with the launch file's switch off: the registration of the six writes nothing, and
    // the withdrawal removes the launch file's values alone.
    settings::EngineSettings chosen = opened;
    chosen.file_type_oalaunch = false;
    const file_types::TypeSet without_launch = settings::file_type_set(chosen);
    done = file_types::register_windows(executable, places, without_launch);
    OA_CHECK(done.supported && !done.changed && done.error.empty() && done.lines.empty());
    file_types::Registration withdrawn =
        file_types::withdraw_windows(without_launch.complement(), places);
    OA_CHECK(withdrawn.supported && withdrawn.changed && withdrawn.error.empty());
    for (const std::string& line : withdrawn.lines)
        OA_CHECK(names_launch_files(line));
    OA_CHECK(!key_exists(classes + L"\\OpenAnnihilation.Launch"));
    OA_CHECK(!read_value(classes + L"\\.oalaunch", nullptr));
    OA_CHECK(!read_value(classes + L"\\.oalaunch", L"Content Type"));
    OA_CHECK(!read_value(classes + L"\\.oalaunch\\OpenWithProgids", L"OpenAnnihilation.Launch"));
    OA_CHECK(other_install_types_registered(classes, executable));

    // OK with it on again: the six values of launch files alone are written.
    const file_types::TypeSet with_launch = settings::file_type_set(opened);
    done = file_types::register_windows(executable, places, with_launch);
    OA_CHECK(done.supported && done.changed && done.error.empty());
    OA_CHECK(done.lines.size() == 6);
    for (const std::string& line : done.lines)
        OA_CHECK(names_launch_files(line));
    withdrawn = file_types::withdraw_windows(with_launch.complement(), places);
    OA_CHECK(withdrawn.supported && !withdrawn.changed && withdrawn.error.empty());
    OA_CHECK(read_value(classes + L"\\.oalaunch", nullptr) == L"OpenAnnihilation.Launch");
    OA_CHECK(
        read_value(classes + L"\\OpenAnnihilation.Launch\\shell\\open\\command", nullptr) ==
        L"\"" + executable.wstring() + L"\" -c \"%1\""
    );
    OA_CHECK(other_install_types_registered(classes, executable));

    OA_CHECK(SHDeleteKeyW(HKEY_CURRENT_USER, scratch.c_str()) == ERROR_SUCCESS);
    // The parent goes too once no other run's key is under it.
    static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, parent.c_str()));
}

/// Writes a string value, making its key.
void write_sz(const std::wstring& key_name, const wchar_t* name, const std::wstring& data) {
    HKEY opened = nullptr;
    OA_CHECK(
        RegCreateKeyExW(
            HKEY_CURRENT_USER,
            key_name.c_str(),
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &opened,
            nullptr
        ) == ERROR_SUCCESS
    );
    const auto size = static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t));
    OA_CHECK(
        RegSetValueExW(
            opened, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()), size
        ) == ERROR_SUCCESS
    );
    RegCloseKey(opened);
}

/// The program identifier Windows opens an extension with while the player has made no choice
/// in Windows: the user's own (Default) where there is one, else the computer's. An empty one
/// names no program.
std::optional<std::wstring> program_id_opening(
    const std::wstring& classes, const std::wstring& machine, const std::wstring& extension
) {
    if (std::optional<std::wstring> own = read_value(classes + L"\\" + extension, nullptr))
        return own;
    return read_value(machine + L"\\" + extension, nullptr);
}

/// The launch file's switch on a computer where the installer associated launch files with the
/// game for every user, as OK does on Windows: Off withdraws this user's registration and hides
/// that association from this user alone, so launch files no longer open the game; On puts this
/// user's registration back over it. The computer's key, and the other six types, are never
/// written.
void the_launch_switch_hides_the_installers_association_for_this_user() {
    const std::wstring parent = L"Software\\OpenAnnihilationSettingsFileTypesTest";
    const std::wstring scratch = parent + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                 std::to_wstring(GetTickCount()) + L"-installed";
    const std::wstring classes = scratch + L"\\Classes";
    const std::wstring machine = scratch + L"\\Machine";
    file_types::Places places{};
    places.classes_key = fs::path(classes).string();
    places.machine_classes_root = file_types::RegistryRoot::current_user;
    places.machine_classes_key = fs::path(machine).string();
    places.notify_shell = false;
    const fs::path executable(L"C:\\Program Files\\Open Annihilation\\open-annihilation.exe");
    const std::wstring command = L"\"" + executable.wstring() + L"\" -c \"%1\"";
    // The installer's association for every user.
    write_sz(machine + L"\\.oalaunch", nullptr, L"OpenAnnihilation.Launch");
    write_sz(machine + L"\\.oalaunch", L"Content Type", L"application/x-oalaunch");
    write_sz(machine + L"\\.oalaunch\\OpenWithProgids", L"OpenAnnihilation.Launch", L"");
    write_sz(machine + L"\\OpenAnnihilation.Launch\\shell\\open\\command", nullptr, command);

    // A start with every switch on registers the seven for this user.
    const settings::EngineSettings opened{};
    file_types::Registration done =
        file_types::register_windows(executable, places, settings::file_type_set(opened));
    OA_CHECK(done.changed && done.error.empty());
    OA_CHECK(other_install_types_registered(classes, executable));

    // OK with the launch file's switch off.
    settings::EngineSettings chosen = opened;
    chosen.file_type_oalaunch = false;
    const file_types::TypeSet without_launch = settings::file_type_set(chosen);
    done = file_types::register_windows(executable, places, without_launch);
    OA_CHECK(!done.changed && done.error.empty());
    file_types::Registration withdrawn =
        file_types::withdraw_windows(without_launch.complement(), places);
    OA_CHECK(withdrawn.changed && withdrawn.error.empty());
    for (const std::string& line : withdrawn.lines)
        OA_CHECK(names_launch_files(line));
    OA_CHECK(program_id_opening(classes, machine, L".oalaunch") == L"");
    OA_CHECK(!key_exists(classes + L"\\OpenAnnihilation.Launch"));
    OA_CHECK(other_install_types_registered(classes, executable));
    OA_CHECK(read_value(machine + L"\\.oalaunch", nullptr) == L"OpenAnnihilation.Launch");
    OA_CHECK(
        read_value(machine + L"\\OpenAnnihilation.Launch\\shell\\open\\command", nullptr) == command
    );

    // OK with it on again: this user's six values, and launch files open the game again.
    const file_types::TypeSet with_launch = settings::file_type_set(opened);
    done = file_types::register_windows(executable, places, with_launch);
    OA_CHECK(done.changed && done.error.empty() && done.lines.size() == 6);
    withdrawn = file_types::withdraw_windows(with_launch.complement(), places);
    OA_CHECK(!withdrawn.changed && withdrawn.error.empty());
    OA_CHECK(program_id_opening(classes, machine, L".oalaunch") == L"OpenAnnihilation.Launch");
    OA_CHECK(
        read_value(classes + L"\\OpenAnnihilation.Launch\\shell\\open\\command", nullptr) == command
    );
    OA_CHECK(other_install_types_registered(classes, executable));
    OA_CHECK(read_value(machine + L"\\.oalaunch", nullptr) == L"OpenAnnihilation.Launch");

    OA_CHECK(SHDeleteKeyW(HKEY_CURRENT_USER, scratch.c_str()) == ERROR_SUCCESS);
    static_cast<void>(RegDeleteKeyW(HKEY_CURRENT_USER, parent.c_str()));
}
#endif

/// A stand-in for the system's default-opener questions: each content type has one opener, and
/// every change is recorded.
struct FakeOpeners {
    std::map<std::string, file_types::Handler> handlers{};
    std::vector<std::pair<std::string, std::string>> writes{}; ///< content type, opener
};

file_types::Handler fake_read(void* context, std::string_view content_type, std::string&) {
    const auto* fake = static_cast<const FakeOpeners*>(context);
    const auto found = fake->handlers.find(std::string(content_type));
    return found == fake->handlers.end() ? file_types::Handler{} : found->second;
}

bool fake_write(
    void* context, std::string_view content_type, const file_types::Handler& handler, std::string&
) {
    auto* fake = static_cast<FakeOpeners*>(context);
    fake->writes.emplace_back(std::string(content_type), handler.bundle_id);
    fake->handlers[std::string(content_type)] = handler;
    return true;
}

/// The launch file's switch on macOS: off asks for the opener of plain text on launch files
/// alone, and on asks for this copy on launch files alone; the switches then show what the
/// system reports, and a launch file the system kept with this copy is named in the hint.
void the_launch_switch_changes_only_the_launch_files_opener() {
    file_types::Handler application;
    application.bundle_id = "net.coreprime.open-annihilation";
    file_types::Handler editor;
    editor.bundle_id = "com.example.editor";
    file_types::Handler archive;
    archive.bundle_id = "com.example.archive";
    FakeOpeners fake;
    fake.handlers["public.plain-text"] = editor;
    fake.handlers["public.zip-archive"] = archive;
    for (const file_types::ExportedContentType& type : file_types::exported_content_types)
        fake.handlers[std::string(type.content_type)] = application;
    file_types::HandlerCalls calls;
    calls.context = &fake;
    calls.default_handler = &fake_read;
    calls.set_handler = &fake_write;
    const std::string launch_type = "net.coreprime.open-annihilation.oalaunch";

    const settings::EngineSettings opened{};
    settings::EngineSettings chosen = opened;
    chosen.file_type_oalaunch = false;
    const std::vector<file_types::OpenerChange> off =
        settings::moved_file_type_switches(opened, chosen);
    file_types::Registration done = file_types::apply_opener_changes(calls, application, off);
    OA_CHECK(done.supported && done.changed && done.error.empty());
    OA_CHECK(fake.writes.size() == 1);
    OA_CHECK(
        !fake.writes.empty() && fake.writes[0].first == launch_type &&
        fake.writes[0].second == editor.bundle_id
    );
    for (const file_types::ExportedContentType& type : file_types::exported_content_types)
        if (type.extension != file_types::launch_extension)
            OA_CHECK(
                fake.handlers[std::string(type.content_type)].bundle_id == application.bundle_id
            );

    // The dialog opened again shows what the system reports: the launch file's switch off, the
    // others on.
    const file_types::OpenerReport report =
        file_types::reported_openers(calls, application.bundle_id, settings::file_type_set(opened));
    OA_CHECK(report.error.empty());
    settings::EngineSettings shown{};
    settings::set_file_type_switches(shown, report.open);
    OA_CHECK(shown == chosen);
    OA_CHECK(
        file_types::extensions_still_opened(settings::file_type_set(chosen), report.open).empty()
    );

    // On again: this copy opens launch files, and nothing else is asked.
    fake.writes.clear();
    const std::vector<file_types::OpenerChange> on =
        settings::moved_file_type_switches(chosen, opened);
    done = file_types::apply_opener_changes(calls, application, on);
    OA_CHECK(done.changed && done.error.empty());
    OA_CHECK(
        fake.writes.size() == 1 && fake.writes[0].first == launch_type &&
        fake.writes[0].second == application.bundle_id
    );

    // A system that keeps this copy as the opener of launch files after Off is named in the
    // hint's list.
    const file_types::TypeSet kept = settings::file_type_set(opened);
    OA_CHECK(
        file_types::extensions_still_opened(settings::file_type_set(chosen), kept) ==
        std::vector<std::string>{".oalaunch"}
    );
}

} // namespace

int main() {
    every_switch_starts_on();
    each_switch_stands_for_its_own_type();
#ifndef _WIN32
    the_launch_switch_registers_and_withdraws_launch_files_alone_in_xdg();
#else
    the_launch_switch_registers_and_withdraws_launch_files_alone_in_the_registry();
    the_launch_switch_hides_the_installers_association_for_this_user();
#endif
    the_launch_switch_changes_only_the_launch_files_opener();
    return oa::test::check_exit_status();
}
