# Running all tests

`transfer_session` exercises the coroutine owner in isolation: shared contact
admission, stale event rejection, bounded chunk/control queues, joined cancellation,
deadline wakeup and disconnect versus local publication. It requires no profile,
network or application binary and has a 30-second CTest timeout.
The `managed_storage` coroutine receiver cases exercise empty/multi-checkpoint
publication and cancellation with a withheld confirmation through the new owner.

From any working directory, invoke the repository's test entry point:

```sh
./tests/run-all-tests.sh
```

It prepares a private Python virtualenv from the pinned
`mcp/requirements.txt`, configures all test features, builds the application and
test binaries, and runs the complete CTest catalog sequentially. This includes
unit, file-manager, functional, Qt Quick, QML smoke, MCP and private-Tor acceptance
tests. Future acceptance tests are included when registered with CTest. The
legacy optional `tor_integration` registration is enabled too; its current
implementation reruns the crypto handshake test, while the real peer scenario
uses the isolated private Tor network.

The existing file-manager test registration also checks common entry/hash/readiness
and capability metadata, visibility/read refusal for updating entries, and Linux
source-name encoding (raw invalid UTF-8, escaped-percent collisions, case and
normalization distinctions, and maximum expansion). These need no extra flags or
dependencies.

The preferences tests include desktop notification redaction combinations and
the global enable setting; managed-storage tests also cover identity preference
defaults, isolation and persistence. They use the existing test registrations
and require no additional runner flags or dependencies.

Image-preview coverage includes the `media_preview` codec test, signed attachment
validation in `protocol_v3`, private-cache and deletion checks in
`managed_storage`, a Qt Quick offer-card binding check, and the isolated Tor
`mcp_peer_image_previews` / `mcp_peer_image_previews_disabled` scenarios. They
use existing Qt image plugins and checked-in assets; no video helper is needed.

The managed-storage suite also exercises reconnection on queued messages and
files with a fake transport: offline identities, disabled contact auto-connect,
connection-attempt coalescing, manual cancellation, blocks and inbound-only work.
It does not contact Tor or a peer for these checks.

Transfer lifecycle coverage also checks persisted checkpoint mode, typed pauses,
partial-content retention, cancellation, terminal failure recovery, stale-attempt
isolation and recovery when the application pause mirror fails. Scoped
transfer-ID collision tests use separate contacts without network access.
`database_recovery` checks real SQLite reopen and idempotent restart recovery of
checkpoint, legacy and credit-wait states. Numeric states and the versioned
application mirror schema are frozen in `fixtures/transfer-lifecycle-v1.json`.
The managed-storage suite checks that asynchronous resume validation takes a
concurrency slot before filesystem work. UI component tests check pause reasons
and resume visibility without opening real profiles or Tor connections.
These extend existing CTest registrations and require no runner changes.

Checkpoint recovery tests additionally enforce exact retained references (including
final/pause flags), EOF-only selection for completed transfers after restart, and
empty-file final references. `transfer_checkpoints` verifies that attempt preflight
does not mutate progress and that commit rechecks terminal failures.
`managed_storage` verifies that unretained checkpoints and stale sequence choices
cannot invalidate partial bytes, and that superseded validation cannot install or
restore an old runtime attempt. These use the existing bounded CTest registrations;
`run-all-tests.sh` needs no new targets, flags or dependencies.

The managed-storage checkpoint matrix also runs real authenticated encrypted
loopback peers through the application contact queues: 64 KiB, 1 MiB and 16 MiB
intervals, empty EOF, signed primary/preview manifests, withheld checkpoints,
stale-attempt ACK rejection and advisory pauses without progress advancement.
The matrix also exercises nonterminal checkpoint resume after a lost mutual
receipt, bounded fallback after receiver-prefix corruption, source absence,
locally proved source mismatch and conservative backend revalidation refusal. Rewind
coverage rechecks accepted source prefixes before their boundaries are sent, including
short pause prefixes inside ordinary chunks. Known checkpoint mismatch, unsupported
version and remote source-failure cancellations retain typed outcomes without producing
local corruption/refund proof.
Malformed retained candidates fail closed before application dispatch;
duplicate exact readiness coalesces while its journal commit is pending, and
late offer rejection cannot rewrite a completed outcome. Shared contact work
budgets remain occupied until cancelled asynchronous work actually returns.
The format-three progress journal fixture covers direction-scoped key bytes and a
monotonic verified prefix through resume rewinds and checkpoint retention. Empty
format-one/two catalogs can be reinitialized transactionally; populated catalogs
remain untouched and unsupported, including across an injected commit failure. Corrupt
catalog scalar values, attempts, sequences and completion claims fail closed on load.
EOF recovery cases drop the final checkpoint receipt or flow ACK, reconnect the
actual peers and verify automatic sender state repair. Empty files, unavailable or
corrupt published content, and attempted earlier-prefix selection are included. Repeated offers coalesce; a completed receiver keeps
its state and published object, opens no pending writer and receives no payload bytes.
The journal fixture separately retains explicit acceptance of an empty final checkpoint.
Outgoing signed manifests reject missing or wrongly directed local transfer records
before sending, preserving checkpoint-mode validation. Preview recovery repeats the
same signed attachment manifest and keeps an unaccepted original unaccepted while
its thumbnail's completion is repaired.

`checkpoint_restart` uses fresh OS processes and isolated profiles for each stage.
It terminates live authenticated loopback transfers after the initial offer, at a
checkpoint, after a lost final receipt, and immediately before/after publication.
Its twenty-four scenarios check explicit and offline acceptance after restart, retained-prefix
reuse, automatic EOF repair, stable published-object identity and zero retransmitted
bytes for EOF. Preview cases preserve the signed manifest and unaccepted original;
they also exercise initial readiness after runtime cache eviction and reject a forged
nonce before any payload or journal advancement. Preference-change cases retain
the original frozen interval for standalone and preview recovery, then reject
oversized-interval offers for unknown IDs or changed content before dispatch.
Policy cases withdraw image-preview permission across restart and during a live
multi-chunk JPEG; the latter retains a typed pause and resumes manually after
permission is restored. Four unsupported-peer cases cover fresh acceptance,
offline-approved queues, retained partials and waiting senders. They preserve the
checkpoint mode and evidence, retain a typed failure across restart, then verify
that a fresh ordinary attachment also aborts without an offer or payload.
Protocol compatibility is not a test goal: every transfer requires checkpoints;
unversioned wire offers, readiness, chunks, ACKs and resumes fail before dispatch.
Storage/channel algorithm fixtures remain local and cannot emit unversioned wire
traffic through the production send APIs. Retired-I/O tests suspend preparation
and reads, destroy the active channel and prove its budget slot remains occupied
until return; a native publication case also retains its reservation through its
final completion. Source preparation retains its original deadline before payload.
The seventeenth restart case fails a resumed readiness send once, then verifies
that a manual retry from its typed pause can reclaim admission and send only
the remaining seven bytes. Negotiation pauses do not require a payload channel.
The eighteenth reconnects while the receiver is Hashing, delivers the completion
hint before native publication starts, and verifies automatic sender completion
without more payload, another writer or a different published object.
Lost-offer cases suppress the original standalone offer or signed preview manifest,
then verify fresh persisted attempts after restart, explicit original approval and
an independently transferred preview. A forged old-connection readiness is rejected
before it restores any runtime attempt or advances payload. A delayed source
preparation exercises concurrent explicit acceptance using the same work reservation
and original deadline.
Two further cases fail the initial offer queue and crash during source
preparation before installing its attempt. They recover the same transfer
without treating zero progress as receiver approval.
The managed-storage offer-budget regression fills all 64 pending slots, evicts
the first File object, and cancels, fails or deletes a reloaded object. Each action
must admit a new offer, while state/deletion events for a distinct incoming row
with the same wire ID must leave the outgoing budget occupied.
The store suite checks stale and cancelled failure transactions against a newer
attempt. A cancellation regression keeps an unstarted receiver journal at zero
with its own UserAction outcome despite a cached sender hint. The managed-storage
policy matrix also separates image and video preferences, JPEG permission, original approval and contact state.
The publication case commits through the storage participant API and terminates
before the application can save its completion; it uses no production fault hook.
Each child is bounded to 30 seconds and the complete CTest case to 180 seconds.
The full runner discovers this new executable without extra flags or dependencies.
`protocol_v3` also verifies socket lifetime when a disconnect handler releases
its last peer reference, including graceful and abrupt loopback disconnects.
Sockets use one factory with deferred Qt deletion so network callbacks finish
before destruction; accepted, outgoing and retried connections share this policy.
The protocol CTest registration has a 30-second timeout.
Production advertises mandatory checkpoints at the default interval after the
coroutine and restart regressions passed. `protocol_v3` rejects unilateral checkpoint capability
before application dispatch. The existing managed-storage CTest timeout is 180
seconds for this expanded matrix; the full runner already honors it and needs no
additional dependencies or flags. Set `DS_TEST_TRACE=1` for fixture trace logging.

Install the normal C++/Qt/MCP dependencies, Python 3.12+ with venv/pip, `tor` and
`tor-gencert` and `ffmpeg` first. Video decoding tests need a Linux kernel with
Landlock and seccomp support. The script does not install system packages. Dependency
preparation can use the internet; peer tests use local Tor and checked-in assets.

Video coverage includes actual FFmpeg frame extraction, a test subprocess that
verifies descriptor/environment isolation, filesystem/network restrictions,
output and time limits, and cancellation. `video_preview_support` checks native
startup advice and per-profile “Don’t show again” persistence; Qt Quick checks
the dialog controls. `mcp_peer_video_previews` and
`mcp_peer_video_previews_disabled` verify real peer video preview transfers and
opt-out with the small generated FFV1 fixture in `fixtures/media-preview-v1/`.

```sh
./tests/run-all-tests.sh --build-dir /tmp/darkspeak-tests --jobs 3
./tests/run-all-tests.sh --python /path/to/python3 -- -DCMAKE_PREFIX_PATH=/opt/Qt
./tests/run-all-tests.sh --dry-run
```

The default build directory is `/tmp/darkspeak-all-tests-$UID`. Builds and the
virtualenv are retained for reuse. Test HOME/XDG directories are temporary and
removed on exit; acceptance tests additionally manage their own profiles and
process cleanup. Core dumps are disabled. Scenario reports remain in
`<build-dir>/tests/mcp-artifacts/`; only sanitized reports should be shared.
CTest logs and GUI smoke profiles remain private in the build directory.

Configure/build errors stop immediately. CTest continues through independent
tests and returns failure if any test fails. Each test's own timeout takes
precedence over the 180-second default. There is no test-name filter or silent
fallback when acceptance infrastructure is unavailable. See
[MCP infrastructure](mcp/README.md) for individual scenarios and failure handling.

Whenever tests are added, removed or renamed, maintain this script and the CTest
registrations in the same change, as required by [agents.md](../agents.md).

The sequential CTest catalog also includes private-Tor bio privacy/multiple
identity, interrupted-file resume and contact-deletion scenarios. The top-level
runner was reviewed for these additions: its unfiltered CTest invocation discovers
all five, per-test deadlines override the default, and no extra dependencies or
build flags are needed. See [scenario definitions](mcp/scenarios/002-error-and-isolation.md).

`database_recovery` checks both transfer directions across every persisted state,
including idempotent restart recovery and preservation of nonzero offsets. It uses
the existing Qt/Core test dependencies and is discovered by the same full runner.

The managed-storage suite also checks that a resumed sender emits no chunks
until receiver readiness, preserves the negotiated offset and becomes resumable
on disconnect. Readiness is bounded to two pending transfers per contact and
30 seconds; no additional dependency or test-runner option is needed.

The full runner also discovers `mcp_peer_file_decisions` and
`mcp_peer_corrupt_partial`. Both use the same build flags and pinned dependencies;
fault setup is restricted to stopped private test profiles. Interruption waits
for confirmed durable progress; resume observations use the selected checkpoint,
and corrupted prefixes rewind before full-hash-verified publication. See
[transfer-error scenarios](mcp/scenarios/003-file-decisions-and-corruption.md).


`storage_diagnostics` (30-second timeout) verifies TRACE diagnostics for rejected
catalog schema versions, missing/invalid persisted limits and SQL preparation
failures, including native codes and failure source locations. Private catalog
values stay out of diagnostics; a current catalog still reopens normally.

`readonly_resources` (60-second timeout, offscreen/software rendering) compiles
the standalone administration model and its backend/VFS dependencies without
building the application target. It covers registration without sharing, bounded
asynchronous status, canonical/alias/ancestor root overlap, exclusions and policy
validation, persisted reopen, dependency/authorization refusal, index removal
without source deletion, external overlap quarantine, corrupt configuration,
event-loop responsiveness, and the production QML dialog using the same model.
The full runner automatically builds and discovers this test. No production or
test-MCP API is added; the reusable model/service harness exercises its commands.

`readonly_filemanager` covers the external read-only backend, raw Linux names,
startup/restart reconciliation, partial-scan safety, Cached/Rehash/Snapshot
preparation, staged resume/expiry, resource quotas, subscription pins, opaque
requester-bound cursors, cross-backend aliases and exclusion revocation. The
bind-mount case uses `unshare` when private user/mount namespaces are available;
otherwise that subcase reports a skip. No additional Qt module is needed.
`managed_storage` also checks mandatory DarkSpeak exclusions and actual sender
completion/failure, including a resumed prefix and tampering that restores mtime.

The full runner discovers `readonly_million_entry_paging` without a test-name
filter. That scale test reports CTest skip code 77 unless
`DSFM_RUN_MILLION_ENTRY_TEST=1` is set and at least 6 GiB of temporary storage is
available. When enabled it creates and indexes one million real files using finite
entry/catalog/page budgets, checks complete keyset paging and UI responsiveness,
and has an explicit 1800-second timeout. The ordinary read-only suite performs the
same paging checks with 1,200 real files. Enable the larger run with:

```sh
DSFM_RUN_MILLION_ENTRY_TEST=1 tests/run-all-tests.sh
```

`vfs` covers identity/mount composition, persistent opaque IDs and format fixtures,
subtree confinement, mount-scoped authorization, synthetic pagination, filtered
subscriptions, cross-mount alias exclusions, revocation, writable delegation and
bounded catalog exports/validation. It runs against real FileManager backends
with a 120-second CTest deadline and no extra test-runner flags.
`recursive_cycles` checks resolved-directory deduplication across repeated/nested
mounts and a read-only symlink loop, route-specific authorization, bounded export
failure, revocation and replacement of an indexed source directory (60 seconds).
`managed_storage` also checks Home-pane filesystem-conflict notifications and
catalog payload completion/revocation through the existing outgoing transfer
channel. See `src/vfs/README.md` for the standalone test command.

The `vfs` target also covers persistent storage assignments and withdrawal,
identity/group/contact overlays, permission precedence/group conflicts,
nonrecursive parents and navigation-only reopening, bounded empty-page
continuations, and assignment-gated external-file symlinks. These cases are part
of the existing CTest target and require no additional runner dependency.

`storage_pool` exercises shared filesystem admission, independent usage classes,
namespace identity checks, materialized/outstanding accounting, retained claims
across restart, per-authority owner locking and independent concurrent profile authorities. It runs sequentially with temporary
private journals and no real profiles or network access.

`temporary_artifacts` covers private native publication, immutable finalized reads,
owner-bound resume leases, revocation, expiry with open handles, class isolation,
read-only snapshots sharing the pool, and process death during creation,
publication and deletion. `transfer_checkpoints` covers the independent durable
progress journal, sender-selected retained checkpoints, final/empty confirmation,
late duplicates, stale attempts (including delayed pause writes), interval bounds, and withheld-checkpoint fences.
`managed_storage` additionally exercises the sender and receiver channel fences,
ordinary ACKs without durable progress or per-ACK SQL updates, incorrect receiver
digests without source-integrity refunds, and final/empty-file confirmation before
publication and final acknowledgement. It also exchanges a complete payload
between real sender/receiver channels, exercises clipped chunks after a short
paused prefix, and retries the final chunk after publication.
Both use isolated private SQLite journals and process-kill fault injection.

`share_administration` (60 s, serial/offscreen) exercises the three production administration models together with the read-only backend and VFS. It invokes actual QML registration, assignment, mount, confirmed contact grant/deny and View as Contact controls; checks actionable error bindings and valid retry; recreates all models, backend and VFS to verify stable IDs and persisted grants/denials; verifies subtree confinement, SHA-256 and immediate withdrawal during a partial read. Profiles and sources are temporary; no Tor or application build is needed. The all-tests runner discovers it through CTest.

`share_mounts` (60 s, offscreen) exercises the reusable identity mount editor: subtree assignment, mount creation/move/removal, restart, dependency refusal, invalid identity/path rejection, no implicit contact access and the production QML dialog. The all-tests runner discovers it through CTest.

`share_permissions` (60 s, offscreen) exercises identity/group/contact rules through the production evaluator and VFS: deny defaults, independent LIST/READ/EXPORT, multi-group precedence and provenance, stable membership persistence, identity isolation, failed publication, stale edits, version rejection, active reader withdrawal, permission-only reopening through sanitized navigation ancestors, snapshot STAT, ordinary RECURSE denial, SHA-256 and the populated permissions QML dialog. The checked-in `fixtures/identity-access-v1.json` locks the identity/group/policy transaction format. The all-tests runner discovers it through CTest; no new dependencies or application build are required for this focused target.

`fileshare_protocol` (30 s) covers the version-one FileShare wire fixture, all eight controls, canonical typed UTF8/ASCII_ESCAPED virtual paths, raw malformed UTF-8 and duplicate-field rejection, portable metadata/result variants, zero-data recursive refusal, request/cursor/page bounds, bilateral capability/readiness gates, correlation/replay/expiry, cancellation and late-result suppression. An isolated loopback socket verifies encrypted capability output and the production request/response send lifecycle; it does not use Tor, real profiles, storage or the application executable. Build only `fileshare_protocol_test` for focused verification.

`fileshare_service` (60 s) exercises the application FileShare controller with the production read-only backend, VFS and contact permission evaluator. Covers encrypted Peer dispatch, direct LIST/STAT without READ or EXPORT, subtree confinement, indistinguishable denied/missing errors, UTF8/ASCII_ESCAPED names, opaque connection-scoped node references/cursors, empty pages with continuation, stale policy, permission withdrawal, disable/re-enable, disconnect, cancellation, request concurrency/rate caps and data-free recursion refusal. Build only `fileshare_service_test` and run CTest `^fileshare_service$`; no Tor or application executable is required. The full-catalog runner discovers it automatically. In the application, the identity FileShare dialog provides explicit remote enablement (off by default, persisted as `identity/<uuid>/fileShareEnabled`); reconnect contacts after enabling. GET remains a separate checklist increment; approved recursive delivery has its own focused suite below.

`file_browser` (60 s, serial/offscreen) exercises the shared C++ Local/Remote
model and production VFS/FileShare browse providers. Covers assigned-subtree
confinement, provider/identity/contact isolation, UTF8/ASCII_ESCAPED LIST/STAT
round trips through the real codec/server, denied listing, malformed replies,
connection changes, cancelled/late replies and view destruction, peer-send
failure, bounded page replacement (including an empty continuing page), and the
same populated QML browser dialog in both modes without QML warnings. Build only
`file_browser_test` and run CTest `^file_browser$`; it uses temporary storage and
settings, no Tor or application executable. The all-tests runner discovers it
automatically. This base suite checks one bounded page at a time; navigation has
the separate focused suite below. View modes, viewport accumulation and selection/
downloads belong to subsequent Stage 1 checklist items. Remote root is a
known logical route; current root authorization comes from LIST, with no synthetic
root key sent as a server-issued node reference.

`browser_navigation` (60 s, serial/offscreen) tests one-folder Local/Remote
navigation through the same browser model and QML: clickable parent/root
breadcrumbs, Back/Forward/Up/Refresh, route reauthorization, preserved ASCII
presentation (including long percent-escaped labels), known/loading/updating/
partial/complete/empty distinctions, empty continuing pages, late replies and
scope changes, and one in-flight operation with a single latest pending request.
History is bounded to 32 entries and alias navigation to 64 explicit steps; no
recursive browsing occurs. A maximum of eight previously validated pages may be
shown as historical OfflineCached metadata in the same provider binding, with
last-refresh time and current access explicitly unconfirmed. Denial and privacy/
scope changes purge that cache. The production offline permit retains a policy
validity snapshot rather than the old Peer and is withdrawn on contact, identity
or connection-preference changes. Build only `browser_navigation_test` and run
CTest `^browser_navigation$`; source, settings and logs are isolated, with no
Tor or application build required. `file_browser_fixture.h` shares the existing
production VFS/codec/service test fixture with the base browser suite. The
all-tests runner discovers both tests automatically.

`fileshare_recursive_service` (60 s, serial) covers explicit zero-data recursion refusal before VFS opening followed by usable direct LIST, EXPORT denial without staging, production immutable catalog v2 generation and validation, complete expected catalog membership with denied-child filtering and no private sibling/path disclosure, delayed generic-offer publication after the accepted control, policy withdrawal before offering, cancellation, disconnect during admission, ASCII logical-root authorization, and replacement admission while revoked artifact handles remain retained. Build only `fileshare_recursive_service_test` and run CTest `^fileshare_recursive_service$`; no application executable or Tor is needed.


`portable_catalog` (60 s, serial) builds only the new standalone VFS test and required libraries. It covers portable v2 UTF8/ASCII catalogs, opaque export-scoped IDs, privacy field filtering, whole-file SHA-256, strict encoding/format/record validation, duplicate and truncated records, cancellation, authoritative root LIST/EXPORT policy denial before staging and active export revocation. The full runner discovers it through CTest.

`remote_catalogs` covers explicit request/connection/offer correlation, bounded
private catalog reception, whole-artifact SHA-256 and negotiated presentation
validation before cache publication, revocation, cancellation and zero-data
refusal. It is a 60-second serial test with isolated temporary storage.

`catalog_transfer` (60 s, serial) uses an isolated production DsEngine and the
established controllable checkpoint Peer harness. It connects explicit recursive
FileShare admission and accepted-control correlation to immutable catalog sending,
then receives the real bytes through IncomingTransfer into the private namespace.
It checks unsolicited/cross-connection refusal, full SHA-256, whole-catalog validation
before cache publication, rejection/cleanup of a structurally invalid catalog despite
a successful SHA-256 byte transfer, and an empty user Downloads namespace. Build only
`catalog_transfer_test` and run CTest `^catalog_transfer$`; it needs no application
executable or Tor.

`fileshare_get` (60 s, serial) exercises the production GET service, prepared VFS
adapter and coroutine-owned checkpoint sender against an isolated read-only source.
Covers immutable hash/size and node fences, denied READ, confinement, escaped
host-neutral names, cancellation/disable/disconnect, bounded pending admission,
active revocation, prepared-reader resume and changed-source refusal, nonzero
checkpoint resume on a reauthenticated connection (including contact isolation),
and complete bytes/SHA-256 with a final mutually verified checkpoint. Also follows
returned UTF8/ASCII_ESCAPED LIST names and node references through STAT to GET,
checks denied LIST and READ without treating visible metadata as authority,
and refuses the selected source after mutation. Hostile LIST/GET paths and encoding
fail before storage access; raw oversized controls and truncated UTF-8 fail validation.
Build only
`fileshare_get_test` and run CTest `^fileshare_get$`; no app executable or Tor needed.

`browser_views` (60 s, serial/offscreen) tests the shared Thumbnail, Small icons
and Details presentations over real local VFS and remote codec/service providers.
It checks persisted mode and column visibility/order/width, mandatory Name,
invalid preference/command bounds, neutral metadata and denial in every mode.
QML controls switch the same virtualized model without content/preview requests;
type icons are the safe fallback because browse providers expose no approved
thumbnail handle. No original files are fetched or decoded for display.
Build only new target `browser_views_test`, then run CTest `^browser_views$`.
Settings and source data use temporary directories; no application build or Tor.
The full runner discovers the test through CTest.

`browser_paging` (60 s, serial/offscreen) exercises incremental LIST accumulation
with grouped Qt inserts and persistent indexes, viewport-driven bounded prefetch,
empty continuation backpressure, duplicate IDs/cursor cycles and metadata admission.
The synthetic 100,000-entry fixture checks stable ordering, an event-loop heartbeat,
no per-page reset/offscreen dataChanged, and fewer than 200 live QML delegates while
scrolling all three modes. Production local VFS subscriptions and slow remote
codec/service pages cover file addition/removal, coalesced refresh and withdrawal.
Only value records enter the C++ index (120,000 rows / conservative 96 MiB metadata,
4,096 page ceiling); historical remote previews retain eight first pages of at most
100 rows. No remote original/thumbnail content is fetched. Build only new target
`browser_paging_test`, then run CTest `^browser_paging$`; no app build or Tor needed.

`browser_selection` (60 s, serial/offscreen) covers stable-ID single/toggle/range
selection across appended pages, bounded Select All (128 loaded items), explicit
batch admission (16 files), offscreen signal bounds, immutable context snapshots,
keyboard/mouse/long-press QML actions, folder-only/mixed selections and scope
cancellation. Production remote codec/service GET checks include whole-source
SHA-256, READ denial independent of LIST, per-file partial batch results and
receive-policy withdrawal. An isolated core profile tests explicit generic-offer
consent (32 pending, 30-second leases), both offer/result orderings, connection,
identity, content and MIME checks, duplicate consent, unrelated offers and shutdown.
Local/offline cached selections never enable download; cached folder navigation
still uses the same bounded selection model.
GET remains the remote READ authority: LIST/STAT do not advertise READ flags; the
UI offers Request download and reports individual refusals. No new wire capability,
transfer execution engine, transfer queue or MCP API is introduced. Build only new
`browser_selection_test` and run CTest `^browser_selection$`; no application build,
real profile or Tor is needed. The fixture initializes Crypto before VFS, matching
the application startup order.

`browser_smoke` (60 s, serial/offscreen) combines the production Local/Remote
providers, real FileShare codec/service and shared QML view. It checks slow real
LIST pages with grouped inserts, stable selection/persistent indexes, bounded
viewport repaint ranges, and a late page fenced by a switch back to Local.
The QML journey uses parent/root breadcrumbs, Ctrl-click multi-selection and the
toolbar Request download (2) action, checks two GET stages/admissions and source
SHA-256, server READ denial despite LIST visibility, local/receive-policy disabled
actions and denied listing cleanup. It requires zero QML warnings. Generic payload
execution, Transfers UI and private-Tor/two-instance acceptance remain separate
worklist gates. Build only new target `browser_smoke_test` and run CTest
`^browser_smoke$`; temporary storage/settings keep real profiles isolated.

`transfer_work` (60 s, serial) covers the versioned private work registry,
idempotency and immutable content bindings, bounded pages/counts, durable ordering
and user pause/acceptance, shared per-contact/global admission, cancelled queue
cleanup, real application restart and authoritative checkpoint pause/failure
reconciliation. It verifies staged sources remain ineligible until released,
missing transient sources require attention, and a production outgoing session
sends checkpointed payload through the same manager with matching SHA-256 and
one completed work record after restart. The counterpart is an in-process test
peer, without Tor or the application executable. Build only new target
`transfer_work_test` and run CTest `^transfer_work$`. The version-one SQLite row
fixture is `fixtures/transfer-work-v1.sql`; `transfer_sender_fixture.h` is shared
with the FileShare GET suite. Existing attempt-level fixtures explicitly enroll
accepted work at the new boundary; those older targets require owner-run
regression checks under the current new-test-only build policy. CTest discovery
keeps the full-catalog runner current without a target/name allowlist.

`transfer_scheduling` (60 s, serial/offscreen) covers trusted Conversation / Utility /
Bulk admission, durable High / Normal / Low priority, scoped reorder without moving
hidden rows, finite bulk waiting under conversation load, identity/contact fairness,
global/contact caps, queued pause/resume fences, cancellation and shutdown during
paged dispatch. It also checks the current-version priority fixture and a production
sender's checkpointed payload and full SHA-256 using an in-process counterpart.
Active user pause remains unavailable until the separate checkpoint-safe pause gate;
this suite does not assert active pause or preemption support.

`transport_priority` (30 s, serial/offscreen) uses localhost TCP only. It checks the
combined application/Qt write-buffer cap, complete-drain notification, and pending
chat taking the next turn before payload. Already serialized records retain stream
order. Build only new targets `transfer_scheduling_test transport_priority_test` and
run CTest `^(transfer_scheduling|transport_priority)$` for focused verification;
no application executable, real profile, or Tor network is required.

`transfer_queue_model` (60 s, serial/offscreen) covers application/identity/contact
scope isolation, both directions and scoped counts, filters, common manager
commands, restricted reorder/restart, stale scope/async-read destruction,
bounded paging, separate provisional/journal-verified bytes and a production
sender's SHA-256. Populated production Transfers QML checks priority/direction
controls, active-pause unavailability and text scaling without QML warnings.
Build only new target `transfer_queue_model_test`, then CTest
`^transfer_queue_model$`. No application executable or public Tor is needed.

`transfer_preemption` (90 s, serial/offscreen) exercises the versioned cooperative
pause exchange, sender-selected checkpoint fencing, receiver settlement, resumed
SHA-256 and reservation ownership, and capacity-triggered conversation priority
with a minimum work quantum and cooldown. Build only new target
`transfer_preemption_test`, then run CTest `^transfer_preemption$`. It uses isolated
in-process peers and does not build the application or access Tor. Active user
pause remains unavailable; automatic priority yielding has its own tested contract.

`queue_integration` (60 s, serial/offscreen) checks actual conversation-card
controls against the same manager used by scoped Transfers models, durable
cancellation before cleanup, persisted order and shared command results. Build
only new target `queue_integration_test`, then run CTest `^queue_integration$`.
The focused queue gate also uses existing `transfer_scheduling`,
`transfer_queue_model` and `transfer_preemption` suites for admission,
checkpoint/resume integrity and reservations; the new integration suite also
checks browser GET consent against the shared queue.
No application executable or public Tor is needed.
