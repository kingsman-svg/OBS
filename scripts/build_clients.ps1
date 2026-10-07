param(
    [string]$QtRoot = 'C:/software/Qt/6.11.2/msvc2022_64',
    [string]$BuildDir = 'OBS client/build-agent',
    [switch]$LiveTests,
    [switch]$Deploy
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$clientBuild = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $BuildDir))
$qtPath = [System.IO.Path]::GetFullPath($QtRoot)
$qtTools = [System.IO.Path]::GetFullPath((Join-Path $qtPath '../../Tools'))
$cmakeExe = Join-Path $qtTools 'CMake_64/bin/cmake.exe'
$ninjaExe = Join-Path $qtTools 'Ninja/ninja.exe'
$vswhereExe = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhereExe -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsInstall) { throw 'No MSVC x64 toolchain was found.' }
$devCommand = Join-Path $vsInstall 'Common7/Tools/VsDevCmd.bat'
$vcVersion = (Get-Content (Join-Path $vsInstall 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$compilerFolder = Join-Path $vsInstall "VC/Tools/MSVC/$vcVersion/bin/Hostx64/x64"
$compilerLanguage = '1033'
$includePrefix = 'Note: including file: '
if (!(Test-Path -LiteralPath (Join-Path $compilerFolder '1033/clui.dll'))) {
    if (!(Test-Path -LiteralPath (Join-Path $compilerFolder '2052/clui.dll'))) {
        throw 'Build helper requires an English or Simplified Chinese MSVC language pack.'
    }
    $compilerLanguage = '2052'
    # Code points keep this script compatible with Windows PowerShell 5 source decoding.
    $includePrefix = -join ([char[]]@(0x6ce8, 0x610f, 0x3a, 0x20, 0x5305, 0x542b, 0x6587, 0x4ef6, 0x3a, 0x20))
}
foreach ($file in @($cmakeExe, $ninjaExe, $devCommand)) {
    if (!(Test-Path -LiteralPath $file)) { throw "Missing build tool: $file" }
}
$liveOption = if ($LiveTests) { 'ON' } else { 'OFF' }
New-Item -ItemType Directory -Force -Path $clientBuild | Out-Null
# Keep compiler environment in a child process and generated commands in the ignored build directory.
$commandFile = Join-Path $clientBuild 'build-clients.cmd'
$commands = @"
@echo off
chcp 65001 >nul
setlocal
call "$devCommand" -arch=x64 >nul
if errorlevel 1 exit /b 1
set VSLANG=$compilerLanguage
"$cmakeExe" -S "$projectRoot/OBS client" -B "$clientBuild" -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninjaExe" "-DCMAKE_PREFIX_PATH=$qtPath" -DCMAKE_BUILD_TYPE=Debug -DOBS_LIVE_TESTS=$liveOption "-DOBS_MSVC_INCLUDE_PREFIX=$includePrefix"
if errorlevel 1 exit /b 1
"$cmakeExe" --build "$clientBuild" --parallel 4
if errorlevel 1 exit /b 1
"@
if ($Deploy) {
    $deployExe = Join-Path $qtPath 'bin/windeployqt.exe'
    $commands += @"

"$deployExe" --debug --no-translations "$clientBuild/OBS_Publisher.exe" "$clientBuild/OBS_Player.exe"
if errorlevel 1 exit /b 1
"@
}
# cmd.exe requires CRLF even when this PowerShell source is checked out with LF.
$commands = ($commands -replace "`r`n", "`n") -replace "`n", "`r`n"
[System.IO.File]::WriteAllText($commandFile, $commands + "`r`n", [System.Text.UTF8Encoding]::new($false))
& cmd.exe /d /c $commandFile
if ($LASTEXITCODE -ne 0) { throw "Client build or deployment failed: $LASTEXITCODE" }
Write-Output "Clients: $clientBuild/OBS_Publisher.exe and OBS_Player.exe"
