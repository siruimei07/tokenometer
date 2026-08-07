# Roadmap

## Current milestone — complete runnable UI

- [x] Select native Windows stack and define provider boundary.
- [x] Establish Liquid Glass design system and threat model.
- [x] Build dashboard with honest demo/source states.
- [x] Build topmost desk capsule and tray lifecycle.
- [x] Verify Release build, deterministic screenshots, interaction paths, and the current security posture.
- [ ] Complete high-contrast, reduced-transparency, and screen-reader verification before packaging.

## Next milestone — Codex local usage

- Validate stable local event fields without reading message content.
- Implement incremental, bounded parser behind `IUsageSource`.
- Add parser self-checks using sanitized fixtures.
- Compare totals against the Codex app's own usage display.

## Later, only if supported

- Account allowance/credit provider through an official interface.
- Signed packaging and auto-start setting.
- User-selectable resource-only skins.
- Signed installer, application icon, and startup preference.

## Explicit non-goals for the UI milestone

- No web scraping or browser-cookie reuse.
- No invented account quota.
- No database, cloud backend, telemetry, or updater.
- No third-party chart/UI framework.
