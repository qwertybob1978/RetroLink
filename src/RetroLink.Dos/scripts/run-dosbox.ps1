param(
    [string]$DosBoxPath = 'C:\Program Files\DOSBox-X\dosbox-x.exe'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDirectory = Join-Path $projectRoot 'build'
$executable = Join-Path $outputDirectory 'RETROLNK.EXE'
$configuration = Join-Path $projectRoot 'dosbox-x.conf'

if (-not (Test-Path $executable)) {
    throw 'RETROLNK.EXE was not found. Run scripts\build.ps1 first.'
}
if (-not (Test-Path $DosBoxPath)) {
    throw "DOSBox-X was not found at '$DosBoxPath'."
}

$runtimeConfiguration = Join-Path $env:TEMP "retrolink-dosbox-run-$PID.conf"
$configurationText = (Get-Content -LiteralPath $configuration -Raw).Replace(
    '@@BUILD_DIRECTORY@@',
    $outputDirectory)

try {
    [System.IO.File]::WriteAllText(
        $runtimeConfiguration,
        $configurationText,
        [System.Text.Encoding]::ASCII)
    $process = Start-Process `
        -FilePath $DosBoxPath `
        -ArgumentList @('-conf', ('"{0}"' -f $runtimeConfiguration)) `
        -PassThru `
        -Wait
    if ($process.ExitCode -ne 0) {
        throw "DOSBox-X exited with code $($process.ExitCode)."
    }
}
finally {
    if (Test-Path $runtimeConfiguration) {
        Remove-Item -LiteralPath $runtimeConfiguration
    }
}
