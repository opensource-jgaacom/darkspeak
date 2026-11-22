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

- [x] Coordinate the full owner-built app and MCP-enabled two-instance private-Tor smoke test; send a normal conversation attachment, verify received SHA-256, interrupt/restart and resume from a mutually verified checkpoint. Record actual build/test evidence. Fix only defects directly blocking this baseline; no second attempt controller or checkpoint redesign.

  **Verified 2026-11-22:** With explicit owner authorization to build, configured this checkout at source revision `f9549325f855bae26983c1c86c4dcc3c0d401b45` in fresh `/tmp/darkspeak-stage1-mcp-hcFGTu` (Debug/Ninja; `BUILD_TESTING=ON`, `DARKSPEAK_WITH_MCP=ON`, `DARKSPEAK_WITH_TEST_MCP=ON`). Installed `tests/mcp/requirements.txt` in its private Python venv and built targets `darkspeak` and `mcp_test_instance` with three jobs. Initialized the missing pinned `dependencies/logfault` submodule; no source defect or protocol change was required.

  **Focused evidence:** Sequential CTest `^mcp_peer_(happy_path|resume_crash|receiver_crash)$` passed **3/3** (359.70 s): ordinary conversation attachments and received SHA-256, sender and receiver crash/restart, nonzero checkpoint resume without duplicate transfer records, and resumed full-file SHA-256. CTest `^qml_smoke$` passed **1/1**. Also ran `tests/mcp/run_scenario.py --scenario resume_crash` with `--instance /tmp/darkspeak-stage1-mcp-hcFGTu/bin/darkspeak`: **18/18 checks passed**, including restart, nonzero resume, SHA-256 and cleanup. This uses the full application executable's isolated headless MCP test mode; QML startup was verified separately, not a manual two-window GUI walkthrough.

  Build/configuration logs and `gate-tests.log`/`qml-smoke-tests.log` are in that build directory. Sanitized reports: `tests/mcp-artifacts/{happy_path-00b327a3407e484788a185dde938cb30,resume_crash-53859b2869284010972fb4ffd1fd3314,receiver_crash-d1977389fca149edb32e8e1950e290ed}.json`; application report: `tests/app-mcp-artifacts/resume_crash-d412a401974d4ebdb762545bf61ed245.json` (all paths relative to the build directory). These `/tmp` artifacts are temporary; this paragraph retains the result. Full Stage 1 interactive acceptance remains open below.

### 1. Minimal usable local share administration

**Specs:** `specs/ftp/fileshare_ui.md` (storage/mount/permission slices), `specs/file_manager.md`, `specs/read_only_file_manager.md`, `specs/vfs.md`.

- [x] Expose application-wide read-only storage registration/inspection, bounded asynchronous indexing, source status and removal in the existing UI via C++ service/models. Validate non-overlapping source roots, exclusions, symlink settings and errors. Keep all background work off the UI thread.

  **Verified 2026-11-22:** `ReadOnlyResources` and Settings → Files / Paths → Read-only storage provide bounded application-wide registration, canonical source/policy/exclusion inspection, background indexing/status refresh, versioned restart configuration and dependency-safe unregister/index cleanup without deleting source files. Uses the shared `ManagedStorage` backend and mandatory private exclusions; registration grants no VFS assignment or contact permission. Canonical/filesystem and mount-table ancestry reject overlapping/aliased roots; externally conflicting or unverifiable roots are closed conservatively. Shutdown stops new model operations before storage drains.

  **Focused evidence:** Built only new target `readonly_resources_test` and its dependencies; CTest `^readonly_resources$` passed **1/1**, **11 QtTest cases, 0 failed/skipped**, including the v1 registry fixture, restart, exclusion/policy/overlap rejection, assignment/authorization refusal, external-overlap quarantine, failed-publication recovery, event-loop responsiveness and the actual QML dialog with populated production model and no QML warnings. Retained logs: `/tmp/darkspeak-readonly-resources-{build,ctest}-verified.log`; build directory `/tmp/darkspeak-stage1-mcp-hcFGTu`. Updated CTest timeout, runner documentation and test catalog. MCP impact reviewed: equivalent reusable model/service harness supplied; no new MCP API. Full application target/build and real-window integration remain owner-run; no bind-mount namespace scenario was executed in this focused suite.
- [x] Expose identity resource assignments and the minimum identity-owned storage/VFS administration needed by the existing backend: select assigned directory/subtree, create/edit/remove identity mounts at virtual UNIX-like paths, display effective composed root, preserve settings across restart. Prevent accidental exposure: registration/assignment alone never shares data.
  **Verified 2026-11-22:** Identity → FileShare uses the reusable C++ `ShareMounts` service/model and `ShareMountDialog.qml` for bounded read-only source/subtree selection, identity assignments, identity mount create/move/remove, and paginated production-resolver composed-root inspection. Existing VFS configuration persists stable assignments/mounts across restart. Commands validate local identity and stable IDs, reject existing virtual/source-child collisions and out-of-assignment roots, refuse unassignment until dependent mounts are explicitly removed, and keep external-file capability disabled. Registration/assignment creates no mount or contact grant; the preview is explicitly local administration, not View as Contact. Only existing read-only sources are offered; application-private Downloads/cache namespaces are never repurposed as identity storage.

  **Focused evidence:** Built only new `share_mounts_test` and its required dependencies; CTest `^share_mounts$` passed **1/1**, **5 QtTest cases, 0 failures/skips**, including subtree confinement, assignment without exposure, mount CRUD/restart/stable IDs, source-child collision and invalid identity/path rejection, dependency refusal, source retention and populated production QML without warnings. Logs: `/tmp/darkspeak-share-mounts-{build,ctest}.log`; build directory `/tmp/darkspeak-stage1-mcp-hcFGTu`. CTest timeout, full-catalog runner review and test documentation updated. MCP impact: reusable model/service test harness covers the same commands; no new MCP API. Full application build and real-window integration remain owner-run.
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
