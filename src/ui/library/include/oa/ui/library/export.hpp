// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Export dialog over the Library's Replays section. It draws nothing and
// reads no file: the app fills one offer, and the screen shows it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oa::ui::library {

struct Library;

/// Which file an export writes.
enum class ExportFormat : uint8_t {
    oademo, ///< an Open Annihilation replay, the one offered first
    tad,    ///< a TA Demo Recorder recording
};

/// One replay the dialog can write, and what the file will hold.
struct ExportFile {
    /// The replay's path below Recordings.
    std::string path{};
    /// The name the file is given, before it is made safe for a folder.
    std::string title{};
    /// Bytes a `.oademo` of this replay will be.
    uint64_t oademo_bytes{};
    /// Bytes a `.tad` that keeps Open Annihilation's own sectors will be.
    uint64_t tad_bytes{};
    /// Bytes a `.tad` that leaves those sectors out will be.
    uint64_t tad_without_bytes{};
    /// The sizes and the warnings were read. A file that was not is not written.
    bool ready{};
    /// A map name or a player name is longer than a `.tad` keeps.
    bool names_cut{};
    /// A player's status datagram is longer than a `.tad` keeps.
    bool status_cut{};
    /// A packet holds one of this mod's wide weapon records.
    bool wide_records{};
    /// The replay was played with mutators.
    bool mutators{};
    /// Those mutators' ids, when the recording names them.
    std::vector<std::string> mutator_ids{};
    /// A display name for each id, from the replay's content note. Empty when
    /// the note names none; the installed name is preferred when the Library
    /// has one.
    std::vector<std::string> mutator_names{};
    /// The replay could not be read, so it cannot be exported.
    bool unreadable{};
};

/// One folder the chooser lists.
struct ExportFolder {
    /// The folder's name, as the row shows it.
    std::string name{};
    /// What the folder is, as the locator describes a folder.
    std::string what{};
    /// An absolute path, for a place that is not a child of the listed folder.
    /// Empty for a child, whose name is joined onto the listed folder.
    std::string path{};
};

/// What the app gives the dialog when it opens.
struct ExportOffer {
    /// The replays. Empty is the dialog with nothing selected.
    std::vector<ExportFile> files{};
    /// The folder the files are written into.
    std::string folder{};
    /// The folder as the dialog shows it when it is not the Downloads folder.
    std::string folder_label{};
    /// The folder is the system's Downloads folder, shown by that word.
    bool folder_is_downloads{};
    /// Why nothing can be written, already looked up. Empty when the offer stands.
    std::string problem{};
};

/// The Export dialog.
struct ExportDialog {
    /// The dialog is open over the Library.
    bool open{};
    /// The replays it writes, one file each.
    std::vector<ExportFile> files{};
    /// Which file. A `.oademo` until the player chooses otherwise.
    ExportFormat format{ExportFormat::oademo};
    /// A `.tad` keeps Open Annihilation's own sectors. A `.oademo` always does.
    bool keep_oa_sectors{true};
    /// The folder the files are written into.
    std::string folder{};
    /// That folder as text, when it is not shown as Downloads.
    std::string folder_label{};
    /// `folder` is the system's Downloads folder.
    bool folder_is_downloads{};
    /// A file is being written.
    bool exporting{};
    /// Files written so far.
    uint32_t progress_done{};
    /// Files the export was asked to write.
    uint32_t progress_total{};
    /// Recording bytes taken so far, while one file is measured or compressed.
    uint64_t progress_bytes_done{};
    /// Recording bytes that file holds.
    uint64_t progress_bytes_total{};
    /// The byte progress is a compression, not a count of files.
    bool compressing{};
    /// The first compatibility warning the page shows.
    std::size_t warning_first{};
    /// How many warnings the last laid-out page showed. 0 until the form is
    /// laid out. The layout takes a const Library, and a page's size is known
    /// only then.
    mutable std::size_t warning_span{};
    /// The first warning of the page before this one. On the first page it is
    /// `warning_first`.
    mutable std::size_t warning_back{};
    /// A later page of warnings exists.
    mutable bool warning_more{};
    /// Every file was written.
    bool done{};
    /// Why the export cannot be written, already looked up. Empty when it can.
    std::string problem{};
    /// The chooser is open in place of the form.
    bool browsing{};
    /// The folder the chooser lists.
    std::string browse_folder{};
    /// The folders in it, in the order shown.
    std::vector<ExportFolder> entries{};
    /// The first folder the page shows.
    std::size_t browse_first{};
};

/// Opens the dialog on an offer. A `.oademo` is selected, and Open
/// Annihilation's own sectors are kept.
///
/// @param[in,out] library the Library
/// @param offer the replays, the folder and a problem when one is already known
void open_export(Library& library, ExportOffer offer);

/// Closes the dialog.
///
/// @param[in,out] library the Library
void close_export(Library& library);

/// Chooses which file the export writes.
///
/// @param[in,out] library the Library
/// @param format a `.oademo` or a `.tad`
void set_export_format(Library& library, ExportFormat format);

/// Keeps or leaves out Open Annihilation's own sectors in a `.tad`.
///
/// @param[in,out] library the Library
/// @param keep true to keep them
void set_export_oa_sectors(Library& library, bool keep);

/// Sets the folder the files are written into.
///
/// @param[in,out] library the Library
/// @param folder the folder
/// @param label the folder as text, when it is not Downloads
/// @param downloads the folder is the system's Downloads folder
///
/// A problem from a write into the previous folder is cleared, so Export can
/// be pressed again.
void set_export_folder(Library& library, std::string folder, std::string label, bool downloads);

/// Says how far a write has come.
///
/// @param[in,out] library the Library
/// @param done files written so far
/// @param total files asked for
void set_export_progress(Library& library, uint32_t done, uint32_t total);

/// Says how far one recording's compression has come.
///
/// Does not by itself say that a file is being written. The form shows it
/// while a size is still being measured, and the exporting state shows it
/// for one file.
///
/// @param[in,out] library the Library
/// @param done recording bytes taken
/// @param total recording bytes
/// @param compressing the bytes are a compression
void set_export_byte_progress(Library& library, uint64_t done, uint64_t total, bool compressing);

/// Says the files were written.
///
/// @param[in,out] library the Library
void finish_export(Library& library);

/// Says the export could not be written. `problem` is already looked up.
///
/// @param[in,out] library the Library
/// @param problem the sentence
void fail_export(Library& library, std::string problem);

/// Opens the chooser on a folder's own folders.
///
/// @param[in,out] library the Library
/// @param folder the folder listed
/// @param entries its folders, in the order shown
void open_export_browse(Library& library, std::string folder, std::vector<ExportFolder> entries);

/// Closes the chooser and returns to the form.
///
/// @param[in,out] library the Library
void close_export_browse(Library& library);

/// Moves the chooser's page by `delta` folders.
///
/// @param[in,out] dialog the dialog
/// @param delta how many folders, positive down the list
void scroll_export_browse(ExportDialog& dialog, int delta);

/// Moves the warning page.
///
/// After the form is laid out, a step of one moves by the whole set that page
/// showed. Any other step, and a step of one before a layout, moves by that
/// many warnings. The place stays between the first warning and the 64th.
///
/// @param[in,out] dialog the dialog
/// @param delta how many warnings, positive down the list, or one page when
///        it is 1 or -1 and the form has been laid out
void scroll_export_warnings(ExportDialog& dialog, int delta);

/// Tells whether Export takes a press.
///
/// @param dialog the dialog
/// @return true when every replay is ready and can be read, a folder is chosen,
///         nothing is being written and the last write did not fail
[[nodiscard]] bool export_can_run(const ExportDialog& dialog);

/// Returns the bytes a `.oademo` of every ready replay will be.
///
/// @param dialog the dialog
/// @return the total, or 0 when a replay is not ready
[[nodiscard]] uint64_t export_oademo_bytes(const ExportDialog& dialog);

/// Returns the bytes a `.tad` of every ready replay will be, for the sectors box as it stands.
///
/// @param dialog the dialog
/// @return the total, or 0 when a replay is not ready
[[nodiscard]] uint64_t export_tad_bytes(const ExportDialog& dialog);

} // namespace oa::ui::library
