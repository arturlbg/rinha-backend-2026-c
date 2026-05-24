param(
    [int]$Runs = 3,
    [switch]$IncludePerConnection,
    [switch]$IncludeTouchFalse,
    [switch]$MetricsDiagnostic,
    [switch]$SkipBuild,
    [string]$CaseFilter = ""
)

$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$OfficialTest = (Resolve-Path (Join-Path $Root "..\rinha-de-backend-2026\test")).Path
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$ResultRoot = Join-Path $Root "tmp\results\phase9b-$Stamp"
$K6Work = Join-Path $ResultRoot "k6"
$K6TestOut = Join-Path $K6Work "test"
New-Item -ItemType Directory -Force $ResultRoot, $K6Work, $K6TestOut | Out-Null

Copy-Item -LiteralPath (Join-Path $OfficialTest "test.js") -Destination (Join-Path $K6Work "test.js") -Force
Copy-Item -LiteralPath (Join-Path $OfficialTest "k6-summary.js") -Destination (Join-Path $K6Work "k6-summary.js") -Force
Copy-Item -LiteralPath (Join-Path $OfficialTest "test-data.json") -Destination (Join-Path $K6Work "test-data.json") -Force

$Request0 = Join-Path $Root "tmp\request0.json"
if (-not (Test-Path $Request0)) {
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
        "RINHA_METRICS_ENABLED"
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
    return ,$ids
}

function Save-CgroupStats([string]$ComposeFile, [string]$OutPath) {
    $ids = Invoke-Compose $ComposeFile @("ps", "-q")
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($id in $ids) {
        if ($id -eq "") { continue }
        $name = (& docker inspect -f "{{.Name}}" $id).TrimStart("/")
        $lines.Add("### $name $id")
        $stat = & docker exec $id sh -c "cat /sys/fs/cgroup/cpu.stat 2>/dev/null || cat /sys/fs/cgroup/cpu/cpu.stat 2>/dev/null || true"
        foreach ($line in $stat) {
            $lines.Add($line)
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
    for ($i = 0; $i -lt 90; $i++) {
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

function Read-ResultJson([string]$Path, [string]$Name, [int]$Run) {
    $result = Get-Content -Raw $Path | ConvertFrom-Json
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
            $process = Start-Process -FilePath "k6" `
                -ArgumentList @("run", ".\test.js") `
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

        $row = Read-ResultJson $copyPath $name $Run
        $summaryRows.Add($row)
        $row | ConvertTo-Json -Depth 5 | Set-Content -Encoding ASCII (Join-Path $runDir "summary.json")
        Write-Host ("{0} run {1}: p99={2} final={3} FP/FN/Error={4}/{5}/{6}" -f $name, $Run, $row.p99, $row.final_score, $row.FP, $row.FN, $row.Error)
    } finally {
        try {
            Invoke-Compose $composeFile @("down", "--remove-orphans") | Out-File -Encoding ASCII (Join-Path $runDir "down-after.log")
        } catch {
            "down failed: $_" | Set-Content -Encoding ASCII (Join-Path $runDir "down-error.txt")
        }
    }
}

$fdpass = "docker-compose.fdpass.yml"
$direct = "docker-compose.direct.yml"

$cases = @(
    @{
        Name = "A_avx2_fdpass"
        Compose = $fdpass
        Env = @{
            RINHA_IMAGE = "rinha-c-phase9b:local"
            RINHA_EXEC_MODE = "epoll"
            RINHA_SEARCH_IMPL = "avx2"
            RINHA_INDEX_WARMUP = "touch"
            RINHA_KDTREE_REPAIR_ENABLED = "false"
            RINHA_KDPRIMARY_TOUCH = "false"
            RINHA_KDPRIMARY2_TOUCH = "false"
            RINHA_METRICS_ENABLED = "false"
        }
    },
    @{
        Name = "B_avx2_perfect_v1"
        Compose = $fdpass
        Env = @{
            RINHA_IMAGE = "rinha-c-phase9b:local"
            RINHA_EXEC_MODE = "epoll"
            RINHA_SEARCH_IMPL = "avx2"
            RINHA_INDEX_WARMUP = "touch"
            RINHA_KDTREE_REPAIR_ENABLED = "true"
            RINHA_KDTREE_REPAIR_POLICY = "perfect_v1"
            RINHA_KDPRIMARY_TOUCH = "false"
            RINHA_KDPRIMARY2_TOUCH = "false"
            RINHA_METRICS_ENABLED = "false"
        }
    },
    @{
        Name = "C_kdprimary_v1_fdpass"
        Compose = $fdpass
        Env = @{
            RINHA_IMAGE = "rinha-c-phase9b:local"
            RINHA_EXEC_MODE = "epoll"
            RINHA_SEARCH_IMPL = "kdprimary"
            RINHA_INDEX_WARMUP = "off"
            RINHA_KDTREE_REPAIR_ENABLED = "false"
            RINHA_KDPRIMARY_TOUCH = "true"
            RINHA_KDPRIMARY2_TOUCH = "false"
            RINHA_METRICS_ENABLED = "false"
        }
    },
    @{
        Name = "D_kdprimary2_fdpass"
        Compose = $fdpass
        Env = @{
            RINHA_IMAGE = "rinha-c-phase9b:local"
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
        Name = "E_kdprimary2_direct"
        Compose = $direct
        Env = @{
            RINHA_IMAGE = "rinha-c-phase9b:local"
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

if ($IncludePerConnection) {
    $cases += @{
        Name = "F_kdprimary2_fdpass_per_connection"
        Compose = $fdpass
        Env = @{
            RINHA_IMAGE = "rinha-c-phase9b:local"
            RINHA_EXEC_MODE = "per_connection"
            RINHA_SEARCH_IMPL = "kdprimary2"
            RINHA_INDEX_WARMUP = "off"
            RINHA_KDTREE_REPAIR_ENABLED = "false"
            RINHA_KDPRIMARY_TOUCH = "false"
            RINHA_KDPRIMARY2_TOUCH = "true"
            RINHA_METRICS_ENABLED = "false"
        }
    }
}

if ($IncludeTouchFalse) {
    $cases += @{
        Name = "G_kdprimary2_fdpass_touch_false"
        Compose = $fdpass
        Env = @{
            RINHA_IMAGE = "rinha-c-phase9b:local"
            RINHA_EXEC_MODE = "epoll"
            RINHA_SEARCH_IMPL = "kdprimary2"
            RINHA_INDEX_WARMUP = "off"
            RINHA_KDTREE_REPAIR_ENABLED = "false"
            RINHA_KDPRIMARY_TOUCH = "false"
            RINHA_KDPRIMARY2_TOUCH = "false"
            RINHA_METRICS_ENABLED = "false"
        }
    }
}

if ($CaseFilter -ne "") {
    $cases = @($cases | Where-Object { $_.Name -like $CaseFilter })
    if ($cases.Count -eq 0) {
        throw "no Phase 9B cases matched CaseFilter=$CaseFilter"
    }
}

foreach ($case in $cases) {
    for ($run = 1; $run -le $Runs; $run++) {
        Invoke-MatrixRun $case $run
    }
}

if ($MetricsDiagnostic) {
    $diag = @{
        Name = "Z_kdprimary2_fdpass_metrics"
        Compose = $fdpass
        Env = @{
            RINHA_IMAGE = "rinha-c-phase9b:local"
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
$summaryRows | ConvertTo-Json -Depth 5 | Set-Content -Encoding ASCII $summaryPath
$summaryRows | Format-Table -AutoSize | Out-String | Set-Content -Encoding ASCII (Join-Path $ResultRoot "summary.txt")
Write-Host "phase9b_result_root=$ResultRoot"
