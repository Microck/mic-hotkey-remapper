[CmdletBinding()]
param(
    [string]$Version = "1.4.1",
    [string]$VsDevCmdPath = "",
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$singleBuildScript = Join-Path $PSScriptRoot "build-release.ps1"
$distRoot = Join-Path $repoRoot "dist"
$stagingRoot = Join-Path $distRoot "mic-hotkey-remapper-v$Version"
$zipPath = Join-Path $distRoot "mic-hotkey-remapper-v$Version.zip"
$checksumPath = "$zipPath.sha256"

if ([string]::IsNullOrWhiteSpace($VsDevCmdPath) -and $env:VSINSTALLDIR) {
    $VsDevCmdPath = Join-Path $env:VSINSTALLDIR "Common7\Tools\VsDevCmd.bat"
}
if ([string]::IsNullOrWhiteSpace($VsDevCmdPath)) {
    $candidates = @(
        (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat")
    )
    $VsDevCmdPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($VsDevCmdPath) -or -not (Test-Path -LiteralPath $VsDevCmdPath)) {
    throw "VsDevCmd.bat was not found. Pass -VsDevCmdPath explicitly."
}

if (Test-Path -LiteralPath $stagingRoot) {
    Remove-Item -LiteralPath $stagingRoot -Recurse -Force
}
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
if (Test-Path -LiteralPath $checksumPath) {
    Remove-Item -LiteralPath $checksumPath -Force
}
New-Item -ItemType Directory -Force $stagingRoot | Out-Null

foreach ($architecture in @("x64", "arm64")) {
    $buildRoot = Join-Path $repoRoot "build\release-$architecture"
    $packageRoot = Join-Path $stagingRoot $architecture
    $cleanArgument = if ($Clean) { " -Clean" } else { "" }
    $command = 'call "' + $VsDevCmdPath + '" -host_arch=amd64 -arch=' + $architecture +
        ' && powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + $singleBuildScript +
        '" -Version "' + $Version + '" -Architecture ' + $architecture +
        ' -BuildRootPath "' + $buildRoot + '" -PackageRootPath "' + $packageRoot +
        '" -NoArchive' + $cleanArgument

    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "The $architecture build failed with exit code $LASTEXITCODE."
    }
}

Copy-Item -LiteralPath (Join-Path $repoRoot "README.md") -Destination (Join-Path $stagingRoot "README.md")
Copy-Item -LiteralPath (Join-Path $repoRoot "LICENSE") -Destination (Join-Path $stagingRoot "LICENSE")
Compress-Archive -Path (Join-Path $stagingRoot "*") -DestinationPath $zipPath -CompressionLevel Optimal

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
