param(
    [string]$RuntimeIdentifier = 'win-x64'
)

$ErrorActionPreference = 'Stop'
$workspaceRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$projectPath = Join-Path $workspaceRoot 'src\RetroLink\RetroLink.csproj'
$releaseDirectory = Join-Path $workspaceRoot 'RELEASE\Windows'
$publishDirectory = Join-Path $env:TEMP "retrolink-publish-$PID"
$executablePath = Join-Path $publishDirectory 'RetroLink.exe'
$releaseExecutablePath = Join-Path $releaseDirectory 'RetroLink.exe'

if (Test-Path -LiteralPath $publishDirectory) {
    throw "Temporary publish directory already exists: $publishDirectory"
}

New-Item -ItemType Directory -Path $releaseDirectory -Force | Out-Null
try {
    & dotnet publish $projectPath `
        --configuration Release `
        --runtime $RuntimeIdentifier `
        --self-contained true `
        "-p:PublishDir=$publishDirectory\" `
        '-p:PublishSingleFile=true' `
        '-p:IncludeNativeLibrariesForSelfExtract=true' `
        '-p:IncludeAllContentForSelfExtract=true' `
        '-p:EnableCompressionInSingleFile=true' `
        '-p:DebugType=None' `
        '-p:DebugSymbols=false'
    if ($LASTEXITCODE -ne 0) {
        throw "Self-contained publish failed with exit code $LASTEXITCODE."
    }

    if (-not (Test-Path -LiteralPath $executablePath -PathType Leaf)) {
        throw "Publish completed without the expected executable: $executablePath"
    }

    $unexpectedOutputs = @(
        Get-ChildItem -LiteralPath $publishDirectory -File |
            Where-Object Name -ne 'RetroLink.exe'
    )
    if ($unexpectedOutputs.Count -gt 0) {
        $names = ($unexpectedOutputs | ForEach-Object Name) -join ', '
        throw "Single-file publish produced sidecar files: $names"
    }

    Copy-Item -LiteralPath $executablePath -Destination $releaseExecutablePath -Force
    Write-Output "Published self-contained $RuntimeIdentifier executable: $releaseExecutablePath"
}
finally {
    if (Test-Path -LiteralPath $publishDirectory) {
        Remove-Item -LiteralPath $publishDirectory -Recurse -Force
    }
}
