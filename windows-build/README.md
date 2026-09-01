# Mengshee Windows Build

This directory contains the Windows standalone SDK, runtime deployment, staging,
and installer scripts for this Mengshee checkout.

Run commands from the repository root:

```powershell
cd C:\Users\jairy\Documents\okular\mengshee
```

Build output lives outside the source checkout under the sibling
`..\windows_build` directory.

## Layout

- Source tree: `.`
- Build workspace: `..\windows_build`
- SDK prefix: `..\windows_build\sdk`
- Mengshee build dir: `..\windows_build\build\mengshee-standalone`
- Mengshee install dir: `..\windows_build\install\mengshee`
- Package stage: `..\windows_build\dist\mengshee-pdf\app`
- Package output: `..\windows_build\dist`

## Requirements

- Windows 10/11 x64.
- MSVC x64 toolchain and Windows SDK. Visual Studio 2022 or newer is fine when
  passed through `-VcVars`.
- Qt for MSVC, for example `C:\Qt\6.11.1\msvc2022_64`.
- Git.
- Inno Setup 6, only when building the installer `.exe`.
- Initialized third-party sources:

```powershell
git submodule update --init --recursive external/poppler external/stemtex
```

StemTeX source is looked up in this order: an explicit `STEMTEX_ROOT`, the
`MENGSHEE_STEMTEX_SOURCE_ROOT` environment variable, `external/stemtex`, and
then the sibling StemTeX development checkout.

The submodule pins the required StemTeX version. Runtime deployment accepts only
the canonical CMake `staging` layout and requires its `runtime\VERSION` to match
the selected source tree. Old `dist\stemtex-installer` and daemon-static trees
are not package inputs.

For release work, install StemTeX into a fresh, version-specific directory and
pass both paths to the Mengshee driver:

```powershell
$stemtexRoot = "C:\Users\jairy\Documents\xetex\stemtex"
$stemtexVersion = (Get-Content "$stemtexRoot\VERSION").Trim()
$stemtexStage = "C:\Users\jairy\Documents\okular\windows_build\stemtex\StemTeX-$stemtexVersion"

if (Test-Path $stemtexStage) {
  throw "StemTeX stage already exists: $stemtexStage"
}

& "C:\Qt\Tools\CMake_64\bin\cmake.exe" --install `
  "$stemtexRoot\build\stemtex-ninja" --prefix $stemtexStage
```

Build and smoke-test StemTeX first using its own
`docs\BUILDING_AND_PACKAGING.md`. A release stage containing generated profile
`.xdv`, `.aux`, `.log`, or `.pdf` files is rejected.

If using a non-default Visual Studio path, pass it to CMake scripts:

```powershell
-DVCVARS="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat"
```

## Canonical CMake Build

Use the CMake driver for normal Windows builds:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DQT_PREFIX=C:/Qt/6.11.1/msvc2022_64 `
  -DVCVARS="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat" `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  "-DSTEMTEX_ROOT=$stemtexRoot" `
  "-DSTEMTEX_STAGE_ROOT=$stemtexStage" `
  -P windows-build/cmake/build-windows.cmake
```

It runs:

1. CMake configure/build/install for Mengshee
2. CMake runtime deployment from the SDK, Qt, StemTeX, and Mengshee data
3. stage refresh under `..\windows_build\dist\mengshee-pdf\app`
4. Inno Setup installer build

For a clean Mengshee rebuild, add `-DCLEAN_BUILD=ON`.

If Inno Setup is not installed, still build and stage the runtime with:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DQT_PREFIX=C:/Qt/6.11.1/msvc2022_64 `
  -DVCVARS="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat" `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -DSKIP_INSTALLER=ON `
  -P windows-build/cmake/build-windows.cmake
```

Expected installer output:

```text
..\windows_build\dist\mengshee-<version>\Mengshee-<version>-Setup.exe
```

## CMake Package Step

After `..\windows_build\install\mengshee` has been built and deployed, the
package-only CMake script can recreate the stage and installer without
rebuilding Mengshee:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DCLEAN_STAGE=ON `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -P windows-build/cmake/package-windows.cmake
```

This script mirrors the deployed install tree into
`..\windows_build\dist\mengshee-pdf\app`, validates Mengshee icons, gettext
catalogs, annotation resources, and Poppler CMap/CID data, then calls Inno
Setup. The main installer keeps the StemTeX renderer binaries and only the
maintained `unicodemath` base profile in its required core component. The TeX
package/font tree is staged
under `..\windows_build\dist\mengshee-pdf\optional\stemtex-support` and embedded
in the same installer as the optional `Bundled StemTeX TeX tree` component.
The recommended installation type selects it by default; users can choose the
compact type or clear the component when they want to use an external TeX tree.

To validate staging without building an installer:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DSKIP_INSTALLER=ON `
  -DCLEAN_STAGE=ON `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -P windows-build/cmake/package-windows.cmake
```

To write the installer into a temporary output directory:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DCLEAN_STAGE=ON `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -DOUTPUT_DIR=C:/Users/jairy/Documents/okular/windows_build/tmp/cmake-package-test `
  -P windows-build/cmake/package-windows.cmake
```

## Internal Scripts

These scripts are internal steps. Use them only when debugging a specific part
of the Windows pipeline.

- `cmake\build-windows.cmake`
  - Canonical Windows driver for Mengshee configure/build/install, runtime
    deploy, stage refresh, and installer creation.
- `cmake\deploy-runtime.cmake`
  - CMake-based runtime deployment from an installed Mengshee tree.
- `cmake\package-windows.cmake`
  - CMake-based stage and installer creation from an already deployed
    `..\windows_build\install\mengshee` tree.
  - Builds one Mengshee installer containing the bundled StemTeX TeX tree as an
    optional installation component.
- `cmake\bootstrap-kf6-sdk.cmake`
  - CMake-based ECM bootstrap for the local Windows SDK.
- `cmake\build-kf6-module.cmake`
  - CMake-based KF6 module checkout, configure, build, and install driver.
- `cmake\build-zlib-sdk.cmake`, `cmake\build-freetype-sdk.cmake`,
  `cmake\build-libintl-shim-sdk.cmake`, `cmake\build-poppler-sdk.cmake`
  - CMake-based support library builders.
- `cmake\install-gettext-native-sdk.cmake`,
  `cmake\install-winflexbison-sdk.cmake`
  - CMake-based pinned binary tool installers.
- `smoke-test-mengshee-stage.ps1`
  - Starts staged `mengshee.exe` with a test PDF and verifies it loads modules
    from the stage, not from another workspace tree.

The legacy PowerShell build, deploy, and installer entry points have been
removed. Use the CMake drivers above for all Windows builds and packages.

## Stage-Only Refresh

If the install tree has already been built and deployed, use the CMake package
script to refresh the stage without rebuilding the installer:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DSKIP_INSTALLER=ON `
  -DCLEAN_STAGE=ON `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -P windows-build/cmake/package-windows.cmake
```

If a staged StemTeX process is still running, stop it before staging:

```powershell
Get-Process | Where-Object { $_.ProcessName -match 'mengshee|stemtex|xetex|xelatex|xetexdaemon' } |
  Stop-Process -Force
```

## Runtime Data Contract

The CMake runtime deployment intentionally mirrors SDK runtime data from
`..\windows_build\sdk\bin\data` into `bin\data`. That SDK sync can remove files
installed earlier by CMake, so deployment must restore Mengshee data after the
sync.

The deployed install tree and stage must contain:

- `bin\mengshee.exe`
- `bin\mengshee.ico`
- `bin\data\applications\org.jairy.mengshee.desktop`
- `bin\data\metainfo\org.jairy.mengshee.appdata.xml`
- `bin\data\icons\hicolor\<size>x<size>\apps\mengshee.png`
- `bin\data\mengshee\tools.xml`
- `bin\data\mengshee\toolsQuick.xml`
- `bin\data\mengshee\drawingtools.xml`
- `bin\data\mengshee\pics\annotation-*.svg`
- `bin\data\locale\<lang>\LC_MESSAGES\okular*.mo`
- `share\poppler\cMap\...`
- `share\poppler\cidToUnicode\...`
- `StemTeX\runtime\bin`
- `StemTeX\runtime\VERSION`
- `StemTeX\runtime\bin\sdk\stemtex-profile.dll`
- `StemTeX\gui\profiles\unicodemath`
- `bin\stemtex-profile-creator.exe`

The main stage must not contain `StemTeX\runtime\texmf-dist` or writable
StemTeX cache/state directories such as `StemTeX\runtime\texmf-var\fonts`.
The xelatex daemon format under `StemTeX\runtime\texmf-var\web2c` is a read-only
runtime input and remains in the main package.

Do not rely on the CMake install step alone for a runnable/packageable Windows
tree. Always run runtime deployment, or use the canonical
`windows-build\cmake\build-windows.cmake` entry point. For packaging an already
deployed tree, use `windows-build\cmake\package-windows.cmake`.

## Validation

Run the staged smoke test:

```powershell
powershell.exe -ExecutionPolicy Bypass -NoProfile `
  -File .\windows-build\scripts\smoke-test-mengshee-stage.ps1 `
  -PdfPath .\autotests\data\file2.pdf
```

Quick file checks:

```powershell
$stage = "C:\Users\jairy\Documents\okular\windows_build\dist\mengshee-pdf\app"
Test-Path "$stage\bin\data\mengshee\pics\annotation-latex-note.svg"
Test-Path "$stage\bin\data\locale\zh_CN\LC_MESSAGES\okular.mo"
Test-Path "$stage\share\poppler\cMap\Adobe-GB1\UniGB-UTF16-H"
Test-Path "$stage\bin\data\icons\hicolor\256x256\apps\mengshee.png"
```

## SDK Bootstrap

The standalone build uses a local SDK prefix for ECM, KF6 modules, zlib,
freetype, libintl shim, helper tools, and custom Poppler.

Bootstrap ECM:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DQT_PREFIX=C:/Qt/6.11.1/msvc2022_64 `
  -DVCVARS="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat" `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -P windows-build/cmake/bootstrap-kf6-sdk.cmake
```

Build support libraries and tools:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -P windows-build/cmake/install-gettext-native-sdk.cmake

& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -P windows-build/cmake/install-winflexbison-sdk.cmake

& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DQT_PREFIX=C:/Qt/6.11.1/msvc2022_64 `
  -DVCVARS="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat" `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -P windows-build/cmake/build-zlib-sdk.cmake

& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DQT_PREFIX=C:/Qt/6.11.1/msvc2022_64 `
  -DVCVARS="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat" `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -P windows-build/cmake/build-freetype-sdk.cmake

& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DQT_PREFIX=C:/Qt/6.11.1/msvc2022_64 `
  -DVCVARS="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat" `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -P windows-build/cmake/build-libintl-shim-sdk.cmake
```

Build required KF6 modules into `..\windows_build\sdk` with the CMake module
driver:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DMODULE=kcoreaddons `
  -DQT_PREFIX=C:/Qt/6.11.1/msvc2022_64 `
  -DVCVARS="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat" `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -P windows-build/cmake/build-kf6-module.cmake
```

Use `windows-build\KF6_SDK_BOOTSTRAP.md` for the module list and per-module
notes.

Build custom Poppler:

```powershell
& "C:\Qt\Tools\CMake_64\bin\cmake.exe" `
  -DQT_PREFIX=C:/Qt/6.11.1/msvc2022_64 `
  -DVCVARS="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat" `
  -DWORKSPACE_ROOT=C:/Users/jairy/Documents/okular/windows_build `
  -P windows-build/cmake/build-poppler-sdk.cmake
```

The command builds the pinned local fork in `external/poppler`. When that
submodule changes, its installed libraries, generated private headers, and the
Mengshee PDF generator must be rebuilt together. The generic page-sequence
editor is compiled into the generator even though the Poppler SDK build uses
`ENABLE_UTILS=OFF`. Maintenance details are in
`docs/local-poppler-fork.md`.

`install-gettext-native-sdk.cmake` installs pinned native Windows
`gettext-iconv-windows` tools under
`..\windows_build\sdk\tools\gettext-native`. Mengshee uses that `msgfmt.exe` to
compile real gettext catalogs; the libintl shim is only the runtime
compatibility layer needed by KI18n.

## LaTeX Notes

Mengshee supports the StemTeX renderer for LaTeX notes. The standalone Windows
runtime bundles the StemTeX renderer binaries under `StemTeX\runtime\bin` with
only the maintained `unicodemath` base profile under
`StemTeX\gui\profiles\unicodemath`; Mengshee starts that renderer during
application startup.

The annotation settings page launches the bundled StemTeX Profile Creator with
Mengshee-owned paths. The bundled base profile and user profiles have distinct
stable IDs in the selector. User-created profiles are stored below Mengshee's Qt
`AppLocalDataLocation`, under `StemTeX\profiles`; they are never written to the
installation or StemTeX's global user-profile directory.

The TeXLive package/font tree is optional. The single Mengshee installer carries
it as the `Bundled StemTeX TeX tree` component and selects it in the recommended
installation type. Users can omit that component and select their own TeX tree
from `Settings -> Configure Mengshee -> Annotations`. An empty setting does not
probe the system for TeX; it only uses the bundled tree when that component was
installed.

StemTeX runtime state, fontconfig files, caches, traces, rendered-note outputs,
and temporary LaTeX appearance files are written below Mengshee's Qt
`AppLocalDataLocation`. This keeps `C:\Program Files\Mengshee` read-only and
keeps Mengshee data out of StemTeX's standalone application directories.

To inspect TeX rendering logs:

```powershell
Get-Content "$env:LOCALAPPDATA\mengshee\mengshee-tex-debug.log" -Tail 80
```
