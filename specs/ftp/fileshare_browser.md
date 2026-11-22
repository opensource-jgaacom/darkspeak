# DarkSpeak FileShare Browser — shared local and remote VFS UI

**Status:** Implementation specification. **First milestone:** interactive local and remote *read-only* navigation and download between two DarkSpeak instances. **Dependencies:** `../vfs.md`, `fileshare_protocol.md`, `fileshare_server.md`, `fileshare_authorization.md`, `../file_manager.md`, `../transfer_queue.md`, and `../transfer_queue_ui.md`. Upload and writable remote mutations are separate later milestones (`fileshare_uploads.md`); credits are optional per identity and off by default (`fileshare_credits.md`).

## 1. One browser, one C++ model, two sources

Use **one C++ browser model/controller** and the same QML view components for browsing local and remote virtual filesystems. The model exposes an explicit enum property, for example:
```cpp
enum class BrowserLocation { Local, Remote };
Q_PROPERTY(BrowserLocation location READ location NOTIFY locationChanged)
```
This property is an explicit, stable statement of the current source, **not** an inference from filenames, URLs, storage paths, icon or capabilities. Scope also includes local identity and, for remote, authenticated contact under that identity; remote objects from different identity/contact pairs must never be mixed.

The browser is VFS-oriented. Define a narrow **async browse-provider interface** (or equivalent adapter) with `root`, `stat`, `listPage`, `resolveChild`, optional `subscribe`, metadata/thumbnail request and capability-dependent actions. A **local provider** adapts the existing `dsvfs::Vfs` and trusted local-admin context; a **remote provider** adapts FileShare v1 LIST/STAT/GET over the encrypted DarkSpeak connection plus a local, validated metadata cache. The remote provider must **not** instantiate a second full `dsvfs::Vfs` tied to foreign physical resources or treat cached remote catalog IDs as local backend IDs. Reuse `dsvfs::NodeInfo`-compatible display data and stable opaque node identities behind a provider-scoped browser key. No per-location duplicated QML, sorting engine, selection model, or transfer scheduler.

C++ owns navigation state, current directory, provider binding, paging, cache, visible-window tracking, sorting/filtering, selection, operation availability, thumbnails, updates, async cancellation and error statuses. QML renders data, focus, hover, shortcuts and context menus, and forwards user commands. Follow existing UI architecture, themes, text scaling (20–200%) and 1366×768 baseline. Keep UI responsive and disk/network work off the UI thread.

## 2. Navigation: exactly one open folder

The browser displays the **immediate children of one current directory** at a time, never a recursive tree embedded in the main file pane. A location header contains a clickable breadcrumb path: `/` → each parent → current folder. **Every parent component is individually selectable** to navigate directly upward without repeatedly pressing Back. Include Back, Forward, Up and Refresh; root remains selectable. Breadcrumb labels are provider-presented names; do not construct native paths or URL-decode escaped labels.

Opening a directory replaces the visible folder contents and updates the breadcrumb/history. Preserve selection/scroll/filter per folder in a **bounded** navigation cache where useful; restoring a folder must not display stale permissions as authority. Navigation does not implicitly recursively list descendants. The browser must never automatically expand an alias loop: follow a user-selected directory only once per action, retain route-aware breadcrumb identity and bound navigation history. Recursive operations are performed by the VFS/server's cycle-safe traversal (`../vfs.md` §5.1), not by recursively calling the browser model. An explicitly requested recursive catalog is a separate optional feature; rejection returns no listing data and the user can continue one directory at a time.

Location selection: local mode selects a local identity's effective authorized VFS or a locally administered selected storage view; remote mode selects a contact under one local identity and displays only that remote contact's authorized exported VFS. Entry points from identity, contact and FileShare pages open the **same** browser with appropriate scope. Switching identity/contact/location invalidates old requests and navigation IDs before applying any callbacks.

A folder may be `Known / Loading / PartiallyListed / Complete / Updating / OfflineCached / Unavailable / Denied`. A known directory with no received children is **not** automatically an empty directory. Show a distinct `Empty` state only after a successful complete listing with zero visible entries; display incremental status/progress when the count is not known. Show stale cached data clearly if current remote state cannot be confirmed.

## 3. Dolphin-inspired display modes

Provide three modes over the **same model**, with user-selectable controls and persisted preferences:

- **Thumbnail view:** grid of tiles with thumbnail when safely available, otherwise type icon, filename and limited metadata. Thumbnail work is lazy and only for visible/near-visible items; decoding/generation uses the existing bounded preview services and image policies. No automatic download or decoding of entire remote original files.
- **Small-icon view:** compact icon/name layout suitable for dense directories. Long names are elided with accessible full-name display on hover/focus; selection state is obvious.
- **Details view:** virtualized rows with selectable/reorderable **optional columns**: Name (always), Type, Size (files), Modified (privacy-reduced remote value), Status, optionally Hash/availability and permitted operation flags. Do not show native owner, device, inode, permissions, filesystem type, physical paths or other OS fingerprints in remote mode. Columns and order/width persist by mode, subject to actual metadata availability.

Offer ascending/descending sorting (at least name, size, modification time where available), directories-first preference and simple search/filter. **Avoid misleading global order for partially loaded giant folders:** VFS pages already have stable order; to sort by a field not supported for server-side paging, either present it explicitly as `loaded items only`, or require a complete bounded local/cached listing before enabling global sort. Never silently claim that 100 loaded items are the largest/smallest among 100,000 unknown items. For remote listings, do not invent server-side sort/filter fields absent from `fileshare_protocol.md`; add such protocol capability only through a separately approved bounded extension. Initial efficient sort uses the VFS's documented deterministic order and compatible in-memory ordering of received entries.

## 4. Incremental folders and viewport-aware updates

A directory can contain **100,000+ entries** and pages may arrive slowly. The browser model must not buffer every file as a separate QML object, issue a model-reset per page, emit row-changed signals for all received items, request all thumbnails, or keep 100,000 heavyweight `QObject` wrappers.

- Fetch remote pages asynchronously with **bounded prefetch**, favoring the current visible window and a small forward/backward overscan. Local browsing uses bounded VFS pages. Receiving entries incrementally updates a **compact C++ directory index/cache**, keyed by provider + identity/contact + stable node ID (and current directory/generation), with the selected stable ordering.
- Publish insert/remove/move notifications to the QAbstractItemModel **only for rows presently represented in the active viewport/window plus small overscan**, or publish **bounded grouped range notifications** when the table model retains the full row index. Maintain Qt model invariants: accurate `rowCount` for any published model range, valid persistent indexes, and no row omission/duplication during reorder. A bounded window/proxy model over a compact C++ index is a suitable implementation; the implementation may choose it rather than invent a Qt-unsupported partial signal contract.
- The UI reports viewport first/last visible indices, focused index, overscan and current scroll position back to C++. The model batches/throttles updates from out-of-window items and applies them on approach, refresh, sort or navigation. **Do not waste individual repaint/dataChanged notifications on far-off-screen rows**. If an offscreen update affects ordering or item count, keep the backing index logically correct and publish only the necessary structural/aggregate changes to satisfy Qt model contracts.
- Backpressure paging and parsing so a fast sender or catalog ingestion cannot overwhelm UI thread, caches, SQLite or signal queues. Batch on worker/coroutine boundaries; keep expensive sorting/search/indexing in C++. Do not fetch the entire directory eagerly just to populate the first screen. Support empty pages with continuation as allowed by VFS.
- Visible rows may transition `Unknown → Loading → Available/Updating/Unavailable`; publish status and metadata as their pages or refresh results arrive. Use stable IDs and request-generation fences to prevent stale slow responses from changing a newly opened directory. Distinguish transmitted/received listing bytes from verified/accepted **recursive catalog** transfers.
- Remote **direct LIST** streams pages, so incremental page results can appear as they arrive. **Recursive catalog export** is different: it is generated as a complete staged artifact by the server and accepted locally **only after full SHA-256 and catalog-format validation**. The browser may then index/display catalog records incrementally from the fully validated artifact; never publish unvalidated partial recursive exports. Cached catalogs are historical metadata, not authorization for subsequent GET.
- During live updates (when supported), update only affected visible rows plus compact offscreen index; invalidate and refresh the current folder if exact changes cannot be reconciled. FTP v1 currently has **optional** subscriptions: without negotiated notifications, provide manual Refresh, foreground refresh on re-entry/reconnect and bounded polling only if explicitly configured, not a mandatory high-rate polling loop. A remote user cannot force the client to retain unlimited directory subscriptions.

Bound cached directory indexes, view-window size, inflight LIST requests, prefetch pages, thumbnail work and cached metadata by memory/disk budgets. Allow paging/disk-backed catalog index for huge directories without loading all entry objects into RAM. Paging errors, invalid cursors, changing authorization/remote generations or interrupted connections must not leave the UI stuck in perpetual `Loading`.

## 5. Selection and batch operations

Dolphin-like selection over the current **one folder**:
- Click/tap selects one item; Ctrl-click (or platform multi-select gesture) toggles individual items; Shift-click selects a contiguous range **in current effective sort order**; keyboard navigation, Select All and Clear Selection work. Touch supports long-press to initiate selection/context menu. Dragging a selection is optional in the first milestone.
- Maintain selection by **stable provider-scoped item IDs**, not QML row numbers: newly inserted/moved/updated rows do not silently select different files. Selected items have a distinct visible state and a selected-count summary. Changing folder clears or restores only a bounded explicit folder selection; never carry selection to a different contact/identity.
- Large `Select All` must not create an unbounded list of IDs or automatically download 100,000 files. The model may represent it as a bounded explicit selection or a revision-bound `all loaded items` selection with clear label; if not all directory entries are loaded, **do not imply selection of unseen files**. Range selection of unloaded rows either loads the bounded needed range first or clearly refuses until it is available.
- Context actions consume an immutable **selection snapshot** (provider, local identity/contact, directory, item IDs, generation and known metadata) and revalidate every affected target before action. For a mixed selection of files and directories, present actions only if they are actually valid; show partial availability and per-item errors for batch results rather than pretending one atomic operation.
- Support selecting one or several ordinary files and **Download** in remote mode; each becomes an independent existing generic transfer/queue work item, with bounded batch admission and progress visible in `../transfer_queue_ui.md`. The initial download action is for files only; recursive directory download requires an explicit later workflow. Do not enqueue content simply by selecting or opening a directory.

## 6. Context menu and actions

Right-click (desktop) and long-press (touch) open the same context/action model; keyboard context-menu key and Shift+F10 work. Right-click on a nonselected item selects that item according to normal desktop convention before acting; right-click on an existing multi-selection acts on the selection. Empty-space context menu contains folder-level actions only.

C++ determines available actions from `BrowserLocation`, authenticated identity/contact, selected entry types, current live capabilities, **effective VFS authorization**, backend readiness and operation state. QML does not recreate this policy. Initial actions:

| Action | Local | Remote |
| --- | --- | --- |
| Open directory / Navigate to parent | Yes, if accessible | Yes, if LIST/traverse permits |
| Refresh / inspect details | Yes | Yes, if permitted/connected (cached details marked stale) |
| Download selected file(s) | Via local materialization/export when meaningful | Yes, with current READ/DOWNLOAD and generic transfer offer/acceptance |
| Open file / Preview | Local safe read/view when permitted | Only if already locally verified or supported preview is explicitly fetched/authorized |
| Copy path / filename | Logical VFS path/name | Logical UNIX-style virtual path/name, never remote OS/native path |
| Create directory / Rename / Move / Delete | When permitted and backend supports it | **Disabled/absent** until separate remote mutation protocol and upload spec |
| Upload selected file(s) | Feature-specific import when writable | **Disabled/absent** until `fileshare_uploads.md` is implemented |
| Properties | Authorized portable metadata | Authorized portable metadata only |

Offer useful selected-count and batch operations (Download N files, Cancel queued batch through transfer queue where applicable). A permission or backend capability change while the menu is open invalidates commands before execution. Disabled operations must not offer a fake button or silently fail. Show a short reason when the user needs to understand why a normally available action is unavailable. Never expose denied or hidden remote paths via menu text.

## 7. Performance, caching and security

One browsing model may be reused across several open lightweight views, but each active view has a stable context and **independent viewport/selection/navigation state**; avoid sharing mutable current-folder state between simultaneously visible browser windows. Reuse safe provider caches across matching scopes where useful without merging identities/contacts. Close/release subscriptions/cursors and cancel or fence outdated requests on navigation, scope changes, destruction and revocation.

Remote cache may retain **validated authorized historical metadata** for convenient offline navigation; label it `Offline / last refreshed` and never infer current READ permission from it. Never retain raw remote native paths, private credentials, physical namespace IDs, hidden counts or unfiltered backend generations. Directory revisions, timestamps, hash and special-file exposure follow `../vfs.md`, `fileshare_protocol.md` and `fileshare_server.md`; use ASCII percent-escaped presentation whenever either peer's preferences require it. Do not double-decode logical labels.

Use the same transfer queue for all file downloads; no separate browser transfer or checkpoint state machine. Check admission and permission at time of each GET and subsequent transfer/resume, regardless of whether the browser believes an object exists. Ordinary browsing must not depend on exchange credits, which default off. Resource and network failure should degrade to accurate cached/partial state and remain responsive, with bounded retry on user action.

## 8. Delivery and focused tests

**First interactive milestone** (after administration and read-only server are available): open local and remote instances of the browser, navigate folders through clickable parent breadcrumbs, switch among Thumbnail/Small icons/Details, see a known-but-loading directory fill progressively from slow paged LIST responses, choose one/multiple files, request download, observe transfer queue and open locally verified results. Check that permission denial hides items and disabled actions cannot run. Do not block first interactive browsing on uploads, credits, dashboard, recursive directory downloads, advanced remote thumbnails or complete corner-case coverage.

**During implementation** provide focused deterministic C++ model/provider tests plus a few QML interactions and two-peer private-Tor smoke tests for:
- Single model/provider contract switching Local/Remote, scoped identity/contact, stale response rejection, breadcrumbs and folder navigation.
- Multiple paginated batches (including empty page with continuation), `Known/Loading/Partial/Complete` transitions and 100k-entry synthetic index/window fixture. Confirm bounded signal/repaint work and thumbnail requests for visible rows only.
- Details optional column/view-mode persistence, basic selection/multi-selection, range/select-all semantics on partial folders, right-click/long-press action correctness.
- Remote download selected files, transfer-queue integration and current authorization recheck.
- Atomic replacement of a remote cached recursive catalog **only after** whole-artifact validation; denied recursion yields no listing entries or partial cache.

After the feature can be tested interactively, Codex may add comprehensive revocation/cursor/sort/reconnect/multi-view, malicious catalog and stress/fault matrices using the existing test-MCP/CTest infrastructure.

**No-goals for the initial browser:** independent physical filesystem access, a second transfer engine, bulk recursive filesystem copying, automatic whole-directory downloads, editable remote metadata, full FTP upload/mutations, advanced analytics, tags/ratings or a new generic QML filesystem implementation.
