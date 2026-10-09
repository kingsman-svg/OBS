param([string]$SampleImage)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$assetDir = Join-Path $projectRoot 'cpp/out/assets'
New-Item -ItemType Directory -Force -Path $assetDir | Out-Null
$sdkDir = Join-Path $projectRoot 'cpp/out/sdk'
$ortHeader = Join-Path $sdkDir 'onnxruntime-win-x64-1.30.0/include/onnxruntime_cxx_api.h'
if (!(Test-Path -LiteralPath $ortHeader)) {
    $sdkArchive = Join-Path $projectRoot 'cpp/out/onnxruntime.zip'
    $sdkDownload = "$sdkArchive.download"
    if (!(Test-Path -LiteralPath $sdkArchive)) {
        Invoke-WebRequest -UseBasicParsing 'https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-win-x64-1.30.0.zip' -OutFile $sdkDownload
        Move-Item -LiteralPath $sdkDownload -Destination $sdkArchive
    }
    Expand-Archive -LiteralPath $sdkArchive -DestinationPath $sdkDir -Force
}
$modelPath = Join-Path $assetDir 'scrfd_10g.onnx'
if (!(Test-Path -LiteralPath $modelPath)) {
    # Official weights are for non-commercial research; see docs/007-model-optimization.md.
    $archivePath = Join-Path $assetDir 'buffalo_l.zip'
    if (!(Test-Path -LiteralPath $archivePath)) {
        $downloadPath = "$archivePath.download"
        Invoke-WebRequest -UseBasicParsing 'https://github.com/deepinsight/insightface/releases/download/model-zoo/buffalo_l.zip' -OutFile $downloadPath
        Move-Item -LiteralPath $downloadPath -Destination $archivePath
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $entry = $archive.Entries | Where-Object { $_.Name -eq 'det_10g.onnx' }
        if (!$entry -or @($entry).Count -ne 1) { throw 'Expected one det_10g.onnx in official buffalo_l pack.' }
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $modelPath, $false)
    } finally { $archive.Dispose() }
}
$hash = Get-FileHash -LiteralPath $modelPath -Algorithm SHA256
if ($hash.Hash -ne '5838F7FE053675B1C7A08B633DF49E7AF5495CEE0493C7DCF6697200B85B5B91') {
    throw 'Unexpected official detection model SHA256. Keep the file for inspection; do not build from unknown weights.'
}
$hash | Select-Object Hash,Path
if ($SampleImage) {
    # Keep user images local and ignored by Git; never include calibration photos in a commit.
    $sampleName = -join ([char[]]@(0x793a,0x4f8b,0x4eba,0x8138,0x2e,0x6a,0x70,0x67))
    Copy-Item -LiteralPath $SampleImage -Destination (Join-Path $assetDir $sampleName)
}
