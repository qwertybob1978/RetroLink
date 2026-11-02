param(
    [string]$WatcomRoot = $env:WATCOM
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'build'
$compiler = if ($WatcomRoot) {
    Join-Path $WatcomRoot 'binnt\wcl.exe'
} else {
    (Get-Command 'wcl.exe' -ErrorAction SilentlyContinue).Source
}

if (-not $compiler -or -not (Test-Path $compiler)) {
    throw 'Open Watcom wcl.exe was not found. Set WATCOM or pass -WatcomRoot.'
}

if ($WatcomRoot) {
    $env:WATCOM = $WatcomRoot
    $env:PATH = "$WatcomRoot\binnt;$WatcomRoot\binw;$env:PATH"
    $env:EDPATH = Join-Path $WatcomRoot 'eddat'
    $env:INCLUDE = Join-Path $WatcomRoot 'h'
}

New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$sources = @(
    (Join-Path $projectRoot 'src\wire.c'),
    (Join-Path $projectRoot 'src\retro_protocol.c'),
    (Join-Path $projectRoot 'src\dos_serial.c'),
    (Join-Path $projectRoot 'src\agent_link.c'),
    (Join-Path $projectRoot 'src\retrolink_main.c')
)

Push-Location $projectRoot
try {
    & $compiler '-bt=dos' '-0' '-ms' '-k8192' "-i=$(Join-Path $projectRoot 'include')" "-fe=$(Join-Path $outputDirectory 'RETROLNK.EXE')" @sources
    if ($LASTEXITCODE -ne 0) {
        throw "Open Watcom failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
