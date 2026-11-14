# Run in a compiler-enabled Developer PowerShell from an authenticated checkout.
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & go run ./cmd/connectcpp setup --source $PSScriptRoot @args
    if ($LASTEXITCODE -ne 0) { throw "Connect C++ setup failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
