$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'build'
$compiler = (Get-Command 'gcc.exe' -ErrorAction SilentlyContinue).Source

if (-not $compiler) {
    throw 'MinGW GCC was not found on PATH.'
}

New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$testExecutable = Join-Path $outputDirectory 'protocol-tests.exe'
& $compiler '-std=c89' '-Wall' '-Wextra' '-Werror' `
    "-I$(Join-Path $projectRoot 'include')" `
    (Join-Path $projectRoot 'src\wire.c') `
    (Join-Path $projectRoot 'src\retro_protocol.c') `
    (Join-Path $projectRoot 'tests\protocol_test.c') `
    '-o' $testExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Native protocol test build failed with exit code $LASTEXITCODE."
}

& $testExecutable
if ($LASTEXITCODE -ne 0) {
    throw "Native protocol tests failed with exit code $LASTEXITCODE."
}
