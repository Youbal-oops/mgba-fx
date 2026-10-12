# Developing mGBA-FX on Windows

This fork implements the experimental [GBA-FX detection prototype](fx-extension.md). Marked GBA development ROMs expose identification registers; expanded RAM, CPU capacity, resource settings, and `.gna` packaging remain later milestones.

## Portable toolchain

Use Python 3.12 or newer with pip, and run this command from the repository root in PowerShell:

```powershell
./tools/fx-build.ps1 -Bootstrap
```

The bootstrap downloads build helpers from PyPI, GCC and Qt from the official Qt repository, cmocka source from cmocka.org, and libepoxy/zlib/PNG packages from the official MSYS2 repository. It installs them under `build-tools/` and builds under `build-fx/`; both directories match the repository's existing ignored build-directory patterns. It does not install system packages or permanently change PATH.

Pinned primary dependencies:

| Dependency | Version |
| --- | --- |
| CMake | 3.31.6 |
| Ninja | 1.11.1.4 |
| aqtinstall | 3.3.0 |
| GCC / MinGW | 13.1.0, `qt.tools.win64_mingw1310` |
| Qt | 6.8.3, `win64_mingw`, including Multimedia |
| cmocka | 1.1.8, built from the checksum-pinned source archive |
| libepoxy | 1.5.10-7, checksum-pinned MSYS2 MinGW64 package |
| zlib | 1.3.2-2, checksum-pinned MSYS2 MinGW64 package |
| libpng | 1.6.59-1, checksum-pinned MSYS2 MinGW64 package |

The Qt archives are verified by aqtinstall. The cmocka source SHA-256 is `58435b558766d7f4c729ba163bdf3aec38bed3bc766dab684e3526ed0aa7c780`.

The libepoxy SHA-256 is `a2dca7211c2c6a3bb335c074167db9929f62da55d566df29aea26e5e88f35bfc`, matching [the official MSYS2 package metadata](https://packages.msys2.org/packages/mingw-w64-x86_64-libepoxy). This fork requires libepoxy for the normal Windows library build, including configurations that request software rendering.

Qt downloads use its official master endpoint to avoid the stalled mirror encountered during initial setup. The bootstrap needs network access; subsequent builds use the downloaded tools locally.

## Rebuilding and running tests

```powershell
./tools/fx-build.ps1
```

The script configures the core, Qt frontend, unit suites, headless runner, performance tool, fuzz harnesses, and CInema. It builds with four parallel jobs and runs the unit/Qt suites. It explicitly checks that the GBA core suite was configured, because CMake otherwise disables it when cmocka cannot be found.

The script selects GCC's `gcc-ar` and `gcc-ranlib` wrappers explicitly so the repository's link-time optimization flags work for the bundled static dependencies.

Link-time optimization is disabled for the emulator development build. With this GCC 13.1 shared-library configuration, automatic exports classified core functions as data, and the resulting executables failed with out-of-range Windows pseudo relocations before running any games. Baseline and enhancement comparisons use the same setting.

Useful options:

```powershell
./tools/fx-build.ps1 -Configuration Debug -Jobs 4
./tools/fx-build.ps1 -SkipQt
./tools/fx-build.ps1 -SkipTests
./tools/fx-build.ps1 -Fresh
```

The script uses the downloaded zlib/PNG libraries and the existing bundled SQLite dependency. It enables OpenGL through libepoxy and disables optional FFmpeg, external archive libraries, ELF loading, Lua, JSON-C, font scripting, and Discord integration for this baseline. CInema/performance runs use the software renderer unless explicitly configured otherwise. This build is a development configuration, not a complete distribution package.

Use `-Fresh` after changing compilers or dependency layouts. It resets the generated CMake configuration inside `build-fx/` so stale compiler/dependency discoveries do not affect the next build.

Qt tests run with the offscreen platform plugin. The script restores the caller's process environment after it completes.

## Running the frontend and regression tools

The downloaded runtime DLLs must be reachable when running executables directly. In a PowerShell session at the repository root:

```powershell
$repoRoot = (Get-Location).Path
$env:PATH = "$repoRoot/build-fx;$repoRoot/build-tools/qt/Tools/mingw1310_64/bin;$repoRoot/build-tools/qt/6.8.3/mingw_64/bin;$repoRoot/build-tools/deps/bin;$repoRoot/build-tools/deps/mingw64/bin;$env:PATH"
$env:QT_PLUGIN_PATH = "$repoRoot/build-tools/qt/6.8.3/mingw_64/plugins"

./build-fx/mGBA.exe
./build-fx/test/mgba-cinema.exe -b ./cinema -v gba
./build-fx/mgba-perf.exe -P -F 600 ./cinema/gba/irq/keyirq/test.gba
```

CInema is run separately from the default unit-test invocation so the caller can select a regression group. Do not use its baseline-update options during ordinary verification. The committed images are the expected output.

## Running the detection demonstration

The build script generates the two original diagnostic ROMs and executes them when tests are enabled. To regenerate or inspect them separately:

```powershell
python ./tools/fx-generate-diagnostic.py
./build-fx/mgba-fx-diagnostic.exe ./build-fx/diagnostics/ordinary.gba 0 ./build-fx/diagnostics/ordinary.png
./build-fx/mgba-fx-diagnostic.exe ./build-fx/diagnostics/enhanced.gba 1 ./build-fx/diagnostics/enhanced.png
./build-fx/mGBA.exe ./build-fx/diagnostics/enhanced.gba
```

Use the runtime PATH/plugin setup above. The ordinary fixture displays `NORMAL GBA`; the enhanced fixture displays `FX DETECTED`. Both use the same original boot program and frame assets. Only the enhanced file has the validated development footer. The standardized cartridge logo comes from the existing GBA regression fixture; no game program or game assets are copied.


The verifier executes eight frames, checks the guest-written mode at `0x02000000`, compares all 38,400 VRAM pixels with the appropriate ROM asset, and writes a PNG screenshot. This demonstrates detection through executed guest code, in addition to parser unit tests.

The fixture generator is test infrastructure, rather than the public SDK or final package format. Temporary footer requirements and register addresses are documented in [fx-extension.md](fx-extension.md).

## Verified baseline and first milestone

Verified locally on 2026-10-11, starting from commit `143adf5aa2a60c6036810b0cd72445e13d74b0e6`, using the pinned toolchain above and `RelWithDebInfo`, LTO disabled. Host: AMD Ryzen 5 PRO 2400GE, eight logical processors, 33,216,200,704 bytes physical RAM.

| Check | Before detection | After detection |
| --- | --- | --- |
| Desktop, core, and command-line build | Pass | Pass |
| Existing CTest suites, excluding CInema | 26/26 pass | 26/26 pass |
| Added GBA-FX suite | Not present | 13/13 cases pass; 27/27 total suites |
| Available GBA CInema cases | 26/26 pass | 26/26 pass |
| Qt executable `-h` smoke test | Exit 0 | Exit 0 |
| Ordinary diagnostic, eight frames | Mode 0, framebuffer CRC `7EDCE083` | Same mode and CRC |
| Enhanced diagnostic, eight frames | Mode 0, CRC `7EDCE083` | Mode 1, CRC `15A05479` |

The new suite covers descriptor fields, required/optional features, descriptor and boot checksums, marker boundaries, short/error reads, probe cursor preservation, mapping failure, non-power-of-two boot images, the 32 MiB boot-plus-footer boundary, register widths/debug views, read-only values, reset/load/unload transitions, failed-load recovery, instance isolation, and enhanced patch rejection. Additional content is rejected in this milestone; content-segment overlap tests belong to the later storage implementation.

Before detection integration, the build exposed three existing Windows/toolchain problems:

- LTO caused automatic DLL exports to classify core functions as data; disabling it fixed executable startup relocations.
- The existing `findNullPath` test crashed on Windows. `mCoreFind(NULL)` now returns `NULL` before calling platform file/archive APIs; the existing test passes.
- The older MinGW headers lacked Windows 11 corner-style enum declarations. The Qt frontend now passes the documented DWORD attribute/value while preserving its existing square-corner preference. See [Microsoft's attribute definition](https://learn.microsoft.com/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute) and [preference definition](https://learn.microsoft.com/windows/win32/api/dwmapi/ne-dwmapi-dwm_window_corner_preference).

The non-power-of-two ROM copy path also now releases the original mapping and handles allocation failure. This supports the descriptor's permitted boot-image lengths without leaving the initial file mapping behind.

Measurements and raw logs are local, ignored artifacts under `build-fx/baseline/` and `build-fx/milestone-1/`. The preserved baseline binaries are under `build-fx/baseline/bin/`. Reproduce software-renderer measurements with:

```powershell
./tools/fx-benchmark.ps1 -OutputDirectory build-fx/my-measurements
```

Each case runs 600 frames three times. The JSON records native emulation durations, startup-inclusive wall time, ROM/core hashes, and sampled process working set at 20 ms intervals. These short host-dependent measurements are smoke checks, not timing guarantees or exact memory peaks.

| Ordinary case | Baseline median, ms | Detection-build median, ms |
| --- | ---: | ---: |
| IRQ/key input | 38.829 | 30.952 |
| Object 2D wrapping | 159.464 | 159.733 |
| Disabled-background blend | 569.919 | 468.775 |

Host scheduling produced noticeable variation, so these figures do not establish a speedup. Visual regression output remains identical; the ordinary diagnostic screenshot also matches the baseline byte for byte. Full timing/audio, persistence, resource-pressure, and long-run validation remain future work.
