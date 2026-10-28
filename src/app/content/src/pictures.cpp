// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The catalogue pictures' cache: the requests the main thread makes, and the
// worker's reads, fetches, checks and writes.
#include "oa/app/content/pictures.hpp"

#include "oa/app/mod_summary.hpp"
#include "oa/base/sha256.hpp"
#include "oa/base/threads.hpp"
#include "oa/netgame/http/client.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app::content {
namespace {

/// The ending of a picture's name, after its SHA-256.
constexpr std::string_view picture_suffix = ".png";

/// One picture asked for: the registry whose catalogue names it, and its address.
using PictureKey = std::pair<std::string, std::string>;

/// Where a picture asked for stands.
enum class PictureState : uint8_t {
    waiting,  ///< asked for; not read or fetched yet
    fetching, ///< the worker is reading or fetching it
    ready,    ///< decoded and checked; take hands it over
};

/// One picture asked for and not taken.
struct PictureEntry {
    PictureState state{PictureState::waiting}; ///< where it stands
    ModBadge badge{};                          ///< the decoded picture, once ready
    std::vector<uint8_t> bytes{};              ///< the checked file, once ready
};

/// One picture in the order it was asked for.
struct PictureRecord {
    PictureKey key{};     ///< the registry and the address
    PictureEntry entry{}; ///< where it stands, and the picture once ready
};

/// What reading or fetching one picture came to.
enum class Obtained : uint8_t {
    ready,     ///< checked and decoded
    refused,   ///< its name, its bytes, its size or the server refuse it
    held,      ///< a match is running, and the folder does not hold it
    cancelled, ///< the cancel flag stopped the fetch
};

/// Reads a whole file of at most max_picture_bytes.
///
/// @param file the file
/// @param[out] bytes its bytes
/// @return false when it is missing, larger or cannot be read
bool read_small_file(const std::filesystem::path& file, std::vector<uint8_t>& bytes) {
    std::error_code error;
    const uintmax_t size = std::filesystem::file_size(file, error);
    if (error || size > max_picture_bytes)
        return false;
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    bytes.assign(static_cast<std::size_t>(size), 0);
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<std::size_t>(in.gcount()) == bytes.size();
}

/// Decodes a picture whose bytes hash to the SHA-256 its name gives.
///
/// @param bytes the file's bytes
/// @param digest the SHA-256 its name gives
/// @param[out] badge the decoded picture
/// @return true when the bytes are that picture, no larger than
///     max_picture_bytes, and decode
bool checked_badge(
    std::span<const uint8_t> bytes, const base::sha256::Digest& digest, ModBadge& badge
) {
    if (bytes.empty() || bytes.size() > max_picture_bytes)
        return false;
    if (base::sha256::digest_of(bytes) != digest)
        return false;
    badge = read_mod_badge(bytes);
    return badge.width > 0 && badge.height > 0 && !badge.pixels.empty();
}

/// Returns the picture asked for under a key, or the end.
///
/// @param entries the pictures asked for and not taken
/// @param key the registry and the address
/// @return the picture; the end when it was not asked for
std::vector<PictureRecord>::iterator
find_record(std::vector<PictureRecord>& entries, const PictureKey& key) {
    return std::find_if(entries.begin(), entries.end(), [&](const PictureRecord& record) {
        return record.key == key;
    });
}

/// Returns the picture asked for under a key, or the end.
///
/// @param entries the pictures asked for and not taken
/// @param key the registry and the address
/// @return the picture; the end when it was not asked for
std::vector<PictureRecord>::const_iterator
find_record(const std::vector<PictureRecord>& entries, const PictureKey& key) {
    return std::find_if(entries.begin(), entries.end(), [&](const PictureRecord& record) {
        return record.key == key;
    });
}

} // namespace

std::optional<base::sha256::Digest> picture_digest(std::string_view url) noexcept {
    const std::size_t slash = url.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? url : url.substr(slash + 1);
    if (name.size() != base::sha256::hex_size + picture_suffix.size() ||
        name.substr(base::sha256::hex_size) != picture_suffix)
        return std::nullopt;
    return base::sha256::parse_hex(name.substr(0, base::sha256::hex_size));
}

struct PictureCache::State {
    std::filesystem::path folder{};       ///< where the pictures are kept; empty for none
    const std::atomic<bool>* cancel{};    ///< set when the worker stops; may be null
    mutable base::threads::Mutex mutex{}; ///< guards entries, refused and generation
    /// The pictures asked for and not taken, in the order they are fetched.
    std::vector<PictureRecord> entries{};
    std::set<PictureKey> refused{};      ///< the pictures never asked for again
    std::atomic<uint64_t> generation{0}; ///< moves on as a picture becomes ready

    /// Reads or fetches one picture.
    ///
    /// @param url its address
    /// @param http the worker's client
    /// @param match_running a match is running
    /// @param[out] badge the decoded picture, when ready
    /// @param[out] bytes the checked file, when ready
    /// @return what it came to
    Obtained obtain(
        const std::string& url,
        netgame::http::Client& http,
        bool match_running,
        ModBadge& badge,
        std::vector<uint8_t>& bytes
    ) const {
        const std::optional<base::sha256::Digest> digest = picture_digest(url);
        if (!digest)
            return Obtained::refused;
        const std::array<char, base::sha256::hex_size> hex = base::sha256::to_hex(*digest);
        const std::string name = std::string(hex.data(), hex.size()) + std::string(picture_suffix);
        if (!folder.empty() && read_small_file(folder / name, bytes) &&
            checked_badge(bytes, *digest, badge))
            return Obtained::ready;
        if (match_running)
            return Obtained::held;
        netgame::http::Request request;
        request.url = url;
        request.limits.max_body_bytes = max_picture_bytes;
        request.cancel = cancel;
        const netgame::http::Response response = http.fetch(request, bytes);
        if (response.failure == netgame::http::Failure::cancelled)
            return Obtained::cancelled;
        if (response.failure != netgame::http::Failure::none || response.status != 200)
            return Obtained::refused;
        if (!checked_badge(bytes, *digest, badge)) {
            bytes.clear();
            return Obtained::refused;
        }
        // A picture the folder cannot keep is still shown this time.
        if (!folder.empty())
            std::ignore = oa::platform::replace_file(folder / name, bytes, nullptr);
        return Obtained::ready;
    }
};

PictureCache::PictureCache(std::filesystem::path folder, const std::atomic<bool>* cancel)
    : state_(std::make_unique<State>()) {
    state_->folder = std::move(folder);
    state_->cancel = cancel;
}

PictureCache::~PictureCache() = default;

void PictureCache::request(std::string_view registry, std::string_view url) {
    PictureKey key{std::string(registry), std::string(url)};
    base::threads::LockGuard guard(state_->mutex);
    if (state_->refused.count(key) != 0 ||
        find_record(state_->entries, key) != state_->entries.end())
        return;
    state_->entries.push_back(PictureRecord{std::move(key), {}});
}

void PictureCache::prefer(std::string_view registry, std::string_view url) {
    PictureKey key{std::string(registry), std::string(url)};
    base::threads::LockGuard guard(state_->mutex);
    if (state_->refused.count(key) != 0)
        return;
    const auto found = find_record(state_->entries, key);
    if (found == state_->entries.end()) {
        state_->entries.insert(state_->entries.begin(), PictureRecord{std::move(key), {}});
        return;
    }
    if (found->entry.state != PictureState::waiting || found == state_->entries.begin())
        return;
    PictureRecord record = std::move(*found);
    state_->entries.erase(found);
    state_->entries.insert(state_->entries.begin(), std::move(record));
}

std::optional<ModBadge> PictureCache::take(std::string_view registry, std::string_view url) {
    const PictureKey key{std::string(registry), std::string(url)};
    base::threads::LockGuard guard(state_->mutex);
    const auto found = find_record(state_->entries, key);
    if (found == state_->entries.end() || found->entry.state != PictureState::ready)
        return std::nullopt;
    ModBadge badge = std::move(found->entry.badge);
    state_->entries.erase(found);
    return badge;
}

std::optional<std::vector<uint8_t>>
PictureCache::take_bytes(std::string_view registry, std::string_view url) {
    const PictureKey key{std::string(registry), std::string(url)};
    base::threads::LockGuard guard(state_->mutex);
    const auto found = find_record(state_->entries, key);
    if (found == state_->entries.end() || found->entry.state != PictureState::ready)
        return std::nullopt;
    std::vector<uint8_t> bytes = std::move(found->entry.bytes);
    state_->entries.erase(found);
    return bytes;
}

bool PictureCache::refused(std::string_view registry, std::string_view url) const {
    const PictureKey key{std::string(registry), std::string(url)};
    base::threads::LockGuard guard(state_->mutex);
    return state_->refused.count(key) != 0;
}

bool PictureCache::asked(std::string_view registry, std::string_view url) const {
    const PictureKey key{std::string(registry), std::string(url)};
    base::threads::LockGuard guard(state_->mutex);
    if (state_->refused.count(key) != 0)
        return true;
    const std::vector<PictureRecord>& entries = state_->entries;
    return find_record(entries, key) != entries.end();
}

void PictureCache::drop_all() {
    base::threads::LockGuard guard(state_->mutex);
    state_->entries.clear();
}

bool PictureCache::waiting() const {
    base::threads::LockGuard guard(state_->mutex);
    for (const PictureRecord& record : state_->entries)
        if (record.entry.state == PictureState::waiting)
            return true;
    return false;
}

bool PictureCache::holding() const {
    base::threads::LockGuard guard(state_->mutex);
    return !state_->entries.empty();
}

uint64_t PictureCache::generation() const noexcept {
    return state_->generation.load();
}

void PictureCache::fetch_waiting(netgame::http::Client& http, bool match_running) {
    for (;;) {
        if (state_->cancel != nullptr && state_->cancel->load())
            return;
        PictureKey key;
        {
            base::threads::LockGuard guard(state_->mutex);
            auto next = state_->entries.begin();
            while (next != state_->entries.end() && next->entry.state != PictureState::waiting)
                ++next;
            if (next == state_->entries.end())
                return;
            next->entry.state = PictureState::fetching;
            key = next->key;
        }
        ModBadge badge;
        std::vector<uint8_t> bytes;
        const Obtained obtained = state_->obtain(key.second, http, match_running, badge, bytes);
        base::threads::LockGuard guard(state_->mutex);
        // drop_all may have forgotten the picture meanwhile; what was
        // fetched stays in the folder.
        const auto found = find_record(state_->entries, key);
        switch (obtained) {
        case Obtained::ready:
            if (found != state_->entries.end()) {
                found->entry.state = PictureState::ready;
                found->entry.badge = std::move(badge);
                found->entry.bytes = std::move(bytes);
                state_->generation.fetch_add(1);
            }
            break;
        case Obtained::refused:
            state_->refused.insert(key);
            if (found != state_->entries.end())
                state_->entries.erase(found);
            break;
        case Obtained::held:
        case Obtained::cancelled:
            if (found != state_->entries.end() && found->entry.state == PictureState::fetching)
                found->entry.state = PictureState::waiting;
            return;
        }
    }
}

} // namespace oa::app::content
