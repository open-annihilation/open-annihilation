// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Text drawn with the fonts that travel with the game, through FreeType, the
// same on every platform: one line of UTF-8 at a pixel size and weight,
// drawn as coverage, hinted to whole pixels in one bit per pixel, or
// anti-aliased. Each character comes from the first open font of the stack
// that has it. The base faces are DejaVu Sans Bold, DejaVu Sans, the endonym
// face and Noto Emoji. The endonym face is a cut of Noto Sans CJK that holds
// the languages' own names and the notice shown before a language pack is
// installed. A language pack may add faces while the stack is open. The full
// Simplified Chinese face travels with that pack. The interface faces,
// Barlow and Barlow Condensed, draw OA's own screens in the modern look and
// leave what they lack to the base faces.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::platform::text_font {

/// The fonts of the stack. These are the base faces. Values from face_count
/// up are pack faces (pack_face), added while the stack is open, up to
/// most_pack_faces of them; the interface faces follow (interface_face).
enum class Face : uint8_t {
    dejavu_sans_bold, ///< Latin, Greek, Cyrillic and symbols, bold
    dejavu_sans,      ///< the same scripts with more characters, regular
    endonyms,         ///< languages' own names and the notice before a pack is installed
    noto_emoji,       ///< emoji, drawn in one colour like any character
};

/// How many base fonts the stack holds. Pack faces are counted apart.
inline constexpr std::size_t face_count = 4;

/// The most font faces a language pack may add to an open stack.
inline constexpr std::size_t most_pack_faces = 4;

/// How many interface faces the stack may hold.
inline constexpr std::size_t interface_face_count = 6;

/// Returns the interface face at a place among them: the faces after the
/// last pack face.
///
/// @param index the face's place among the interface faces, from 0 to
///     interface_face_count - 1
/// @return the face
[[nodiscard]] constexpr Face interface_face(std::size_t index) noexcept {
    return static_cast<Face>(face_count + most_pack_faces + index);
}

/// Barlow Regular, the interface family's regular weight.
inline constexpr Face barlow_regular = interface_face(0);
/// Barlow Medium.
inline constexpr Face barlow_medium = interface_face(1);
/// Barlow SemiBold.
inline constexpr Face barlow_semibold = interface_face(2);
/// Barlow Bold.
inline constexpr Face barlow_bold = interface_face(3);
/// Barlow Condensed SemiBold, the condensed family's lighter weight.
inline constexpr Face barlow_condensed_semibold = interface_face(4);
/// Barlow Condensed Bold.
inline constexpr Face barlow_condensed_bold = interface_face(5);

/// How a pack's face is drawn.
enum class FaceRole : uint8_t {
    ideographs, ///< drawn as Noto Sans CJK is, at the style's least size
    letters,    ///< drawn at the sans faces' size
};

/// Returns the pack face added at an index.
///
/// face_count keeps meaning the base faces. The first pack face is
/// face_count, the next one past it, and so on up to most_pack_faces.
///
/// @param index the face's place among the pack faces, from 0
/// @return the face
[[nodiscard]] constexpr Face pack_face(std::size_t index) noexcept {
    return static_cast<Face>(face_count + index);
}

/// Tells whether open requires a base face's file.
///
/// Every base face is required. A missing file fails open.
///
/// @param face a face
/// @return true for a base face
[[nodiscard]] constexpr bool face_required(Face face) noexcept {
    return static_cast<std::size_t>(face) < face_count;
}

/// The file of each font in the fonts folder, by Face.
inline constexpr std::array<std::string_view, face_count> face_files{
    "DejaVuSans-Bold.ttf",
    "DejaVuSans.ttf",
    "NotoSansCJKsc-Bold-Endonyms.otf",
    "NotoEmoji.ttf",
};

/// The file of each interface face in the fonts folder, in the order of
/// interface_face.
inline constexpr std::array<std::string_view, interface_face_count> interface_face_files{
    "Barlow-Regular.ttf",
    "Barlow-Medium.ttf",
    "Barlow-SemiBold.ttf",
    "Barlow-Bold.ttf",
    "BarlowCondensed-SemiBold.ttf",
    "BarlowCondensed-Bold.ttf",
};

/// Tells whether a face is one of the interface faces.
///
/// @param face a face
/// @return true from barlow_regular to the last interface face
[[nodiscard]] constexpr bool is_interface_face(Face face) noexcept {
    const auto index = static_cast<std::size_t>(face);
    const auto first = static_cast<std::size_t>(interface_face(0));
    return index >= first && index < first + interface_face_count;
}

/// The name of the folder beside the game that holds the fonts.
inline constexpr std::string_view font_folder = "fonts";

/// The largest pixel size a draw accepts.
inline constexpr int32_t max_pixel_size = 256;
/// The longest text, in UTF-8 bytes, a draw accepts.
inline constexpr std::size_t max_text_bytes = 4096;
/// The widest line, in pixels, a draw gives; the rest of a longer one is cut off.
inline constexpr int32_t max_line_width = 8192;
/// The most pixels of letter spacing a style may add after each character.
inline constexpr int32_t max_letter_spacing = 16;

/// The weight a line is drawn in.
enum class Weight : uint8_t {
    regular, ///< DejaVu Sans first; Noto Emoji at its regular weight
    bold,    ///< DejaVu Sans Bold first, then DejaVu Sans for what it lacks
    /// between the two: Barlow Medium in the interface family; the sans
    /// faces draw it as regular
    medium,
    /// Barlow SemiBold in the interface family; the sans faces draw it as bold
    semibold,
};

/// The faces a line looks a character up in first.
enum class Family : uint8_t {
    /// DejaVu Sans and DejaVu Sans Bold: game text, and the characters the
    /// game's fonts lack
    sans,
    /// Barlow at the line's weight, then the sans family: OA's own screens
    /// in the modern look
    interface,
    /// Barlow Condensed, SemiBold for a regular, medium or semibold line and
    /// Bold for a bold one, then the sans family: the modern look's window
    /// titles and section headings
    interface_condensed,
};

/// How glyphs become pixels.
enum class Rendering : uint8_t {
    mono,        ///< hinted to whole pixels, one bit per pixel: coverage 0 or 255
    antialiased, ///< hinted, 256 levels of coverage
};

/// How a line is drawn.
struct Style {
    /// pixels per em of the DejaVu faces, 1..max_pixel_size; the endonym
    /// face and Noto Emoji are drawn at related_pixel_size of it
    int32_t pixel_size{14};
    Weight weight{Weight::bold};
    Rendering rendering{Rendering::mono};
    /// pixels added after each character's advance, 0..max_letter_spacing
    int32_t letter_spacing{};
    /// the least pixels per em an ideograph-sized face is drawn at,
    /// 0..max_pixel_size: smaller ideographs fill in; 0 leaves it at
    /// related_pixel_size
    int32_t least_cjk_pixel_size{};
    /// the faces looked in first; an interface family draws at pixel_size
    /// too, and a line holding a letter beyond ASCII its face lacks is drawn
    /// whole in the sans family, so that no word changes typeface part way
    Family family{Family::sans};
};

/// The rows of a line in one style, which fit every font of the stack.
struct LineMetrics {
    int32_t ascent{};  ///< rows above the baseline
    int32_t descent{}; ///< rows below the baseline, the baseline's own row among them
};

/// The rows of one face at the pixel size a style draws it at.
struct FaceMetrics {
    int32_t pixel_size{}; ///< pixels per em of this face
    int32_t ascent{};     ///< rows above the baseline
    int32_t descent{};    ///< rows below the baseline, the baseline's own row among them
};

/// Pixels per em of the bold sans face beside the message log's font, at
/// the game fonts' size and a scale of 1.
inline constexpr int32_t message_log_pixel_size = 14;
/// Pixels per em of the bold sans face beside the status readouts' font.
inline constexpr int32_t status_readout_pixel_size = 11;
/// Pixels per em of the regular sans face beside the labels' and the chat
/// line's font.
inline constexpr int32_t label_pixel_size = 11;
/// The least pixels per em of an ideograph-sized face while a Chinese,
/// Japanese or Korean language is shown, so its strokes do not fill in.
inline constexpr int32_t least_cjk_language_pixel_size = 12;

/// One drawn line: how much of each pixel the text covers.
struct Coverage {
    int32_t width{};  ///< columns
    int32_t height{}; ///< rows
    /// the row of the baseline: the line's ascent, or more when a glyph
    /// rises above it
    int32_t baseline{};
    /// the column the pen starts at: 0, or more when a glyph reaches left
    /// of the pen
    int32_t origin{};
    int32_t advance{}; ///< pixels the pen moved, letter spacing included
    /// width * height bytes, top row first: 0 is untouched, 255 fully
    /// covered; a mono line holds no other value
    std::vector<uint8_t> alpha{};
};

/// One character of a line: the font that draws it and where.
struct Placement {
    char32_t character{};
    Face face{};
    int32_t pen{};     ///< the pen's column, counted from the line's start
    int32_t advance{}; ///< pixels the pen moves past it, letter spacing included
};

/// Gives the base faces a weight looks a character up in, in that order.
///
/// This is the base chain, without a pack's faces. Bold and semibold look
/// in DejaVu Sans Bold first, then DejaVu Sans for what it lacks. Regular
/// and medium look in DejaVu Sans first. Both then look in the endonym face
/// and Noto Emoji. FontStack::chain gives the faces one open stack looks
/// in, pack faces and interface faces included. The span lives as long as
/// the program.
///
/// @param weight the weight of the line
/// @return the base faces, the first that has a character drawing it
[[nodiscard]] std::span<const Face> fallback_chain(Weight weight) noexcept;

/// Gives the pixel size the endonym face and Noto Emoji are drawn at next
/// to the DejaVu faces at a size.
///
/// Ideographs stand about as tall as DejaVu's capitals with one or two
/// rows more, as they do at 12 px beside the 14-px log font: six sevenths
/// of the size, rounded, but no less than the size itself or 12, whichever
/// is smaller.
///
/// @param pixel_size the DejaVu faces' pixels per em
/// @return the ideograph and emoji pixels per em
[[nodiscard]] int32_t related_pixel_size(int32_t pixel_size) noexcept;

/// Decodes UTF-8 text.
///
/// @param text the bytes
/// @return the code points; empty for bytes that are not UTF-8: a stray or
///         missing continuation byte, an overlong form, a surrogate or a
///         value above U+10FFFF
[[nodiscard]] std::optional<std::vector<char32_t>> decode_utf8(std::string_view text);

/// Tells whether a character takes no room and draws nothing: a control
/// character, a variation selector, a zero-width space or joiner, or the
/// byte-order mark.
///
/// @param character the code point
/// @return true when the line passes over it
[[nodiscard]] bool is_invisible(char32_t character) noexcept;

/// Gives the release of FreeType the stack draws with.
///
/// @return the release, such as "2.14.3"
[[nodiscard]] std::string freetype_version();

/// Gives the folder the game's fonts travel in: the fonts folder of the
/// application bundle's resources on macOS, else the fonts folder beside
/// the executable.
///
/// @return the folder; empty when the executable's own path is unknown
[[nodiscard]] std::filesystem::path bundled_font_directory();

/// Tells whether a font file opened for drawing would stay open in a program
/// this one starts.
///
/// Windows will not rename the fonts folder while a file in it is open in
/// any program. A font file is opened so that does not happen. A file that
/// cannot be opened reports false, as does every system but Windows, and a
/// Windows that cannot say.
///
/// @param path the font file
/// @return true when a program this one starts would still hold the file
[[nodiscard]] bool font_file_reaches_a_started_program(const std::filesystem::path& path);

/// The fonts of the stack, opened from one folder, with the glyphs drawn so
/// far kept for the lines that draw them again.
///
/// The fonts are read from their files as glyphs are needed. One thread at a
/// time uses a stack.
class FontStack {
  public:

    /// Opens the stack's base fonts and its interface faces.
    ///
    /// Every base face is required (face_required). A missing file, a file
    /// FreeType cannot read, or a file that is not scalable fails the open.
    /// Each interface face (interface_face_files) opens when its file is
    /// there and is skipped otherwise: an interface family whose face did
    /// not open draws in the sans family.
    ///
    /// @param directory the folder that holds the files of face_files
    /// @return the stack; null when a required file is missing or FreeType
    ///         cannot read a file that is there
    [[nodiscard]] static std::unique_ptr<FontStack> open(const std::filesystem::path& directory);

    /// Adds a pack face, after the ones added already.
    ///
    /// @param file the font file
    /// @param role how the face is drawn
    /// @return false, changing nothing, when the stack already holds
    ///     most_pack_faces, or the file is missing, unreadable or not a
    ///     scalable font; true when the face was added
    [[nodiscard]] bool add_face(const std::filesystem::path& file, FaceRole role);

    /// Drops every pack face and every glyph drawn from one.
    ///
    /// The base faces stay. A face added afterwards may reuse a pack face's
    /// index; the glyphs of the face that left are not drawn for it.
    void remove_pack_faces() noexcept;

    /// Returns how many pack faces the stack holds.
    ///
    /// @return the count, at most most_pack_faces
    [[nodiscard]] std::size_t pack_face_count() const noexcept;

    /// Tells whether a face is open.
    ///
    /// @param face a base face or a pack face
    /// @return true when the stack opened it
    [[nodiscard]] bool has_face(Face face) const noexcept;

    /// Gives the faces a weight looks a character up in, in that order.
    ///
    /// Bold: DejaVu Sans Bold, DejaVu Sans, the letters pack faces in the
    /// order they were added, the ideographs pack faces in that order, the
    /// endonym face, then Noto Emoji. Semibold is the same. Regular and
    /// medium are the same without DejaVu Sans Bold. An interface family
    /// puts its face for the weight first (Family). A face that is not open
    /// is left out of the chain, and lookup skips one that is closed.
    ///
    /// @param weight the weight of the line
    /// @param family the faces looked in first
    /// @return the faces
    [[nodiscard]] std::vector<Face> chain(Weight weight, Family family = Family::sans) const;

    /// Tells whether FreeType opens a file as a scalable font.
    ///
    /// @param file the file
    /// @return true when it opens and is scalable
    [[nodiscard]] static bool face_file_opens(const std::filesystem::path& file);

    /// Closes the fonts and the FreeType library they were opened with.
    ~FontStack();
    FontStack(const FontStack&) = delete;
    FontStack& operator=(const FontStack&) = delete;

    /// Finds the font that draws a character.
    ///
    /// @param character the code point
    /// @param weight the weight of the line
    /// @param family the faces looked in first
    /// @return the first open font of the stack's chain for the weight that
    ///         has the character, else that chain's first open font, which
    ///         draws its missing-glyph box; empty for a character
    ///         is_invisible names
    [[nodiscard]] std::optional<Face>
    face_for(char32_t character, Weight weight, Family family = Family::sans) const;

    /// Tells whether the stack can draw a text in a weight with no
    /// missing character.
    ///
    /// True when the text is UTF-8 and every character that takes room is
    /// held by an open face of the stack's chain for the weight. A caller
    /// uses it to choose between a language's own name and its English
    /// name. A character that draws nothing is passed over.
    ///
    /// @param text the bytes
    /// @param weight the weight of the line
    /// @param family the faces looked in first
    /// @return false when the text is not UTF-8, or a visible character is
    ///     in no face of the chain
    [[nodiscard]] bool
    draws(std::string_view text, Weight weight, Family family = Family::sans) const;

    /// Gives the family a line is drawn in, as layout and draw choose it:
    /// the style's, or the sans family when the line holds a letter beyond
    /// ASCII its family's face lacks, or that face is not open. A caller
    /// that draws a line a character at a time draws each in this family.
    ///
    /// @param text UTF-8 text
    /// @param style the style
    /// @return the family; the sans family for text that is not UTF-8
    [[nodiscard]] Family line_family(std::string_view text, const Style& style) const;

    /// Gives the rows of a line in a style.
    ///
    /// The rows are the greatest ascent and descent of the open base faces
    /// at the size the style draws them, and of an interface family's face
    /// for the weight. A pack's faces never change them.
    /// The endonym face is cut from Noto Sans CJK and keeps those rows, so
    /// a line beside the 14 px bold face has 14 above the baseline and 4
    /// below.
    ///
    /// @param style the style; its pixel size must be 1..max_pixel_size
    /// @return the rows; empty for a size out of range
    [[nodiscard]] std::optional<LineMetrics> metrics(const Style& style);

    /// Gives the rows of one face at the pixel size a style draws it at.
    ///
    /// The sans faces, the interface faces and letters pack faces take the
    /// style's pixel size. The endonym face and ideographs pack faces take
    /// related_pixel_size
    /// of it, and no less than the style's least size. Noto Emoji takes
    /// related_pixel_size and is set to the style's weight.
    ///
    /// @param face the face
    /// @param style the style; its pixel size must be 1..max_pixel_size
    /// @return the face's size and rows; empty for a size out of range or a
    ///         face that is not open
    [[nodiscard]] std::optional<FaceMetrics> face_metrics(Face face, const Style& style);

    /// Lays a line out without drawing it.
    ///
    /// @param text UTF-8 text, at most max_text_bytes
    /// @param style the style
    /// @return each visible character with its font and pen position; empty
    ///         for text that is not UTF-8 or too long, or a style out of range
    [[nodiscard]] std::optional<std::vector<Placement>>
    layout(std::string_view text, const Style& style);

    /// Draws a line.
    ///
    /// Each character is drawn at the pen, which starts at origin and moves
    /// by the character's advance, rounded to a whole pixel, and the
    /// style's letter spacing. Characters are not shaped, kerned or
    /// reordered. Where two glyphs cover one pixel, the greater coverage is
    /// kept.
    ///
    /// @param text UTF-8 text, at most max_text_bytes
    /// @param style the style
    /// @return the coverage, at least the line's ascent and descent tall;
    ///         empty for text that is not UTF-8 or too long, or a style out
    ///         of range
    [[nodiscard]] std::optional<Coverage> draw(std::string_view text, const Style& style);

    /// Tells how many drawn glyphs the stack keeps.
    ///
    /// @return glyphs kept, at most kept_glyphs
    [[nodiscard]] std::size_t cached_glyphs() const noexcept;

    /// Tells how many glyphs the stack has drawn since it opened, each
    /// counted again when it is drawn again after the store forgot it.
    ///
    /// @return glyphs drawn
    [[nodiscard]] std::size_t drawn_glyphs() const noexcept;

    /// The most drawn glyphs a stack keeps; past it, the glyph used longest
    /// ago is forgotten first.
    static constexpr std::size_t kept_glyphs = 4096;

  private:

    struct Fonts;
    /// Takes the opened fonts (open makes them).
    ///
    /// @param fonts the FreeType library, the faces and the glyph store
    explicit FontStack(std::unique_ptr<Fonts> fonts) noexcept;
    std::unique_ptr<Fonts> fonts_{};
};

} // namespace oa::platform::text_font
