param(
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$buildScript = Join-Path $PSScriptRoot "build.ps1"
$projectFile = Join-Path $projectRoot "Lattice.vcxproj"
$releaseDir = Join-Path $projectRoot "x64\Release"
$offlineReleaseDir = Join-Path $projectRoot "x64\ReleaseOffline"
$releaseOutputDir = Join-Path $projectRoot "release"
$installerScript = Join-Path $projectRoot "installer\Lattice.iss"
$signScript = Join-Path $PSScriptRoot "sign-artifacts.ps1"
$shortcutOverlayAsset = Join-Path $projectRoot "assets\branding\lattice-shortcut-overlay.ico"
$resourceScript = Join-Path $projectRoot "src\app\Lattice.rc"
$iconCacheSourcePath = Join-Path $projectRoot "src\rendering\IconCache.cpp"
$iconGridSourcePath = Join-Path $projectRoot "src\ui\IconGrid.cpp"

foreach ($requiredPath in @(
    $projectFile,
    $installerScript,
    $signScript,
    $shortcutOverlayAsset,
    $resourceScript,
    $iconCacheSourcePath,
    $iconGridSourcePath)) {
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
$resourceSource = Get-Content -LiteralPath $resourceScript -Raw
$iconCacheSource = Get-Content -LiteralPath $iconCacheSourcePath -Raw
$iconGridSource = Get-Content -LiteralPath $iconGridSourcePath -Raw
if (
    [regex]::Matches($installerSource, [regex]::Escape('@十七°')).Count -ne 1 -or
    [regex]::Matches($installerSource, [regex]::Escape('{#MyAuthorSignature}')).Count -ne 2 -or
    $installerSource -notmatch '(?m)^#define MyAuthorSignature "@十七°"\r?$' -or
    $installerSource -notmatch '(?m)^#define MyAppPublisher "Lattice"\r?$' -or
    ([regex]::Matches($installerSource, '(?m)^#if Int\(OfflineBuild\) == 1\r?$').Count -ne 1) -or
    $installerSource -notmatch '(?m)^AppVerName=\{#MyAppName\} \{#MyAppVersion\} \{#MyAuthorSignature\}\r?$' -or
    $installerSource -notmatch "VersionLabel\.Caption\s*:=\s*'Lattice / \{#MyAppVersion\} \{#MyAuthorSignature\}';"
) {
    throw "Installer author signature contract is incomplete or duplicated."
}
foreach ($taskName in @("startup", "desktopicon", "startmenuicon")) {
    $taskDefinitions = [regex]::Matches($installerSource, ('(?m)^Name: "' + $taskName + '";[^\r\n]*$'))
    if ($taskDefinitions.Count -ne 1 -or $taskDefinitions[0].Value -match '\bFlags:') {
        throw "Installer task is missing or not checked by default: $taskName"
    }
}
if (
    $installerSource -notmatch '(?m)^UsePreviousTasks=yes\r?$' -or
    $installerSource -notmatch 'ValueName: "Lattice"; Flags: deletevalue; Check: not WizardIsTaskSelected\(''startup''\)' -or
    $installerSource -notmatch 'ValueData: """\{app\}\\\{#MyAppExeName\}"""' -or
    $installerSource -notmatch 'Name: "\{userdesktop\}\\\{#MyAppName\}\.lnk"; Check: not WizardIsTaskSelected\(''desktopicon''\)' -or
    ([regex]::Matches($installerSource, 'Name: "\{group\}\\(?:\{#MyAppName\}|卸载 \{#MyAppName\})\.lnk"; Check: not WizardIsTaskSelected\(''startmenuicon''\)').Count -ne 2) -or
    $installerSource -notmatch 'LegacyStartupWasEnabled and WizardIsTaskSelected\(''startup''\)'
) {
    throw "Installer task persistence, deselection cleanup, or quoted startup contract is incomplete."
}
if (
    $installerSource -match '(?m)^\s*function\s+InitializeSetup\s*[:(]' -or
    $installerSource -match 'RuntimeVersion' -or
    $installerSource -match 'RuntimeDll' -or
    $installerSource -match 'GetVersionNumbersString\s*\(' -or
    $installerSource -match '\bLoadDLL\s*\(' -or
    $installerSource -match '\bFreeDLL\s*\('
) {
    throw "The standard installer must not contain a Visual C++ runtime preflight gate."
}
if (
    $resourceSource -notmatch '(?m)^\s*VALUE "CompanyName", "Lattice"\r?$' -or
    $resourceSource -match '(?m)^\s*VALUE "LegalCopyright"' -or
    $resourceSource -match '@十七°'
) {
    throw "PE company identity must remain Lattice and must not contain the installer author signature."
}
if (
    $installerSource -match '(?m)^\s*Name:\s*"shortcutoverlay"' -or
    $installerSource -notmatch 'ShellIconsKey\s*=\s*''Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Icons''' -or
    $installerSource -notmatch 'OverlayValueName\s*=\s*''29''' -or
    $installerSource -notmatch 'SnapshotShortcutOverlay' -or
    $installerSource -notmatch 'RestoreShortcutOverlay' -or
    $resourceSource -notmatch 'IDI_SHORTCUT_OVERLAY\s+ICON\s+"\.\./\.\./assets/branding/lattice-shortcut-overlay\.ico"' -or
    $iconCacheSource -notmatch 'SHGFI_ADDOVERLAYS\s*\|\s*SHGFI_OVERLAYINDEX' -or
    $iconCacheSource -notmatch 'INDEXTOOVERLAYMASK\s*\(overlayIndex\)' -or
    $iconCacheSource -match 'BuildShortcutOverlayPixels|GetShortcutOverlay' -or
    $iconGridSource -match 'GetShortcutOverlay'
) {
    throw "Automatic shortcut overlay packaging contract is incomplete, a user-selectable task returned, or manual grid composition returned."
}
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

if (!$SkipBuild) {
    & $buildScript -Configuration Release
    & $buildScript -Configuration ReleaseOffline
}
$releaseExe = Join-Path $releaseDir "Lattice.exe"
$offlineReleaseExe = Join-Path $offlineReleaseDir "Lattice.exe"

function Assert-ReleaseBinary([string]$Executable, [string]$Label) {
    if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) {
        throw "$Label executable not found: $Executable"
    }
    foreach ($legacyName in @("Luno.exe", "Luno.pdb", "DesktopOrganizer.exe", "DesktopOrganizer.pdb")) {
        $legacyArtifact = Join-Path (Split-Path -Parent $Executable) $legacyName
        if (Test-Path -LiteralPath $legacyArtifact) {
            throw "Legacy release artifact must not be packaged: $legacyArtifact"
        }
    }

    $versionInfo = (Get-Item -LiteralPath $Executable).VersionInfo
    if ($versionInfo.CompanyName.Trim() -ne "Lattice") {
        throw "$Label CompanyName mismatch. Expected Lattice, actual=$($versionInfo.CompanyName)"
    }
    $fileVersionString = [version]$versionInfo.FileVersion
    $productVersionString = [version]$versionInfo.ProductVersion
    $fileVersionFixed = [version]::new(
        $versionInfo.FileMajorPart,
        $versionInfo.FileMinorPart,
        $versionInfo.FileBuildPart,
        $versionInfo.FilePrivatePart)
    $productVersionFixed = [version]::new(
        $versionInfo.ProductMajorPart,
        $versionInfo.ProductMinorPart,
        $versionInfo.ProductBuildPart,
        $versionInfo.ProductPrivatePart)
    if (
        $fileVersionString -ne $expectedBinaryVersion -or
        $productVersionString -ne $expectedBinaryVersion -or
        $fileVersionFixed -ne $expectedBinaryVersion -or
        $productVersionFixed -ne $expectedBinaryVersion
    ) {
        throw "$Label version mismatch. Expected $expectedBinaryVersion, file-string=$fileVersionString, product-string=$productVersionString, file-fixed=$fileVersionFixed, product-fixed=$productVersionFixed"
    }
}

Assert-ReleaseBinary $releaseExe "Standard Lattice.exe"
Assert-ReleaseBinary $offlineReleaseExe "Offline Lattice.exe"

function Find-DumpBin {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }
    $toolsRoot = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC"
    if (Test-Path -LiteralPath $toolsRoot) {
        $candidate = Get-ChildItem -LiteralPath $toolsRoot -Filter dumpbin.exe -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\Hostx64\\x64\\dumpbin\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($null -ne $candidate) {
            return $candidate.FullName
        }
    }
    throw "Visual Studio dumpbin.exe was not found."
}

$dumpBin = Find-DumpBin
function Get-DirectDependencies([string]$Executable, [string]$Label) {
    $outputLines = @(& $dumpBin /nologo /dependents $Executable 2>&1)
    $dumpBinExitCode = $LASTEXITCODE
    $output = $outputLines -join [Environment]::NewLine
    if ($dumpBinExitCode -ne 0) {
        throw "dumpbin failed for $Label with exit code $dumpBinExitCode.`n$output"
    }
    $section = [regex]::Match(
        $output,
        '(?ims)^\s*Image has the following dependencies:\s*\r?\n(?<dependencies>.*?)(?=^\s*Image has the following|^\s*Summary\s*$|\z)')
    if (-not $section.Success) {
        throw "dumpbin output for $Label did not contain a parseable direct dependency section.`n$output"
    }
    $dependencyLines = @(
        $section.Groups['dependencies'].Value -split '\r?\n' |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_.Length -ne 0 }
    )
    $unparsedDependencyLines = @($dependencyLines | Where-Object { $_ -notmatch '^[A-Za-z0-9._-]+\.dll$' })
    if ($unparsedDependencyLines.Count -ne 0) {
        throw "dumpbin output for $Label contained unparseable dependency lines: $($unparsedDependencyLines -join ', ')"
    }
    $dependencies = @($dependencyLines | ForEach-Object { $_.ToUpperInvariant() } | Sort-Object -Unique)
    if ($dependencies.Count -eq 0) {
        throw "dumpbin output for $Label contained no direct dependencies."
    }
    return $dependencies
}

$standardDependencies = @(Get-DirectDependencies $releaseExe "Standard Lattice.exe")
$offlineDependencies = @(Get-DirectDependencies $offlineReleaseExe "Offline Lattice.exe")
$requiredStandardRuntimeDependencies = @("MSVCP140.DLL", "VCRUNTIME140.DLL", "VCRUNTIME140_1.DLL")
$missingStandardRuntimeDependencies = @(
    $requiredStandardRuntimeDependencies | Where-Object { $_ -notin $standardDependencies }
)
if ($missingStandardRuntimeDependencies.Count -ne 0) {
    throw "Standard Lattice.exe is missing direct Visual C++ runtime imports: $($missingStandardRuntimeDependencies -join ', ')"
}
$forbiddenOfflineRuntimePattern = '^(?:MSVCP|MSVCR|VCRUNTIME|CONCRT|MFC|MFCO|ATL|VCOMP|UCRTBASE|API-MS-WIN-CRT-).*\.DLL$'
$forbiddenOfflineDependencies = @($offlineDependencies | Where-Object { $_ -match $forbiddenOfflineRuntimePattern })
if ($forbiddenOfflineDependencies.Count -ne 0) {
    throw "Offline Lattice.exe imports forbidden MSVC/MFC/ATL/VCOMP runtime dependencies: $($forbiddenOfflineDependencies -join ', ')"
}
$allowedOfflineDependencies = @(
    "ADVAPI32.DLL",
    "COMDLG32.DLL",
    "COMCTL32.DLL",
    "D2D1.DLL",
    "DWMAPI.DLL",
    "DWRITE.DLL",
    "GDI32.DLL",
    "KERNEL32.DLL",
    "OLE32.DLL",
    "OLEAUT32.DLL",
    "SHELL32.DLL",
    "SHLWAPI.DLL",
    "USER32.DLL",
    "UXTHEME.DLL",
    "VERSION.DLL",
    "WINDOWSCODECS.DLL",
    "WINHTTP.DLL"
)
$unknownOfflineDependencies = @($offlineDependencies | Where-Object { $_ -notin $allowedOfflineDependencies })
if ($unknownOfflineDependencies.Count -ne 0) {
    throw "Offline Lattice.exe imports non-allowlisted direct dependencies: $($unknownOfflineDependencies -join ', ')"
}

$publicCertificateSource = & $signScript -ArtifactPath @($releaseExe, $offlineReleaseExe) -RunTamperCheck |
    Select-Object -Last 1
if ([string]::IsNullOrWhiteSpace($publicCertificateSource) -or
    !(Test-Path -LiteralPath $publicCertificateSource -PathType Leaf)) {
    throw "The self-signed public certificate was not produced."
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

& $isccPath "--define=OfflineBuild=0" $installerScript
if ($LASTEXITCODE -ne 0) {
    throw "Standard Inno Setup compilation failed with exit code $LASTEXITCODE"
}
& $isccPath "--define=OfflineBuild=1" $installerScript
if ($LASTEXITCODE -ne 0) {
    throw "Offline Inno Setup compilation failed with exit code $LASTEXITCODE"
}

$versionedInstaller = Join-Path $releaseOutputDir "Lattice-Setup-$installerVersion.exe"
$offlineVersionedInstaller = Join-Path $releaseOutputDir "Lattice-Setup-$installerVersion-Offline.exe"
if (!(Test-Path -LiteralPath $versionedInstaller)) {
    throw "Installer output not found: $versionedInstaller"
}
if (!(Test-Path -LiteralPath $offlineVersionedInstaller)) {
    throw "Offline installer output not found: $offlineVersionedInstaller"
}

& $signScript -ArtifactPath @($versionedInstaller, $offlineVersionedInstaller) | Out-Null

function Assert-InstallerMetadata([string]$Installer, [string]$Label) {
$installerVersionInfo = (Get-Item -LiteralPath $Installer).VersionInfo
$installerFileVersionText = $installerVersionInfo.FileVersion.Trim()
$installerProductVersionText = $installerVersionInfo.ProductVersion.Trim()
if ([string]::IsNullOrWhiteSpace($installerFileVersionText)) {
    throw "$Label FileVersion is empty."
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
    throw "$Label version mismatch. Expected file=$expectedBinaryVersion and product=$installerVersion, file-string=$installerFileVersionString, file-fixed=$installerFileVersionFixed, product=$installerProductVersionText"
}
if (
    $installerVersionInfo.ProductName.Trim() -ne "Lattice" -or
    $installerVersionInfo.FileDescription.Trim() -ne "Lattice Setup" -or
    $installerVersionInfo.CompanyName.Trim() -ne "Lattice"
) {
    throw "$Label branding mismatch. ProductName=$($installerVersionInfo.ProductName), FileDescription=$($installerVersionInfo.FileDescription), CompanyName=$($installerVersionInfo.CompanyName)"
}
}

Assert-InstallerMetadata $versionedInstaller "Standard installer"
Assert-InstallerMetadata $offlineVersionedInstaller "Offline installer"

$latestInstaller = Join-Path $releaseOutputDir "Lattice-Setup-Latest.exe"
$offlineLatestInstaller = Join-Path $releaseOutputDir "Lattice-Setup-Latest-Offline.exe"
$publicCertificate = Join-Path $releaseOutputDir "Lattice-$installerVersion-SelfSigned-Public.cer"
Copy-Item -LiteralPath $versionedInstaller -Destination $latestInstaller -Force
Copy-Item -LiteralPath $offlineVersionedInstaller -Destination $offlineLatestInstaller -Force
Copy-Item -LiteralPath $publicCertificateSource -Destination $publicCertificate -Force

$releaseItem = Get-Item -LiteralPath $releaseExe
$versionedInstallerItem = Get-Item -LiteralPath $versionedInstaller
if ($versionedInstallerItem.LastWriteTimeUtc -lt $releaseItem.LastWriteTimeUtc) {
    throw "Installer is older than the packaged Lattice.exe."
}
$offlineReleaseItem = Get-Item -LiteralPath $offlineReleaseExe
$offlineVersionedInstallerItem = Get-Item -LiteralPath $offlineVersionedInstaller
if ($offlineVersionedInstallerItem.LastWriteTimeUtc -lt $offlineReleaseItem.LastWriteTimeUtc) {
    throw "Offline installer is older than the packaged offline Lattice.exe."
}

$signatureThumbprints = @()
foreach ($signedArtifact in @(
    $releaseExe,
    $offlineReleaseExe,
    $versionedInstaller,
    $latestInstaller,
    $offlineVersionedInstaller,
    $offlineLatestInstaller)) {
    $signature = Get-AuthenticodeSignature -LiteralPath $signedArtifact
    if ($null -eq $signature.SignerCertificate -or
        $signature.Status -eq [System.Management.Automation.SignatureStatus]::NotSigned -or
        $signature.Status -eq [System.Management.Automation.SignatureStatus]::HashMismatch) {
        throw "Authenticode verification failed for $signedArtifact. Status=$($signature.Status)"
    }
    $signatureThumbprints += $signature.SignerCertificate.Thumbprint
}
$uniqueThumbprints = @($signatureThumbprints | Sort-Object -Unique)
$publicCertificateInfo = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($publicCertificate)
try {
    $publicCertificateThumbprint = $publicCertificateInfo.Thumbprint
} finally {
    $publicCertificateInfo.Dispose()
}
if ($uniqueThumbprints.Count -ne 1 -or
    $uniqueThumbprints[0] -ne $publicCertificateThumbprint) {
    throw "Signed artifacts and exported public certificate do not share one signer."
}

$versionedHash = (Get-FileHash -LiteralPath $versionedInstaller -Algorithm SHA256).Hash
$latestHash = (Get-FileHash -LiteralPath $latestInstaller -Algorithm SHA256).Hash
$offlineVersionedHash = (Get-FileHash -LiteralPath $offlineVersionedInstaller -Algorithm SHA256).Hash
$offlineLatestHash = (Get-FileHash -LiteralPath $offlineLatestInstaller -Algorithm SHA256).Hash
if ($versionedHash -ne $latestHash) {
    throw "Latest installer hash does not match the versioned installer."
}
if ($offlineVersionedHash -ne $offlineLatestHash) {
    throw "Latest offline installer hash does not match the versioned offline installer."
}

Write-Host "Installer built: $versionedInstaller"
Write-Host "Latest installer alias: $latestInstaller"
Write-Host "Installer SHA256: $versionedHash"
Write-Host "Offline installer built: $offlineVersionedInstaller"
Write-Host "Latest offline installer alias: $offlineLatestInstaller"
Write-Host "Offline installer SHA256: $offlineVersionedHash"
Write-Host "Self-signed public certificate: $publicCertificate"
