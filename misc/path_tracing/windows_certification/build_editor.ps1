param(
    [Parameter(Mandatory = $true)]
    [string]$GodotRoot,
    [int]$Jobs = 12
)

$ErrorActionPreference = "Stop"
$requiredScons = "4.10.1"
$sconsVersionOutput = (& scons --version | Out-String)
if ($sconsVersionOutput -notmatch [regex]::Escape($requiredScons)) {
    throw "SCons $requiredScons is required by the recorded Windows recipe. Actual output: $sconsVersionOutput"
}
if (-not (Test-Path (Join-Path $GodotRoot "SConstruct"))) {
    throw "Godot checkout not found: $GodotRoot"
}

Push-Location $GodotRoot
try {
    & scons `
        platform=windows `
        arch=x86_64 `
        target=editor `
        dev_build=yes `
        tests=yes `
        debug_symbols=yes `
        "-j$Jobs"
    if ($LASTEXITCODE -ne 0) {
        throw "Godot Windows editor build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
