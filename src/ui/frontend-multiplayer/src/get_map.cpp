// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// GET MAP's state and line, decided from the host's record, the packs this
// machine has and what the registry offers.
#include "oa/ui/frontend_multiplayer/get_map.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace oa::ui::frontend_multiplayer {

namespace {

/// The button's caption.
constexpr std::string_view kCaption = "GET MAP";
/// What the button fetches: the pack, its version, its size and where from.
constexpr std::string_view kGetText = "{pack} {version} · {size} from {registry}";
/// Another release is installed.
constexpr std::string_view kOtherReleaseText =
    "You have release {mine} of {pack}; the host plays release {theirs}.";
/// The host's release is offered by no registry the player can use.
constexpr std::string_view kGoneText =
    "The host's release of {pack} is no longer offered by {registry}.";
/// The registry must be added first.
constexpr std::string_view kAddRegistryText = "Add {registry} to get {pack}";
/// Downloads from the registry are off.
constexpr std::string_view kDownloadsOffText =
    "Downloads from {registry} are off (Settings › Downloads).";
/// The download is on its way.
constexpr std::string_view kDownloadingText = "Downloading {pack} · {done} of {size}";
/// The last try failed.
constexpr std::string_view kFailedText = "Couldn't get {pack}: {reason}";
/// A release a pack from no catalogue has: none is known.
constexpr std::string_view kUnknownRelease = "?";
/// A size the catalogue has not given yet.
constexpr std::string_view kUnknownSize = "…";

/// Looks a text up in the interface's catalogue, for the Library's sizes.
///
/// @param english the text, in English
/// @return its translation, or the English
std::string_view interface_word(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// Returns a size in the Library's units, looked up in the interface's catalogue.
///
/// @param bytes the size; 0 is not known yet
/// @return the size, such as "41.0 MB"
std::string size_words(uint64_t bytes) {
    if (bytes == 0)
        return std::string(kUnknownSize);
    const oa::ui::library::TextHooks hooks{nullptr, interface_word};
    return oa::ui::library::size_text(bytes, hooks);
}

/// One place of a text and its value.
using Field = std::pair<std::string_view, std::string_view>;

/// Looks a text up in the interface's catalogue and fills each "{name}" with
/// its value. A field whose value is empty drops, with the space before it.
///
/// @param english the text, in English
/// @param fields each place's name and value
/// @return the text
std::string filled(std::string_view english, std::initializer_list<Field> fields) {
    std::string text(oa::data::languages::interface_text(english));
    for (const auto& [name, value] : fields) {
        const std::string place = "{" + std::string(name) + "}";
        for (std::size_t at = text.find(place); at != std::string::npos;
             at = text.find(place, at + value.size())) {
            if (value.empty() && at > 0 && text[at - 1] == ' ') {
                text.erase(at - 1, place.size() + 1);
                at -= 1;
            } else {
                text.replace(at, place.size(), value);
            }
        }
    }
    return text;
}

/// Returns the pack's name as the line shows it: the catalogue's, else the
/// record's, else its id.
///
/// @param host the host's pack
/// @param offer what the registry offers
/// @return the name
std::string_view pack_words(const HostPack& host, const Offer& offer) {
    if (!offer.pack_name.empty())
        return offer.pack_name;
    if (!host.name.empty())
        return host.name;
    return host.id;
}

/// Returns the registry's name as the line shows it: its descriptor's, else its id.
///
/// @param host the host's pack
/// @param offer what the registry offers
/// @return the name
std::string_view registry_words(const HostPack& host, const Offer& offer) {
    return offer.registry_name.empty() ? std::string_view(host.registry_id)
                                       : std::string_view(offer.registry_name);
}

/// Returns a catalogue release as the line shows it.
///
/// @param release the release; 0 or less is not known
/// @return its number, or "?"
std::string release_words(int64_t release) {
    return release > 0 ? std::to_string(release) : std::string(kUnknownRelease);
}

/// Tells whether the registry offers the host's release.
///
/// @param host the host's pack
/// @param offer what the registry offers
/// @return true when its catalogue lists the id at the host's SHA-256
bool offers_host_release(const HostPack& host, const Offer& offer) noexcept {
    return offer.registry_known && offer.registry_enabled && offer.catalogue_loaded &&
           offer.listed && offer.sha256 == host.sha256;
}

} // namespace

HostPack host_pack_of(const netgame::PresenceMapPack& pack) {
    HostPack host;
    host.id = pack.key;
    const auto hex = oa::base::sha256::to_hex(pack.sha256);
    host.sha256.assign(hex.data(), hex.size());
    host.registry_id = pack.registry;
    host.registry_url = pack.descriptor_url;
    host.name = pack.name;
    host.version = pack.version;
    host.release = pack.release;
    return host;
}

GetMap get_map_state(
    std::string_view host_map,
    const std::optional<HostPack>& host,
    const LocalPack& local,
    const Offer& offer,
    bool downloading,
    bool failed
) {
    const auto split = oa::data::map_pack::split_pack_map_name(host_map);
    if (!split || !host || host->id != split->id ||
        (local.installed && local.sha256 == host->sha256))
        return GetMap::none;
    if (downloading)
        return GetMap::downloading;
    if (failed)
        return GetMap::failed;
    if (!local.installed) {
        if (!offer.registry_known)
            return host->registry_url.empty() ? GetMap::gone : GetMap::add_registry_first;
        if (!offer.registry_enabled || !offer.downloads_on)
            return GetMap::downloads_off;
        if (!offer.catalogue_loaded)
            return GetMap::downloading;
        if (!offer.listed || offer.sha256 != host->sha256)
            return GetMap::gone;
        // The catalogue says this game cannot play the host's map: the pack
        // is not fetched only for the installer to refuse it.
        if (!offer.unplayable.empty())
            return GetMap::unplayable;
        return GetMap::get;
    }
    if (!offers_host_release(*host, offer))
        return GetMap::other_release_gone;
    return offer.unplayable.empty() ? GetMap::other_release_get : GetMap::unplayable;
}

std::string get_map_text(
    GetMap state,
    const HostPack& host,
    const LocalPack& local,
    const Offer& offer,
    uint64_t done_bytes,
    std::string_view error
) {
    const std::string_view pack = pack_words(host, offer);
    const std::string_view registry = registry_words(host, offer);
    switch (state) {
    case GetMap::none:
        return {};
    case GetMap::get: {
        const std::string_view version = offer.version.empty() ? std::string_view(host.version)
                                                               : std::string_view(offer.version);
        const std::string size = size_words(offer.bytes);
        return filled(
            kGetText, {{"pack", pack}, {"version", version}, {"size", size}, {"registry", registry}}
        );
    }
    case GetMap::add_registry_first:
        return filled(kAddRegistryText, {{"registry", registry}, {"pack", pack}});
    case GetMap::other_release_get:
    case GetMap::other_release_gone: {
        const std::string mine = release_words(local.release);
        const std::string theirs = release_words(host.release);
        return filled(kOtherReleaseText, {{"mine", mine}, {"pack", pack}, {"theirs", theirs}});
    }
    case GetMap::gone:
        return filled(kGoneText, {{"pack", pack}, {"registry", registry}});
    case GetMap::unplayable:
        return offer.unplayable;
    case GetMap::downloads_off:
        return filled(kDownloadsOffText, {{"registry", registry}});
    case GetMap::downloading: {
        // Nothing arrived yet reads 0, as no size of the Library's units does.
        const std::string done = done_bytes == 0 ? std::string("0") : size_words(done_bytes);
        const std::string size = size_words(offer.bytes);
        return filled(kDownloadingText, {{"pack", pack}, {"done", done}, {"size", size}});
    }
    case GetMap::failed:
        return filled(kFailedText, {{"pack", pack}, {"reason", error}});
    }
    return {};
}

std::string get_map_caption() {
    return std::string(oa::data::languages::interface_text(kCaption));
}

bool get_map_button_shown(GetMap state) noexcept {
    return state == GetMap::get || state == GetMap::add_registry_first ||
           state == GetMap::other_release_get || state == GetMap::failed;
}

} // namespace oa::ui::frontend_multiplayer
