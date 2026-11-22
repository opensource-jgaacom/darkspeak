# FileShare Stage 1 — interactive read-only FTP implementation checklist

**Active goal:** Two actual DarkSpeak instances can configure a share, authorize a contact, browse its UNIX-like virtual filesystem and download selected files interactively, with one application-wide transfer queue and UI. Prioritize a working end-to-end vertical slice over exhaustive tests. This is the **Stage 1 worklist only**; specifications define behavior, not additional automatically approved tasks.

Follow `agents.md`: implement one checked item/increment at a time; check off immediately with short evidence (test/commit); keep test registration and docs current; do not expand scope without owner approval. **Before full application builds or the two-instance test gate, coordinate with the owner as required by build rules.** Maintain security/correctness invariants throughout. No separate FTP filesystem, transfer engine, queue per feature, or protocol compatibility fallback.

## Acceptance target

- [ ] **Interactive Stage 1 accepted:** On instance A, configure a read-only directory, assign it to a local identity, mount it and grant a specific contact access. On instance B, use the shared browser model in Remote mode to navigate authorized folders, see paginated/slow listings, select one or multiple regular files and download them. Confirm full-file SHA-256, current permissions, generic checkpoint/resume and progress in the application-wide Transfers view. Repeat the essential steps after restarting both instances; the GUI stays responsive.

**Stop on acceptance.** During each increment add narrow unit/integration tests for the path just implemented (including immediate authorization, confinement, integrity and recursion-cycle safeguards). After interactive acceptance, the owner may separately schedule broad failure-injection, cross-product permission matrices and exhaustive regression coverage.

## Verified implementation foundation — do not reimplement

- [x] Coroutine-owned generic incoming/outgoing transfer sessions, checkpoint/restart mechanics and corresponding targeted suites have been implemented; previous worklist evidence: `managed_storage` 97 cases, `checkpoint_restart` 24 cases and selected transfer/protocol/recovery tests passed on 2026-11-19. Production advertises mandatory `file-checkpoint-v1`. **Full real-application verification below is still open.**
- [x] Reusable `IFileManager`, read-only indexing, VFS resource/assignment/mount APIs, authorized paginated listing, export/validation and `addVfsTransfer`/`addCatalogTransfer` adapters exist in source. This checkbox verifies source availability, **not** completion of FTP application integration or a fresh test run.

## Ordered Stage 1 implementation

### 0. Close the existing generic-transfer application gate

- [ ] Coordinate the full owner-built app and MCP-enabled two-instance private-Tor smoke test; send a normal conversation attachment, verify received SHA-256, interrupt/restart and resume from a mutually verified checkpoint. Record actual build/test evidence. Fix only defects directly blocking this baseline; no second attempt controller or checkpoint redesign.

### 1. Minimal usable local share administration

**Specs:** `specs/ftp/fileshare_ui.md` (storage/mount/permission slices), `specs/file_manager.md`, `specs/read_only_file_manager.md`, `specs/vfs.md`.

- [ ] Expose application-wide read-only storage registration/inspection, bounded asynchronous indexing, source status and removal in the existing UI via C++ service/models. Validate non-overlapping source roots, exclusions, symlink settings and errors. Keep all background work off the UI thread.
- [ ] Expose identity resource assignments and the minimum identity-owned storage/VFS administration needed by the existing backend: select assigned directory/subtree, create/edit/remove identity mounts at virtual UNIX-like paths, display effective composed root, preserve settings across restart. Prevent accidental exposure: registration/assignment alone never shares data.
- [ ] Provide identity/contact-scoped permission editing and **View as Contact** using the production evaluator. Persist core LIST, READ/DOWNLOAD, RECURSE, EXPORT, DENIED and required traversal rules; follow existing group and scope precedence instead of inventing simplified FTP-specific rules. Implement only the general identity-scoped group membership/configuration needed for accurate effective views. Propagate changes/revocation immediately to live handles.
- [ ] Add focused model/UI + backend tests for add/assign/mount/grant/deny, source confinement, restart persistence, and one active permission withdrawal. Surface actionable errors instead of dummy controls.

### 2. FTP v1 read-only protocol and remote server

**Specs:** `specs/ftp/fileshare_protocol.md`, `specs/ftp/fileshare_server.md`, `specs/ftp/fileshare_authorization.md`, `specs/vfs.md`.

- [ ] Implement `fileshare-v1` negotiation and bounded, authenticated protobuf control messages in `proto/darkspeak.proto`: typed `VirtualPath` with explicit UTF8/ASCII_ESCAPED, nonrecursive LIST pages, optional recursive request/catalog result, STAT, GET and request cancellation. Reuse v3 envelope correlation/security validation. No native OS paths or host-specific errors on the wire.
- [ ] Add a contact/identity-bound application FileShare service, not storage behavior in `Peer`. Resolve remote virtual paths through the existing effective VFS and current core authorization; enforce mount context, assignments, path traversal, LIST versus READ versus EXPORT, remote privacy and resource caps. Nonrecursive LIST returns bounded authorized pages, including empty pages with continuation.
- [ ] Implement optional explicit recursive catalog admission: server approves or refuses before enumerating; **refusal sends no entries, counts, metadata or artifact offer**. Approved exports use `Vfs::exportCatalog` and the generic immutable transfer adapter; validate whole catalog and SHA-256 before publishing remotely cached metadata.
- [ ] Require **cycle-safe recursive traversal** in the VFS/read-only backend and exports: resolved backend directory identities plus bounded active-ancestor/visited tracking across symlinks, directory hard links where supported and repeated mount/alias routes. Never recurse indefinitely or borrow permissions across aliases; fail incomplete exports safely.
- [ ] Implement GET using the existing prepared VFS reader and generic checkpointed sender, reauthorizing at admission, subsequent reads and resume. Filename/path presentation must remain host neutral; on Windows, native external shares are read-only.
- [ ] Add focused tests for permitted/denied LIST/GET, escaped name round trip, zero-data recursion refusal, approved complete catalog, a symlink loop and repeated-mount loop, a changed/denied source, and one successful verified download. Keep hostile-input size/encoding validation in this initial slice.

### 3. Shared C++ local/remote browser and interactive navigation

**Spec:** `specs/ftp/fileshare_browser.md`.

- [ ] Implement **one C++ browser model** with explicit `Local`/`Remote` enum and provider-scoped identity/contact, backed by a narrow VFS-oriented local adapter or FileShare remote adapter. One QML browser, no duplicated sorting/selection/navigation controllers.
- [ ] Implement one-folder navigation, parent-selectable breadcrumbs, Back/Forward/Up/Refresh; display known/loading/partially listed/complete/offline-cached/unavailable states distinctly. Reject stale responses after directory/contact switches.
- [ ] Provide Dolphin-inspired thumbnail, small-icon and details modes (optional columns) with persistable view settings; thumbnails only for safe, visible/near-visible items. Do not require a remote thumbnail wire extension to make normal browsing usable.
- [ ] Incrementally incorporate remote LIST pages and local VFS changes with **viewport-aware** batching/windowing; avoid individual QML repaint events for offscreen entries. Preserve Qt model notification correctness, stable ordering/IDs and bounded RAM/CPU. Demonstrate a synthetic 100k-entry directory without constructing 100k QML objects or blocking the UI.
- [ ] Implement stable-ID single/multi/range selection, keyboard and right-click/long-press context menu. Available actions derive from local/remote and actual capability/authorization; select multiple remote files and request individual generic downloads via the same transfer service.
- [ ] Add focused C++ provider/model and QML smoke tests: local/remote switch, breadcrumbs, slow paginated listing, selection + Download N files, viewport signal bounds, and disabled unauthorized actions.

### 4. One application-wide transfer queue and Transfers UI

**Specs:** `specs/transfer_queue.md`, `specs/transfer_queue_ui.md`, `specs/protocol.md`.

- [ ] Add durable feature-neutral work items and scheduling over existing `TransferManager`/`TransferSession`. Integrate conversations/attachments, previews, directory catalogs and FileShare downloads without duplicate attempt state machines. Persist user ordering and paused/eligible state across restart; reconcile from the authoritative checkpoint journal.
- [ ] Implement trusted scheduling classes: conversation attachments ahead of bulk FTP by default, control/chat traffic always higher than payload; bound admission and ensure fairness. Expose user reordering, High/Normal/Low priority and safe pause/resume/cancel where supported; do not allow remote peers to claim priority via filenames/MIME.
- [ ] Expose a single **Transfers** UI/model with **application-wide**, **per-identity**, and **per-contact-within-identity** overviews. Include both directions, active/queued/paused/completed status, effective priority and progress (distinguish provisional bytes from durably verified bytes); actions affect the same work regardless of filter/scope.
- [ ] Implement **checkpoint-safe priority preemption** only using the existing cooperative verified pause/resume contract: an eligible conversation transfer can yield an occupied FTP bulk slot at a safe durable boundary. Verify existing sender/receiver semantics first; if they cannot support it safely, report the exact blocker rather than introducing a new transfer protocol. Preserve anti-thrashing and fairness.
- [ ] Focused tests: conversation attachment admitted before queued bulk work, reorder survives restart, one pause/checkpoint/resume scenario, UI scope consistency, no content corruption or lost reservations. The browser and conversation cards use the same queue command/state API.

### 5. Interactive two-instance acceptance and handoff

- [ ] Build/run two current application instances under the approved private-Tor test setup. Walk the **Acceptance target** through the real GUI: configure/authorize, browse via breadcrumbs/modes, show large slow directory arriving in pages, download one and several files, change ordering, pause/resume a bulk item and confirm conversation attachment priority. Check SHA-256 and that revoked access cannot continue a download.
- [ ] Run only affected focused unit/CTest/MCP/QML scenarios and required basic integration/regression tests as work changes. Register tests with bounded timeouts, keep `tests/run-all-tests.sh` / `tests/README.md` current, and never claim a passing result from inspection alone.
- [ ] Record actual evidence, known limitations, and completed boxes; declare **Stage 1 interactively testable** and stop. Comprehensive corner-case test authoring is a separate follow-up.

## Stage 1 scope control

Use only the referenced read-only FTP and shared-core specifications for work above. Implement permissions, storage resource guards, link-cycle avoidance, authenticated protobuf validation, mutual SHA-256 checkpoints, cancellation and active revocation **before** exposing unsafe operations interactively. Do not let file size, network slowness or large directory metadata cause unbounded work or UI stalls. Any extra feature or broad redesign requires owner approval.

This checklist intentionally contains **no work items for functionality outside Stage 1**. The underlying specifications remain authoritative and unchanged.
