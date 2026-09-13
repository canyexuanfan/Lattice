param(
    [Parameter(Mandatory = $true)]
    [string]$Path,

    [string]$ExpectedProductName = 'Lattice'
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrEmpty($ExpectedProductName)) {
    throw 'ExpectedProductName must not be empty.'
}

$resolvedPath = [System.IO.Path]::GetFullPath($Path)
if (-not [System.IO.File]::Exists($resolvedPath)) {
    throw "Installer not found: $resolvedPath"
}

$bytes = [System.IO.File]::ReadAllBytes($resolvedPath)
$encoding = [System.Text.Encoding]::Unicode
$keyBytes = $encoding.GetBytes('ProductName')
$expectedBytes = $encoding.GetBytes($ExpectedProductName)

function Read-UInt16LE([byte[]]$Buffer, [int]$Offset) {
    if ($Offset -lt 0 -or $Offset + 2 -gt $Buffer.Length) {
        throw "UInt16 offset is outside the file: $Offset"
    }

    return [int]$Buffer[$Offset] -bor ([int]$Buffer[$Offset + 1] -shl 8)
}

function Align-4([int]$Value) {
    return ($Value + 3) -band -4
}

$matches = [System.Collections.Generic.List[object]]::new()
for ($offset = 6; $offset -le $bytes.Length - $keyBytes.Length - 2; $offset++) {
    $same = $true
    for ($index = 0; $index -lt $keyBytes.Length; $index++) {
        if ($bytes[$offset + $index] -ne $keyBytes[$index]) {
            $same = $false
            break
        }
    }

    if (-not $same) {
        continue
    }

    $keyEnd = $offset + $keyBytes.Length
    if ($bytes[$keyEnd] -ne 0 -or $bytes[$keyEnd + 1] -ne 0) {
        continue
    }

    $blockOffset = $offset - 6
    $blockLength = Read-UInt16LE $bytes $blockOffset
    $valueLength = Read-UInt16LE $bytes ($blockOffset + 2)
    $valueType = Read-UInt16LE $bytes ($blockOffset + 4)
    $valueOffset = Align-4 ($keyEnd + 2)
    $valueByteLength = $valueLength * 2

    if ($blockLength -le 0 -or
        $blockOffset + $blockLength -gt $bytes.Length -or
        $valueType -ne 1 -or
        $valueLength -le $ExpectedProductName.Length -or
        $valueOffset + $valueByteLength -gt $blockOffset + $blockLength) {
        continue
    }

    $prefixMatches = $true
    for ($index = 0; $index -lt $expectedBytes.Length; $index++) {
        if ($bytes[$valueOffset + $index] -ne $expectedBytes[$index]) {
            $prefixMatches = $false
            break
        }
    }

    if (-not $prefixMatches) {
        continue
    }

    $terminatorOffset = $valueOffset + $expectedBytes.Length
    $isAlreadyNormalized = $bytes[$terminatorOffset] -eq 0 -and $bytes[$terminatorOffset + 1] -eq 0
    $isSpacePadded = $bytes[$terminatorOffset] -eq 0x20 -and $bytes[$terminatorOffset + 1] -eq 0
    if (-not $isAlreadyNormalized -and -not $isSpacePadded) {
        continue
    }

    $paddingIsCanonical = $true
    for ($characterIndex = $ExpectedProductName.Length + 1; $characterIndex -lt $valueLength - 1; $characterIndex++) {
        $characterOffset = $valueOffset + ($characterIndex * 2)
        if ($bytes[$characterOffset] -ne 0x20 -or $bytes[$characterOffset + 1] -ne 0) {
            $paddingIsCanonical = $false
            break
        }
    }
    $finalTerminatorOffset = $valueOffset + (($valueLength - 1) * 2)
    if (-not $paddingIsCanonical -or
        $bytes[$finalTerminatorOffset] -ne 0 -or
        $bytes[$finalTerminatorOffset + 1] -ne 0) {
        continue
    }

    $matches.Add([pscustomobject]@{
        BlockOffset = $blockOffset
        ValueOffset = $valueOffset
        TerminatorOffset = $terminatorOffset
        AlreadyNormalized = $isAlreadyNormalized
    })
}

if ($matches.Count -ne 1) {
    throw "Expected exactly one padded ProductName version-resource field, found $($matches.Count)."
}

$match = $matches[0]
if (-not $match.AlreadyNormalized) {
    $originalLength = $bytes.Length
    $bytes[$match.TerminatorOffset] = 0
    $bytes[$match.TerminatorOffset + 1] = 0
    [System.IO.File]::WriteAllBytes($resolvedPath, $bytes)

    if ((Get-Item -LiteralPath $resolvedPath).Length -ne $originalLength) {
        throw 'ProductName normalization unexpectedly changed the installer length.'
    }
}

$versionInfo = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($resolvedPath)
if ($versionInfo.ProductName -cne $ExpectedProductName) {
    throw "Normalized ProductName is not exact: '$($versionInfo.ProductName)'"
}

[pscustomobject]@{
    Path = $resolvedPath
    ProductName = $versionInfo.ProductName
    FileVersion = $versionInfo.FileVersion
    Changed = -not $match.AlreadyNormalized
    Length = (Get-Item -LiteralPath $resolvedPath).Length
}
