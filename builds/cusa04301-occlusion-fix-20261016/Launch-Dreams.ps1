param([string]$GamePath)
$ErrorActionPreference='Stop'
if (-not $GamePath) { $GamePath=Read-Host 'Full path to your Dreams eboot.bin' }
if (-not (Test-Path -LiteralPath $GamePath -PathType Leaf)) { throw 'Game eboot.bin not found.' }
$exe=Join-Path $PSScriptRoot 'shadps4.exe'
if ((Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -ne 'fd882da7acb151db2a149b25a8a108683e4ef5d076f24fdd8d636dbdeb1772be') { throw 'Unexpected executable checksum.' }
$configDir=Join-Path $env:APPDATA 'shadPS4\custom_configs'
New-Item -ItemType Directory -Path $configDir -Force | Out-Null
$config=Join-Path $configDir 'CUSA04301.json'
if (Test-Path -LiteralPath $config) { Copy-Item -LiteralPath $config -Destination ($config+'.backup-'+(Get-Date -Format 'yyyyMMdd-HHmmss')) }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CUSA04301.json') -Destination $config
$start=New-Object System.Diagnostics.ProcessStartInfo
$start.FileName=$exe
$start.WorkingDirectory=$PSScriptRoot
$start.UseShellExecute=$false
$start.Arguments='--fullscreen false --game "'+$GamePath+'"'
@($start.EnvironmentVariables.Keys) | Where-Object { $_ -like 'SHADPS4_*' } | ForEach-Object { $start.EnvironmentVariables.Remove($_) }
$settings=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'environment.json') -Raw | ConvertFrom-Json
foreach ($property in $settings.PSObject.Properties) { if ($property.Name -ne 'SHADPS4_DREAMS_PRIMARY_QUEUE_PROBE_DIR') { $start.EnvironmentVariables[$property.Name]=[string]$property.Value } }
$mode=Join-Path $PSScriptRoot ('queue-mode-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $mode | Out-Null
[IO.File]::WriteAllText((Join-Path $mode 'mode.txt'),"native`n")
$start.EnvironmentVariables['SHADPS4_DREAMS_PRIMARY_QUEUE_PROBE_DIR']=$mode
[Diagnostics.Process]::Start($start) | Out-Null
