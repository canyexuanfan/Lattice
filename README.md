English | [简体中文](README.zh-CN.md)

# Lattice

A local, low-resource Windows desktop category lattice organizer.

Lattice lets you group desktop icons, files, and folders into persistent
"category lattices" (lattice = 格子). Categories stay on the desktop, work
without a network, and can be expanded or collapsed without disturbing the
real Explorer desktop icons underneath.

This is the open-source release of Lattice. The proprietary development
history, internal automation, and Windows desktop integration details are
intentionally not included.

---

## Highlights

- **Native Windows desktop layers.** Lattice uses Win32, Direct2D, DirectWrite,
  and Windows Shell COM. It does not depend on Qt, .NET, or any third-party
  UI framework.
- **Persistent category lattices.** Each category remembers its position,
  size, collapsed/expanded state, opacity, and contained items across
  restarts and reboots.
- **Single-original desktop custody.** Dropping a real file, folder, `.lnk`,
  or `.url` from the current user's or Public Desktop into a category moves
  that one original into the application's managed data folder; it is never
  copied or hidden by changing Explorer coordinates. Dropping it back restores
  the same original to its recorded desktop path and positions it at the
  requested Explorer grid cell. Items dragged from outside the desktop remain
  references at their original locations. Virtual Shell items use a separate,
  reversible visibility transaction.
- **Desktop-layer awareness.** Lattice's category windows live on the
  desktop layer, so they are covered by any normal application window but
  remain visible when the desktop is in the foreground. They never overlap
  ordinary application windows.
- **Configurable.** Position, size, opacity, theme, category list, and
  default behavior are all stored in a JSON config under the user's local
  AppData.

- **Installer preflight with second-scale timeout.** `PrepareToInstall`
  creates a config snapshot, asks the running Lattice instance to exit
  through `WM_QUIT`, and waits only for a short fixed window. If the
  instance is still running after that window, a single `WM_CLOSE` is sent
  and a second short window follows. The total wait is bounded in
  seconds; on timeout the installer returns a Chinese error and lets the
  user quit from the tray and retry. There is no multi-minute blocking
  dialog, no infinite wait, and no process kill.
- **Silent update exit.** All custom modal message loops (input dialog,
  message dialog, settings dialog) propagate `WM_QUIT`, so an update
  request that arrives while a dialog is open still ends the outer
  `App::Run` cleanly and releases the single-instance mutex.
- **Journalled managed storage.** Desktop files, folders, `.lnk`, and `.url`
  items use the same move transaction. The history of collection, move-out,
  configuration rollback, and crash recovery is journalled so the exact
  original path remains restorable. Public Desktop permission is requested
  through the Windows file-operation UI only when Windows actually requires it.

## Requirements

- Windows 10 or later (x64)
- Visual Studio 2022 with the "Desktop development with C++" workload
  (MSVC v143, Windows 10 SDK)
- Approximately 20 MB of disk space for installation

## Build

Lattice uses MSBuild and a Visual Studio project file. Two scripts in
`scripts/` cover the standard workflows:

```powershell
# Debug build of the EXE and the smoke harness
.\scripts\build.ps1 -Configuration Debug

# Release build with Whole Program Optimization
.\scripts\build.ps1 -Configuration Release

# Produce the Inno Setup installer
.\scripts\package.ps1
```

The output binary is `x64\Debug\Lattice.exe` or `x64\Release\Lattice.exe`.

## Smoke / Regression

Lattice does not yet have a unit-test framework, so the project's
regression coverage is an integrated Win32 / Explorer / window-layer
harness implemented in `src/testing/SmokeCommands.cpp` and driven by
`scripts/smoke.ps1`. The runner:

- Builds Debug
- Creates an isolated GUID-suffixed Config / Data / Runs tree for the run
- Exercises the smoke modes with a 30-second watchdog per mode
- Cleans the run tree on success, retains the evidence tree on failure

See `tests/README.md` for the full entry point and conventions.

## Project Layout

```
Lattice/
├── assets/         Branding assets (logo, installer images)
├── docs/           Product and architecture documentation
├── installer/      Inno Setup script for the Windows installer
├── scripts/        Build, package, run, and smoke scripts (PowerShell)
├── src/            C++20 source
│   ├── app/        Process startup, single-instance, tray icon
│   ├── config/     ConfigStore (JSON persistence)
│   ├── desktop/    Desktop scan, layout, session, watcher, shortcut store
│   ├── model/      OrganizerModel (MVC model header)
│   ├── rendering/  D2D context, icon cache, wallpaper backdrop
│   ├── shell/      Shell COM integration, drop targets, launcher
│   ├── testing/    Built-in smoke commands
│   ├── ui/         Windows, dialogs, icon grid, drag ghost
│   └── util/       ComInit, PathUtil, StringUtil, Win32Error
├── tests/          Test entry points and conventions
├── Lattice.vcxproj Visual Studio project file
├── LICENSE         GNU General Public License v3
└── README.md       This file
```

## Documentation

- `docs/desktop-organizer-prd-architecture-roadmap.md` — the product
  requirements, architecture, and roadmap for the project.

## License

Lattice is released under the **GNU General Public License v3 (GPL-3.0-only)**.
See [`LICENSE`](./LICENSE) for the full text.

Any derivative work that you distribute must also be released under
GPL-3.0, with the corresponding source made available under the same
terms. See the [GPL-3.0 FAQ](https://www.gnu.org/licenses/gpl-faq.html)
for the practical implications.

## Contributing

Bug reports, translations, and patches are welcome. By contributing, you
agree that your contributions will be licensed under GPL-3.0.

## Trademarks

"Windows", "Win32", "Direct2D", "DirectWrite", "WIC", "Inno Setup" and
other names are trademarks of their respective owners. Their use here
is for identification only and does not imply endorsement.
