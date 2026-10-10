param(
    [string]$BuildDir = 'OBS client/build-agent',
    [string]$QtRoot = 'C:/software/Qt/6.11.2/msvc2022_64',
    [string]$OpenCVRoot = 'C:/dev/opencv/opencv/build',
    [string]$Engine = 'cpp/out/scrfd_final_smoke.engine',
    [string]$Image = 'cpp/out/assets/示例人脸.jpg',
    [switch]$Gpu
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$originalPath = $env:PATH
$originalPlatform = $env:QT_QPA_PLATFORM
# Missing DLL/driver failures must return an error, not leave a modal Windows error dialog hanging.
if (-not ('FaceCheckErrorMode' -as [type])) {
    Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public class FaceCheckErrorMode { [DllImport("kernel32.dll")] public static extern uint SetErrorMode(uint mode); }'
}
$originalMode = [FaceCheckErrorMode]::SetErrorMode(0x8003)
try {
    $env:PATH = "$QtRoot/bin;$OpenCVRoot/x64/vc16/bin;$originalPath"
    $env:QT_QPA_PLATFORM = 'windows'
    $exe = Join-Path $projectRoot "$BuildDir/OBSFaceTests.exe"
    $testArgs = @()
    if ($Gpu) {
        $outputFolder = Join-Path $projectRoot 'out'
        New-Item -ItemType Directory -Force -Path $outputFolder | Out-Null
        $testArgs = @('--gpu', '--engine', (Join-Path $projectRoot $Engine), '--image', (Join-Path $projectRoot $Image),
            '--output', (Join-Path $outputFolder 'GPU人脸框点预览.png'))
    }
    & $exe @testArgs
    if ($LASTEXITCODE -ne 0) { throw "Face tests failed: $LASTEXITCODE" }
} finally {
    $env:PATH = $originalPath
    $env:QT_QPA_PLATFORM = $originalPlatform
    [FaceCheckErrorMode]::SetErrorMode($originalMode) | Out-Null
}
