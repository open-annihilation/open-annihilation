// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// post_opened_file sends each extension, in any case, to its queue: a .oareg
// file to the add-registry queue, which keeps order and drops a repeat; a
// .oademo or a .tad file to the replay queue, which does the same; and every
// other file of the seven to the package queue. A replay leaves the package
// queue and the add-registry queue untouched.

#include "oa/app/package_install/inbox.hpp"
#include "oa/test/check.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace install = oa::app::package_install;

std::string file_name(const std::optional<std::filesystem::path>& file) {
    return file ? file->filename().string() : std::string{};
}

std::string file_name(const std::optional<install::OpenedPackage>& opened) {
    return opened ? opened->file.filename().string() : std::string{};
}

} // namespace

int main() {
    OA_CHECK(install::opens_file("a.oamod"));
    OA_CHECK(install::opens_file("B.OAMAP"));
    OA_CHECK(install::opens_file("c.oareg"));
    OA_CHECK(install::opens_file("d.OALANG"));
    OA_CHECK(install::opens_file("e.OAMUT"));
    OA_CHECK(install::opens_file("e.OABRAIN"));
    OA_CHECK(install::opens_file("l.OAKEYS"));
    OA_CHECK(!install::opens_file("a.zip"));
    OA_CHECK(!install::opens_file("oamod"));
    OA_CHECK(!install::opens_file("notes.oamod.txt"));
    OA_CHECK(!install::opens_file("a.oademo"));
    OA_CHECK(!install::opens_file("a.tad"));
    OA_CHECK(install::opens_replay("a.oademo"));
    OA_CHECK(install::opens_replay("B.TAD"));
    OA_CHECK(!install::opens_replay("a.oamod"));
    OA_CHECK(!install::opens_replay("a.zip"));

    for (const char* name :
         {"a.oamod",
          "B.OAMOD",
          "c.oalang",
          "D.OALANG",
          "e.oamap",
          "F.OAMAP",
          "i.oamut",
          "J.OAMUT",
          "k.oabrain",
          "L.OABRAIN",
          "m.oakeys",
          "N.OAKEYS",
          "g.oareg",
          "H.OAREG"})
        install::post_opened_file(name);

    std::vector<std::string> packages;
    while (install::package_files_waiting()) {
        const auto file = install::take_package_file();
        packages.push_back(file_name(file));
        install::finish_package_file();
    }
    OA_CHECK(packages.size() == 12);
    if (packages.size() == 12) {
        OA_CHECK(packages[0] == "a.oamod");
        OA_CHECK(packages[1] == "B.OAMOD");
        OA_CHECK(packages[2] == "c.oalang");
        OA_CHECK(packages[3] == "D.OALANG");
        OA_CHECK(packages[4] == "e.oamap");
        OA_CHECK(packages[5] == "F.OAMAP");
        OA_CHECK(packages[6] == "i.oamut");
        OA_CHECK(packages[7] == "J.OAMUT");
        OA_CHECK(packages[8] == "k.oabrain");
        OA_CHECK(packages[9] == "L.OABRAIN");
        OA_CHECK(packages[10] == "m.oakeys");
        OA_CHECK(packages[11] == "N.OAKEYS");
    }

    std::vector<std::string> registries;
    while (install::registry_files_waiting())
        registries.push_back(file_name(install::take_registry_file()));
    OA_CHECK(registries.size() == 2);
    if (registries.size() == 2) {
        OA_CHECK(registries[0] == "g.oareg");
        OA_CHECK(registries[1] == "H.OAREG");
    }
    OA_CHECK(!install::replay_files_waiting());

    // The registry queue keeps order and drops a repeat of the same path.
    install::post_registry_file("one.oareg");
    install::post_registry_file("two.oareg");
    install::post_registry_file("one.oareg");
    install::post_registry_file("./one.oareg");
    OA_CHECK(file_name(install::take_registry_file()) == "one.oareg");
    OA_CHECK(file_name(install::take_registry_file()) == "two.oareg");
    OA_CHECK(!install::take_registry_file());
    OA_CHECK(!install::registry_files_waiting());

    // A .oareg file leaves the package queue untouched.
    install::post_package_file("keep.oamod");
    install::post_opened_file("only.OAREG");
    OA_CHECK(install::package_files_waiting());
    OA_CHECK(file_name(install::take_package_file()) == "keep.oamod");
    install::finish_package_file();
    OA_CHECK(!install::package_files_waiting());
    OA_CHECK(install::registry_files_waiting());
    OA_CHECK(file_name(install::take_registry_file()) == "only.OAREG");
    OA_CHECK(!install::registry_files_waiting());
    OA_CHECK(!install::package_files_waiting());

    // The package queue drops a repeat under another spelling of the same path.
    install::post_package_file("a.oamod");
    install::post_package_file("./a.oamod");
    OA_CHECK(file_name(install::take_package_file()) == "a.oamod");
    install::finish_package_file();
    OA_CHECK(!install::take_package_file());
    OA_CHECK(!install::package_files_waiting());

    // A path through a parent step is the same file.
    install::post_registry_file("sub/../one.oareg");
    install::post_registry_file("one.oareg");
    OA_CHECK(file_name(install::take_registry_file()) == "one.oareg");
    OA_CHECK(!install::take_registry_file());
    OA_CHECK(!install::registry_files_waiting());

    // A replay goes to its own queue, and a repeat of the same path is dropped.
    install::post_opened_file("show.oademo");
    install::post_opened_file("anteer.TAD");
    install::post_opened_file("keep.oamod");
    install::post_replay_file("show.oademo");
    install::post_replay_file("./show.oademo");
    OA_CHECK(install::package_files_waiting());
    OA_CHECK(file_name(install::take_package_file()) == "keep.oamod");
    install::finish_package_file();
    OA_CHECK(!install::package_files_waiting());
    OA_CHECK(!install::registry_files_waiting());
    OA_CHECK(file_name(install::take_replay_file()) == "show.oademo");
    OA_CHECK(file_name(install::take_replay_file()) == "anteer.TAD");
    OA_CHECK(!install::take_replay_file());
    OA_CHECK(!install::replay_files_waiting());

    return oa::test::check_exit_status();
}
