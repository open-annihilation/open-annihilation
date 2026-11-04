// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Where Rover finds and creates its files. The game opens ./boneyards/...
// and creates http.log relative to its folder; oa-game runs from anywhere.
// A relative path is read from the player's folder when a copy is there,
// then from the game's files when a reader is set, then from the expansion
// folder when the file is there, and otherwise from the game folder,
// matching names without regard to case. boneyards/server.byd is the
// exception, in any spelling: only the player's copy is read. The
// expansion's copy and the game folder's copy are the 1998 list, so they
// are absent and Rover uses the list baked into the build. A relative path is
// created only under the player's folder, with http.log in its Boneyards
// directory. A relative name whose first folder is boneyards in any case is
// that directory: the player's folder spells it Boneyards. A boneyards
// directory left by an earlier build is read while Boneyards is absent, and
// the first write renames it to Boneyards where the file system has two
// names. The expansion and the game folder keep the spelling they have.
// The saved server list named at configuration is what this run
// reads; a rewrite of it still goes to the player's folder. Nothing is
// written into the game folder, including while the player's folder is not
// chosen yet.
#include "oa/boneyards/rover_session.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/platform/files.hpp"

#include "ad_server.h"
#include "oa/rover/platform.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace oa::boneyards {
namespace {

namespace fs = std::filesystem;

// The saved server list Rover reads first, under boneyards/.
constexpr const char* kSavedServerList = "boneyards/serv_sav.byd";
// The list Rover reads next. Only the player's copy is opened.
constexpr const char* kRoverServerListFile = "boneyards/server.byd";
constexpr const char* kServerListExtension = ".byd";
// The fetch worker's log. It is created as http.log and kept under boneyards/.
constexpr const char* kFetchLogFile = "http.log";
constexpr const char* kFetchLogStored = "boneyards/http.log";
// The player's own copy of that directory. The names above stay as the
// client spells them; only the folder inside the player's folder uses this.
constexpr const char* kPlayerBoneyardsFolder = "Boneyards";
// Room for a folder path the query copies, including the terminating NUL.
constexpr std::size_t kPlayerFolderCapacity = 4096;

// Rover dials every yard master on this port.
constexpr uint16_t kYardMasterPort = 9110;
// Longest host a server list entry holds (its name field is 0xff bytes).
constexpr std::size_t kHostCapacity = 0xfe;
// A host list's line: the bare host (a domain of one space), a cluster size
// of 0, the location and the selected flag.
constexpr const char* kNoDomainField = " ";
constexpr const char* kHostClusterSizeField = "0";
constexpr const char* kSelectedFlags = "D";
constexpr const char* kHostListDirectory = "oa-boneyards";

std::string g_install_dir;
std::string g_expansion_folder;
bool g_expansion_explicit = false;
PlayerFolderQuery g_expansion_query = nullptr;
void* g_expansion_query_context = nullptr;
std::string g_server_list;
std::string g_host_list; // the list rover_session_server_list last wrote
std::string g_player_folder;
bool g_player_folder_explicit = false;
PlayerFolderQuery g_player_folder_query = nullptr;
void* g_player_folder_query_context = nullptr;
VirtualFileRead g_virtual_read = nullptr;
void* g_virtual_context = nullptr;
RoverPlatform g_placed_platform{};
bool g_configured = false;

// A test's ad server. While set, it hides the host Rover stored, even when its send_id suppresses
// the fetch.
struct AdServerOverride {
    bool set = false;
    int32_t send_id = 0;
    char url[0x104]{};
    char params[0x104]{};
};

AdServerOverride g_ad_server{};

void copy_field(char* out, std::size_t capacity, const char* text) {
    if (out == nullptr || capacity == 0)
        return;
    std::size_t count = 0;
    if (text != nullptr) {
        while (text[count] != '\0' && count + 1 < capacity) {
            out[count] = text[count];
            ++count;
        }
    }
    out[count] = '\0';
}

// One open file. A disk file keeps the native handle; a file taken from the
// game's files keeps its bytes. The placed platform's read, write and close
// all see this, so a native handle is never handed out as a RoverFile.
struct OpenedFile {
    RoverFile* stream = nullptr;
    std::vector<uint8_t> bytes;
    std::size_t cursor = 0;
};

char lower_ascii(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower_ascii(a[i]) != lower_ascii(b[i]))
            return false;
    return true;
}

bool is_relative(const char* path) {
    if (path[0] == '/' || path[0] == '\\')
        return false;
    return !(path[0] != '\0' && path[1] == ':');
}

// The path with '/' separators and without leading "./" components.
std::string relative_form(const char* path) {
    std::string out(path);
    for (char& c : out)
        if (c == '\\')
            c = '/';
    while (out.rfind("./", 0) == 0)
        out.erase(0, 2);
    return out;
}

// Follows each component under `base` by listing the folder. A component
// that is not there yet keeps the requested spelling, so a file being
// created is named as the caller asked. An existing entry is taken from
// the listing, so the path uses the spelling on disk. `exists` of the
// requested spelling is not used: on a case-insensitive volume that
// succeeds for another spelling and hides the name the disk stored.
fs::path listed_place(const fs::path& base, const std::string& relative) {
    fs::path current = base;
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t end = relative.find('/', start);
        const std::string part =
            relative.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = end == std::string::npos ? relative.size() + 1 : end + 1;
        if (part.empty() || part == ".")
            continue;
        std::error_code error;
        fs::path next;
        bool matched = false;
        if (fs::is_directory(current, error)) {
            for (fs::directory_iterator it(current, error), last; !error && it != last;
                 it.increment(error)) {
                if (same_name(it->path().filename().string(), part)) {
                    next = it->path();
                    matched = true;
                    break;
                }
            }
        }
        if (!matched)
            next = current / part;
        current = next;
    }
    return current;
}

// The host path of `relative` under `base`. An existing file is resolved by
// the engine's loose-file lookup, which ignores ASCII case and returns the
// spelling on disk. A file that is not there yet is named by listing each
// folder on the way, so a created file still has a path.
fs::path place(const fs::path& base, const std::string& relative) {
    if (!base.empty() && !relative.empty()) {
        try {
            const oa::AssetStore store(base);
            if (const std::optional<fs::path> found = store.loose_file(relative))
                return *found;
        } catch (const std::exception&) {
            // An unusable path is named by the listing below.
        }
    }
    return listed_place(base, relative);
}

bool is_host_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '-' || c == '_';
}

// A missing file named as one: it has a directory or the list extension.
bool names_file(const std::string& server) {
    if (server.find_first_of("/\\") != std::string::npos)
        return true;
    const std::string extension = kServerListExtension;
    return server.size() > extension.size() &&
           same_name(server.substr(server.size() - extension.size()), extension);
}

// The folder named for this run: the explicit one, else what the query
// reports, else none.
std::string player_folder_now() {
    if (g_player_folder_explicit)
        return g_player_folder;
    if (g_player_folder_query == nullptr)
        return {};
    char buffer[kPlayerFolderCapacity];
    buffer[0] = '\0';
    g_player_folder_query(g_player_folder_query_context, buffer, sizeof buffer);
    return buffer;
}

// The expansion folder named for this run: the explicit one, else what the
// query reports, else none.
std::string expansion_folder_now() {
    if (g_expansion_explicit)
        return g_expansion_folder;
    if (g_expansion_query == nullptr)
        return {};
    char buffer[kPlayerFolderCapacity];
    buffer[0] = '\0';
    g_expansion_query(g_expansion_query_context, buffer, sizeof buffer);
    return buffer;
}

// `relative` under `base` when that names a regular file. False otherwise,
// and `out` is left unchanged.
bool regular_under(const std::string& base, const std::string& relative, std::string* out) {
    if (base.empty())
        return false;
    const fs::path placed = place(oa::platform::path_from_utf8(base), relative);
    std::error_code error;
    if (!fs::is_regular_file(placed, error))
        return false;
    *out = oa::platform::path_to_utf8(placed);
    return true;
}

// http.log is kept under boneyards/; every other relative path stays as named.
std::string stored_relative(const std::string& relative) {
    if (same_name(relative, kFetchLogFile))
        return kFetchLogStored;
    return relative;
}

// The player's folder spells the first boneyards component as Boneyards.
// Every other relative path, and every later component, stays as named.
std::string player_folder_relative(const std::string& relative) {
    const std::size_t slash = relative.find('/');
    const std::string first = relative.substr(0, slash);
    if (!same_name(first, kPlayerBoneyardsFolder))
        return relative;
    std::string mapped(kPlayerBoneyardsFolder);
    if (slash != std::string::npos)
        mapped.append(relative, slash, std::string::npos);
    return mapped;
}

// The Boneyards directory under the player's folder. An exact Boneyards
// entry wins. Otherwise a boneyards entry of another spelling is that
// directory (an earlier build, or a volume that has one folder for both
// names). When neither is there, the path names Boneyards so a create
// uses that spelling.
fs::path player_boneyards_directory(const fs::path& base) {
    std::error_code error;
    fs::path exact;
    fs::path other;
    if (fs::is_directory(base, error) && !error) {
        for (fs::directory_iterator it(base, error), last; !error && it != last;
             it.increment(error)) {
            std::error_code kind;
            if (!it->is_directory(kind) || kind)
                continue;
            const std::string name = oa::platform::path_to_utf8(it->path().filename());
            if (name == kPlayerBoneyardsFolder)
                exact = it->path();
            else if (other.empty() && same_name(name, kPlayerBoneyardsFolder))
                other = it->path();
        }
    }
    if (!exact.empty())
        return exact;
    if (!other.empty())
        return other;
    return base / kPlayerBoneyardsFolder;
}

// Renames a lowercase boneyards directory to Boneyards the first time a
// file is written there. When the volume already has one folder for both
// names, or Boneyards is already present, the directories are left as they
// are. A rename that the file system refuses leaves the old directory, and
// the write then goes into it.
void adopt_player_boneyards() {
    const std::string folder = player_folder_now();
    if (folder.empty())
        return;
    const fs::path base = oa::platform::path_from_utf8(folder);
    std::error_code error;
    if (!fs::is_directory(base, error) || error)
        return;
    fs::path other;
    for (fs::directory_iterator it(base, error), last; !error && it != last; it.increment(error)) {
        std::error_code kind;
        if (!it->is_directory(kind) || kind)
            continue;
        const std::string name = oa::platform::path_to_utf8(it->path().filename());
        if (name == kPlayerBoneyardsFolder)
            return;
        if (other.empty() && same_name(name, kPlayerBoneyardsFolder))
            other = it->path();
    }
    if (other.empty())
        return;
    const fs::path target = base / kPlayerBoneyardsFolder;
    if (fs::exists(target, error))
        return;
    fs::rename(other, target, error);
}

// `relative` under the player's folder, or "" when that folder is not chosen.
// A missing last component is still named, so a created file has a path.
std::string under_player(const std::string& relative) {
    const std::string folder = player_folder_now();
    if (folder.empty())
        return {};
    const fs::path base = oa::platform::path_from_utf8(folder);
    const std::string mapped = player_folder_relative(relative);
    const std::size_t slash = mapped.find('/');
    const bool in_boneyards = slash == std::string::npos
                                  ? mapped == kPlayerBoneyardsFolder
                                  : mapped.compare(0, slash, kPlayerBoneyardsFolder) == 0;
    if (!in_boneyards)
        return oa::platform::path_to_utf8(place(base, mapped));
    const fs::path top = player_boneyards_directory(base);
    if (slash == std::string::npos)
        return oa::platform::path_to_utf8(top);
    return oa::platform::path_to_utf8(place(top, mapped.substr(slash + 1)));
}

// The native disk path a file is read from. The configured server list
// stands in for the saved one. Otherwise the player's copy is preferred,
// then the expansion folder when the file is there, and the game folder's
// path is named when neither has it. boneyards/server.byd names the
// player's copy or, when the player has none, a path that is not a file.
std::string read_path(const char* path) {
    if (!is_relative(path))
        return path;
    const std::string relative = relative_form(path);
    if (!g_server_list.empty() && same_name(relative, kSavedServerList))
        return g_server_list;
    const std::string in_player = under_player(stored_relative(relative));
    if (!in_player.empty()) {
        std::error_code error;
        if (fs::is_regular_file(oa::platform::path_from_utf8(in_player), error))
            return in_player;
    }
    if (same_name(relative, kRoverServerListFile))
        return in_player;
    std::string in_expansion;
    if (regular_under(expansion_folder_now(), relative, &in_expansion))
        return in_expansion;
    if (g_install_dir.empty())
        return path;
    return place(g_install_dir, relative).string();
}

// Where a file is created. An absolute path is the caller's. A relative path
// goes under the player's folder, or nowhere while that folder is not chosen.
std::string write_path(const char* path) {
    if (!is_relative(path))
        return path;
    const std::string relative = stored_relative(relative_form(path));
    const std::size_t slash = relative.find('/');
    if (same_name(relative.substr(0, slash), kPlayerBoneyardsFolder))
        adopt_player_boneyards();
    return under_player(relative);
}

// Wraps a native file. Null when it cannot be opened or the wrapper cannot
// be kept, and then a native handle that was opened is closed.
RoverFile* open_disk(const char* path) {
    if (path == nullptr || path[0] == '\0')
        return nullptr;
    RoverFile* native = rover_native_platform()->file_open(path);
    if (native == nullptr)
        return nullptr;
    try {
        auto* opened = new OpenedFile;
        opened->stream = native;
        return reinterpret_cast<RoverFile*>(opened);
    } catch (const std::bad_alloc&) {
        rover_native_platform()->file_close(native);
        return nullptr;
    }
}

// A file whose bytes are already in hand. Null when the wrapper cannot be kept.
RoverFile* open_memory(std::vector<uint8_t> bytes) {
    try {
        auto* opened = new OpenedFile;
        opened->bytes = std::move(bytes);
        return reinterpret_cast<RoverFile*>(opened);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

RoverFile* open_placed(const char* path) {
    if (path == nullptr)
        return nullptr;
    if (!is_relative(path))
        return open_disk(path);
    const std::string relative = relative_form(path);
    if (!g_server_list.empty() && same_name(relative, kSavedServerList))
        return open_disk(g_server_list.c_str());
    const std::string in_player = under_player(stored_relative(relative));
    if (!in_player.empty()) {
        std::error_code error;
        if (fs::is_regular_file(oa::platform::path_from_utf8(in_player), error))
            return open_disk(in_player.c_str());
    }
    if (same_name(relative, kRoverServerListFile))
        return nullptr;
    if (g_virtual_read != nullptr) {
        std::vector<uint8_t> bytes;
        bool found = false;
        try {
            found = g_virtual_read(g_virtual_context, relative.c_str(), &bytes);
        } catch (...) {
            found = false;
        }
        if (found)
            return open_memory(std::move(bytes));
    }
    std::string in_expansion;
    if (regular_under(expansion_folder_now(), relative, &in_expansion))
        return open_disk(in_expansion.c_str());
    if (g_install_dir.empty())
        return open_disk(path);
    return open_disk(place(g_install_dir, relative).string().c_str());
}

int32_t read_placed(RoverFile* file, void* buffer, int32_t capacity) {
    if (file == nullptr || capacity < 0)
        return -1;
    if (capacity == 0)
        return 0;
    if (buffer == nullptr)
        return -1;
    auto* opened = reinterpret_cast<OpenedFile*>(file);
    if (opened->stream != nullptr)
        return rover_native_platform()->file_read(opened->stream, buffer, capacity);
    if (opened->cursor >= opened->bytes.size())
        return 0;
    const std::size_t available = opened->bytes.size() - opened->cursor;
    const std::size_t count = std::min(available, static_cast<std::size_t>(capacity));
    std::memcpy(buffer, opened->bytes.data() + opened->cursor, count);
    opened->cursor += count;
    return static_cast<int32_t>(count);
}

int32_t write_placed(RoverFile* file, const void* data, int32_t length) {
    if (file == nullptr || length < 0)
        return -1;
    if (length > 0 && data == nullptr)
        return -1;
    auto* opened = reinterpret_cast<OpenedFile*>(file);
    if (opened->stream == nullptr)
        return -1;
    return rover_native_platform()->file_write(opened->stream, data, length);
}

void close_placed(RoverFile* file) {
    if (file == nullptr)
        return;
    auto* opened = reinterpret_cast<OpenedFile*>(file);
    if (opened->stream != nullptr)
        rover_native_platform()->file_close(opened->stream);
    delete opened;
}

RoverFile* create_placed(const char* path) {
    if (path == nullptr)
        return nullptr;
    const bool relative = is_relative(path);
    const std::string placed = relative ? write_path(path) : std::string(path);
    if (placed.empty())
        return nullptr;
    if (relative) {
        std::error_code error;
        const fs::path parent = oa::platform::path_from_utf8(placed).parent_path();
        if (!parent.empty())
            fs::create_directories(parent, error);
    }
    RoverFile* native = rover_native_platform()->file_create(placed.c_str());
    if (native == nullptr)
        return nullptr;
    try {
        auto* opened = new OpenedFile;
        opened->stream = native;
        return reinterpret_cast<RoverFile*>(opened);
    } catch (const std::bad_alloc&) {
        rover_native_platform()->file_close(native);
        return nullptr;
    }
}

} // namespace

void rover_session_configure(const char* install_dir, const char* server_list) {
    g_install_dir = install_dir != nullptr ? install_dir : "";
    g_server_list = server_list != nullptr ? server_list : "";
    g_player_folder.clear();
    g_player_folder_explicit = false;
    g_player_folder_query = nullptr;
    g_player_folder_query_context = nullptr;
    g_virtual_read = nullptr;
    g_virtual_context = nullptr;
    g_expansion_folder.clear();
    g_expansion_explicit = false;
    g_expansion_query = nullptr;
    g_expansion_query_context = nullptr;
    g_ad_server = {};
    g_placed_platform = *rover_native_platform();
    g_placed_platform.file_open = open_placed;
    g_placed_platform.file_create = create_placed;
    g_placed_platform.file_read = read_placed;
    g_placed_platform.file_write = write_placed;
    g_placed_platform.file_close = close_placed;
    rover_use_platform(&g_placed_platform);
    g_configured = true;
}

void rover_session_set_ad_server(const char* url, int32_t send_id, const char* params) {
    g_ad_server = {};
    g_ad_server.set = true;
    g_ad_server.send_id = send_id;
    copy_field(g_ad_server.url, sizeof g_ad_server.url, url);
    copy_field(g_ad_server.params, sizeof g_ad_server.params, params);
}

void rover_session_clear_ad_server() {
    g_ad_server = {};
}

bool rover_session_ad_server(
    char* url, std::size_t url_capacity, int32_t* send_id, char* params, std::size_t params_capacity
) {
    if (g_ad_server.set) {
        if (g_ad_server.send_id != 1 || g_ad_server.url[0] == '\0')
            return false;
        if (url == nullptr || url_capacity == 0)
            return false;
        std::size_t url_length = 0;
        while (g_ad_server.url[url_length] != '\0')
            ++url_length;
        if (url_length + 1 > url_capacity)
            return false;
        std::size_t params_length = 0;
        while (g_ad_server.params[params_length] != '\0')
            ++params_length;
        if (params != nullptr && params_length + 1 > params_capacity)
            return false;
        copy_field(url, url_capacity, g_ad_server.url);
        if (send_id != nullptr)
            *send_id = g_ad_server.send_id;
        if (params != nullptr)
            copy_field(params, params_capacity, g_ad_server.params);
        return true;
    }
    return rover_ad_server_copy(url, url_capacity, send_id, params, params_capacity);
}

void rover_session_set_virtual_files(VirtualFileRead read, void* context) {
    g_virtual_read = read;
    g_virtual_context = context;
}

void rover_session_set_expansion_folder(const char* folder) {
    g_expansion_folder = folder != nullptr ? folder : "";
    g_expansion_explicit = true;
}

void rover_session_set_expansion_folder_query(PlayerFolderQuery query, void* context) {
    g_expansion_query = query;
    g_expansion_query_context = context;
}

bool rover_session_read_file(const char* path, std::vector<uint8_t>* out) {
    if (path == nullptr || out == nullptr)
        return false;
    RoverFile* file = open_placed(path);
    if (file == nullptr)
        return false;
    try {
        std::vector<uint8_t> bytes;
        uint8_t chunk[4096];
        for (;;) {
            const int32_t read = read_placed(file, chunk, static_cast<int32_t>(sizeof chunk));
            if (read < 0) {
                close_placed(file);
                return false;
            }
            if (read == 0)
                break;
            bytes.insert(bytes.end(), chunk, chunk + static_cast<std::size_t>(read));
        }
        close_placed(file);
        *out = std::move(bytes);
        return true;
    } catch (const std::bad_alloc&) {
        close_placed(file);
        return false;
    }
}

void rover_session_set_player_folder(const char* folder) {
    g_player_folder = folder != nullptr ? folder : "";
    g_player_folder_explicit = true;
}

void rover_session_set_player_folder_query(PlayerFolderQuery query, void* context) {
    g_player_folder_query = query;
    g_player_folder_query_context = context;
}

std::string rover_session_placed_path(const char* path) {
    if (path == nullptr)
        return {};
    return read_path(path);
}

std::string rover_session_write_path(const char* path) {
    if (path == nullptr)
        return {};
    return write_path(path);
}

bool rover_session_configured() {
    return g_configured;
}

const RoverPlatform* rover_session_platform() {
    return g_configured ? &g_placed_platform : nullptr;
}

std::string rover_session_server_list(const std::string& server, std::string* error) {
    std::error_code status;
    if (fs::is_regular_file(server, status))
        return server;
    if (names_file(server)) {
        *error = "server list not found: " + server;
        return {};
    }
    std::string host = server;
    const std::size_t colon = host.find(':');
    if (colon != std::string::npos) {
        if (host.substr(colon + 1) != std::to_string(kYardMasterPort)) {
            *error = "Rover dials the yard master on port " + std::to_string(kYardMasterPort) +
                     " only: " + server;
            return {};
        }
        host.erase(colon);
    }
    if (host.empty() || host.size() > kHostCapacity ||
        !std::all_of(host.begin(), host.end(), is_host_char)) {
        *error = "neither a server list nor a host name: " + server;
        return {};
    }
    const fs::path directory = fs::temp_directory_path(status) / kHostListDirectory;
    fs::create_directories(directory, status);
    const fs::path path = directory / (host + kServerListExtension);
    std::ofstream list(path, std::ios::binary | std::ios::trunc);
    list << host << '\t' << kNoDomainField << '\t' << kHostClusterSizeField << '\t'
         << kHostServerLocation << '\t' << kSelectedFlags << "\r\n";
    list.close();
    if (!list) {
        *error = "cannot write the server list " + path.string();
        return {};
    }
    g_host_list = path.string();
    return g_host_list;
}

bool rover_session_uses_host_list() {
    return !g_host_list.empty() && g_server_list == g_host_list;
}

} // namespace oa::boneyards
