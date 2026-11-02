param(
    [string]$DosBoxPath = 'C:\Program Files\DOSBox-X\dosbox-x.exe'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'build'
$executable = Join-Path $outputDirectory 'RETROLNK.EXE'
$configuration = Join-Path $projectRoot 'dosbox-selftest.conf'
$report = Join-Path $outputDirectory 'SELFTEST.TXT'

if (-not (Test-Path $executable)) {
    throw 'RETROLNK.EXE was not found. Run scripts\build.ps1 first.'
}
if (-not (Test-Path $DosBoxPath)) {
    throw "DOSBox-X was not found at '$DosBoxPath'."
}
if (Test-Path $report) {
    Remove-Item -LiteralPath $report
}

$runtimeConfiguration = Join-Path $env:TEMP "retrolink-dosbox-selftest-$PID.conf"
$configurationText = (Get-Content -LiteralPath $configuration -Raw).Replace(
    '@@BUILD_DIRECTORY@@',
    $outputDirectory)

Push-Location $projectRoot
try {
    [System.IO.File]::WriteAllText(
        $runtimeConfiguration,
        $configurationText,
        [System.Text.Encoding]::ASCII)
    $process = Start-Process `
        -FilePath $DosBoxPath `
        -ArgumentList @('-conf', ('"{0}"' -f $runtimeConfiguration)) `
        -PassThru
    if (-not $process.WaitForExit(30000)) {
        Stop-Process -Id $process.Id
        throw 'DOSBox-X did not exit after running the self-test.'
    }
    $process.Refresh()
    if ($process.ExitCode -ne 0) {
        throw "DOSBox-X exited with code $($process.ExitCode)."
    }
}
finally {
    Pop-Location
    if (Test-Path $runtimeConfiguration) {
        Remove-Item -LiteralPath $runtimeConfiguration
    }
}

if (-not (Test-Path $report)) {
    throw 'DOSBox-X did not produce the self-test report.'
}
$result = Get-Content -LiteralPath $report -Raw
Remove-Item -LiteralPath $report
if ($result -notmatch 'RETROLINK DOS SELFTEST PASS') {
    throw "The 8088 DOS self-test failed: $result"
}
Write-Output 'DOSBox-X 8088 self-test passed.'
