# Builds Bathos and bathos-cli (Release) and dist\Bathos-win64.zip, through build.bat (Visual Studio 2022 + CMake).
$ErrorActionPreference = 'Stop'
& cmd /c "`"$PSScriptRoot\build.bat`""
exit $LASTEXITCODE
