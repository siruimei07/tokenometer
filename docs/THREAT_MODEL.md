# Threat Model

Application: Tokenometer  
Method: STRIDE  
Stage: UI architecture / pre-collector  
Data classification: local Codex usage metadata is confidential; session text and credentials are restricted and out of scope.

## Context and assumptions

| Item | Status |
|---|---|
| Purpose and users | Obtained — single-user Windows desktop monitor |
| Components | Obtained — WPF UI, tray, read-only provider boundary, local preference store |
| Data flows | Partially obtained — local event format and supported remote sources remain under validation |
| Authentication | Missing for future account source; no account login in UI milestone |
| Compliance | No regulated-data requirement stated |
| Deployment | Obtained — local Windows process running as the signed-in user |

## Assets and trust boundaries

- **A-1:** Codex session metadata and token counts.
- **A-2:** future account allowance/credit information.
- **A-3:** local preference and source configuration.
- **A-4:** integrity of values displayed to the user.

- **TB-1:** Codex-owned files to the Tokenometer parser.
- **TB-2:** optional OpenAI service to the future account provider.
- **TB-3:** source provider to normalized in-memory snapshot.
- **TB-4:** user-controlled skin/resource path to the visual layer.

## Data flow

```text
[Codex files, current user]
        -- local read / confidential / no auth --> [bounded parser]
                                                     |
                                                     v
                                             [normalized snapshot]
                                                     |
                         +---------------------------+------------------+
                         v                                              v
                  [dashboard UI]                                [desk capsule]

[optional official API]
        -- HTTPS / explicit auth / confidential --> [account provider]
```

The local flow fails closed: unreadable or malformed data becomes unavailable, never a fabricated value. The future network flow must use a supported API and OS-protected credential storage.

## Component / STRIDE matrix

| Component | S | T | R | I | D | E | Overall |
|---|---|---|---|---|---|---|---|
| Local event parser | L | H | L | H | M | L | High |
| Future account provider | H | M | M | H | M | M | High |
| Normalized snapshot | L | M | L | M | L | L | Medium |
| UI / clipboard / export | L | L | L | M | L | L | Medium |
| Optional skin loader | L | M | L | L | M | M | Medium |
| Dependency/build chain | M | H | M | H | M | H | High |

## Threat register

| ID | STRIDE | Description | Component | ATT&CK | Likelihood | Impact | Severity | Mitigation | Owner | Status |
|---|---|---|---|---|---|---|---|---|---|---|
| TM-001 | Information disclosure | A broad file scan or diagnostics capture session text, credentials, or tokens | Local parser | T1552.001 | Medium | High | High | Allowlisted paths/events/fields; never log record bodies; redact diagnostics | App | Required before collector |
| TM-002 | Tampering | Malformed or attacker-written JSONL produces false values or parser failure | Local parser | T1565 | Medium | Medium | Medium | Bounded streaming parse, checked arithmetic, schema guards, source/confidence badge | App | Required before collector |
| TM-003 | Spoofing | An unsupported login flow or copied browser cookie impersonates the user | Account provider | T1528 | Medium | High | High | Only official OAuth/API flows; never read browser cookies; OS credential vault | App | Blocked by design |
| TM-004 | Information disclosure | Access or refresh material is committed, logged, or stored in plaintext | Account provider | T1552 | Medium | High | High | No secrets in repo/config; Windows Credential Manager/DPAPI; redaction tests | App | Required before provider |
| TM-005 | Denial of service | Large session history causes UI hangs or excessive memory use | Parser/UI | T1499.003 | High | Medium | High | Incremental tailing, file/record caps, cancellation, background parsing | App | Required before collector |
| TM-006 | Tampering | Stale data is presented as live and changes user decisions | Snapshot/UI | T1565 | Medium | Medium | Medium | Last-updated timestamp, stale threshold, atomic snapshot replacement | App | In design |
| TM-007 | Elevation of privilege | App requests admin rights or follows privileged links unnecessarily | Process | T1548 | Low | High | Medium | `asInvoker`, no service/driver, least-privilege file access | App | Implemented in manifest |
| TM-008 | Tampering / EoP | A skin loads executable content or unsafe remote resources | Skin loader | T1059 | Medium | High | High | Resource-only allowlist; image decoding limits; no scripts/assemblies/XAML loading | App | Required before skins |
| TM-009 | Supply-chain tampering | Compromised package/build input gains current-user access | Build chain | T1195.001 | Medium | High | High | Prefer platform libraries, lock dependencies, scan releases, reproducible build | Maintainer | In design |
| TM-010 | Repudiation | Source transitions or parse failures cannot be reconstructed | Diagnostics | T1070 | Low | Medium | Low | Minimal local structured events without sensitive payloads; bounded retention | App | Future |

## Security requirements

- **SR-1:** Run as the current user and never require elevation.
- **SR-2:** Collectors are read-only and path/field allowlisted.
- **SR-3:** Never collect, persist, display, export, or log session message bodies.
- **SR-4:** Never reuse browser cookies or copy Codex authentication files.
- **SR-5:** Remote account data requires a documented supported interface and explicit user action.
- **SR-6:** Secrets, if later required, use an OS-protected store and are redacted from diagnostics.
- **SR-7:** Parsing is bounded, cancellable, overflow-safe, and off the UI thread.
- **SR-8:** Every value carries source, freshness, and availability semantics.
- **SR-9:** Skins are declarative resources only; no external XAML, scripts, or assemblies.
- **SR-10:** Release builds undergo code review and dependency audit before distribution.

