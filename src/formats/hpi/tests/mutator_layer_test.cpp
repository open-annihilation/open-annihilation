// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A mutator layer above the loose folders: one table of served and hidden
// paths, plus masks, mounted and replaced without changing the archives,
// the loose index or the pack layer.

#include "oa/base/threads.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

int failures = 0;

const char* kRidgeA = "units/RIDGEA.fbi";
const char* kRidgeB = "units/RIDGEB.fbi";
const char* kWeapon = "weapons/ridge.tdf";
const char* kDefault = "ai/default.txt";
const char* kTurbo = "ai/turbo.txt";
const char* kSub = "ai/sub/x.txt";

const char* kPaths[] = {kRidgeA, kRidgeB, kWeapon, kDefault, kTurbo, kSub};

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

Bytes text(std::string_view value) {
    return Bytes(value.begin(), value.end());
}

const Bytes kBaseA = text("base-a");
const Bytes kBaseWeapon = text("base-weapon");
const Bytes kBaseDefault = text("base-default");
const Bytes kModB = text("mod-b");
const Bytes kModTurbo = text("mod-turbo");
const Bytes kModSub = text("mod-sub");
const Bytes kHostA = text("host-a");
const Bytes kLayerB = text("layer-b");
const Bytes kBytesWeapon = text("bytes-weapon");
const Bytes kPackA = text("pack-a");
const Bytes kRidgeC = text("ridge-c");
const Bytes kKeptA = text("kept-a");
const Bytes kOne = text("mut-one");
const Bytes kTwo = text("mut-two");

std::shared_ptr<const Bytes> hold(Bytes bytes) {
    return std::make_shared<const Bytes>(std::move(bytes));
}

bool digits(std::string_view value) {
    if (value.empty())
        return false;
    for (const unsigned char character : value)
        if (character < '0' || character > '9')
            return false;
    return true;
}

std::string fold(std::string_view path) {
    std::string folded;
    folded.reserve(path.size());
    for (unsigned char character : path) {
        if (character == '\\')
            character = '/';
        if (character >= 'A' && character <= 'Z')
            character = static_cast<unsigned char>(character - 'A' + 'a');
        folded.push_back(static_cast<char>(character));
    }
    return folded;
}

void write_bytes(const fs::path& path, const Bytes& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    if (!stream)
        throw std::runtime_error("cannot write " + path.string());
}

Bytes hpi_bytes(std::vector<oa::HpiWriteFile> files) {
    return oa::write_hpi(files);
}

/// Two loose roots and two archives, each holding the same six paths.
struct World {
    fs::path root;
    fs::path base;
    fs::path mod;
    fs::path archive_one;
    fs::path archive_two;
    fs::path host_file;
    fs::path layer_hpi;
    fs::path pack_dir;
    std::shared_ptr<const oa::HpiArchive> layer_archive;

    World() : root(oa::test::make_scratch_directory("F01-mutator-layer")) {
        base = root / "base";
        mod = root / "mod";
        write_bytes(base / "units" / "RIDGEA.fbi", kBaseA);
        write_bytes(base / "weapons" / "ridge.tdf", kBaseWeapon);
        write_bytes(base / "ai" / "default.txt", kBaseDefault);
        write_bytes(mod / "units" / "RIDGEB.fbi", kModB);
        write_bytes(mod / "ai" / "turbo.txt", kModTurbo);
        write_bytes(mod / "ai" / "sub" / "x.txt", kModSub);

        archive_one = root / "one.hpi";
        archive_two = root / "two.hpi";
        write_bytes(
            archive_one,
            hpi_bytes({
                {"units/RIDGEA.fbi", text("arc1-a")},
                {"units/RIDGEB.fbi", text("arc1-b")},
                {"weapons/ridge.tdf", text("arc1-weapon")},
                {"ai/default.txt", text("arc1-default")},
                {"ai/turbo.txt", text("arc1-turbo")},
                {"ai/sub/x.txt", text("arc1-sub")},
            })
        );
        write_bytes(
            archive_two,
            hpi_bytes({
                {"units/RIDGEA.fbi", text("arc2-a")},
                {"units/RIDGEB.fbi", text("arc2-b")},
                {"weapons/ridge.tdf", text("arc2-weapon")},
                {"ai/default.txt", text("arc2-default")},
                {"ai/turbo.txt", text("arc2-turbo")},
                {"ai/sub/x.txt", text("arc2-sub")},
            })
        );
        host_file = root / "host" / "ridgea.fbi";
        write_bytes(host_file, kHostA);
        layer_hpi = root / "layer.hpi";
        auto layer_bytes = hpi_bytes({
            {"units/RIDGEB.fbi", kLayerB},
            {"ai/sub/x.txt", text("layer-sub")},
            {"units/sub/x.txt", text("layer-nested")},
        });
        write_bytes(layer_hpi, layer_bytes);
        auto opened = oa::HpiArchive::open(std::move(layer_bytes));
        if (!opened.ok())
            throw std::runtime_error("layer archive");
        layer_archive = std::make_shared<const oa::HpiArchive>(std::move(*opened.value));
        pack_dir = root / "pack";
        fs::create_directories(pack_dir);
    }

    ~World() {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    oa::AssetStore store() const {
        oa::AssetStore built(std::vector<fs::path>{mod, base});
        built.mount(archive_one);
        built.mount(archive_two);
        return built;
    }
};

oa::MutatorLayerEntry loose_entry(std::string path, std::string owner, const fs::path& host) {
    oa::MutatorLayerEntry entry;
    entry.path = std::move(path);
    entry.owner = std::move(owner);
    entry.kind = oa::MutatorSourceKind::loose;
    entry.host = host;
    return entry;
}

oa::MutatorLayerEntry
archive_entry(std::string path, std::string owner, std::size_t archive, std::string archive_path) {
    oa::MutatorLayerEntry entry;
    entry.path = std::move(path);
    entry.owner = std::move(owner);
    entry.kind = oa::MutatorSourceKind::archive;
    entry.archive = archive;
    entry.archive_path = std::move(archive_path);
    return entry;
}

oa::MutatorLayerEntry bytes_entry(std::string path, std::string owner, Bytes bytes) {
    oa::MutatorLayerEntry entry;
    entry.path = std::move(path);
    entry.owner = std::move(owner);
    entry.kind = oa::MutatorSourceKind::contents;
    entry.contents = hold(std::move(bytes));
    return entry;
}

oa::MutatorLayerEntry hidden_entry(std::string path, std::string owner) {
    oa::MutatorLayerEntry entry;
    entry.path = std::move(path);
    entry.owner = std::move(owner);
    entry.kind = oa::MutatorSourceKind::hidden;
    return entry;
}

oa::MutatorLayerSpec layer_with(std::string label, std::vector<oa::MutatorLayerEntry> entries) {
    oa::MutatorLayerSpec spec;
    spec.label = std::move(label);
    spec.entries = std::move(entries);
    return spec;
}

bool throws_not_found(oa::AssetStore& store, std::string_view path) {
    try {
        (void)store.read(path);
    } catch (const std::runtime_error& error) {
        return std::string(error.what()).find("asset not found") != std::string::npos;
    }
    return false;
}

bool has_name(const std::vector<oa::FoundEntry>& found, std::string_view name) {
    const auto folded = fold(name);
    for (const auto& entry : found)
        if (!entry.directory && fold(entry.name) == folded)
            return true;
    return false;
}

bool same_found(const std::vector<oa::FoundEntry>& left, const std::vector<oa::FoundEntry>& right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& one = left[index];
        const auto& other = right[index];
        if (one.name != other.name || one.directory != other.directory || one.size != other.size ||
            one.mount != other.mount || one.pack_layer != other.pack_layer ||
            one.mutator_layer != other.mutator_layer)
            return false;
    }
    return true;
}

bool has_key(const std::vector<std::string>& names, std::string_view key) {
    return std::find(names.begin(), names.end(), key) != names.end();
}

bool has_scan(const std::vector<std::string>& paths, std::string_view key) {
    for (const auto& path : paths)
        if (fold(path) == key)
            return true;
    return false;
}

std::vector<fs::path> mount_list(const oa::AssetStore& store) {
    const auto mounts = store.mount_paths();
    return {mounts.begin(), mounts.end()};
}

struct Fingerprint {
    std::vector<fs::path> mounts;
    bool loose_index = false;
    bool mutator = false;
    std::optional<std::string> label;
    std::vector<std::string> paths;
    bool pack = false;
    std::optional<std::string> pack_label;
    std::map<std::string, Bytes> reads;
    std::vector<std::string> listed;
};

Fingerprint fingerprint(oa::AssetStore& store) {
    Fingerprint print;
    print.mounts = mount_list(store);
    print.loose_index = store.loose_index_enabled();
    print.mutator = store.mutator_layer_mounted();
    print.label = store.mutator_layer_label();
    print.paths = store.mutator_layer_paths();
    print.pack = store.pack_layer_mounted();
    print.pack_label = store.pack_layer_label();
    for (const char* path : kPaths)
        print.reads.emplace(path, store.read(path).bytes);
    print.listed = store.list_effective_recursive("", "");
    return print;
}

void check_same(const Fingerprint& before, oa::AssetStore& store, const std::string& why) {
    const Fingerprint after = fingerprint(store);
    check(after.mounts == before.mounts, why + ": mounts changed");
    check(after.loose_index == before.loose_index, why + ": loose index changed");
    check(after.mutator == before.mutator, why + ": mutator mount changed");
    check(after.label == before.label, why + ": label changed");
    check(after.paths == before.paths, why + ": paths changed");
    check(after.pack == before.pack, why + ": pack mount changed");
    check(after.pack_label == before.pack_label, why + ": pack label changed");
    check(after.reads == before.reads, why + ": a read changed");
    check(after.listed == before.listed, why + ": a listing changed");
}

void check_handle(oa::AssetStore& store, std::string_view path, const Bytes& bytes, bool archived) {
    check(store.file_size(path) == bytes.size(), std::string(path) + " file_size");
    oa::ResourceFile* file = store.open(path);
    check(file != nullptr, std::string(path) + " opens");
    if (file == nullptr)
        return;
    check(oa::AssetStore::archived(file) == archived, std::string(path) + " handle archived");
    check(oa::AssetStore::length(file) == bytes.size(), std::string(path) + " length");
    Bytes got(bytes.size());
    check(
        oa::AssetStore::read(file, got) == static_cast<int32_t>(got.size()) && got == bytes,
        std::string(path) + " handle read"
    );
    check(oa::AssetStore::seek(file, 1) != -1, std::string(path) + " seek");
    check(oa::AssetStore::tell(file) == 1, std::string(path) + " tell");
    Bytes tail(bytes.size() - 1);
    check(
        oa::AssetStore::read(file, tail) == static_cast<int32_t>(tail.size()) &&
            tail == Bytes(bytes.begin() + 1, bytes.end()),
        std::string(path) + " read after seek"
    );
    oa::AssetStore::close(file);
    const auto loaded = store.load_file_contents(path);
    check(loaded == bytes, std::string(path) + " load_file_contents");
}

void check_provider(
    const oa::Provider& provider,
    oa::ProviderKind kind,
    const fs::path& path,
    std::string_view owner,
    std::string* serial
) {
    check(provider.kind == kind, "provider kind");
    check(provider.path == path, "provider path " + provider.path.string());
    const std::string prefix = "mutator:" + std::string(owner) + "#";
    check(provider.identity.starts_with(prefix), "identity " + provider.identity);
    const auto rest = provider.identity.substr(prefix.size());
    check(digits(rest), "serial in " + provider.identity);
    if (serial != nullptr)
        *serial = std::string(rest);
}

bool hidden_everywhere(oa::AssetStore& store, std::string_view path, std::string_view key) {
    const bool read_missing = throws_not_found(store, path) && !store.read_if_present(path);
    const bool open_missing = store.open(path) == nullptr;
    const bool size_missing = store.file_size(path) == 0;
    const bool provider_missing = store.provider(path).kind == oa::ProviderKind::none;
    const bool effective = !has_key(store.list_effective("units", ".fbi"), key);
    const bool ordered = !has_key(store.list_effective_in_mount_order("units", ".fbi"), key);
    const bool recursive = !has_key(store.list_effective_recursive("", ""), key);
    const bool found = !has_name(store.find("units\\*.fbi"), "RIDGEA.fbi");
    const bool scanned = !has_scan(store.scan_recursive("units", "*"), key);
    return read_missing && open_missing && size_missing && provider_missing && effective &&
           ordered && recursive && found && scanned;
}

void nothing_above_changes() {
    World world;
    auto store = world.store();
    fs::create_directories(world.pack_dir);
    oa::PackLayerSpec pack;
    pack.kind = oa::PackLayerKind::folder;
    pack.location = world.pack_dir;
    pack.label = "ridge pack";
    pack.files.push_back({"maps/ridge.ota", "maps/ridge.ota", hold(text("pack-ota"))});
    std::string error;
    check(store.mount_pack_layer(pack, &error), "the pack mounts: " + error);
    const auto mounts = mount_list(store);
    const bool loose_index = store.loose_index_enabled();
    std::map<std::string, oa::AssetData> reads;
    std::map<std::string, oa::Provider> providers;
    for (const char* path : kPaths) {
        reads.emplace(path, store.read(path));
        providers.emplace(path, store.provider(path));
    }
    const auto weapons = store.find("weapons\\*");
    const auto units = store.find("units\\*");
    const auto listed = store.list_effective_recursive("", "");
    const auto pack_bytes = store.read_pack_layer("maps/ridge.ota");

    auto spec = layer_with("ridge layer", {bytes_entry("units/RIDGEC.fbi", "ridge", kRidgeC)});
    check(store.mount_mutator_layer(spec, &error), "the layer mounts: " + error);
    check(store.mutator_layer_mounted(), "the layer is mounted");
    check(store.mutator_layer_label() == std::optional<std::string>{"ridge layer"}, "the label");
    check(
        store.mutator_layer_paths() == std::vector<std::string>{"units/RIDGEC.fbi"},
        "served paths, hidden ones left out"
    );
    check(mount_list(store) == mounts, "mount_paths() is unchanged");
    check(store.loose_index_enabled() == loose_index, "the loose index is unchanged");
    check(store.pack_layer_mounted(), "the pack layer stays mounted");
    check(
        store.pack_layer_label() == std::optional<std::string>{"ridge pack"}, "the pack label stays"
    );
    check(store.read_pack_layer("maps/ridge.ota") == pack_bytes, "the pack's own bytes stay");
    for (const char* path : kPaths) {
        const auto data = store.read(path);
        check(data.bytes == reads.at(path).bytes, std::string(path) + " bytes stay");
        check(data.source == reads.at(path).source, std::string(path) + " source stays");
        check(data.archived == reads.at(path).archived, std::string(path) + " archived stays");
        check(
            store.provider(path).identity == providers.at(path).identity,
            std::string(path) + " provider"
        );
    }
    check(
        same_found(store.find("weapons\\*"), weapons),
        "a directory the layer does not touch is unchanged"
    );
    const auto units_now = store.find("units\\*");
    check(!units_now.empty(), "the units folder lists files");
    if (!units_now.empty()) {
        check(units_now.front().mutator_layer, "the layer's file is first");
        check(
            units_now.front().mount == oa::formats::hpi::MutatorLayerMount, "the layer mount index"
        );
        check(
            units_now.front().name == "RIDGEC.fbi" && !units_now.front().pack_layer, "RIDGEC.fbi"
        );
        check(
            same_found({units_now.begin() + 1, units_now.end()}, units),
            "the old units matches follow the layer"
        );
    }
    const auto listed_now = store.list_effective_recursive("", "");
    check(
        !listed_now.empty() && listed_now.front() == "units/ridgec.fbi", "the layer's path is first"
    );
    if (!listed_now.empty()) {
        check(
            std::vector<std::string>(listed_now.begin() + 1, listed_now.end()) == listed,
            "the old listing follows the layer"
        );
    }
    const auto alone =
        store.find("units\\*", oa::FindScope{oa::formats::hpi::MutatorLayerMount, false});
    check(
        alone.size() == 1 && alone.front().mutator_layer && alone.front().name == "RIDGEC.fbi",
        "FindScope of the layer mount lists the layer alone"
    );
    check(store.read("units/RIDGEC.fbi").bytes == kRidgeC, "the added file reads");
}

void unmount_restores_exactly() {
    World world;
    auto store = world.store();
    const Fingerprint before = fingerprint(store);
    const auto units = store.find("units\\*");
    const auto weapons = store.find("weapons\\*");
    std::map<std::string, oa::Provider> providers;
    for (const char* path : kPaths)
        providers.emplace(path, store.provider(path));
    std::string error;
    auto spec = layer_with("ridge layer", {bytes_entry(kRidgeA, "ridge", kKeptA)});
    check(store.mount_mutator_layer(spec, &error), error);
    const auto first = store.provider(kRidgeA).identity;
    check(store.unmount_mutator_layer(), "unmount reports the layer");
    check(!store.mutator_layer_mounted(), "the layer is gone");
    check(!store.mutator_layer_label(), "the label is gone");
    check(store.mutator_layer_paths().empty(), "the path list is gone");
    check_same(before, store, "unmount");
    check(same_found(store.find("units\\*"), units), "find(units) is restored");
    check(same_found(store.find("weapons\\*"), weapons), "find(weapons) is restored");
    for (const char* path : kPaths)
        check(store.provider(path).identity == providers.at(path).identity, std::string(path));
    check(!store.unmount_mutator_layer(), "unmounting again reports that nothing was mounted");
    check(store.mount_mutator_layer(spec, &error), "the layer mounts again: " + error);
    const auto second = store.provider(kRidgeA).identity;
    check(second != first, "mounting again gives a new serial");
    check(second.starts_with("mutator:ridge#"), second);
}

void each_source_serves() {
    World world;
    auto store = world.store();
    oa::MutatorLayerSpec spec;
    spec.label = "ridge layer";
    spec.archives.push_back({world.layer_archive, world.layer_hpi, "ridge"});
    spec.entries.push_back(loose_entry(kRidgeA, "ridge", world.host_file));
    spec.entries.push_back(archive_entry(kRidgeB, "ridge", 0, "units/RIDGEB.fbi"));
    spec.entries.push_back(bytes_entry(kWeapon, "ridge", kBytesWeapon));
    std::string error;
    check(store.mount_mutator_layer(spec, &error), error);

    const auto loose = store.read(kRidgeA);
    check(loose.bytes == kHostA && !loose.archived, "a loose host file is not archived");
    check(loose.source == world.host_file, "the loose source is the host file");
    const auto archived = store.read(kRidgeB);
    check(archived.bytes == kLayerB && archived.archived, "a layer archive file is archived");
    check(archived.source == world.layer_hpi, "the archive source is the location");
    const auto given = store.read(kWeapon);
    check(given.bytes == kBytesWeapon && given.archived, "given bytes are archived");
    check(given.source.empty(), "given bytes have an empty source");

    std::string serial;
    check_provider(
        store.provider(kRidgeA), oa::ProviderKind::mutator_layer, world.host_file, "ridge", &serial
    );
    std::string archive_serial;
    check_provider(
        store.provider(kRidgeB),
        oa::ProviderKind::mutator_layer,
        world.layer_hpi,
        "ridge",
        &archive_serial
    );
    std::string bytes_serial;
    check_provider(
        store.provider(kWeapon), oa::ProviderKind::mutator_layer, {}, "ridge", &bytes_serial
    );
    check(serial == archive_serial && serial == bytes_serial, "one layer shares a serial");
    check(store.provided_above_pack_layer(kRidgeA), "a served path is above the pack");
    check(!store.providing_archive(kWeapon), "the layer is not an archive mount");
    const auto skipped = store.read_without(kRidgeA, world.archive_one);
    check(skipped.bytes == kHostA, "read_without still answers from the layer");

    check_handle(store, kRidgeA, kHostA, false);
    check_handle(store, kRidgeB, kLayerB, true);
    check_handle(store, kWeapon, kBytesWeapon, true);
    check(store.loose_file(kRidgeA).has_value(), "loose_file stays the loose folders' answer");
}

void served_path_wins() {
    World world;
    auto store = world.store();
    std::string error;
    check(
        store.mount_mutator_layer(
            layer_with("ridge layer", {bytes_entry(kRidgeA, "ridge", kKeptA)}), &error
        ),
        error
    );
    for (const char* asked : {"units/RIDGEA.fbi", "UNITS\\ridgea.fbi", "Units/RidgeA.FBI"}) {
        const auto data = store.read(asked);
        check(data.bytes == kKeptA, std::string(asked) + " reads the layer");
        check(store.provider(asked).kind == oa::ProviderKind::mutator_layer, std::string(asked));
    }
    check(
        store.loose_file("UNITS\\RIDGEA.fbi") == store.loose_file(kRidgeA),
        "loose_file ignores the layer"
    );
}

void hidden_entry_is_missing() {
    World world;
    auto store = world.store();
    const int before = store.count_entries("units\\*.fbi", false);
    std::string error;
    check(
        store.mount_mutator_layer(
            layer_with("ridge layer", {hidden_entry(kRidgeA, "ridge-hide")}), &error
        ),
        error
    );
    check(hidden_everywhere(store, kRidgeA, "units/ridgea.fbi"), "a hidden entry is missing");
    check(store.count_entries("units\\*.fbi", false) < before, "count_entries leaves it out");
    check(store.provider(kRidgeA).identity.empty(), "a hidden path has an empty provider");
    check(!store.provided_above_pack_layer(kRidgeA), "a hidden path is not provided");
    check(!store.providing_archive(kRidgeA), "a hidden path names no archive");
    check(store.loose_file(kRidgeA).has_value(), "loose_file still finds the loose file");
    check(store.mutator_layer_paths().empty(), "hidden paths are not listed");

    check(store.unmount_mutator_layer(), "the hide unmounts");
    oa::PackLayerSpec pack;
    pack.kind = oa::PackLayerKind::folder;
    pack.location = world.pack_dir;
    pack.label = "ridge pack";
    pack.files.push_back({kRidgeA, kRidgeA, hold(kPackA)});
    check(store.mount_pack_layer(pack, &error), "the pack mounts: " + error);
    check(
        store.mount_mutator_layer(
            layer_with("ridge layer", {hidden_entry(kRidgeA, "ridge-hide")}), &error
        ),
        error
    );
    check(
        hidden_everywhere(store, kRidgeA, "units/ridgea.fbi"),
        "a hidden entry stays missing above a pack"
    );
    check(store.read_pack_layer(kRidgeA) == kPackA, "the pack's own copy is untouched");
    check(
        store.pack_layer_paths() == std::vector<std::string>{kRidgeA},
        "the pack's list is untouched"
    );
}

void masks_hide_files() {
    check(oa::mutator_mask_matches("ai/*.txt", "ai/default.txt"), "star matches one segment");
    check(oa::mutator_mask_matches("ai/*.txt", "AI\\DEFAULT.TXT"), "case and slash");
    check(!oa::mutator_mask_matches("ai/*.txt", "ai/sub/x.txt"), "star does not cross a segment");
    check(
        !oa::mutator_mask_matches("ai/*.txt", "ai/default.tdf"),
        "star matches the rest of the segment"
    );
    check(oa::mutator_mask_matches("ai/t?rbo.txt", "ai/turbo.txt"), "question mark");
    check(
        !oa::mutator_mask_matches("ai/t?rbo.txt", "ai/trbo.txt"), "question mark needs a character"
    );
    check(
        !oa::mutator_mask_matches("ai/t?rbo.txt", "ai/tuurbo.txt"), "question mark is one character"
    );
    check(oa::mutator_mask_matches("**/x.txt", "x.txt"), "a leading ** matches none");
    check(oa::mutator_mask_matches("**/x.txt", "a/b/x.txt"), "a leading ** matches segments");
    check(oa::mutator_mask_matches("ai/**/x.txt", "ai/x.txt"), "a middle ** matches none");
    check(oa::mutator_mask_matches("ai/**/x.txt", "ai/sub/x.txt"), "a middle ** matches segments");
    check(oa::mutator_mask_matches("units/**", "units"), "a trailing ** matches none");
    check(oa::mutator_mask_matches("units/**", "units/a"), "a trailing ** matches one");
    check(oa::mutator_mask_matches("units/**", "units/a/b"), "a trailing ** matches several");
    check(
        !oa::mutator_mask_matches("units/**", "weapons/ridge.tdf"),
        "a trailing ** stays under its prefix"
    );
    check(oa::mutator_mask_matches("Units/*.FBI", "units\\ridgea.fbi"), "case and slash together");
    check(oa::mutator_mask_problem("*").empty(), "a star segment is valid");
    check(oa::mutator_mask_problem("*a*").empty(), "stars inside a segment are valid");

    // Each pattern keeps its own length, so "units/\0*" holds its embedded NUL.
    const std::string_view refused[] = {
        std::string_view{"", 0},
        std::string_view{"/units/*", 8},
        std::string_view{"\\units\\*", 8},
        std::string_view{"c:/units/*", 10},
        std::string_view{"units/\0*", 8},
        std::string_view{"units//a", 8},
        std::string_view{"units/", 6},
        std::string_view{"units/./a", 9},
        std::string_view{"units/../a", 10},
        std::string_view{".backup/a", 9},
        std::string_view{".BACKUP/*", 9},
        std::string_view{"*****************", 17},
        std::string_view{"***", 3},
        std::string_view{"a**b", 4},
        std::string_view{"**.fbi", 6},
    };
    for (const std::string_view pattern : refused) {
        const std::string text_pattern(pattern);
        check(!oa::mutator_mask_problem(text_pattern).empty(), "refused: " + fold(text_pattern));
        check(
            !oa::mutator_mask_matches(text_pattern, "units/a"), "a refused pattern matches nothing"
        );
    }
    std::string too_long(oa::formats::hpi::MutatorMaskByteLimit + 1, 'a');
    check(!oa::mutator_mask_problem(too_long).empty(), "a pattern past the byte limit is refused");

    World world;
    auto store = world.store();
    oa::MutatorLayerSpec spec;
    spec.label = "ridge layer";
    spec.entries.push_back(bytes_entry(kRidgeA, "ridge", kKeptA));
    spec.masks.push_back({"units/**", "ridge-mask"});
    spec.masks.push_back({"ai/*.txt", "ridge-mask"});
    std::string error;
    check(store.mount_mutator_layer(spec, &error), error);
    check(store.read(kRidgeA).bytes == kKeptA, "a table entry inside a mask is still served");
    check(throws_not_found(store, kRidgeB), "units/** hides the other unit");
    check(throws_not_found(store, kDefault), "ai/*.txt hides default.txt");
    check(throws_not_found(store, kTurbo), "ai/*.txt hides turbo.txt");
    check(store.read(kSub).bytes == kModSub, "ai/*.txt leaves ai/sub/x.txt");
    check(
        !has_key(store.list_effective("ai", ".txt"), "ai/default.txt"), "default.txt is not listed"
    );
    check(!has_key(store.list_effective("ai", ".txt"), "ai/turbo.txt"), "turbo.txt is not listed");
    check(
        has_key(store.list_effective_recursive("ai", ""), "ai/sub/x.txt"),
        "ai/sub/x.txt stays listed"
    );
    check(!has_name(store.find("units\\*.fbi"), "RIDGEB.fbi"), "find leaves the masked unit out");
    check(has_name(store.find("units\\*.fbi"), "RIDGEA.fbi"), "find keeps the served unit");
    check(!has_scan(store.scan_recursive("ai", "*"), "ai/default.txt"), "scan leaves the mask out");
    check(has_scan(store.scan_recursive("ai", "*"), "ai/sub/x.txt"), "scan keeps the nested file");
}

void expect_refused(
    oa::AssetStore& store, const oa::MutatorLayerSpec& spec, const Fingerprint& before
) {
    std::string error = "unchanged";
    check(!store.mount_mutator_layer(spec, &error), "mount was refused");
    check(!error.empty() && error != "unchanged", "mount names the reason");
    check_same(before, store, "refused mount");
    error = "unchanged";
    check(!store.replace_mutator_layer(spec, &error), "replace was refused");
    check(!error.empty() && error != "unchanged", "replace names the reason");
    check_same(before, store, "refused replace");
}

oa::MutatorLayerSpec labeled(std::string label) {
    oa::MutatorLayerSpec spec;
    spec.label = std::move(label);
    return spec;
}

void refusals() {
    World world;
    auto store = world.store();
    const Fingerprint before = fingerprint(store);

    {
        oa::MutatorLayerSpec spec = labeled("ridge");
        spec.entries.resize(oa::formats::hpi::MutatorLayerEntryLimit + 1);
        spec.entries.back().path = "units/toomany.fbi";
        spec.entries.back().owner = "ridge-limit";
        std::string error;
        check(!store.mount_mutator_layer(spec, &error), "too many paths");
        check(error.find("units/toomany.fbi") != std::string::npos, error);
        check(error.find("ridge-limit") != std::string::npos, error);
        check_same(before, store, "too many paths");
    }
    {
        oa::MutatorLayerSpec spec = labeled("ridge");
        spec.masks.resize(oa::formats::hpi::MutatorLayerPatternLimit + 1);
        spec.masks.back().pattern = "extra/*.txt";
        spec.masks.back().owner = "ridge-limit";
        std::string error;
        check(!store.mount_mutator_layer(spec, &error), "too many masks");
        check(error.find("extra/*.txt") != std::string::npos, error);
        check(error.find("ridge-limit") != std::string::npos, error);
        check_same(before, store, "too many masks");
    }
    {
        oa::MutatorLayerSpec spec = labeled("ridge");
        spec.archives.resize(oa::formats::hpi::MutatorLayerArchiveLimit + 1);
        spec.archives.back().owner = "ridge-limit";
        std::string error;
        check(!store.mount_mutator_layer(spec, &error), "too many archives");
        check(error.find("ridge-limit") != std::string::npos, error);
        check(
            error.find("null") == std::string::npos, "the count is checked before a null archive"
        );
        check_same(before, store, "too many archives");
    }

    expect_refused(store, labeled(""), before);
    expect_refused(store, labeled("ridge\nlayer"), before);

    {
        auto spec = labeled("ridge");
        spec.entries.push_back(hidden_entry(kRidgeA, ""));
        expect_refused(store, spec, before);
    }
    {
        auto spec = labeled("ridge");
        spec.entries.push_back(hidden_entry(kRidgeA, "ridge\nhide"));
        expect_refused(store, spec, before);
    }
    {
        auto spec = labeled("ridge");
        spec.masks.push_back({"ai/*.txt", ""});
        expect_refused(store, spec, before);
    }
    {
        auto spec = labeled("ridge");
        spec.masks.push_back({"ai/*.txt", "ridge\nmask"});
        expect_refused(store, spec, before);
    }
    {
        auto spec = labeled("ridge");
        spec.archives.push_back({world.layer_archive, world.layer_hpi, ""});
        expect_refused(store, spec, before);
    }
    {
        auto spec = labeled("ridge");
        spec.archives.push_back({world.layer_archive, world.layer_hpi, "ridge\narch"});
        expect_refused(store, spec, before);
    }

    for (const char* path :
         {"",
          "/units/a.fbi",
          "\\units\\a.fbi",
          "c:units/a.fbi",
          ".backup/a.fbi",
          "units/../a.fbi",
          "units/./a.fbi"}) {
        auto spec = labeled("ridge");
        spec.entries.push_back(hidden_entry(path, "ridge"));
        expect_refused(store, spec, before);
    }
    {
        auto spec = labeled("ridge");
        std::string nul_path("units/");
        nul_path.push_back('\0');
        nul_path += "a.fbi";
        spec.entries.push_back(hidden_entry(nul_path, "ridge"));
        expect_refused(store, spec, before);
    }
    {
        auto spec = labeled("ridge");
        spec.entries.push_back(hidden_entry(std::string(4097, 'a'), "ridge"));
        expect_refused(store, spec, before);
    }
    {
        auto spec = labeled("ridge");
        spec.entries.push_back(hidden_entry("units/RIDGEA.fbi", "ridge"));
        spec.entries.push_back(hidden_entry("UNITS\\ridgea.fbi", "ridge-other"));
        std::string error;
        check(!store.mount_mutator_layer(spec, &error), "alike paths");
        check(error.find("UNITS\\ridgea.fbi") != std::string::npos, error);
        check(error.find("ridge-other") != std::string::npos, error);
        check_same(before, store, "alike paths");
    }
    {
        auto spec = labeled("ridge");
        spec.masks.push_back({"a**b", "ridge-mask"});
        std::string error;
        check(!store.mount_mutator_layer(spec, &error), "a bad mask");
        check(error.find("a**b") != std::string::npos, error);
        check(error.find("ridge-mask") != std::string::npos, error);
        check_same(before, store, "a bad mask");
    }
    {
        auto spec = labeled("ridge");
        spec.entries.push_back(loose_entry(kRidgeA, "ridge", world.root));
        expect_refused(store, spec, before);
    }
    {
        const auto over = world.root / "over.bin";
        write_bytes(over, text("x"));
        std::error_code resize_error;
        fs::resize_file(over, oa::formats::hpi::EntryByteLimit + 1, resize_error);
        if (resize_error) {
            std::cout << "skipped the loose entry limit case: " << resize_error.message() << '\n';
        } else {
            auto spec = labeled("ridge");
            spec.entries.push_back(loose_entry("units/over.bin", "ridge", over));
            std::string error;
            check(!store.mount_mutator_layer(spec, &error), "a loose file past the entry limit");
            check(error.find("units/over.bin") != std::string::npos, error);
            check(error.find("ridge") != std::string::npos, error);
            check_same(before, store, "loose entry limit");
        }
    }
    {
        auto spec = labeled("ridge");
        spec.archives.push_back({world.layer_archive, world.layer_hpi, "ridge"});
        spec.entries.push_back(archive_entry(kRidgeA, "ridge", 4, "units/RIDGEA.fbi"));
        std::string error;
        check(!store.mount_mutator_layer(spec, &error), "archive index");
        check(error.find(kRidgeA) != std::string::npos, error);
        check_same(before, store, "archive index");
    }
    {
        auto spec = labeled("ridge");
        spec.archives.push_back({nullptr, world.layer_hpi, "ridge"});
        std::string error;
        check(!store.mount_mutator_layer(spec, &error), "null archive");
        check(error.find("ridge") != std::string::npos, error);
        check_same(before, store, "null archive");
    }
    {
        auto spec = labeled("ridge");
        spec.archives.push_back({world.layer_archive, world.layer_hpi, "ridge"});
        spec.entries.push_back(archive_entry(kRidgeA, "ridge", 0, "missing.txt"));
        expect_refused(store, spec, before);
        spec.entries.clear();
        spec.entries.push_back(archive_entry("units/sub", "ridge", 0, "units/sub"));
        expect_refused(store, spec, before);
    }
    {
        auto spec = labeled("ridge");
        oa::MutatorLayerEntry entry;
        entry.path = kRidgeA;
        entry.owner = "ridge";
        entry.kind = oa::MutatorSourceKind::contents;
        spec.entries.push_back(entry);
        expect_refused(store, spec, before);
    }
    {
        auto spec = labeled("ridge");
        spec.entries.push_back(bytes_entry("units/a.fbi", "ridge", text("a")));
        spec.entries.push_back(bytes_entry("units/a.fbi/b.fbi", "ridge", text("b")));
        expect_refused(store, spec, before);
        spec.entries.clear();
        spec.entries.push_back(bytes_entry("units/RIDGEA.fbi", "ridge", text("a")));
        spec.entries.push_back(bytes_entry("units", "ridge", text("b")));
        expect_refused(store, spec, before);
    }
    {
        auto huge =
            std::make_shared<Bytes>(static_cast<std::size_t>(oa::formats::hpi::EntryByteLimit + 1));
        auto spec = labeled("ridge");
        oa::MutatorLayerEntry entry;
        entry.path = "units/huge.fbi";
        entry.owner = "ridge";
        entry.kind = oa::MutatorSourceKind::contents;
        entry.contents = huge;
        spec.entries.push_back(entry);
        std::string error;
        check(!store.mount_mutator_layer(spec, &error), "contents past the entry limit");
        check(error.find("units/huge.fbi") != std::string::npos, error);
        check(error.find("entry size limit") != std::string::npos, error);
        check_same(before, store, "contents entry limit");
    }
    {
        auto huge = std::make_shared<Bytes>(
            static_cast<std::size_t>(oa::formats::hpi::MutatorLayerContentByteLimit + 1)
        );
        auto spec = labeled("ridge");
        oa::MutatorLayerEntry entry;
        entry.path = "units/sum.fbi";
        entry.owner = "ridge-sum";
        entry.kind = oa::MutatorSourceKind::contents;
        entry.contents = huge;
        spec.entries.push_back(std::move(entry));
        std::string error;
        check(!store.mount_mutator_layer(spec, &error), "contents past the content limit");
        check(error.find("units/sum.fbi") != std::string::npos, error);
        check(error.find("ridge-sum") != std::string::npos, error);
        check(error.find("content limit") != std::string::npos, error);
        check_same(before, store, "content limit");
    }

    std::string error;
    auto good = layer_with("ridge layer", {bytes_entry(kWeapon, "ridge", kOne)});
    check(store.mount_mutator_layer(good, &error), error);
    const Fingerprint mounted = fingerprint(store);
    error = "unchanged";
    check(!store.mount_mutator_layer(good, &error), "a second layer is refused");
    check(error == "a mutator layer is mounted already: ridge layer", error);
    check_same(mounted, store, "refused second mount");
    auto bad = labeled("");
    check(!store.replace_mutator_layer(bad, &error), "a bad replace is refused");
    check(store.read(kWeapon).bytes == kOne, "a refused replace keeps the mounted bytes");
    check(
        store.mutator_layer_label() == std::optional<std::string>{"ridge layer"}, "the label stays"
    );
    check_same(mounted, store, "refused replace");

    check(store.unmount_mutator_layer(), "the good layer unmounts");
    auto hidden_prefix = labeled("ridge");
    hidden_prefix.entries.push_back(hidden_entry("units", "ridge-hide"));
    hidden_prefix.entries.push_back(bytes_entry(kRidgeA, "ridge", kKeptA));
    check(
        store.mount_mutator_layer(hidden_prefix, &error),
        "a hidden file does not collide with a served path below it: " + error
    );
    check(store.read(kRidgeA).bytes == kKeptA, "the served path below a hidden prefix reads");
    check(throws_not_found(store, "units"), "the hidden prefix is missing");
}

struct RaceState {
    oa::AssetStore* store = nullptr;
    std::string failure;
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
};

void note_failure(RaceState* state, std::string message) {
    state->failure = std::move(message);
    state->failed.store(true);
}

void read_during_replace(void* argument) {
    auto* state = static_cast<RaceState*>(argument);
    for (int round = 0; !state->stop.load() || round < 200; ++round) {
        try {
            const auto data = state->store->read(kWeapon);
            if (data.bytes != kOne && data.bytes != kTwo) {
                note_failure(state, "a read returned bytes from neither layer");
                return;
            }
        } catch (const std::exception& error) {
            note_failure(state, std::string("read failed: ") + error.what());
            return;
        } catch (...) {
            note_failure(state, "read failed with an unknown exception");
            return;
        }
    }
}

void readers_survive_unmount() {
    World world;
    auto store = world.store();
    oa::MutatorLayerSpec spec;
    spec.label = "ridge layer";
    spec.archives.push_back({world.layer_archive, world.layer_hpi, "ridge"});
    spec.entries.push_back(loose_entry(kRidgeA, "ridge", world.host_file));
    spec.entries.push_back(archive_entry(kRidgeB, "ridge", 0, "units/RIDGEB.fbi"));
    spec.entries.push_back(bytes_entry(kWeapon, "ridge", kBytesWeapon));
    std::string error;
    check(store.mount_mutator_layer(spec, &error), error);
    oa::ResourceFile* loose = store.open(kRidgeA);
    oa::ResourceFile* archived = store.open(kRidgeB);
    oa::ResourceFile* given = store.open(kWeapon);
    check(loose != nullptr && archived != nullptr && given != nullptr, "the three handles open");
    check(store.unmount_mutator_layer(), "the layer unmounts with the handles open");
    auto read_handle = [](oa::ResourceFile* file, const Bytes& bytes, const char* name) {
        if (file == nullptr)
            return;
        Bytes got(oa::AssetStore::length(file));
        check(
            oa::AssetStore::read(file, got) == static_cast<int32_t>(got.size()) && got == bytes,
            std::string(name) + " still reads after unmount"
        );
        oa::AssetStore::close(file);
    };
    read_handle(loose, kHostA, "loose");
    read_handle(archived, kLayerB, "archive");
    read_handle(given, kBytesWeapon, "bytes");
}

void replace_while_reading() {
    World world;
    auto store = world.store();
    auto first = layer_with("ridge one", {bytes_entry(kWeapon, "ridge", kOne)});
    auto second = layer_with("ridge two", {bytes_entry(kWeapon, "ridge-next", kTwo)});
    std::string error;
    check(store.mount_mutator_layer(first, &error), error);
    RaceState state;
    state.store = &store;
    oa::base::threads::Thread thread;
    check(
        oa::base::threads::start_thread(thread, read_during_replace, &state), "the reader starts"
    );
    std::string replace_failure;
    for (int round = 0; round < 200 && !state.failed.load(); ++round) {
        if (!store.replace_mutator_layer(round % 2 == 0 ? second : first, &error)) {
            replace_failure = "replace failed: " + error;
            break;
        }
    }
    state.stop.store(true);
    oa::base::threads::join_thread(thread);
    if (!replace_failure.empty())
        check(false, replace_failure);
    else
        check(state.failure.empty(), state.failure.empty() ? "the reader finished" : state.failure);
    check(
        store.read(kWeapon).bytes == kOne || store.read(kWeapon).bytes == kTwo,
        "the path is still served after the replaces"
    );
}

void discover_keeps_layer() {
    World world;
    auto store = world.store();
    std::string error;
    check(
        store.mount_mutator_layer(
            layer_with("ridge layer", {bytes_entry(kWeapon, "ridge", kBytesWeapon)}), &error
        ),
        error
    );
    store.mark_loose_shadows();
    check(store.mutator_layer_mounted(), "mark_loose_shadows leaves the layer mounted");
    check(store.read(kWeapon).bytes == kBytesWeapon, "mark_loose_shadows leaves the served bytes");
    (void)store.discover(oa::DiscoveryPlan{});
    check(store.mutator_layer_mounted(), "discover leaves the layer mounted");
    check(
        store.mutator_layer_label() == std::optional<std::string>{"ridge layer"},
        "discover leaves the label"
    );
    check(store.read(kWeapon).bytes == kBytesWeapon, "discover leaves the served bytes");
    check(store.loose_file(kRidgeA).has_value(), "discover leaves the loose file");
}

void moved_store_keeps_layer() {
    World world;
    auto store = world.store();
    std::string error;
    check(
        store.mount_mutator_layer(
            layer_with("ridge layer", {bytes_entry(kWeapon, "ridge", kBytesWeapon)}), &error
        ),
        error
    );
    const auto identity = store.provider(kWeapon).identity;
    oa::AssetStore moved(std::move(store));
    check(!store.mutator_layer_mounted(), "a moved-from store has no layer");
    check(moved.mutator_layer_mounted(), "a moved store keeps the layer");
    check(
        moved.mutator_layer_label() == std::optional<std::string>{"ridge layer"},
        "a moved store keeps the label"
    );
    check(moved.read(kWeapon).bytes == kBytesWeapon, "a moved store reads the layer");
    check(moved.provider(kWeapon).identity == identity, "a moved store keeps the identity");
    oa::AssetStore assigned(world.base);
    assigned = std::move(moved);
    check(assigned.mutator_layer_mounted(), "move assignment keeps the layer");
    check(assigned.read(kWeapon).bytes == kBytesWeapon, "the assigned store reads the layer");
}

struct Note {
    std::string path;
    std::string owner;
    bool hidden = false;
};

struct Watch {
    std::vector<Note> notes;
};

void on_mutator(void* context, std::string_view path, std::string_view owner, bool hidden) {
    static_cast<Watch*>(context)->notes.push_back({std::string(path), std::string(owner), hidden});
}

int note_count(const Watch& watch, std::string_view path, std::string_view owner, bool hidden) {
    int count = 0;
    for (const auto& note : watch.notes)
        if (note.path == path && note.owner == owner && note.hidden == hidden)
            ++count;
    return count;
}

void observer_names_the_mutator() {
    World world;
    auto store = world.store();
    oa::MutatorLayerSpec spec;
    spec.label = "ridge layer";
    spec.entries.push_back(bytes_entry(kRidgeA, "ridge", kKeptA));
    spec.entries.push_back(hidden_entry(kRidgeB, "ridge-hide"));
    spec.masks.push_back({"ai/*.txt", "ridge-mask"});
    std::string error;
    check(store.mount_mutator_layer(spec, &error), error);
    Watch watch;
    store.observe_lookups({&watch, nullptr, on_mutator});
    check(store.read("UNITS\\RIDGEA.fbi").bytes == kKeptA, "the served read");
    check(
        note_count(watch, "UNITS\\RIDGEA.fbi", "ridge", false) == 1,
        "a served read names the mutator"
    );
    watch.notes.clear();
    check(throws_not_found(store, "units/RIDGEB.fbi"), "the hidden read");
    check(
        note_count(watch, "units/RIDGEB.fbi", "ridge-hide", true) == 1,
        "a hidden lookup names the mutator"
    );
    watch.notes.clear();
    const auto listed = store.list_effective_recursive("ai", ".txt");
    check(
        !has_key(listed, "ai/default.txt") && !has_key(listed, "ai/turbo.txt"),
        "the mask hides both"
    );
    check(has_key(listed, "ai/sub/x.txt"), "the nested file stays");
    check(
        note_count(watch, "ai/default.txt", "ridge-mask", true) == 1,
        "a listing reports default.txt once"
    );
    check(
        note_count(watch, "ai/turbo.txt", "ridge-mask", true) == 1,
        "a listing reports turbo.txt once"
    );
    check(
        note_count(watch, "ai/sub/x.txt", "ridge-mask", true) == 0,
        "a kept path is not reported hidden"
    );
    for (const auto& note : watch.notes)
        check(note.hidden, "a listing does not report a served path as hidden");
}

} // namespace

int main() {
    try {
        nothing_above_changes();
        unmount_restores_exactly();
        each_source_serves();
        served_path_wins();
        hidden_entry_is_missing();
        masks_hide_files();
        refusals();
        readers_survive_unmount();
        replace_while_reading();
        discover_keeps_layer();
        moved_store_keeps_layer();
        observer_names_the_mutator();
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " mutator layer check(s) failed\n";
        return 1;
    }
    std::cout << "hpi mutator layer tests passed\n";
    return 0;
}
