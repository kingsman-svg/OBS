param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $projectRoot "cpp/out/$Configuration/FaceOptimizer.exe"
$savedPath = $env:PATH
$testDir = Join-Path $projectRoot ('cpp/out/checks-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $testDir | Out-Null
function Check-Exit([string[]]$Arguments, [int]$Expected) {
    $ErrorActionPreference = 'Continue'
    $output = & $executable @Arguments 2>&1 | Out-String
    $actual = $LASTEXITCODE
    if ($actual -ne $Expected) { throw "Expected $Expected, got $actual : $output" }
    return $output
}
try {
    $env:PATH = "$env:TENSORRT_ROOT/bin;$env:CUDA_PATH/bin;C:/dev/opencv/opencv/build/x64/vc16/bin;$projectRoot/cpp/out/sdk/onnxruntime-win-x64-1.30.0/lib;$savedPath"
    Check-Exit @('--self-test') 0 | Write-Output
    Check-Exit @('--unknown') 1 | Out-Null
    Check-Exit @('--output') 1 | Out-Null
    $protected = Join-Path $testDir 'protected.engine'
    [System.IO.File]::WriteAllText($protected, 'keep-existing-engine')
    Check-Exit @('--smoke', '--output', $protected) 1 | Out-Null
    if ([System.IO.File]::ReadAllText($protected) -ne 'keep-existing-engine') { throw 'Existing engine was changed.' }
    $calibration = Join-Path $testDir 'calibration'
    $validation = Join-Path $testDir 'validation'
    New-Item -ItemType Directory -Force -Path $calibration,$validation | Out-Null
    # Same bytes across two distinct directories must not pass formal validation.
    [System.IO.File]::WriteAllBytes((Join-Path $calibration 'duplicate.jpg'), [byte[]]@(1,2,3))
    [System.IO.File]::WriteAllBytes((Join-Path $validation 'duplicate.jpg'), [byte[]]@(1,2,3))
    $candidate = Join-Path $testDir 'rejected.engine'
    $model = Join-Path $projectRoot 'cpp/out/assets/scrfd_10g.onnx'
    $result = Check-Exit @('--onnx', $model, '--calibration', $calibration, '--validation', $validation, '--output', $candidate) 1
    if ($result -notmatch 'same image bytes' -or (Test-Path -LiteralPath $candidate)) { throw 'Overlapping sets were not rejected before building.' }
    $systemOrt = Join-Path $env:SystemRoot 'System32/onnxruntime.dll'
    if ((Test-Path -LiteralPath $systemOrt) -and (Get-Item -LiteralPath $systemOrt).VersionInfo.FileMinorPart -lt 30) {
        # Reproduce Windows loading its old ORT DLL; API mismatch must return 1, not dereference nullptr.
        $oldDir = Join-Path $testDir 'old-runtime'
        New-Item -ItemType Directory -Path $oldDir | Out-Null
        Copy-Item -LiteralPath $executable -Destination (Join-Path $oldDir 'FaceOptimizer.exe')
        $executable = Join-Path $oldDir 'FaceOptimizer.exe'
        $result = Check-Exit @('--smoke') 1
        if ($result -notmatch 'DLL/API mismatch') { throw 'Old ORT did not fail with a clear version error.' }
        Write-Output 'PASS: old ORT DLL is rejected without a null API access.'
    }
    Write-Output 'PASS: invalid CLI, existing output preservation, disjoint dataset guard.'
} finally { $env:PATH = $savedPath }
