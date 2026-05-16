# Dash Portal Design System

The portal lives at `data/web/` and is served verbatim off LittleFS. Single
HTML page with three tabs (Study, Stats, Settings) and a separate
onboarding flow. Zero framework dependencies — vanilla JS + one CSS file.

## Voice

Earnest with mild sass. Dash cares about your focus but will side-eye you for
slacking. The portal mirrors this tone: warm, direct, never patronising.
Microcopy examples:

- "Start a session" not "Begin study session"
- "What are you working on?" not "Optional session label"
- "0 distractions resisted" celebrates the lack of distractions, framing it as
  a win even when the value is zero
- "Dash needs a name" (onboarding) not "Please enter device name"

## Palette

Dark-mode-only. Dash is for evening focus; dark is the appropriate default.

```
--bg          #0f1115   page background
--bg-card     #181c24   surfaces
--bg-card-2   #20252f   elevated surfaces (modals, dropdowns)
--bg-inset    #0b0d11   inputs / inset panels

--fg          #ecf0f9   primary text
--fg-dim      #8a92a2   secondary text
--fg-mute     #5a6373   tertiary text / placeholders

--accent      #f3c969   warm yellow — Dash's chosen accent.
                       single accent across the entire portal.
--accent-soft #f3c96922 same yellow at 13% opacity for tints
--accent-press#d8b14e   darker for active button states

--success     #66d9b9   completed session badge, "all good" indicators
--warn        #f3a548   distraction count, gentle warnings
--danger      #ef6577   destructive action confirmation, errors

--stroke      #2a3142   borders, dividers
--shadow      0 8px 24px rgba(0,0,0,0.4)
```

## Typography

System stack only. No web fonts.

```
font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", system-ui,
             "Inter", Roboto, sans-serif;
```

Three weights: 400 / 500 / 700.

Scale (mobile, base 16px):
- `--fs-xs`  12px  metadata, captions
- `--fs-sm`  14px  body small, labels
- `--fs-md`  16px  body, inputs
- `--fs-lg`  20px  card headings
- `--fs-xl`  28px  device name / page title
- `--fs-2xl` 44px  hero numbers (focused-time total, timer)

Line height: 1.4 for body, 1.15 for headings.

## Spacing

4 px base unit. Allowed values: 4, 8, 12, 16, 20, 24, 32, 48. Nothing else.

CSS variables: `--s-1` (4 px) through `--s-8` (48 px).

## Corners

- `--r-sm` 8 px  inputs, ghost buttons
- `--r-md` 12 px cards
- `--r-lg` 18 px hero card, modals
- `--r-pill` 999 px status pills, segmented control

## Motion

All transitions 200 ms ease-out. Single duration variable; no exceptions.

- Tab switch: fade + slight Y translate
- Button press: 100 ms scale 0.96 → 1
- Progress bar fill: 400 ms ease-out (slightly longer because it's data)
- Status pill colour change: 200 ms

## Components

- **Card**: `bg-card`, `r-md`, padding `s-5` (20px), gap `s-3`
- **Hero card**: same but `r-lg`, padding `s-6`
- **Button (primary)**: 100% width on mobile, height 52 px, `r-md`, `accent`
  background, weight 700
- **Button (ghost)**: 100% width, `r-md`, transparent, `stroke` border
- **Chip**: pill, height 36 px, `bg-inset` background; selected chip swaps to
  `accent` background, `bg` (dark) text
- **Input**: height 48 px, `bg-inset`, `r-sm`, 12 px horizontal padding
- **Toggle**: 52 × 32 thumb-style switch, `accent` when on
- **Tab bar**: sticky bottom on mobile, 3 tabs (Study / Stats / Settings),
  bottom-safe-area-inset aware

## Layout

Single column, max-width 480 px, centered on tablet+.

Header (sticky top): logo + status pill.
Tab bar (sticky bottom): three tabs.
Content scrolls between them, with safe-area padding.

## Accessibility

- All buttons have `aria-label` if icon-only.
- Focus ring: 2 px `accent` outline with 2 px offset.
- Touch targets ≥ 44 px tall.
- Contrast: fg/bg ≥ 4.5:1 (WCAG AA). Verified.
- Colour is never the only cue (status pill has text AND colour).

## Loading & error states

Every API call has both. Skeleton shimmer for first load; spinner button state
for actions; toast for transient errors with retry.
