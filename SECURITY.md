# Security policy

## Supported version

Security fixes are currently provided for the latest `0.1.x` release only.
Tokenometer is an unsigned developer preview until a release explicitly says
otherwise.

## Reporting a vulnerability

Please report vulnerabilities privately through the repository's GitHub
Security Advisory form. Do not put Codex transcripts, ChatGPT exports,
`tokenometer.db`, account labels, authentication files, cookies, tokens, or
tool input/output in a public issue. A minimal synthetic reproduction is
preferred.

Include the affected Tokenometer version, Windows version, reproduction steps,
and expected impact. We will acknowledge a report when it is received and
coordinate disclosure after a fix is available.

## Local data boundary

Tokenometer has no telemetry or usage-upload client. It reads local Codex
transcripts and user-selected ChatGPT exports, then stores usage metadata in
`%LOCALAPPDATA%\Tokenometer\tokenometer.db`. The database is not encrypted.
At startup the application protects the data directory and SQLite files with a
DACL limited to the current user, LocalSystem, and local Administrators, and it
rejects a reparse-point storage path. Same-user malware, administrators, and
offline disk access remain outside that boundary.
Tool input/output is read only on demand and redaction is best effort, so an
expanded detail view must still be treated as sensitive during screen sharing
or screenshots. See [docs/DATA_STORAGE.md](docs/DATA_STORAGE.md) for the full
data model and retention boundary.

The optional floating-bubble blur uses Windows Graphics Capture while the
bubble is visible. Frames remain in local memory/GPU processing and are not
saved or uploaded. Blur is off by default and `--no-backdrop` disables this
path.

## Release integrity

Official archives are paired with a SHA-256 checksum. Verify the checksum
before running the application, extract each update into a new empty directory,
and do not run Tokenometer as administrator. An unsigned build may show an
unknown-publisher warning; download only from the repository's release page and
do not use third-party instructions that disable Windows security controls.

Tokenometer has no automatic updater. Exit the running tray process before
replacing or removing an installation.
