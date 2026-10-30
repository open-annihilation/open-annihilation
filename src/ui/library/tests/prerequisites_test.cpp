// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The order of made-up mutators: one id, a chain of three, a diamond, two
// roots that share a prerequisite, a cycle of two and of three, an id the
// graph does not hold and a repeated id; dependants direct, through another
// and none, in the list's order; an order that keeps a chain, a diamond and
// a list with an outside id, and one that does not; and a chain of 1,000.
#include "oa/test/check.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/library/uninstall.hpp"

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace lib = oa::ui::library;

constexpr std::string_view grounded = "grounded";
constexpr std::string_view gatehouse = "gatehouse";
constexpr std::string_view ridge_walls = "ridge-walls";
constexpr std::string_view gatehouse_keep = "gatehouse-keep";
constexpr std::string_view fast_builders = "fast-builders";
constexpr std::string_view outside = "not-a-mutator";

/// One need, for a short graph.
///
/// @param id the mutator's id
/// @param needs the ids it needs
/// @return the record
lib::Need need(std::string_view id, std::vector<std::string> needs) {
    return lib::Need{std::string(id), std::move(needs)};
}

/// The made-up mutators and what each needs.
///
/// Gatehouse Keep needs Gatehouse and Ridge Walls. Gatehouse and Ridge Walls
/// both need Grounded. Fast Builders needs nothing.
///
/// @return the graph
std::vector<lib::Need> made_up() {
    return {
        need(fast_builders, {}),
        need(grounded, {}),
        need(gatehouse, {std::string(grounded)}),
        need(ridge_walls, {std::string(grounded)}),
        need(gatehouse_keep, {std::string(gatehouse), std::string(ridge_walls)}),
    };
}

/// Checks a closure's ids and whether it met a cycle.
///
/// @param result the closure
/// @param expected the ids, in order
/// @param cyclic whether a cycle remains
void check_closure(
    const lib::Closure& result, const std::vector<std::string>& expected, bool cyclic
) {
    OA_CHECK(result.cyclic == cyclic);
    OA_CHECK(result.order.size() == expected.size());
    if (result.order.size() != expected.size())
        return;
    for (std::size_t index = 0; index < expected.size(); ++index)
        OA_CHECK(result.order[index] == expected[index]);
}

/// Checks a list of ids.
///
/// @param actual the ids written
/// @param expected the ids the case expects
void check_ids(const std::vector<std::string>& actual, const std::vector<std::string>& expected) {
    OA_CHECK(actual.size() == expected.size());
    if (actual.size() != expected.size())
        return;
    for (std::size_t index = 0; index < expected.size(); ++index)
        OA_CHECK(actual[index] == expected[index]);
}

/// One id, a chain of three, a diamond, two roots, and a repeated id.
void closures() {
    const std::vector<lib::Need> graph = made_up();
    const std::string only_grounded[]{std::string(grounded)};
    check_closure(lib::prerequisite_closure(graph, only_grounded), {std::string(grounded)}, false);

    const lib::Need chain[]{
        need(grounded, {}),
        need(gatehouse, {std::string(grounded)}),
        need(gatehouse_keep, {std::string(gatehouse)}),
    };
    const std::string chain_from[]{std::string(gatehouse_keep)};
    check_closure(
        lib::prerequisite_closure(chain, chain_from),
        {std::string(grounded), std::string(gatehouse), std::string(gatehouse_keep)},
        false
    );

    const std::string keep_only[]{std::string(gatehouse_keep)};
    check_closure(
        lib::prerequisite_closure(graph, keep_only),
        {std::string(grounded),
         std::string(gatehouse),
         std::string(ridge_walls),
         std::string(gatehouse_keep)},
        false
    );

    const std::string two_roots[]{std::string(ridge_walls), std::string(gatehouse)};
    check_closure(
        lib::prerequisite_closure(graph, two_roots),
        {std::string(grounded), std::string(ridge_walls), std::string(gatehouse)},
        false
    );

    const std::string repeated[]{std::string(gatehouse_keep), std::string(gatehouse_keep)};
    check_closure(
        lib::prerequisite_closure(graph, repeated),
        {std::string(grounded),
         std::string(gatehouse),
         std::string(ridge_walls),
         std::string(gatehouse_keep)},
        false
    );
}

/// An id the graph does not hold stays, and needs nothing of its own.
void outside_id() {
    const std::vector<lib::Need> graph = made_up();
    const std::string ids[]{std::string(outside), std::string(gatehouse)};
    check_closure(
        lib::prerequisite_closure(graph, ids),
        {std::string(outside), std::string(grounded), std::string(gatehouse)},
        false
    );
}

/// A cycle of two and of three keeps each id once, in the order first met,
/// and an id that only needs a cycle stays below that cycle.
void cycles() {
    const lib::Need pair[]{
        need(gatehouse, {std::string(ridge_walls)}),
        need(ridge_walls, {std::string(gatehouse)}),
    };
    const std::string pair_from[]{std::string(gatehouse)};
    check_closure(
        lib::prerequisite_closure(pair, pair_from),
        {std::string(gatehouse), std::string(ridge_walls)},
        true
    );

    const lib::Need triple[]{
        need(gatehouse_keep, {std::string(gatehouse)}),
        need(gatehouse, {std::string(ridge_walls)}),
        need(ridge_walls, {std::string(gatehouse_keep)}),
    };
    const std::string triple_from[]{std::string(gatehouse)};
    check_closure(
        lib::prerequisite_closure(triple, triple_from),
        {std::string(gatehouse), std::string(ridge_walls), std::string(gatehouse_keep)},
        true
    );

    const lib::Need above_pair[]{
        need("x", {std::string("a")}),
        need("a", {std::string("b")}),
        need("b", {std::string("a")}),
    };
    const std::string above_pair_from[]{std::string("x")};
    check_closure(
        lib::prerequisite_closure(above_pair, above_pair_from),
        {std::string("a"), std::string("b"), std::string("x")},
        true
    );

    const lib::Need above_deeper[]{
        need("keep", {std::string("gate")}),
        need("gate", {std::string("wall"), std::string("base")}),
        need("wall", {std::string("tower")}),
        need("tower", {std::string("wall")}),
        need("base", {}),
    };
    const std::string above_deeper_from[]{std::string("keep")};
    check_closure(
        lib::prerequisite_closure(above_deeper, above_deeper_from),
        {std::string("wall"),
         std::string("tower"),
         std::string("base"),
         std::string("gate"),
         std::string("keep")},
        true
    );
}

/// Dependants direct, through another id, and none, in the list's order.
void dependants() {
    const std::vector<lib::Need> graph = made_up();
    const std::string direct_among[]{
        std::string(ridge_walls), std::string(fast_builders), std::string(gatehouse)
    };
    check_ids(
        lib::dependants_of(graph, direct_among, grounded),
        {std::string(ridge_walls), std::string(gatehouse)}
    );

    const std::string through_among[]{std::string(fast_builders), std::string(gatehouse_keep)};
    check_ids(lib::dependants_of(graph, through_among, grounded), {std::string(gatehouse_keep)});

    const std::string none_among[]{std::string(fast_builders), std::string(grounded)};
    check_ids(lib::dependants_of(graph, none_among, fast_builders), {});

    const std::string ordered[]{
        std::string(ridge_walls),
        std::string(fast_builders),
        std::string(gatehouse_keep),
        std::string(gatehouse),
    };
    check_ids(
        lib::dependants_of(graph, ordered, grounded),
        {std::string(ridge_walls), std::string(gatehouse_keep), std::string(gatehouse)}
    );
}

/// A chain, a diamond and a list that names an id outside the graph.
void order_holds() {
    const std::vector<lib::Need> graph = made_up();
    const std::string chain_above[]{
        std::string(grounded), std::string(gatehouse), std::string(gatehouse_keep)
    };
    const std::string chain_below[]{
        std::string(gatehouse_keep), std::string(gatehouse), std::string(grounded)
    };
    OA_CHECK(lib::prerequisites_above(graph, chain_above));
    OA_CHECK(!lib::prerequisites_above(graph, chain_below));

    const std::string diamond_above[]{
        std::string(grounded),
        std::string(gatehouse),
        std::string(ridge_walls),
        std::string(gatehouse_keep),
    };
    const std::string diamond_below[]{
        std::string(gatehouse),
        std::string(ridge_walls),
        std::string(grounded),
        std::string(gatehouse_keep),
    };
    OA_CHECK(lib::prerequisites_above(graph, diamond_above));
    OA_CHECK(!lib::prerequisites_above(graph, diamond_below));

    const std::string outside_above[]{
        std::string(outside), std::string(grounded), std::string(gatehouse)
    };
    const std::string outside_below[]{
        std::string(gatehouse), std::string(outside), std::string(grounded)
    };
    OA_CHECK(lib::prerequisites_above(graph, outside_above));
    OA_CHECK(!lib::prerequisites_above(graph, outside_below));
}

/// A chain of 1,000 closes, the first id at the top, without a cycle.
void long_chain() {
    constexpr std::size_t count = 1000;
    std::vector<lib::Need> graph;
    graph.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        std::vector<std::string> needs;
        if (index > 0)
            needs.push_back("m" + std::to_string(index - 1));
        graph.push_back(need("m" + std::to_string(index), std::move(needs)));
    }
    const std::string ids[]{graph.back().id};
    const lib::Closure result = lib::prerequisite_closure(graph, ids);
    OA_CHECK(!result.cyclic);
    OA_CHECK(result.order.size() == count);
    if (result.order.size() == count) {
        OA_CHECK(result.order.front() == "m0");
        OA_CHECK(result.order.back() == "m999");
        OA_CHECK(result.order[500] == "m500");
    }
    const lib::Closure again = lib::prerequisite_closure(graph, ids);
    OA_CHECK(again.order == result.order);
    OA_CHECK(again.cyclic == result.cyclic);
}

namespace kit = oa::ui::kit;
using lib::Kind;

/// The built-in registry the plan's listings come from.
constexpr std::string_view core = "fixture-core";
/// Another registry, so a prerequisite can be listed twice.
constexpr std::string_view extra = "fixture-extra";

/// Prints both texts when they differ.
///
/// @param actual the text written
/// @param expected the text the case expects
void check_text(std::string_view actual, std::string_view expected) {
    if (actual == expected)
        return;
    std::fprintf(
        stderr,
        "text was \"%.*s\"\nexpected \"%.*s\"\n",
        static_cast<int>(actual.size()),
        actual.data(),
        static_cast<int>(expected.size()),
        expected.data()
    );
    ++oa::test::failed_checks();
}

/// A built-in registry and an added one.
///
/// @return the inputs, with no packages yet
lib::Inputs shelf() {
    lib::Inputs inputs;
    lib::Registry built;
    built.id = std::string(core);
    built.name = "Fixture Core";
    built.built_in = true;
    lib::Registry added;
    added.id = std::string(extra);
    added.name = "Fixture Extra";
    inputs.registries.push_back(std::move(built));
    inputs.registries.push_back(std::move(added));
    return inputs;
}

/// One prerequisite, named as the player reads it.
///
/// @param id the mutator's id
/// @param range the range as written; empty for any version
/// @param name the name
/// @return the prerequisite
lib::Prerequisite prerequisite(std::string_view id, std::string_view range, std::string_view name) {
    return lib::Prerequisite{std::string(id), std::string(range), std::string(name)};
}

/// A mutator listing.
///
/// @param registry the registry's id
/// @param key the package key
/// @param name the name
/// @param version the version
/// @param release the registry's release number
/// @param needs what it needs
/// @return the listing
lib::Listing mutator_listing(
    std::string_view registry,
    std::string_view key,
    std::string_view name,
    std::string_view version,
    int64_t release,
    std::vector<lib::Prerequisite> needs
) {
    lib::Listing made;
    made.registry = std::string(registry);
    made.kind = Kind::mutator;
    made.key = std::string(key);
    made.name = std::string(name);
    made.version = std::string(version);
    made.release = release;
    lib::MutatorFacts facts;
    facts.tags = {"gameplay"};
    facts.needs = std::move(needs);
    made.mutator = std::move(facts);
    return made;
}

/// An installed mutator.
///
/// @param registry the registry it came from
/// @param key the package key
/// @param name the name
/// @param version the version
/// @param needs what its manifest needs
/// @return the copy
lib::Installed mutator_installed(
    std::string_view registry,
    std::string_view key,
    std::string_view name,
    std::string_view version,
    std::vector<lib::Prerequisite> needs
) {
    lib::Installed made;
    made.kind = Kind::mutator;
    made.key = std::string(key);
    made.name = std::string(name);
    made.version = std::string(version);
    made.registry = std::string(registry);
    made.release = 1;
    made.folder = std::string("Mutators/") + std::string(key);
    lib::MutatorFacts facts;
    facts.tags = {"gameplay"};
    facts.needs = std::move(needs);
    made.mutator = std::move(facts);
    return made;
}

/// Builds a Library on the Mutators tab.
///
/// @param inputs the inputs
/// @return the Library
lib::Library opened(lib::Inputs inputs) {
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::mutators);
    return library;
}

/// Finds a mutator entry.
///
/// @param library the Library
/// @param registry its registry
/// @param key its key
/// @return the entry, or null
const lib::Entry*
mutator_at(const lib::Library& library, std::string_view registry, std::string_view key) {
    return lib::find_entry(library, {std::string(registry), Kind::mutator, std::string(key)});
}

/// The button of one action, copied out of the action list.
///
/// The list is a temporary, so a pointer into it would not outlive this call.
///
/// @param library the Library
/// @param entry the entry
/// @param action the action
/// @return the button, or none
std::optional<lib::ActionButton>
button_of(const lib::Library& library, const lib::Entry& entry, lib::Action action) {
    const std::vector<lib::ActionButton> buttons = lib::entry_actions(library, entry);
    for (const lib::ActionButton& button : buttons)
        if (button.action == action)
            return button;
    return std::nullopt;
}

/// needs_of reads the installed manifest when one is set, and the listing otherwise.
void needs_follow_the_installed_manifest() {
    lib::Inputs inputs = shelf();
    inputs.listings.push_back(mutator_listing(
        core,
        gatehouse_keep,
        "Gatehouse Keep",
        "2.0",
        1,
        {prerequisite(ridge_walls, "", "Ridge Walls")}
    ));
    inputs.installed.push_back(mutator_installed(
        core,
        gatehouse_keep,
        "Gatehouse Keep",
        "2.0",
        {prerequisite(gatehouse, ">= 4.0", "Gatehouse")}
    ));
    inputs.listings.push_back(mutator_listing(
        core, fast_builders, "Fast Builders", "1.0", 1, {prerequisite(grounded, "", "Grounded")}
    ));
    const lib::Library library = opened(std::move(inputs));
    const std::vector<lib::Need> graph = lib::needs_of(library);
    const lib::Need* keep = nullptr;
    const lib::Need* fast = nullptr;
    for (const lib::Need& record : graph) {
        if (record.id == gatehouse_keep)
            keep = &record;
        if (record.id == fast_builders)
            fast = &record;
    }
    OA_CHECK(keep != nullptr && fast != nullptr);
    if (keep != nullptr)
        OA_CHECK(keep->needs == std::vector<std::string>{std::string(gatehouse)});
    if (fast != nullptr)
        OA_CHECK(fast->needs == std::vector<std::string>{std::string(grounded)});
}

/// Gatehouse Keep's plan while Gatehouse is missing, too old, new enough, or unusable.
void plans_for_gatehouse_keep() {
    const auto keep_listing = [] {
        return mutator_listing(
            core,
            gatehouse_keep,
            "Gatehouse Keep",
            "2.0",
            1,
            {prerequisite(gatehouse, ">= 4.0", "Gatehouse")}
        );
    };
    {
        lib::Inputs inputs = shelf();
        inputs.listings.push_back(keep_listing());
        inputs.listings.push_back(mutator_listing(core, gatehouse, "Gatehouse", "4.1", 2, {}));
        const lib::Library library = opened(std::move(inputs));
        const lib::Entry* keep = mutator_at(library, core, gatehouse_keep);
        OA_CHECK(keep != nullptr);
        if (keep == nullptr)
            return;
        const lib::GetPlan plan = lib::get_plan(library, *keep);
        OA_CHECK(plan.blocked.empty());
        OA_CHECK(plan.steps.size() == 2);
        if (plan.steps.size() == 2) {
            OA_CHECK(plan.steps[0].id.key == gatehouse);
            OA_CHECK(plan.steps[0].version == "4.1");
            OA_CHECK(plan.steps[0].name == "Gatehouse");
            OA_CHECK(plan.steps[0].registry_name == "Fixture Core");
            OA_CHECK(plan.steps[1].id.key == gatehouse_keep);
            OA_CHECK(plan.steps[1].version == "2.0");
        }
        const std::optional<lib::ActionButton> with =
            button_of(library, *keep, lib::Action::get_with);
        OA_CHECK(with.has_value() && with->enabled);
        if (with.has_value())
            check_text(with->text, "Get with Gatehouse");
        OA_CHECK(!button_of(library, *keep, lib::Action::get).has_value());
    }
    {
        lib::Inputs inputs = shelf();
        inputs.listings.push_back(keep_listing());
        inputs.listings.push_back(mutator_listing(core, gatehouse, "Gatehouse", "4.1", 3, {}));
        inputs.installed.push_back(mutator_installed(core, gatehouse, "Gatehouse", "3.9", {}));
        const lib::Library library = opened(std::move(inputs));
        const lib::Entry* keep = mutator_at(library, core, gatehouse_keep);
        OA_CHECK(keep != nullptr);
        if (keep == nullptr)
            return;
        const lib::GetPlan plan = lib::get_plan(library, *keep);
        OA_CHECK(plan.blocked.empty() && plan.steps.size() == 2);
        if (plan.steps.size() == 2)
            OA_CHECK(plan.steps[0].id.key == gatehouse && plan.steps[0].version == "4.1");
    }
    {
        lib::Inputs inputs = shelf();
        inputs.listings.push_back(keep_listing());
        inputs.listings.push_back(mutator_listing(core, gatehouse, "Gatehouse", "4.1", 2, {}));
        inputs.installed.push_back(mutator_installed(core, gatehouse, "Gatehouse", "4.1", {}));
        const lib::Library library = opened(std::move(inputs));
        const lib::Entry* keep = mutator_at(library, core, gatehouse_keep);
        OA_CHECK(keep != nullptr);
        if (keep == nullptr)
            return;
        const lib::GetPlan plan = lib::get_plan(library, *keep);
        OA_CHECK(plan.blocked.empty() && plan.steps.size() == 1);
        if (plan.steps.size() == 1)
            OA_CHECK(plan.steps[0].id.key == gatehouse_keep);
        const std::optional<lib::ActionButton> get = button_of(library, *keep, lib::Action::get);
        OA_CHECK(get.has_value() && get->enabled);
        OA_CHECK(!button_of(library, *keep, lib::Action::get_with).has_value());
    }
    {
        lib::Inputs inputs = shelf();
        inputs.listings.push_back(keep_listing());
        inputs.listings.push_back(mutator_listing(core, gatehouse, "Gatehouse", "3.9", 1, {}));
        const lib::Library library = opened(std::move(inputs));
        const lib::Entry* keep = mutator_at(library, core, gatehouse_keep);
        OA_CHECK(keep != nullptr);
        if (keep == nullptr)
            return;
        const lib::GetPlan plan = lib::get_plan(library, *keep);
        OA_CHECK(plan.steps.empty());
        check_text(plan.blocked, "Needs Gatehouse 4.0 or later, which no registry offers");
        const std::optional<lib::ActionButton> with =
            button_of(library, *keep, lib::Action::get_with);
        OA_CHECK(with.has_value() && !with->enabled);
    }
    {
        lib::Inputs inputs = shelf();
        inputs.listings.push_back(keep_listing());
        inputs.listings.push_back(mutator_listing(core, gatehouse, "Gatehouse", "4.0", 1, {}));
        inputs.listings.push_back(mutator_listing(extra, gatehouse, "Gatehouse", "9.0", 20, {}));
        const lib::Library library = opened(std::move(inputs));
        const lib::Entry* keep = mutator_at(library, core, gatehouse_keep);
        OA_CHECK(keep != nullptr);
        if (keep == nullptr)
            return;
        const lib::GetPlan plan = lib::get_plan(library, *keep);
        OA_CHECK(plan.steps.size() == 2);
        if (plan.steps.size() == 2) {
            OA_CHECK(plan.steps[0].id.registry == core);
            OA_CHECK(plan.steps[0].version == "4.0");
        }
    }
}

/// Three missing prerequisites stay in closure order, ahead of Gatehouse Keep.
void a_chain_downloads_in_order() {
    lib::Inputs inputs = shelf();
    inputs.listings.push_back(mutator_listing(core, grounded, "Grounded", "1.0", 1, {}));
    inputs.listings.push_back(mutator_listing(
        core, ridge_walls, "Ridge Walls", "1.2", 1, {prerequisite(grounded, "", "Grounded")}
    ));
    inputs.listings.push_back(mutator_listing(
        core, gatehouse, "Gatehouse", "4.1", 1, {prerequisite(ridge_walls, "", "Ridge Walls")}
    ));
    inputs.listings.push_back(mutator_listing(
        core, gatehouse_keep, "Gatehouse Keep", "2.0", 1, {prerequisite(gatehouse, "", "Gatehouse")}
    ));
    const lib::Library library = opened(std::move(inputs));
    const lib::Entry* keep = mutator_at(library, core, gatehouse_keep);
    OA_CHECK(keep != nullptr);
    if (keep == nullptr)
        return;
    const lib::GetPlan plan = lib::get_plan(library, *keep);
    OA_CHECK(plan.blocked.empty() && plan.steps.size() == 4);
    if (plan.steps.size() == 4) {
        OA_CHECK(plan.steps[0].id.key == grounded);
        OA_CHECK(plan.steps[1].id.key == ridge_walls);
        OA_CHECK(plan.steps[2].id.key == gatehouse);
        OA_CHECK(plan.steps[3].id.key == gatehouse_keep);
    }
    const std::optional<lib::ActionButton> with = button_of(library, *keep, lib::Action::get_with);
    OA_CHECK(with.has_value() && with->enabled);
    if (with.has_value())
        check_text(with->text, "Get with 3 more");
}

/// A prerequisite the played mod cannot use is still downloaded, and does not block.
void an_incompatible_prerequisite_stays_in_the_plan() {
    lib::Inputs inputs = shelf();
    inputs.played.id = "ridge";
    inputs.played.version = "1.0";
    inputs.played.name = "Ridge";
    lib::Listing gate = mutator_listing(core, gatehouse, "Gatehouse", "4.1", 1, {});
    gate.mutator->works_with.push_back({"cinder", "", "Cinder"});
    inputs.listings.push_back(std::move(gate));
    inputs.listings.push_back(mutator_listing(
        core,
        gatehouse_keep,
        "Gatehouse Keep",
        "2.0",
        1,
        {prerequisite(gatehouse, ">= 4.0", "Gatehouse")}
    ));
    const lib::Library library = opened(std::move(inputs));
    const lib::Entry* gate_row = mutator_at(library, core, gatehouse);
    const lib::Entry* keep = mutator_at(library, core, gatehouse_keep);
    OA_CHECK(gate_row != nullptr && keep != nullptr);
    if (gate_row == nullptr || keep == nullptr)
        return;
    OA_CHECK(gate_row->not_compatible);
    const lib::GetPlan plan = lib::get_plan(library, *keep);
    OA_CHECK(plan.blocked.empty() && plan.steps.size() == 2);
    if (plan.steps.size() == 2)
        OA_CHECK(plan.steps[0].id.key == gatehouse);
    const std::optional<lib::ActionButton> with = button_of(library, *keep, lib::Action::get_with);
    OA_CHECK(with.has_value() && with->enabled);
}

/// A cycle is refused, naming both ids, and queues nothing.
void a_cycle_is_refused() {
    lib::Inputs inputs = shelf();
    inputs.listings.push_back(mutator_listing(
        core, gatehouse, "Gatehouse", "1.0", 1, {prerequisite(ridge_walls, "", "Ridge Walls")}
    ));
    inputs.listings.push_back(mutator_listing(
        core, ridge_walls, "Ridge Walls", "1.0", 1, {prerequisite(gatehouse, "", "Gatehouse")}
    ));
    const lib::Library library = opened(std::move(inputs));
    const lib::Entry* gate = mutator_at(library, core, gatehouse);
    OA_CHECK(gate != nullptr);
    if (gate == nullptr)
        return;
    const lib::GetPlan plan = lib::get_plan(library, *gate);
    OA_CHECK(plan.steps.empty());
    check_text(plan.blocked, "These mutators need each other: gatehouse and ridge-walls");
    const std::optional<lib::ActionButton> with = button_of(library, *gate, lib::Action::get_with);
    OA_CHECK(with.has_value() && !with->enabled);
}

/// The question names every package, in order, and its text fits at every class.
void the_question_lists_every_package() {
    lib::Inputs inputs = shelf();
    inputs.listings.push_back(mutator_listing(core, gatehouse, "Gatehouse", "4.1", 2, {}));
    inputs.listings.push_back(mutator_listing(
        core,
        gatehouse_keep,
        "Gatehouse Keep",
        "2.0",
        1,
        {prerequisite(gatehouse, ">= 4.0", "Gatehouse")}
    ));
    const lib::Library library = opened(std::move(inputs));
    const lib::Entry* keep = mutator_at(library, core, gatehouse_keep);
    OA_CHECK(keep != nullptr);
    if (keep == nullptr)
        return;
    const kit::Question asked = lib::get_with_question(library, *keep);
    check_text(asked.title, "Get Gatehouse Keep?");
    OA_CHECK(asked.paragraphs.size() == 1);
    if (!asked.paragraphs.empty()) {
        check_text(
            asked.paragraphs[0].text,
            "Gatehouse Keep needs Gatehouse 4.0 or later, which you don't have. Both download "
            "now, and Gatehouse goes above Gatehouse Keep in every list."
        );
    }
    OA_CHECK(asked.items.size() == 2);
    if (asked.items.size() == 2) {
        check_text(asked.items[0].text, "Gatehouse 4.1");
        check_text(asked.items[0].aside, " · from Fixture Core");
        OA_CHECK(asked.items[0].badge.tag.has_value());
        check_text(asked.items[1].text, "Gatehouse Keep 2.0");
        check_text(asked.items[1].aside, " · from Fixture Core");
    }
    OA_CHECK(asked.buttons.size() == 2);
    if (asked.buttons.size() == 2) {
        check_text(asked.buttons[0].caption, "Cancel");
        check_text(asked.buttons[0].id, "cancel");
        OA_CHECK(!asked.buttons[0].accent);
        check_text(asked.buttons[1].caption, "Get both");
        check_text(asked.buttons[1].id, "get-both");
        OA_CHECK(asked.buttons[1].accent);
    }
    OA_CHECK(asked.marked == 0 && asked.cancel_button == 0 && asked.primary_button == 1);
    OA_CHECK(kit::question_word_of(asked) == "prompt");
    for (const kit::SizeClass size_class :
         {kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large}) {
        kit::Question sized = asked;
        sized.size_class = size_class;
        OA_CHECK(!kit::place_question(sized, nullptr).cut);
        const kit::DisplayList list = kit::question_list(sized, nullptr);
        std::vector<std::string> names;
        for (const kit::Control& control : list.controls)
            names.push_back(control.name);
        OA_CHECK(
            names == std::vector<std::string>({"prompt.cancel", "prompt.get-both", "prompt.body"})
        );
    }

    lib::Inputs many_inputs = shelf();
    many_inputs.listings.push_back(mutator_listing(core, grounded, "Grounded", "1.0", 1, {}));
    many_inputs.listings.push_back(mutator_listing(
        core, ridge_walls, "Ridge Walls", "1.0", 1, {prerequisite(grounded, "", "Grounded")}
    ));
    many_inputs.listings.push_back(mutator_listing(
        core, gatehouse, "Gatehouse", "4.1", 1, {prerequisite(ridge_walls, "", "Ridge Walls")}
    ));
    many_inputs.listings.push_back(mutator_listing(
        core, gatehouse_keep, "Gatehouse Keep", "2.0", 1, {prerequisite(gatehouse, "", "Gatehouse")}
    ));
    const lib::Library many = opened(std::move(many_inputs));
    const lib::Entry* chain = mutator_at(many, core, gatehouse_keep);
    OA_CHECK(chain != nullptr);
    if (chain == nullptr)
        return;
    const kit::Question all = lib::get_with_question(many, *chain);
    OA_CHECK(all.items.size() == 4);
    if (!all.paragraphs.empty()) {
        check_text(
            all.paragraphs[0].text,
            "Gatehouse Keep needs Grounded, Ridge Walls and Gatehouse, which you don't have. "
            "They download first, and go above Gatehouse Keep in every list."
        );
    }
    if (all.buttons.size() == 2) {
        check_text(all.buttons[1].caption, "Get all");
        check_text(all.buttons[1].id, "get-all");
    }
    for (const kit::SizeClass size_class :
         {kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large}) {
        kit::Question sized = all;
        sized.size_class = size_class;
        OA_CHECK(!kit::place_question(sized, nullptr).cut);
        const kit::DisplayList list = kit::question_list(sized, nullptr);
        std::vector<std::string> names;
        for (const kit::Control& control : list.controls)
            names.push_back(control.name);
        OA_CHECK(
            names == std::vector<std::string>({"prompt.cancel", "prompt.get-all", "prompt.body"})
        );
    }
}

/// The details note says what downloads first.
void the_note_names_what_downloads_first() {
    lib::Inputs inputs = shelf();
    inputs.listings.push_back(mutator_listing(core, gatehouse, "Gatehouse", "4.1", 1, {}));
    inputs.listings.push_back(mutator_listing(
        core,
        gatehouse_keep,
        "Gatehouse Keep",
        "2.0",
        1,
        {prerequisite(gatehouse, ">= 4.0", "Gatehouse")}
    ));
    lib::Library library = opened(std::move(inputs));
    lib::select(library, {std::string(core), Kind::mutator, std::string(gatehouse_keep)});
    check_text(
        lib::details_note(library),
        "Gatehouse downloads first, then sits above Gatehouse Keep in every list."
    );

    lib::Inputs many_inputs = shelf();
    many_inputs.listings.push_back(mutator_listing(core, grounded, "Grounded", "1.0", 1, {}));
    many_inputs.listings.push_back(mutator_listing(
        core, ridge_walls, "Ridge Walls", "1.0", 1, {prerequisite(grounded, "", "Grounded")}
    ));
    many_inputs.listings.push_back(mutator_listing(
        core, gatehouse, "Gatehouse", "4.1", 1, {prerequisite(ridge_walls, "", "Ridge Walls")}
    ));
    many_inputs.listings.push_back(mutator_listing(
        core, gatehouse_keep, "Gatehouse Keep", "2.0", 1, {prerequisite(gatehouse, "", "Gatehouse")}
    ));
    lib::Library many = opened(std::move(many_inputs));
    lib::select(many, {std::string(core), Kind::mutator, std::string(gatehouse_keep)});
    check_text(
        lib::details_note(many),
        "Grounded, Ridge Walls and Gatehouse download first, then sit above Gatehouse Keep in "
        "every list."
    );
}

/// Uninstall says which installed mutators need the one being removed.
void uninstall_names_dependants() {
    const auto ask = [](lib::Inputs inputs) {
        lib::Library library = opened(std::move(inputs));
        const lib::Entry* gate = mutator_at(library, core, gatehouse);
        OA_CHECK(gate != nullptr);
        if (gate == nullptr)
            return kit::Question{};
        return lib::uninstall_question(library, *gate, "Mutators/gatehouse", library.text);
    };
    {
        lib::Inputs inputs = shelf();
        inputs.listings.push_back(mutator_listing(core, gatehouse, "Gatehouse", "4.1", 1, {}));
        inputs.installed.push_back(mutator_installed(core, gatehouse, "Gatehouse", "4.1", {}));
        inputs.listings.push_back(mutator_listing(
            core,
            gatehouse_keep,
            "Gatehouse Keep",
            "2.0",
            1,
            {prerequisite(gatehouse, ">= 4.0", "Gatehouse")}
        ));
        inputs.installed.push_back(mutator_installed(
            core,
            gatehouse_keep,
            "Gatehouse Keep",
            "2.0",
            {prerequisite(gatehouse, ">= 4.0", "Gatehouse")}
        ));
        const kit::Question asked = ask(std::move(inputs));
        OA_CHECK(asked.paragraphs.size() == 3);
        if (asked.paragraphs.size() == 3) {
            check_text(
                asked.paragraphs[0].text,
                "Uninstall Gatehouse? Its folder, Mutators/gatehouse, is removed."
            );
            check_text(
                asked.paragraphs[1].text,
                "Gatehouse Keep needs it: it stays installed, and cannot be switched on until "
                "this is back."
            );
            check_text(asked.paragraphs[2].text, "Get installs it again.");
        }
        OA_CHECK(asked.buttons.size() == 2 && asked.marked == 0);
    }
    {
        lib::Inputs inputs = shelf();
        inputs.listings.push_back(mutator_listing(core, gatehouse, "Gatehouse", "4.1", 1, {}));
        inputs.installed.push_back(mutator_installed(core, gatehouse, "Gatehouse", "4.1", {}));
        inputs.installed.push_back(mutator_installed(
            core, fast_builders, "Fast Builders", "1.0", {prerequisite(gatehouse, "", "Gatehouse")}
        ));
        inputs.installed.push_back(mutator_installed(
            core,
            gatehouse_keep,
            "Gatehouse Keep",
            "2.0",
            {prerequisite(gatehouse, "", "Gatehouse")}
        ));
        const kit::Question asked = ask(std::move(inputs));
        OA_CHECK(asked.paragraphs.size() == 3);
        if (asked.paragraphs.size() == 3) {
            check_text(
                asked.paragraphs[1].text,
                "Fast Builders and Gatehouse Keep need it: they stay installed, and cannot be "
                "switched on until this is back."
            );
        }
    }
}

/// A range reads in the shared words, and a range-free prerequisite leaves no blank.
void a_range_free_prerequisite_reads_with_no_blank() {
    OA_CHECK(lib::range_words(">= 4.0") == "4.0 or later");
    OA_CHECK(lib::range_words(">= 4.0, < 5.0") == "4.0 or later, before 5.0");
    OA_CHECK(lib::range_words("").empty());
    OA_CHECK(lib::range_words("about 4") == "about 4");

    lib::Inputs inputs = shelf();
    inputs.listings.push_back(mutator_listing(
        core, gatehouse_keep, "Gatehouse Keep", "2.0", 1, {prerequisite(gatehouse, "", "Gatehouse")}
    ));
    const lib::Library library = opened(std::move(inputs));
    const lib::Entry* keep = mutator_at(library, core, gatehouse_keep);
    OA_CHECK(keep != nullptr);
    if (keep == nullptr)
        return;
    const lib::GetPlan plan = lib::get_plan(library, *keep);
    OA_CHECK(plan.steps.empty());
    check_text(plan.blocked, "Needs Gatehouse, which no registry offers");
    OA_CHECK(plan.blocked.find(" ,") == std::string::npos);
}

/// A lookup translates the question's title and its button.
void a_lookup_translates() {
    lib::Inputs inputs = shelf();
    inputs.listings.push_back(mutator_listing(core, gatehouse, "Gatehouse", "4.1", 1, {}));
    inputs.listings.push_back(mutator_listing(
        core,
        gatehouse_keep,
        "Gatehouse Keep",
        "2.0",
        1,
        {prerequisite(gatehouse, ">= 4.0", "Gatehouse")}
    ));
    lib::Library library = opened(std::move(inputs));
    library.text.shown = [](void*, std::string_view english) -> std::string_view {
        if (english == lib::words::get_with_title)
            return "Obtenir {name} ?";
        if (english == lib::words::action_get_both)
            return "Tout obtenir";
        return english;
    };
    const lib::Entry* keep = mutator_at(library, core, gatehouse_keep);
    OA_CHECK(keep != nullptr);
    if (keep == nullptr)
        return;
    const kit::Question asked = lib::get_with_question(library, *keep);
    check_text(asked.title, "Obtenir Gatehouse Keep ?");
    if (asked.buttons.size() == 2)
        check_text(asked.buttons[1].caption, "Tout obtenir");
}

} // namespace

int main() {
    closures();
    outside_id();
    cycles();
    dependants();
    order_holds();
    long_chain();
    needs_follow_the_installed_manifest();
    plans_for_gatehouse_keep();
    a_chain_downloads_in_order();
    an_incompatible_prerequisite_stays_in_the_plan();
    a_cycle_is_refused();
    the_question_lists_every_package();
    the_note_names_what_downloads_first();
    uninstall_names_dependants();
    a_range_free_prerequisite_reads_with_no_blank();
    a_lookup_translates();
    return oa::test::check_exit_status();
}
