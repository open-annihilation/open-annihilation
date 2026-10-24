// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain picker on the OA layer. open_brain_picker pushes it, modal over
// the screen that asked, and hands that seat's choice back on USE THIS.
// CANCEL, the header's close mark and the screen underneath going away hand
// nothing. The list comes from the brains installed in the player's Brains
// folder. The runtime's own feed is installed until a check replaces it.
// A brain's words are resolved for the language in use when the list is
// built. Its icon is decoded then and kept until the picker closes. draw
// and tick do not list the folder, read a manifest or decode an icon.

#include "oa_layer.hpp"
#include "library_host.hpp"

#include "engine_settings_state.hpp"

#include "oa/app/content/updates.hpp"
#include "oa/app/mod_summary.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/match_rules/difficulty_names.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/brain_picker/model.hpp"
#include "oa/ui/brain_picker/screen.hpp"
#include "oa/ui/brain_picker/text.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/pad_controls.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace kit = oa::ui::kit;
namespace brains = oa::ui::brain_picker;
namespace mutators = oa::ui::mutator_picker;
namespace library = oa::ui::library;
namespace seats = oa::data::brain_library;
namespace oabrain = oa::formats::oabrain;
namespace oamut = oa::formats::oamut;
namespace dialogs = oa::ui::frontend_dialogs;

/// A toggle's off value, written as the manifest writes one.
constexpr std::string_view kOffValue = "false";
/// A toggle's on value.
constexpr std::string_view kOnValue = "true";

/// The built-in player's 3.1c labels, easy then medium then hard.
constexpr std::array<std::string_view, 3> kLevelLabels{"Easy", "Medium", "Hard"};

/// Returns one of the interface's words in the language shown.
///
/// @param context unused
/// @param english the word, in English
/// @return its translation, or the English
std::string_view shown_word(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// The hooks the picker looks words up through.
///
/// @return the hooks
library::TextHooks text_hooks() {
    return library::TextHooks{nullptr, shown_word};
}

/// Tells whether two language tags name one language.
///
/// ASCII case is ignored, and '_' is read as '-'.
///
/// @param left a tag
/// @param right a tag
/// @return true when they name one language
bool same_tag(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t place = 0; place < left.size(); ++place) {
        unsigned char first = static_cast<unsigned char>(left[place]);
        unsigned char second = static_cast<unsigned char>(right[place]);
        if (first == '_')
            first = '-';
        if (second == '_')
            second = '-';
        if (std::tolower(first) != std::tolower(second))
            return false;
    }
    return true;
}

/// The name folded to lower case, ASCII letters only, as the picker sorts.
///
/// @param name the name
/// @return the folded name
std::string folded_name(std::string_view name) {
    std::string folded;
    folded.reserve(name.size());
    for (const unsigned char byte : name)
        folded.push_back(static_cast<char>(std::tolower(byte)));
    return folded;
}

/// Returns a badge the picker draws from a decoded picture.
///
/// @param picture the decoded picture
/// @return the badge; nothing for an empty picture
std::optional<library::Badge> badge_of(const ModBadge& picture) {
    if (picture.width == 0 || picture.height == 0 || picture.pixels.empty())
        return std::nullopt;
    return library::Badge{picture.pixels, picture.width, picture.height};
}

/// Reads a brain's icon and decodes it as a mod badge.
///
/// A file larger than max_mod_badge_bytes, an empty file, or one that cannot
/// be read gives no badge. Called only while the own feed builds its list.
///
/// @param path the icon file
/// @return the badge; empty when there is none
ModBadge read_limited_badge(const std::filesystem::path& path) {
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr)
        return {};
    std::vector<uint8_t> bytes;
    std::array<uint8_t, 4096> chunk{};
    bool failed = false;
    while (!failed) {
        const std::size_t read = std::fread(chunk.data(), 1, chunk.size(), file);
        if (read > 0) {
            if (bytes.size() + read > max_mod_badge_bytes) {
                failed = true;
                break;
            }
            bytes.insert(
                bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read)
            );
        }
        if (read < chunk.size()) {
            if (std::ferror(file) != 0)
                failed = true;
            break;
        }
    }
    std::fclose(file);
    if (failed || bytes.empty())
        return {};
    return read_mod_badge(bytes);
}

/// The names a manifest's translations give, filled along the fallback chain.
struct ResolvedStrings {
    std::string name{};
    std::string description{};
    std::map<std::string, std::string> option_labels{};
    std::map<std::string, std::vector<std::string>> value_labels{};
    std::map<std::string, std::string> level_names{};
};

/// Fills empty fields from one translation's words.
///
/// @param[in,out] into the fields filled so far
/// @param translation the translation's words
void absorb_words(ResolvedStrings& into, const oamut::Translation& translation) {
    if (into.name.empty())
        into.name = translation.name;
    if (into.description.empty())
        into.description = translation.description;
    for (const oamut::OptionLabel& option : translation.options) {
        if (!option.label.empty() && !into.option_labels.contains(option.id))
            into.option_labels.emplace(option.id, option.label);
    }
    for (const oamut::ValueLabels& labels : translation.labels) {
        if (!labels.labels.empty() && !into.value_labels.contains(labels.id))
            into.value_labels.emplace(labels.id, labels.labels);
    }
}

/// Fills empty fields from one brain translation, including its level names.
///
/// @param[in,out] into the fields filled so far
/// @param translation the translation
void absorb(ResolvedStrings& into, const oabrain::Translation& translation) {
    absorb_words(into, translation.words);
    for (const oabrain::LevelName& level : translation.levels) {
        if (!level.name.empty() && !into.level_names.contains(level.id))
            into.level_names.emplace(level.id, level.name);
    }
}

/// Resolves a brain's strings for the language the interface shows.
///
/// The fallback chain is walked preferred first. Empty fields stay empty
/// here; the caller fills them from the manifest's own strings.
///
/// @param translations the manifest's translations
/// @return the fields a translation gave
ResolvedStrings resolve_strings(const std::vector<oabrain::Translation>& translations) {
    ResolvedStrings resolved;
    const oa::data::languages::FallbackChain chain =
        oa::data::languages::fallback_chain(oa::data::languages::interface_language());
    for (const oa::data::languages::Language* language : chain.view()) {
        if (language == nullptr)
            continue;
        for (const oabrain::Translation& translation : translations)
            if (same_tag(translation.words.language, language->tag))
                absorb(resolved, translation);
    }
    return resolved;
}

/// A toggle's two values, off first, captioned in the language shown.
///
/// @param[out] spec the option
/// @param fallback `true` or `false`
void fill_toggle(mutators::OptionSpec& spec, std::string fallback) {
    spec.type = mutators::OptionSpec::Type::toggle;
    spec.values.emplace_back(kOffValue);
    spec.values.emplace_back(kOnValue);
    spec.captions.emplace_back(oa::data::languages::interface_text("Off"));
    spec.captions.emplace_back(oa::data::languages::interface_text("On"));
    spec.fallback = std::move(fallback);
}

/// Copies an installed brain's options.
///
/// @param manifest the manifest
/// @param resolved the translated labels
/// @return the options, in the manifest's order
std::vector<mutators::OptionSpec>
options_of(const oabrain::Manifest& manifest, const ResolvedStrings& resolved) {
    std::vector<mutators::OptionSpec> options;
    for (const oamut::Option& option : manifest.options) {
        mutators::OptionSpec spec;
        spec.id = option.id;
        if (const auto label = resolved.option_labels.find(option.id);
            label != resolved.option_labels.end())
            spec.label = label->second;
        else
            spec.label = option.label;
        if (option.type == oamut::OptionType::toggle) {
            fill_toggle(
                spec, option.toggle_default ? std::string(kOnValue) : std::string(kOffValue)
            );
        } else if (option.type == oamut::OptionType::number) {
            spec.type = mutators::OptionSpec::Type::number;
            spec.least = formats::oamod::canonical_number_text(option.minimum);
            spec.most = formats::oamod::canonical_number_text(option.maximum);
            spec.step = formats::oamod::canonical_number_text(option.step);
            spec.fallback = formats::oamod::canonical_number_text(option.number_default);
        } else {
            spec.type = mutators::OptionSpec::Type::choice;
            for (const oamut::ChoiceValue& value : option.values)
                spec.values.push_back(value.text);
            const auto translated = resolved.value_labels.find(option.id);
            if (translated != resolved.value_labels.end() &&
                translated->second.size() == spec.values.size())
                spec.captions = translated->second;
            else if (option.value_labels.size() == spec.values.size())
                spec.captions = option.value_labels;
            else
                spec.captions = spec.values;
            if (option.default_index < spec.values.size())
                spec.fallback = spec.values[option.default_index];
            else if (!spec.values.empty())
                spec.fallback = spec.values.front();
        }
        options.push_back(std::move(spec));
    }
    return options;
}

/// Maps a manifest difficulty onto the seat's, in the same order.
///
/// @param difficulty the manifest's difficulty
/// @return the seat's
seats::Difficulty seat_difficulty(oabrain::Difficulty difficulty) noexcept {
    switch (difficulty) {
    case oabrain::Difficulty::easy:
        return seats::Difficulty::easy;
    case oabrain::Difficulty::medium:
        return seats::Difficulty::medium;
    case oabrain::Difficulty::hard:
        return seats::Difficulty::hard;
    }
    return seats::Difficulty::easy;
}

/// The mod being played, for a brain's fit. Empty when the game plays no mod.
///
/// A template so a free function never names `EngineSettingsState`, which is
/// private to Runtime. The caller is a member and passes that state.
///
/// @param mods the mods the game lists
/// @return the mod's id, name and version; an empty id is the base game
template <typename Mods>
library::PlayedMod played_mod(const Mods& mods) {
    library::PlayedMod played;
    if (mods.playing_mod_folder.empty())
        return played;
    for (std::size_t place = 0; place < mods.mod_folders.size() && place < mods.mod_names.size() &&
                                place < mods.mod_details.size();
         ++place) {
        if (mods.mod_folders[place] != mods.playing_mod_folder)
            continue;
        const auto& details = mods.mod_details[place];
        played.id = details.profile_id;
        played.name = mods.mod_names[place];
        if (details.version != mod_summary_no_version)
            played.version = details.version;
        break;
    }
    return played;
}

/// The name the picker shows for the mod being played. Empty says No Mod.
///
/// @param mods the mods the game lists
/// @return the mod's name; empty when none is playing
template <typename Mods>
std::string played_mod_name(const Mods& mods) {
    return played_mod(mods).name;
}

/// Names each works-with entry from the mods the game lists.
///
/// An id the list does not know keeps an empty name, and the fact then says
/// the id. `*` is any mod and is left unnamed.
///
/// @param[in,out] facts the brain's facts
/// @param mods the mods the game lists
template <typename Mods>
void name_works_with(library::BrainFacts& facts, const Mods& mods) {
    for (library::WorksWith& item : facts.works_with) {
        if (item.id.empty() || item.id == "*")
            continue;
        for (std::size_t place = 0;
             place < mods.mod_details.size() && place < mods.mod_names.size();
             ++place) {
            if (mods.mod_details[place].profile_id == item.id && !mods.mod_names[place].empty()) {
                item.name = mods.mod_names[place];
                break;
            }
        }
    }
}

/// The mods a brain works with, already in words.
///
/// An empty list and `*` say any mod, so the details do not drop the fact.
///
/// @param facts the brain's facts, its works-with names already filled
/// @param probe the game being played
/// @param text how texts are looked up
/// @return the words
std::string works_with_text(
    const library::BrainFacts& facts, const library::Inputs& probe, const library::TextHooks& text
) {
    const library::BrainWorksWith works = library::brain_works_with(probe, facts, text);
    if (works.any)
        return std::string(library::shown(text, library::words::works_with_any));
    return library::join_or(text, works.names);
}

/// The fit probe: this build, the mod being played, and the expansions on.
///
/// @param mods the mods the game lists
/// @param expansions the expansions that are on
/// @return the probe
template <typename Mods>
library::Inputs fit_probe(const Mods& mods, const std::vector<std::string>& expansions) {
    library::Inputs probe;
    probe.engine = content::this_engine_rules().engine;
    probe.played = played_mod(mods);
    probe.content.enabled = expansions;
    return probe;
}

/// The built-in player's Easy, Medium and Hard, as the game names them.
///
/// The labels are 3.1c's, in the order ai.difficulty-names gives them, and
/// translated as the 3.1c screens translate them.
///
/// @param runtime the runtime
/// @return the three names, easy then medium then hard
std::array<std::string, 3> built_in_level_names(Runtime& runtime) {
    const oa::data::match_rules::AiDifficultyNames rule = runtime.difficulty_names();
    std::array<std::string, 3> names;
    for (int32_t difficulty = 0; difficulty < 3; ++difficulty) {
        const int32_t slot = oa::data::match_rules::difficulty_name_index(rule, difficulty);
        const std::size_t label_at = slot >= 0 && slot < 3 ? static_cast<std::size_t>(slot)
                                                           : static_cast<std::size_t>(difficulty);
        names[static_cast<std::size_t>(difficulty)] = runtime.translate_ui(kLevelLabels[label_at]);
    }
    return names;
}

/// Tells whether a value is one an option allows.
///
/// A number must be one of the values `number_values` lists. The list is
/// kept in a local, so nothing points into the temporary it returns.
///
/// @param spec the option
/// @param value the value, as written
/// @return true when the option allows it
bool value_allowed(const mutators::OptionSpec& spec, std::string_view value) {
    if (spec.type == mutators::OptionSpec::Type::number) {
        const std::vector<std::string> offered = mutators::number_values(spec);
        return std::find(offered.begin(), offered.end(), value) != offered.end();
    }
    return std::find(spec.values.begin(), spec.values.end(), value) != spec.values.end();
}

/// The key a menu action hands the picker. Empty when the action is not one.
///
/// @param action the gamepad action
/// @return the key; nothing when the picker does not take it as a key
std::optional<kit::Key> key_of(oa::ui::pad_controls::Action action) {
    using oa::ui::pad_controls::Action;
    switch (action) {
    case Action::press:
        return kit::Key::enter;
    case Action::focus_next:
        return kit::Key::tab;
    case Action::focus_previous:
        return kit::Key::back_tab;
    case Action::focus_up:
        return kit::Key::up;
    case Action::focus_down:
        return kit::Key::down;
    case Action::focus_left:
        return kit::Key::left;
    case Action::focus_right:
        return kit::Key::right;
    default:
        return std::nullopt;
    }
}

} // namespace

/// The pictures the rows' badges point at. A deque, so a new picture does
/// not move one a row already points at.
struct Runtime::BrainPickerState {
    std::deque<library::Badge> badges{};
};

void Runtime::destroy_brain_picker_state(BrainPickerState* state) noexcept {
    delete state;
}

/// The picker screen.
struct Runtime::BrainPickerScreen final : LayerScreen {
    /// Makes the screen. start fills it and is called before the layer shows it.
    ///
    /// @param runtime the runtime
    /// @param request the seat, the choice and what it hands back
    BrainPickerScreen(Runtime& runtime, BrainPickerRequest request)
        : runtime_(runtime), request_(std::move(request)) {}

    /// Fills the picker from the feed. False when the feed gives no list.
    ///
    /// @return false when the picker cannot be filled
    bool start();

    /// Drops badge pointers the rows hold, before the pictures are freed.
    void drop_badges();

    [[nodiscard]] std::string_view name() const override { return brains::screen_name; }

    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        if (placed_on.match || runtime_.match_ != nullptr || runtime_.screen_ == Screen::match ||
            runtime_.screen_ != opened_over_)
            return {};
        const kit::Point size = brains::window_size(placed_on.frame);
        return centred_placement(placed_on, size.x, size.y);
    }

    [[nodiscard]] bool modal() const override { return true; }

    [[nodiscard]] bool backdrop() const override { return true; }

    void lay_out(const LayerView& placed_on) override {
        if (laid_out_ && placed_on.frame.area.width == frame_.area.width &&
            placed_on.frame.area.height == frame_.area.height &&
            placed_on.frame.size_class == frame_.size_class &&
            placed_on.frame.scale_percent == frame_.scale_percent)
            return;
        frame_ = placed_on.frame;
        laid_out_ = true;
        relayout();
    }

    void draw(const kit::Canvas& canvas) const override {
        // Does not list the Brains folder, read a manifest or decode an icon.
        brains::draw_picker(canvas, list_, brains::window_size(frame_));
    }

    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        brains::PointerKind moved = brains::PointerKind::move;
        if (kind == ScreenInputKind::pointer_down)
            moved = brains::PointerKind::down;
        else if (kind == ScreenInputKind::pointer_up)
            moved = brains::PointerKind::up;
        else if (kind != ScreenInputKind::pointer_move)
            return LayerAnswer::none;
        return take(
            brains::picker_pointer(picker_, state_, list_, moved, button, at, finger_reach)
        );
    }

    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        return take(brains::picker_key(picker_, state_, list_, pressed));
    }

    LayerAnswer shortcut(uint32_t /*sdl_key*/, uint16_t /*modifiers*/) override {
        return LayerAnswer::pass;
    }

    LayerAnswer pad_action(oa::ui::pad_controls::Action action) override {
        using oa::ui::pad_controls::Action;
        if (action == Action::back)
            return take(
                brains::picker_command(picker_, state_, list_, brains::ScreenCommand::back)
            );
        if (action == Action::default_button)
            return take(brains::picker_command(picker_, state_, list_, brains::ScreenCommand::use));
        if (const std::optional<kit::Key> pressed = key_of(action))
            return take(brains::picker_key(picker_, state_, list_, *pressed));
        return LayerAnswer::pass;
    }

    LayerAnswer pad_stick(float notches) override {
        return take(brains::picker_stick(picker_, state_, list_, notches));
    }

    LayerAnswer wheel(kit::Point at, float notches) override {
        return take(brains::picker_wheel(picker_, state_, list_, at, notches));
    }

    [[nodiscard]] kit::DisplayList display_list() const override { return list_; }

    [[nodiscard]] kit::Interaction interaction() const override { return state_.interaction; }

    LayerAnswer tick() override {
        // Does not list the Brains folder, read a manifest or decode an icon.
        // A match, or the screen underneath going away, closes as Cancel.
        if (runtime_.screen_ != opened_over_ || runtime_.match_ != nullptr ||
            runtime_.screen_ == Screen::match)
            return LayerAnswer::close;
        const BrainPickerFeed& feed = runtime_.brain_picker_feed_;
        uint64_t generation = seen_generation_;
        if (feed.generation != nullptr)
            generation = feed.generation(feed.context);
        if (generation == seen_generation_)
            return LayerAnswer::none;
        seen_generation_ = generation;
        if (!load(false)) {
            ending_ = Ending::cancel;
            return LayerAnswer::close;
        }
        relayout();
        return LayerAnswer::redraw;
    }

    [[nodiscard]] uint64_t revision() const override { return revision_; }

    void close(bool by_key) override;

  private:

    /// How the picker is leaving. A close that is not a hand cancels.
    enum class Ending : uint8_t { cancel, hand };

    /// Reads the feed and opens the model on the choice to keep.
    ///
    /// @param opening true the first time, which opens on the request's choice
    /// @return false when the feed gives no list
    bool load(bool opening);

    /// Tells whether a choice may be handed to the caller.
    ///
    /// Refuses an unfit brain, a brain that is not listed, a level it lacks
    /// and an option value it refuses. The built-in player takes easy,
    /// medium or hard and no options.
    ///
    /// @param choice the choice
    /// @return true when USE THIS may hand it
    [[nodiscard]] bool choice_allowed(const brains::Choice& choice) const;

    /// USE THIS. An unfit choice closes as Cancel and is not handed.
    ///
    /// @return close
    LayerAnswer hand();

    /// Carries out what one input asked, then lays the picker out again.
    ///
    /// @param result what the input asked
    /// @return what the layer should do
    LayerAnswer take(brains::ScreenResult result);

    /// Lays the picker out and counts the change.
    void relayout();

    Runtime& runtime_;
    BrainPickerRequest request_;
    Screen opened_over_{};
    brains::Picker picker_{};
    brains::ScreenState state_{};
    kit::DisplayList list_{};
    kit::Frame frame_{};
    bool laid_out_{false};
    uint64_t revision_{};
    uint64_t seen_generation_{};
    bool answered_{false};
    Ending ending_{Ending::cancel};
    brains::Choice handed_{};
    bool handed_every_{false};
};

bool Runtime::BrainPickerScreen::start() {
    opened_over_ = runtime_.screen_;
    if (runtime_.brain_picker_feed_.brains == nullptr)
        return false;
    if (!load(true))
        return false;
    const BrainPickerFeed& feed = runtime_.brain_picker_feed_;
    if (feed.generation != nullptr)
        seen_generation_ = feed.generation(feed.context);
    frame_ = runtime_.oa_layer().view().frame;
    laid_out_ = true;
    relayout();
    return true;
}

void Runtime::BrainPickerScreen::drop_badges() {
    for (brains::Brain& brain : picker_.inputs.brains)
        brain.badge = nullptr;
}

void Runtime::BrainPickerScreen::close(bool /*by_key*/) {
    if (answered_)
        return;
    answered_ = true;
    drop_badges();
    runtime_.brain_picker_state_.reset();
    std::function<void(brains::Choice, bool)> used = std::move(request_.used);
    std::function<void()> cancelled = std::move(request_.cancelled);
    brains::Choice handed = std::move(handed_);
    const bool every = handed_every_;
    const bool hand = ending_ == Ending::hand;
    if (hand) {
        if (used)
            used(std::move(handed), every);
    } else if (cancelled) {
        cancelled();
    }
}

bool Runtime::BrainPickerScreen::load(bool opening) {
    const BrainPickerFeed& feed = runtime_.brain_picker_feed_;
    if (feed.brains == nullptr)
        return false;
    // open clears every seat. A refresh keeps the player's choice of it.
    brains::Choice kept;
    bool every = false;
    if (!opening) {
        kept = brains::result(picker_);
        every = picker_.every_seat;
    }
    drop_badges();
    brains::Inputs inputs;
    inputs.brains = feed.brains(feed.context);
    inputs.setup = request_.setup;
    inputs.seat_name = request_.seat_name;
    inputs.mod_name = played_mod_name(runtime_.engine_settings_state());
    inputs.current = opening ? request_.current : std::move(kept);
    inputs.setup_difficulty = request_.setup_difficulty;
    inputs.built_in_level_fixed = request_.built_in_level_fixed;
    inputs.built_in_level_names = built_in_level_names(runtime_);
    inputs.offer_every_seat = request_.offer_every_seat;
    inputs.text = text_hooks();
    brains::open(picker_, std::move(inputs));
    if (!opening)
        std::ignore = brains::set_every_seat(picker_, every);
    return true;
}

bool Runtime::BrainPickerScreen::choice_allowed(const brains::Choice& choice) const {
    if (choice.brain.empty()) {
        if (!choice.options.empty())
            return false;
        return choice.level == "easy" || choice.level == "medium" || choice.level == "hard";
    }
    const brains::Brain* brain = nullptr;
    for (const brains::Brain& one : picker_.inputs.brains) {
        if (!one.id.empty() && one.id == choice.brain)
            brain = &one;
    }
    if (brain == nullptr || !brain->unfit.empty())
        return false;
    bool level_ok = false;
    for (const brains::Level& one_level : brain->levels) {
        if (one_level.id == choice.level)
            level_ok = true;
    }
    if (!level_ok)
        return false;
    if (choice.options.size() != brain->options.size())
        return false;
    for (const mutators::OptionSpec& spec : brain->options) {
        const std::string* found = nullptr;
        for (const auto& pair : choice.options) {
            if (pair.first != spec.id)
                continue;
            if (found != nullptr)
                return false;
            found = &pair.second;
        }
        if (found == nullptr || !value_allowed(spec, *found))
            return false;
    }
    return true;
}

LayerAnswer Runtime::BrainPickerScreen::hand() {
    const brains::Choice choice = brains::result(picker_);
    if (!choice_allowed(choice)) {
        ending_ = Ending::cancel;
        return LayerAnswer::close;
    }
    handed_ = choice;
    handed_every_ = picker_.every_seat;
    ending_ = Ending::hand;
    return LayerAnswer::close;
}

LayerAnswer Runtime::BrainPickerScreen::take(brains::ScreenResult result) {
    switch (result.action) {
    case brains::PickerAction::cancel:
        ending_ = Ending::cancel;
        return LayerAnswer::close;
    case brains::PickerAction::use:
        return hand();
    case brains::PickerAction::none:
        break;
    }
    if (result.redraw) {
        relayout();
        return LayerAnswer::redraw;
    }
    return LayerAnswer::none;
}

void Runtime::BrainPickerScreen::relayout() {
    ++revision_;
    const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
    if (fonts == nullptr) {
        list_ = {};
        return;
    }
    list_ = brains::picker_layout(picker_, state_, frame_, *fonts);
}

std::vector<brains::Brain> Runtime::own_brain_picker_brains(void* context) {
    // The only place the picker lists the Brains folder or decodes an icon.
    // draw and tick do not call this. The listing is the one already read;
    // this does not open a manifest or a script itself.
    Runtime& runtime = *static_cast<Runtime*>(context);
    if (runtime.brain_picker_state_ == nullptr)
        runtime.brain_picker_state_.reset(new BrainPickerState);
    BrainPickerState& held = *runtime.brain_picker_state_;
    held.badges.clear();

    const library::TextHooks hooks = text_hooks();
    const auto& mods = runtime.engine_settings_state();
    const library::Inputs probe = fit_probe(mods, runtime.content_choice().expansions);
    const seats::Listing& listing = runtime.installed_brains();

    std::vector<brains::Brain> listed;
    brains::Brain built_in;
    built_in.name = std::string(library::shown(hooks, brains::words::built_in_player));
    built_in.summary = std::string(library::shown(hooks, brains::words::built_into));
    listed.push_back(std::move(built_in));

    for (const seats::InstalledBrain& installed : listing.brains) {
        const oabrain::Manifest& manifest = installed.manifest;
        const ResolvedStrings resolved = resolve_strings(manifest.strings);
        brains::Brain brain;
        brain.id = manifest.id;
        brain.name = resolved.name.empty() ? manifest.name : resolved.name;
        brain.version = oamut::version_text(manifest.version);
        brain.author = manifest.author.name;
        brain.description =
            resolved.description.empty() ? manifest.description : resolved.description;
        oabrain::Manifest described;
        described.description = brain.description;
        brain.summary = oabrain::summary_of(described);
        brain.tags = manifest.tags;
        for (const oabrain::Level& level : manifest.levels) {
            brains::Level made;
            made.id = level.id;
            const auto named = resolved.level_names.find(level.id);
            made.name = named != resolved.level_names.end() ? named->second : level.name;
            made.difficulty = seat_difficulty(level.difficulty);
            brain.levels.push_back(std::move(made));
        }
        brain.options = options_of(manifest, resolved);
        brain.full_awareness = manifest.full_awareness;
        brain.network_bytes = installed.network_bytes;
        library::BrainFacts facts = brain_facts_of(manifest, installed.network_bytes);
        name_works_with(facts, mods);
        brain.works_with_text = works_with_text(facts, probe, hooks);
        brain.unfit = library::brain_unfit_reason(probe, facts, hooks);
        if (!manifest.icon.empty()) {
            const ModBadge picture = read_limited_badge(installed.folder / manifest.icon);
            if (const std::optional<library::Badge> badge = badge_of(picture)) {
                held.badges.push_back(*badge);
                brain.badge = &held.badges.back();
            }
        }
        listed.push_back(std::move(brain));
    }
    std::stable_sort(
        listed.begin() + 1, listed.end(), [](const brains::Brain& one, const brains::Brain& other) {
            return folded_name(one.name) < folded_name(other.name);
        }
    );
    return listed;
}

uint64_t Runtime::own_brain_picker_generation(void* context) {
    // The count reload_installed_brains moves. This does not list the folder.
    return static_cast<Runtime*>(context)->installed_brains_generation_;
}

bool Runtime::open_brain_picker(BrainPickerRequest request) {
    if (match_ != nullptr || screen_ == Screen::match)
        return false;
    if (dialogs::dialog_count() != 0)
        return false;
    if (oa_layer().modal_shown(false))
        return false;
    if (oa_layer().screen_fonts() == nullptr)
        return false;
    if (oa_layer().find(brains::screen_name) != nullptr)
        return false;
    if (!brain_picker_feed_set_)
        restore_brain_picker_feed();
    if (brain_picker_feed_.brains == nullptr)
        return false;
    auto screen = std::make_unique<BrainPickerScreen>(*this, std::move(request));
    if (!screen->start()) {
        brain_picker_state_.reset();
        return false;
    }
    oa_layer().push(std::move(screen));
    return true;
}

void Runtime::set_brain_picker_feed(BrainPickerFeed feed) {
    brain_picker_feed_ = feed;
    brain_picker_feed_set_ = true;
    if (LayerScreen* screen = oa_layer().find(brains::screen_name))
        static_cast<BrainPickerScreen*>(screen)->drop_badges();
    if (brain_picker_state_ != nullptr)
        brain_picker_state_->badges.clear();
}

void Runtime::restore_brain_picker_feed() {
    BrainPickerFeed feed;
    feed.context = this;
    feed.brains = own_brain_picker_brains;
    feed.generation = own_brain_picker_generation;
    set_brain_picker_feed(feed);
}

} // namespace oa::app
