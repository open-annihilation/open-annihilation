// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The words of the update question, and the release number a skip remembers.
#include "oa/app/update/offer.hpp"

#include "oa/ui/engine_settings.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace oa::app::update {
namespace {

namespace settings = oa::ui::engine_settings;
namespace library = oa::ui::library;
namespace feed = oa::data::engine_feed;

} // namespace

OfferText offer_text(
    const feed::Offer& offer,
    const ThisBuild& build,
    std::string_view language,
    const library::TextHooks& text
) {
    OfferText written;
    if (offer.way == feed::Way::none || offer.release == nullptr || offer.package == nullptr)
        return written;
    written.title =
        library::filled(text, words::is_out, {{"version", offer.release->version_text}});
    const std::string size = library::size_text(offer.package->zip.size, text);
    written.paragraphs.push_back(
        library::filled(text, words::you_have, {{"current", build.version_text}, {"size", size}})
    );
    written.paragraphs.push_back(std::string(library::shown(text, words::stay)));
    if (const feed::Notes* notes = feed::notes_for(*offer.release, language))
        written.notes = notes->lines;
    return written;
}

bool offer_wanted(const feed::Offer& offer, std::optional<int64_t> skipped) noexcept {
    if (offer.way == feed::Way::none || offer.release == nullptr)
        return false;
    if (!skipped)
        return true;
    return offer.release->release > *skipped;
}

std::optional<int64_t> read_skipped(const platform::preferences::Values& values) {
    const auto found = values.find(std::string(settings::key::update_skipped));
    if (found == values.end() || found->second.empty())
        return std::nullopt;
    int64_t number = 0;
    const char* const begin = found->second.data();
    const char* const end = begin + found->second.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return std::nullopt;
    return number;
}

void write_skipped(platform::preferences::Values& values, int64_t release) {
    values[std::string(settings::key::update_skipped)] = std::to_string(release);
}

} // namespace oa::app::update
