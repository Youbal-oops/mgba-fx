# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
param(
    [string]$Rom,
    [switch]$TimingExperiment,
    [switch]$RAMDiagnostic,
    [switch]$ContentPrototype,
    [ValidateRange(1, 100)][int]$MemoryPercent = 80,
    [ValidateRange(0, 1048576)][int]$MemoryMiB = 0,
    [ValidateRange(1, 100)][int]$CPUPercent = 80
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $repoRoot $(if ($ContentPrototype) { 'build-fx/content' } elseif ($TimingExperiment) { 'build-fx/timing' } else { 'build-fx' })
$qtRoot = Join-Path $repoRoot 'build-tools/qt/6.8.3/mingw_64'
$compilerBin = Join-Path $repoRoot 'build-tools/qt/Tools/mingw1310_64/bin'
$depsRoot = Join-Path $repoRoot 'build-tools/deps'
if (-not $Rom) {
    $Rom = Join-Path $buildRoot $(if ($ContentPrototype) { 'diagnostics/content-64.gba' } elseif ($RAMDiagnostic) { 'diagnostics/ram.gba' } elseif ($TimingExperiment) { 'diagnostics/timing-2x.gba' } else { 'diagnostics/resources.gba' })
}
if (-not (Test-Path -LiteralPath (Join-Path $buildRoot 'mGBA.exe')) -or -not (Test-Path -LiteralPath $Rom)) {
    throw 'Build the emulator and generate the diagnostics with tools/fx-build.ps1 first.'
}
$oldPath = $env:PATH
$oldPluginPath = $env:QT_PLUGIN_PATH
$oldPlatform = $env:QT_QPA_PLATFORM
try {
    $env:PATH = $buildRoot + ';' + $compilerBin + ';' + (Join-Path $depsRoot 'bin') + ';' + (Join-Path $depsRoot 'mingw64/bin') + ';' + (Join-Path $qtRoot 'bin') + ';' + $oldPath
    $env:QT_PLUGIN_PATH = Join-Path $qtRoot 'plugins'
    $env:QT_QPA_PLATFORM = 'windows'
    $arguments = @('-C', 'skipBios=1', '-C', 'useBios=0')
    if ($TimingExperiment -or $RAMDiagnostic -or $ContentPrototype) {
        $arguments += @('-C', 'rewindEnable=0')
    }
    if ($RAMDiagnostic -or $ContentPrototype) {
        $arguments += @('-C', 'audioSync=0', '-C', 'videoSync=1', '-C', 'hwaccelVideo=0')
    }
    if ($PSBoundParameters.ContainsKey('MemoryPercent')) {
        $arguments += @('-C', "gba.fxMemoryPercent=$MemoryPercent")
    }
    if ($PSBoundParameters.ContainsKey('MemoryMiB')) {
        $arguments += @('-C', "gba.fxMemoryMiB=$MemoryMiB")
    }
    if ($PSBoundParameters.ContainsKey('CPUPercent')) {
        $arguments += @('-C', "gba.fxCPUPercent=$CPUPercent")
    }
    $arguments += $Rom
    & (Join-Path $buildRoot 'mGBA.exe') @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "mGBA exited with code $LASTEXITCODE"
    }
} finally {
    $env:PATH = $oldPath
    $env:QT_PLUGIN_PATH = $oldPluginPath
    $env:QT_QPA_PLATFORM = $oldPlatform
}
