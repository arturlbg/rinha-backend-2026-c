param(
    [ValidateSet("kdprimary2", "kdclass3-touch", "kdclass3-notouch", "kdclass3-fallback", "all")]
    [string]$Case = "kdprimary2",
    [int]$Runs = 1,
    [switch]$SkipK6,
    [string]$KdPrimary2Image = "rinha-c-preview:kdprimary2-l64-debug",
    [string]$KdClass3Image = "rinha-c-preview:kdclass3-l64-debug",
    [string]$KdClass3FallbackImage = "rinha-c-preview:kdclass3-l64-debug-both",
    [string]$FdlbImage = "rinha-c-fdlb:local"
)

$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$OfficialTest = (Resolve-Path (Join-Path $Root "..\rinha-de-backend-2026\test")).Path
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$ResultRoot = Join-Path $Root "tmp\results\phase12c-$Stamp"
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
        RINHA_METRICS_ENABLED = "true"
        RINHA_DEBUG_TIMING = "true"
        LB_CPUS = "0.05"
        LB_MEMORY = "16m"
        API_CPUS = "0.475"
        API_MEMORY = "167m"
    }

    switch ($Name) {
        "kdprimary2" {
            return @{
                Name = "kdprimary2"
                Compose = Join-Path $Root "docker-compose.preview-cfdlb.yml"
                Env = $base + @{ RINHA_API_IMAGE = $KdPrimary2Image }
            }
        }
        "kdclass3-touch" {
            return @{
                Name = "kdclass3-touch"
                Compose = Join-Path $Root "docker-compose.preview-kdclass3.yml"
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
                Compose = Join-Path $Root "docker-compose.preview-kdclass3.yml"
                Env = $base + @{
                    RINHA_API_IMAGE = $KdClass3Image
                    RINHA_KDCLASS3_TOUCH = "false"
                    RINHA_KDCLASS3_FALLBACK = "none"
                }
            }
        }
        "kdclass3-fallback" {
            return @{
                Name = "kdclass3-fallback"
                Compose = Join-Path $Root "docker-compose.preview-kdclass3.yml"
                Env = $base + @{
                    RINHA_API_IMAGE = $KdClass3FallbackImage
                    RINHA_KDCLASS3_TOUCH = "true"
                    RINHA_KDCLASS3_FALLBACK = "kdprimary2"
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

function Invoke-Warmup {
    for ($i = 0; $i -lt 50; $i++) {
        & curl.exe -s --max-time 5 -X POST "http://localhost:9999/fraud-score" `
            -H "Content-Type: application/json" `
            --data-binary "@$Request0" | Out-Null
    }
}

function Parse-DebugText([string]$Text) {
    $map = @{}
    foreach ($line in ($Text -split "`n")) {
        $trimmed = $line.Trim()
        if ($trimmed -eq "" -or -not $trimmed.Contains("=")) {
            continue
        }
        $parts = $trimmed.Split("=", 2)
        $map[$parts[0]] = $parts[1]
    }
    return $map
}

function Capture-Debug([string]$OutDir, [string]$Prefix) {
    $byInstance = @{}
    for ($i = 0; $i -lt 24; $i++) {
        $text = (& curl.exe -s --max-time 3 "http://localhost:9999/debug/info") -join "`n"
        $rawPath = Join-Path $OutDir ("{0}-debug-{1}.txt" -f $Prefix, $i)
        $text | Set-Content -Encoding ASCII $rawPath
        $map = Parse-DebugText $text
        $instance = $map["debug_instance"]
        if ($null -eq $instance -or $instance -eq "") {
            $instance = "unknown"
        }
        $byInstance[$instance] = $map
        if ($byInstance.ContainsKey("api1") -and $byInstance.ContainsKey("api2")) {
            break
        }
        Start-Sleep -Milliseconds 100
    }
    $byInstance | ConvertTo-Json -Depth 5 | Set-Content -Encoding ASCII (Join-Path $OutDir "$Prefix-debug.json")
    return $byInstance
}

function To-Number($Value) {
    if ($null -eq $Value) {
        return $null
    }
    $n = 0.0
    if ([double]::TryParse([string]$Value, [ref]$n)) {
        return $n
    }
    return $null
}

function Add-CounterDelta([System.Collections.Generic.List[object]]$Rows,
                          [string]$Instance,
                          [hashtable]$Before,
                          [hashtable]$After,
                          [string]$Key) {
    $b = To-Number $Before[$Key]
    $a = To-Number $After[$Key]
    if ($null -ne $a -and $null -ne $b) {
        $Rows.Add([PSCustomObject]@{
            instance = $Instance
            metric = $Key
            delta_count = $a - $b
            delta_total_ns = $null
            delta_avg_ns = $null
            after_max_ns = $null
        })
    }
}

function Add-TimingDelta([System.Collections.Generic.List[object]]$Rows,
                         [string]$Instance,
                         [hashtable]$Before,
                         [hashtable]$After,
                         [string]$Prefix) {
    $bc = To-Number $Before["${Prefix}_count"]
    $ac = To-Number $After["${Prefix}_count"]
    $bt = To-Number $Before["${Prefix}_total_ns"]
    $at = To-Number $After["${Prefix}_total_ns"]
    $am = To-Number $After["${Prefix}_max_ns"]
    if ($null -eq $bc -or $null -eq $ac -or $null -eq $bt -or $null -eq $at) {
        return
    }
    $dc = $ac - $bc
    $dt = $at - $bt
    $avg = $null
    if ($dc -gt 0) {
        $avg = $dt / $dc
    }
    $Rows.Add([PSCustomObject]@{
        instance = $Instance
        metric = $Prefix
        delta_count = $dc
        delta_total_ns = $dt
        delta_avg_ns = $avg
        after_max_ns = $am
    })
}

function Write-DebugDeltas([string]$OutDir, [hashtable]$Before, [hashtable]$After) {
    $rows = New-Object System.Collections.Generic.List[object]
    foreach ($instance in $After.Keys) {
        if (-not $Before.ContainsKey($instance)) {
            continue
        }
        $b = $Before[$instance]
        $a = $After[$instance]
        foreach ($key in @(
            "request_count",
            "fraud_count",
            "kdprimary2_search_count",
            "kdclass3_search_count",
            "kdclass3_fallback_count",
            "kdclass3_fraud_decisions",
            "kdclass3_legit_decisions",
            "write_errors",
            "malformed_requests"
        )) {
            Add-CounterDelta $rows $instance $b $a $key
        }
        foreach ($prefix in @(
            "timing_fraud_handler",
            "timing_http_parse",
            "timing_search",
            "timing_write_response"
        )) {
            Add-TimingDelta $rows $instance $b $a $prefix
        }
    }
    $rows | ConvertTo-Json -Depth 5 | Set-Content -Encoding ASCII (Join-Path $OutDir "debug-deltas.json")
    $rows | Format-Table -AutoSize | Out-String | Set-Content -Encoding ASCII (Join-Path $OutDir "debug-deltas.txt")
    $rows | Format-Table -AutoSize
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

function Read-RunSummary([string]$RunDir, [string]$Name, [int]$Run) {
    $result = Get-Content -Raw (Join-Path $RunDir "results.json") | ConvertFrom-Json
    $summary = Get-Content -Raw (Join-Path $RunDir "k6-summary-full.json") | ConvertFrom-Json
    return [PSCustomObject]@{
        name = $Name
        run = $Run
        p99 = $result.p99
        final_score = $result.scoring.final_score
        p99_score = $result.scoring.p99_score.value
        detection_score = $result.scoring.detection_score.value
        FP = $result.scoring.breakdown.false_positive_detections
        FN = $result.scoring.breakdown.false_negative_detections
        Error = $result.scoring.breakdown.http_errors
        failure_rate = $result.scoring.failure_rate
        waiting_p99 = Get-MetricValue $summary "http_req_waiting" "p(99)"
        duration_p99 = Get-MetricValue $summary "http_req_duration" "p(99)"
    }
}

function Invoke-CaseRun([hashtable]$Config, [int]$Run) {
    $name = $Config.Name
    $compose = $Config.Compose
    $runDir = Join-Path $ResultRoot ("{0}-run{1}" -f $name, $Run)
    New-Item -ItemType Directory -Force $runDir | Out-Null
    Set-CaseEnv $Config.Env
    try {
        Invoke-Compose $compose @("down", "--remove-orphans") | Out-File -Encoding ASCII (Join-Path $runDir "down-before.log")
        Invoke-Compose $compose @("up", "-d", "--no-build") | Out-File -Encoding ASCII (Join-Path $runDir "up.log")
        Wait-Ready
        Invoke-Warmup
        $before = Capture-Debug $runDir "before"

        if (-not $SkipK6) {
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
            Copy-Item -LiteralPath (Join-Path $K6Out "results.json") -Destination (Join-Path $runDir "results.json") -Force
        }

        $after = Capture-Debug $runDir "after"
        Write-DebugDeltas $runDir $before $after
        Invoke-Compose $compose @("logs", "--no-color") | Out-File -Encoding ASCII (Join-Path $runDir "compose.log")

        if (-not $SkipK6) {
            $row = Read-RunSummary $runDir $name $Run
            $row | ConvertTo-Json -Depth 4 | Set-Content -Encoding ASCII (Join-Path $runDir "summary.json")
            Write-Host ("{0} run {1}: p99={2} final={3} FP/FN/Error={4}/{5}/{6} waiting_p99={7}" -f $name, $Run, $row.p99, $row.final_score, $row.FP, $row.FN, $row.Error, $row.waiting_p99)
            return $row
        }
    } finally {
        Invoke-Compose $compose @("down", "--remove-orphans") | Out-File -Encoding ASCII (Join-Path $runDir "down-after.log")
    }
    return $null
}

$caseNames = if ($Case -eq "all") {
    @("kdprimary2", "kdclass3-touch", "kdclass3-notouch", "kdclass3-fallback")
} else {
    @($Case)
}

$summaryRows = New-Object System.Collections.Generic.List[object]
foreach ($caseName in $caseNames) {
    $config = Get-CaseConfig $caseName
    for ($run = 1; $run -le $Runs; $run++) {
        $row = Invoke-CaseRun $config $run
        if ($null -ne $row) {
            $summaryRows.Add($row)
        }
    }
}

if ($summaryRows.Count -gt 0) {
    $summaryRows | ConvertTo-Json -Depth 5 | Set-Content -Encoding ASCII (Join-Path $ResultRoot "summary.json")
    $summaryRows | Format-Table -AutoSize | Out-String | Set-Content -Encoding ASCII (Join-Path $ResultRoot "summary.txt")
    $summaryRows | Format-Table -AutoSize
}

Clear-CaseEnv
Write-Host "phase12c_result_root=$ResultRoot"
