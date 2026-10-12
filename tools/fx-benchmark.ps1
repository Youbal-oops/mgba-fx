# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
[CmdletBinding()]
param(
    [string]$OutputDirectory = 'build-fx/baseline',
    [string]$BuildDirectory = 'build-fx',
    [ValidateRange(1, 1000000)][int]$Frames = 600,
    [ValidateRange(1, 20)][int]$Runs = 3
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$oldPath = $env:PATH
try {
    Push-Location $repoRoot
    $buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDirectory))
    if (-not $buildRoot.StartsWith($repoRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Benchmark binaries must be inside the repository.'
    }
    $env:PATH = "$buildRoot;$repoRoot/build-tools/qt/Tools/mingw1310_64/bin;$repoRoot/build-tools/deps/bin;$repoRoot/build-tools/deps/mingw64/bin;$env:PATH"
    $outputRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot $OutputDirectory))
    if (-not $outputRoot.StartsWith($repoRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Benchmark output must be inside the repository.'
    }
    New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
    $measurements = @()
    $cases = @(
        'cinema/gba/irq/keyirq/test.gba',
        'cinema/gba/obj/2d-wrap/test.gba',
        'cinema/gba/blend/disabled-bg-semitrans-blend/test.gba'
    )
    foreach ($rom in $cases) {
        $caseName = Split-Path -Leaf (Split-Path -Parent $rom)
        for ($run = 1; $run -le $Runs; $run++) {
            $stdout = Join-Path $outputRoot "$caseName-$run.csv"
            $stderr = Join-Path $outputRoot "$caseName-$run.stderr.log"
            $timer = [Diagnostics.Stopwatch]::StartNew()
            $process = Start-Process -FilePath (Join-Path $buildRoot 'mgba-perf.exe') -ArgumentList @('-P', '-F', "$Frames", $rom) -WorkingDirectory $repoRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
            # Retain the handle so Windows PowerShell can read a short-lived exit code.
            $processHandle = $process.Handle
            $sampledPeak = 0L
            while (-not $process.HasExited) {
                try {
                    $process.Refresh()
                    $sampledPeak = [Math]::Max($sampledPeak, $process.WorkingSet64)
                } catch [InvalidOperationException] {
                    # A short run can exit between the status check and the sample.
                }
                Start-Sleep -Milliseconds 20
            }
            $process.WaitForExit()
            $timer.Stop()
            if ($process.ExitCode -ne 0) {
                throw "Benchmark failed for $rom. See $stderr"
            }
            $csv = Get-Content -LiteralPath $stdout | Where-Object { $_ -match '^[^,]+,[0-9]+,[0-9]+,software$' } | Select-Object -Last 1
            if (-not $csv) { throw "Missing performance result in $stdout" }
            $fields = $csv.Split(',')
            $measurements += [ordered]@{
                rom = $rom; run = $run; frames = [int]$fields[1]
                emulationMicroseconds = [long]$fields[2]
                wallSecondsIncludingStartup = $timer.Elapsed.TotalSeconds
                sampledPeakWorkingSetBytes = $sampledPeak
                romSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $rom).Hash
            }
            Write-Output "$caseName run ${run}: $($fields[2]) us; sampled working set $sampledPeak bytes"
        }
    }
    $machine = Get-CimInstance Win32_ComputerSystem
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $report = [ordered]@{
        timestampUtc = [DateTime]::UtcNow.ToString('o')
        startingCommit = (& git rev-parse HEAD)
        coreDllSha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $buildRoot 'libmgba.dll')).Hash
        configuration = 'RelWithDebInfo, GCC 13.1, LTO OFF, software renderer'
        host = @{ cpu = $cpu.Name; logicalProcessors = $machine.NumberOfLogicalProcessors; physicalMemoryBytes = $machine.TotalPhysicalMemory }
        memorySamplingIntervalMs = 20
        measurements = $measurements
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputRoot 'performance.json') -Encoding UTF8
} finally {
    Pop-Location
    $env:PATH = $oldPath
}
