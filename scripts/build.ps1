param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug"
)

$ErrorActionPreference = "Stop"

$buildWrapper = Join-Path $PSScriptRoot "build-clean.cmd"
if (!(Test-Path -LiteralPath $buildWrapper)) {
    throw "Build wrapper not found: $buildWrapper"
}

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$buildOutputRoot = [System.IO.Path]::GetFullPath((Join-Path $projectRoot "x64"))
$configurationOutput = [System.IO.Path]::GetFullPath((Join-Path $buildOutputRoot $Configuration))
$buildOutputPrefix = $buildOutputRoot + [System.IO.Path]::DirectorySeparatorChar
if (!$configurationOutput.StartsWith($buildOutputPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean an output directory outside the project x64 root: $configurationOutput"
}

$legacyArtifacts = @(
    (Join-Path $configurationOutput "Luno.exe"),
    (Join-Path $configurationOutput "Luno.pdb"),
    (Join-Path $configurationOutput "DesktopOrganizer.exe"),
    (Join-Path $configurationOutput "DesktopOrganizer.pdb")
)
foreach ($legacyArtifact in $legacyArtifacts) {
    if (Test-Path -LiteralPath $legacyArtifact) {
        Remove-Item -LiteralPath $legacyArtifact -Force
    }
}

$intermediateDirectory = [System.IO.Path]::GetFullPath((Join-Path $configurationOutput "obj"))
$configurationPrefix = $configurationOutput + [System.IO.Path]::DirectorySeparatorChar
if (!$intermediateDirectory.StartsWith($configurationPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean an intermediate directory outside the configuration output: $intermediateDirectory"
}
if (Test-Path -LiteralPath $intermediateDirectory) {
    Remove-Item -LiteralPath $intermediateDirectory -Recurse -Force
}

# PowerShell hosts can expose both PATH and Path. Start the build in a
# child environment containing exactly one PATH key, while the wrapper sets a
# writable task-local TEMP/TMP before loading the Visual Studio environment.
$pathValue = $env:PATH
if ([string]::IsNullOrWhiteSpace($pathValue)) {
    $pathValue = [Environment]::GetEnvironmentVariable("Path", "Machine")
}

$processInfo = [System.Diagnostics.ProcessStartInfo]::new()
$processInfo.FileName = Join-Path $env:SystemRoot "System32\cmd.exe"
$processInfo.WorkingDirectory = $PSScriptRoot
$processInfo.UseShellExecute = $false
if ($null -ne $processInfo.ArgumentList) {
    $processInfo.ArgumentList.Add("/d")
    $processInfo.ArgumentList.Add("/c")
    $processInfo.ArgumentList.Add("build-clean.cmd")
    $processInfo.ArgumentList.Add($Configuration)
} else {
    $processInfo.Arguments = "/d /c build-clean.cmd $Configuration"
}

$processEnvironment = $processInfo.Environment
if ($null -eq $processEnvironment) {
    $processEnvironment = $processInfo.EnvironmentVariables
}
$processEnvironment.Clear()
Get-ChildItem Env: | ForEach-Object {
    if ($_.Name -ine "Path") {
        $processEnvironment[$_.Name] = $_.Value
    }
}
$processEnvironment["PATH"] = $pathValue

$process = [System.Diagnostics.Process]::Start($processInfo)
$process.WaitForExit()
if ($process.ExitCode -ne 0) {
    throw "MSBuild failed with exit code $($process.ExitCode)"
}

foreach ($legacyArtifact in $legacyArtifacts) {
    if (Test-Path -LiteralPath $legacyArtifact) {
        throw "Legacy build artifact still exists after the Lattice build: $legacyArtifact"
    }
}
