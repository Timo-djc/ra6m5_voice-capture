$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$python = Get-Command py -ErrorAction SilentlyContinue

if ($python) {
    & $python.Path -3 (Join-Path $scriptDir "build_prebuilt_libs.py") @args
    exit $LASTEXITCODE
}

$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    & $python.Path (Join-Path $scriptDir "build_prebuilt_libs.py") @args
    exit $LASTEXITCODE
}

Write-Error "Python launcher not found. Install Python or py.exe, then rerun."
