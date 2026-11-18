# Optional -ZigExe: path to a portable zig.exe used as "zig c++" (no PATH change).
param([string]$ZigExe = "")

$ErrorActionPreference = "Stop"

# BUILD-IOT-SENSOR-FAILOVER-05: host tests of the real
# coordinator/CoordinatorSensors.cpp against test doubles (no hardware).
$root = Split-Path -Parent $PSScriptRoot
$compiler = $null
$compilerPrefix = @()
if ($ZigExe) {
    if (-not (Test-Path -LiteralPath $ZigExe -PathType Leaf)) {
        Write-Output "ZigExe not found: $ZigExe. Failover host tests NOT executed."
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
    Write-Output "No host C++ compiler found (g++ or clang++). Failover host tests NOT executed."
    exit 2
}

$sources = @(
    (Join-Path $PSScriptRoot "failover_sensor_tests.cpp"),
    (Join-Path $root "coordinator\CoordinatorSensors.cpp"),
    (Join-Path $root "shared\src\AgrotechProtocol.cpp"),
    (Join-Path $root "shared\src\AgrotechSha256.cpp")
)

$output = Join-Path $env:TEMP "agrotech_failover_host_tests.exe"
$arguments = $compilerPrefix + @(
    "-std=c++11",
    "-Wall",
    "-Wextra",
    "-I",
    (Join-Path $PSScriptRoot "failover_mocks"),
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
