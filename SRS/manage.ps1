param([ValidateSet('start', 'stop', 'status', 'logs')][string]$Action = 'status')
$ErrorActionPreference = 'Stop'
$composeFile = Join-Path $PSScriptRoot 'compose.yaml'
switch ($Action) {
    'start' {
        & docker compose -f $composeFile config --quiet
        if ($LASTEXITCODE -ne 0) { throw 'Invalid SRS compose configuration' }
        & docker compose -f $composeFile up -d
    }
    'stop' { & docker compose -f $composeFile stop }
    'status' { & docker compose -f $composeFile ps }
    'logs' { & docker compose -f $composeFile logs --tail 60 }
}
if ($LASTEXITCODE -ne 0) { throw "SRS $Action failed. Check Docker and the reserved media ports; see SRS/README.md." }
