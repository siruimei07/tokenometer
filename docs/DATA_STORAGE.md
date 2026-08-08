# Local usage storage

Tokenometer 0.1 is a local-only Windows application. It uses the SQLite engine shipped with Windows (`winsqlite3.dll`); there is no bundled database server, background service, telemetry endpoint, or cross-device cloud sync.

## Location and removal

The database and all application preferences live in the operating-system user-data directory:

```text
%LOCALAPPDATA%\Tokenometer\tokenometer.db
```

SQLite can create `tokenometer.db-wal` and `tokenometer.db-shm` beside the database while the application is running. To remove all Tokenometer state, exit from the tray first and delete `%LOCALAPPDATA%\Tokenometer`. Removing this directory does not remove source Codex transcripts or ChatGPT export files.

The database is ordinary, unencrypted SQLite. On startup Tokenometer replaces inherited permissions on the data directory and the database/WAL/SHM files with a protected DACL limited to the current user, LocalSystem, and local Administrators; a reparse-point data directory or database is rejected. This prevents unrelated inherited local groups from reading usage metadata, but an administrator, malware already running as the same user, or offline access to an unencrypted disk can still read it. Use Windows device encryption or BitLocker when at-rest disclosure is part of the threat model.

SQLite runs with foreign keys, WAL mode, a bounded journal, short write transactions, passive checkpoints, and incremental vacuuming. A full automatic `VACUUM` is deliberately avoided.

## Source matrix

| Source | Collection | Measurement | Explicitly unavailable |
|---|---|---|---|
| Windows Codex JSONL | Incremental local scan, approximately every 2 seconds | Provider-reported transcript counters | Billing cost and login switching |
| Codex JSONL in running WSL distributions | Automatic discovery and bounded merge, approximately every 5 minutes | Provider-reported transcript counters, attributed to that WSL device | Stopped distributions, remote quota, SQLite/WAL tools |
| ChatGPT official conversation export | User-selected import | Visible-branch user/assistant text estimate | Real-time data, exact tokens, cache, quota, cost, device, and tool calls |

Provider-reported Codex counters are exact with respect to the local transcript, not a billing invoice. No v0.1 source provides a reliable subscription cost, so Tokenometer stores and displays no invented currency value.

Each discovered Windows or WSL device stores its last attempt, last successful merge, and one of `never`, `synced`, `partial`, or `failed`. A malformed, oversized, unreadable, or permission-blocked source is reported without discarding previously collected data. WSL outcomes are recorded per running distribution; one failing distribution does not overwrite the status of another.

## Codex allowlist

The Windows collector reads only these files:

```text
%CODEX_HOME%\sessions\**\rollout-*.jsonl
%CODEX_HOME%\archived_sessions\rollout-*.jsonl
%CODEX_HOME%\session_index.jsonl
```

When `CODEX_HOME` is not set, `%USERPROFILE%\.codex` is used. Tokenometer does **not** read or write `auth.json`, browser profiles, cookies, credential stores, the Codex internal SQLite database, or prompt history in global state files. Codex account values in v0.1 are grouping metadata (`current` or an unknown WSL label), not login controls.

The database stores normalized token counters, model/tool/session/project/device metadata, quota snapshots found in the transcript, processed-record keys, and byte locators for on-demand detail. This includes transcript absolute paths, project working directories, and session titles from `session_index.jsonl`, which can themselves be sensitive metadata. It does not mirror prompt text, model responses, tool arguments, or tool output. When the user expands a tool call, Tokenometer reads only the referenced range from the original transcript and renders a bounded, best-effort-redacted preview; that content is not written back to the database. Redaction covers common credential keys, authorization/cookie headers, CLI secret flags, and known provider token shapes, but it cannot prove that arbitrary tool text contains no secret. Treat expanded details as sensitive during screen sharing or screenshots. Session prompt rows show stored token/tool aggregates rather than re-reading prompt bodies.

### Counter correctness

- For a monotonic cumulative sequence, the collector computes the delta between consecutive `total_token_usage` counters. It accepts `last_token_usage` only when every reported field agrees with that delta; a zero or inconsistent claim is replaced by the cumulative difference.
- If cumulative counters decrease or restart, the next valid `last_token_usage` begins the new segment; if it is absent, the new cumulative value is treated as the segment delta.
- Forked/sub-agent files ignore replayed parent history until the child's own `task_started` boundary.
- Active files moved into `archived_sessions` retain their logical session identity and are not counted again.
- A compact permanent processed-record ledger prevents duplication after detailed event rows expire.
- An incomplete JSONL tail never advances the source cursor. A complete final JSON record is accepted even without a trailing newline.

## WSL boundary

About every five minutes, Tokenometer asks the system `wsl.exe` for currently running distributions and selects only those results. It does not intentionally select a stopped distribution. There is no atomic WSL “execute only if still running” operation, however, so a distribution that stops between the last running-state check and `wsl.exe --distribution ... --exec` can be restarted by Windows. Inside each selected distribution, the allowlist is:

```text
${CODEX_HOME:-$HOME/.codex}/sessions/**/rollout-*.jsonl
${CODEX_HOME:-$HOME/.codex}/archived_sessions/**/rollout-*.jsonl
```

Only changed byte ranges are streamed through the Codex parser; transcripts are not copied into `%LOCALAPPDATA%`. Commands use bounded output, cancellation, timeouts, and a dedicated Linux process group. The collector requires the exact Bash/findutils/coreutils/`sed`/util-linux capabilities it probes; a distribution that cannot satisfy them is reported as unavailable rather than partially collected.

The newest active page is scanned first. Older active and archived pages use bounded round-robin cursors persisted in `app_state`, preventing a large archive from starving older files or unboundedly growing the Windows process. WSL distributions receive stable local device IDs. Remote quota snapshots are ignored because Tokenometer cannot prove that a WSL transcript belongs to the current Windows Codex account.

Live SQLite/WAL files must not be read through the Windows WSL filesystem bridge. OpenCode, Hermes, and similar sources would require a read-only headless agent running inside WSL and a duplicate-source guard, following the architecture described in [WSL SQLite setup](https://github.com/Javis603/token-monitor/blob/main/docs/wsl-sqlite-setup.md). That agent is **not implemented in v0.1**; the capability remains experimental and these tools are not reported from WSL.

## ChatGPT import boundary

Consumer ChatGPT exposes no supported real-time local token feed. Tokenometer therefore accepts only files the user explicitly selects from an official ChatGPT data export (`conversations.json` or numbered conversation JSON files). It never scrapes a ChatGPT browser profile, cookie, session, or credential.

For each conversation, the importer follows `current_node` back through its parents and processes only the current visible branch. Visually hidden messages, alternate branches, system records, attachments, and non-user/non-assistant messages are excluded. Each visible message estimate is:

```text
ceil((UTF-8 bytes of visible text + UTF-8 bytes of role + 4 framing bytes) / 4)
```

User text is labelled estimated input and subsequent visible assistant text is labelled estimated output. This is a deterministic heuristic, not an OpenAI tokenizer, so it must not be combined with Codex exact counters without an “estimated” marker. Cache hits/misses, quotas, costs, devices, tool calls, and precise model billing data are unavailable from the export.

The database persists only the selected file's basename, SHA-256, size, modified time, the user-supplied account label, stable session/turn identifiers, available export model labels (which may be missing and are not billing-model proof), timestamps, message counts, and estimated totals. Absolute export paths and message bodies are discarded. Re-importing an unchanged source is a no-op; a changed source replaces matching stable session IDs rather than accumulating duplicates.

Hashing and parsing use one read-only file handle that denies concurrent writes and replacement. The importer rejects files larger than 256 MiB, individual conversation objects larger than 32 MiB, more than 25,000 conversations, more than 100,000 visible prompts, or a current branch deeper than 25,000 nodes. These limits bound memory use and treat oversized or malformed exports as unavailable rather than partially importing them.

Account labels are local grouping text only. They do not switch, add, remove, inspect, or authenticate a Codex or ChatGPT account. OpenAI API organization usage is a separate product surface and is not collected in v0.1.

## Retention and growth

| Data | Default retention | Purpose |
|---|---:|---|
| Token, prompt, and tool locator events | 180 days | Recent drill-down |
| Hourly Codex usage buckets | 400 days | One-year bars and K-line views |
| Prompt/tool event detail and locators | 180 days | Recent prompt token splits and on-demand tool detail |
| Session metadata and daily aggregates | Permanent | Historical session and trend summaries |
| Daily usage buckets | Permanent | Heatmap, streaks, and long-term totals |
| Processed-record keys and source cursors | Permanent | Idempotent rescans and archive moves |
| ChatGPT export estimates | Permanent until replaced | Account/session/prompt/day estimates derived from an import |
| Surface preferences | Until application data is deleted | Tray, bubble, appearance, layout, tool order, and bubble position |

Permanent usage tables are compact numerical aggregates. Source JSONL and ChatGPT message bodies are not mirrored, so storage grows with normalized events rather than full transcripts.

## Floating backdrop boundary

The floating bubble's blur effect is disabled by default. If the user enables it, Tokenometer uses Windows Graphics Capture only while the bubble is visible to acquire the current monitor without the cursor, crop the pixels around the bubble, and render the refracted backdrop in GPU memory. Frames are not written to disk, inserted into SQLite, or sent over a network. Hiding the bubble, disabling blur, switching back to the dashboard, or using `--no-backdrop` stops this capture path.

## Time and device semantics

- Heatmap days use the collecting Windows device's local date; raw event timestamps remain Unix UTC seconds.
- WSL usage is attributed to a stable per-distribution local device ID and a last-seen timestamp.
- “Synced” means merged into this local database. It does not imply remote or cloud synchronization.
- v0.1 does not transfer data between Windows computers.
