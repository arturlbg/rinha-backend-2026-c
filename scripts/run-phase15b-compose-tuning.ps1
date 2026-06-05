param(
    [string]$ApiImage = "ghcr.io/arturlbg/rinha-backend-2026-c:kdclass3-l64",
    [string]$LbImage = "ghcr.io/arturlbg/rinha-backend-2026-c:fdlb",
    [int]$CooldownSeconds = 0,
    [switch]$GenerateOnly,
    [switch]$SkipRepeat,
    [switch]$SkipDiagnostics
)

$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$OfficialTest = (Resolve-Path (Join-Path $Root "..\rinha-de-backend-2026\test")).Path
$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$WorkRoot = Join-Path $Root "tmp\phase15b"
$ResultRoot = Join-Path $Root "tmp\results\phase15b-$Stamp"
$K6Work = Join-Path $ResultRoot "k6"
$K6Out = Join-Path $K6Work "test"
$Request0 = Join-Path $WorkRoot "request0.json"
New-Item -ItemType Directory -Force $WorkRoot, $ResultRoot, $K6Work, $K6Out | Out-Null

Copy-Item -LiteralPath (Join-Path $OfficialTest "test.js") -Destination (Join-Path $K6Work "test.js") -Force
Copy-Item -LiteralPath (Join-Path $OfficialTest "k6-summary.js") -Destination (Join-Path $K6Work "k6-summary.js") -Force
Copy-Item -LiteralPath (Join-Path $OfficialTest "test-data.json") -Destination (Join-Path $K6Work "test-data.json") -Force

$testData = Get-Content -Raw (Join-Path $OfficialTest "test-data.json") | ConvertFrom-Json
$testData.entries[0].request | ConvertTo-Json -Depth 20 -Compress | Set-Content -Encoding ASCII $Request0

$variants = @(
    [ordered]@{ Name = "A-stable"; LbCpu = "0.16"; LbMemory = "30m"; ApiCpu = "0.42"; ApiMemory = "160m"; Tmpfs = $false; LoggingNone = $false; Nofile = $false; Cpuset = $false; NetworkNone = $false; Portable = $true },
    [ordered]@{ Name = "B-split"; LbCpu = "0.20"; LbMemory = "20m"; ApiCpu = "0.40"; ApiMemory = "165m"; Tmpfs = $false; LoggingNone = $false; Nofile = $false; Cpuset = $false; NetworkNone = $false; Portable = $true },
    [ordered]@{ Name = "C-tmpfs"; LbCpu = "0.20"; LbMemory = "20m"; ApiCpu = "0.40"; ApiMemory = "165m"; Tmpfs = $true; LoggingNone = $false; Nofile = $false; Cpuset = $false; NetworkNone = $false; Portable = $true },
    [ordered]@{ Name = "D-runtime"; LbCpu = "0.20"; LbMemory = "20m"; ApiCpu = "0.40"; ApiMemory = "165m"; Tmpfs = $true; LoggingNone = $true; Nofile = $true; Cpuset = $false; NetworkNone = $false; Portable = $true },
    [ordered]@{ Name = "E-cpuset"; LbCpu = "0.20"; LbMemory = "20m"; ApiCpu = "0.40"; ApiMemory = "165m"; Tmpfs = $true; LoggingNone = $true; Nofile = $true; Cpuset = $true; NetworkNone = $false; Portable = $false },
    [ordered]@{ Name = "F-network-none"; LbCpu = "0.20"; LbMemory = "20m"; ApiCpu = "0.40"; ApiMemory = "165m"; Tmpfs = $true; LoggingNone = $true; Nofile = $true; Cpuset = $false; NetworkNone = $true; Portable = $false }
)

function Add-IndentedBlock([System.Text.StringBuilder]$Builder, [string]$Block) {
    [void]$Builder.AppendLine($Block.TrimEnd())
}

function Write-Compose([System.Collections.IDictionary]$Variant) {
    $path = Join-Path $WorkRoot ("compose-{0}.yml" -f $Variant.Name)
    $lbExtras = ""
    $apiExtras1 = ""
    $apiExtras2 = ""
    $apiNetwork = @"
    networks:
      - rinha
"@

    if ($Variant.LoggingNone) {
        $logging = @"
    logging:
      driver: none
"@
        $lbExtras += $logging + "`n"
        $apiExtras1 += $logging + "`n"
        $apiExtras2 += $logging + "`n"
    }
    if ($Variant.Nofile) {
        $nofile = @"
    ulimits:
      nofile:
        soft: 65535
        hard: 65535
"@
        $lbExtras += $nofile + "`n"
        $apiExtras1 += $nofile + "`n"
        $apiExtras2 += $nofile + "`n"
    }
    if ($Variant.Cpuset) {
        $lbExtras += "    cpuset: `"2,3`"`n"
        $apiExtras1 += "    cpuset: `"0`"`n"
        $apiExtras2 += "    cpuset: `"1`"`n"
    }
    if ($Variant.NetworkNone) {
        $apiNetwork = "    network_mode: none`n"
    }

    $volume = @"
volumes:
  sockets:
"@
    if ($Variant.Tmpfs) {
        $volume = @"
volumes:
  sockets:
    driver: local
    driver_opts:
      type: tmpfs
      device: tmpfs
      o: "size=4m,uid=0,gid=0,mode=0777"
"@
    }

    $yaml = @"
name: phase15b-$($Variant.Name.ToLower())
services:
  lb:
    image: $LbImage
    platform: linux/amd64
    ports:
      - "9999:9999"
    environment:
      RINHA_LB_ADDR: ":9999"
      RINHA_FDPASS_UPSTREAMS: "/sockets/api1.ctrl,/sockets/api2.ctrl"
    volumes:
      - sockets:/sockets
    depends_on:
      api1:
        condition: service_healthy
      api2:
        condition: service_healthy
    cpus: "$($Variant.LbCpu)"
    mem_limit: "$($Variant.LbMemory)"
$lbExtras    networks:
      - rinha

  api1:
    image: $ApiImage
    platform: linux/amd64
    user: "0:0"
    environment:
      RINHA_SEARCH_IMPL: "kdclass3"
      RINHA_KDCLASS3_PATH: "/app/resources/kdclass3.bin"
      RINHA_KDCLASS3_TOUCH: "true"
      RINHA_KDCLASS3_FALLBACK: "none"
      RINHA_LISTEN_MODE: "fdpass"
      RINHA_EXEC_MODE: "epoll"
      RINHA_API_PROCESS_MODE: "sync"
      RINHA_API_PROCESSES: "1"
      RINHA_METRICS_ENABLED: "false"
      RINHA_UNIX_SOCKET: "/sockets/api1.ctrl"
    volumes:
      - sockets:/sockets
    healthcheck:
      test: ["CMD-SHELL", "test -S /sockets/api1.ctrl"]
      interval: 1s
      timeout: 1s
      retries: 60
      start_period: 1s
    cpus: "$($Variant.ApiCpu)"
    mem_limit: "$($Variant.ApiMemory)"
$apiExtras1$apiNetwork
  api2:
    image: $ApiImage
    platform: linux/amd64
    user: "0:0"
    environment:
      RINHA_SEARCH_IMPL: "kdclass3"
      RINHA_KDCLASS3_PATH: "/app/resources/kdclass3.bin"
      RINHA_KDCLASS3_TOUCH: "true"
      RINHA_KDCLASS3_FALLBACK: "none"
      RINHA_LISTEN_MODE: "fdpass"
      RINHA_EXEC_MODE: "epoll"
      RINHA_API_PROCESS_MODE: "sync"
      RINHA_API_PROCESSES: "1"
      RINHA_METRICS_ENABLED: "false"
      RINHA_UNIX_SOCKET: "/sockets/api2.ctrl"
    volumes:
      - sockets:/sockets
    healthcheck:
      test: ["CMD-SHELL", "test -S /sockets/api2.ctrl"]
      interval: 1s
      timeout: 1s
      retries: 60
      start_period: 1s
    cpus: "$($Variant.ApiCpu)"
    mem_limit: "$($Variant.ApiMemory)"
$apiExtras2$apiNetwork
$volume
networks:
  rinha:
    driver: bridge
"@
    $yaml | Set-Content -Encoding ASCII $path
    return $path
}

function Invoke-Compose([string]$Compose, [string[]]$ComposeArgs) {
    & docker compose -f $Compose @ComposeArgs
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose failed for $Compose args=$($ComposeArgs -join ' ')"
    }
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
    $empty = 0
    for ($i = 0; $i -lt 10; $i++) {
        $body = & curl.exe -s --max-time 5 -X POST "http://localhost:9999/fraud-score" `
            -H "Content-Type: application/json" --data-binary "@$Request0"
        if ([string]::IsNullOrWhiteSpace($body)) {
            $empty++
        } elseif ($body -match '^\{"approved":(true|false),"fraud_score":') {
            $fraudOk++
        }
    }
    @("ready_ok=$readyOk/10", "fraud_ok=$fraudOk/10", "empty=$empty") | Set-Content -Encoding ASCII $OutPath
    if ($readyOk -ne 10 -or $fraudOk -ne 10 -or $empty -ne 0) {
        throw "smoke failed: ready=$readyOk fraud=$fraudOk empty=$empty"
    }
}

function Save-DockerStats([string]$Compose, [string]$OutPath) {
    $ids = @((Invoke-Compose $Compose @("ps", "-q")) | Where-Object { $_ -ne "" })
    if ($ids.Count -eq 0) {
        "" | Set-Content -Encoding ASCII $OutPath
        return
    }
    & docker stats --no-stream --format "table {{.Name}}\t{{.CPUPerc}}\t{{.MemUsage}}\t{{.PIDs}}" @ids |
        Set-Content -Encoding ASCII $OutPath
}

function Save-Inspect([string]$Compose, [string]$OutPath) {
    $ids = @((Invoke-Compose $Compose @("ps", "-q")) | Where-Object { $_ -ne "" })
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
    $metric = $Summary.metrics.PSObject.Properties[$MetricName]
    if ($null -eq $metric) { return $null }
    $source = $metric.Value
    if ($null -ne $metric.Value.values) { $source = $metric.Value.values }
    $value = $source.PSObject.Properties[$ValueName]
    if ($null -eq $value) { return $null }
    return $value.Value
}

function Invoke-Run([System.Collections.IDictionary]$Variant, [string]$Label, [int]$Index) {
    $compose = $Variant.Compose
    $runDir = Join-Path $ResultRoot ("{0:00}-{1}" -f $Index, $Label)
    New-Item -ItemType Directory -Force $runDir | Out-Null
    try {
        Invoke-Compose $compose @("down", "-v", "--remove-orphans") | Out-File -Encoding ASCII (Join-Path $runDir "down-before.log")
        Invoke-Compose $compose @("up", "-d", "--no-build") | Out-File -Encoding ASCII (Join-Path $runDir "up.log")
        Wait-Ready
        Save-Smoke (Join-Path $runDir "smoke.txt")
        Save-DockerStats $compose (Join-Path $runDir "stats-before.txt")
        Save-Inspect $compose (Join-Path $runDir "inspect-before.json")

        Remove-Item -LiteralPath (Join-Path $K6Out "results.json") -ErrorAction SilentlyContinue
        Push-Location $K6Work
        try {
            $summaryPath = Join-Path $runDir "k6-summary-full.json"
            $process = Start-Process -FilePath "k6" `
                -ArgumentList @("run", "--summary-export", $summaryPath, ".\test.js") `
                -NoNewWindow -Wait -PassThru `
                -RedirectStandardOutput (Join-Path $runDir "k6.stdout.log") `
                -RedirectStandardError (Join-Path $runDir "k6.stderr.log")
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

        $result = Get-Content -Raw (Join-Path $runDir "results.json") | ConvertFrom-Json
        $summary = Get-Content -Raw (Join-Path $runDir "k6-summary-full.json") | ConvertFrom-Json
        $breakdown = $result.scoring.breakdown
        $completed = [int]$breakdown.true_positive_detections + [int]$breakdown.true_negative_detections +
            [int]$breakdown.false_positive_detections + [int]$breakdown.false_negative_detections +
            [int]$breakdown.http_errors
        $row = [PSCustomObject]@{
            index = $Index
            label = $Label
            variant = $Variant.Name
            portable = $Variant.Portable
            p99 = [double]$result.p99
            waiting_p99 = [double](Get-MetricValue $summary "http_req_waiting" "p(99)")
            final_score = [double]$result.scoring.final_score
            p99_score = [double]$result.scoring.p99_score.value
            detection_score = [double]$result.scoring.detection_score.value
            FP = [int]$breakdown.false_positive_detections
            FN = [int]$breakdown.false_negative_detections
            Error = [int]$breakdown.http_errors
            failure_rate = [double]$result.scoring.failure_rate
            completed = $completed
            lb_cpu = $Variant.LbCpu
            api_cpu = $Variant.ApiCpu
            tmpfs = $Variant.Tmpfs
            logging_none = $Variant.LoggingNone
            nofile = $Variant.Nofile
            cpuset = $Variant.Cpuset
            network_none = $Variant.NetworkNone
        }
        $row | ConvertTo-Json -Depth 5 | Set-Content -Encoding ASCII (Join-Path $runDir "summary.json")
        Write-Host ("{0}: p99={1:n3} waiting={2:n3} score={3:n2} FP/FN/Error={4}/{5}/{6}" -f
            $Label, $row.p99, $row.waiting_p99, $row.final_score, $row.FP, $row.FN, $row.Error)
        return $row
    } finally {
        Invoke-Compose $compose @("down", "-v", "--remove-orphans") | Out-File -Encoding ASCII (Join-Path $runDir "down-after.log")
    }
}

function Get-Median([double[]]$Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    $mid = [int]($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return $sorted[$mid] }
    return ($sorted[$mid - 1] + $sorted[$mid]) / 2.0
}

foreach ($variant in $variants) {
    $variant.Compose = Write-Compose $variant
    & docker compose -f $variant.Compose config | Set-Content -Encoding ASCII (Join-Path $WorkRoot ("config-{0}.txt" -f $variant.Name))
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose config failed for $($variant.Name)"
    }
}

if ($GenerateOnly) {
    Write-Host "phase15b_work_root=$WorkRoot"
    exit 0
}

$rows = New-Object System.Collections.Generic.List[object]
$runIndex = 1
foreach ($variant in $variants) {
    if ($SkipDiagnostics -and -not $variant.Portable) { continue }
    try {
        $rows.Add((Invoke-Run $variant $variant.Name $runIndex))
    } catch {
        Write-Warning "$($variant.Name) failed: $($_.Exception.Message)"
        [PSCustomObject]@{ variant = $variant.Name; error = $_.Exception.Message } |
            ConvertTo-Json -Depth 4 | Set-Content -Encoding ASCII (Join-Path $ResultRoot ("{0:00}-{1}-error.json" -f $runIndex, $variant.Name))
    }
    $runIndex++
    if ($CooldownSeconds -gt 0) { Start-Sleep -Seconds $CooldownSeconds }
}

$baseline = $rows | Where-Object { $_.variant -eq "A-stable" } | Select-Object -First 1
$bestPortable = $rows | Where-Object {
    $_.portable -and $_.FP -eq 0 -and $_.FN -eq 0 -and $_.Error -eq 0 -and $_.variant -ne "A-stable"
} | Sort-Object p99 | Select-Object -First 1

if (-not $SkipRepeat -and $null -ne $baseline -and $null -ne $bestPortable -and $bestPortable.p99 -lt ($baseline.p99 * 0.95)) {
    $bestVariant = $variants | Where-Object { $_.Name -eq $bestPortable.variant } | Select-Object -First 1
    $baselineVariant = $variants | Where-Object { $_.Name -eq "A-stable" } | Select-Object -First 1
    foreach ($variant in @($baselineVariant, $bestVariant, $baselineVariant, $bestVariant)) {
        $label = "repeat-$($variant.Name)-$runIndex"
        $rows.Add((Invoke-Run $variant $label $runIndex))
        $runIndex++
        if ($CooldownSeconds -gt 0) { Start-Sleep -Seconds $CooldownSeconds }
    }
}

$rows | ConvertTo-Json -Depth 6 | Set-Content -Encoding ASCII (Join-Path $ResultRoot "summary.json")
$rows | Format-Table -AutoSize | Out-String | Set-Content -Encoding ASCII (Join-Path $ResultRoot "summary.txt")
$rows | Format-Table -AutoSize

$repeatRows = @($rows | Where-Object { $_.label -like "repeat-*" })
if ($repeatRows.Count -gt 0) {
    $repeatSummary = $repeatRows | Group-Object variant | ForEach-Object {
        $p99s = [double[]]@($_.Group | ForEach-Object { $_.p99 })
        [PSCustomObject]@{
            variant = $_.Name
            runs = $_.Count
            best_p99 = ($p99s | Measure-Object -Minimum).Minimum
            median_p99 = Get-Median $p99s
            worst_p99 = ($p99s | Measure-Object -Maximum).Maximum
        }
    }
    $repeatSummary | ConvertTo-Json -Depth 4 | Set-Content -Encoding ASCII (Join-Path $ResultRoot "repeat-summary.json")
    $repeatSummary | Format-Table -AutoSize | Out-String | Set-Content -Encoding ASCII (Join-Path $ResultRoot "repeat-summary.txt")
}

& (Join-Path $PSScriptRoot "collect-k6-details.ps1") -Root $ResultRoot | Out-File -Encoding ASCII (Join-Path $ResultRoot "collect-k6-details.log")
Write-Host "phase15b_work_root=$WorkRoot"
Write-Host "phase15b_result_root=$ResultRoot"
