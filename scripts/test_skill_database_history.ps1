param(
    [string]$VcVars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $repo 'x64\tests\skill_history'
New-Item -ItemType Directory -Force $buildDir | Out-Null
if (!(Test-Path -LiteralPath $VcVars)) { throw "Pass -VcVars with your vcvars64.bat path." }

# Compile the actual database implementation without the application's graphics
# PCH. The C++ test provides the unrelated scale-classification stubs.
$source = Get-Content (Join-Path $repo 'SourceFiles\SkillDatabase.cpp') -Raw
[IO.File]::WriteAllText((Join-Path $buildDir 'SkillDatabase.cpp'),
    $source.Replace('#include "pch.h"', ''))
$installerDir = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
$commands = @"
@echo off
set "PATH=$installerDir;%PATH%"
call "$VcVars" >nul
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /I"$repo\SourceFiles" "$PSScriptRoot\test_skill_database_history.cpp" SkillDatabase.cpp /Fe:skill_history_test.exe
if errorlevel 1 exit /b %errorlevel%
.\skill_history_test.exe "$repo\Data"
exit /b %errorlevel%
"@
$commandFile = Join-Path $buildDir 'test.cmd'
[IO.File]::WriteAllText($commandFile, $commands)
Push-Location $buildDir
try {
    & $commandFile
    if ($LASTEXITCODE -ne 0) { throw "Skill history regression test failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
}
