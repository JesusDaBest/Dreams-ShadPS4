param([Parameter(Mandatory=$true)][string]$SourceRoot, [string]$Compiler = "clang-cl")
$ErrorActionPreference = 'Stop'
$sourcePath = (Resolve-Path -LiteralPath $SourceRoot).Path
$testPath = $PSScriptRoot
$buildPath = Join-Path $sourcePath 'work/random-api-test'
New-Item -ItemType Directory -Force -Path $buildPath | Out-Null
Push-Location $buildPath
try {
    & $Compiler /nologo /std:c++20 /EHsc "/I$testPath/stubs" "/I$sourcePath/src" "$testPath/test_random.cpp" "$sourcePath/src/core/libraries/random/random.cpp" /Fe:random-api-test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
    $result = & './random-api-test.exe'
    if ($LASTEXITCODE -ne 0) { throw 'API boundary tests failed' }
    $result
    if ($result -notcontains 'Identical after host srand reset: NO' -or $result -notcontains 'Unique concurrent samples: 1024/1024') { throw 'Random streams repeated' }
    $samples = @()
    for ($i = 0; $i -lt 20; $i++) {
        $samples += (& './random-api-test.exe' sample)
        if ($LASTEXITCODE -ne 0) { throw 'Separate-process test failed' }
    }
    if (($samples | Sort-Object -Unique).Count -ne 20) { throw 'Separate processes repeated samples' }
    Write-Output '20 separate-process samples distinct; all checks passed.'
} finally { Pop-Location }
