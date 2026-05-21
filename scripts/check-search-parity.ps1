param(
    [int]$Limit = 1000,
    [string]$IndexPath = "C:\Users\Usuario\Documents\rinha-backend\rinha-2026-go\release\index.bin",
    [string]$DataPath = "C:\Users\Usuario\Documents\rinha-backend\rinha-de-backend-2026\test\test-data.json",
    [string]$GoRepo = "C:\Users\Usuario\Documents\rinha-backend\rinha-2026-go",
    [string]$GoBranch = "experiment/go-ivf8-lowlatency",
    [string]$DockerImage = "rinha-c-phase5-test",
    [int]$MaxCandidates = 4096,
    [int]$Probes = 8
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ParityDir = Join-Path $ProjectRoot "tmp\search-parity"
$GoTmp = Join-Path $ParityDir "go"
$GoOut = Join-Path $ParityDir "go_counts.txt"
$COut = Join-Path $ParityDir "c_counts.txt"
$CStats = Join-Path $ParityDir "c_stats.txt"

function Write-Utf8NoBom {
    param(
        [string]$Path,
        [string]$Content
    )
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, $Content, $encoding)
}

New-Item -ItemType Directory -Force -Path $ParityDir | Out-Null
if (Test-Path $GoTmp) {
    Remove-Item -LiteralPath $GoTmp -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $GoTmp | Out-Null

Write-Host "Preparing temporary Go IVF8 search helper from $GoBranch..."
Write-Utf8NoBom -Path (Join-Path $GoTmp "go.mod") -Content "module github.com/arturlbg/rinha-2026-go`n`ngo 1.22`n"
foreach ($path in @(
    "internal/fastvector/fastvector.go",
    "internal/vector/vector.go",
    "internal/ivf8/index.go",
    "internal/ivf8/search.go",
    "internal/ivf8/quant.go"
)) {
    $content = & git -C $GoRepo show "${GoBranch}:$path" | Out-String
    Write-Utf8NoBom -Path (Join-Path $GoTmp $path.Replace("/", "\")) -Content $content
}

$goMain = @'
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"os"

	"github.com/arturlbg/rinha-2026-go/internal/fastvector"
	"github.com/arturlbg/rinha-2026-go/internal/ivf8"
)

type testData struct {
	Entries []entry `json:"entries"`
}

type entry struct {
	Request json.RawMessage `json:"request"`
}

func main() {
	indexPath := flag.String("index", "", "index path")
	dataPath := flag.String("test-data", "", "test-data path")
	limit := flag.Int("limit", 0, "limit")
	maxCandidates := flag.Int("max-candidates", 4096, "max candidates")
	probes := flag.Int("probes", 8, "probes")
	flag.Parse()
	if *indexPath == "" || *dataPath == "" {
		fmt.Fprintln(os.Stderr, "missing --index or --test-data")
		os.Exit(2)
	}
	idx, err := ivf8.LoadFile(*indexPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	raw, err := os.ReadFile(*dataPath)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	var data testData
	if err := json.Unmarshal(raw, &data); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	entries := data.Entries
	if *limit > 0 && *limit < len(entries) {
		entries = entries[:*limit]
	}
	cfg := ivf8.SearchConfig{Policy: ivf8.PolicyFixed, MaxCandidates: *maxCandidates, NProbe: *probes}
	for i, entry := range entries {
		vec, err := fastvector.Vectorize(entry.Request)
		if err != nil {
			fmt.Fprintf(os.Stderr, "line %d: %v\n", i, err)
			os.Exit(1)
		}
		result := ivf8.Search(idx, ivf8.ToQuery(vec), cfg)
		fmt.Println(result.FraudCount)
	}
}
'@
Write-Utf8NoBom -Path (Join-Path $GoTmp "cmd\search_go\main.go") -Content $goMain

$env:GOCACHE = Join-Path $ProjectRoot "tmp\go-build"
$env:GOTMPDIR = Join-Path $ProjectRoot "tmp\go-tmp"
New-Item -ItemType Directory -Force -Path $env:GOCACHE | Out-Null
New-Item -ItemType Directory -Force -Path $env:GOTMPDIR | Out-Null

Write-Host "Running Go IVF8 search helper..."
Push-Location $GoTmp
try {
    & go run .\cmd\search_go -index $IndexPath -test-data $DataPath -limit $Limit -max-candidates $MaxCandidates -probes $Probes |
        Set-Content -Encoding ascii -LiteralPath $GoOut
    if ($LASTEXITCODE -ne 0) {
        throw "Go search helper failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

Write-Host "Running C IVF8 evaluator through Docker image $DockerImage..."
$releaseDir = Split-Path -Parent $IndexPath
$testDir = Split-Path -Parent $DataPath
$workMount = "${ParityDir}:/work"
$indexMount = "${releaseDir}:/data:ro"
$testMount = "${testDir}:/testdata:ro"
& docker run --rm -v $indexMount -v $testMount -v $workMount $DockerImage /src/build/evaluate_c `
    --index /data/index.bin `
    --test-data /testdata/test-data.json `
    --limit $Limit `
    --max-candidates $MaxCandidates `
    --probes $Probes `
    --counts-output /work/c_counts.txt |
    Set-Content -Encoding ascii -LiteralPath $CStats
if ($LASTEXITCODE -ne 0) {
    throw "C evaluator failed with exit code $LASTEXITCODE"
}

$goLines = Get-Content -LiteralPath $GoOut
$cLines = Get-Content -LiteralPath $COut
if ($goLines.Count -ne $cLines.Count) {
    throw "line count mismatch: go=$($goLines.Count) c=$($cLines.Count)"
}

$mismatches = 0
for ($i = 0; $i -lt $goLines.Count; $i++) {
    if ($goLines[$i] -ne $cLines[$i]) {
        $mismatches++
        if ($mismatches -le 20) {
            Write-Host "Mismatch at query $i go=$($goLines[$i]) c=$($cLines[$i])"
        }
    }
}

Write-Host "Compared $($goLines.Count) query fraud_count(s); mismatches=$mismatches"
Write-Host "C evaluator summary:"
Get-Content -LiteralPath $CStats
if ($mismatches -ne 0) {
    exit 1
}

