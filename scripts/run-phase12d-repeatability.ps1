param(
    [ValidateSet("default", "reverse", "both")]
    [string]$Order = "default",
    [int]$CooldownSeconds = 60,
    [string]$KdPrimary2Image = "rinha-c-preview:kdprimary2-l64",
    [string]$KdClass3Image = "rinha-c-preview:kdclass3-l64",
    [string]$FdlbImage = "rinha-c-fdlb:local"
)

$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$OfficialTest = (Resolve-Path (Join-Path $Root "..\rinha-de-backend-2026\test")).Path
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$ResultRoot = Join-Path $Root "tmp\results\phase12d-$Stamp"
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

function Clear-CaseEnv {
    $names = @(
        "RINHA_API_IMAGE",
        "RINHA_FDLB_IMAGE",
        "RINHA_METRICS_ENABLED",
        "RINHA_DEBUG_TIMING",
        "RINHA_KDCLASS3_TOUCH",
        "RINHA_KDCLASS3_FALLBACK",
        "LB_CPUS",
        "LB_MEMORY",
        "API_CPUS",
        "API_MEMORY"
    )
    foreach ($name in $names) {
        Remove-Item -Path "env:$name" -ErrorAction SilentlyContinue
    }
}

function Set-CaseEnv([hashtable]$Env) {
    Clear-CaseEnv
    foreach ($name in $Env.Keys) {
        Set-Item -Path "env:$name" -Value ([string]$Env[$name])
    }
}

function Get-CaseConfig([string]$Name) {
    $base = @{
        RINHA_FDLB_IMAGE = $FdlbImage
        RINHA_METRICS_ENABLED = "false"
        RINHA_DEBUG_TIMING = "false"
        LB_CPUS = "0.05"
        LB_MEMORY = "16m"
        API_CPUS = "0.475"
        API_MEMORY = "167m"
    }

    switch ($Name) {
        "kdprimary2" {
            return @{
                Name = "kdprimary2"
                Mode = "kdprimary2"
                Compose = Join-Path $Root "docker-compose.preview-cfdlb.yml"
                SearchImpl = "kdprimary2"
                KdClass3Touch = ""
                KdClass3Fallback = ""
                Image = $KdPrimary2Image
                Env = $base + @{ RINHA_API_IMAGE = $KdPrimary2Image }
            }
        }
        "kdclass3-touch" {
            return @{
                Name = "kdclass3-touch"
                Mode = "kdclass3-touch"
                Compose = Join-Path $Root "docker-compose.preview-kdclass3.yml"
                SearchImpl = "kdclass3"
                KdClass3Touch = "true"
                KdClass3Fallback = "none"
                Image = $KdClass3Image
                Env = $base + @{
                    RINHA_API_IMAGE = $KdClass3Image
                    RINHA_KDCLASS3_TOUCH = "true"
                    RINHA_KDCLASS3_FALLBACK = "none"
                }
            }
        }
        "kdclass3-notouch" {
            return @{
                Name = "kdclass3-notouch"
                Mode = "kdclass3-notouch"
                Compose = Join-Path $Root "docker-compose.preview-kdclass3.yml"
                SearchImpl = "kdclass3"
                KdClass3Touch = "false"
                KdClass3Fallback = "none"
                Image = $KdClass3Image
                Env = $base + @{
                    RINHA_API_IMAGE = $KdClass3Image
                    RINHA_KDCLASS3_TOUCH = "false"
                    RINHA_KDCLASS3_FALLBACK = "none"
                }
            }
        }
    }
}

function Invoke-Compose([string]$ComposeFile, [string[]]$ComposeArgs) {
    & docker compose -f $ComposeFile @ComposeArgs
}

function Wait-Ready {
    for ($i = 0; $i -lt 120; $i++) {
        $body = & curl.exe -s --max-time 2 "http://localhost:9999/ready"
        if ($LASTEXITCODE -eq 0 -and $body -eq "ok") {
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
        if ([string]::IsNullOrWhiteSpace($body)) {
            $bodies["<empty>"] = 1
        } else {
            $bodies[[string]$body] = 1
        }
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

function Save-DockerStats([string]$ComposeFile, [string]$OutPath) {
    $ids = @((Invoke-Compose $ComposeFile @("ps", "-q")) | Where-Object { $_ -ne "" })
    if ($ids.Count -eq 0) {
        "" | Set-Content -Encoding ASCII $OutPath
        return
    }
    & docker stats --no-stream --format "table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.PIDs}}" @ids |
        Set-Content -Encoding ASCII $OutPath
}

function Save-Inspect([string]$ComposeFile, [string]$OutPath) {
    $ids = @((Invoke-Compose $ComposeFile @("ps", "-q")) | Where-Object { $_ -ne "" })
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

function Read-RunSummary([string]$RunDir, [hashtable]$CaseConfig, [int]$Index) {
    $result = Get-Content -Raw (Join-Path $RunDir "results.json") | ConvertFrom-Json
    $summary = Get-Content -Raw (Join-Path $RunDir "k6-summary-full.json") | ConvertFrom-Json
    return [PSCustomObject]@{
        index = $Index
        timestamp = (Get-Date -Format o)
        name = $CaseConfig.Name
        mode = $CaseConfig.Mode
        image = $CaseConfig.Image
        search_impl = $CaseConfig.SearchImpl
        kdclass3_touch = $CaseConfig.KdClass3Touch
        kdclass3_fallback = $CaseConfig.KdClass3Fallback
        debug_timing = "false"
        metrics_enabled = "false"
        lb_cpus = $CaseConfig.Env.LB_CPUS
        api_cpus = $CaseConfig.Env.API_CPUS
        lb_memory = $CaseConfig.Env.LB_MEMORY
        api_memory = $CaseConfig.Env.API_MEMORY
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

function Invoke-Run([hashtable]$CaseConfig, [int]$Index, [int]$Total) {
    $compose = $CaseConfig.Compose
    $name = $CaseConfig.Name
    $runDir = Join-Path $ResultRoot ("{0:00}-{1}" -f $Index, $name)
    New-Item -ItemType Directory -Force $runDir | Out-Null
    Set-CaseEnv $CaseConfig.Env

    try {
        $envReport = [ordered]@{
            RINHA_API_IMAGE = $CaseConfig.Env.RINHA_API_IMAGE
            RINHA_FDLB_IMAGE = $CaseConfig.Env.RINHA_FDLB_IMAGE
            RINHA_METRICS_ENABLED = $CaseConfig.Env.RINHA_METRICS_ENABLED
            RINHA_DEBUG_TIMING = $CaseConfig.Env.RINHA_DEBUG_TIMING
            RINHA_KDCLASS3_TOUCH = $CaseConfig.Env.RINHA_KDCLASS3_TOUCH
            RINHA_KDCLASS3_FALLBACK = $CaseConfig.Env.RINHA_KDCLASS3_FALLBACK
            LB_CPUS = $CaseConfig.Env.LB_CPUS
            LB_MEMORY = $CaseConfig.Env.LB_MEMORY
            API_CPUS = $CaseConfig.Env.API_CPUS
            API_MEMORY = $CaseConfig.Env.API_MEMORY
        }
        $envReport | ConvertTo-Json -Depth 3 | Set-Content -Encoding ASCII (Join-Path $runDir "env.json")

        Invoke-Compose $compose @("down", "--remove-orphans") | Out-File -Encoding ASCII (Join-Path $runDir "down-before.log")
        Invoke-Compose $compose @("up", "-d", "--no-build") | Out-File -Encoding ASCII (Join-Path $runDir "up.log")
        Wait-Ready
        Save-Smoke (Join-Path $runDir "smoke.txt")
        Save-DockerStats $compose (Join-Path $runDir "stats-before.txt")
        Save-Inspect $compose (Join-Path $runDir "inspect-before.json")

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
        Save-DockerStats $compose (Join-Path $runDir "stats-after.txt")
        Save-Inspect $compose (Join-Path $runDir "inspect-after.json")
        Invoke-Compose $compose @("logs", "--no-color") | Out-File -Encoding ASCII (Join-Path $runDir "compose.log")

        $row = Read-RunSummary $runDir $CaseConfig $Index
        $row | ConvertTo-Json -Depth 5 | Set-Content -Encoding ASCII (Join-Path $runDir "summary.json")
        Write-Host ("{0}/{1} {2}: p99={3} waiting_p99={4} final={5} FP/FN/Error={6}/{7}/{8}" -f $Index, $Total, $name, $row.p99, $row.waiting_p99, $row.final_score, $row.FP, $row.FN, $row.Error)
        return $row
    } finally {
        Invoke-Compose $compose @("down", "--remove-orphans") | Out-File -Encoding ASCII (Join-Path $runDir "down-after.log")
    }
}

function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) {
        return $null
    }
    $sorted = @($Values | Sort-Object)
    $mid = [int]($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) {
        return $sorted[$mid]
    }
    return ($sorted[$mid - 1] + $sorted[$mid]) / 2.0
}

function New-ModeSummary($Rows, [string]$Mode) {
    $items = @($Rows | Where-Object { $_.mode -eq $Mode })
    if ($items.Count -eq 0) {
        return $null
    }
    $p99s = [double[]]@($items | ForEach-Object { [double]$_.p99 })
    $scores = [double[]]@($items | ForEach-Object { [double]$_.final_score })
    return [PSCustomObject]@{
        mode = $Mode
        runs = $items.Count
        best_p99 = ($p99s | Measure-Object -Minimum).Minimum
        median_p99 = Get-Median $p99s
        worst_p99 = ($p99s | Measure-Object -Maximum).Maximum
        best_score = ($scores | Measure-Object -Maximum).Maximum
        median_score = Get-Median $scores
        http_errors = ($items | Measure-Object -Property Error -Sum).Sum
        fp = ($items | Measure-Object -Property FP -Sum).Sum
        fn = ($items | Measure-Object -Property FN -Sum).Sum
    }
}

function Write-Decision($Rows, $ModeSummaries) {
    $kdprimary = $ModeSummaries | Where-Object { $_.mode -eq "kdprimary2" } | Select-Object -First 1
    $kdclassTouch = $ModeSummaries | Where-Object { $_.mode -eq "kdclass3-touch" } | Select-Object -First 1
    $kdclassNoTouch = $ModeSummaries | Where-Object { $_.mode -eq "kdclass3-notouch" } | Select-Object -First 1
    $decision = "insufficient_data"
    $recommendation = "Run the repeatability matrix again before changing the canary status."

    if ($null -ne $kdprimary -and $null -ne $kdclassTouch) {
        $pairedPrimary = @($Rows | Where-Object { $_.mode -eq "kdprimary2" })
        $pairedClass = @($Rows | Where-Object { $_.mode -eq "kdclass3-touch" })
        $classBest = [double]$kdclassTouch.best_p99
        $primaryBest = [double]$kdprimary.best_p99
        $classMedian = [double]$kdclassTouch.median_p99
        $primaryMedian = [double]$kdprimary.median_p99
        $errorsClean = (($kdclassTouch.http_errors -eq 0) -and ($kdclassTouch.fp -eq 0) -and ($kdclassTouch.fn -eq 0))

        if ($errorsClean -and $classBest -lt $primaryBest -and $classMedian -lt $primaryMedian) {
            if ($classBest -le 1.82 -or $classMedian -le 1.82) {
                $decision = "advance_to_official_canary"
                $recommendation = "kdclass3 touch=true beat kdprimary2 and is near/below the official checkpoint; consider an official canary after one more clean run."
            } else {
                $decision = "keep_canary_then_phase13a"
                $recommendation = "kdclass3 touch=true beat kdprimary2 locally, but remains above official checkpoint p99=1.82ms; keep it canary and move to LB/runtime attribution."
            }
        } elseif ($errorsClean -and $classBest -lt $primaryBest) {
            $decision = "inconsistent_repeatability"
            $recommendation = "kdclass3 has a better best p99 but not a better median; repeat under cooler/more controlled conditions before changing defaults."
        } else {
            $decision = "remain_opt_in_canary"
            $recommendation = "kdclass3 did not consistently beat kdprimary2 in this non-debug matrix; keep kdprimary2 default."
        }

        if ($null -ne $kdclassNoTouch -and $kdclassTouch.best_p99 -le ($kdclassNoTouch.best_p99 + 0.05)) {
            $recommendation += " touch=true remains preferred because it is the candidate setting and avoids cold-page risk."
        }
    }

    $decisionObj = [PSCustomObject]@{
        decision = $decision
        recommendation = $recommendation
        official_checkpoint_p99 = 1.82
        official_checkpoint_score = 5740.81
    }
    $decisionObj | ConvertTo-Json -Depth 4 | Set-Content -Encoding ASCII (Join-Path $ResultRoot "decision.json")
    @(
        "decision=$($decisionObj.decision)",
        "recommendation=$($decisionObj.recommendation)",
        "official_checkpoint_p99=$($decisionObj.official_checkpoint_p99)",
        "official_checkpoint_score=$($decisionObj.official_checkpoint_score)"
    ) | Set-Content -Encoding ASCII (Join-Path $ResultRoot "decision.txt")
    Write-Host "decision=$($decisionObj.decision)"
    Write-Host "recommendation=$($decisionObj.recommendation)"
}

$defaultOrder = @("kdprimary2", "kdclass3-touch", "kdclass3-notouch", "kdclass3-touch", "kdprimary2")
$reverseOrder = @("kdclass3-touch", "kdprimary2", "kdprimary2", "kdclass3-touch")
$caseNames = switch ($Order) {
    "default" { $defaultOrder }
    "reverse" { $reverseOrder }
    "both" { $defaultOrder + $reverseOrder }
}

$summaryRows = New-Object System.Collections.Generic.List[object]
$total = $caseNames.Count
for ($i = 0; $i -lt $caseNames.Count; $i++) {
    $caseConfig = Get-CaseConfig $caseNames[$i]
    $row = Invoke-Run $caseConfig ($i + 1) $total
    $summaryRows.Add($row)
    if ($CooldownSeconds -gt 0 -and $i -lt ($caseNames.Count - 1)) {
        Write-Host "cooldown_seconds=$CooldownSeconds"
        Start-Sleep -Seconds $CooldownSeconds
    }
}

$summaryRows | ConvertTo-Json -Depth 6 | Set-Content -Encoding ASCII (Join-Path $ResultRoot "summary.json")
$summaryRows | Format-Table -AutoSize | Out-String | Set-Content -Encoding ASCII (Join-Path $ResultRoot "summary.txt")
$summaryRows | Format-Table -AutoSize

$modes = @("kdprimary2", "kdclass3-touch", "kdclass3-notouch")
$modeSummaries = New-Object System.Collections.Generic.List[object]
foreach ($mode in $modes) {
    $modeSummary = New-ModeSummary $summaryRows $mode
    if ($null -ne $modeSummary) {
        $modeSummaries.Add($modeSummary)
    }
}
$modeSummaries | ConvertTo-Json -Depth 5 | Set-Content -Encoding ASCII (Join-Path $ResultRoot "mode-summary.json")
$modeSummaries | Format-Table -AutoSize | Out-String | Set-Content -Encoding ASCII (Join-Path $ResultRoot "mode-summary.txt")
$modeSummaries | Format-Table -AutoSize
Write-Decision $summaryRows $modeSummaries

& (Join-Path $PSScriptRoot "collect-k6-details.ps1") -Root $ResultRoot | Out-File -Encoding ASCII (Join-Path $ResultRoot "collect-k6-details.log")

Clear-CaseEnv
Write-Host "phase12d_result_root=$ResultRoot"

