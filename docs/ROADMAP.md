# Roadmap

## Current milestone — complete runnable UI

- [x] Select native Windows stack and define provider boundary.
- [x] Establish Liquid Glass design system and threat model.
- [ ] Build dashboard with honest demo/source states.
- [ ] Build topmost desk capsule and tray lifecycle.
- [ ] Verify build, interaction, screenshots, accessibility basics, and security posture.

## Next milestone — Codex local usage

- Validate stable local event fields without reading message content.
- Implement incremental, bounded parser behind `IUsageSource`.
- Add parser self-checks using sanitized fixtures.
- Compare totals against the Codex app's own usage display.

## Later, only if supported

- Account allowance/credit provider through an official interface.
- Signed packaging and auto-start setting.
- User-selectable resource-only skins.

## Explicit non-goals for the UI milestone

- No web scraping or browser-cookie reuse.
- No invented account quota.
- No database, cloud backend, telemetry, or updater.
- No third-party chart/UI framework.

