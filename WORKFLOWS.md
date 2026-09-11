# Workflows and tools

Commands are Windows-side (Visual Studio 2022 Build Tools v18, Windows SDK 10.0.26100). When the repository lives in WSL, run them through interop from `/mnt/c` with the UNC path, for example `cmd.exe /c '\\wsl.localhost\Ubuntu-24.04\home\albert\hypetabs\tools\build.cmd'` — the `.cmd` files `pushd` the UNC path onto a drive letter. Each live-Chrome run injects mouse input and takes the foreground; check the foreground window first and never run them while the user is working or gaming.

## Build and unit tests

| Command | Purpose |
| --- | --- |
| `tools\build.cmd` | Builds `HypeTabs.exe` and `HypeTabs.Bridge.exe`, then runs core, protocol, browser-state, history, and startup tests. Run after every native change. |
| `node tools/build-extension.mjs` then `node tests/extension.test.mjs` | Builds the worker with Node's TypeScript stripping and runs the extension tests (no packages). |
| `tools\test-cue.cmd` | Cue overlay: click-through, dismissal, taskbar continuation, repeated press, minimized restore, timeout. Needs the foreground (owned test windows). |
| `tools\test-window-identity.cmd` | Bounds matching, saved placement, scaling gate cross-check, display-layout mapping (real 3-monitor layout encoded). |
| `tools\test-options-layout.cmd` | Options DPI layout, scrolling, fit. |
| `tools\test-install.cmd`, `tools\test-uninstall.cmd` | Installer/uninstaller behavior with isolated files and registry keys. |
| `tools\test-wasm-url-native.cmd` | Experimental URL kernel equivalence (not production). |
| `tools\build-guidance-probe.cmd`, `tools\build-accessibility-probe.cmd`, `tools\build-locator-probe.cmd` | Helper executables required by the harnesses below. |

## Windows smoke

`tools\build-accessibility-probe.cmd` once, then `tools\smoke.cmd`: stages an isolated host with two synthetic profile pipes and checks lifecycle, keyboard flow, Options, persistence, notification recovery, and the overlay's UI Automation names. About 40 s; opens windows.

## Live Chrome harness

`tools\prepare-chrome-test.ps1` once (pinned Chrome for Testing download), then `powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe-extension-cft.ps1` with:

- `-Native` — production host/bridge with two visible profiles: snapshot, routing, restoration, stopped-profile explanation.
- `-Native -WindowBounds -TaskbarFlow` — physical taskbar-to-tab guidance (requires `guidance_probe.exe`). Variants: `-WindowState minimized|maximized`, `-TabLayout pinned|grouped|collapsed`, `-ReducedMotion`, `-ExtraWindow` (grouped taskbar identity fallback), `-Monitor secondary` (scaled monitor via Chrome display layout).
- `-Native -Incognito` — off-the-record page never reaches search.
- `-WasmBenchmark`, `-PrepareProbe` — experiments.

The harness refuses to start if `HypeTabs*` processes or the `com.hypetabs.bridge` registration exist; on an aborted run, kill `powershell` harness instances, `HypeTabs*`, CfT `chrome.exe`, and delete `HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.hypetabs.bridge` plus `HKCU:\Software\HypeTabs\ExtensionOrigin`. Both test browsers run visibly because Chrome for Testing 153 exits in `--headless=new` when the worker queries `chrome.system.display`. From WSL, prefer redirecting output on the Windows side or waiting for the final `Harness finished.` line; piping powershell output back into WSL can stall until Chrome's children exit, and a `cmd.exe /c` wrapper makes Chrome relaunch itself (the harness tolerates this).

## Screenshots

`powershell -NoProfile -ExecutionPolicy Bypass -File tools\screenshots.ps1` starts an isolated host, feeds two synthetic profiles over the local pipe, and writes `docs\screenshots\search.png`, `options.png`, and `about.png` (used by the README). Requires a built host and an idle desktop.

## Manual checks

First-run and moved-registration prompts appear only when the host runs without `--data-dir`; verify them by hand with the installed copy. Screenshots of the isolated host windows can be taken with a small script that starts `build\HypeTabs.exe --data-dir <temp>`, posts `WM_HOTKEY` / `WM_COMMAND` (102 Options, 105 About), and copies the window rectangles from the screen.

## Measurements

- `tools\measure-host.ps1` — five-minute idle CPU, private working set, overlay/query latency with 2,000 synthetic records.
- `tools\benchmark-search.cmd`, `tools\benchmark-index.cmd`, `node tools/benchmark-url.mjs` — core benchmarks.

## Packaging

`tools\install.ps1` / `tools\uninstall.ps1` (per-user, no administrator rights); see `README.md` and `docs/IMPLEMENTATION.md` for evidence and limits.
