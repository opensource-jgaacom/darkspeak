#pragma once
#include <ds/browseprovider.h>
#include <QAbstractListModel>
#include <QVariantList>
#include <QSet>

namespace ds::core { class ManagedStorage; }
namespace ds::models {
class FileBrowserModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(int focusedRow READ focusedRow NOTIFY selectionChanged)
    Q_PROPERTY(bool downloading READ downloading NOTIFY selectionChanged)
    Q_PROPERTY(bool canDownloadSelection READ canDownloadSelection NOTIFY selectionChanged)
    Q_PROPERTY(QString downloadReason READ downloadReason NOTIFY selectionChanged)
    Q_PROPERTY(bool contextCanDownload READ contextCanDownload NOTIFY selectionChanged)
    Q_PROPERTY(bool contextCanOpen READ contextCanOpen NOTIFY selectionChanged)
    Q_PROPERTY(QString contextDownloadReason READ contextDownloadReason NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList downloadResults READ downloadResults NOTIFY selectionChanged)
    Q_PROPERTY(ViewMode viewMode READ viewMode WRITE setViewMode NOTIFY viewSettingsChanged)
    Q_PROPERTY(QVariantList detailColumns READ detailColumns NOTIFY viewSettingsChanged)
    Q_PROPERTY(Location location READ location NOTIFY changed)
    Q_PROPERTY(QUuid identityId READ identityId NOTIFY changed)
    Q_PROPERTY(QUuid contactId READ contactId NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool more READ more NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(State state READ state NOTIFY changed)
    Q_PROPERTY(QString stateText READ stateText NOTIFY changed)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY changed)
    Q_PROPERTY(QVariantList breadcrumbs READ breadcrumbs NOTIFY changed)
    Q_PROPERTY(bool canBack READ canBack NOTIFY changed)
    Q_PROPERTY(bool canForward READ canForward NOTIFY changed)
    Q_PROPERTY(bool canUp READ canUp NOTIFY changed)
    Q_PROPERTY(QDateTime lastRefreshed READ lastRefreshed NOTIFY changed)
public:
    enum ViewMode { Thumbnails, SmallIcons, Details };
    Q_ENUM(ViewMode)
    ViewMode viewMode() const { return view_mode_; }
    void setViewMode(ViewMode mode);
    QVariantList detailColumns() const;
    Q_INVOKABLE void setColumnVisible(QString id, bool visible);
    Q_INVOKABLE void moveColumn(QString id, int position);
    Q_INVOKABLE void setColumnWidth(QString id, int width);
    Q_INVOKABLE QString columnText(int row, QString id) const;
    enum Location { Local, Remote };
    Q_ENUM(Location)
    enum State { Known, Loading, PartiallyListed, Complete, Updating, OfflineCached, Unavailable, Denied, Empty };
    Q_ENUM(State)
    enum Role { Key = Qt::UserRole + 1, Name, Directory, Size, SizeKnown, Selected };
    enum SelectionMode { Replace, Toggle, Range };
    Q_ENUM(SelectionMode)
    int selectedCount() const { return int(selected_.size()); }
    int focusedRow() const { return focused_; }
    bool downloading() const { return downloads_running_; }
    bool canDownloadSelection() const;
    QString downloadReason() const;
    bool contextCanDownload() const;
    bool contextCanOpen() const;
    QString contextDownloadReason() const;
    QVariantList downloadResults() const { return download_results_; }
    Q_INVOKABLE void selectRow(int row, SelectionMode mode = Replace);
    Q_INVOKABLE void moveFocus(int delta, bool extend = false);
    Q_INVOKABLE void selectAllLoaded();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void prepareContext(int row = -1);
    Q_INVOKABLE void openContext();
    Q_INVOKABLE void activateFocused();
    Q_INVOKABLE void requestDownloads();
    Q_INVOKABLE void requestContextDownloads();
    using Factory = std::function<dsfm::Result<std::shared_ptr<BrowseProvider>>(Location, BrowserScope)>;
    explicit FileBrowserModel(QObject *parent = nullptr);
    ~FileBrowserModel() override;
    void setProviderFactory(Factory factory);
    Q_INVOKABLE void openLocal(QUuid identity);
    Q_INVOKABLE void openRemote(QUuid identity, QUuid contact);
    Q_INVOKABLE void nextPage();
    // Geometry only; C++ owns bounds, paging and scheduling.
    Q_INVOKABLE void setViewport(int first, int last, int focused, int overscan, double scroll_position);
    Q_INVOKABLE void openEntry(QString key);
    Q_INVOKABLE void openBreadcrumb(int index);
    Q_INVOKABLE void back();
    Q_INVOKABLE void forward();
    Q_INVOKABLE void up();
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void stop();
    Location location() const { return location_; }
    QUuid identityId() const { return scope_.identity; }
    QUuid contactId() const { return scope_.contact; }
    bool busy() const { return busy_; }
    bool more() const { return state_ == PartiallyListed && !cursor_.isEmpty(); }
    bool ready() const { return state_ == PartiallyListed || state_ == Complete || state_ == Empty || state_ == OfflineCached; }
    QString error() const { return error_; }
    State state() const { return state_; }
    QString stateText() const;
    QString currentPath() const;
    QVariantList breadcrumbs() const;
    bool canBack() const { return history_position_ > 0; }
    bool canForward() const { return history_position_ + 1 < history_.size(); }
    bool canUp() const { return !route_.isEmpty(); }
    QDateTime lastRefreshed() const { return last_refreshed_; }
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
signals:
    void changed();
    void viewSettingsChanged();
    void selectionChanged();
    void revealRow(int row);
private:
    void bind(Location location, BrowserScope scope);
    void clearRows();
    void invalidate();
    bool makeProvider();
    void navigate(QStringList route, bool push_history);
    void requestPage(bool next, bool refresh = false);
    void disconnected();
    bool showCached();
    void remember();
    QByteArray key(const BrowserNode &node) const;
    void schedulePrefetch();
    void folderChanged(quint64 watch_generation, bool invalidated);
    bool appendPage(QVector<BrowserNode> page);
    struct SelectionSnapshot {
        std::shared_ptr<BrowseProvider> provider;
        BrowserScope scope;
        QByteArray directory;
        quint64 generation = 0;
        QVector<BrowserNode> entries;
    };
    SelectionSnapshot selectionSnapshot() const;
    bool currentSnapshot(const SelectionSnapshot &snapshot) const;
    QString downloadReason(const SelectionSnapshot &snapshot) const;
    void publishSelection(QSet<QByteArray> selection);
    QCoro::Task<void> downloadBatch(SelectionSnapshot snapshot);
    QSet<QByteArray> selected_;
    QByteArray selection_anchor_;
    quint64 selection_revision_ = 0;
    std::optional<SelectionSnapshot> context_;
    bool downloads_running_ = false;
    bool last_download_allowed_ = false, last_context_allowed_ = false;
    QVariantList download_results_;
    struct Load {
        quint64 generation = 0;
        std::shared_ptr<BrowseProvider> provider;
        QStringList route;
        BrowserNode directory;
        QByteArray cursor;
        bool next = false, refresh = false;
        dsfm::CancellationToken cancel;
        dsvfs::ChangeCallback on_change;
    };
    struct Loaded { BrowserNode directory; BrowserPage page; };
    static QCoro::Task<dsfm::Result<Loaded>> fetch(Load load);
    QCoro::Task<void> run();
    struct CachedPage {
        QStringList route;
        QVector<BrowserNode> entries;
        QDateTime observed;
        bool seen_entries = false;
    };
    void saveViewSettings();
    struct Column { QString id; bool visible; int width; };
    QVector<Column> columns_{{"name", true, 280}, {"type", true, 100}, {"size", true, 100},
                             {"modified", false, 180}, {"status", true, 130}};
    ViewMode view_mode_ = Details;
    Factory factory_;
    std::shared_ptr<BrowseProvider> provider_;
    BrowserScope scope_;
    Location location_ = Local;
    BrowserNode directory_;
    QStringList route_;
    QVector<QStringList> history_;
    int history_position_ = -1;
    QVector<CachedPage> cache_; // At most eight historical first pages in this binding.
    // Value records only: capped by entry count and conservative metadata bytes.
    // Full row index uses grouped inserts, never per-row offscreen repaint signals.
    QVector<BrowserNode> entries_;
    QHash<QByteArray, int> entry_rows_;
    QSet<QByteArray> cursors_;
    quint64 metadata_bytes_ = 0, watch_generation_ = 0;
    int pages_ = 0, empty_prefetch_ = 0;
    int viewport_first_ = -1, viewport_last_ = -1, focused_ = -1, overscan_ = 0;
    double scroll_position_ = 0;
    QTimer prefetch_, live_refresh_;
    QByteArray cursor_;
    quint64 generation_ = 0;
    State state_ = Unavailable;
    QDateTime last_refreshed_;
    std::optional<Load> pending_;
    dsfm::CancellationToken cancel_;
    bool running_ = false, busy_ = false, stopped_ = false, seen_entries_ = false;
    QString error_;
};
// QML owns each model, so separate dialogs never share current-folder state.
class FileBrowserSources final : public QObject {
    Q_OBJECT
public:
    FileBrowserSources(FileBrowserModel::Factory factory, QObject *parent = nullptr)
        : QObject(parent), factory_(std::move(factory)) {}
    Q_INVOKABLE void configure(FileBrowserModel *model) {
        if (!model) return;
        model->setProviderFactory(factory_);
        connect(this, &FileBrowserSources::stopping, model, &FileBrowserModel::stop, Qt::UniqueConnection);
        if (stopped_) model->stop();
    }
    void stop() { stopped_ = true; emit stopping(); }
signals:
    void stopping();
private:
    FileBrowserModel::Factory factory_;
    bool stopped_ = false;
};
FileBrowserModel::Factory applicationBrowserFactory(core::ManagedStorage &storage, prot::FileShareService &responses);
}
