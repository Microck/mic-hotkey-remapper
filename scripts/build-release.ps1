[CmdletBinding()]
param(
    [string]$Version = "1.4.0",
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$sourceRoot = Join-Path $repoRoot "src"
$resourceRoot = Join-Path $repoRoot "resources"
$buildRoot = Join-Path $repoRoot "build\release"
$distRoot = Join-Path $repoRoot "dist"
$packageRoot = Join-Path $distRoot "mic-hotkey-remapper-v$Version"
$zipPath = Join-Path $distRoot "mic-hotkey-remapper-v$Version.zip"
$checksumPath = "$zipPath.sha256"

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE"
    }
}

if ($null -eq (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "cl.exe was not found. Run this script from a Visual Studio Developer Command Prompt."
}
if ($null -eq (Get-Command rc.exe -ErrorAction SilentlyContinue)) {
    throw "rc.exe was not found. Install the Windows SDK and run this script from a Visual Studio Developer Command Prompt."
}

if ($Clean -and (Test-Path -LiteralPath $buildRoot)) {
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
}
if (Test-Path -LiteralPath $packageRoot) {
    Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
if (Test-Path -LiteralPath $checksumPath) {
    Remove-Item -LiteralPath $checksumPath -Force
}

New-Item -ItemType Directory -Force $buildRoot, $distRoot, $packageRoot | Out-Null

Push-Location $repoRoot
try {
    Invoke-Native "cl.exe" @(
        "/nologo", "/std:c++17", "/O2", "/MT", "/LD", "/EHsc-",
        (Join-Path $sourceRoot "mic-audio-apo.cpp"),
        "/Fo$(Join-Path $buildRoot 'mic-audio-apo.obj')",
        "/link", "/SUBSYSTEM:WINDOWS", "/NODEFAULTLIB:atls.lib",
        "/DEF:$(Join-Path $sourceRoot 'mic-audio-apo.def')",
        "/OUT:$(Join-Path $buildRoot 'mic-audio-apo.dll')"
    )

    Invoke-Native "rc.exe" @(
        "/nologo", "/I", $sourceRoot,
        "/fo", (Join-Path $buildRoot "mic-hotkey-remapper-resources.res"),
        (Join-Path $resourceRoot "mic-hotkey-remapper-resources.rc")
    )

    Invoke-Native "cl.exe" @(
        "/nologo", "/std:c++17", "/O2", "/MT", "/EHsc",
        "/DMIC_HOTKEY_REMAPPER_EMBEDDED", "/c",
        (Join-Path $sourceRoot "mic-audio-cleaner.cpp"),
        "/Fo$(Join-Path $buildRoot 'mic-audio-cleaner.obj')"
    )

    Invoke-Native "cl.exe" @(
        "/nologo", "/std:c++17", "/O2", "/MT", "/EHsc",
        "/DMIC_HOTKEY_REMAPPER_EMBEDDED", "/c",
        (Join-Path $sourceRoot "mic-audio-cleaner-apo.cpp"),
        "/Fo$(Join-Path $buildRoot 'mic-audio-cleaner-apo.obj')"
    )

    Invoke-Native "cl.exe" @(
        "/nologo", "/std:c++17", "/O2", "/MT", "/EHsc", "/c",
        (Join-Path $sourceRoot "mic-hotkey-remapper.cpp"),
        "/Fo$(Join-Path $buildRoot 'mic-hotkey-remapper.obj')"
    )

    Invoke-Native "cl.exe" @(
        "/nologo",
        (Join-Path $buildRoot "mic-hotkey-remapper.obj"),
        (Join-Path $buildRoot "mic-audio-cleaner.obj"),
        (Join-Path $buildRoot "mic-audio-cleaner-apo.obj"),
        (Join-Path $buildRoot "mic-hotkey-remapper-resources.res"),
        "/link", "/SUBSYSTEM:WINDOWS",
        "/OUT:$(Join-Path $buildRoot 'mic-hotkey-remapper.exe')"
    )
} finally {
    Pop-Location
}

Copy-Item -LiteralPath (Join-Path $buildRoot "mic-hotkey-remapper.exe") -Destination (Join-Path $packageRoot "mic-hotkey-remapper.exe")
Copy-Item -LiteralPath (Join-Path $repoRoot "README.md") -Destination (Join-Path $packageRoot "README.md")
Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $packageRoot "LICENSE")
Compress-Archive -Path $packageRoot -DestinationPath $zipPath -CompressionLevel Optimal

$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $hashBytes = $sha256.ComputeHash([System.IO.File]::ReadAllBytes($zipPath))
    $hash = ([System.BitConverter]::ToString($hashBytes)).Replace("-", "").ToLowerInvariant()
} finally {
    $sha256.Dispose()
}
Set-Content -LiteralPath $checksumPath -Value "$hash  $(Split-Path -Leaf $zipPath)" -Encoding ascii

Write-Host "Release package: $zipPath"
Write-Host "SHA256: $hash"
