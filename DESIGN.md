# Tokenometer Design System

## Direction

Tokenometer uses a neutral Apple-style Liquid Glass layer over the real Windows desktop. Glass never owns a hue: the desktop or window behind it supplies every visible color. The product does not bundle a wallpaper, texture, or game artwork.

The current product has two surfaces:

- **Dashboard:** a three-column monitoring field inspired by the supplied finance dashboard layout.
- **Desktop widget:** a borderless, draggable, always-on-top summary that appears when the dashboard is hidden.

## Main layout

The dashboard keeps one clear reading order:

1. Header: source identity on the left; widget, refresh, search, minimize, maximize, and close on the right.
2. Left column: allowance, current session, and quick actions.
3. Wide center column: session trend, 1/5-minute token pulse, and recent sessions.
4. Right column: a tall usage overview plus data-source status.
5. Floating bottom navigation: overview is active; reserved destinations keep working buttons and honest placeholder feedback.

At the 1280 × 820 design size the columns are 292 / fluid / 300 px. The minimum window is 1080 × 720; overflow scrolls instead of clipping data.

## Liquid Glass construction

`BackdropGlass` is an optical control, not a translucent card style. Its layers are:

1. Windows DWM transient Acrylic samples the real desktop and preserves its color.
2. A live `VisualBrush` samples the shared application backdrop for fallback rendering and deterministic visual QA.
3. A four-band perimeter lens re-samples progressively smaller regions. The outer band bends the background most; the inner band eases back into the surface.
4. The center uses only neutral density: primary glass is about 5% white and content glass about 2% white.
5. A non-uniform edge places specular light on the top/left, a local corner glint near the light source, and a dark contact edge on the bottom/right.
6. Detached surfaces use soft depth shadows; secondary cards use less refraction and a weaker shadow than the two primary cards.

Current optical starting values:

| Surface | Blur | Lens | Refraction | Radius |
|---|---:|---:|---:|---:|
| Primary glass | 20 px | 12 px | 8 px / 86% | 26 px |
| Content glass | 14 px | 6 px | 3.2 px / 52% | 24 px |
| Capsule | 20 px | 7 px | 2.8 px / 70% | full pill |

The live desktop remains the source of truth. The warm/green two-tone backdrop used by `--snapshot` is a clean QA target only: it makes blur, color transmission, and edge displacement measurable and is never shown during normal use.

## Interaction

- Glass follows the pointer with a low-energy white specular spot.
- Hover eases to 1.007 scale in 160 ms; press compresses to 0.994 and releases in 150 ms.
- Reduced Windows client-area animation disables interpolation.
- Trend and pulse charts isolate the focused sample, fade unrelated marks, and open a glass tooltip with exact values.
- Closing or minimizing the dashboard hides it to the tray and shows the widget. The tray owns explicit exit.

## Type, geometry, and color

- Font: `Segoe UI Variable Text`, with `Microsoft YaHei UI` fallback.
- Window / primary / nested radii: 32 / 26 / 14–24 px.
- Structural spacing: 8 / 12 / 16 / 24 px.
- White text is used over the system dark Acrylic backdrop; semantic cyan, mint, violet, lime, and coral are limited to data and actions, never the glass material.
- Demo data is always labeled `DEMO`; unavailable data must render `--` rather than an invented number.

## Optional skins

The UI keeps a collapsed decoration slot and resource hooks for future skins. External artwork may change an accent palette, logo glyph, or one low-opacity decoration, but may not change metric meaning, focus order, hit targets, or layout geometry.

No Angelina / Arknights asset is committed or redistributed. Local artwork can only be imported after the user confirms its license; code licensed under MIT does not grant rights to third-party images.

## Accessibility guardrails

- Interactive targets are at least 38 px in this dense desktop layout, with the main actions at 40–48 px.
- Icon-only controls keep tooltips and keyboard focus behavior.
- Status always includes text, not color alone.
- High-contrast and reduced-transparency fallbacks remain required before packaging.
- The data collector must never trade legibility for wallpaper-dependent transparency; a future adaptive contrast pass may increase neutral density without adding hue.

## Visual acceptance

The final UI snapshot was reviewed against a 100-point Liquid Glass rubric. It reached 85/100 overall (dashboard 86, widget 84). Required evidence is present: background color remains continuous across surfaces, the center is not an opaque card, the two-tone boundary softens under glass, and its position/width changes inside the layered edge lens.
