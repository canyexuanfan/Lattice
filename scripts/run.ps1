$ErrorActionPreference = "Stop"

& "$PSScriptRoot\build.ps1"
$exe = Join-Path (Split-Path -Parent $PSScriptRoot) "x64\Debug\Lattice.exe"
if (!(Test-Path $exe)) {
    throw "Executable not found: $exe"
}

Start-Process $exe
