# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
[CmdletBinding()]
param(
    [switch]$Bootstrap,
    [switch]$SkipQt,
    [switch]$SkipTests,
    [switch]$Fresh,
    [switch]$TimingExperiment,
    [switch]$ContentPrototype,
    [switch]$AdaptivePrototype,
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string]$Configuration = 'RelWithDebInfo',
    [ValidateRange(1, 64)]
    [int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$toolsRoot = Join-Path $repoRoot 'build-tools'
$pythonRoot = Join-Path $toolsRoot 'python'
$qtRoot = Join-Path $toolsRoot 'qt/6.8.3/mingw_64'
$compilerBin = Join-Path $toolsRoot 'qt/Tools/mingw1310_64/bin'
$depsRoot = Join-Path $toolsRoot 'deps'
$epoxyRoot = Join-Path $depsRoot 'mingw64'
$cmockaSource = Join-Path $toolsRoot 'cmocka-1.1.8'
$cmockaBuild = Join-Path $toolsRoot 'cmocka-build'
$buildRoot = Join-Path $repoRoot $(if ($AdaptivePrototype) { 'build-fx/adaptive' } elseif ($ContentPrototype) { 'build-fx/content' } elseif ($TimingExperiment) { 'build-fx/timing' } else { 'build-fx' })
$cmake = Join-Path $pythonRoot 'cmake/data/bin/cmake.exe'
$ctest = Join-Path $pythonRoot 'cmake/data/bin/ctest.exe'
$ninja = Join-Path $pythonRoot 'bin/ninja.exe'
$oldPath = $env:PATH
$oldPythonPath = $env:PYTHONPATH
$oldQtPluginPath = $env:QT_PLUGIN_PATH
$oldQtPlatform = $env:QT_QPA_PLATFORM

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $Program $($Arguments -join ' ')"
    }
}

try {
    Push-Location $repoRoot
    $env:PYTHONPATH = $pythonRoot
    if ($oldPythonPath) {
        $env:PYTHONPATH += [IO.Path]::PathSeparator + $oldPythonPath
    }

    if ($Bootstrap) {
        New-Item -ItemType Directory -Force -Path $toolsRoot | Out-Null
        Invoke-Checked 'python' @(
            '-m', 'pip', 'install', '--disable-pip-version-check', '--no-cache-dir',
            '--target', $pythonRoot, '-r', (Join-Path $PSScriptRoot 'fx-build-requirements.txt')
        )

        # Keep aqt's logs and temporary packages in the ignored tools directory.
        Push-Location $toolsRoot
        try {
            $qtToolsRoot = Join-Path $toolsRoot 'qt'
            $downloads = Join-Path $toolsRoot 'downloads'
            New-Item -ItemType Directory -Force -Path $downloads | Out-Null
            if (-not (Test-Path -LiteralPath (Join-Path $compilerBin 'gcc.exe'))) {
                Invoke-Checked 'python' @(
                    '-m', 'aqt', 'install-tool', 'windows', 'desktop',
                    'tools_mingw1310', 'qt.tools.win64_mingw1310',
                    '--outputdir', $qtToolsRoot, '--base', 'https://master.qt.io',
                    '--archive-dest', $downloads
                )
            }
            if (-not $SkipQt -and -not (Test-Path -LiteralPath (Join-Path $qtRoot 'lib/cmake/Qt6Multimedia/Qt6MultimediaConfig.cmake'))) {
                Invoke-Checked 'python' @(
                    '-m', 'aqt', 'install-qt', 'windows', 'desktop', '6.8.3', 'win64_mingw',
                    '--outputdir', $qtToolsRoot, '--modules', 'qtmultimedia',
                    '--base', 'https://master.qt.io', '--archive-dest', $downloads
                )
            }

            $archive = Join-Path $downloads 'cmocka-1.1.8.tar.xz'
            $expectedHash = '58435B558766D7F4C729BA163BDF3AEC38BED3BC766DAB684E3526ED0AA7C780'
            if (-not (Test-Path -LiteralPath $archive)) {
                Invoke-WebRequest -UseBasicParsing -Uri 'https://cmocka.org/files/1.1/cmocka-1.1.8.tar.xz' -OutFile $archive
            }
            if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $expectedHash) {
                throw 'cmocka archive checksum does not match the pinned source.'
            }
            if (-not (Test-Path -LiteralPath (Join-Path $cmockaSource 'CMakeLists.txt'))) {
                Invoke-Checked 'python' @(
                    '-c', "import sys,tarfile; tarfile.open(sys.argv[1]).extractall(sys.argv[2], filter='data')",
                    $archive, $toolsRoot
                )
            }

            $nativePackages = @(
                @{ File = 'mingw-w64-x86_64-libepoxy-1.5.10-7-any.pkg.tar.zst'; Hash = 'A2DCA7211C2C6A3BB335C074167DB9929F62DA55D566DF29AEA26E5E88F35BFC'; Library = 'bin/libepoxy-0.dll' },
                @{ File = 'mingw-w64-x86_64-zlib-1.3.2-2-any.pkg.tar.zst'; Hash = '9E75842A070BA648E986E12424E1C92C9D7D77200E85F6A34EEB600819F2E694'; Library = 'bin/zlib1.dll' },
                @{ File = 'mingw-w64-x86_64-libpng-1.6.59-1-any.pkg.tar.zst'; Hash = 'E1830319EA6C601C1E4D146B8E78264BCBB6326F0935B82C14A9B282AF2127E1'; Library = 'bin/libpng16-16.dll' }
            )
            foreach ($nativePackage in $nativePackages) {
                $nativeArchive = Join-Path $downloads $nativePackage.File
                if (-not (Test-Path -LiteralPath $nativeArchive)) {
                    Invoke-WebRequest -UseBasicParsing -Uri ('https://repo.msys2.org/mingw/mingw64/' + $nativePackage.File) -OutFile $nativeArchive
                }
                if ((Get-FileHash -Algorithm SHA256 -LiteralPath $nativeArchive).Hash -ne $nativePackage.Hash) {
                    throw "Dependency checksum mismatch: $($nativePackage.File)"
                }
                if (-not (Test-Path -LiteralPath (Join-Path $epoxyRoot $nativePackage.Library))) {
                    Invoke-Checked 'python' @(
                        '-c', "import sys,tarfile; from backports import zstd; tarfile.open(fileobj=zstd.ZstdFile(sys.argv[1]), mode='r|').extractall(sys.argv[2], filter='data')",
                        $nativeArchive, $depsRoot
                    )
                }
            }
        } finally {
            Pop-Location
        }
    }

    $requiredFiles = @(
        $cmake, $ctest, $ninja,
        (Join-Path $compilerBin 'gcc.exe'),
        (Join-Path $compilerBin 'g++.exe'),
        (Join-Path $cmockaSource 'CMakeLists.txt'),
        (Join-Path $epoxyRoot 'include/epoxy/gl.h'),
        (Join-Path $epoxyRoot 'bin/libepoxy-0.dll'),
        (Join-Path $epoxyRoot 'bin/zlib1.dll'),
        (Join-Path $epoxyRoot 'bin/libpng16-16.dll')
    )
    if (-not $SkipQt) {
        $requiredFiles += Join-Path $qtRoot 'lib/cmake/Qt6Multimedia/Qt6MultimediaConfig.cmake'
    }
    foreach ($requiredFile in $requiredFiles) {
        if (-not (Test-Path -LiteralPath $requiredFile)) {
            throw "Missing portable dependency: $requiredFile. Run tools/fx-build.ps1 -Bootstrap first."
        }
    }

    $env:PATH = $buildRoot + ';' + $compilerBin + ';' + (Join-Path $depsRoot 'bin') + ';' + (Join-Path $epoxyRoot 'bin') + ';' + (Join-Path $qtRoot 'bin') + ';' + $oldPath
    $env:QT_PLUGIN_PATH = Join-Path $qtRoot 'plugins'
    $env:QT_QPA_PLATFORM = 'offscreen'

    Invoke-Checked $cmake @(
        '-S', $cmockaSource, '-B', $cmockaBuild, '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DCMAKE_C_COMPILER=$(Join-Path $compilerBin 'gcc.exe')",
        "-DCMAKE_AR=$(Join-Path $compilerBin 'gcc-ar.exe')",
        "-DCMAKE_RANLIB=$(Join-Path $compilerBin 'gcc-ranlib.exe')",
        "-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_INSTALL_PREFIX=$depsRoot",
        '-DCMAKE_INSTALL_LIBDIR=lib', '-DCMAKE_INSTALL_BINDIR=bin',
        '-DBUILD_SHARED_LIBS=ON', '-DUNIT_TESTING=OFF', '-DWITH_EXAMPLES=OFF'
    )
    Invoke-Checked $cmake @('--build', $cmockaBuild, '--parallel', "$Jobs")
    Invoke-Checked $cmake @('--install', $cmockaBuild)

    $qtEnabled = if ($SkipQt) { 'OFF' } else { 'ON' }
    $timingEnabled = if ($TimingExperiment) { 'ON' } else { 'OFF' }
    $configureArguments = @(
        '-S', $repoRoot, '-B', $buildRoot, '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$ninja",
        "-DCMAKE_C_COMPILER=$(Join-Path $compilerBin 'gcc.exe')",
        "-DCMAKE_CXX_COMPILER=$(Join-Path $compilerBin 'g++.exe')",
        "-DCMAKE_AR=$(Join-Path $compilerBin 'gcc-ar.exe')",
        "-DCMAKE_RANLIB=$(Join-Path $compilerBin 'gcc-ranlib.exe')",
        "-DCMAKE_BUILD_TYPE=$Configuration", '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DBUILD_LTO=OFF',
        "-DBUILD_FX_TIMING_EXPERIMENT=$timingEnabled",
        "-DCMAKE_PREFIX_PATH=$qtRoot;$depsRoot;$epoxyRoot",
        "-DCMOCKA_INCLUDE_DIRS=$(Join-Path $depsRoot 'include')",
        "-DCMOCKA_LIBRARY_DIRS=$(Join-Path $depsRoot 'lib')",
        "-DBUILD_QT=$qtEnabled", '-DFORCE_QT_VERSION=6', '-DBUILD_SDL=OFF',
        '-DBUILD_SUITE=ON', '-DBUILD_TEST=ON', '-DBUILD_CINEMA=ON',
        '-DBUILD_HEADLESS=ON', '-DBUILD_PERF=ON',
        '-DBUILD_GL=ON', '-DBUILD_GLES2=ON', '-DBUILD_GLES3=ON',
        '-DUSE_FFMPEG=OFF', '-DUSE_MINIZIP=OFF', '-DUSE_LIBZIP=OFF',
        '-DUSE_EPOXY=ON', '-DUSE_ELF=OFF', '-DUSE_LUA=OFF',
        '-DUSE_JSON_C=OFF', '-DUSE_FREETYPE=OFF', '-DUSE_DISCORD_RPC=OFF'
    )
    if ($Fresh) {
        $configureArguments = @('--fresh') + $configureArguments
    }
    Invoke-Checked $cmake $configureArguments

    # Fail rather than claiming test coverage when dependency detection disables it.
    $generatedTests = Join-Path $buildRoot 'test/CTestTestfile.cmake'
    if (-not (Test-Path -LiteralPath $generatedTests) -or
        -not (Select-String -LiteralPath $generatedTests -Pattern 'gba-core' -Quiet)) {
        throw 'The GBA unit suite was not configured. Check cmocka detection.'
    }

    Invoke-Checked $cmake @('--build', $buildRoot, '--parallel', "$Jobs")
    if (-not $SkipTests) {
        Invoke-Checked $ctest @('--test-dir', $buildRoot, '--output-on-failure', '-E', '^cinema$')
        $cinema = Join-Path $buildRoot 'test/mgba-cinema.exe'
        if (Test-Path -LiteralPath $cinema) {
            $generateArguments = @((Join-Path $PSScriptRoot 'fx-generate-diagnostic.py'), '--output-dir', (Join-Path $buildRoot 'diagnostics'))
            if ($TimingExperiment) {
                $generateArguments += '--timing'
            }
            if ($ContentPrototype) {
                $generateArguments += '--content'
            }
            if ($AdaptivePrototype) {
                $generateArguments += '--adaptive'
            }
            Invoke-Checked 'python' $generateArguments
            $fixtures = Join-Path $buildRoot 'diagnostics'
            $verifyArguments = @((Join-Path $PSScriptRoot 'fx-check-roms.py'), '--cinema', $cinema, '--fixtures', $fixtures)
            if ($TimingExperiment) {
                $verifyArguments += '--timing'
            }
            if ($ContentPrototype) {
                $verifyArguments += '--content'
            }
            if ($AdaptivePrototype) {
                $verifyArguments += '--adaptive'
            }
            Invoke-Checked 'python' $verifyArguments
        }
    }
    Write-Output "Build complete: $buildRoot"
    Write-Output "Run CInema separately: $buildRoot/test/mgba-cinema.exe -b $repoRoot/cinema -v"
} finally {
    Pop-Location
    $env:PATH = $oldPath
    $env:PYTHONPATH = $oldPythonPath
    $env:QT_PLUGIN_PATH = $oldQtPluginPath
    $env:QT_QPA_PLATFORM = $oldQtPlatform
}
