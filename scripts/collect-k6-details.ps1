param(
    [string]$Root = "",
    [switch]$Recurse
)

$ErrorActionPreference = "Stop"

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if ($Root -eq "") {
    $resultsRoot = Join-Path $ProjectRoot "tmp\results"
    $latest = Get-ChildItem -LiteralPath $resultsRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($null -eq $latest) {
        Write-Host "No result directories found under $resultsRoot"
        exit 0
    }
    $Root = $latest.FullName
}

$Root = (Resolve-Path $Root).Path
$summaryFiles = Get-ChildItem -LiteralPath $Root -Filter "k6-summary-full.json" -Recurse:$true -File -ErrorAction SilentlyContinue
if ($summaryFiles.Count -eq 0) {
    Write-Host "No k6-summary-full.json files found under $Root. Detailed k6 timings are unavailable for this result set."
    exit 0
}

function Get-MetricValue($Summary, [string]$MetricName, [string]$ValueName) {
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

function Get-ResultValue($Result, [string]$Path) {
    $current = $Result
    foreach ($part in $Path.Split(".")) {
        if ($null -eq $current) {
            return $null
        }
        $prop = $current.PSObject.Properties[$part]
        if ($null -eq $prop) {
            return $null
        }
        $current = $prop.Value
    }
    return $current
}

$rows = New-Object System.Collections.Generic.List[object]
foreach ($file in $summaryFiles) {
    $runDir = $file.Directory.FullName
    $resultPath = Join-Path $runDir "results.json"
    $summary = Get-Content -Raw -LiteralPath $file.FullName | ConvertFrom-Json
    $result = $null
    if (Test-Path $resultPath) {
        $result = Get-Content -Raw -LiteralPath $resultPath | ConvertFrom-Json
    }

    $rows.Add([PSCustomObject]@{
        run_dir = Resolve-Path -Relative $runDir
        p99 = Get-ResultValue $result "p99"
        final_score = Get-ResultValue $result "scoring.final_score"
        detection_score = Get-ResultValue $result "scoring.detection_score.value"
        http_errors = Get-ResultValue $result "scoring.breakdown.http_errors"
        failure_rate = Get-ResultValue $result "scoring.failure_rate"
        http_req_duration_p99 = Get-MetricValue $summary "http_req_duration" "p(99)"
        http_req_waiting_p99 = Get-MetricValue $summary "http_req_waiting" "p(99)"
        http_req_blocked_p99 = Get-MetricValue $summary "http_req_blocked" "p(99)"
        http_req_connecting_p99 = Get-MetricValue $summary "http_req_connecting" "p(99)"
        http_req_sending_p99 = Get-MetricValue $summary "http_req_sending" "p(99)"
        http_req_receiving_p99 = Get-MetricValue $summary "http_req_receiving" "p(99)"
        iteration_duration_p99 = Get-MetricValue $summary "iteration_duration" "p(99)"
        checks_passes = Get-MetricValue $summary "checks" "passes"
        checks_fails = Get-MetricValue $summary "checks" "fails"
    })
}

$jsonPath = Join-Path $Root "k6-details-summary.json"
$txtPath = Join-Path $Root "k6-details-summary.txt"
$rows | ConvertTo-Json -Depth 6 | Set-Content -Encoding ASCII $jsonPath
$rows | Format-Table -AutoSize | Out-String | Set-Content -Encoding ASCII $txtPath
$rows | Format-Table -AutoSize
Write-Host "k6_details_json=$jsonPath"
Write-Host "k6_details_txt=$txtPath"
