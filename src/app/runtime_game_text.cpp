// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Game text in the modern fonts: the bundled fonts, opened once and shared,
// the lines they draw at the text size, kept for the frames that draw them
// again, the game-text hooks the text loops reach them and the player's
// settings through, how large the match's text is painted where it lies,
// and lines painted on the match's RGB layer, reduced to the palette, their
// shadow over the Full tier's overlay canvas left to the card.

#include "oa/app/runtime.hpp"
#include "oa/app/view_rules.hpp"

#include "oa/platform/text_font.hpp"
#include "oa/base/threads.hpp"
#include "oa/present/game_text.hpp"

#include <algorithm>
#include <cstddef>
#include <list>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace oa::app {

namespace {

namespace text_font = oa::platform::text_font;

/// The most drawn lines kept; past it, the line drawn again longest ago is
/// forgotten first.
constexpr std::size_t kept_lines = 512;

/// The characters drawn ahead into the glyph store when a Chinese language
/// comes to be shown, so that its first screen draws few new glyphs on one
/// core: the 360 hanzi the interface's and the game's texts use most, and
/// the CJK punctuation.
constexpr std::string_view common_cjk_characters =
    "级机高能单甲战位产雷备生地水弹型的炮初量火空和导击器重建车子力工程动金属面配攻光"
    "达大防舰可装下开射形气垫试对厂验选存上激储反速一造中核用游戏海取者斗等消自坦克集"
    "发小潜两式体台离关箭飞无电定修超有栖度择置采只设船炸侦时平城指场艇敌声标加闭视制"
    "移坞换隐以打究范围载图输目要扰转运回音长方筑页部出卫复理鱼墙干塔难利作快所令武保"
    "限人队站门雾返于纳确死农当轰止许固透测深护轻极前全较坚家野信停控在统伤描述觉始源"
    "玩挥过原蛛强个太阳具环塞事害种模明息应内井系陆行增读任项阵主收间默普通使到辆爆成"
    "少非御观军名必杀特为脉远直合联禁允灭剩乐效认操冲向并巨将截查记录黑失屏入役恢命枪"
    "神头手拦弱菜风族胜随实新守巡星短堆突清略被官道助真圆简般困界象共享称资毁距供之眼"
    "像龙航升或察兵务亡放预后状态驱除没四活占领密去致切需耗聚变底垂摄闪虫引母蛇送潮汐"
    "，。、：；？！（）《》「」…—";

/// Tells whether a language is written in Chinese, Japanese or Korean.
///
/// @param language the language
/// @return true for the tags zh, ja and ko and theirs
bool writes_cjk(const oa::data::languages::Language& language) {
    const std::string_view tag = language.tag;
    for (const std::string_view primary : {"zh", "ja", "ko"})
        if (tag.substr(0, 2) == primary && (tag.size() == 2 || tag[2] == '-'))
            return true;
    return false;
}

/// The pixel size and weight of each face at a scale of 1 and the game
/// fonts' size. DejaVu Sans Bold at message_log_pixel_size stands as tall as
/// hattfont12, Bold at status_readout_pixel_size as hattfont11, and DejaVu
/// Sans at label_pixel_size matches CONSOLE.FNT's x-height.
struct FaceSize {
    int32_t pixel_size{};
    text_font::Weight weight{};
};

constexpr FaceSize message_face{text_font::message_log_pixel_size, text_font::Weight::bold};
constexpr FaceSize status_face{text_font::status_readout_pixel_size, text_font::Weight::bold};
constexpr FaceSize label_face{text_font::label_pixel_size, text_font::Weight::regular};

/// A drawn line in the store, with its place in the order of use.
struct KeptLine {
    std::shared_ptr<const oa::present::TextMask> mask{}; ///< null when it could not be drawn
    std::list<std::string>::iterator place{};
};

/// The bundled fonts, opened on first use, and the lines they drew, by
/// face, scale and text; a null line is one they could not draw.
struct ModernFonts {
    base::threads::Mutex mutex{};
    bool opened{};
    std::unique_ptr<text_font::FontStack> stack{};
    std::unordered_map<std::string, KeptLine> lines{};
    /// the keys of the kept lines, the one drawn last first
    std::list<std::string> order{};
    /// the language the lines were drawn for; null before the first line
    const oa::data::languages::Language* language{};
    /// the least pixel size of ideographs for that language, 0 for none
    int32_t least_cjk_size{};
};

ModernFonts& modern_fonts() {
    static ModernFonts fonts;
    return fonts;
}

/// Opens the fonts the first time they are asked for; null when they cannot be.
text_font::FontStack* opened_stack(ModernFonts& fonts) {
    if (!fonts.opened) {
        fonts.opened = true;
        try {
            fonts.stack = text_font::FontStack::open(text_font::bundled_font_directory());
        } catch (const std::exception&) {
            fonts.stack.reset();
        }
    }
    return fonts.stack.get();
}

/// The style a face is drawn in at a scale and text size: hinted to whole
/// pixels, one bit a pixel, as crisp as the game's own fonts at every size.
text_font::Style
face_style(oa::present::TextFace face, int32_t scale, int32_t text_size, int32_t least_cjk_size) {
    const FaceSize size = face == oa::present::TextFace::message  ? message_face
                          : face == oa::present::TextFace::status ? status_face
                                                                  : label_face;
    text_font::Style style;
    style.pixel_size = std::clamp(
        oa::present::face_pixel_size(size.pixel_size, scale, text_size),
        1,
        text_font::max_pixel_size
    );
    style.weight = size.weight;
    style.rendering = text_font::Rendering::mono;
    style.least_cjk_pixel_size = least_cjk_size;
    return style;
}

/// Draws the common CJK characters into the glyph store in each face at a
/// scale and text size.
void warm_glyphs(ModernFonts& fonts, int32_t scale, int32_t text_size) {
    auto* stack = opened_stack(fonts);
    if (stack == nullptr)
        return;
    for (const auto face :
         {oa::present::TextFace::message,
          oa::present::TextFace::status,
          oa::present::TextFace::label})
        std::ignore = stack->layout(
            common_cjk_characters, face_style(face, scale, text_size, fonts.least_cjk_size)
        );
}

/// Readies the fonts for the language shown when it is not the one their
/// lines were drawn for: the least size of ideographs for it, no lines kept
/// from before, and the common characters drawn ahead for a Chinese,
/// Japanese or Korean language.
void follow_language(
    ModernFonts& fonts,
    const oa::data::languages::Language& language,
    int32_t scale,
    int32_t text_size
) {
    if (fonts.language == &language)
        return;
    fonts.language = &language;
    fonts.least_cjk_size = writes_cjk(language) ? text_font::least_cjk_language_pixel_size : 0;
    fonts.lines.clear();
    fonts.order.clear();
    if (fonts.least_cjk_size != 0)
        warm_glyphs(fonts, scale, text_size);
}

/// Draws a line in the stack; null when it cannot.
std::shared_ptr<const oa::present::TextMask>
draw_mask(text_font::FontStack& stack, std::string_view text, const text_font::Style& style) {
    const auto drawn = stack.draw(text, style);
    const auto placements = stack.layout(text, style);
    const auto characters = text_font::decode_utf8(text);
    if (!drawn || !placements || !characters)
        return nullptr;
    auto mask = std::make_shared<oa::present::TextMask>();
    mask->width = drawn->width;
    mask->height = drawn->height;
    mask->baseline = drawn->baseline;
    mask->origin = drawn->origin;
    mask->advance = drawn->advance;
    mask->alpha = drawn->alpha;
    // The placements are the visible characters'; the others leave the pen.
    std::size_t placed = 0;
    int32_t pen = 0;
    for (const char32_t character : *characters) {
        if (!text_font::is_invisible(character) && placed < placements->size()) {
            const auto& placement = (*placements)[placed++];
            pen = placement.pen + placement.advance;
        }
        mask->character_ends.push_back(pen);
    }
    return mask;
}

} // namespace

GameTextHooksInstall::~GameTextHooksInstall() {
    if (runtime != nullptr && oa::present::game_text_hooks().context == runtime)
        oa::present::set_game_text_hooks({});
}

bool Runtime::modern_fonts_open() {
    auto& fonts = modern_fonts();
    const base::threads::LockGuard lock(fonts.mutex);
    return opened_stack(fonts) != nullptr;
}

void Runtime::warm_game_text() {
    const int32_t size = oa::present::game_text_size();
    auto& fonts = modern_fonts();
    const base::threads::LockGuard lock(fonts.mutex);
    fonts.language = nullptr;
    follow_language(fonts, shown_language(), 1, size);
}

bool Runtime::game_text_utf8() const {
    // A language drawn in the modern fonts holds its text in UTF-8, and
    // chat in UTF-8 is read so too.
    return unicode_chat_on() || language_needs_modern_fonts();
}

oa::present::TextSettings Runtime::game_text_settings() const {
    oa::present::TextSettings settings;
    settings.style = text_style();
    settings.style.modern_fonts = settings.style.modern_fonts && modern_fonts_open();
    settings.utf8 = game_text_utf8();
    return settings;
}

std::string Runtime::typed_game_text(std::string_view typed) const {
    return oa::present::encode_game_text(typed, game_text_utf8());
}

Runtime::PanelText::PanelText(Runtime& owner) noexcept
    : runtime(&owner), kept(owner.text_place_), kept_top_row(owner.panel_top_row_) {
    owner.text_place_ = TextPlace::panel;
}

Runtime::PanelText::PanelText(Runtime& owner, int top_row) noexcept : PanelText(owner) {
    owner.panel_top_row_ = top_row;
}

Runtime::PanelText::~PanelText() {
    runtime->text_place_ = kept;
    runtime->panel_top_row_ = kept_top_row;
}

int32_t Runtime::painted_text_size(const oa::present::TextRun& run) const noexcept {
    // A panel is laid out for the game's fonts: its text never grows past
    // them.
    if (text_place_ == TextPlace::panel)
        return std::min(run.size, oa::present::game_font_text_size);
    return run.size;
}

int32_t Runtime::painted_baseline(int32_t font_baseline, int32_t size) const noexcept {
    // Over the battlefield a line's top stays at the pen and its baseline
    // moves with the size; in a panel the text keeps the font's baseline.
    if (text_place_ == TextPlace::panel)
        return font_baseline;
    return oa::present::sized_length(font_baseline, size);
}

int Runtime::paint_modern_text(
    const oa::present::TextLayers& layers, int x, int baseline_y, std::array<uint8_t, 3> color
) {
    auto& dest = paint_target();
    const bool has_palette = match_palette_ != oa::PaletteBytes{};
    auto canvas = oa::present::rgb_canvas(
        dest.rgb,
        static_cast<int32_t>(dest.width),
        static_cast<int32_t>(dest.height),
        has_palette ? std::span<const uint8_t>(match_palette_) : std::span<const uint8_t>{}
    );
    // The Full tier's overlay canvas holds no world: over it the card
    // darkens the world under the shadow at the shadow's alpha, holds it to
    // the outline grey at the most under the outline, which is black over
    // black ground and the grey over ground lighter than it, and blends
    // the letters' colour over it where they cover a pixel in part, each
    // at its share in 256ths.
    if (paints_full_canvas()) {
        canvas.see_through = full_overlay_key();
        canvas.world_user = this;
        canvas.world_run = [](void* user, const oa::present::TextWorldRun& run) {
            auto& runtime = *static_cast<Runtime*>(user);
            switch (run.layer) {
            case oa::present::TextWorldLayer::shadow: {
                constexpr uint32_t shadow_opacity =
                    (uint32_t{oa::present::text_shadow_alpha} * 256U + 127U) / 255U;
                std::ignore = runtime.paint_world_blend(
                    run.x, run.y, run.width, 1, oa::present::text_shade_color, shadow_opacity
                );
                break;
            }
            case oa::present::TextWorldLayer::outline:
                std::ignore = runtime.paint_world_minimum(
                    run.x, run.y, run.width, 1, oa::present::text_outline_color
                );
                break;
            case oa::present::TextWorldLayer::letter:
                std::ignore = runtime.paint_world_blend(
                    run.x,
                    run.y,
                    run.width,
                    1,
                    run.color,
                    (uint32_t{run.coverage} * 256U + 127U) / 255U
                );
                break;
            }
        };
    }
    oa::present::lay_text(canvas, layers, x, baseline_y, color);
    return layers.advance;
}

void Runtime::install_game_text_hooks() {
    oa::present::GameTextHooks hooks{};
    hooks.context = this;
    hooks.settings = [](void* context) {
        return static_cast<const Runtime*>(context)->game_text_settings();
    };
    hooks.draw = [](void* context,
                    std::string_view text,
                    oa::present::TextFace face,
                    int32_t scale,
                    int32_t size) -> std::shared_ptr<const oa::present::TextMask> {
        if (text.empty() || text.size() > text_font::max_text_bytes)
            return nullptr;
        const auto& language = static_cast<const Runtime*>(context)->shown_language();
        auto& fonts = modern_fonts();
        const base::threads::LockGuard lock(fonts.mutex);
        follow_language(fonts, language, scale, size);
        const auto style = face_style(face, scale, size, fonts.least_cjk_size);
        std::string key;
        key.push_back(static_cast<char>(face));
        key += std::to_string(style.pixel_size);
        key.push_back('\0');
        key.append(text);
        if (const auto found = fonts.lines.find(key); found != fonts.lines.end()) {
            fonts.order.splice(fonts.order.begin(), fonts.order, found->second.place);
            return found->second.mask;
        }
        auto* stack = opened_stack(fonts);
        if (stack == nullptr)
            return nullptr;
        auto mask = draw_mask(*stack, text, style);
        while (!fonts.order.empty() && fonts.lines.size() >= kept_lines) {
            fonts.lines.erase(fonts.order.back());
            fonts.order.pop_back();
        }
        fonts.order.push_front(key);
        fonts.lines.emplace(std::move(key), KeptLine{mask, fonts.order.begin()});
        return mask;
    };
    // A match draws in its palette, the frontend in its screen's.
    hooks.palette = [](void* context) -> std::span<const uint8_t> {
        const auto& runtime = *static_cast<const Runtime*>(context);
        const oa::PaletteBytes& palette =
            runtime.screen_ == Screen::match ? runtime.match_palette_ : runtime.screen_palette();
        return palette;
    };
    oa::present::set_game_text_hooks(hooks);
    game_text_hooks_.runtime = this;
}

} // namespace oa::app
