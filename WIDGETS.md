# Floating Widget Design Philosophy

A reference concept for adding an always-visible desktop widget to any application. The examples come from a Windows implementation (Win32/GDI), but the principles are platform- and domain-agnostic.

## 1. What a widget is

A widget is a small, borderless, always-available surface that shows **only live, relevant state** and otherwise stays out of the way. It is not a window the user works in; it is glanceable, movable, resizable, and forgettable. Everything below follows from that.

## 2. Visual philosophy

- **Show only what is real.** Rows, items, and values appear only when they exist and have data. No placeholders, no "N/A", no spinners, no empty sections. If there is nothing to show, show one short sentence telling the user how to get something to show.
- **Dense, quiet, flat.** One background color, no chrome, no borders, no title bar, softly rounded corners. One typeface, one weight, one size. Text is never clipped or ellipsized — the widget's logical size is computed from its content, so the layout is authoritative, not the window.
- **Encode state in color and brightness, not in words.**
  - Continuous quantities use a continuous color ramp (e.g. good → caution → critical), not a few fixed swatches.
  - Items that changed since the last update are rendered **brighter**; unchanged items are **dimmed**. Recent activity is visible at a glance without reading.
  - Exactly one "needs attention" tint (background, text, and accents together) for items in an error or blocked state. Keep the last known values visible in that state — stale-but-honest beats blank.
- **Minimal labels.** Single glyphs or one-word labels beside thin bars, dots, or numbers. Full detail belongs in a hover tooltip, never in the body.
- **Detail on hover, action on click.** Tooltip gives the complete explanation of the item under the cursor. Click does one obvious thing per item (usually: fix or open the thing that needs attention). Everything else lives in a tray menu, context menu, or settings window — not in the widget.

## 3. Implementation philosophy

### Logical layout + scaled render
The widget has a fixed **logical** coordinate space (`logicalWidth × logicalHeight`, in layout px derived from font metrics and content). The user resizes the **window**; `scale = clientWidth / logicalWidth`. This separates *what is on the widget* from *how big it is*:

1. **Layout pass** walks the model, measures text, computes the logical size, and records a list of **hit rects** in logical space (item → rect).
2. **Render pass** draws into an offscreen bitmap at an integer oversampling factor (`renderScale = max(2, ceil(client / logical))`), with the font created at `fontPx × renderScale`. This runs once per data change, never per frame.
3. **Paint** is a single high-quality stretch of that bitmap to the client area. Drags are smooth and cheap; text stays crisp at any size; no reflow ever happens on resize.
4. Height is always derived from width via the logical aspect ratio, so a resize from any edge reduces to "choose a new width".

Pointer coordinates are converted client → logical before hit-testing, so tooltips and clicks are resolution-independent.

### Window mechanics
- Borderless popup, excluded from the taskbar/dock, shown **without activating** so it never steals focus. Optional user setting for always-on-top.
- Rounded corners via a window region/mask, re-applied on every size change with the radius scaled along with the content.
- Disable default background erase; everything comes from the bitmap, so there is no flicker.

### Drag vs. resize vs. click
- A single pointer-down handler captures the mouse. A small edge zone (≈8px) on the right edge, bottom edge, and bottom-right corner means **resize** (with the matching directional cursor); anywhere else means **move**.
- A **click slop** (≈4px): if the pointer never moves past it before release, treat it as a click and dispatch through the hit rects. The whole surface is therefore both draggable and clickable with no dedicated grip or handle.
- Enforce a minimum size only; **no maximum**. Trust the user.
- On release, persist position and size; after a real resize, invalidate the bitmap so it re-renders at the new oversampling factor.

### Persistence
Persist only: position, size (width suffices), visibility, always-on-top. Restore on launch. The widget carries no other state.

### Data flow
The widget is a **pure function of a model snapshot**. Data acquisition (network, files, sensors, IPC) happens on a background thread; the UI thread receives a "data changed" message, re-runs layout + render, resizes the window if the logical size changed, and invalidates. The widget never fetches, never blocks, and never animates on a timer.

### Cost
Because a widget is always running, it must be invisible in resource terms: background/idle process priority, low memory and I/O priority, no timer-resolution changes, no per-frame work. A widget that costs anything noticeable gets closed.

## 4. Checklist for porting

- [ ] Logical layout computed from content and font metrics; window size is a scale of it.
- [ ] Render once to an oversampled offscreen bitmap; paint by stretching.
- [ ] Hit rects stored in logical space; convert pointer coordinates before testing.
- [ ] Borderless, no-activate, no taskbar entry, rounded mask, optional topmost.
- [ ] Unified pointer handling: edge/corner = resize, body = move, no movement = click.
- [ ] Minimum size only; height follows aspect ratio.
- [ ] Persist position, size, visibility.
- [ ] Only real data is drawn; absence is silence, not a placeholder.
- [ ] State encoded by a continuous color ramp, changed-vs-unchanged brightness, and one attention tint that is clickable.
- [ ] Detail in tooltip, single-purpose click, everything else elsewhere.
- [ ] Process runs at background priority with no per-frame work.
