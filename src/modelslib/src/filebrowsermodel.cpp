#include <ds/filebrowsermodel.h>
#include <QCryptographicHash>
#include <QLocale>
#include <QSettings>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <logfault/logfault.h>

namespace ds::models {
namespace {
constexpr int max_history = 32, max_cache = 8, max_depth = 64;
constexpr int max_selection = 128, max_download_batch = 16;
constexpr int max_entries = 120000, max_pages = 4096;
constexpr quint64 max_metadata_bytes = 96ULL << 20;
quint64 metadataBytes(const BrowserNode &node) {
    // Includes conservative container/hash/allocation overhead and spare vector capacity.
    return 2 * sizeof(BrowserNode) + 192 + 2 * quint64(node.info.id.size() + node.info.parent.size() +
        node.info.sha256.size()) + 4 * quint64(node.path.size() + node.info.name.size());
}
QString routePath(const QStringList &route) { return route.isEmpty() ? QStringLiteral("/") : "/" + route.join('/'); }
bool validRoute(const QStringList &route) {
    if (route.size() > max_depth || routePath(route).toUtf8().size() > 4096) return false;
    for (const auto &name : route)
        if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains(QChar(0)) ||
            name.toUtf8().size() > prot::fileshare::max_component_bytes * 3) return false;
    return true;
}
}
FileBrowserModel::FileBrowserModel(QObject *parent) : QAbstractListModel(parent) {
    QSettings settings;
    const auto mode = settings.value("fileBrowser/viewMode", int(Details)).toInt();
    if (mode >= Thumbnails && mode <= Details) view_mode_ = ViewMode(mode);
    const auto order = settings.value("fileBrowser/details/columnOrder").toStringList();
    // Accept only a complete permutation of the fixed display-only schema.
    if (order.size() == columns_.size()) {
        auto sorted = columns_;
        bool valid = true;
        QSet<QString> seen;
        for (int i = 0; i < order.size(); ++i) {
            const auto it = std::find_if(columns_.cbegin(), columns_.cend(), [&](const auto &c) { return c.id == order[i]; });
            if (it == columns_.cend() || seen.contains(order[i])) { valid = false; break; }
            seen.insert(order[i]); sorted[i] = *it;
        }
        if (valid) columns_ = sorted;
    }
    for (auto &column : columns_) {
        const auto prefix = "fileBrowser/details/" + column.id;
        column.visible = column.id == "name" || settings.value(prefix + "/visible", column.visible).toBool();
        column.width = std::clamp(settings.value(prefix + "/width", column.width).toInt(), 80, 640);
    }
    prefetch_.setSingleShot(true); prefetch_.setInterval(16);
    connect(&prefetch_, &QTimer::timeout, this, [this] {
        if (!downloads_running_ && !busy_ && more() && viewport_first_ >= 0 && viewport_last_ + overscan_ >= rowCount() && empty_prefetch_ < 4)
            nextPage();
    });
    live_refresh_.setSingleShot(true); live_refresh_.setInterval(100);
    connect(&live_refresh_, &QTimer::timeout, this, &FileBrowserModel::refresh);
    connect(this, &FileBrowserModel::selectionChanged, this, [this] {
        last_download_allowed_ = canDownloadSelection(); last_context_allowed_ = contextCanDownload();
    });
    auto *timer = new QTimer(this);
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, [this] {
        // Check in-memory connection/policy state only; never poll the remote VFS.
        if (provider_ && !provider_->current() && (state_ != Unavailable || !provider_->cachedMetadataAllowed())) disconnected();
        const bool allowed = canDownloadSelection(), context_allowed = contextCanDownload();
        if (allowed != last_download_allowed_ || context_allowed != last_context_allowed_) {
            last_download_allowed_ = allowed; last_context_allowed_ = context_allowed; emit selectionChanged();
        }
    });
    timer->start();
}
void FileBrowserModel::setViewMode(ViewMode mode) {
    if (mode < Thumbnails || mode > Details || mode == view_mode_) return;
    view_mode_ = mode; saveViewSettings();
    LFLOG_DEBUG << "FileShare browser display mode=" << int(mode);
    emit viewSettingsChanged();
}
QVariantList FileBrowserModel::detailColumns() const {
    QVariantList result;
    for (const auto &c : columns_) {
        const auto title = c.id == "name" ? tr("Name") : c.id == "type" ? tr("Type") :
                           c.id == "size" ? tr("Size") : c.id == "modified" ? tr("Modified") : tr("Status");
        result.append(QVariantMap{{"id", c.id}, {"title", title}, {"visible", c.visible}, {"width", c.width}});
    }
    return result;
}
void FileBrowserModel::saveViewSettings() {
    // Store presentation preferences only, never peer metadata or folder routes.
    QSettings settings; settings.setValue("fileBrowser/viewMode", int(view_mode_));
    QStringList order;
    for (const auto &c : columns_) {
        order.append(c.id);
        const auto prefix = "fileBrowser/details/" + c.id;
        settings.setValue(prefix + "/visible", c.visible); settings.setValue(prefix + "/width", c.width);
    }
    settings.setValue("fileBrowser/details/columnOrder", order);
}
void FileBrowserModel::setColumnVisible(QString id, bool visible) {
    for (auto &c : columns_) if (c.id == id && c.id != "name" && c.visible != visible) {
        c.visible = visible; saveViewSettings(); emit viewSettingsChanged(); return;
    }
}
void FileBrowserModel::moveColumn(QString id, int position) {
    if (position < 0 || position >= columns_.size()) return;
    for (int i = 0; i < columns_.size(); ++i) if (columns_[i].id == id && i != position) {
        columns_.move(i, position); saveViewSettings(); emit viewSettingsChanged(); return;
    }
}
void FileBrowserModel::setColumnWidth(QString id, int width) {
    if (width < 80 || width > 640) return;
    for (auto &c : columns_) if (c.id == id && c.width != width) {
        c.width = width; saveViewSettings(); emit viewSettingsChanged(); return;
    }
}
QString FileBrowserModel::columnText(int row, QString id) const {
    if (row < 0 || row >= entries_.size()) return {};
    const auto &entry = entries_[row];
    const bool directory = entry.info.type == dsfm::EntryType::Directory;
    if (id == "name") return entry.info.name;
    if (id == "type") return directory ? tr("Folder") : tr("File");
    if (id == "size") {
        if (directory || !entry.size_known) return {};
        if (entry.info.size > quint64(std::numeric_limits<qint64>::max()))
            return tr("%1 bytes").arg(QLocale().toString(entry.info.size));
        return QLocale().formattedDataSize(qint64(entry.info.size));
    }
    if (id == "modified") {
        if (!entry.info.modified.isValid()) return {};
        return location_ == Remote ? QLocale().toString(entry.info.modified.date(), QLocale::ShortFormat)
                                   : QLocale().toString(entry.info.modified.toLocalTime(), QLocale::ShortFormat);
    }
    if (id == "status") {
        if (state_ == OfflineCached) return tr("Cached");
        if (state_ == Unavailable) return tr("Unavailable");
        switch (entry.info.status) {
        case dsfm::EntryStatus::Available: return tr("Listed");
        case dsfm::EntryStatus::Missing: return tr("Missing");
        case dsfm::EntryStatus::Inconsistent: return tr("Inconsistent");
        case dsfm::EntryStatus::Updating: return tr("Updating");
        }
    }
    return {}; // Never expose arbitrary native metadata fields.
}
FileBrowserModel::~FileBrowserModel() { if (provider_) provider_->cancel(); }
void FileBrowserModel::clearRows() {
    ++watch_generation_; prefetch_.stop(); live_refresh_.stop();
    if (provider_) provider_->unwatch();
    beginResetModel(); entries_.clear(); entry_rows_.clear(); cursors_.clear(); endResetModel();
    metadata_bytes_ = 0; pages_ = 0; empty_prefetch_ = 0;
    viewport_first_ = viewport_last_ = focused_ = -1; scroll_position_ = 0;
    selected_.clear(); selection_anchor_.clear(); context_.reset(); download_results_.clear();
    selection_revision_ = 0; emit selectionChanged();
    cursor_.clear(); directory_ = {}; seen_entries_ = false; last_refreshed_ = {};
}
void FileBrowserModel::invalidate() {
    ++generation_; pending_.reset(); cancel_.cancel();
    if (provider_) provider_->cancel();
    provider_.reset(); clearRows(); cache_.clear();
    route_.clear(); history_.clear(); history_position_ = -1;
    busy_ = false; state_ = Unavailable; error_.clear();
}
void FileBrowserModel::stop() { stopped_ = true; invalidate(); emit changed(); }
void FileBrowserModel::setProviderFactory(Factory factory) {
    invalidate(); factory_ = std::move(factory); emit changed();
}
bool FileBrowserModel::makeProvider() {
    if (stopped_ || !factory_ || scope_.identity.isNull() || (location_ == Remote && scope_.contact.isNull())) return false;
    auto provider = factory_(location_, scope_);
    if (!provider || !provider.value() || provider.value()->scope() != scope_ || !provider.value()->current()) return false;
    if (provider_) provider_->cancel();
    provider_ = std::move(provider.value()); cache_.clear();
    return true;
}
void FileBrowserModel::openLocal(QUuid identity) { bind(Local, {identity, {}}); }
void FileBrowserModel::openRemote(QUuid identity, QUuid contact) { bind(Remote, {identity, contact}); }
void FileBrowserModel::bind(Location location, BrowserScope scope) {
    invalidate(); location_ = location; scope_ = scope;
    if (!makeProvider()) {
        error_ = tr("FileShare is unavailable. Check the identity, contact connection and file preferences, then Refresh.");
        emit changed(); return;
    }
    LFLOG_DEBUG << "FileShare browser bound location=" << (location == Local ? "local" : "remote");
    navigate({}, true);
}
void FileBrowserModel::navigate(QStringList route, bool push_history) {
    if (!provider_) return;
    if (!validRoute(route)) { error_ = tr("This folder exceeds the browser's path or depth limit."); emit changed(); return; }
    if (push_history && (history_position_ < 0 || route != history_[history_position_])) {
        history_.resize(history_position_ + 1);
        history_.append(route);
        if (history_.size() > max_history) history_.removeFirst();
        history_position_ = int(history_.size()) - 1;
    }
    route_ = std::move(route);
    requestPage(false);
}
void FileBrowserModel::openEntry(QString value) {
    if (busy_ || !ready() || !provider_) return;
    for (const auto &entry : entries_) {
        if (key(entry) != value.toLatin1() || entry.info.type != dsfm::EntryType::Directory) continue;
        auto route = route_; route.append(entry.info.name); navigate(std::move(route), true); return;
    }
}
void FileBrowserModel::openBreadcrumb(int index) {
    if (index < 0 || index > route_.size() || !provider_) return;
    navigate(route_.mid(0, index), true);
}
void FileBrowserModel::back() {
    if (provider_ && canBack()) navigate(history_[--history_position_], false);
}
void FileBrowserModel::forward() {
    if (provider_ && canForward()) navigate(history_[++history_position_], false);
}
void FileBrowserModel::up() { if (canUp()) openBreadcrumb(int(route_.size()) - 1); }
void FileBrowserModel::refresh() {
    if (stopped_) return;
    if (!provider_ || !provider_->current()) {
        if (!makeProvider()) {
            disconnected();
            if (state_ != OfflineCached) error_ = tr("FileShare is unavailable. Reconnect the contact and check file preferences, then Refresh.");
            emit changed(); return;
        }
        if (history_.isEmpty()) { history_.append(route_); history_position_ = 0; }
    }
    requestPage(false, true);
}
void FileBrowserModel::publishSelection(QSet<QByteArray> selection) {
    const auto previous = selected_; selected_ = std::move(selection); ++selection_revision_;
    context_.reset();
    // Offscreen role changes are coalesced until that window approaches. At most
    // 256 changed IDs are examined per selection operation, never the full index.
    auto affected = previous; affected.unite(selected_);
    int first = rowCount(), last = -1;
    for (const auto &id : affected) {
        const int row = entry_rows_.value(id, -1);
        if (row >= std::max(0, viewport_first_ - overscan_) && row <= viewport_last_ + overscan_) {
            first = std::min(first, row); last = std::max(last, row);
        }
    }
    if (first <= last) emit dataChanged(index(first), index(last), {Selected});
    emit selectionChanged();
}
void FileBrowserModel::selectRow(int row, SelectionMode mode) {
    if (!ready() || busy_ || mode < Replace || mode > Range) return;
    if (row < 0 || row >= rowCount()) {
        if (mode == Range) { error_ = tr("Load the requested rows before selecting a range."); emit changed(); }
        return;
    }
    const auto id = entries_[row].info.id;
    auto selection = selected_;
    if (mode == Range && !selection_anchor_.isEmpty() && entry_rows_.contains(selection_anchor_)) {
        const int anchor = entry_rows_.value(selection_anchor_);
        const int begin = std::min(row, anchor), end = std::max(row, anchor);
        if (end - begin + 1 > max_selection) { error_ = tr("Select at most %1 loaded items.").arg(max_selection); emit changed(); return; }
        selection.clear(); for (int i = begin; i <= end; ++i) selection.insert(entries_[i].info.id);
    } else if (mode == Toggle) {
        if (!selection.remove(id)) {
            if (selection.size() >= max_selection) { error_ = tr("Select at most %1 loaded items.").arg(max_selection); emit changed(); return; }
            selection.insert(id);
        }
        selection_anchor_ = id;
    } else { selection = {id}; selection_anchor_ = id; }
    focused_ = row; error_.clear(); publishSelection(std::move(selection)); emit changed();
}
void FileBrowserModel::moveFocus(int delta, bool extend) {
    if (!rowCount()) return;
    const int row = int(std::clamp(qint64(focused_ < 0 ? 0 : focused_) + delta, qint64(0), qint64(rowCount() - 1)));
    selectRow(row, extend ? Range : Replace); emit revealRow(row);
}
void FileBrowserModel::selectAllLoaded() {
    if (!ready() || busy_) return;
    if (rowCount() > max_selection) { error_ = tr("Select All is limited to %1 loaded items; narrow your selection.").arg(max_selection); emit changed(); return; }
    QSet<QByteArray> selection;
    for (const auto &entry : entries_) selection.insert(entry.info.id);
    if (!entries_.isEmpty()) selection_anchor_ = entries_.first().info.id;
    publishSelection(std::move(selection));
}
void FileBrowserModel::clearSelection() { selection_anchor_.clear(); publishSelection({}); }
FileBrowserModel::SelectionSnapshot FileBrowserModel::selectionSnapshot() const {
    SelectionSnapshot result{provider_, scope_, directory_.info.id, watch_generation_, {}};
    QVector<int> rows;
    for (const auto &id : selected_) if (entry_rows_.contains(id)) rows.append(entry_rows_.value(id));
    std::sort(rows.begin(), rows.end());
    for (int row : rows) result.entries.append(entries_[row]);
    return result;
}
bool FileBrowserModel::currentSnapshot(const SelectionSnapshot &snapshot) const {
    return snapshot.provider && snapshot.provider == provider_ && snapshot.scope == scope_ &&
        snapshot.directory == directory_.info.id && snapshot.generation == watch_generation_ && ready() && !busy_;
}
QString FileBrowserModel::downloadReason(const SelectionSnapshot &snapshot) const {
    if (location_ != Remote) return tr("Downloads are available from a remote contact.");
    if (downloads_running_) return tr("A download request batch is in progress.");
    if (!currentSnapshot(snapshot) || !provider_->current() || state_ == OfflineCached) return tr("Reconnect and Refresh before requesting downloads.");
    if (snapshot.entries.isEmpty()) return tr("Select regular files to request a download.");
    if (snapshot.entries.size() > max_download_batch) return tr("Request at most %1 files per batch.").arg(max_download_batch);
    for (const auto &entry : snapshot.entries)
        if (!provider_->canDownload(entry)) return tr("Select available regular files; folders and unavailable files cannot be downloaded.");
    return {};
}
QString FileBrowserModel::downloadReason() const { return downloadReason(selectionSnapshot()); }
bool FileBrowserModel::canDownloadSelection() const { return downloadReason().isEmpty(); }
bool FileBrowserModel::contextCanDownload() const { return context_ && downloadReason(*context_).isEmpty(); }
QString FileBrowserModel::contextDownloadReason() const { return context_ ? downloadReason(*context_) : tr("Reopen the context menu."); }
bool FileBrowserModel::contextCanOpen() const {
    return context_ && currentSnapshot(*context_) && context_->entries.size() == 1 &&
        context_->entries.first().info.type == dsfm::EntryType::Directory;
}
void FileBrowserModel::prepareContext(int row) {
    if (row >= 0 && row < rowCount()) {
        if (!selected_.contains(entries_[row].info.id)) selectRow(row);
    } else clearSelection();
    context_ = selectionSnapshot(); emit selectionChanged();
}
void FileBrowserModel::openContext() {
    if (!contextCanOpen()) return;
    const auto value = key(context_->entries.first()); context_.reset(); openEntry(QString::fromLatin1(value));
}
void FileBrowserModel::activateFocused() {
    if (focused_ >= 0 && focused_ < rowCount()) openEntry(QString::fromLatin1(key(entries_[focused_])));
}
void FileBrowserModel::requestDownloads() {
    auto snapshot = selectionSnapshot();
    const auto reason = downloadReason(snapshot);
    if (!reason.isEmpty()) { error_ = reason; emit changed(); return; }
    downloadBatch(std::move(snapshot));
}
void FileBrowserModel::requestContextDownloads() {
    if (!contextCanDownload()) { error_ = contextDownloadReason(); emit changed(); return; }
    auto snapshot = std::move(*context_); context_.reset(); downloadBatch(std::move(snapshot));
}
QCoro::Task<void> FileBrowserModel::downloadBatch(SelectionSnapshot snapshot) {
    QPointer<FileBrowserModel> self(this);
    downloads_running_ = true; download_results_.clear(); prefetch_.stop(); emit selectionChanged();
    for (const auto &entry : snapshot.entries) {
        if (!self || !self->currentSnapshot(snapshot) || !snapshot.provider->current()) break;
        auto result = co_await snapshot.provider->download(entry);
        if (!self) co_return;
        if (!self->currentSnapshot(snapshot)) break;
        const auto message = result ? tr("Requested — waiting for the generic file transfer") :
            result.error().code == dsfm::ErrorCode::PermissionDenied ? tr("Access denied or file unavailable") :
            result.error().code == dsfm::ErrorCode::IntegrityError ? tr("File changed; Refresh before retrying") : tr("Request failed; Refresh or retry later");
        self->download_results_.append(QVariantMap{{"name", entry.info.name}, {"success", bool(result)}, {"message", message}});
        emit self->selectionChanged();
    }
    if (self) {
        self->downloads_running_ = false; emit self->selectionChanged();
        if (self->pending_ && !self->running_) self->run(); else self->schedulePrefetch();
    }
}
void FileBrowserModel::setViewport(int first, int last, int focused, int overscan, double scroll_position) {
    if (first < 0 || first > rowCount() || last < first) return;
    const int previous_first = viewport_first_, previous_last = viewport_last_;
    viewport_first_ = first;
    viewport_last_ = std::min({last, first + 511, rowCount()});
    focused_ = focused >= 0 && focused < rowCount() ? focused : -1;
    overscan_ = std::clamp(overscan, 0, 100);
    scroll_position_ = std::isfinite(scroll_position) ? std::clamp(scroll_position, 0.0, 1.0) : 0;
    if (selection_revision_ && rowCount() && (previous_first != viewport_first_ || previous_last != viewport_last_)) {
        const int begin = std::max(0, first - overscan_);
        const int end = std::min(rowCount() - 1, viewport_last_ + overscan_);
        if (begin <= end) emit dataChanged(index(begin), index(end), {Selected});
    }
    schedulePrefetch();
}
void FileBrowserModel::schedulePrefetch() {
    if (!downloads_running_ && !busy_ && more() && viewport_first_ >= 0 && viewport_last_ + overscan_ >= rowCount() &&
        empty_prefetch_ < 4 && !prefetch_.isActive()) prefetch_.start();
}
void FileBrowserModel::folderChanged(quint64 generation, bool invalidated) {
    LFLOG_TRACE << "FileShare browser folder notification current=" << (generation == watch_generation_)
                << " invalidated=" << invalidated;
    if (generation != watch_generation_ || !provider_) return;
    // VFS supplies invalidations, not item deltas. Fence the old cursor immediately;
    // coalesce bursts into one refresh rather than inventing partial model changes.
    ++generation_; pending_.reset(); cancel_.cancel(); provider_->cancelPending();
    cursor_.clear(); prefetch_.stop(); cache_.clear(); busy_ = false;
    if (invalidated) clearRows();
    context_.reset();
    state_ = Updating; emit changed(); emit selectionChanged();
    if (!live_refresh_.isActive()) live_refresh_.start();
}
void FileBrowserModel::nextPage() { if (!downloads_running_ && !busy_ && more() && provider_) requestPage(true); }
void FileBrowserModel::requestPage(bool next, bool refresh) {
    LFLOG_DEBUG << "FileShare browser request page next=" << next << " refresh=" << refresh;
    if (!provider_) return;
    ++generation_; pending_.reset(); cancel_.cancel(); cancel_ = dsfm::CancellationToken{}; provider_->cancelPending();
    auto directory = directory_; auto cursor = cursor_;
    if (!next) clearRows();
    if (next && pages_ >= max_pages) {
        cursor_.clear(); error_ = tr("Browser page limit reached. Open a smaller folder or Refresh."); emit changed(); return;
    }
    error_.clear();
    state_ = refresh ? Updating : Known;
    if (!provider_->current()) { disconnected(); return; }
    busy_ = true;
    const auto watch_generation = watch_generation_;
    pending_ = Load{generation_, provider_, route_, std::move(directory), next ? cursor : QByteArray{}, next, refresh, cancel_,
        {this, [this, watch_generation](dsvfs::Change change) { folderChanged(watch_generation, change.invalidated); }}};
    emit changed(); emit selectionChanged();
    if (!running_ && !downloads_running_ && pending_) run();
}
bool FileBrowserModel::showCached() {
    if (!provider_ || location_ != Remote || !provider_->cachedMetadataAllowed()) return false;
    for (const auto &page : cache_) if (page.route == route_) {
        beginResetModel(); entries_ = page.entries; entry_rows_.clear(); selected_.clear(); context_.reset(); focused_ = -1;
        for (int row = 0; row < entries_.size(); ++row) entry_rows_.insert(entries_[row].info.id, row);
        endResetModel(); emit selectionChanged();
        cursor_.clear(); seen_entries_ = page.seen_entries; last_refreshed_ = page.observed;
        busy_ = false; state_ = OfflineCached; error_.clear(); return true;
    }
    return false;
}
void FileBrowserModel::disconnected() {
    if (state_ == OfflineCached && provider_ && provider_->cachedMetadataAllowed()) return;
    ++generation_; pending_.reset(); cancel_.cancel(); busy_ = false;
    prefetch_.stop(); live_refresh_.stop();
    if (provider_) { provider_->cancelPending(); provider_->unwatch(); }
    if (showCached()) { emit changed(); return; }
    clearRows(); state_ = Unavailable;
    error_ = tr("This folder is unavailable. Refresh after reconnecting; cached metadata grants no access.");
    if (!provider_ || !provider_->cachedMetadataAllowed()) {
        cache_.clear(); history_.clear(); history_position_ = -1; route_.clear();
        if (provider_) provider_->cancel();
        provider_.reset();
    }
    emit changed(); emit selectionChanged();
}
void FileBrowserModel::remember() {
    for (int i = 0; i < cache_.size(); ++i) if (cache_[i].route == route_) { cache_.removeAt(i); break; }
    if (location_ != Remote) return;
    // Historical previews retain at most eight first pages, never eight giant indexes.
    cache_.append({route_, entries_.mid(0, 100), last_refreshed_, seen_entries_});
    if (cache_.size() > max_cache) cache_.removeFirst();
}
QCoro::Task<dsfm::Result<FileBrowserModel::Loaded>> FileBrowserModel::fetch(Load load) {
    auto provider = load.provider;
    if (!provider->current() || load.cancel.cancelled()) co_return dsfm::Error{dsfm::ErrorCode::Cancelled};
    BrowserNode directory = load.directory;
    if (!load.next) {
        auto resolved = co_await provider->resolveDirectory(load.route, load.cancel); if (!resolved) co_return resolved.error();
        directory = std::move(resolved.value());
    }
    if (load.cancel.cancelled()) co_return dsfm::Error{dsfm::ErrorCode::Cancelled};
    if (!load.next) {
        auto watched = co_await provider->watch(directory, load.on_change);
        if (!watched) co_return watched.error();
        if (load.cancel.cancelled()) { provider->unwatch(); co_return dsfm::Error{dsfm::ErrorCode::Cancelled}; }
    }
    auto page = co_await provider->listPage(directory, load.cursor);
    if (!page) co_return page.error();
    if (directory.provider != provider->id() || directory.path != routePath(load.route) || directory.info.id.isEmpty() ||
        directory.info.type != dsfm::EntryType::Directory || page.value().entries.size() > 100 ||
        page.value().continuation.size() > 2048 || (!load.cursor.isEmpty() && load.cursor == page.value().continuation))
        co_return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    QSet<QByteArray> ids;
    for (const auto &entry : page.value().entries) {
        if (entry.provider != provider->id() || entry.info.id.isEmpty() || entry.info.id.size() > 4096 ||
            entry.info.parent.size() > 4096 || entry.info.sha256.size() > 32 || entry.path.size() > 4096 ||
            entry.info.name.size() > 3 * prot::fileshare::max_component_bytes || ids.contains(entry.info.id))
            co_return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
        ids.insert(entry.info.id);
    }
    co_return Loaded{std::move(directory), std::move(page.value())};
}
bool FileBrowserModel::appendPage(QVector<BrowserNode> page) {
    quint64 bytes = 0;
    for (const auto &entry : page) {
        if (entry_rows_.contains(entry.info.id)) return false;
        bytes += metadataBytes(entry);
    }
    if (entries_.size() + page.size() > max_entries || bytes > max_metadata_bytes - metadata_bytes_) return false;
    if (page.isEmpty()) { ++empty_prefetch_; return true; }
    empty_prefetch_ = 0;
    const int first = rowCount();
    beginInsertRows({}, first, first + int(page.size()) - 1);
    for (auto &entry : page) { entry_rows_.insert(entry.info.id, int(entries_.size())); entries_.append(std::move(entry)); }
    metadata_bytes_ += bytes;
    endInsertRows();
    return true;
}
QCoro::Task<void> FileBrowserModel::run() {
    QPointer<FileBrowserModel> self(this);
    running_ = true;
    while (self && self->pending_) {
        auto load = std::move(*self->pending_); self->pending_.reset();
        self->state_ = load.refresh ? Updating : Loading; emit self->changed();
        if (!self) co_return;
        auto result = co_await fetch(load);
        LFLOG_TRACE << "FileShare browser page completed current=" << (self && self->generation_ == load.generation)
                    << " success=" << bool(result);
        if (!self) co_return;
        if (self->generation_ != load.generation) continue;
        if (!load.provider->current()) { self->disconnected(); continue; }
        self->busy_ = false;
        if (!result) {
            LFLOG_TRACE << "FileShare browser load failed stage=" << (load.next ? "list-page" : "resolve/list")
                        << " code=" << int(result.error().code) << " native=" << result.error().native_code;
            self->clearRows(); self->cache_.clear();
            self->state_ = result.error().code == dsfm::ErrorCode::PermissionDenied || result.error().code == dsfm::ErrorCode::MissingEntry ? Denied : Unavailable;
            self->error_ = self->state_ == Denied ? tr("This folder is unavailable or access was denied.") : tr("Could not list this folder. Use Refresh to retry.");
        } else {
            self->directory_ = std::move(result.value().directory);
            auto &page = result.value().page;
            const bool repeated_cursor = !page.continuation.isEmpty() && self->cursors_.contains(page.continuation);
            if (repeated_cursor || !self->appendPage(std::move(page.entries))) {
                LFLOG_TRACE << "FileShare browser rejected page stage=index-admission duplicate/cursor/resource-bound";
                self->clearRows(); self->cache_.clear(); self->state_ = Unavailable;
                self->error_ = tr("The listing changed or exceeded the browser's safety limits. Use Refresh.");
                emit self->changed(); continue;
            }
            ++self->pages_;
            self->cursor_ = std::move(page.continuation);
            if (!self->cursor_.isEmpty()) self->cursors_.insert(self->cursor_);
            self->seen_entries_ = (load.next && self->seen_entries_) || !self->entries_.isEmpty();
            self->state_ = !self->cursor_.isEmpty() ? PartiallyListed : (self->seen_entries_ ? Complete : Empty);
            self->last_refreshed_ = QDateTime::currentDateTimeUtc(); self->error_.clear(); self->remember();
        }
        emit self->changed();
        if (self) { emit self->selectionChanged(); self->schedulePrefetch(); }
    }
    if (self) self->running_ = false;
}
QString FileBrowserModel::currentPath() const { return routePath(route_); }
QVariantList FileBrowserModel::breadcrumbs() const {
    QVariantList result;
    result.append(QVariantMap{{"label", "/"}, {"index", 0}});
    for (int i = 0; i < route_.size(); ++i) result.append(QVariantMap{{"label", route_[i]}, {"index", i + 1}});
    return result;
}
QString FileBrowserModel::stateText() const {
    switch (state_) {
    case Known: return tr("Known folder — waiting to load");
    case Loading: return tr("Loading folder…");
    case PartiallyListed: return cursor_.isEmpty() ? tr("Partially listed — browser limit reached") : tr("Partially listed — more pages available");
    case Complete: return tr("Listing complete");
    case Updating: return tr("Refreshing folder…");
    case OfflineCached: return tr("Offline — cached first page, last refreshed %1; current access is unconfirmed").arg(QLocale().toString(last_refreshed_.toLocalTime(), QLocale::ShortFormat));
    case Unavailable: return tr("Folder unavailable");
    case Denied: return tr("Access denied or folder unavailable");
    case Empty: return tr("Empty folder");
    }
    return {};
}
QByteArray FileBrowserModel::key(const BrowserNode &entry) const {
    return QCryptographicHash::hash(entry.provider.toRfc4122() + scope_.identity.toRfc4122() +
        scope_.contact.toRfc4122() + entry.info.id, QCryptographicHash::Sha256).toHex();
}
int FileBrowserModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(entries_.size()); }
QVariant FileBrowserModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.model() != this || index.column() != 0 || index.row() < 0 || index.row() >= entries_.size()) return {};
    const auto &entry = entries_[index.row()];
    switch (role) {
    case Key: return key(entry);
    case Qt::DisplayRole:
    case Name: return entry.info.name;
    case Directory: return entry.info.type == dsfm::EntryType::Directory;
    case Size: return QVariant::fromValue(entry.info.size);
    case SizeKnown: return entry.size_known;
    case Selected: return selected_.contains(entry.info.id);
    default: return {};
    }
}
QHash<int, QByteArray> FileBrowserModel::roleNames() const {
    return {{Key, "entryKey"}, {Name, "entryName"}, {Directory, "directory"}, {Size, "fileSize"}, {SizeKnown, "sizeKnown"}, {Selected, "selected"}};
}
}
