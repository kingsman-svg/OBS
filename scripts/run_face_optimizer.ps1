param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$OpenCVRoot = 'C:/dev/opencv/opencv/build',
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$OptimizerArgs
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$originalPath = $env:PATH
try {
    $env:PATH = "$env:TENSORRT_ROOT/bin;$env:CUDA_PATH/bin;$OpenCVRoot/x64/vc16/bin;$projectRoot/cpp/out/sdk/onnxruntime-win-x64-1.30.0/lib;$originalPath"
    Push-Location (Join-Path $projectRoot 'cpp')
    try {
        & "./out/$Configuration/FaceOptimizer.exe" @OptimizerArgs
        if ($LASTEXITCODE -ne 0) { throw "Optimizer failed: $LASTEXITCODE" }
    } finally { Pop-Location }
} finally { $env:PATH = $originalPath }
