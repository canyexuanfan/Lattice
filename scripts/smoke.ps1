param(
    [ValidateSet(
        "--smoke-scan",
        "--smoke-resource-idle",
        "--smoke-config",
        "--smoke-layout",
        "--smoke-shell-new",
        "--smoke-managed-items",
        "--smoke-legacy-storage-migration",
        "--smoke-category-storage",
        "--smoke-shortcut-overlay",
        "--smoke-widget-alignment",
        "--smoke-widget-desktop-layer",
        "--smoke-desktop-icon-fidelity",
        "--smoke-desktop-display-takeover",
        "--smoke-collapse-selection-logic",
        "--smoke-widget-interaction",
        "--smoke-widget-normal-exit",
        "--smoke-widget-drop-latency",
        "--smoke-widget-drop-placement",
        "--smoke-update-dialog")]
    [string]$Mode = "",
    [switch]$SkipBuild,
    [switch]$VisibleFixture
)

$ErrorActionPreference = "Stop"

if (!$SkipBuild) {
    & "$PSScriptRoot\build.ps1"
}
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$debugRoot = (Resolve-Path -LiteralPath (Join-Path $projectRoot "x64\Debug")).Path
$exe = Join-Path $debugRoot "Lattice.exe"
if (!(Test-Path -LiteralPath $exe)) {
    throw "Executable not found: $exe"
}
$hangCapture = Join-Path $projectRoot '.workspace\tools\hang-capture.exe'
$hangCaptureSource = Join-Path $projectRoot 'tools\hang-capture\HangCapture.cpp'
$hangCaptureBuild = Join-Path $PSScriptRoot 'build-hang-capture.cmd'
$hangCaptureNeedsBuild = !(Test-Path -LiteralPath $hangCapture)
if (!$hangCaptureNeedsBuild) {
    $toolTimestamp = (Get-Item -LiteralPath $hangCapture).LastWriteTimeUtc
    $hangCaptureNeedsBuild =
        (Get-Item -LiteralPath $hangCaptureSource).LastWriteTimeUtc -gt $toolTimestamp -or
        (Get-Item -LiteralPath $hangCaptureBuild).LastWriteTimeUtc -gt $toolTimestamp
}
if ($hangCaptureNeedsBuild) {
    & $hangCaptureBuild
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $hangCapture)) {
        throw "Unable to build the fixed hang capture tool."
    }
}

$smokeRunsRoot = Join-Path $debugRoot "smoke-runs"
$runId = [Guid]::NewGuid().ToString("D")
$runRoot = Join-Path $smokeRunsRoot $runId
$smokeConfigDir = Join-Path $runRoot "Config"
$smokeDataDir = Join-Path $runRoot "Data"
$managedItemsSmokeDir = Join-Path $runRoot "Runs"
$runSucceeded = $false

foreach ($path in @($projectRoot, (Join-Path $projectRoot "x64"), $debugRoot)) {
    $item = Get-Item -LiteralPath $path
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing smoke path through reparse point: $path"
    }
}
New-Item -ItemType Directory -Force -Path $smokeRunsRoot | Out-Null
New-Item -ItemType Directory -Path $runRoot | Out-Null
New-Item -ItemType Directory -Path $smokeConfigDir, $smokeDataDir, $managedItemsSmokeDir | Out-Null
foreach ($path in @($smokeRunsRoot, $runRoot)) {
    $item = Get-Item -LiteralPath $path
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing smoke path through reparse point: $path"
    }
}

function Invoke-SmokeMode {
    param(
        [string]$Mode
    )

    Write-Output "SMOKE_MODE_START=$Mode"
    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $exe
    if ($psi.PSObject.Properties.Name -contains "ArgumentList") {
        $psi.ArgumentList.Add($Mode)
    } else {
        $psi.Arguments = $Mode
    }
    $psi.WorkingDirectory = Split-Path -Parent $exe
    $psi.UseShellExecute = $false
    $processEnvironment = $psi.EnvironmentVariables
    $processEnvironment["DESKTOP_ORGANIZER_CONFIG_DIR"] = $smokeConfigDir
    $processEnvironment["DESKTOP_ORGANIZER_DATA_DIR"] = $smokeDataDir
    $processEnvironment["DESKTOP_ORGANIZER_INSTANCE_SUFFIX"] = "smoke-$runId"
    $processEnvironment["DESKTOP_ORGANIZER_SMOKE_ITEMS_DIR"] = $managedItemsSmokeDir
    $processEnvironment["DESKTOP_ORGANIZER_DISABLE_AUTO_UPDATE"] = "1"
    if ($VisibleFixture) {
        if ($Mode -ne '--smoke-resource-idle') {
            throw 'VisibleFixture requires the bounded resource-idle fixture.'
        }
        $processEnvironment['DESKTOP_ORGANIZER_SMOKE_RESOURCE_VISIBLE'] = '1'
    }
    $process = [System.Diagnostics.Process]::Start($psi)
    $processId = $process.Id
    $processStartTime = $process.StartTime.ToFileTime()
    if (!$process.WaitForExit(30000)) {
        $actual = Get-Process -Id $processId -ErrorAction SilentlyContinue
        $actualPath = $null
        if ($null -ne $actual) {
            try { $actualPath = $actual.Path } catch {}
        }
        $identityMatches = $null -ne $actual -and
            [string]::Equals($actualPath, $exe, [StringComparison]::OrdinalIgnoreCase) -and
            $actual.StartTime.ToFileTime() -eq $processStartTime
        if (!$identityMatches) {
            throw "Smoke mode timed out, but exact PID/Path/StartTime no longer matches; refusing dump or termination: $Mode"
        }

        $dumpStem = $Mode.TrimStart('-') -replace '[^A-Za-z0-9._-]', '-'
        $dumpName = $dumpStem + '-hang.dmp'
        $dumpPath = Join-Path $runRoot $dumpName
        $stackPath = Join-Path $runRoot ($dumpStem + '-hang-stacks.txt')
        $dumpFailure = $null
        try {
            $dumpPsi = [System.Diagnostics.ProcessStartInfo]::new()
            $dumpPsi.FileName = $hangCapture
            if ($dumpPsi.PSObject.Properties.Name -contains 'ArgumentList') {
                $dumpPsi.ArgumentList.Add([string]$processId)
                $dumpPsi.ArgumentList.Add($dumpPath)
                $dumpPsi.ArgumentList.Add($stackPath)
                $dumpPsi.ArgumentList.Add($debugRoot)
            } else {
                $dumpPsi.Arguments = $processId.ToString() + ' "' + $dumpPath + '" "' + $stackPath + '" "' + $debugRoot + '"'
            }
            $dumpPsi.UseShellExecute = $false
            $dumpProcess = [System.Diagnostics.Process]::Start($dumpPsi)
            if (!$dumpProcess.WaitForExit(20000)) {
                $dumpFailure = 'hang dump helper exceeded 20 seconds'
            } elseif ($dumpProcess.ExitCode -ne 0 -or
                !(Test-Path -LiteralPath $dumpPath) -or
                !(Test-Path -LiteralPath $stackPath)) {
                $dumpFailure = "hang dump capture failed with exit $($dumpProcess.ExitCode)"
            }
        } catch {
            $dumpFailure = $_.Exception.Message
        } finally {
            $actual = Get-Process -Id $processId -ErrorAction SilentlyContinue
            $actualPath = $null
            if ($null -ne $actual) {
                try { $actualPath = $actual.Path } catch {}
            }
            if ($null -ne $actual -and
                [string]::Equals($actualPath, $exe, [StringComparison]::OrdinalIgnoreCase) -and
                $actual.StartTime.ToFileTime() -eq $processStartTime) {
                Stop-Process -Id $processId -Force
                [void]$process.WaitForExit(5000)
            }
        }
        if ($null -ne $dumpFailure) {
            throw "Smoke mode exceeded 30 seconds and dump failed: $Mode ($dumpFailure)"
        }
        throw "Smoke mode exceeded 30 seconds: $Mode. Hang dump: $dumpPath. Thread stacks: $stackPath"
    }
    Write-Output "SMOKE_MODE_EXIT=$Mode`:$($process.ExitCode)"
    if ($process.ExitCode -ne 0) {
        throw "Smoke mode failed: $Mode (exit $($process.ExitCode))"
    }
}

try {
    $modes = if ($Mode) {
        @($Mode)
    } else {
        @(
        "--smoke-collapse-selection-logic",
        "--smoke-scan",
        "--smoke-config",
        "--smoke-layout",
        "--smoke-shell-new",
        "--smoke-managed-items",
        "--smoke-legacy-storage-migration",
        "--smoke-category-storage",
        "--smoke-shortcut-overlay",
        "--smoke-widget-alignment",
        "--smoke-widget-desktop-layer",
        "--smoke-desktop-icon-fidelity",
        "--smoke-desktop-display-takeover",
        "--smoke-widget-interaction",
        "--smoke-widget-normal-exit",
        "--smoke-widget-drop-latency",
        "--smoke-widget-drop-placement",
        "--smoke-update-dialog")
    }
    foreach ($mode in $modes) {
        Invoke-SmokeMode $mode
    }
    $runSucceeded = $true
    Write-Output "SMOKE_STATUS=PASS"
} finally {
    if ($runSucceeded -and (Test-Path -LiteralPath $runRoot)) {
        $reparsePoint = Get-ChildItem -LiteralPath $runRoot -Recurse -Force |
            Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 } |
            Select-Object -First 1
        if ($null -ne $reparsePoint) {
            throw "Refusing to clean smoke root containing reparse point: $($reparsePoint.FullName)"
        }
        Remove-Item -LiteralPath $runRoot -Recurse -Force
        Write-Output "SMOKE_RUN_ROOT_CLEANED=1"
    } elseif (!$runSucceeded) {
        Write-Output "SMOKE_RUN_ROOT_PRESERVED=$runRoot"
    }
}
