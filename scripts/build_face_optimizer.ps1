param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$OpenCVRoot = 'C:/dev/opencv/opencv/build'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsInstall) { throw 'Visual Studio C++ tools were not found.' }
$msbuild = Join-Path $vsInstall 'MSBuild/Current/Bin/MSBuild.exe'
& $msbuild (Join-Path $projectRoot 'cpp/FaceOptimizer.sln') /m "/p:Configuration=$Configuration" /p:Platform=x64 "/p:OpenCVRoot=$OpenCVRoot" /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw "Build failed: $LASTEXITCODE" }
Write-Output "Executable: $projectRoot/cpp/out/$Configuration/FaceOptimizer.exe"
