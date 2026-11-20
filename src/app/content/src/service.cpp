// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The public face of the content service: the rules, the snapshot and the
// calls the main thread makes.
#include "service_state.hpp"

#include <chrono>
#include <exception>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app::content {
namespace {

/// Appends a URL that is not already listed.
///
/// @param urls the list
/// @param url the URL
void append_unique(std::vector<formats::url::Url>& urls, const formats::url::Url& url) {
    for (const formats::url::Url& have : urls) {
        if (have == url)
            return;
    }
    urls.push_back(url);
}

/// Reports whether a view's packages belong in the snapshot.
///
/// @param view the registry
/// @return true when it holds a catalogue and the rules let it give entries
bool gives_entries(const RegistryView& view) {
    if (!view.catalogue)
        return false;
    switch (view.status) {
    case RegistryStatus::needs_developer_mode:
    case RegistryStatus::conflicting:
    case RegistryStatus::disabled:
        return false;
    case RegistryStatus::never_fetched:
    case RegistryStatus::fresh:
    case RegistryStatus::out_of_date:
    case RegistryStatus::failed:
        return true;
    }
    return false;
}

/// Reads the built-in descriptors. A missing folder is not an error.
///
/// @param state the service
/// @return the descriptors that were kept
std::vector<data::registry::Descriptor> read_builtins(ServiceState& state) {
    if (state.options.builtin_folder.empty())
        return {};
    std::error_code error;
    if (!std::filesystem::is_directory(state.options.builtin_folder, error))
        return {};
    std::vector<std::string> problems;
    std::vector<data::registry::Descriptor> builtins =
        data::registry::read_builtin_registries(state.options.builtin_folder, &problems);
    for (const std::string& problem : problems)
        log_line(state, problem);
    return builtins;
}

/// Reads Registries.yaml. An unreadable file is remembered and never written.
///
/// @param state the service
void read_player(ServiceState& state) {
    state.player = {};
    state.player_load = data::registry::LoadResult::missing;
    state.registries_file_error.clear();
    if (state.options.player_folder.empty())
        return;
    const std::filesystem::path file =
        state.options.player_folder / std::string(data::registry::player_registries_file);
    std::string error;
    state.player_load = data::registry::load_player_registries(file, state.player, &error);
    if (state.player_load == data::registry::LoadResult::unreadable) {
        state.registries_file_error =
            error.empty() ? std::string("Registries.yaml could not be read") : std::move(error);
    }
}

/// Logs a worker that could not be started. The caller does not hold the mutex.
///
/// @param state the service
/// @param started false when ensure_worker failed
void log_worker_failure(ServiceState& state, bool started) {
    if (!started)
        log_line(state, "the catalogue worker could not be started");
}

} // namespace

ServiceState::ServiceState(ServiceOptions options_in)
    : options(std::move(options_in)), snapshot(std::make_shared<const Snapshot>()) {
}

int64_t now_seconds(const ServiceState& state) noexcept {
    if (state.options.clock)
        return state.options.clock();
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

void log_line(const ServiceState& state, std::string_view line) {
    if (state.options.log)
        state.options.log(state.options.log_context, line);
}

std::filesystem::path content_directory(const ServiceState& state) {
    if (state.options.data_folder.empty())
        return {};
    return state.options.data_folder / std::string(content_folder_name);
}

bool can_fetch(const RegistryRecord& record, bool developer_mode) {
    if (record.registry.conflicting || !record.registry.enabled)
        return false;
    if (!record.registry.descriptor.is_signed())
        return developer_mode && data::registry::loopback_only(record.registry.descriptor);
    return true;
}

RegistryStatus status_of(const RegistryRecord& record, bool developer_mode) {
    if (record.registry.conflicting)
        return RegistryStatus::conflicting;
    if (!record.registry.enabled)
        return RegistryStatus::disabled;
    if (!record.registry.descriptor.is_signed() && !developer_mode)
        return RegistryStatus::needs_developer_mode;
    if (record.refresh_failed)
        return RegistryStatus::failed;
    if (record.catalogue)
        return record.expired ? RegistryStatus::out_of_date : RegistryStatus::fresh;
    return RegistryStatus::never_fetched;
}

std::vector<formats::url::Url> sources_of(const RegistryRecord& record) {
    std::vector<formats::url::Url> urls;
    append_unique(urls, record.registry.descriptor.catalogue);
    for (const formats::url::Url& mirror : record.registry.descriptor.mirrors)
        append_unique(urls, mirror);
    if (record.registry.origin == data::registry::Origin::added) {
        for (const formats::url::Url& mirror : record.registry.player_mirrors)
            append_unique(urls, mirror);
    }
    if (record.catalogue) {
        for (const formats::url::Url& mirror : record.catalogue->mirrors)
            append_unique(urls, mirror);
    }
    return urls;
}

RegistryView view_of(const RegistryRecord& record) {
    RegistryView view;
    view.registry = record.registry;
    view.status = record.status;
    view.catalogue = record.catalogue;
    view.served_from = record.served_from;
    view.sources = record.sources;
    view.checked = record.checked;
    view.expired = record.expired;
    view.refreshing = record.refreshing;
    view.last_error = record.last_error;
    view.fingerprints = data::registry::key_fingerprints(record.registry.descriptor);
    view.releases = record.releases;
    return view;
}

bool queue_refresh(ServiceState& state, std::size_t index, RefreshReason reason, int64_t now) {
    if (index >= state.registries.size())
        return false;
    RegistryRecord& record = state.registries[index];
    if (record.retired || record.refreshing)
        return false;
    if (!can_fetch(record, state.options.developer_mode))
        return false;
    if (reason == RefreshReason::start && !state.options.automatic)
        return false;
    if (!refresh_due(reason, state.options.check, now, record.checked, record.last_attempt))
        return false;
    record.refreshing = true;
    record.last_attempt = now;
    WorkItem item;
    item.kind = WorkItem::Kind::refresh;
    item.index = index;
    state.queue.push_back(std::move(item));
    state.wake.notify_one();
    return true;
}

void publish(ServiceState& state) {
    std::vector<RegistryView> views;
    views.reserve(state.registries.size());
    for (const RegistryRecord& record : state.registries) {
        if (record.retired)
            continue;
        views.push_back(view_of(record));
    }
    Snapshot snapshot = make_snapshot(std::move(views), state.registries_file_error);
    snapshot.generation = state.generation + 1;
    auto shared = std::make_shared<const Snapshot>(std::move(snapshot));
    state.generation = shared->generation;
    state.snapshot = std::move(shared);
}

bool refresh_due(
    RefreshReason reason,
    CheckForUpdates check,
    int64_t now,
    std::optional<int64_t> checked,
    std::optional<int64_t> last_attempt
) noexcept {
    if (reason == RefreshReason::check_now)
        return true;
    if (reason == RefreshReason::start) {
        if (check != CheckForUpdates::automatically)
            return false;
        if (!checked)
            return true;
        if (now < *checked)
            return false;
        return now - *checked > start_refresh_age_seconds;
    }
    if (check == CheckForUpdates::never)
        return false;
    if (!last_attempt)
        return true;
    if (now < *last_attempt)
        return false;
    return now - *last_attempt >= library_refresh_gap_seconds;
}

Snapshot make_snapshot(std::vector<RegistryView> registries, std::string registries_file_error) {
    Snapshot snapshot;
    snapshot.registries_file_error = std::move(registries_file_error);
    snapshot.registries = std::move(registries);
    for (const RegistryView& view : snapshot.registries) {
        if (!gives_entries(view))
            continue;
        for (const data::catalogue::Package& package : view.catalogue->packages) {
            Entry entry;
            entry.registry = view.registry.descriptor.id;
            entry.package = &package;
            entry.reviewed = view.registry.origin == data::registry::Origin::built_in;
            snapshot.entries.push_back(std::move(entry));
        }
    }
    return snapshot;
}

std::vector<formats::url::Url>
reference_urls(const RegistryView& view, std::string_view reference) {
    std::vector<formats::url::Url> resolved;
    if (!view.catalogue)
        return resolved;
    std::vector<formats::url::Url> bases;
    if (view.served_from)
        append_unique(bases, *view.served_from);
    for (const formats::url::Url& source : view.sources)
        append_unique(bases, source);
    for (const formats::url::Url& base : bases) {
        const std::optional<formats::url::Url> url =
            formats::url::resolve_reference(base, reference);
        if (url)
            append_unique(resolved, *url);
    }
    return resolved;
}

Service::Service(ServiceOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {
}

Service::~Service() {
    impl_->cancel.store(true);
    bool join = false;
    {
        base::threads::LockGuard guard(impl_->mutex);
        impl_->stop = true;
        impl_->queue.clear();
        impl_->listeners.clear();
        join = impl_->thread_started;
        impl_->wake.notify_all();
    }
    if (join)
        base::threads::join_thread(impl_->thread);
}

void Service::start() {
    {
        base::threads::LockGuard guard(impl_->mutex);
        if (impl_->started || impl_->stop)
            return;
    }

    read_player(*impl_);
    const std::vector<data::registry::Descriptor> builtins = read_builtins(*impl_);
    std::vector<data::registry::RegistryInEffect> in_effect =
        data::registry::registries_in_effect(builtins, impl_->player);
    std::vector<RegistryRecord> records;
    records.reserve(in_effect.size());
    for (data::registry::RegistryInEffect& registry : in_effect) {
        RegistryRecord record;
        record.registry = std::move(registry);
        record.sources = sources_of(record);
        read_cache(*impl_, record);
        records.push_back(std::move(record));
    }

    bool worker_started = true;
    {
        base::threads::LockGuard guard(impl_->mutex);
        if (impl_->stop || impl_->started)
            return;
        impl_->registries = std::move(records);
        for (RegistryRecord& record : impl_->registries)
            record.status = status_of(record, impl_->options.developer_mode);
        impl_->started = true;
        const int64_t now = now_seconds(*impl_);
        for (std::size_t index = 0; index < impl_->registries.size(); ++index)
            queue_refresh(*impl_, index, RefreshReason::start, now);
        worker_started = ensure_worker(*impl_);
        publish(*impl_);
    }
    log_worker_failure(*impl_, worker_started);
}

void Service::refresh(RefreshReason reason, std::string_view registry) {
    bool worker_started = true;
    bool queued = false;
    {
        base::threads::LockGuard guard(impl_->mutex);
        if (!impl_->started || impl_->stop)
            return;
        const int64_t now = now_seconds(*impl_);
        for (std::size_t index = 0; index < impl_->registries.size(); ++index) {
            if (impl_->registries[index].retired)
                continue;
            if (!registry.empty() && impl_->registries[index].registry.descriptor.id != registry)
                continue;
            if (queue_refresh(*impl_, index, reason, now))
                queued = true;
        }
        if (!queued)
            return;
        worker_started = ensure_worker(*impl_);
        publish(*impl_);
    }
    log_worker_failure(*impl_, worker_started);
}

void Service::set_developer_mode(bool on) {
    bool worker_started = true;
    {
        base::threads::LockGuard guard(impl_->mutex);
        if (impl_->stop)
            return;
        const bool was = impl_->options.developer_mode;
        if (was == on)
            return;
        impl_->options.developer_mode = on;
        if (!impl_->started)
            return;
        const int64_t now = now_seconds(*impl_);
        for (std::size_t index = 0; index < impl_->registries.size(); ++index) {
            RegistryRecord& record = impl_->registries[index];
            if (record.retired)
                continue;
            const bool became_fetchable = on && !can_fetch(record, was) && can_fetch(record, on);
            record.status = status_of(record, on);
            if (became_fetchable && !record.catalogue && !record.refreshing) {
                record.refreshing = true;
                record.last_attempt = now;
                WorkItem item;
                item.kind = WorkItem::Kind::refresh;
                item.index = index;
                impl_->queue.push_back(std::move(item));
                impl_->wake.notify_one();
            }
        }
        worker_started = ensure_worker(*impl_);
        publish(*impl_);
    }
    log_worker_failure(*impl_, worker_started);
}

void Service::set_check_for_updates(CheckForUpdates check) {
    base::threads::LockGuard guard(impl_->mutex);
    if (impl_->stop)
        return;
    impl_->options.check = check;
}

void Service::run_on_worker(WorkerJob job) {
    if (!job)
        return;
    bool worker_started = true;
    {
        base::threads::LockGuard guard(impl_->mutex);
        if (impl_->stop)
            return;
        WorkItem item;
        item.kind = WorkItem::Kind::job;
        item.job = std::move(job);
        impl_->queue.push_back(std::move(item));
        impl_->wake.notify_one();
        worker_started = ensure_worker(*impl_);
    }
    log_worker_failure(*impl_, worker_started);
}

void Service::on_refreshed(RefreshListener listener) {
    if (!listener)
        return;
    base::threads::LockGuard guard(impl_->mutex);
    if (impl_->stop)
        return;
    impl_->listeners.push_back(std::move(listener));
}

std::filesystem::path Service::content_folder() const {
    return content_directory(*impl_);
}

std::shared_ptr<const Snapshot> Service::snapshot() const {
    base::threads::LockGuard guard(impl_->mutex);
    return impl_->snapshot;
}

uint64_t Service::generation() const noexcept {
    base::threads::LockGuard guard(impl_->mutex);
    return impl_->generation;
}

} // namespace oa::app::content
