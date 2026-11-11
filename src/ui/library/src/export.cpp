// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Export dialog's own state. The screen draws it and the app writes the files.

#include "oa/ui/library/export.hpp"

#include "oa/ui/library/library.hpp"

#include <algorithm>
#include <utility>

namespace oa::ui::library {

void open_export(Library& library, ExportOffer offer) {
    ExportDialog dialog;
    dialog.open = true;
    dialog.files = std::move(offer.files);
    dialog.folder = std::move(offer.folder);
    dialog.folder_label = std::move(offer.folder_label);
    dialog.folder_is_downloads = offer.folder_is_downloads;
    dialog.problem = std::move(offer.problem);
    library.export_dialog = std::move(dialog);
}

void close_export(Library& library) {
    library.export_dialog = {};
}

void set_export_format(Library& library, ExportFormat format) {
    library.export_dialog.format = format;
    library.export_dialog.warning_first = 0;
    library.export_dialog.warning_span = 0;
    library.export_dialog.warning_back = 0;
    library.export_dialog.warning_more = false;
}

void set_export_oa_sectors(Library& library, bool keep) {
    library.export_dialog.keep_oa_sectors = keep;
}

void set_export_folder(Library& library, std::string folder, std::string label, bool downloads) {
    ExportDialog& dialog = library.export_dialog;
    dialog.folder = std::move(folder);
    dialog.folder_label = std::move(label);
    dialog.folder_is_downloads = downloads;
    dialog.browsing = false;
    dialog.entries.clear();
    dialog.browse_first = 0;
    dialog.problem.clear();
}

void set_export_progress(Library& library, uint32_t done, uint32_t total) {
    ExportDialog& dialog = library.export_dialog;
    dialog.exporting = true;
    dialog.done = false;
    dialog.progress_done = done;
    dialog.progress_total = total;
}

void set_export_byte_progress(Library& library, uint64_t done, uint64_t total, bool compressing) {
    ExportDialog& dialog = library.export_dialog;
    dialog.progress_bytes_done = done;
    dialog.progress_bytes_total = total;
    dialog.compressing = compressing;
}

void finish_export(Library& library) {
    ExportDialog& dialog = library.export_dialog;
    dialog.exporting = false;
    dialog.done = true;
}

void fail_export(Library& library, std::string problem) {
    ExportDialog& dialog = library.export_dialog;
    dialog.exporting = false;
    dialog.done = false;
    dialog.problem = std::move(problem);
}

void open_export_browse(Library& library, std::string folder, std::vector<ExportFolder> entries) {
    ExportDialog& dialog = library.export_dialog;
    dialog.browsing = true;
    dialog.browse_folder = std::move(folder);
    dialog.entries = std::move(entries);
    dialog.browse_first = 0;
}

void close_export_browse(Library& library) {
    ExportDialog& dialog = library.export_dialog;
    dialog.browsing = false;
    dialog.entries.clear();
    dialog.browse_folder.clear();
    dialog.browse_first = 0;
}

void scroll_export_browse(ExportDialog& dialog, int delta) {
    if (dialog.entries.empty() || delta == 0)
        return;
    const int last = static_cast<int>(dialog.entries.size()) - 1;
    const int next = std::clamp(static_cast<int>(dialog.browse_first) + delta, 0, last);
    dialog.browse_first = static_cast<std::size_t>(next);
}

void scroll_export_warnings(ExportDialog& dialog, int delta) {
    if (delta == 0)
        return;
    // A laid-out page moves as a set. The span is cleared so a second step
    // before the next layout does not skip another set.
    if ((delta == 1 || delta == -1) && dialog.warning_span > 0) {
        if (delta > 0) {
            if (!dialog.warning_more)
                return;
            dialog.warning_back = dialog.warning_first;
            constexpr std::size_t most = 64;
            const std::size_t first = std::min(dialog.warning_first, most);
            const std::size_t room = most - first;
            dialog.warning_first = first + std::min(dialog.warning_span, room);
        } else {
            dialog.warning_first = dialog.warning_back;
        }
        dialog.warning_span = 0;
        dialog.warning_more = false;
        return;
    }
    constexpr int most = 64;
    const int next = std::clamp(static_cast<int>(dialog.warning_first) + delta, 0, most);
    dialog.warning_first = static_cast<std::size_t>(next);
}

bool export_can_run(const ExportDialog& dialog) {
    if (!dialog.open || dialog.exporting || dialog.done || dialog.browsing ||
        !dialog.problem.empty())
        return false;
    if (dialog.files.empty() || dialog.folder.empty())
        return false;
    for (const ExportFile& file : dialog.files) {
        if (!file.ready || file.unreadable)
            return false;
    }
    return true;
}

uint64_t export_oademo_bytes(const ExportDialog& dialog) {
    uint64_t total = 0;
    for (const ExportFile& file : dialog.files) {
        if (!file.ready)
            return 0;
        total += file.oademo_bytes;
    }
    return total;
}

uint64_t export_tad_bytes(const ExportDialog& dialog) {
    uint64_t total = 0;
    for (const ExportFile& file : dialog.files) {
        if (!file.ready)
            return 0;
        total += dialog.keep_oa_sectors ? file.tad_bytes : file.tad_without_bytes;
    }
    return total;
}

} // namespace oa::ui::library
