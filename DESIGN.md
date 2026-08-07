# Tokenometer Design System

## 1. Direction

Tokenometer is a calm, light desktop instrument inspired by iOS 26 Liquid Glass. It combines Apple's quiet hierarchy with Linear's information density. The interface must look translucent and dimensional without sacrificing legibility.

The product has two surfaces:

- **Dashboard:** a roomy desktop canvas with an asymmetric metric grid.
- **Desk capsule:** a compact always-on-top window for current usage and remaining allowance.

Data is the protagonist. Decoration stays behind the data. The default skin uses no game artwork; optional skins can add one restrained illustration layer without changing component geometry.

## 2. Core Principles

1. Light glass, not white cards: every raised surface has controlled translucency, a bright top edge, and a faint cool border.
2. One primary accent: periwinkle is reserved for selection, live state, focus, and the most important chart series.
3. Honest data: unavailable values show `--` and a source label; demo values are visibly marked.
4. Progressive density: summary first, detail on hover or drill-in.
5. Motion explains state: 120–220 ms fades and lifts only. Respect reduced-motion settings.
6. Native behavior: keyboard focus, 44 px targets, Windows snap/resize, tray semantics, and high-contrast fallbacks.

## 3. Color Tokens

| Token | Value | Role |
|---|---:|---|
| `canvas.sky` | `#E9EDFF` | cool upper background |
| `canvas.blush` | `#F7EAF4` | warm lower background |
| `canvas.mint` | `#E7F7F3` | secondary atmosphere |
| `glass.base` | `#B8FFFFFF` | primary glass fill |
| `glass.soft` | `#86FFFFFF` | nested glass fill |
| `glass.hover` | `#D6FFFFFF` | hover fill |
| `glass.stroke` | `#CFFFFFFF` | illuminated edge |
| `glass.stroke.cool` | `#6FA8B5D0` | outer definition |
| `ink.primary` | `#172033` | headings and numbers |
| `ink.secondary` | `#626C80` | labels and metadata |
| `ink.tertiary` | `#8A94A7` | axes and unavailable values |
| `accent.primary` | `#6C75F6` | live/selected/focus |
| `accent.cyan` | `#39B8D8` | input/cached series |
| `accent.violet` | `#9A6AF2` | output series |
| `accent.mint` | `#58C7A1` | healthy/remaining state |
| `semantic.warning` | `#E9A23B` | nearing allowance limit |
| `semantic.danger` | `#E66676` | exhausted/error |

All body text must reach WCAG AA contrast on its final composited surface. If DWM material or wallpaper makes this uncertain, the app increases glass opacity automatically.

## 4. Typography

Use `Segoe UI Variable Text` with `Microsoft YaHei UI` fallback.

| Role | Size | Weight | Notes |
|---|---:|---:|---|
| Display metric | 38–44 | 600 | tabular numbers, tight tracking |
| Page title | 26 | 600 | one per page |
| Card metric | 24–30 | 600 | no more than one per compact card |
| Card title | 15–17 | 600 | sentence case |
| Body | 14–15 | 400 | 1.45 line height |
| Caption | 12–13 | 400/600 | source, time, axis |

Use tabular figures for token counts, percentages, times, and money-like credits. Avoid all-caps paragraphs.

## 5. Geometry and Spacing

- Base spacing: 4 px; structural rhythm: 8 / 12 / 16 / 24 / 32.
- Dashboard shell radius: 30 px.
- Main cards: 22 px; nested surfaces: 14–16 px; buttons: 12–16 px or full pill.
- Card padding: 20–24 px.
- Grid gap: 14–16 px.
- Minimum interactive target: 44 × 44 px.
- Main content max width: 1440 px; minimum supported window: 960 × 620.

## 6. Glass Construction

Every glass surface uses four layers:

1. DWM system backdrop when supported, otherwise a cool atmospheric gradient.
2. Semi-transparent neutral fill.
3. One-pixel cool outer stroke plus a brighter top/left highlight.
4. Very soft shadow only for detached surfaces such as the desk capsule and tooltips.

Do not stack more than two translucent surfaces over each other. Charts sit on a slightly more opaque nested surface to preserve contrast.

## 7. Components

### Dashboard shell

Borderless frame with custom drag region and native resize. The top row holds identity/source state, a segmented page switcher, and window actions. The overview uses a 12-column asymmetric grid.

### Metric card

Title and source timestamp at top, one dominant value, one visual, and at most two supporting values. Hover raises border luminance and reveals contextual detail; it never moves more than 2 px.

### Allowance ring

A 240–300 degree arc with the remaining percentage centered. A second line states the reset time. Unknown plan allowance renders a dotted neutral arc and `未提供`.

### Token pulse

A dotted/bar pulse for the last 1 or 5 minutes. Input, cached input, and output use the three accent series. Hover isolates a narrow time neighborhood and opens a glass tooltip with exact values.

### Session trend

A quiet smoothed line/area chart. Hover shows a luminous point, vertical guide, timestamp, and token breakdown. Axes are sparse.

### Desk capsule

Default size about 340 × 196 px. Borderless, topmost, draggable, and excluded from the taskbar. It shows current-session total, short pulse, remaining allowance, source state, and three compact actions: open dashboard, pin mode, hide.

### Navigation and buttons

Selected segmented controls use an opaque white lens with a faint accent glow. Icon buttons are circular or 14 px rounded. Press state scales to 0.96. Keyboard focus is a 2 px accent ring outside the component.

### Tooltip / hover lens

Use a 12–14 px radius glass bubble. The focused chart mark saturates; unrelated marks fade to 35–50%. Tooltips never cover the pointer and remain within the owning window.

## 8. Optional Skins

Skinning is resource-based. A skin may replace:

- atmospheric canvas brushes;
- accent palette;
- one optional low-opacity illustration in the header or empty-state region;
- logo glyph.

It may not change data semantics, layout order, typography scale, focus indicators, or hit targets. External artwork is never copied into builds without an explicit redistribution license.

## 9. Accessibility and State

- Full keyboard navigation and visible focus.
- Screen-reader names for icon-only actions and charts.
- High-contrast mode replaces transparency with opaque system colors.
- Reduced motion removes scale/lift animations.
- Color is never the only signal: status includes text or icon.
- Demo, local-log, API, and unavailable values always carry a visible source badge.

## 10. Guardrails

Do use restrained gradients behind glass, one accent, asymmetric card sizes, and generous internal spacing.

Do not use heavy neumorphic shadows, illegible wallpaper-dependent transparency, gratuitous blur, five competing accent colors, fake quota precision, or decorative assets inside dense charts.

