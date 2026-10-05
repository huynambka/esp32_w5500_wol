$ErrorActionPreference = 'Stop'
$project = $PSScriptRoot
$idf = Join-Path $project 'vendor\esp-idf'

if (-not (Test-Path -LiteralPath (Join-Path $idf 'tools\idf.py'))) {
    throw "ESP-IDF was not found at $idf"
}

$env:IDF_PATH = $idf
$env:IDF_TOOLS_PATH = Join-Path $project '.tools'
$env:PIP_CACHE_DIR = Join-Path $project '.cache\pip'
$env:TEMP = Join-Path $project '.cache\tmp'
$env:TMP = $env:TEMP
$env:XDG_CACHE_HOME = Join-Path $project '.cache'
Remove-Item Env:PIP_NO_INDEX -ErrorAction SilentlyContinue

@($env:IDF_TOOLS_PATH, $env:PIP_CACHE_DIR, $env:TEMP) |
    ForEach-Object { New-Item -ItemType Directory -Force -Path $_ | Out-Null }

& python (Join-Path $idf 'tools\idf_tools.py') --idf-path $idf install --targets=esp32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& python (Join-Path $idf 'tools\idf_tools.py') --idf-path $idf install-python-env --features=core
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Installed ESP-IDF tools under $env:IDF_TOOLS_PATH"
Write-Host 'Next: .\idf.ps1 build'
