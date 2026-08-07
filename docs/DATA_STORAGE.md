# Local usage storage

Tokenometer uses the SQLite engine shipped with Windows (`winsqlite3.dll`). The database lives at:

```text
%LOCALAPPDATA%\Tokenometer\tokenometer.db
```

This keeps the application single-process and avoids a bundled database runtime or background service. SQLite runs in WAL mode with foreign keys, a bounded journal, short write transactions, and passive maintenance.

## What is collected

The Codex collector only reads these allowlisted files:

```text
%CODEX_HOME%\sessions\**\rollout-*.jsonl
%CODEX_HOME%\archived_sessions\rollout-*.jsonl
%CODEX_HOME%\session_index.jsonl
```

When `CODEX_HOME` is not set, `%USERPROFILE%\.codex` is used. Tokenometer does not read `auth.json`, browser profiles, cookies, the Codex internal SQLite database, or prompt history in global state files.

On Windows, Tokenometer also discovers only currently running WSL distributions about every five minutes. It invokes the system `wsl.exe` directly with bounded output, cancellation, and timeouts; no `cmd.exe`, PowerShell, UNC traversal, or login shell is used. Inside each running distribution it allowlists only:

```text
${CODEX_HOME:-$HOME/.codex}/sessions/**/rollout-*.jsonl
${CODEX_HOME:-$HOME/.codex}/archived_sessions/**/rollout-*.jsonl
```

Only changed byte ranges are streamed into the same Codex parser. WSL transcripts are never copied into the application data directory. Active SQLite/WAL files are deliberately excluded; tools such as OpenCode or Hermes require a future read-only agent running inside WSL and must not be reported by both that agent and the Windows scanner.

Metadata enumeration requires Bash, GNU findutils/coreutils/`sed`, and util-linux `setsid`. Tokenometer checks the exact `find`, `sort`, `sed`, `dd`, `stat`, `readlink`, `timeout`, and `setsid` capabilities it uses, and reports that distribution as unavailable instead of accepting a partial pipeline result. Each remote command runs in a unique Linux process group with a bounded `timeout`; Windows cancellation rechecks that the distribution is still running, then sends TERM and KILL to that group without terminating the distribution. The newest active page is scanned first on every pass; older active and archived pages use bounded round-robin cursors persisted in `app_state`, so a restart or a large long-lived archive cannot starve older files or grow the Windows process without limit.

The database stores normalized token counters, model/tool/session metadata, quota snapshots, and byte offsets for on-demand tool detail. It never copies prompt text, model responses, tool arguments, tool output, or credentials.

## Retention and growth

| Data | Default retention | Purpose |
|---|---:|---|
| Token, prompt, and tool locator events | 180 days | Recent drill-down |
| Hourly usage buckets | 400 days | One-year bars and K-line views |
| Per-turn token totals and tool names | Permanent | Historical session summaries |
| Daily usage buckets | Permanent | Heatmap, streaks, and long-term totals |
| Processed-record keys and source cursors | Permanent | Idempotent rescans and archive moves |
| ChatGPT export estimates | Permanent until replaced | Account/session/prompt/day estimates derived from official exports |

Permanent tables are compact numerical aggregates. Source JSONL is never mirrored, so storage grows with normalized events rather than with the much larger transcripts. `PRAGMA optimize`, passive WAL checkpoints, and incremental vacuuming are run periodically; automatic full `VACUUM` is intentionally avoided.

## Correctness rules

- Token usage prefers Codex's per-request `last_token_usage` and validates it against cumulative counters.
- Forked/sub-agent files ignore replayed parent history until the child's own `task_started` boundary.
- Active files moved into `archived_sessions` keep the same logical session and do not increment totals again.
- A compact permanent processed-record ledger prevents double counting even after detailed event rows expire.
- Incomplete JSONL tails never advance the source cursor; a complete final JSON record is accepted even before its trailing newline arrives.
- WSL distributions receive stable per-distribution device IDs, and moving a transcript from active to archived storage preserves the logical file identity.
- WSL quota snapshots are ignored so an unknown remote account cannot overwrite the current Windows Codex account's limit state.
- Heatmap days are assigned using the collecting device's local date. Raw timestamps remain Unix UTC seconds.

## ChatGPT boundary

Consumer ChatGPT does not expose a supported real-time local token feed. Tokenometer must not scrape its Chromium profile or copy authentication state. ChatGPT support is therefore limited to user-initiated official data-export imports (clearly labelled as estimated where tokenization is reconstructed). OpenAI API organization usage is a separate optional source and does not represent ChatGPT Plus or Pro subscription usage.

ChatGPT imports are stored in dedicated `estimated` tables so they can never be added to exact Codex counters by accident. Only the selected file's basename, SHA-256, size, modified time, account label, session identifiers, model labels, timestamps, message counts, and estimated token totals are persisted. Absolute export paths and message bodies are discarded. Re-importing the same file is a no-op; a changed export replaces matching stable session IDs instead of accumulating duplicates.
