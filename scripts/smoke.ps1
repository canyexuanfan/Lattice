$ErrorActionPreference = "Stop"

& "$PSScriptRoot\build.ps1"
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$debugRoot = (Resolve-Path -LiteralPath (Join-Path $projectRoot "x64\Debug")).Path
$exe = Join-Path $debugRoot "Lattice.exe"
if (!(Test-Path -LiteralPath $exe)) {
    throw "Executable not found: $exe"
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
    $process = [System.Diagnostics.Process]::Start($psi)
    $processId = $process.Id
    $processStartTime = $process.StartTime.ToFileTime()
    if (!$process.WaitForExit(30000)) {
        $actual = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if ($null -ne $actual) {
            $actualPath = $null
            try {
                $actualPath = $actual.Path
            } catch {
            }
            if ([string]::Equals($actualPath, $exe, [StringComparison]::OrdinalIgnoreCase) -and
                $actual.StartTime.ToFileTime() -eq $processStartTime) {
                Stop-Process -Id $processId -Force
                if (!$process.WaitForExit(5000)) {
                    throw "Smoke process did not exit after exact termination: $Mode"
                }
            }
        }
        throw "Smoke mode exceeded 30 seconds: $Mode"
    }
    Write-Output "SMOKE_MODE_EXIT=$Mode`:$($process.ExitCode)"
    if ($process.ExitCode -ne 0) {
        throw "Smoke mode failed: $Mode (exit $($process.ExitCode))"
    }
}

try {
    foreach ($mode in @(
        "--smoke-scan",
        "--smoke-config",
        "--smoke-layout",
        "--smoke-shell-new",
        "--smoke-managed-items",
        "--smoke-category-storage",
        "--smoke-shortcut-overlay",
        "--smoke-widget-alignment",
        "--smoke-widget-desktop-layer",
        "--smoke-widget-interaction",
        "--smoke-widget-normal-exit",
        "--smoke-widget-drop-latency",
        "--smoke-widget-drop-placement",
        "--smoke-update-dialog")) {
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
