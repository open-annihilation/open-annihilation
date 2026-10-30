// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/key_editor/model.hpp"

#include "oa/formats/oakeys/manifest.hpp"
#include "oa/ui/key_bindings/layout_file.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace oa::ui::key_editor {
namespace {

namespace keys = oa::ui::key_bindings;
namespace oakeys = oa::formats::oakeys;

/// How many of the six helpers a layout set.
///
/// @param helpers the helpers
/// @return the count SHARE… shows
std::size_t helper_count(const oakeys::Helpers& helpers) noexcept {
    std::size_t count = 0;
    if (helpers.toggle)
        ++count;
    if (helpers.sticky_modifiers)
        ++count;
    if (helpers.hold_to_confirm)
        ++count;
    if (helpers.pointer_keys)
        ++count;
    if (helpers.radial_orders)
        ++count;
    if (helpers.edge_scroll)
        ++count;
    return count;
}

/// Replaces one binding, or appends it.
///
/// @param[in,out] layout the layout
/// @param binding the binding
void overlay_binding(keys::Layout& layout, keys::LayoutBinding binding) {
    for (keys::LayoutBinding& existing : layout.bindings) {
        if (existing.action == binding.action) {
            existing = std::move(binding);
            return;
        }
    }
    layout.bindings.push_back(std::move(binding));
}

} // namespace

std::optional<std::string> id_from_name(std::string_view name) {
    std::string id;
    bool hyphen = false;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const auto byte = static_cast<unsigned char>(name[index]);
        if (byte >= 'A' && byte <= 'Z') {
            id.push_back(static_cast<char>(byte - 'A' + 'a'));
            hyphen = false;
            continue;
        }
        if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) {
            id.push_back(static_cast<char>(byte));
            hyphen = false;
            continue;
        }
        if (!id.empty() && !hyphen) {
            id.push_back('-');
            hyphen = true;
        }
    }
    while (!id.empty() && id.back() == '-')
        id.pop_back();
    if (id.empty() || id.size() > 64)
        return std::nullopt;
    if (id == "oa-default" || id == "classic" || id.starts_with("own-"))
        return std::nullopt;
    return id;
}

std::optional<std::string> share_id(const Editor& editor) {
    if (editor.layout_is_own() && !editor.share_name_.empty() &&
        editor.share_name_ == editor.chosen_.name && !editor.chosen_.id.empty())
        return editor.chosen_.id;
    return id_from_name(editor.share_name_);
}

oakeys::Manifest share_manifest(const Editor& editor) {
    const std::optional<std::string> id = share_id(editor);
    oakeys::Manifest rest;
    rest.format = oakeys::format_version;
    rest.id = id ? *id : std::string{};
    rest.name = editor.share_name_;
    rest.version.major = 1;
    rest.version.minor = 0;
    rest.version.parts = 2;
    rest.author.name = std::string(own_author);
    // The player's own layout keeps its description in the file.
    if (editor.layout_is_own())
        rest.description = editor.chosen_.description;
    rest.tags = {"shared"};
    rest.base = oakeys::Base::oa_default;
    rest.helpers = editor.effective_helpers();

    keys::Layout layout;
    layout.id = rest.id;
    layout.base = keys::Base::oa_default;
    if (editor.has_chosen_)
        layout.bindings = editor.chosen_.layout.bindings;
    for (const keys::LayoutBinding& binding : editor.own_.bindings)
        overlay_binding(layout, binding);
    return keys::manifest_from_layout(layout, rest);
}

ShareSummary share_summary(const Editor& editor) {
    const oakeys::Manifest manifest = share_manifest(editor);
    ShareSummary summary;
    summary.name = editor.share_name_;
    summary.id = manifest.id;
    summary.base = editor.layout_name();
    summary.keys = manifest.bindings.size();
    summary.helpers = helper_count(manifest.helpers);
    summary.text = oakeys::write_bindings_text(manifest);
    return summary;
}

} // namespace oa::ui::key_editor
