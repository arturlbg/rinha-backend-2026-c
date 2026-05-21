param(
    [int]$Limit = 1000,
    [string]$DataPath = "C:\Users\Usuario\Documents\rinha-backend\rinha-de-backend-2026\test\test-data.json",
    [string]$GoRepo = "C:\Users\Usuario\Documents\rinha-backend\rinha-2026-go",
    [string]$GoBranch = "experiment/go-ivf8-lowlatency",
    [string]$DockerImage = "rinha-c-phase4-test"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ParityDir = Join-Path $ProjectRoot "tmp\vector-parity"
$GoTmp = Join-Path $ParityDir "go"
$RequestsPath = Join-Path $ParityDir "requests.jsonl"
$GoOut = Join-Path $ParityDir "go_vectors.txt"
$COut = Join-Path $ParityDir "c_vectors.txt"

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

Write-Host "Extracting $Limit official-local requests..."
$json = Get-Content -LiteralPath $DataPath -Raw | ConvertFrom-Json
$lines = New-Object System.Collections.Generic.List[string]
$take = [Math]::Min($Limit, $json.entries.Count)
for ($i = 0; $i -lt $take; $i++) {
    $lines.Add(($json.entries[$i].request | ConvertTo-Json -Compress -Depth 32))
}
Write-Utf8NoBom -Path $RequestsPath -Content (($lines -join "`n") + "`n")

Write-Host "Preparing temporary Go vectorizer from $GoBranch..."
Write-Utf8NoBom -Path (Join-Path $GoTmp "go.mod") -Content "module github.com/arturlbg/rinha-2026-go`n`ngo 1.22`n"
$fastvectorSource = & git -C $GoRepo show "${GoBranch}:internal/fastvector/fastvector.go" | Out-String
$vectorSource = & git -C $GoRepo show "${GoBranch}:internal/vector/vector.go" | Out-String
Write-Utf8NoBom -Path (Join-Path $GoTmp "internal\fastvector\fastvector.go") -Content $fastvectorSource
Write-Utf8NoBom -Path (Join-Path $GoTmp "internal\vector\vector.go") -Content $vectorSource

$goMain = @'
package main

import (
	"bufio"
	"flag"
	"fmt"
	"os"

	"github.com/arturlbg/rinha-2026-go/internal/fastvector"
)

func main() {
	input := flag.String("input", "", "JSONL request file")
	flag.Parse()
	var file *os.File
	var err error
	if *input == "" {
		file = os.Stdin
	} else {
		file, err = os.Open(*input)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
		defer file.Close()
	}
	scanner := bufio.NewScanner(file)
	scanner.Buffer(make([]byte, 0, 1024*1024), 1024*1024)
	line := 0
	for scanner.Scan() {
		line++
		body := scanner.Bytes()
		if len(body) == 0 {
			continue
		}
		vec, err := fastvector.Vectorize(body)
		if err != nil {
			fmt.Fprintf(os.Stderr, "line %d: %v\n", line, err)
			os.Exit(1)
		}
		for i, value := range vec {
			if i != 0 {
				fmt.Print(" ")
			}
			fmt.Print(value)
		}
		fmt.Println()
	}
	if err := scanner.Err(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
'@
Write-Utf8NoBom -Path (Join-Path $GoTmp "cmd\vectorize_go\main.go") -Content $goMain

$env:GOCACHE = Join-Path $ProjectRoot "tmp\go-build"
$env:GOTMPDIR = Join-Path $ProjectRoot "tmp\go-tmp"
New-Item -ItemType Directory -Force -Path $env:GOCACHE | Out-Null
New-Item -ItemType Directory -Force -Path $env:GOTMPDIR | Out-Null

Write-Host "Running Go vectorizer..."
Push-Location $GoTmp
try {
    & go run .\cmd\vectorize_go -input $RequestsPath | Set-Content -Encoding ascii -LiteralPath $GoOut
    if ($LASTEXITCODE -ne 0) {
        throw "Go vectorizer failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

Write-Host "Running C vectorizer through Docker image $DockerImage..."
$mount = "${ParityDir}:/work:ro"
& docker run --rm -v $mount $DockerImage /src/build/vectorize_c --jsonl /work/requests.jsonl | Set-Content -Encoding ascii -LiteralPath $COut
if ($LASTEXITCODE -ne 0) {
    throw "C vectorizer failed with exit code $LASTEXITCODE"
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
        if ($mismatches -le 10) {
            Write-Host "Mismatch at request $i"
            Write-Host "  go: $($goLines[$i])"
            Write-Host "   c: $($cLines[$i])"
        }
    }
}

Write-Host "Compared $($goLines.Count) request(s); mismatches=$mismatches"
if ($mismatches -ne 0) {
    exit 1
}

