param(
    [Parameter(Mandatory = $true)]
    [string[]]$ArtifactPath,
    [switch]$RunTamperCheck
)

$ErrorActionPreference = "Stop"

if ($PSVersionTable.PSEdition -eq "Core") {
    Add-Type -AssemblyName System.Security.Cryptography.ProtectedData
} else {
    Add-Type -AssemblyName System.Security
}

$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$projectPrefix = $projectRoot + [System.IO.Path]::DirectorySeparatorChar
$signingDirectory = Join-Path $projectRoot ".workspace\signing"
$pfxPath = Join-Path $signingDirectory "lattice-self-signed-codesign.pfx"
$cerPath = Join-Path $signingDirectory "Lattice-self-signed-public.cer"
$passwordPath = Join-Path $signingDirectory "lattice-self-signed-password.dpapi"
$expectedDistinguishedName = "CN=Lattice @" +
    [char]0x5341 + [char]0x4E03 + [char]0x00B0

function Resolve-WorkspaceArtifact([string]$Value) {
    if (!(Test-Path -LiteralPath $Value -PathType Leaf)) {
        throw "Signing input not found: $Value"
    }
    $resolved = (Resolve-Path -LiteralPath $Value).Path
    if (!$resolved.StartsWith($projectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to sign a file outside the project workspace: $resolved"
    }
    return $resolved
}

function New-CodeSigningMaterial {
    if (!(Test-Path -LiteralPath $signingDirectory)) {
        New-Item -ItemType Directory -Path $signingDirectory | Out-Null
    }

    $passwordBytes = [byte[]]::new(32)
    $random = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $random.GetBytes($passwordBytes)
    } finally {
        $random.Dispose()
    }
    $password = [Convert]::ToBase64String($passwordBytes)
    $rsa = [System.Security.Cryptography.RSA]::Create()
    $rsa.KeySize = 3072
    try {
        $request = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new(
            $expectedDistinguishedName,
            $rsa,
            [System.Security.Cryptography.HashAlgorithmName]::SHA256,
            [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
        [void]$request.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new(
                $false, $false, 0, $true))
        [void]$request.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
                [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature,
                $true))
        $enhancedKeyUsages = [System.Security.Cryptography.OidCollection]::new()
        [void]$enhancedKeyUsages.Add(
            [System.Security.Cryptography.Oid]::new("1.3.6.1.5.5.7.3.3", "Code Signing"))
        [void]$request.CertificateExtensions.Add(
            [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new(
                $enhancedKeyUsages, $true))
        $certificate = $request.CreateSelfSigned(
            [DateTimeOffset]::Now.AddDays(-1),
            [DateTimeOffset]::Now.AddYears(5))
        try {
            [System.IO.File]::WriteAllBytes(
                $pfxPath,
                $certificate.Export(
                    [System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx,
                    $password))
            [System.IO.File]::WriteAllBytes(
                $cerPath,
                $certificate.Export(
                    [System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
        } finally {
            $certificate.Dispose()
        }
    } finally {
        $rsa.Dispose()
    }

    $protectedPassword = [System.Security.Cryptography.ProtectedData]::Protect(
        [System.Text.Encoding]::UTF8.GetBytes($password),
        $null,
        [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
    [System.IO.File]::WriteAllBytes($passwordPath, $protectedPassword)
    return $password
}

function Get-CodeSigningPassword {
    if (!(Test-Path -LiteralPath $pfxPath -PathType Leaf) -or
        !(Test-Path -LiteralPath $cerPath -PathType Leaf) -or
        !(Test-Path -LiteralPath $passwordPath -PathType Leaf)) {
        return New-CodeSigningMaterial
    }
    $protectedPassword = [System.IO.File]::ReadAllBytes($passwordPath)
    $passwordBytes = [System.Security.Cryptography.ProtectedData]::Unprotect(
        $protectedPassword,
        $null,
        [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
    return [System.Text.Encoding]::UTF8.GetString($passwordBytes)
}

function Assert-CodeSigningCertificate(
    [System.Security.Cryptography.X509Certificates.X509Certificate2]$Certificate,
    [string]$Label,
    [bool]$RequirePrivateKey) {
    $rsaPublicKeyOid = "1.2.840.113549.1.1.1"
    $sha256WithRsaOid = "1.2.840.113549.1.1.11"
    $codeSigningEkuOid = "1.3.6.1.5.5.7.3.3"

    if ($Certificate.Subject -cne $expectedDistinguishedName -or
        $Certificate.Issuer -cne $expectedDistinguishedName) {
        throw "$Label certificate Subject and Issuer must both be $expectedDistinguishedName."
    }
    if ($RequirePrivateKey -and !$Certificate.HasPrivateKey) {
        throw "$Label certificate does not contain its private key."
    }
    if ($Certificate.PublicKey.Oid.Value -ne $rsaPublicKeyOid) {
        throw "$Label certificate public key is not RSA."
    }

    $rsa = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPublicKey(
        $Certificate)
    if ($null -eq $rsa) {
        throw "$Label certificate RSA public key could not be read."
    }
    try {
        if ($rsa.KeySize -lt 3072) {
            throw "$Label certificate RSA public key is smaller than 3072 bits."
        }
    } finally {
        $rsa.Dispose()
    }

    if ($Certificate.SignatureAlgorithm.Value -ne $sha256WithRsaOid) {
        throw "$Label certificate signature algorithm is not SHA-256 with RSA."
    }

    $hasCodeSigningEku = $false
    foreach ($extension in $Certificate.Extensions) {
        if ($extension.Oid.Value -eq "2.5.29.37") {
            $ekuExtension = [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]$extension
            foreach ($usage in $ekuExtension.EnhancedKeyUsages) {
                if ($usage.Value -eq $codeSigningEkuOid) {
                    $hasCodeSigningEku = $true
                }
            }
        }
    }
    if (!$hasCodeSigningEku) {
        throw "$Label certificate does not contain the Code Signing EKU."
    }

    $nowUtc = [DateTime]::UtcNow
    if ($nowUtc -lt $Certificate.NotBefore.ToUniversalTime() -or
        $nowUtc -gt $Certificate.NotAfter.ToUniversalTime()) {
        throw "$Label certificate is not currently valid."
    }
}

function Assert-CodeSigningMaterial([string]$Password) {
    $ephemeralKeySet = [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet
    $cerCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($cerPath)
    $pfxCertificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
        $pfxPath,
        $Password,
        $ephemeralKeySet)
    try {
        Assert-CodeSigningCertificate $cerCertificate "CER" $false
        Assert-CodeSigningCertificate $pfxCertificate "PFX" $true
        if ($cerCertificate.Thumbprint -ine $pfxCertificate.Thumbprint) {
            throw "CER and PFX certificate thumbprints do not match."
        }
        return $cerCertificate.Thumbprint
    } finally {
        $pfxCertificate.Dispose()
        $cerCertificate.Dispose()
    }
}

function Find-SignTool {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }
    $sdkBin = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    if (Test-Path -LiteralPath $sdkBin) {
        $candidate = Get-ChildItem -LiteralPath $sdkBin -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if ($null -ne $candidate) {
            return $candidate.FullName
        }
    }
    throw "Windows SDK SignTool was not found."
}

function Invoke-WorkspaceSignature(
    [string]$SignTool,
    [string]$Pfx,
    [string]$Password,
    [string]$Artifact) {
    $substTool = Join-Path $env:SystemRoot "System32\subst.exe"
    $mappedDrive = $null
    foreach ($driveCode in (90..77)) {
        $candidateDrive = ([char]$driveCode).ToString() + ":"
        if (!(Test-Path -LiteralPath ($candidateDrive + "\"))) {
            & $substTool $candidateDrive $projectRoot | Out-Null
            if ($LASTEXITCODE -eq 0) {
                $mappedDrive = $candidateDrive
                break
            }
        }
    }
    if ($null -eq $mappedDrive) {
        throw "No free drive letter was available for the workspace signing map."
    }

    try {
        $mappedPfx = $mappedDrive + $Pfx.Substring($projectRoot.Length)
        $mappedArtifact = $mappedDrive + $Artifact.Substring($projectRoot.Length)
        & $SignTool sign /fd SHA256 /f $mappedPfx /p $Password /d "Lattice" $mappedArtifact
        if ($LASTEXITCODE -ne 0) {
            throw "SignTool failed for $Artifact with exit code $LASTEXITCODE"
        }
    } finally {
        & $substTool $mappedDrive /d | Out-Null
    }
}

$resolvedArtifacts = @($ArtifactPath | ForEach-Object { Resolve-WorkspaceArtifact $_ })
$password = Get-CodeSigningPassword
$expectedThumbprint = Assert-CodeSigningMaterial $password
$signTool = Find-SignTool

foreach ($artifact in $resolvedArtifacts) {
    $existingSignature = Get-AuthenticodeSignature -LiteralPath $artifact
    if ($null -ne $existingSignature.SignerCertificate) {
        if ($existingSignature.SignerCertificate.Thumbprint -ne $expectedThumbprint) {
            throw "Refusing to replace a signature from a different certificate: $artifact"
        }
        if ($existingSignature.Status -ne [System.Management.Automation.SignatureStatus]::HashMismatch -and
            $existingSignature.Status -ne [System.Management.Automation.SignatureStatus]::NotSigned) {
            Write-Host "ALREADY_SIGNED=$artifact"
            continue
        }
    }
    Invoke-WorkspaceSignature $signTool $pfxPath $password $artifact
    $signature = Get-AuthenticodeSignature -LiteralPath $artifact
    if ($null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.Thumbprint -ne $expectedThumbprint -or
        $signature.Status -eq [System.Management.Automation.SignatureStatus]::NotSigned -or
        $signature.Status -eq [System.Management.Automation.SignatureStatus]::HashMismatch) {
        throw "Authenticode verification failed for $artifact. Status=$($signature.Status)"
    }
    Write-Host "SIGNED=$artifact"
    Write-Host "SIGNATURE_STATUS=$($signature.Status)"
    Write-Host "SIGNER_THUMBPRINT=$expectedThumbprint"
}

if ($RunTamperCheck -and $resolvedArtifacts.Count -gt 0) {
    $tamperPath = Join-Path $signingDirectory "tamper-check.exe"
    Copy-Item -LiteralPath $resolvedArtifacts[0] -Destination $tamperPath -Force
    try {
        $bytes = [System.IO.File]::ReadAllBytes($tamperPath)
        if ($bytes.Length -lt 4096) {
            throw "Signed artifact is too small for the tamper check."
        }
        $bytes[2048] = $bytes[2048] -bxor 1
        [System.IO.File]::WriteAllBytes($tamperPath, $bytes)
        $tamperedSignature = Get-AuthenticodeSignature -LiteralPath $tamperPath
        if ($tamperedSignature.Status -ne [System.Management.Automation.SignatureStatus]::HashMismatch) {
            throw "Tamper verification did not report HashMismatch. Status=$($tamperedSignature.Status)"
        }
        Write-Host "TAMPER_CHECK=HashMismatch"
    } finally {
        if (Test-Path -LiteralPath $tamperPath) {
            Remove-Item -LiteralPath $tamperPath -Force
        }
    }
}

Write-Output $cerPath
