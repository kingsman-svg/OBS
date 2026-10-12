$ErrorActionPreference = 'Stop'
$srsRoot = $PSScriptRoot
$sourceRoot = Join-Path $srsRoot 'upstream'
$sourceLock = Get-Content -LiteralPath (Join-Path $srsRoot 'source.lock.json') -Raw | ConvertFrom-Json
# 1. 上游源码有自己的Git历史，主工程只提交版本锁和接入文件，避免两份源码历史混在一起。
if (Test-Path -LiteralPath $sourceRoot) {
    $existingHead = & git -C $sourceRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $existingHead -ne $sourceLock.commit) {
        throw 'SRS/upstream already exists with a different state. Inspect it before updating; this script never resets local work.'
    }
    Write-Output "SRS source already prepared: $sourceRoot"
    exit 0
}
# 2. 固定到已记录的稳定分支提交；不执行上游脚本，也不改写用户的Git身份/凭据。
& git init $sourceRoot
if ($LASTEXITCODE -ne 0) { throw 'git init failed' }
& git -C $sourceRoot remote add upstream $sourceLock.repository
if ($LASTEXITCODE -ne 0) { throw 'adding upstream failed' }
& git -C $sourceRoot fetch --depth 1 upstream $sourceLock.commit
if ($LASTEXITCODE -ne 0) { throw 'fetch failed; inspect SRS/upstream before retrying' }
& git -C $sourceRoot checkout -b obs-bridge $sourceLock.commit
if ($LASTEXITCODE -ne 0) { throw 'checkout failed' }
Write-Output "SRS source: $sourceRoot (branch obs-bridge)"
