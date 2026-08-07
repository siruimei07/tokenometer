# Architecture

## Scope

The UI milestone is a Windows 11 desktop client that remains useful when minimized. It presents demo data through the same read-only contract future Codex collectors will implement.

## Runtime shape

```text
Codex local logs / supported account source (future)
                    |
                    v
              IUsageSource
                    |
                    v
           DashboardViewModel
              /           \
             v             v
       Main dashboard   Desk capsule
             \             /
              Tray coordinator
```

- **WPF / .NET 10:** native windowing, drawing, accessibility, and packaging.
- **DWM interop:** rounded corners and transient Acrylic preserve the actual desktop color behind a neutral window.
- **Optical control:** `BackdropGlass` adds shared-backdrop blur, four-band perimeter refraction, asymmetric highlights, and pointer-driven specular energy without coloring the sampled background.
- **No third-party UI/chart packages:** the small chart set is drawn with WPF primitives.
- **Read-only providers:** collectors return normalized snapshots and never mutate Codex state.

## Data contract

`IUsageSource` will return a snapshot containing:

- source kind and confidence;
- last-updated time and staleness;
- current session model and token breakdown;
- short time-series samples;
- context-window use when reported by the source;
- optional allowance/credit value and reset time;
- recent session summaries without message content.

Unavailable data is nullable. UI components must render an unavailable state instead of inventing a number.

## Expected providers

1. `DemoUsageSource` — deterministic UI development data; visibly marked.
2. `CodexLocalUsageSource` — future read-only parser for documented/observed local event files.
3. `OpenAiAccountUsageSource` — only if an official supported API can provide the required account-level fields; no scraping or reuse of browser cookies.

## Persistence

Only UI preferences are persisted: window bounds, capsule position, selected skin, update interval, and privacy switches. Session message content and authentication material are never stored by Tokenometer.

## Failure behavior

- Provider failures preserve the last good snapshot, mark it stale, and show a local error.
- Malformed or oversized records are skipped with bounded diagnostics.
- Unsupported DWM effects fall back to an opaque high-contrast surface.
- Closing the dashboard hides to the tray; only the explicit Exit action terminates the process.

## Desktop lifecycle

`App` owns one `DashboardViewModel`, the dashboard window, the topmost widget, and the tray icon. Hiding the dashboard does not create a second view model or collector. Opening the dashboard hides the widget; the header and tray can show the widget explicitly. Explicit tray Exit disposes the icon and view model before closing both windows.

The `--snapshot` and `--widget-snapshot` switches are developer QA entry points. They render against a clean two-tone proof backdrop so no desktop or private window content is captured.
