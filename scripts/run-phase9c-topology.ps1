param(
    [int]$Runs = 3,
    [switch]$MetricsDiagnostic,
    [switch]$IncludeCpuSplit049,
    [switch]$SkipBuild,
    [string]$CaseFilter = ""
)

$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$OfficialTest = (Resolve-Path (Join-Path $Root "..\rinha-de-backend-2026\test")).Path
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$ResultRoot = Join-Path $Root "tmp\results\phase9c-$Stamp"
$K6Work = Join-Path $ResultRoot "k6"
$K6TestOut = Join-Path $K6Work "test"
New-Item -ItemType Directory -Force $ResultRoot, $K6Work, $K6TestOut | Out-Null

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

function Set-MatrixEnv([hashtable]$Env) {
    $names = @(
        "RINHA_IMAGE",
        "RINHA_EXEC_MODE",
        "RINHA_SEARCH_IMPL",
        "RINHA_INDEX_WARMUP",
        "RINHA_KDTREE_REPAIR_ENABLED",
        "RINHA_KDTREE_REPAIR_POLICY",
        "RINHA_KDPRIMARY_TOUCH",
        "RINHA_KDPRIMARY2_TOUCH",
        "RINHA_METRICS_ENABLED",
        "RINHA_DIRECT_CPUS",
        "RINHA_DIRECT_MEM",
        "RINHA_LB_CPUS",
        "RINHA_LB_MEM",
        "RINHA_API_CPUS",
        "RINHA_API_MEM"
    )
    foreach ($name in $names) {
        if ($Env.ContainsKey($name)) {
            Set-Item -Path "env:$name" -Value ([string]$Env[$name])
        } else {
            Remove-Item -Path "env:$name" -ErrorAction SilentlyContinue
        }
    }
}

function Invoke-Compose([string]$ComposeFile, [string[]]$ComposeArgs) {
    & docker compose -f $ComposeFile @ComposeArgs
}

function Save-ComposeIds([string]$ComposeFile, [string]$OutPath) {
    $rawIds = @(Invoke-Compose $ComposeFile @("ps", "-q"))
    $ids = @($rawIds | Where-Object { $_ -ne "" })
    $ids | Set-Content -Encoding ASCII $OutPath
    return $ids
}

function Save-CgroupStats([string]$ComposeFile, [string]$OutPath) {
    $ids = @(Save-ComposeIds $ComposeFile ($OutPath + ".ids"))
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($id in $ids) {
        if ($id -eq "") { continue }
        $name = (& docker inspect -f "{{.Name}}" $id).TrimStart("/")
        $lines.Add("### $name $id")
        try {
            $stat = & docker exec $id sh -c "cat /sys/fs/cgroup/cpu.stat 2>/dev/null || cat /sys/fs/cgroup/cpu/cpu.stat 2>/dev/null || true" 2>$null
            if ($LASTEXITCODE -ne 0 -or $null -eq $stat) {
                $lines.Add("cpu.stat unavailable")
            } else {
                foreach ($line in $stat) {
                    $lines.Add($line)
                }
            }
        } catch {
            $lines.Add("cpu.stat unavailable: $_")
        }
    }
    $lines | Set-Content -Encoding ASCII $OutPath
}

function Save-DockerStats([string]$ComposeFile, [string]$OutPath) {
    $ids = @(Save-ComposeIds $ComposeFile ($OutPath + ".ids"))
    if ($ids.Count -eq 0) {
        "" | Set-Content -Encoding ASCII $OutPath
        return
    }
    & docker stats --no-stream --format "table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.PIDs}}" @ids |
        Set-Content -Encoding ASCII $OutPath
}

function Wait-Ready([string]$OutPath) {
    for ($i = 0; $i -lt 120; $i++) {
        try {
            $body = & curl.exe -s --max-time 2 "http://localhost:9999/ready"
            if ($LASTEXITCODE -eq 0 -and $body -eq "ok") {
                "ready after ${i}s" | Set-Content -Encoding ASCII $OutPath
                return
            }
        } catch {
        }
        Start-Sleep -Seconds 1
    }
    throw "service did not become ready"
}

function Save-DebugInfo([string]$OutPath) {
    try {
        & curl.exe -s --max-time 3 "http://localhost:9999/debug/info" | Set-Content -Encoding ASCII $OutPath
    } catch {
        "debug unavailable: $_" | Set-Content -Encoding ASCII $OutPath
    }
}

function Save-Smoke([string]$OutPath) {
    $ready = & curl.exe -i -s --max-time 3 "http://localhost:9999/ready"
    $fraud = & curl.exe -i -s --max-time 5 -X POST "http://localhost:9999/fraud-score" -H "Content-Type: application/json" --data-binary "@$Request0"
    @("### ready", $ready, "### fraud-score", $fraud) | Set-Content -Encoding ASCII $OutPath
}

function Get-K6MetricValue($Summary, [string]$MetricName, [string]$ValueName) {
    if ($null -eq $Summary -or $null -eq $Summary.metrics) {
        return $null
    }
    $metric = $Summary.metrics.PSObject.Properties[$MetricName]
    if ($null -eq $metric) {
        return $null
    }
    $source = $metric.Value
    if ($null -ne $metric.Value.values) {
        $source = $metric.Value.values
    }
    $value = $source.PSObject.Properties[$ValueName]
    if ($null -eq $value) {
        return $null
    }
    return $value.Value
}

function Read-ResultJson([string]$Path, [string]$SummaryPath, [string]$Name, [int]$Run) {
    $result = Get-Content -Raw $Path | ConvertFrom-Json
    $k6Summary = $null
    if (Test-Path $SummaryPath) {
        $k6Summary = Get-Content -Raw $SummaryPath | ConvertFrom-Json
    }
    return [PSCustomObject]@{
        name = $Name
        run = $Run
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
        http_req_duration_p99 = Get-K6MetricValue $k6Summary "http_req_duration" "p(99)"
        http_req_waiting_p99 = Get-K6MetricValue $k6Summary "http_req_waiting" "p(99)"
        http_req_blocked_p99 = Get-K6MetricValue $k6Summary "http_req_blocked" "p(99)"
        http_req_connecting_p99 = Get-K6MetricValue $k6Summary "http_req_connecting" "p(99)"
        http_req_sending_p99 = Get-K6MetricValue $k6Summary "http_req_sending" "p(99)"
        http_req_receiving_p99 = Get-K6MetricValue $k6Summary "http_req_receiving" "p(99)"
        iteration_duration_p99 = Get-K6MetricValue $k6Summary "iteration_duration" "p(99)"
    }
}

function Invoke-MatrixRun([hashtable]$Case, [int]$Run) {
    $name = [string]$Case.Name
    $composeFile = Join-Path $Root ([string]$Case.Compose)
    $runDir = Join-Path $ResultRoot ("{0}-run{1}" -f $name, $Run)
    New-Item -ItemType Directory -Force $runDir | Out-Null

    Set-MatrixEnv $Case.Env
    try {
        Invoke-Compose $composeFile @("down", "--remove-orphans") | Out-File -Encoding ASCII (Join-Path $runDir "down-before.log")
        if (-not $SkipBuild) {
            Invoke-Compose $composeFile @("up", "--build", "-d") | Out-File -Encoding ASCII (Join-Path $runDir "up.log")
        } else {
            Invoke-Compose $composeFile @("up", "-d") | Out-File -Encoding ASCII (Join-Path $runDir "up.log")
        }
        Wait-Ready (Join-Path $runDir "ready.txt")
        Save-Smoke (Join-Path $runDir "smoke.txt")
        Save-DebugInfo (Join-Path $runDir "debug-before.txt")
        Save-CgroupStats $composeFile (Join-Path $runDir "cpu-before.txt")
        Save-DockerStats $composeFile (Join-Path $runDir "stats-before.txt")

        Remove-Item -LiteralPath (Join-Path $K6TestOut "results.json") -ErrorAction SilentlyContinue
        Push-Location $K6Work
        try {
            $k6Log = Join-Path $runDir "k6.log"
            $k6Stdout = Join-Path $runDir "k6.stdout.log"
            $k6Stderr = Join-Path $runDir "k6.stderr.log"
            $k6SummaryFull = Join-Path $runDir "k6-summary-full.json"
            $process = Start-Process -FilePath "k6" `
                -ArgumentList @("run", "--summary-export", $k6SummaryFull, ".\test.js") `
                -NoNewWindow `
                -Wait `
                -PassThru `
                -RedirectStandardOutput $k6Stdout `
                -RedirectStandardError $k6Stderr
            Get-Content -LiteralPath $k6Stdout, $k6Stderr -ErrorAction SilentlyContinue |
                Set-Content -Encoding ASCII $k6Log
            if ($process.ExitCode -ne 0) {
                throw "k6 failed with exit code $($process.ExitCode)"
            }
        } finally {
            Pop-Location
        }
        $resultPath = Join-Path $K6TestOut "results.json"
        if (-not (Test-Path $resultPath)) {
            throw "k6 did not produce $resultPath"
        }
        $copyPath = Join-Path $runDir "results.json"
        Copy-Item -LiteralPath $resultPath -Destination $copyPath -Force

        Save-CgroupStats $composeFile (Join-Path $runDir "cpu-after.txt")
        Save-DockerStats $composeFile (Join-Path $runDir "stats-after.txt")
        Save-DebugInfo (Join-Path $runDir "debug-after.txt")
        Invoke-Compose $composeFile @("logs", "--no-color") | Out-File -Encoding ASCII (Join-Path $runDir "compose.log")

        $row = Read-ResultJson $copyPath (Join-Path $runDir "k6-summary-full.json") $name $Run
        $summaryRows.Add($row)
        $row | ConvertTo-Json -Depth 6 | Set-Content -Encoding ASCII (Join-Path $runDir "summary.json")
        Write-Host ("{0} run {1}: p99={2} final={3} waiting_p99={4} FP/FN/Error={5}/{6}/{7}" -f $name, $Run, $row.p99, $row.final_score, $row.http_req_waiting_p99, $row.FP, $row.FN, $row.Error)
    } finally {
        try {
            Invoke-Compose $composeFile @("down", "--remove-orphans") | Out-File -Encoding ASCII (Join-Path $runDir "down-after.log")
        } catch {
            "down failed: $_" | Set-Content -Encoding ASCII (Join-Path $runDir "down-error.txt")
        }
    }
}

$fdpass = "docker-compose.fdpass.yml"
$fdpass3 = "docker-compose.fdpass3.yml"
$direct = "docker-compose.direct.yml"
$image = "rinha-c-phase9c:local"

$cases = @(
    @{
        Name = "direct_0475"
        Compose = $direct
        Env = @{
            RINHA_IMAGE = $image
            RINHA_DIRECT_CPUS = "0.475"
            RINHA_DIRECT_MEM = "167m"
            RINHA_EXEC_MODE = "per_connection"
            RINHA_SEARCH_IMPL = "kdprimary2"
            RINHA_INDEX_WARMUP = "off"
            RINHA_KDTREE_REPAIR_ENABLED = "false"
            RINHA_KDPRIMARY_TOUCH = "false"
            RINHA_KDPRIMARY2_TOUCH = "true"
            RINHA_METRICS_ENABLED = "false"
        }
    },
    @{
        Name = "direct_095"
        Compose = $direct
        Env = @{
            RINHA_IMAGE = $image
            RINHA_DIRECT_CPUS = "0.95"
            RINHA_DIRECT_MEM = "320m"
            RINHA_EXEC_MODE = "per_connection"
            RINHA_SEARCH_IMPL = "kdprimary2"
            RINHA_INDEX_WARMUP = "off"
            RINHA_KDTREE_REPAIR_ENABLED = "false"
            RINHA_KDPRIMARY_TOUCH = "false"
            RINHA_KDPRIMARY2_TOUCH = "true"
            RINHA_METRICS_ENABLED = "false"
        }
    },
    @{
        Name = "fdpass2_epoll"
        Compose = $fdpass
        Env = @{
            RINHA_IMAGE = $image
            RINHA_LB_CPUS = "0.05"
            RINHA_LB_MEM = "16m"
            RINHA_API_CPUS = "0.475"
            RINHA_API_MEM = "167m"
            RINHA_EXEC_MODE = "epoll"
            RINHA_SEARCH_IMPL = "kdprimary2"
            RINHA_INDEX_WARMUP = "off"
            RINHA_KDTREE_REPAIR_ENABLED = "false"
            RINHA_KDPRIMARY_TOUCH = "false"
            RINHA_KDPRIMARY2_TOUCH = "true"
            RINHA_METRICS_ENABLED = "false"
        }
    },
    @{
        Name = "fdpass3_epoll"
        Compose = $fdpass3
        Env = @{
            RINHA_IMAGE = $image
            RINHA_LB_CPUS = "0.04"
            RINHA_LB_MEM = "12m"
            RINHA_API_CPUS = "0.32"
            RINHA_API_MEM = "112m"
            RINHA_EXEC_MODE = "epoll"
            RINHA_SEARCH_IMPL = "kdprimary2"
            RINHA_INDEX_WARMUP = "off"
            RINHA_KDTREE_REPAIR_ENABLED = "false"
            RINHA_KDPRIMARY_TOUCH = "false"
            RINHA_KDPRIMARY2_TOUCH = "true"
            RINHA_METRICS_ENABLED = "false"
        }
    },
    @{
        Name = "fdpass2_per_connection"
        Compose = $fdpass
        Env = @{
            RINHA_IMAGE = $image
            RINHA_LB_CPUS = "0.05"
            RINHA_LB_MEM = "16m"
            RINHA_API_CPUS = "0.475"
            RINHA_API_MEM = "167m"
            RINHA_EXEC_MODE = "per_connection"
            RINHA_SEARCH_IMPL = "kdprimary2"
            RINHA_INDEX_WARMUP = "off"
            RINHA_KDTREE_REPAIR_ENABLED = "false"
            RINHA_KDPRIMARY_TOUCH = "false"
            RINHA_KDPRIMARY2_TOUCH = "true"
            RINHA_METRICS_ENABLED = "false"
        }
    }
)

if ($IncludeCpuSplit049) {
    $cases += @{
        Name = "fdpass2_epoll_api049"
        Compose = $fdpass
        Env = @{
            RINHA_IMAGE = $image
            RINHA_LB_CPUS = "0.02"
            RINHA_LB_MEM = "12m"
            RINHA_API_CPUS = "0.49"
            RINHA_API_MEM = "169m"
            RINHA_EXEC_MODE = "epoll"
            RINHA_SEARCH_IMPL = "kdprimary2"
            RINHA_INDEX_WARMUP = "off"
            RINHA_KDTREE_REPAIR_ENABLED = "false"
            RINHA_KDPRIMARY_TOUCH = "false"
            RINHA_KDPRIMARY2_TOUCH = "true"
            RINHA_METRICS_ENABLED = "false"
        }
    }
}

if ($CaseFilter -ne "") {
    $cases = @($cases | Where-Object { $_.Name -like $CaseFilter })
    if ($cases.Count -eq 0) {
        throw "no Phase 9C cases matched CaseFilter=$CaseFilter"
    }
}

foreach ($case in $cases) {
    for ($run = 1; $run -le $Runs; $run++) {
        Invoke-MatrixRun $case $run
    }
}

if ($MetricsDiagnostic) {
    $diag = @{
        Name = "fdpass2_epoll_metrics"
        Compose = $fdpass
        Env = @{
            RINHA_IMAGE = $image
            RINHA_LB_CPUS = "0.05"
            RINHA_LB_MEM = "16m"
            RINHA_API_CPUS = "0.475"
            RINHA_API_MEM = "167m"
            RINHA_EXEC_MODE = "epoll"
            RINHA_SEARCH_IMPL = "kdprimary2"
            RINHA_INDEX_WARMUP = "off"
            RINHA_KDTREE_REPAIR_ENABLED = "false"
            RINHA_KDPRIMARY_TOUCH = "false"
            RINHA_KDPRIMARY2_TOUCH = "true"
            RINHA_METRICS_ENABLED = "true"
        }
    }
    Invoke-MatrixRun $diag 1
}

$summaryPath = Join-Path $ResultRoot "summary.json"
$summaryRows | ConvertTo-Json -Depth 6 | Set-Content -Encoding ASCII $summaryPath
$summaryRows | Format-Table -AutoSize | Out-String | Set-Content -Encoding ASCII (Join-Path $ResultRoot "summary.txt")
& (Join-Path $PSScriptRoot "collect-k6-details.ps1") -Root $ResultRoot | Out-File -Encoding ASCII (Join-Path $ResultRoot "collect-k6-details.log")
Write-Host "phase9c_result_root=$ResultRoot"
