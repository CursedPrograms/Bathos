@echo off
setlocal
cd /d "%~dp0"
rem Installs Bathos for this user: %LOCALAPPDATA%\Programs\Bathos, with Start menu and Desktop shortcuts.
rem Chimera finds it there for Filter > Bathos. Run build.bat first (or unzip a release and run this beside it).
set "SRC=dist\Bathos"
if exist "Bathos.exe" set "SRC=."
if not exist "%SRC%\Bathos.exe" ( echo Build it first: build.bat & exit /b 1 )
tasklist /fi "imagename eq Bathos.exe" | find /i "Bathos.exe" >nul
if not errorlevel 1 ( echo Bathos is open: close it and run this again. & exit /b 1 )
set "INST=%LOCALAPPDATA%\Programs\Bathos"
if not exist "%INST%" mkdir "%INST%"
xcopy /e /i /q /y "%SRC%\*" "%INST%\" >nul || ( echo Couldn't copy to %INST% & exit /b 1 )
powershell -NoProfile -Command "$s=New-Object -ComObject WScript.Shell; foreach($d in @([Environment]::GetFolderPath('Programs'),[Environment]::GetFolderPath('Desktop'))){ $l=$s.CreateShortcut((Join-Path $d 'Bathos.lnk')); $l.TargetPath='%INST%\Bathos.exe'; $l.WorkingDirectory='%INST%'; $l.IconLocation='%INST%\Bathos.exe,0'; $l.Description='Bathos: a picture in, its depth map out'; $l.Save() }"
echo Installed: %INST%
if /i not "%~1"=="--quiet" echo Start it from the Start menu or the Desktop.
exit /b 0
