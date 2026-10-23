// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Active tab's base game and expansions. A pill asks
// turn_content_expansion and keeps the result on the Library until the
// Library closes. Nothing here writes a preference.
#include "oa/ui/library/library.hpp"

#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/library/text.hpp"
#include "screen_controls.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::library {

namespace {

namespace bundles = oa::data::content_bundles;

/// The letters drawn on the base game tile when its icon is absent.
constexpr std::string_view base_letters = "TA";

/// Tells whether a list holds an id.
///
/// @param ids the ids
/// @param id the id
/// @return true when it is among them
bool holds(const std::vector<std::string>& ids, std::string_view id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/// Tells whether two id lists hold the same ids.
///
/// @param left one list
/// @param right the other
/// @return true when each id of either is in the other
bool same_ids(const std::vector<std::string>& left, const std::vector<std::string>& right) {
    if (left.size() != right.size())
        return false;
    for (const std::string& id : left) {
        if (!holds(right, id))
            return false;
    }
    return true;
}

/// Returns views of ids owned by `ids`, for the call that reads them.
///
/// @param ids the ids
/// @return views of them, in the same order
std::vector<std::string_view> id_views(const std::vector<std::string>& ids) {
    std::vector<std::string_view> views;
    views.reserve(ids.size());
    for (const std::string& id : ids)
        views.emplace_back(id);
    return views;
}

/// Returns the base game id the inputs name, or the default when they name none.
///
/// @param id the stored id
/// @return the id to show and to turn
std::string resolved_base(std::string_view id) {
    if (id.empty())
        return std::string(bundles::default_base_game_id);
    return std::string(id);
}

/// Tells whether a mod is played: an installed mod marked playing, or a mod
/// the inputs name outside the packages.
///
/// @param library the Library
/// @return true when one is
bool mod_is_played(const Library& library) {
    if (library.inputs.playing_mod)
        return true;
    for (const Installed& installed : library.inputs.installed) {
        if (installed.kind == Kind::mod && installed.playing)
            return true;
    }
    return false;
}

/// Tells whether an expansion extends a base game.
///
/// @param bundle the expansion
/// @param base the base game id
/// @return true when it extends that game
bool extends_game(const bundles::Bundle& bundle, std::string_view base) {
    for (const std::string_view game : bundle.extends) {
        if (game == base)
            return true;
    }
    return false;
}

/// The choice the pills are turning, and the base game it names.
struct PendingChoice {
    bundles::ContentChoice choice{};
    std::string base{};
};

/// Returns the choice a pill turns: the draft while it has been edited, and
/// the stored choice otherwise.
///
/// @param library the Library
/// @return the choice
PendingChoice pending_choice(const Library& library) {
    PendingChoice pending;
    const ContentDraft& draft = library.content;
    const ContentSnapshot& stored = library.inputs.content;
    if (draft.edited && !draft.base_game.empty())
        pending.base = draft.base_game;
    else if (!draft.seed_base.empty())
        pending.base = draft.seed_base;
    else
        pending.base = resolved_base(stored.base_id);
    pending.choice.base_game = pending.base;
    pending.choice.command_line = stored.locked;
    if (draft.edited)
        pending.choice.expansions = draft.expansions;
    else if (!draft.seed_base.empty())
        pending.choice.expansions = draft.seed_enabled;
    else
        pending.choice.expansions = stored.enabled;
    return pending;
}

/// Returns the expansions of one base game, in the built-in order.
///
/// @param base the base game id
/// @return those expansions
std::vector<const bundles::Bundle*> expansions_of(std::string_view base) {
    std::vector<const bundles::Bundle*> found;
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        if (bundle.kind == bundles::Kind::expansion && extends_game(bundle, base))
            found.push_back(&bundle);
    }
    return found;
}

/// Returns the name a bundle shows, in the language shown. An unknown id is
/// shown as itself.
///
/// @param text how texts are looked up
/// @param id the bundle id
/// @return the name
std::string bundle_name(const TextHooks& text, std::string_view id) {
    const bundles::Bundle* bundle = bundles::find_bundle(id);
    if (bundle == nullptr)
        return std::string(id);
    return std::string(shown(text, bundle->english_name));
}

/// Returns the expansions that went off with a press, apart from the one pressed.
///
/// @param before the expansions that were on
/// @param after the expansions that are on
/// @param pressed the expansion the pill named
/// @return the others that went off, in the built-in order
std::vector<std::string> dependents_off(
    const std::vector<std::string>& before,
    const std::vector<std::string>& after,
    std::string_view pressed
) {
    std::vector<std::string> gone;
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        if (bundle.kind != bundles::Kind::expansion || bundle.id == pressed)
            continue;
        if (holds(before, bundle.id) && !holds(after, bundle.id))
            gone.emplace_back(bundle.id);
    }
    return gone;
}

/// Drops an unload whose reason no longer holds.
///
/// While another base game is pending, the unload stays. Otherwise a
/// required-expansion unload clears only when that expansion is on again, and
/// any other unload clears: back on the stored base game, the mod played with
/// it plays again.
///
/// @param[in,out] draft the draft
void settle_unload(ContentDraft& draft) {
    if (draft.pending_base)
        return;
    const bool required_back = draft.clear_reason == PendingClear::required_expansion_off &&
                               !draft.unloaded_for.empty() &&
                               holds(draft.expansions, draft.unloaded_for);
    const bool other = draft.clear_reason != PendingClear::required_expansion_off;
    if (required_back || other) {
        draft.unload_mod = false;
        draft.clear_reason = PendingClear::none;
        draft.unloaded_mod.clear();
        draft.unloaded_for.clear();
    }
}

/// Marks the draft edited when closing the Library has something to apply:
/// another base game, other expansions, or a mod to unload.
///
/// The host stores only an edited draft, and a draft that is not edited shows
/// the stored choice and the mod played, so an unload alone edits it too.
///
/// @param[in,out] draft the draft
void mark_edited(ContentDraft& draft) {
    draft.edited = draft.unload_mod || draft.base_game != draft.seed_base ||
                   !same_ids(draft.expansions, draft.seed_enabled);
}

/// Writes a choice onto the draft and records the pill that made it.
///
/// A note that a mod was unloaded is dropped once that expansion is on again.
///
/// @param[in,out] library the Library
/// @param choice the choice G04 returned
/// @param id the expansion
/// @param on that expansion is on afterwards
void keep_choice(
    Library& library, const bundles::ContentChoice& choice, std::string_view id, bool on
) {
    ContentDraft& draft = library.content;
    draft.base_game = choice.base_game;
    draft.expansions = choice.expansions;
    draft.last_id = std::string(id);
    draft.last_on = on;
    draft.asking = false;
    draft.ask_id.clear();
    draft.proposed.clear();
    draft.turned_off.clear();
    // A pill leaves the base game as it is, so a pending one keeps its unload.
    settle_unload(draft);
    mark_edited(draft);
}

/// Returns the name a base game or an expansion shows.
///
/// Total Annihilation is the Active tab's name, without the release. Every
/// other bundle uses the name the bundle table gives.
///
/// @param text how texts are looked up
/// @param id the bundle id
/// @return the name
std::string display_name(const TextHooks& text, std::string_view id) {
    if (id == bundles::default_base_game_id)
        return std::string(shown(text, words::active_game_name));
    return bundle_name(text, id);
}

/// Joins names with "and" before the last.
///
/// @param text how texts are looked up
/// @param names the names
/// @return the joined names; empty when there are none
std::string join_and(const TextHooks& text, const std::vector<std::string>& names) {
    if (names.empty())
        return {};
    if (names.size() == 1)
        return names.front();
    std::string joined = names.front();
    for (std::size_t index = 1; index + 1 < names.size(); ++index)
        joined = filled(text, words::names_comma, {{"left", joined}, {"right", names[index]}});
    return filled(text, words::names_and, {{"left", joined}, {"right", names.back()}});
}

/// Joins fact values with a middle dot.
///
/// @param text how texts are looked up
/// @param parts the parts
/// @return the joined fact; empty when there are none
std::string join_dot(const TextHooks& text, const std::vector<std::string>& parts) {
    if (parts.empty())
        return {};
    std::string joined = parts.front();
    for (std::size_t index = 1; index < parts.size(); ++index)
        joined = filled(text, words::fact_join, {{"left", joined}, {"right", parts[index]}});
    return joined;
}

/// The games and expansions a package names, after the default games.
struct FitLists {
    std::vector<std::string> games{};
    std::vector<std::string> expansions{};
    bool declared{};
};

/// Returns the lists the package is fitted against.
///
/// Installed lists win when they name a game or an expansion. A package that
/// names expansions and no games takes both base games for a language pack,
/// and totala otherwise.
///
/// @param entry the entry
/// @return the lists; `declared` is false when both sources name nothing
FitLists fit_lists(const Entry& entry) {
    FitLists fit;
    const Installed* installed = entry.installed;
    const Listing* listing = entry.listing;
    const bool from_installed = installed != nullptr && (!installed->supported_games.empty() ||
                                                         !installed->required_expansions.empty());
    const bool from_listing = listing != nullptr && (!listing->supported_games.empty() ||
                                                     !listing->required_expansions.empty());
    if (from_installed) {
        fit.games = installed->supported_games;
        fit.expansions = installed->required_expansions;
        fit.declared = true;
    } else if (from_listing) {
        fit.games = listing->supported_games;
        fit.expansions = listing->required_expansions;
        fit.declared = true;
    }
    if (!fit.declared || !fit.games.empty() || fit.expansions.empty())
        return fit;
    if (entry.id.kind == Kind::language) {
        fit.games.emplace_back(bundles::default_base_game_id);
        fit.games.emplace_back("totala-demo");
    } else {
        fit.games.emplace_back(bundles::default_base_game_id);
    }
    return fit;
}

/// Asks G04 to turn one expansion on, on a copy of the choice.
///
/// The played mod is left out, so the press does not clear it.
///
/// @param choice the choice before the press
/// @param id the expansion
/// @param copied the bundle ids that are copied
/// @param in_match true while a match is running
/// @param[out] after the choice afterwards
/// @return true when the expansion is on afterwards
bool press_on(
    const bundles::ContentChoice& choice,
    std::string_view id,
    const std::vector<std::string_view>& copied,
    bool in_match,
    bundles::ContentChoice& after
) {
    const std::vector<std::string_view> no_mod;
    const bundles::ContentChangeResult turned =
        bundles::turn_content_expansion(choice, id, true, copied, in_match, false, no_mod);
    if (turned.refusal != bundles::ContentRefusal::none || !turned.applied)
        return false;
    if (!holds(turned.choice.expansions, id))
        return false;
    after = turned.choice;
    return true;
}

/// Returns the registry the inputs name.
///
/// @param inputs the inputs
/// @param id the registry's id
/// @return the registry; null when the inputs do not name it
const Registry* registry_named(const Inputs& inputs, std::string_view id) {
    for (const Registry& registry : inputs.registries) {
        if (registry.id == id)
            return &registry;
    }
    return nullptr;
}

/// Tells whether an offered expansion extends a base game.
///
/// An empty list extends none.
///
/// @param offered the offer
/// @param base the base game id
/// @return true when the offer names that game
bool offer_extends(const DownloadableExpansion& offered, std::string_view base) {
    for (const std::string& game : offered.extends) {
        if (game == base)
            return true;
    }
    return false;
}

/// The registry an expansion's download arrow uses.
struct ExpansionOffer {
    const DownloadableExpansion* chosen{}; ///< the offer; null when none matches
    bool downloads_on{};                   ///< that registry's downloads are on
    std::string registry_name{};           ///< the name the blocked line uses
    std::string off_reason{};              ///< the registry's own reason; empty for the shared one
};

/// Picks the offer for one expansion of the pending base game.
///
/// A built-in registry whose downloads are on wins, then any whose downloads
/// are on, then any match. An offer that names no base game matches none.
///
/// @param library the Library
/// @param id the expansion
/// @param base the pending base game
/// @return the offer
ExpansionOffer offer_for(const Library& library, std::string_view id, std::string_view base) {
    const DownloadableExpansion* built_in_on = nullptr;
    const DownloadableExpansion* any_on = nullptr;
    const DownloadableExpansion* any = nullptr;
    for (const DownloadableExpansion& offered : library.inputs.downloadable_expansions) {
        if (offered.id != id || !offer_extends(offered, base))
            continue;
        const Registry* registry = registry_named(library.inputs, offered.registry);
        const bool downloads_on = registry != nullptr && registry->downloads_on;
        if (any == nullptr)
            any = &offered;
        if (!downloads_on)
            continue;
        if (any_on == nullptr)
            any_on = &offered;
        if (registry->built_in && built_in_on == nullptr)
            built_in_on = &offered;
    }
    ExpansionOffer offer;
    if (built_in_on != nullptr)
        offer.chosen = built_in_on;
    else if (any_on != nullptr)
        offer.chosen = any_on;
    else
        offer.chosen = any;
    if (offer.chosen == nullptr)
        return offer;
    const Registry* registry = registry_named(library.inputs, offer.chosen->registry);
    offer.downloads_on = registry != nullptr && registry->downloads_on;
    if (registry != nullptr) {
        offer.registry_name = registry->name.empty() ? registry->id : registry->name;
        offer.off_reason = registry->downloads_off_reason;
    }
    return offer;
}

/// Returns an expansion's queue item.
///
/// @param library the Library
/// @param id the expansion
/// @return the item; null when the queue has none
const QueueItem* expansion_item(const Library& library, std::string_view id) {
    for (const QueueItem& item : library.inputs.queue) {
        if (item.expansion && item.key == id)
            return &item;
    }
    return nullptr;
}

/// Tells whether a queue item is still to finish.
///
/// @param phase the item's phase
/// @return true until it has failed or is installed
bool download_pending(QueueItem::Phase phase) {
    return phase != QueueItem::Phase::failed && phase != QueueItem::Phase::installed;
}

/// Remembers that the player asked to download an expansion.
///
/// The choice is not edited, so closing the Library before the copy arrives
/// mounts nothing.
///
/// @param[in,out] library the Library
/// @param id the expansion
void remember_download(Library& library, std::string_view id) {
    if (!holds(library.content.downloads, id))
        library.content.downloads.emplace_back(id);
}

/// Forgets an expansion the player asked to download.
///
/// @param[in,out] library the Library
/// @param id the expansion
void forget_download(Library& library, std::string_view id) {
    std::vector<std::string>& asked = library.content.downloads;
    asked.erase(std::remove(asked.begin(), asked.end(), id), asked.end());
}

/// Returns why an expansion's GET or RETRY is off.
///
/// @param library the Library
/// @param offer the offer
/// @return the words; empty when the arrow can be pressed, or when the reason
///         is only that `--content` locked the run
std::string arrow_blocked(const Library& library, const ExpansionOffer& offer) {
    const TextHooks& text = library.text;
    if (library.inputs.in_match)
        return std::string(shown(text, words::blocked_in_match));
    if (!offer.downloads_on) {
        if (!offer.off_reason.empty())
            return offer.off_reason;
        return filled(text, words::blocked_downloads_off, {{"registry", offer.registry_name}});
    }
    return {};
}

/// Writes the hint for an expansion download that is under way or has failed.
///
/// The expansion the player asked for is named first.
///
/// @param library the Library
/// @return the line; empty when no such download is listed
std::string expansion_download_hint(const Library& library) {
    const QueueItem* chosen = nullptr;
    for (const std::string& id : library.content.downloads) {
        const QueueItem* item = expansion_item(library, id);
        if (item != nullptr && item->phase != QueueItem::Phase::installed) {
            chosen = item;
            break;
        }
    }
    if (chosen == nullptr) {
        for (const QueueItem& item : library.inputs.queue) {
            if (!item.expansion || item.phase == QueueItem::Phase::installed)
                continue;
            chosen = &item;
            break;
        }
    }
    if (chosen == nullptr)
        return {};
    return queue_status_text(*chosen, library.text);
}

/// Writes why the first offered expansion's download arrow is off.
///
/// @param library the Library
/// @param base the pending base game
/// @return the words; empty when every offered arrow can be pressed
std::string expansion_blocked_hint(const Library& library, std::string_view base) {
    for (const bundles::Bundle* bundle : expansions_of(base)) {
        if (holds(library.inputs.content.copied, bundle->id))
            continue;
        const ExpansionOffer offer = offer_for(library, bundle->id, base);
        if (offer.chosen == nullptr || offer.downloads_on)
            continue;
        const QueueItem* queued = expansion_item(library, bundle->id);
        if (queued != nullptr && download_pending(queued->phase))
            continue;
        return arrow_blocked(library, offer);
    }
    return {};
}

/// Sets the download arrow on a pill that is not copied.
///
/// @param[in,out] pill the pill
/// @param library the Library
/// @param offer the offer; none leaves the pill as ADD FROM…
/// @param pills_on false during a match and while `--content` locked the run
void set_download_arrow(
    ExpansionPill& pill, const Library& library, const ExpansionOffer& offer, bool pills_on
) {
    if (offer.chosen == nullptr)
        return;
    pill.download_registry = offer.chosen->registry;
    const QueueItem* queued = expansion_item(library, pill.id);
    const bool asked = holds(library.content.downloads, pill.id);
    if (queued != nullptr && download_pending(queued->phase)) {
        pill.arrow = ExpansionArrow::cancel;
        pill.arrow_enabled = true;
    } else if (
        queued != nullptr && queued->phase == QueueItem::Phase::failed && queued->retryable
    ) {
        pill.arrow = ExpansionArrow::retry;
        pill.arrow_enabled = offer.downloads_on && pills_on;
    } else if (queued != nullptr) {
        pill.arrow = std::nullopt;
        pill.arrow_enabled = false;
    } else if (asked) {
        pill.arrow = ExpansionArrow::cancel;
        pill.arrow_enabled = true;
    } else {
        pill.arrow = ExpansionArrow::get;
        pill.arrow_enabled = offer.downloads_on && pills_on;
    }
    if (pill.arrow == ExpansionArrow::cancel)
        pill.enabled = true;
    if (pill.arrow && !pill.arrow_enabled)
        pill.blocked = arrow_blocked(library, offer);
}

/// Presses an expansion that is not copied.
///
/// No offer is ADD FROM…. An offer downloads it, or cancels or retries the
/// download already asked for. GET and RETRY are refused during a match,
/// while `--content` locked the run, and while that registry's downloads are
/// off. CANCEL is not.
///
/// @param[in,out] library the Library
/// @param id the expansion
/// @param base the pending base game
/// @return what the press did
ContentPress press_uncopied(Library& library, std::string_view id, std::string_view base) {
    ContentPress result;
    const ExpansionOffer offer = offer_for(library, id, base);
    const QueueItem* queued = expansion_item(library, id);
    const bool choosing_locked = library.inputs.content.locked || library.inputs.in_match;
    if (queued != nullptr && download_pending(queued->phase)) {
        forget_download(library, id);
        result.kind = ContentPressKind::cancel_download;
        result.expansion = std::string(id);
        result.registry = queued->registry;
        return result;
    }
    if (queued != nullptr && queued->phase == QueueItem::Phase::failed && queued->retryable &&
        offer.chosen != nullptr && offer.downloads_on && !choosing_locked) {
        remember_download(library, id);
        result.kind = ContentPressKind::retry_download;
        result.expansion = std::string(id);
        result.registry = queued->registry;
        return result;
    }
    if (queued != nullptr)
        return result;
    if (holds(library.content.downloads, id)) {
        const std::string registry =
            offer.chosen != nullptr ? offer.chosen->registry : std::string{};
        forget_download(library, id);
        result.kind = ContentPressKind::cancel_download;
        result.expansion = std::string(id);
        result.registry = registry;
        return result;
    }
    if (offer.chosen == nullptr) {
        if (choosing_locked)
            return result;
        result.kind = ContentPressKind::add;
        result.expansion = std::string(id);
        return result;
    }
    if (choosing_locked || !offer.downloads_on)
        return result;
    remember_download(library, id);
    result.kind = ContentPressKind::download;
    result.expansion = std::string(id);
    result.registry = offer.chosen->registry;
    return result;
}

/// Returns the name of the mod being played, when the pending choice has not
/// already unloaded it.
///
/// @param library the Library
/// @return the name; empty when no mod shows
std::string played_mod_name(const Library& library) {
    if (library.content.unload_mod)
        return {};
    for (const Installed& installed : library.inputs.installed) {
        if (installed.kind == Kind::mod && installed.playing && !installed.name.empty())
            return installed.name;
    }
    if (library.inputs.playing_mod && !library.inputs.playing_mod->name.empty())
        return library.inputs.playing_mod->name;
    if (mod_is_played(library))
        return std::string(shown(library.text, words::no_mod));
    return {};
}

/// Turns an expansion on or off on the pending choice as its pill would, and
/// returns the result without keeping it. A mod being played that the
/// pending choice has not already unloaded passes its needs.
///
/// @param library the Library
/// @param pending the pending choice
/// @param id the expansion
/// @param on_now the expansion is on in the pending choice
/// @return what turning it gives
bundles::ContentChangeResult turned_by_pill(
    const Library& library, const PendingChoice& pending, std::string_view id, bool on_now
) {
    const std::vector<std::string_view> copied_ids = id_views(library.inputs.content.copied);
    // The mod is already coming off, so a later pill does not ask again.
    const bool playing = mod_is_played(library) && !library.content.unload_mod;
    const std::vector<std::string_view> required_ids =
        playing ? id_views(library.inputs.content.mod_required) : std::vector<std::string_view>{};
    return bundles::turn_content_expansion(
        pending.choice, id, !on_now, copied_ids, library.inputs.in_match, playing, required_ids
    );
}

/// Tells whether a pill is locked: the command line chose the mod being
/// played, and pressing the pill would turn off an expansion that mod needs.
/// Closing the Library never unloads that mod, so press_expansion refuses
/// the press. During a match and while `--content` locked the run every
/// press is refused, and a pill that is off turns on, so none of those is
/// locked.
///
/// @param library the Library
/// @param pending the pending choice
/// @param id an expansion of the pending base game
/// @return true when the pill is locked
bool locked_by_command_line(
    const Library& library, const PendingChoice& pending, std::string_view id
) {
    if (!library.inputs.mod_from_command_line || library.inputs.content.locked ||
        library.inputs.in_match)
        return false;
    if (!holds(pending.choice.expansions, id))
        return false;
    const bundles::ContentChangeResult turned = turned_by_pill(library, pending, id, true);
    return turned.refusal == bundles::ContentRefusal::none && turned.applied &&
           turned.mod_cleared == bundles::ModClearReason::required_expansion_off;
}

} // namespace

std::string
brain_unfit_reason(const Inputs& inputs, const BrainFacts& facts, const TextHooks& text) {
    if (const std::optional<formats::oamod::EngineRange> range =
            formats::oamod::parse_engine_range(facts.requires_engine)) {
        if (!formats::oamod::engine_range_met(*range, inputs.engine))
            return filled(text, words::blocked_engine, {{"range", engine_text(*range, text)}});
    }
    if (!facts.works_with.empty()) {
        bool fits = false;
        std::vector<std::string> names;
        for (const WorksWith& item : facts.works_with) {
            if (item.id == "*") {
                fits = true;
                break;
            }
            std::string label;
            if (item.id == supported_base) {
                label = std::string(shown(text, words::base_supported));
                if (inputs.played.id.empty())
                    fits = true;
            } else {
                label = item.name.empty() ? item.id : item.name;
                formats::oamut::Condition condition;
                condition.id = item.id;
                condition.range_text = item.range;
                if (!item.range.empty()) {
                    const std::optional<formats::oamut::VersionRange> parsed =
                        formats::oamut::parse_version_range(item.range);
                    if (!parsed) {
                        names.push_back(std::move(label));
                        continue;
                    }
                    condition.range = *parsed;
                }
                if (formats::oamut::condition_met(
                        condition, inputs.played.id, inputs.played.version
                    ))
                    fits = true;
            }
            names.push_back(std::move(label));
        }
        if (!fits) {
            const auto join_or = [&](const std::vector<std::string>& parts) {
                if (parts.empty())
                    return std::string{};
                if (parts.size() == 1)
                    return parts.front();
                std::string joined = parts.front();
                for (std::size_t index = 1; index + 1 < parts.size(); ++index)
                    joined = filled(
                        text, words::names_comma, {{"left", joined}, {"right", parts[index]}}
                    );
                return filled(text, words::names_or, {{"left", joined}, {"right", parts.back()}});
            };
            return filled(text, words::made_for, {{"names", join_or(names)}});
        }
    }
    for (const std::string& expansion : facts.requires_expansions) {
        if (std::find(inputs.content.enabled.begin(), inputs.content.enabled.end(), expansion) !=
            inputs.content.enabled.end())
            continue;
        return filled(text, words::chip_needs, {{"name", bundle_name(text, expansion)}});
    }
    return {};
}

void note_content(Library& library) {
    const std::string base = resolved_base(library.inputs.content.base_id);
    const std::vector<std::string>& enabled = library.inputs.content.enabled;
    ContentDraft& draft = library.content;
    const bool same_seed = draft.seed_base == base && same_ids(draft.seed_enabled, enabled);
    // A download asked for before the copy arrives has not edited the choice,
    // and nor has the question asked before an expansion a playing mod needs
    // goes off. A refresh that brings the same stored choice keeps both, so
    // the question's TURN OFF still turns the expansion off when a catalogue
    // or a picture arrived while it showed. A different stored choice drops
    // them, as it drops an edit.
    const bool kept = draft.edited || draft.asking || !draft.downloads.empty();
    if (!same_seed || !kept) {
        draft = {};
        draft.seed_base = base;
        draft.seed_enabled = enabled;
    }
    // Catalogues, pictures and update searches arrive while the Library is
    // open, often in the first second. The chooser stays open through them,
    // on the game the player marked, with its rows read from the new inputs.
    // A different stored choice closes it.
    if (same_seed)
        refresh_base_chooser(library);
    else
        library.chooser = {};
}

ActiveContent active_content(const Library& library) {
    const TextHooks& text = library.text;
    const ContentSnapshot& stored = library.inputs.content;
    const PendingChoice pending = pending_choice(library);
    ActiveContent face;
    face.letters = std::string(base_letters);
    face.badge = library.inputs.base_game_badge ? &*library.inputs.base_game_badge : nullptr;

    const bool demo = pending.base == "totala-demo";
    const bool retail = pending.base == bundles::default_base_game_id;
    if (retail || demo) {
        face.name = std::string(shown(text, words::active_game_name));
    } else {
        const bundles::Bundle* game = bundles::find_bundle(pending.base);
        face.name = game != nullptr ? bundle_name(text, game->id) : pending.base;
    }
    std::string version;
    if (retail)
        version = std::string(shown(text, words::active_version));
    else if (demo)
        version = std::string(shown(text, words::active_version_demo));
    if (!version.empty() && !stored.source.empty())
        face.version_line = filled(
            text, words::active_version_source, {{"version", version}, {"source", stored.source}}
        );
    else
        face.version_line = version;
    if (stored.files_differ) {
        const int32_t count = *stored.files_differ;
        if (count <= 0) {
            face.check_line = std::string(shown(text, words::active_files_match));
            face.check_good = true;
        } else if (count == 1) {
            const std::string one = "1";
            face.check_line = filled(text, words::active_file_differs, {{"count", one}});
            face.check_good = false;
        } else {
            const std::string count_text = std::to_string(count);
            face.check_line = filled(text, words::active_files_differ, {{"count", count_text}});
            face.check_good = false;
        }
    }
    face.read = version.empty() ? face.name : face.name + " · " + version;

    const std::vector<const bundles::Bundle*> expansions = expansions_of(pending.base);
    const bool pills_on = !stored.locked && !library.inputs.in_match;
    face.change_enabled = pills_on && !library.inputs.mod_from_command_line;
    int32_t on_count = 0;
    std::vector<std::string> locked_names;
    face.pills.reserve(expansions.size());
    for (const bundles::Bundle* bundle : expansions) {
        ExpansionPill pill;
        pill.id = std::string(bundle->id);
        const bool on = holds(pending.choice.expansions, bundle->id);
        const bool copied = holds(stored.copied, bundle->id);
        if (on)
            ++on_count;
        const ExpansionOffer offer = offer_for(library, bundle->id, pending.base);
        if (on)
            pill.face = ExpansionFace::on;
        else if (!copied && offer.chosen == nullptr)
            pill.face = ExpansionFace::add;
        else
            pill.face = ExpansionFace::off;
        const bool music_short = bundle->id == "music" && stored.music_tracks &&
                                 *stored.music_tracks < stored.music_track_count;
        if (pill.face == ExpansionFace::add)
            pill.text =
                filled(text, words::active_add_from, {{"name", bundle_name(text, bundle->id)}});
        else if (music_short)
            pill.text = filled(
                text,
                words::music_tracks,
                {{"held", std::to_string(*stored.music_tracks)},
                 {"total", std::to_string(stored.music_track_count)}}
            );
        else
            pill.text = bundle_name(text, bundle->id);
        pill.enabled = pills_on;
        if (!copied && !on)
            set_download_arrow(pill, library, offer, pills_on);
        if (locked_by_command_line(library, pending, bundle->id)) {
            pill.locked = true;
            pill.enabled = false;
            locked_names.push_back(bundle_name(text, bundle->id));
        }
        face.pills.push_back(std::move(pill));
    }
    if (!locked_names.empty()) {
        const std::string names = join_and(text, locked_names);
        face.lock = filled(text, words::active_command_line_needs, {{"names", names}});
    }

    if (expansions.empty()) {
        face.expansions_heading = std::string(shown(text, words::active_heading_expansions));
        face.hint = std::string(shown(text, words::active_no_expansions));
    } else {
        const std::string on_text = std::to_string(on_count);
        const std::string count_text = std::to_string(static_cast<int32_t>(expansions.size()));
        face.expansions_heading =
            filled(text, words::active_expansions_count, {{"on", on_text}, {"count", count_text}});
    }

    const ContentDraft& draft = library.content;
    const bool unloaded = draft.clear_reason == PendingClear::required_expansion_off &&
                          !draft.unloaded_mod.empty() && !draft.unloaded_for.empty() &&
                          !holds(pending.choice.expansions, draft.unloaded_for);
    if (draft.pending_base) {
        face.hint = std::string(shown(text, words::base_applies));
    } else if (unloaded) {
        face.hint = filled(
            text,
            words::mod_unloaded_because,
            {{"mod", draft.unloaded_mod}, {"name", bundle_name(text, draft.unloaded_for)}}
        );
    } else if (draft.edited && !draft.last_id.empty()) {
        const std::string name = bundle_name(text, draft.last_id);
        face.hint = filled(
            text, draft.last_on ? words::active_turns_on : words::active_turns_off, {{"name", name}}
        );
    } else if (
        const std::string downloading = expansion_download_hint(library); !downloading.empty()
    ) {
        face.hint = downloading;
    } else if (!draft.edited && library.expansion_note) {
        face.hint = std::string(shown(text, words::mod_unloaded_note));
    } else if (
        const std::string blocked = expansion_blocked_hint(library, pending.base); !blocked.empty()
    ) {
        face.hint = blocked;
    } else if (!expansions.empty()) {
        for (const bundles::Bundle* bundle : expansions) {
            if (holds(pending.choice.expansions, bundle->id) || bundle->needs.empty())
                continue;
            const std::string need = bundle_name(text, bundle->needs.front());
            face.hint = filled(
                text, words::active_needs, {{"name", bundle_name(text, bundle->id)}, {"need", need}}
            );
            break;
        }
    }
    return face;
}

ContentPress press_expansion(Library& library, std::string_view id) {
    ContentPress result;
    if (library.content.seed_base.empty() && !library.content.edited)
        note_content(library);
    if (library.content.asking)
        return result;

    const bundles::Bundle* bundle = bundles::find_bundle(id);
    const PendingChoice pending = pending_choice(library);
    if (bundle == nullptr || bundle->kind != bundles::Kind::expansion ||
        !extends_game(*bundle, pending.base))
        return result;

    const bool copied = holds(library.inputs.content.copied, id);
    const bool on_now = holds(pending.choice.expansions, id);
    if (!copied && !on_now)
        return press_uncopied(library, id, pending.base);
    if (library.inputs.content.locked || library.inputs.in_match)
        return result;

    const bundles::ContentChangeResult turned = turned_by_pill(library, pending, id, on_now);
    if (turned.refusal != bundles::ContentRefusal::none || !turned.applied)
        return result;

    if (turned.mod_cleared == bundles::ModClearReason::required_expansion_off) {
        // Closing the Library does not unload the mod the command line chose,
        // so an expansion it needs stays on, as CHANGE… stays off. The pill
        // is locked by the same rule (locked_by_command_line).
        if (library.inputs.mod_from_command_line)
            return result;
        ContentDraft& draft = library.content;
        draft.asking = true;
        draft.ask_id = std::string(id);
        draft.proposed = turned.choice.expansions;
        draft.turned_off = dependents_off(pending.choice.expansions, turned.choice.expansions, id);
        result.kind = ContentPressKind::ask;
        result.expansion = std::string(id);
        return result;
    }

    const bool on_after = holds(turned.choice.expansions, id);
    keep_choice(library, turned.choice, id, on_after);
    result.kind = ContentPressKind::redraw;
    return result;
}

void drop_expansion_download(Library& library, std::string_view id) {
    forget_download(library, id);
}

void adopt_downloaded_expansions(Library& library) {
    if (library.content.downloads.empty())
        return;
    if (library.inputs.in_match || library.inputs.content.locked || library.content.asking)
        return;
    std::vector<std::string> waiting;
    for (const std::string& id : library.content.downloads) {
        if (!holds(library.inputs.content.copied, id)) {
            waiting.push_back(id);
            continue;
        }
        const PendingChoice pending = pending_choice(library);
        if (holds(pending.choice.expansions, id))
            continue;
        const std::vector<std::string_view> copied_ids = id_views(library.inputs.content.copied);
        bundles::ContentChoice after;
        if (!press_on(pending.choice, id, copied_ids, false, after))
            continue;
        keep_choice(library, after, id, true);
    }
    library.content.downloads = std::move(waiting);
}

void accept_expansion_off(Library& library) {
    ContentDraft& draft = library.content;
    if (!draft.asking)
        return;
    const std::string unloaded = played_mod_name(library);
    const std::string because = draft.ask_id;
    if (draft.base_game.empty())
        draft.base_game = draft.seed_base.empty() ? resolved_base(library.inputs.content.base_id)
                                                  : draft.seed_base;
    draft.expansions = draft.proposed;
    draft.last_id = draft.ask_id;
    draft.last_on = false;
    draft.asking = false;
    draft.ask_id.clear();
    draft.proposed.clear();
    draft.turned_off.clear();
    draft.pending_base = false;
    draft.unload_mod = true;
    draft.clear_reason = PendingClear::required_expansion_off;
    if (!unloaded.empty() && !because.empty()) {
        draft.unloaded_mod = unloaded;
        draft.unloaded_for = because;
    }
    // The expansions can end where they were stored, when the one turned off
    // was turned on first. The unload still edits the draft.
    mark_edited(draft);
}

void cancel_expansion_off(Library& library) {
    ContentDraft& draft = library.content;
    draft.asking = false;
    draft.ask_id.clear();
    draft.proposed.clear();
    draft.turned_off.clear();
}

namespace {

/// Returns the base games, in the built-in order.
///
/// @return those bundles
std::vector<const bundles::Bundle*> base_games() {
    std::vector<const bundles::Bundle*> found;
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        if (bundle.kind == bundles::Kind::base_game)
            found.push_back(&bundle);
    }
    return found;
}

/// Writes the chooser's rows from the inputs, keeping `marked`.
///
/// @param[in,out] library the Library
/// @param marked the row the radio marks
void fill_chooser(Library& library, std::string_view marked) {
    // A row's id can be what `marked` views. The chooser below is replaced,
    // and that row with it, so the id is kept first.
    const std::string marked_id{marked};
    const TextHooks& text = library.text;
    const ContentSnapshot& stored = library.inputs.content;
    const PendingChoice pending = pending_choice(library);
    BaseChooser& chooser = library.chooser;
    const std::string problem = chooser.problem;
    chooser = {};
    chooser.open = true;
    chooser.problem = problem;
    chooser.marked = marked_id;
    const std::string mounted = resolved_base(stored.base_id);
    for (const bundles::Bundle* game : base_games()) {
        BaseGameRow row;
        row.id = std::string(game->id);
        const bool demo = game->id == "totala-demo";
        const bool retail = game->id == bundles::default_base_game_id;
        row.letters = demo ? "DEMO" : "TA";
        if ((retail || demo) && library.inputs.base_game_badge)
            row.badge = &*library.inputs.base_game_badge;
        if (retail)
            row.name = std::string(shown(text, words::active_game_name));
        else if (demo)
            row.name = std::string(shown(text, words::base_demo_name));
        else
            row.name = bundle_name(text, game->id);
        if (retail)
            row.version = std::string(shown(text, words::active_version));
        else if (demo)
            row.version = std::string(shown(text, words::base_demo_version));
        const bool copied = holds(stored.copied, game->id);
        if (game->id == pending.base)
            row.state = BaseGameState::in_use;
        else if (copied)
            row.state = BaseGameState::copied;
        else
            row.state = BaseGameState::not_added;
        if (row.state == BaseGameState::in_use)
            row.state_text = std::string(shown(text, words::chip_in_use));
        else if (row.state == BaseGameState::copied)
            row.state_text = std::string(shown(text, words::base_copied));
        else
            row.state_text = std::string(shown(text, words::base_not_added));
        if (row.state == BaseGameState::not_added) {
            row.detail =
                std::string(shown(text, demo ? words::base_demo_detail : words::base_ask_game));
        } else if (game->id == mounted && !stored.source.empty()) {
            row.detail = filled(text, words::base_copied_from, {{"source", stored.source}});
            if (stored.files_differ) {
                std::string check;
                const int32_t count = *stored.files_differ;
                if (count <= 0) {
                    check = std::string(shown(text, words::active_files_match));
                } else if (count == 1) {
                    check = filled(text, words::active_file_differs, {{"count", "1"}});
                } else {
                    check = filled(
                        text, words::active_files_differ, {{"count", std::to_string(count)}}
                    );
                }
                row.detail += " · ";
                // The check line is already looked up. Fold its first letter
                // so it reads as the rest of the source line.
                if (!check.empty() && check.front() >= 'A' && check.front() <= 'Z')
                    check.front() = static_cast<char>(check.front() - 'A' + 'a');
                row.detail += check;
            }
        }
        if (game->id == pending.base) {
            for (const bundles::Bundle* expansion : expansions_of(game->id)) {
                if (holds(pending.choice.expansions, expansion->id))
                    row.expansions.push_back(bundle_name(text, expansion->id));
            }
        } else if (copied) {
            for (const bundles::Bundle* expansion : expansions_of(game->id)) {
                if (holds(stored.copied, expansion->id))
                    row.expansions.push_back(bundle_name(text, expansion->id));
            }
        }
        row.marked = row.id == chooser.marked;
        chooser.rows.push_back(std::move(row));
    }
    if (chooser.marked != pending.base) {
        const std::string mod = played_mod_name(library);
        if (mod.empty())
            chooser.note = std::string(shown(text, words::base_note_plain));
        else
            chooser.note = filled(text, words::base_note_mod, {{"mod", mod}});
    }
}

} // namespace

BaseOpen open_base_chooser(Library& library) {
    BaseOpen opened;
    if (library.inputs.content.locked || library.inputs.in_match ||
        library.inputs.mod_from_command_line || library.content.asking)
        return opened;
    if (library.content.seed_base.empty() && !library.content.edited)
        note_content(library);
    const PendingChoice pending = pending_choice(library);
    fill_chooser(library, pending.base);
    opened.opened = true;
    for (std::size_t index = 0;
         index < library.chooser.rows.size() && index < controls::most_base_rows;
         ++index) {
        if (library.chooser.rows[index].id == pending.base) {
            opened.focus = controls::first_base_row + static_cast<int32_t>(index);
            break;
        }
    }
    return opened;
}

void close_base_chooser(Library& library) {
    library.chooser = {};
}

void mark_base_game(Library& library, std::string_view id) {
    if (!library.chooser.open)
        return;
    bool known = false;
    for (const BaseGameRow& row : library.chooser.rows) {
        if (row.id == id)
            known = true;
    }
    if (!known)
        return;
    const std::string problem = library.chooser.problem;
    fill_chooser(library, id);
    library.chooser.problem = problem;
}

void refresh_base_chooser(Library& library) {
    if (!library.chooser.open)
        return;
    const std::string marked = library.chooser.marked;
    const std::string problem = library.chooser.problem;
    fill_chooser(library, marked);
    library.chooser.problem = problem;
}

void note_base_copy_problem(Library& library, std::string_view problem) {
    if (!library.chooser.open)
        return;
    library.chooser.problem = std::string(problem);
}

BaseUse use_marked_base_game(Library& library) {
    BaseUse used;
    if (!library.chooser.open)
        return used;
    if (library.inputs.content.locked || library.inputs.in_match ||
        library.inputs.mod_from_command_line) {
        used.kind = BaseUseKind::refused;
        return used;
    }
    if (library.content.seed_base.empty() && !library.content.edited)
        note_content(library);
    const std::string marked = library.chooser.marked;
    const PendingChoice pending = pending_choice(library);
    if (marked.empty() || marked == pending.base) {
        close_base_chooser(library);
        used.kind = BaseUseKind::none;
        return used;
    }
    if (!holds(library.inputs.content.copied, marked)) {
        used.kind = BaseUseKind::needs_source;
        used.id = marked;
        return used;
    }
    const std::vector<std::string_view> copied_ids = id_views(library.inputs.content.copied);
    const bool playing = mod_is_played(library) && !library.content.unload_mod;
    const bundles::ContentChangeResult turned = bundles::choose_content_base_game(
        pending.choice, marked, copied_ids, library.inputs.in_match, playing
    );
    if (turned.refusal != bundles::ContentRefusal::none || !turned.applied) {
        used.kind = BaseUseKind::refused;
        return used;
    }
    ContentDraft& draft = library.content;
    const PendingClear owed = draft.clear_reason;
    draft.base_game = turned.choice.base_game;
    draft.expansions = turned.choice.expansions;
    draft.last_id.clear();
    draft.last_on = false;
    draft.asking = false;
    draft.ask_id.clear();
    draft.proposed.clear();
    draft.turned_off.clear();
    draft.pending_base = draft.base_game != draft.seed_base;
    if (turned.mod_cleared == bundles::ModClearReason::base_game_chosen ||
        owed == PendingClear::required_expansion_off) {
        draft.unload_mod = true;
        if (owed == PendingClear::required_expansion_off) {
            draft.clear_reason = PendingClear::required_expansion_off;
        } else {
            draft.clear_reason = PendingClear::base_game_chosen;
            draft.unloaded_mod.clear();
            draft.unloaded_for.clear();
        }
    }
    // Back on the stored base game, the mod unloaded for another game plays
    // again, and so does one unloaded for an expansion this choice turns back
    // on. Closing then stores no unload, so the tile shows the mod.
    settle_unload(draft);
    mark_edited(draft);
    close_base_chooser(library);
    used.kind = BaseUseKind::applied;
    return used;
}

ContentLines content_lines(const Library& library, const Entry& entry) {
    ContentLines lines;
    if (is_base_game(entry.id) || is_built_in_language(entry.id))
        return lines;
    const FitLists fit = fit_lists(entry);
    if (!fit.declared)
        return lines;

    const TextHooks& text = library.text;
    const PendingChoice pending = pending_choice(library);
    const bool game_ok = holds(fit.games, pending.base);

    if (!fit.games.empty()) {
        std::vector<std::string> parts;
        parts.reserve(fit.games.size());
        for (const std::string& id : fit.games) {
            const std::string name = display_name(text, id);
            if (id == pending.base)
                parts.push_back(filled(text, words::name_met, {{"name", name}}));
            else
                parts.push_back(name);
        }
        lines.games = join_dot(text, parts);
        lines.games_mark = game_ok ? Mark::none : Mark::warn;
    }

    std::vector<std::string> off_names;
    bool any_off = false;
    if (!fit.expansions.empty()) {
        std::vector<std::string> parts;
        parts.reserve(fit.expansions.size());
        for (const std::string& id : fit.expansions) {
            const std::string name = display_name(text, id);
            if (holds(pending.choice.expansions, id)) {
                parts.push_back(filled(text, words::name_met, {{"name", name}}));
            } else {
                any_off = true;
                off_names.push_back(name);
                parts.push_back(filled(text, words::name_off, {{"name", name}}));
            }
        }
        lines.expansions = join_dot(text, parts);
        lines.expansions_mark = any_off ? Mark::warn : Mark::none;
    }

    if (!game_ok) {
        lines.blocks_play = true;
        const bool only_demo = fit.games.size() == 1 && fit.games.front() == "totala-demo";
        if (only_demo) {
            lines.row = std::string(shown(text, words::for_demo_only));
        } else {
            std::vector<std::string> names;
            names.reserve(fit.games.size());
            for (const std::string& id : fit.games)
                names.push_back(display_name(text, id));
            lines.row = filled(text, words::for_game_only, {{"name", join_and(text, names)}});
        }
        return lines;
    }
    if (fit.expansions.empty())
        return lines;

    if (!any_off) {
        std::vector<std::string> names;
        names.reserve(fit.expansions.size());
        for (const std::string& id : fit.expansions)
            names.push_back(display_name(text, id));
        const std::string joined = join_and(text, names);
        if (names.size() == 1)
            lines.row = filled(text, words::needs_which_on, {{"name", joined}});
        else
            lines.row = filled(text, words::needs_which_are_on, {{"names", joined}});
        return lines;
    }

    lines.blocks_play = true;
    const std::string joined = join_and(text, off_names);
    if (off_names.size() == 1)
        lines.row = filled(text, words::needs_which_off, {{"name", joined}});
    else
        lines.row = filled(text, words::needs_which_are_off, {{"names", joined}});
    if (library.inputs.in_match || pending.choice.command_line)
        return lines;

    const std::vector<std::string_view> copied = id_views(library.inputs.content.copied);
    bundles::ContentChoice trial = pending.choice;
    for (const std::string& id : fit.expansions) {
        if (holds(trial.expansions, id))
            continue;
        bundles::ContentChoice after;
        if (!press_on(trial, id, copied, false, after)) {
            lines.turn_on.clear();
            lines.turn_on_name.clear();
            lines.several = false;
            return lines;
        }
        if (lines.turn_on_name.empty())
            lines.turn_on_name = display_name(text, id);
        lines.turn_on.push_back(id);
        trial = std::move(after);
    }
    lines.several = lines.turn_on.size() > 1;
    return lines;
}

bool package_can_run(const Library& library, const Entry& entry) {
    return !content_lines(library, entry).blocks_play;
}

bool turn_on_required(Library& library, const Entry& entry) {
    if (library.content.seed_base.empty() && !library.content.edited)
        note_content(library);
    const ContentLines lines = content_lines(library, entry);
    if (lines.turn_on.empty())
        return false;
    const PendingChoice pending = pending_choice(library);
    if (library.inputs.in_match || pending.choice.command_line)
        return false;
    const std::vector<std::string_view> copied = id_views(library.inputs.content.copied);
    bundles::ContentChoice trial = pending.choice;
    std::string last;
    for (const std::string& id : lines.turn_on) {
        if (holds(trial.expansions, id))
            continue;
        bundles::ContentChoice after;
        if (!press_on(trial, id, copied, false, after))
            return false;
        trial = std::move(after);
        last = id;
    }
    if (last.empty())
        return false;
    keep_choice(library, trial, last, holds(trial.expansions, last));
    return true;
}

std::string turn_on_note(const Library& library) {
    const Entry* entry = selected_entry(library);
    if (entry == nullptr)
        return {};
    const ContentLines lines = content_lines(library, *entry);
    if (lines.turn_on.empty() || !lines.blocks_play)
        return {};
    const TextHooks& text = library.text;
    std::string_view pattern = words::turn_on_then_play;
    if (entry->id.kind == Kind::map_pack)
        pattern = lines.several ? words::turn_on_several_applies : words::turn_on_applies;
    else if (entry->id.kind == Kind::language)
        pattern = lines.several ? words::turn_on_several_use : words::turn_on_then_use;
    else if (lines.several)
        pattern = words::turn_on_several_play;
    return std::string(shown(text, pattern));
}

} // namespace oa::ui::library
