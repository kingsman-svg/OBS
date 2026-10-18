param([ValidateSet('build', 'start', 'stop', 'status', 'logs')][string]$Action = 'status')
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    # 只管理 media profile，不停止或重建 OBS SSH/业务服务及独立 SRS。
    if ($Action -eq 'build') {
        docker compose --profile media build media
        if ($LASTEXITCODE -ne 0) { throw '媒体镜像构建失败' }
        docker compose --profile media run --rm --no-deps media cmake -S . -B build-media -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOBS_BUILD_MEDIA=ON
        if ($LASTEXITCODE -ne 0) { throw '媒体网关 CMake 配置失败' }
        docker compose --profile media run --rm --no-deps media cmake --build build-media -j 4
        if ($LASTEXITCODE -ne 0) { throw '媒体网关编译失败' }
    } elseif ($Action -eq 'start') {
        docker compose --profile media up -d --no-deps media
    } elseif ($Action -eq 'stop') {
        docker compose --profile media stop media
    } elseif ($Action -eq 'logs') {
        docker compose --profile media logs --tail 60 media
    } else {
        docker compose --profile media ps media
    }
    if ($LASTEXITCODE -ne 0) { throw '媒体容器操作失败' }
} finally { Pop-Location }
