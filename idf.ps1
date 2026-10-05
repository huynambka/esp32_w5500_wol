param(
    [Alias('p')]
    [string]$Port,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArgs
)

$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$idf = Join-Path $project 'vendor\esp-idf'
$env:IDF_PATH = $idf
$env:IDF_TOOLS_PATH = Join-Path $project '.tools'
$env:PIP_CACHE_DIR = Join-Path $project '.cache\pip'
$env:TEMP = Join-Path $project '.cache\tmp'
$env:TMP = $env:TEMP
$env:XDG_CACHE_HOME = Join-Path $project '.cache'

if (-not (Test-Path -LiteralPath (Join-Path $idf 'tools\idf.py'))) {
    throw 'ESP-IDF is missing. Run setup.ps1 after extracting the release archive.'
}

@($env:PIP_CACHE_DIR, $env:TEMP) |
    ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }

& (Join-Path $idf 'export.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $IdfArgs -or $IdfArgs.Count -eq 0) { $IdfArgs = @('build') }
$runArgs = @()
if ($Port) { $runArgs += @('-p', $Port) }
$runArgs += $IdfArgs
Push-Location $project
try {
    & python (Join-Path $idf 'tools\idf.py') @runArgs
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
