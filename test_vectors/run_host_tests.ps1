# Optional -ZigExe: path to a portable zig.exe used as "zig c++" (no PATH change).
param([string]$ZigExe = "")

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$compiler = $null
$compilerPrefix = @()
if ($ZigExe) {
    if (-not (Test-Path -LiteralPath $ZigExe -PathType Leaf)) {
        Write-Output "ZigExe not found: $ZigExe. Host tests NOT executed."
        exit 2
    }
    $compiler = $ZigExe
    $compilerPrefix = @("c++")
} else {
    foreach ($candidate in @("g++", "clang++")) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) {
            $compiler = $command.Source
            break
        }
    }
}

if (-not $compiler) {
    Write-Output "No host C++ compiler found (g++ or clang++). Host tests NOT executed."
    exit 2
}

$sources = @(
    (Join-Path $PSScriptRoot "host_tests.cpp"),
    (Join-Path $root "shared\src\AgrotechProtocol.cpp"),
    (Join-Path $root "shared\src\AgrotechSha256.cpp"),
    (Join-Path $root "shared\src\AgrotechBase64.cpp"),
    (Join-Path $root "shared\src\AgrotechMqttPayload.cpp"),
    (Join-Path $root "shared\src\AgrotechCounters.cpp")
)

$output = Join-Path $env:TEMP "agrotech_host_tests.exe"
$arguments = $compilerPrefix + @(
    "-std=c++11",
    "-Wall",
    "-Wextra",
    "-I",
    (Join-Path $root "shared\src"),
    "-I",
    (Join-Path $root "coordinator")
) + $sources + @("-o", $output)

& $compiler @arguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $output
exit $LASTEXITCODE
