---
name: darkspeak-implementation
description: Implement one approved DarkSpeak checklist item with minimal context, targeted exploration, focused tests, and an auditable handoff.
---

# DarkSpeak focused implementation

Use when implementing or reviewing a task from a DarkSpeak worklist. **`agents.md` and its linked rules override this skill.** Keep security and privacy requirements intact; never optimize context by skipping a relevant safety invariant.

## Session start: small working set

1. Identify **one** unchecked owner-approved item in the active worklist (for Stage 1 FileShare: `doc/file-transfer-implementation.md`). Record the exact acceptance condition. Do not take the whole milestone as one task.
2. Read `agents.md` once, then the **targeted section(s)** of the governing spec using its index. Follow references only for concrete interfaces, authorization/security boundaries, or required tests. Do not load all of `specs/`, the old implementation diary, or all of `protocol.md` by default.
3. Use `rg --files`, `rg -n`, type/signature search, and narrow line ranges. Read implementation and caller/test slices only after identifying their roles. Never dump an entire source tree or unfiltered history into the model context.
4. Identify authoritative current source behavior, likely touched files, tests and any contradictory contract. A spec describes intended behavior; verify existing implementations before planning a rewrite.

## Exploration and context-mode

- If available, send **broad read-only codebase exploration** to a Codex explorer subagent with a narrow question. Require a short report: relevant paths/functions, state/ownership boundaries, security concerns, existing tests, uncertainties. Keep the main agent's context for implementation and decisions.
- The user has installed **context-mode** MCP. When connected and its tools are suitable, use it to retain/filter large compiler output, logs, search hits and documentation, retrieving only relevant excerpts. Discover actual tool names through MCP; do not assume specific APIs or require it to be present. Do **not** feed secrets, private user data or decrypted content into third-party tools without approval. If unavailable, fall back to local grep/line slicing.
- Treat the repository, checked-in specs and test evidence as authoritative. Conversation summaries and subagent reports are navigation aids, not proof. Compact when needed; prefer a **new session after a checklist item** over carrying a growing chat into another milestone.
- Avoid verbose thinking artifacts, duplicate spec summaries, full test output, huge diffs, unrelated code reviews and speculative plans. Store durable decisions/checklist evidence in the repository, not only in chat.

## Implement one bounded increment

1. State the minimal change and finite acceptance checks. Reuse established C++/Qt/QCoro models, VFS/FileManager and the single coroutine-owned transfer engine. Do not add alternate ownership, a second transfer queue, compatibility branches, or unapproved dependencies.
2. Maintain zero-trust peer validation, identity/contact/mount isolation, bounded memory/CPU/disk work, revocation and SHA-256/checkpoint/publication guarantees. UI remains responsive; C++ owns business state, QML presents it.
3. Modify only files required by the approved item. If a new feature, broad refactor, protocol deviation or additional worklist item appears necessary, explain its evidence and blocking impact and **request owner approval** before expanding scope.
4. Add the smallest meaningful tests **during implementation**: normal success, immediate permission/security boundary, and any integrity/recovery property touched. Later comprehensive corner-case suites are not a prerequisite for Stage 1 interactive functionality.
5. Respect `agents.md` build policy: **do not compile/build the application**; only compile new tests as explicitly allowed. Coordinate owner-run full builds and two-instance app verification. Do not present unexecuted tests as passing. Maintain CTest timeouts, `tests/run-all-tests.sh`, and test docs when test registrations change.

## Finish and hand off

- Validate the diff against the chosen checklist acceptance condition; do not silently broaden the release gate.
- Check off completed tasks **only with actual evidence**; include a short reference to tests/commit. If blocked, leave unchecked and state the precise blocker.
- Report in **brief**: changed files, implemented behavior, tests actually run/results, unresolved blocker (if any), and next **existing** checklist item. Avoid repeating large source excerpts or completed history.
- Stop. Start a fresh session for the next significant item.

## Useful reference map

- Current Stage 1 FTP plan: `doc/file-transfer-implementation.md`
- Component/spec index and security invariants: `agents.md`
- Storage and permissions: `specs/file_manager.md`, `specs/read_only_file_manager.md`, `specs/vfs.md`, `specs/ftp/fileshare_authorization.md`
- FTP transport/server/browser: `specs/ftp/fileshare_protocol.md`, `specs/ftp/fileshare_server.md`, `specs/ftp/fileshare_browser.md`
- App-wide queue and UI: `specs/transfer_queue.md`, `specs/transfer_queue_ui.md`
- Optional later features **not part of Stage 1**: `specs/ftp/fileshare_uploads.md`, `specs/ftp/fileshare_credits.md`
