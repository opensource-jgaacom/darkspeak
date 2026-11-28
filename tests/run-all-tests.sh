#!/usr/bin/env bash
# Keep this entry point current whenever tests or their prerequisites change.
set -euo pipefail
umask 077
ulimit -c 0

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
project_dir="$(dirname -- "$script_dir")"
test_build_dir="/tmp/darkspeak-all-tests-${UID}"
test_python="python3"
build_jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-3}"
dry_run=false
runtime_dir=""

usage() {
    cat <<'EOF'
Usage: tests/run-all-tests.sh [options] [-- extra CMake arguments]

Configure, build and run all registered unit, functional and acceptance tests.
Requires Linux, Python 3.12+, Tor/tor-gencert and the project's C++/Qt dependencies.
Creates a private Python virtualenv and installs tests/mcp/requirements.txt.
Builds the application too: the QML smoke tests execute it.

Options:
  --build-dir DIR  Build directory (default: /tmp/darkspeak-all-tests-$UID)
  --python PATH    Python interpreter used to create the virtualenv
  --jobs N         Parallel build jobs (default: CMAKE_BUILD_PARALLEL_LEVEL or 3)
  --dry-run        Print commands without creating files, installing or building
  -h, --help      Show this help

Tests run sequentially, with their CTest deadlines (180 seconds if unspecified).
CTest runs the entire catalog, including future registered acceptance scenarios.
Any dependency, configure, build or test failure produces a nonzero exit status.
EOF
}

fail() { printf 'run-all-tests: %s\n' "$*" >&2; exit 2; }
while (($#)); do
    case "$1" in
        --build-dir|--python|--jobs)
            (($# >= 2)) && [[ -n "$2" ]] || fail "$1 requires a value"
            case "$1" in
                --build-dir) test_build_dir="$2" ;;
                --python) test_python="$2" ;;
                --jobs) build_jobs="$2" ;;
            esac
            shift 2
            ;;
        --dry-run) dry_run=true; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; break ;;
        *) fail "Unknown option: $1 (use --help)" ;;
    esac
done
[[ "$build_jobs" =~ ^[1-9][0-9]*$ ]] || fail "--jobs must be a positive integer"
[[ "$(uname -s)" == Linux ]] || fail "The isolated test runtime requires Linux"
for dependency in cmake ctest tor tor-gencert ffmpeg "$test_python"; do
    command -v "$dependency" >/dev/null || fail "Missing dependency: $dependency"
done
"$test_python" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 12) else 1)' \
    || fail "Python 3.12 or newer is required"
# Resolve relative build paths before changing directories for a stable invocation.
test_build_dir="$("$test_python" -c 'import os,sys; print(os.path.abspath(sys.argv[1]))' "$test_build_dir")"
venv_python="$test_build_dir/python-venv/bin/python"

run() {
    if "$dry_run"; then
        printf '  '
        printf '%q ' "$@"
        printf '\n'
    else
        "$@"
    fi
}
cleanup() {
    if ! "$dry_run" && [[ -n "$runtime_dir" && -d "$runtime_dir" ]]; then
        rm -rf -- "$runtime_dir"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

cd -- "$project_dir"
run mkdir -p -- "$test_build_dir"
if [[ ! -x "$venv_python" ]]; then
    run "$test_python" -m venv "$test_build_dir/python-venv"
fi
run "$venv_python" -m pip install --quiet --disable-pip-version-check --no-input \
    -r "$script_dir/mcp/requirements.txt"

# Extra toolchain arguments are allowed, but cannot switch off mandatory suites.
run cmake -S "$project_dir" -B "$test_build_dir" -DCMAKE_BUILD_TYPE=Debug "$@" \
    -DBUILD_TESTING=ON -DDSFM_BUILD_TESTS=ON -DDSFM_BUILD_EXAMPLE=OFF \
    -DDARKSPEAK_WITH_MCP=ON -DDARKSPEAK_WITH_TEST_MCP=ON \
    -DDS_ENABLE_TOR_TESTS=ON -DPython3_EXECUTABLE="$venv_python"
# The default build includes every test executable and the app used by QML smoke
# checks. Private-Tor privacy, multi-identity, resume, deletion, transfer-decision
# and corrupted-partial scenarios
# and database crash-recovery checks use this build and are discovered below.
# Media-preview codec, managed-cache, and private-Tor preview scenarios use Qt's existing JPEG plugin;
# no additional Qt module or system thumbnailer is needed for image previews.
# Video tests require FFmpeg and a Linux kernel with Landlock and seccomp.
# The startup dependency-dialog and persistent opt-out tests are also discovered.
# The existing filemanager CTest target also covers prepared reads and directory
# subscriptions, including cross-thread delivery and cancellation, plus shared
# status/hash/capability metadata and Linux filename encoding. No extra flags
# or dependencies are required.
# remote_catalogs covers request/connection isolation and validated private cache publication.
# portable_catalog covers portable v2 metadata, strict presentation and SHA-256 validation.
# The vfs CTest target includes assignment, overlay, permission and mount-scoped
# composition, catalog formats/exports, revocation and alias privacy. Its host
# Home-pane and ordinary-transfer integration is also in managed_storage.
# recursive_cycles checks resolved target cycle/alias bounds and source identity changes.
# protocol_v3 also checks deferred socket destruction during local disconnects.
# fileshare_protocol covers bounded v1 controls, strict path decoding, negotiation,
# authenticated output, correlation, cancellation and request admission/expiry.
# fileshare_service covers the production VFS-backed remote LIST/STAT controller,
# encrypted Peer dispatch, cursor isolation, empty pages, revocation and bounded admission.
# fileshare_recursive_service checks recursion refusal/direct fallback, complete filtered catalogs and publication fences.
# fileshare_get checks LIST/STAT-selected GET, hostile inputs, revocation/resume and generic checkpointed bytes.
# file_browser checks the shared Local/Remote model/providers and QML with isolated settings/storage.
# browser_navigation checks breadcrumbs/history, listing states, stale replies and scoped offline metadata.
# transfer_scheduling and transport_priority cover trusted/fair admission, durable local priority,
# scoped commands, cleanup and bounded chat-before-payload socket scheduling (no extra flags).
# transfer_queue_model covers shared scoped Transfers UI, journal progress and queue commands (no extra flags).
# transfer_preemption covers cooperative checkpoint fences, resume integrity and bounded priority yielding (no extra flags).
# queue_integration covers conversation card/shared scoped queue commands and durable intent (no extra flags).
# transfer_work covers the durable global registry, queue admission, checkpoint recovery and payload SHA-256.
# browser_smoke combines real providers, slow pages, QML batch GET/denial and location fences.
# browser_selection checks bounded stable selection, context actions, GET batches and scoped generic-offer consent.
# browser_paging checks viewport backpressure, bounded grouped pages/live invalidations and 100k virtualization.
# browser_views checks shared display modes, safe type icons, columns and isolated persisted preferences.
# catalog_transfer checks generic checkpointed catalog bytes and private validated-cache publication.
# transfer_checkpoints covers durable progress and accounting-independent backpressure.
# transfer_session covers bounded owner events, shared admission and joined cancellation.
# managed_storage also reclaims offer admission slots through reloaded File lifecycles.
# checkpoint_restart kills and restarts isolated authenticated loopback fixtures,
# including lost initial offers, acceptance races, partials, publication hints and unsupported peers.
# temporary_artifacts covers private publication, lease isolation and recovery.
# storage_pool covers shared capacity admission, reservation recovery and owner locks.
# readonly_filemanager covers external sources, exclusion/authorization alias
# share_administration covers the combined QML registration/assignment/mount/grant flow,
# full model/backend restart, confinement, visible errors and live reader withdrawal.
# share_permissions covers scoped rules, general membership, View as Contact and active revocation.
# share_mounts covers identity assignments, mount CRUD, confinement, persistence and QML.
# readonly_resources covers global registration/status/removal, default names,
# indexed file counts/bytes, persistent rename, quiet polling with changed-role
# notifications, queued actions during polls, and registry persistence,
# overlap/exclusion validation and the production QML storage dialog/model.
# storage_diagnostics checks TRACE failure sites, schema/limit validation,
# SQL preparation errors/native codes and redaction of catalog values.
# guards, snapshot leases, quotas, paging and replacement races. Bind-mount
# checks use unshare only when private user/mount namespaces are available.
# readonly_million_entry_paging has its own 1800-second deadline and reports a
# CTest skip unless DSFM_RUN_MILLION_ENTRY_TEST=1 and 6 GiB temporary space exist.
# managed_storage also exercises mandatory host exclusions and sender payload
# verification, including resumed prefixes and timestamp-preserving tampering.
# No explicit target allowlist can accidentally omit a future test binary.
run cmake --build "$test_build_dir" --parallel "$build_jobs"

if "$dry_run"; then
    runtime_dir="/tmp/darkspeak-test-run.DRY_RUN"
else
    runtime_dir="$(mktemp -d /tmp/darkspeak-test-run.XXXXXXXX)"
fi
run mkdir -p -- "$runtime_dir/home" "$runtime_dir/config" "$runtime_dir/data" \
    "$runtime_dir/cache" "$runtime_dir/runtime"
run env HOME="$runtime_dir/home" XDG_CONFIG_HOME="$runtime_dir/config" \
    XDG_DATA_HOME="$runtime_dir/data" XDG_CACHE_HOME="$runtime_dir/cache" \
    XDG_RUNTIME_DIR="$runtime_dir/runtime" DBUS_SESSION_BUS_ADDRESS= \
    QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
    DARKSPEAK_MCP_TEST_PYTHON="$venv_python" \
    ctest --test-dir "$test_build_dir" --parallel 1 --timeout 180 \
    --output-on-failure --no-tests=error
