# Remaining implementation work

Implement in the order below. Tasks remain unchecked until their complete behavior has been verified. Native shell, search, browser transport, and baseline extension evidence is recorded in `docs/IMPLEMENTATION.md`. `SPEC.md` defines required behavior; this checklist introduces no conflicting requirements. Mark tasks complete only after their behavior has been verified, and update the specification when decisions change it. Use no third-party dependencies without prior user approval.

## 1. Validate platform feasibility

- [ ] Establish a Windows 11 and Chrome development/test environment; record supported versions and select first-party Visual C++/Windows SDK and extension tooling.
- [ ] Benchmark representative extension event forwarding in TypeScript-generated JavaScript versus C++/WASM with a JavaScript bridge; compare steady-state CPU and memory, prioritizing even small repeatable gains over startup cost.
- [ ] Include bridge conversion, serialization, allocations, idle wakeups, and multiple profiles in the comparison. Repeat measurements and report variability; retain verified gains below perceptible thresholds and continue optimizing below the acceptance ceilings without weakening correctness or security.
- [ ] Compare total daily cost over extended browser sessions, including actual worker restarts, cumulative CPU use, retained memory, and resource growth. Prefer verified recurring savings even when startup is slower and the improvement is imperceptible.

URL normalization now avoids unnecessary byte-buffer allocations and duplicate work, with boundary tests and a reproducible Node microbenchmark in `tools/benchmark-url.mjs`. This does not complete the Chrome or WASM comparison.

The approved WASI SDK compiler now builds an import-free C++ URL-size kernel. Native and Node/WASM equivalence checks pass. The first warmed Node comparison includes copying strings into WASM and shows no advantage for this candidate; the production extension remains TypeScript. Full Chrome event-forwarding, memory, and worker-lifecycle measurements remain open.

The same kernel now runs in a disposable Chrome for Testing extension worker via `probe-extension-cft.ps1 -WasmBenchmark`. Randomized and boundary equivalence checks passed; nine warmed alternating trials again showed no clear advantage. This establishes Chrome execution and kernel measurements, not completion of the event-forwarding or lifecycle comparison.
- [ ] Prototype a minimal extension and C++ native messaging endpoint in two Chrome profiles. Verify separate profile identities, initial tab snapshots, and profile-scoped activation.
  `tools/probe-extension-ui.ps1` has verified Developer mode and Load unpacked control access in a disposable profile. Folder-picker completion and actual extension loading remain unverified; synthetic native messaging tests do not close this gate.
  `tools/probe-extension-cft.ps1 -Native -WindowBounds` verifies the production bridge and host with two actual Chrome for Testing profiles, a tab loaded before native registration appearing in the initial snapshot, separate duplicate-title results, and profile-scoped search activation. Ordinary Chrome setup/interaction acceptance remains outstanding.
- [ ] Establish how to match a Chrome window to its Windows window without relying on titles alone, private browser databases, or undocumented profile layouts.
  Unique physical bounds at 100% scaling now identify the foreground Chrome window for a live post-activation cue. Native tests reject overlapping, hidden, moved, and invalid candidates. Mixed scaling, broader multi-profile layouts, and taskbar-first identification remain outstanding.
  The development desktop is mixed (primary 96 DPI, two 125% portrait monitors); the gate uses effective per-monitor DPI. With the approved `system.display` permission the extension reports Chrome's display layout and the host converts DIP bounds per monitor; the taskbar-to-tab flow passed twice on the 125% secondary monitor (secondary taskbar, converted bounds), with later attempts disrupted by a fullscreen game holding the foreground during unattended runs.
- [ ] Validate restoration of an individual closed tab and URL reopening in its original running profile. Investigate verified launch identity for stopped profiles; document the retry workflow when safe launch is unavailable.
  A live HTTP tab closed in the second test profile now reopens exactly once there through native search, stays absent from the first profile, and consumes the retained closed result. Explicit session-versus-URL-fallback coverage and stopped-profile launch remain outstanding.
- [ ] Probe taskbar icons/previews and tab-header positions through supported accessibility interfaces. Cover grouping, pinned tabs, collapsed tab groups, and display scaling; document where direct activation is required.
- [ ] Record the validated architecture and platform limitations in `SPEC.md` before building production guidance or profile launching.

## 2. Establish the application foundation

- [ ] Scaffold the C++ desktop host and selected TypeScript or WASM extension with reproducible builds and a dependency inventory. Exclude secrets, `.env` files, and generated sensitive data from source control.
- [ ] Enforce one tray host per Windows user and the lowest process priority for every application-owned process, with supported background CPU/I/O settings.
- [ ] Implement the tray icon and Search, Options, Pause collection, and Exit commands; wire their full behavior as the relevant features are completed.
  The tray menu offers Search, Options, About HypeTabs (version, hypedriven.com link, GitHub link when a public page is configured), Pause collection, and Exit; Options carries a muted "Developed by hypedriven.com" link; both windows default to a dark palette unless high contrast is active (AGENTS.md/SYSTRAY.md conventions, 2026-09-11). Screenshots confirmed the overlay, Options, and About; smoke and options-layout tests pass.
- [ ] Establish clean startup/shutdown and resource disposal without a permanent taskbar window or administrator privileges.
- [ ] Add atomic settings storage, safe defaults, corruption recovery, and bounded opt-in diagnostics that omit browsing metadata.

## 3. Implement secure browser communication

- [ ] Complete the versioned native messaging protocol with restoration. Snapshots, tab events, activation, request correlation, and connection lifetimes are implemented and covered by native/mocked tests; live Chrome verification remains outstanding.
- [ ] Restrict native messaging to the installed extension and any local IPC to the current Windows user; avoid a listening network service.
- [ ] Validate message schemas, lengths, identifiers, allowed commands, rates, and queue limits before processing input. Verify malformed and excessive traffic is rejected safely.
- [ ] Implement per-profile connection registration, stable opaque profile identities, user-facing labels, and session-scoped tab/window identifiers.
- [ ] Implement reconnect reconciliation and extension lifecycle handling without duplicate entries or stale commands.
  Requests now expire independently after ten seconds and reject late responses/hints; the final response stops the timer. Native deadline regressions and a Windows early-timer restoration check pass. Delayed-response and lifecycle acceptance remain outstanding.
- [ ] Apply minimal extension permissions and exclude incognito data before it enters collection or messaging. Verify exclusion with actual incognito windows.
  Permissions: tabs, nativeMessaging, storage, alarms, sessions, and (user-approved on 2026-09-11) system.display for display-layout hints.
  `probe-extension-cft.ps1 -Native -Incognito` opens a page in a separate off-the-record DevTools browser context in the live test browser; the extension (incognito not allowed) does not observe it, and native search finds nothing for its title while open or after it closes. A user-opened Ctrl+Shift+N window with the extension explicitly allowed in incognito remains untested.

## 4. Track and activate open tabs

- [ ] Build the in-memory tab model from initial snapshots and incremental create, update, activate, move, detach/attach, close, and window events.
- [ ] Track profile/window context and recent activity; keep identical titles and URLs as distinct tabs.
- [ ] Implement direct activation in the correct profile and window, revalidating the target immediately before acting.
- [ ] Handle minimized windows and operating-system foreground restrictions with an explicit user action when needed.
  Foreground-failure notifications now reopen search for explicit retry and explain taskbar activation. Synthetic Windows callback/retry checks pass; physical notification interaction and actual foreground-restriction recovery remain unverified.
  The host now raises the natively verified Chrome window when Chrome reports focus but Windows kept the foreground elsewhere, and reports a focus failure when that is refused. The grouped-identity visible-Chrome flow reproduced the discrepancy (a foreground-owning fixture plus a second same-profile window) and passes with this handling.
  The host also delegates its foreground permission to the connection's Chrome browser process before hiding search; direct activation on a 125% secondary monitor (`-Monitor secondary`, no geometry verification possible) now brings Chrome forward without a cue.
- [ ] Verify activation across multiple profiles and windows, including tabs moved, navigated, or closed between lookup and selection.

## 5. Build local search

- [ ] Add incremental indexing of titles, hostnames, URLs, and profile labels, with case-insensitive partial-word, multiple-term, and inexpensive typo matching.
- [ ] Rank exact/prefix matches first and apply open-status/recent-activity tie-breakers; show recent tabs for an empty query.
- [ ] Cancel obsolete searches and preserve result identity across updates. Return a bounded initial list while supporting additional results.
  Unchanged row text and identities now reuse the native list, preserving scroll/selection. Windows smoke checks pass; repeated-query message p95 improved from 35.1 to 4.0 ms in the documented comparison. Changed-query latency stayed similar. Obsolete-search cancellation and full performance acceptance remain open.
- [ ] Verify ranking and responsiveness using representative titles, duplicate tabs, long URLs, and hostile text rendered only as data.
  Unicode regression and live Chrome tests cover accented Latin, Cyrillic, Japanese-only/no-match queries, combining marks, and supplementary characters. Windows invariant mapping replaces C-locale tokenization; broader linguistic and responsiveness acceptance remains outstanding.

## 6. Deliver the shortcut and search overlay

- [ ] Implement configurable global shortcut registration, capture, reset, and conflict reporting; choose and document a tested default.
- [ ] Open a compact overlay on the foreground window's monitor, focus its input, and support shortcut toggling and Escape with focus restoration.
- [ ] Display eight initial results with title, site, profile, window context, and status; support keyboard-accessible scrolling, Up/Down, Enter, and clicking.
  Windows smoke evidence verifies eight visible rows, twelve-result scrolling, navigation keys, Tab focus, and explicit mouse/Enter activation. Physical-input, visual readability, and display-scaling acceptance remain outstanding.
- [ ] Connect open results to direct activation and preserve selection during live updates.
- [ ] Show actionable empty, disconnected, stale-result, and missing-integration states while keeping connected profiles searchable.
- [ ] Implement screen-reader labels, high contrast, display scaling, and keyboard-only use as part of the overlay.
  The overlay now annotates its input, list, and status label with MSAA and UI Automation names and a polite live region; `build/accessibility_probe.exe` verifies them from another process during `tools\smoke.cmd`. Actual screen-reader (Narrator/NVDA) and high-contrast theme acceptance remain manual.

## 7. Add recently closed tabs and protected persistence

Implemented with native, mocked-extension, and Windows synthetic-profile tests: observed/imported closures, same-profile restore commands, retention controls, DPAPI persistence, and Clear saved data. Items remain unchecked until their full Chrome behavior and failure cases are verified. See `docs/IMPLEMENTATION.md`.

- [ ] Capture observed closures and available browser restoration identifiers; import browser-provided recent closures where available without promising complete prior history.
- [ ] Store only required metadata in protected per-user storage using atomic writes and corruption recovery; batch writes and bound memory/disk usage.
- [ ] Enforce seven-day and 1,000-entry default retention, oldest-first eviction, startup/runtime expiry, shorter retention, and disabled retention.
- [ ] Include closed tabs in search with closure time and clearly distinguish them from open tabs.
- [ ] Restore the selected tab through browser restoration when possible; otherwise reopen only HTTP/HTTPS URLs in the original profile using safe structured commands.
- [ ] Implement verified stopped-profile launching where feasible, or an explicit explanation and retry after the user opens the profile. Never fall back to a different profile.
  Chrome exposes no profile-directory identity to the extension, so launching a stopped profile is not implemented; the explanation path is verified live: `probe-extension-cft.ps1 -Native` now exits the second test profile after a tab closed there, and the closed entry stays searchable, Enter shows "Open this Chrome profile and reconnect its extension, then try again.", and the other running profile opens nothing.
- [ ] Require confirmation if restoration would open extra tabs/windows, and reconcile successful restores without duplicate closed candidates.
- [ ] Verify persistence across restart, expiry, unsupported URL rejection, safe profile launch parameters, and restoration race conditions.

## 8. Complete application options

Profile names/status, reconnect instructions, opt-in sign-in registration, guidance toggle, timeout, and outline preference are implemented. Native/mocked tests and the Options smoke flow pass; actual sign-in, live Chrome label persistence, and broader accessibility acceptance remain unverified.

- [ ] Wire persistent shortcut settings and connected-profile labels/status, with per-profile extension setup instructions.
- [ ] Add opt-in start at sign-in and verify disabling it removes the registration.
  First run now asks once whether to start at sign-in (skipped for isolated data directories used by tests); later starts detect a registration pointing at another copy and offer to move it to the running path. Physical acceptance of the prompt remains manual.
- [ ] Implement Pause/resume collection: stop indexing and metadata writes while paused, retain searchable data with stale labels, continue expiry, and reconcile on resume.
- [ ] Add retention controls and Clear saved data; verify deletion is immediate and open tabs repopulate only when collection is enabled.
- [ ] Add guided/direct activation preference, five-second default cue timeout, and reduced-motion settings, ready for the guidance implementation.
  A cue colour picker (standard colour dialog, "System" reset) persists in settings version 5, and the tab-header arrow now sits below the header pointing up. The outline preference persists across restart and overrides arrow placement; the cue also reads Windows client-area animation policy. Native cue geometry/interaction tests and the Windows Options persistence smoke pass. System-policy changes and full settings migration coverage remain to be verified.
- [ ] Verify closing Options keeps the application running and Exit unregisters shortcuts and disposes application resources.

Options now scales retained control coordinates and Windows fonts with DPI, fits the monitor work area, scrolls when needed, and reveals keyboard-focused controls. Native layout tests pass at 96/120/144/192 DPI and the full Options smoke passes. Physical mixed-monitor transitions, visual readability, and screen-reader acceptance remain open.

## 9. Implement visual guidance

Progress: cue interaction tests, taskbar association, and one integrated visible taskbar-to-tab flow pass. Guided/direct settings and timeout controls exist. Broader cross-profile native window identity, scaled layouts, grouped previews, and full accessibility acceptance remain open; see `docs/IMPLEMENTATION.md`.

The integrated taskbar-first flow now passes `probe-extension-cft.ps1 -Native -WindowBounds -TaskbarFlow` with visible Chrome at 96 DPI. The test physically clicks the independently rechecked Explorer target, verifies immediate first-cue dismissal and a second cue at the selected Chrome tab header, then verifies dismissal on another click. This establishes one complete layout/target case; grouped previews, other scales, races, and accessibility coverage remain open.

- [ ] Build noninteractive, accessible, non-focus-stealing cues using the validated Windows/browser target discovery approach.
- [ ] Show a taskbar icon or preview cue only when its position is reliable; do not imply a grouped icon uniquely identifies a window.
  `probe-extension-cft.ps1 -Native -WindowBounds -TaskbarFlow -ExtraWindow` verifies that a second visible window in the same profile suppresses the taskbar cue and that direct activation still ends with a foreground Chrome window and a tab-header cue.
  `tools/probe-taskbar.cmd` verifies an experimental locator using the target window's explicit AppUserModelID and Explorer's automation identity. On the current three-monitor layout it found one matching button on the target monitor and rejected a second visible window sharing that identity. Generic labels/HWND properties alone did not work. The host now integrates background discovery and the two-stage flow. Three synthetic fallback paths pass; physical taskbar acceptance, full race revalidation, and grouped-window preview association remain open.
- [ ] When the intended window opens, show a cue at the actual tab header when reliably located; use direct activation for hidden or unlocatable targets.
- [ ] Implement guidance state transitions so each cue disappears on the next click anywhere, while the result-selection click cannot dismiss the cue it creates. Allow a second cue only after the intended window-activation click completes.
  The `prepare` command now selects a tab without window focus and returns revalidated bounds. Extension/native protocol tests pass; a live Chrome probe confirms a previously inactive tab becomes active while its window remains unfocused. Production correlation and background taskbar lookup are connected; actual taskbar click acceptance remains pending.
  Cue-level click transitions now pass owned-window tests: immediate mouse-down hiding, release plus intended foreground before continuation, cancellation on another click/Escape/release outside, and timeout cleanup. The host now connects these transitions to preparation and asynchronous taskbar discovery, with generation, deadline, and connection checks.
- [ ] Cancel active and pending guidance on unrelated clicks, Escape, target disappearance, timeout, or cancellation; release input monitoring and accessibility observers immediately.
- [ ] Verify pinned/grouped tabs, minimized/maximized windows, taskbar grouping, multiple monitors, scaling changes, and reduced motion without screenshots, OCR, or continuous polling.
  Minimized, normal, and maximized visible-Chrome taskbar-to-tab flows now pass at 96 DPI with production identity matching; the cue accepts Explorer's single restore move for minimized windows. `-TabLayout pinned|grouped|collapsed` also passes after the locator accepted Chrome's accessible-name suffixes. `-ReducedMotion` runs the same flow with the saved outline preference and requires both cues to be outlines. `-ExtraWindow` covers taskbar grouping by fallback; a scaled secondary monitor covers direct activation only. Live scaling changes and grouped previews remain open.

## 10. Validate reliability, security, and performance

`tools/benchmark-search.cmd` verifies unchanged ranking and measures native search at idle/background priority. Skipping score comparisons that cannot improve a match reduced median CPU cycles about 10.8% on the documented synthetic 2,000-record workload. This is a core-search comparison, not full application budget acceptance; see `docs/IMPLEMENTATION.md` for methodology and variability.

`tools/benchmark-index.cmd` verifies identical final indexes and measures reuse for metadata-only updates. Median CPU cycles decreased about 82.9% for its synthetic mixed-event workload; native regressions and the live two-profile Chrome flow pass. Full transport/browser CPU and idle-memory acceptance remain outstanding.

`tools/measure-host.ps1` completed a five-minute idle run with 2,000 records and five synthetic profile connections: no measurable process CPU-time increment, about 7 MiB sampled private working set, 66.8 ms overlay-message p95, and 39.5 ms fixed-query p95. These exclude Chrome/bridge overhead, physical input, rendering completion, peak memory, varied queries, and explicit contention. Current payload files total 0.432 MiB before the generated registration manifest. Full acceptance remains open; details and build identity are in `docs/IMPLEMENTATION.md`.

- [ ] Exercise browser/host restarts, extension updates, disconnects, sleep/resume, monitor changes, and corrupt persisted state; confirm no stale activations or duplicated records.
- [ ] Verify local access restrictions, bounded message handling, incognito exclusion, safe text rendering and process launching, retention/clearing, and absence of browsing metadata in logs.
- [ ] Run complete keyboard and screen-reader workflows and verify focus restoration, high contrast, click dismissal, and direct activation fallback.
- [ ] Benchmark 1,000 open tabs, 1,000 retained closures, five profiles, and ten windows on a documented Windows 11 reference machine with lowest priority enabled.
- [ ] Meet host budgets: idle CPU below 0.1% over five minutes, private working set at or below 75 MiB, warm overlay readiness below 100 ms p95, and search results below 50 ms p95.
  Re-measured 2026-09-11 after the guidance/accessibility changes: 0.02% idle CPU, 6.5 MiB private working set, 13.3 ms overlay p95, 4.2 ms query p95 on the synthetic workload (host-only; Chrome/bridge and physical input excluded).
- [ ] Measure cold start, behavior under CPU contention, and extension CPU/memory separately and across profiles. Resolve budget failures without raising priority or adding unapproved dependencies.

## 11. Package and verify the first release

- [ ] Package the host, extension, and per-user native messaging registration with setup instructions for each Chrome profile and documented C++ runtime requirements (if any).
  Installation preflight and copy tests verify exact artifacts/removal scripts, repeat updates preserving unknown files, and rejection of redirected/malformed destinations or missing source files before writes. The production installer uses this tested path; actual registration and clean-account setup remain separate acceptance checks.
- [ ] Provide removal instructions or uninstall support that cleans up tray processes, sign-in entries, shortcuts, native messaging registration, and retained application data.
  Installed removal scripts and isolated registry/file tests are implemented. Tests verify refusal of a running production host, junctions at the root/ancestor/App/extension, drive roots, and non-user registry paths. User exits the app and removes profile extensions before removal; running-bridge-specific and clean-account acceptance remain to be verified.
- [ ] Verify application-owned installed assets stay at or below 20 MiB; report prerequisite and fresh-install download sizes separately.
- [ ] Test installation and normal use on a clean Windows user account without administrator privileges; verify the dependency inventory contains no unapproved third-party additions and no third-party application runtime dependencies.
- [ ] Demonstrate all seven acceptance scenarios in `SPEC.md`, record results and supported platform limitations, and update both documents to reflect delivered behavior.

## Current checkpoint

Verified on 2026-09-11 in visible Chrome for Testing: minimized taskbar-to-tab guidance, pinned/grouped/collapsed tab headers, reduced-motion outline cues, grouped taskbar identity fallback, exclusion of an off-the-record page, the stopped-profile explanation path, and — after the user approved the `system.display` permission — taskbar-to-tab guidance on the 125% secondary monitor using Chrome's reported display layout. Fixes along the way: cue tolerates Explorer's restore move and a repeated press on the same taskbar button; tab locator accepts accessible-name suffixes; scaling gate uses effective per-monitor DPI; host delegates its foreground permission to the connection's chrome.exe ancestor and raises a verified window when Chrome's focus report disagrees with Windows; overlay carries MSAA/UIA names and a live status region.

Tooling is catalogued in `WORKFLOWS.md`. Harness notes: Chrome for Testing 153 exits in headless mode when the worker calls `chrome.system.display.getInfo()`, so both test browsers are now visible; Chrome relaunches itself (launcher exits) when started inside a job object, which the harness now tolerates; a fullscreen game in the foreground during some unattended runs made taskbar clicks unreliable; once the desktop was idle, `tools\test-cue.cmd` (including the repeated-press case), `tools\smoke.cmd`, and the `-Monitor secondary` taskbar-to-tab flow all passed with the final build. Remaining unchecked items need manual acceptance: grouped taskbar previews, verified stopped-profile launching (no profile identity from Chrome), Narrator/NVDA and high-contrast review, clean-account installation, ordinary Chrome setup, and the seven acceptance scenarios.
