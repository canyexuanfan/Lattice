$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot "build.ps1"
$projectFile = Join-Path $projectRoot "Lattice.vcxproj"
$releaseDir = Join-Path $projectRoot "x64\Release"
$releaseOutputDir = Join-Path $projectRoot "release"
$installerScript = Join-Path $projectRoot "installer\Lattice.iss"

foreach ($requiredPath in @($projectFile, $installerScript)) {
    if (!(Test-Path -LiteralPath $requiredPath)) {
        throw "Required packaging input not found: $requiredPath"
    }
}

$versionDefinition = Select-String -LiteralPath $installerScript -Pattern '^#define MyAppVersion "([^"]+)"' | Select-Object -First 1
if ($null -eq $versionDefinition -or $versionDefinition.Line -notmatch '^#define MyAppVersion "([^"]+)"') {
    throw "Unable to read MyAppVersion from $installerScript"
}
$installerVersion = $Matches[1]
$expectedBinaryVersion = [version]("$installerVersion.0")

$installerSource = Get-Content -LiteralPath $installerScript -Raw
if ($installerSource -match 'WaitForProductExit\s*\(\s*\d{3,}\s*\)') {
    throw "Installer contains a minute-scale product exit wait."
}
$pollMatch = [regex]::Match($installerSource, 'ProductExitPollMilliseconds\s*=\s*(\d+)\s*;')
$messageAttemptsMatch = [regex]::Match($installerSource, 'ProductExitMessageWaitAttempts\s*=\s*(\d+)\s*;')
$closeAttemptsMatch = [regex]::Match($installerSource, 'ProductExitCloseWaitAttempts\s*=\s*(\d+)\s*;')
if (-not $pollMatch.Success -or -not $messageAttemptsMatch.Success -or -not $closeAttemptsMatch.Success) {
    throw "Installer product exit timing constants are missing."
}
$productExitPollMilliseconds = [int]$pollMatch.Groups[1].Value
$productExitMessageAttempts = [int]$messageAttemptsMatch.Groups[1].Value
$productExitCloseAttempts = [int]$closeAttemptsMatch.Groups[1].Value
$maximumProductExitWaitMilliseconds =
    $productExitPollMilliseconds * ($productExitMessageAttempts + $productExitCloseAttempts)
if (
    $productExitPollMilliseconds -le 0 -or
    $productExitMessageAttempts -le 0 -or
    $productExitCloseAttempts -le 0 -or
    $maximumProductExitWaitMilliseconds -gt 5000
) {
    throw "Installer product exit wait exceeds the 5-second UX budget: $maximumProductExitWaitMilliseconds ms."
}

& $buildScript -Configuration Release
$releaseExe = Join-Path $releaseDir "Lattice.exe"
if (!(Test-Path -LiteralPath $releaseExe)) {
    throw "Release executable not found: $releaseExe"
}

$legacyReleaseArtifacts = @(
    (Join-Path $releaseDir "Luno.exe"),
    (Join-Path $releaseDir "Luno.pdb"),
    (Join-Path $releaseDir "DesktopOrganizer.exe"),
    (Join-Path $releaseDir "DesktopOrganizer.pdb")
)
foreach ($legacyArtifact in $legacyReleaseArtifacts) {
    if (Test-Path -LiteralPath $legacyArtifact) {
        throw "Legacy release artifact must not be packaged: $legacyArtifact"
    }
}

$releaseVersionInfo = (Get-Item -LiteralPath $releaseExe).VersionInfo
$releaseFileVersionString = [version]$releaseVersionInfo.FileVersion
$releaseProductVersionString = [version]$releaseVersionInfo.ProductVersion
$releaseFileVersionFixed = [version]::new(
    $releaseVersionInfo.FileMajorPart,
    $releaseVersionInfo.FileMinorPart,
    $releaseVersionInfo.FileBuildPart,
    $releaseVersionInfo.FilePrivatePart)
$releaseProductVersionFixed = [version]::new(
    $releaseVersionInfo.ProductMajorPart,
    $releaseVersionInfo.ProductMinorPart,
    $releaseVersionInfo.ProductBuildPart,
    $releaseVersionInfo.ProductPrivatePart)
if (
    $releaseFileVersionString -ne $expectedBinaryVersion -or
    $releaseProductVersionString -ne $expectedBinaryVersion -or
    $releaseFileVersionFixed -ne $expectedBinaryVersion -or
    $releaseProductVersionFixed -ne $expectedBinaryVersion
) {
    throw "Lattice.exe version mismatch. Expected $expectedBinaryVersion, file-string=$releaseFileVersionString, product-string=$releaseProductVersionString, file-fixed=$releaseFileVersionFixed, product-fixed=$releaseProductVersionFixed"
}

$isccPath = $null
$customIscc = $env:LATTICE_ISCC
if ([string]::IsNullOrWhiteSpace($customIscc)) {
    $customIscc = $env:LUNO_ISCC
}
if ([string]::IsNullOrWhiteSpace($customIscc)) {
    $customIscc = $env:DESKTOP_ORGANIZER_ISCC
}
if (-not [string]::IsNullOrWhiteSpace($customIscc) -and (Test-Path -LiteralPath $customIscc)) {
    $isccPath = (Resolve-Path -LiteralPath $customIscc).Path
}
$isccCommand = Get-Command iscc.exe -ErrorAction SilentlyContinue
if ($null -eq $isccPath -and $null -ne $isccCommand) {
    $isccPath = $isccCommand.Source
}
if ($null -eq $isccPath) {
    $candidatePaths = @(
        "E:\Program\Inno Setup 7\ISCC.exe",
        "E:\Program\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )
    foreach ($candidate in $candidatePaths) {
        if (Test-Path -LiteralPath $candidate) {
            $isccPath = $candidate
            break
        }
    }
}

if ($null -eq $isccPath) {
    throw "ISCC.exe was not found. Install Inno Setup before packaging $installerScript."
}

& $isccPath $installerScript
if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compilation failed with exit code $LASTEXITCODE"
}

$versionedInstaller = Join-Path $releaseOutputDir "Lattice-Setup-$installerVersion.exe"
if (!(Test-Path -LiteralPath $versionedInstaller)) {
    throw "Installer output not found: $versionedInstaller"
}

$installerVersionInfo = (Get-Item -LiteralPath $versionedInstaller).VersionInfo
$installerFileVersionText = $installerVersionInfo.FileVersion.Trim()
$installerProductVersionText = $installerVersionInfo.ProductVersion.Trim()
if ([string]::IsNullOrWhiteSpace($installerFileVersionText)) {
    throw "Installer FileVersion is empty."
}
$installerFileVersionString = [version]$installerFileVersionText
$installerFileVersionFixed = [version]::new(
    $installerVersionInfo.FileMajorPart,
    $installerVersionInfo.FileMinorPart,
    $installerVersionInfo.FileBuildPart,
    $installerVersionInfo.FilePrivatePart)
if (
    $installerFileVersionString -ne $expectedBinaryVersion -or
    $installerFileVersionFixed -ne $expectedBinaryVersion -or
    $installerProductVersionText -ne $installerVersion
) {
    throw "Installer version mismatch. Expected file=$expectedBinaryVersion and product=$installerVersion, file-string=$installerFileVersionString, file-fixed=$installerFileVersionFixed, product=$installerProductVersionText"
}
if (
    $installerVersionInfo.ProductName.Trim() -ne "Lattice" -or
    $installerVersionInfo.FileDescription.Trim() -ne "Lattice Setup"
) {
    throw "Installer branding mismatch. ProductName=$($installerVersionInfo.ProductName), FileDescription=$($installerVersionInfo.FileDescription)"
}

$latestInstaller = Join-Path $releaseOutputDir "Lattice-Setup-Latest.exe"
Copy-Item -LiteralPath $versionedInstaller -Destination $latestInstaller -Force

$releaseItem = Get-Item -LiteralPath $releaseExe
$versionedInstallerItem = Get-Item -LiteralPath $versionedInstaller
if ($versionedInstallerItem.LastWriteTimeUtc -lt $releaseItem.LastWriteTimeUtc) {
    throw "Installer is older than the packaged Lattice.exe."
}

$versionedHash = (Get-FileHash -LiteralPath $versionedInstaller -Algorithm SHA256).Hash
$latestHash = (Get-FileHash -LiteralPath $latestInstaller -Algorithm SHA256).Hash
if ($versionedHash -ne $latestHash) {
    throw "Latest installer hash does not match the versioned installer."
}

Write-Host "Installer built: $versionedInstaller"
Write-Host "Latest installer alias: $latestInstaller"
Write-Host "Installer SHA256: $versionedHash"
