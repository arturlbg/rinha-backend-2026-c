param(
    [int]$Runs = 1,
    [string]$CaseFilter = ""
)

$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$OfficialTest = (Resolve-Path (Join-Path $Root "..\rinha-de-backend-2026\test")).Path
$ComposeFile = Join-Path $Root "docker-compose.preview-cfdlb.yml"
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$ResultRoot = Join-Path $Root "tmp\results\phase11b-$Stamp"
$K6Work = Join-Path $ResultRoot "k6"
$K6Out = Join-Path $K6Work "test"
New-Item -ItemType Directory -Force $ResultRoot, $K6Work, $K6Out | Out-Null

Copy-Item -LiteralPath (Join-Path $OfficialTest "test.js") -Destination (Join-Path $K6Work "test.js") -Force
Copy-Item -LiteralPath (Join-Path $OfficialTest "k6-summary.js") -Destination (Join-Path $K6Work "k6-summary.js") -Force
Copy-Item -LiteralPath (Join-Path $OfficialTest "test-data.json") -Destination (Join-Path $K6Work "test-data.json") -Force

$Request0 = Join-Path $Root "tmp\request0.json"
if (-not (Test-Path $Request0)) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $Request0) | Out-Null
    $json = Get-Content -Raw (Join-Path $OfficialTest "test-data.json") | ConvertFrom-Json
    $json.entries[0].request | ConvertTo-Json -Depth 20 -Compress | Set-Content -Encoding ASCII $Request0
}

$summaryRows = New-Object System.Collections.Generic.List[object]

function Set-CaseEnv([hashtable]$Env) {
    $names = @(
        "RINHA_API_IMAGE",
        "RINHA_FDLB_IMAGE",
        "LB_CPUS",
        "LB_MEMORY",
        "API_CPUS",
        "API_MEMORY",
        "RINHA_API_PROCESS_MODE",
        "RINHA_FDLB_STRATEGY",
        "RINHA_FDLB_LEAN",
        "RINHA_FAST_FRAUD_PARSER",
        "RINHA_FDLB_TCP_DEFER_ACCEPT",
        "RINHA_FDLB_TCP_FASTOPEN",
        "RINHA_FDLB_SO_BUSY_POLL_US",
        "RINHA_FDLB_LISTEN_BACKLOG",
        "RINHA_FDLB_REUSEPORT",
        "RINHA_API_TCP_NODELAY",
        "RINHA_API_TCP_QUICKACK"
    )
    foreach ($name in $names) {
        if ($Env.ContainsKey($name)) {
            Set-Item -Path "env:$name" -Value ([string]$Env[$name])
        } else {
            Remove-Item -Path "env:$name" -ErrorAction SilentlyContinue
        }
    }
}

function Invoke-Compose([string[]]$ComposeArgs) {
    & docker compose -f $ComposeFile @ComposeArgs
}

function Wait-Ready([string]$OutPath) {
    for ($i = 0; $i -lt 120; $i++) {
        $body = & curl.exe -s --max-time 2 "http://localhost:9999/ready"
        if ($LASTEXITCODE -eq 0 -and $body -eq "ok") {
            "ready after ${i}s" | Set-Content -Encoding ASCII $OutPath
            return
        }
        Start-Sleep -Seconds 1
    }
    throw "service did not become ready"
}

function Save-Smoke([string]$OutPath) {
    $readyOk = 0
    for ($i = 0; $i -lt 10; $i++) {
        $code = & curl.exe -s -o NUL -w "%{http_code}" "http://localhost:9999/ready"
        if ($code -eq "200") { $readyOk++ }
    }

    $fraudOk = 0
    $bodies = @{}
    for ($i = 0; $i -lt 10; $i++) {
        $body = & curl.exe -s --max-time 5 -X POST "http://localhost:9999/fraud-score" `
            -H "Content-Type: application/json" `
            --data-binary "@$Request0"
        $bodies[$body] = 1
        if ($body -match '^\{"approved":(true|false),"fraud_score":(0|0\.2|0\.4|0\.6|0\.8|1)\}$') {
            $fraudOk++
        }
    }

    @(
        "ready_ok=$readyOk/10",
        "fraud_ok=$fraudOk/10",
        "fraud_bodies=$($bodies.Keys -join ';')"
    ) | Set-Content -Encoding ASCII $OutPath
}

function Save-DockerStats([string]$OutPath) {
    $ids = @((Invoke-Compose @("ps", "-q")) | Where-Object { $_ -ne "" })
    if ($ids.Count -eq 0) {
        "" | Set-Content -Encoding ASCII $OutPath
        return
    }
    & docker stats --no-stream --format "table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.PIDs}}" @ids |
        Set-Content -Encoding ASCII $OutPath
}

function Save-Inspect([string]$OutPath) {
    $ids = @((Invoke-Compose @("ps", "-q")) | Where-Object { $_ -ne "" })
    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($id in $ids) {
        $raw = & docker inspect $id | ConvertFrom-Json
        $state = $raw[0].State
        $rows.Add([PSCustomObject]@{
            name = $raw[0].Name.TrimStart("/")
            restart_count = $raw[0].RestartCount
            oom_killed = $state.OOMKilled
            running = $state.Running
            exit_code = $state.ExitCode
        })
    }
    $rows | ConvertTo-Json -Depth 4 | Set-Content -Encoding ASCII $OutPath
}

function Get-MetricValue($Summary, [string]$MetricName, [string]$ValueName) {
    if ($null -eq $Summary -or $null -eq $Summary.metrics) { return $null }
    $metric = $Summary.metrics.PSObject.Properties[$MetricName]
    if ($null -eq $metric) { return $null }
    $source = $metric.Value
    if ($null -ne $metric.Value.values) { $source = $metric.Value.values }
    $value = $source.PSObject.Properties[$ValueName]
    if ($null -eq $value) { return $null }
    return $value.Value
}

function Read-RunSummary([string]$RunDir, [hashtable]$Case, [int]$Run) {
    $result = Get-Content -Raw (Join-Path $RunDir "results.json") | ConvertFrom-Json
    $summary = Get-Content -Raw (Join-Path $RunDir "k6-summary-full.json") | ConvertFrom-Json
    return [PSCustomObject]@{
        name = $Case.Name
        run = $Run
        lean = $Case.Env.RINHA_FDLB_LEAN
        fast_parser = $Case.Env.RINHA_FAST_FRAUD_PARSER
        defer_accept = $Case.Env.RINHA_FDLB_TCP_DEFER_ACCEPT
        busy_poll_us = $Case.Env.RINHA_FDLB_SO_BUSY_POLL_US
        p99 = $result.p99
        final_score = $result.scoring.final_score
        p99_score = $result.scoring.p99_score.value
        detection_score = $result.scoring.detection_score.value
        TP = $result.scoring.breakdown.true_positive_detections
        TN = $result.scoring.breakdown.true_negative_detections
        FP = $result.scoring.breakdown.false_positive_detections
        FN = $result.scoring.breakdown.false_negative_detections
        Error = $result.scoring.breakdown.http_errors
        failure_rate = $result.scoring.failure_rate
        duration_p99 = Get-MetricValue $summary "http_req_duration" "p(99)"
        waiting_p99 = Get-MetricValue $summary "http_req_waiting" "p(99)"
        blocked_p99 = Get-MetricValue $summary "http_req_blocked" "p(99)"
        connecting_p99 = Get-MetricValue $summary "http_req_connecting" "p(99)"
        sending_p99 = Get-MetricValue $summary "http_req_sending" "p(99)"
        receiving_p99 = Get-MetricValue $summary "http_req_receiving" "p(99)"
    }
}

function Invoke-Run([hashtable]$Case, [int]$Run) {
    $name = [string]$Case.Name
    $runDir = Join-Path $ResultRoot ("{0}-run{1}" -f $name, $Run)
    New-Item -ItemType Directory -Force $runDir | Out-Null

    Set-CaseEnv $Case.Env
    try {
        Invoke-Compose @("down", "--remove-orphans") | Out-File -Encoding ASCII (Join-Path $runDir "down-before.log")
        Invoke-Compose @("up", "-d", "--no-build") | Out-File -Encoding ASCII (Join-Path $runDir "up.log")
        Wait-Ready (Join-Path $runDir "ready.txt")
        Save-Smoke (Join-Path $runDir "smoke.txt")
        Save-DockerStats (Join-Path $runDir "stats-before.txt")
        Save-Inspect (Join-Path $runDir "inspect-before.json")

        Remove-Item -LiteralPath (Join-Path $K6Out "results.json") -ErrorAction SilentlyContinue
        Push-Location $K6Work
        try {
            $stdout = Join-Path $runDir "k6.stdout.log"
            $stderr = Join-Path $runDir "k6.stderr.log"
            $summaryPath = Join-Path $runDir "k6-summary-full.json"
            $process = Start-Process -FilePath "k6" `
                -ArgumentList @("run", "--summary-export", $summaryPath, ".\test.js") `
                -NoNewWindow `
                -Wait `
                -PassThru `
                -RedirectStandardOutput $stdout `
                -RedirectStandardError $stderr
            if ($process.ExitCode -ne 0) {
                throw "k6 failed with exit code $($process.ExitCode)"
            }
        } finally {
            Pop-Location
        }

        $resultPath = Join-Path $K6Out "results.json"
        if (-not (Test-Path $resultPath)) {
            throw "k6 did not produce $resultPath"
        }
        Copy-Item -LiteralPath $resultPath -Destination (Join-Path $runDir "results.json") -Force
        Save-DockerStats (Join-Path $runDir "stats-after.txt")
        Save-Inspect (Join-Path $runDir "inspect-after.json")
        Invoke-Compose @("logs", "--no-color") | Out-File -Encoding ASCII (Join-Path $runDir "compose.log")

        $row = Read-RunSummary $runDir $Case $Run
        $summaryRows.Add($row)
        $row | ConvertTo-Json -Depth 5 | Set-Content -Encoding ASCII (Join-Path $runDir "summary.json")
        Write-Host ("{0} run {1}: p99={2} final={3} FP/FN/Error={4}/{5}/{6} waiting_p99={7}" -f $name, $Run, $row.p99, $row.final_score, $row.FP, $row.FN, $row.Error, $row.waiting_p99)
    } finally {
        Invoke-Compose @("down", "--remove-orphans") | Out-File -Encoding ASCII (Join-Path $runDir "down-after.log")
    }
}

$baseEnv = @{
    RINHA_API_IMAGE = "rinha-c-preview:kdprimary2-l64"
    RINHA_FDLB_IMAGE = "rinha-c-fdlb:local"
    LB_CPUS = "0.05"
    LB_MEMORY = "16m"
    API_CPUS = "0.475"
    API_MEMORY = "167m"
    RINHA_API_PROCESS_MODE = "sync"
    RINHA_FDLB_STRATEGY = "round_robin"
    RINHA_FDLB_REUSEPORT = "true"
    RINHA_FDLB_LISTEN_BACKLOG = "4096"
    RINHA_API_TCP_NODELAY = "true"
    RINHA_API_TCP_QUICKACK = "true"
}

$cases = @(
    @{
        Name = "baseline"
        Env = $baseEnv + @{
            RINHA_FDLB_LEAN = "false"
            RINHA_FAST_FRAUD_PARSER = "false"
            RINHA_FDLB_TCP_DEFER_ACCEPT = "false"
            RINHA_FDLB_TCP_FASTOPEN = "false"
            RINHA_FDLB_SO_BUSY_POLL_US = "0"
        }
    },
    @{
        Name = "lean"
        Env = $baseEnv + @{
            RINHA_FDLB_LEAN = "true"
            RINHA_FAST_FRAUD_PARSER = "false"
            RINHA_FDLB_TCP_DEFER_ACCEPT = "false"
            RINHA_FDLB_TCP_FASTOPEN = "false"
            RINHA_FDLB_SO_BUSY_POLL_US = "0"
        }
    },
    @{
        Name = "lean_fast_parser"
        Env = $baseEnv + @{
            RINHA_FDLB_LEAN = "true"
            RINHA_FAST_FRAUD_PARSER = "true"
            RINHA_FDLB_TCP_DEFER_ACCEPT = "false"
            RINHA_FDLB_TCP_FASTOPEN = "false"
            RINHA_FDLB_SO_BUSY_POLL_US = "0"
        }
    },
    @{
        Name = "lean_fast_tuned"
        Env = $baseEnv + @{
            RINHA_FDLB_LEAN = "true"
            RINHA_FAST_FRAUD_PARSER = "true"
            RINHA_FDLB_TCP_DEFER_ACCEPT = "true"
            RINHA_FDLB_TCP_FASTOPEN = "false"
            RINHA_FDLB_SO_BUSY_POLL_US = "0"
        }
    }
)

if ($CaseFilter -ne "") {
    $cases = @($cases | Where-Object { $_.Name -like $CaseFilter })
    if ($cases.Count -eq 0) {
        throw "no Phase 11B cases matched CaseFilter=$CaseFilter"
    }
}

foreach ($case in $cases) {
    for ($run = 1; $run -le $Runs; $run++) {
        Invoke-Run $case $run
    }
}

$summaryRows | ConvertTo-Json -Depth 6 | Set-Content -Encoding ASCII (Join-Path $ResultRoot "summary.json")
$summaryRows | Format-Table -AutoSize | Out-String | Set-Content -Encoding ASCII (Join-Path $ResultRoot "summary.txt")
& (Join-Path $PSScriptRoot "collect-k6-details.ps1") -Root $ResultRoot | Out-File -Encoding ASCII (Join-Path $ResultRoot "collect-k6-details.log")
Write-Host "phase11b_result_root=$ResultRoot"
