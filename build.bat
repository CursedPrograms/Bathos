@echo off
setlocal
cd /d "%~dp0"
rem Builds Bathos (the window) and bathos-cli (the command line) into build\Release, and dist\Bathos-win64.zip.
rem Needs Visual Studio 2022 (C++) and CMake. SDL2 and ONNX Runtime are fetched into external\ once.
if not exist "external\SDL2-2.30.9\cmake" (
    echo Getting SDL2's development package, once...
    if not exist external mkdir external
    curl -sL -o external\SDL2-devel.zip https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-VC.zip || goto :error
    tar -xf external\SDL2-devel.zip -C external || goto :error
    del external\SDL2-devel.zip
)
if not exist "external\onnxruntime\onnxruntime.dll" (
    echo Getting ONNX Runtime, once...
    if not exist external\onnxruntime mkdir external\onnxruntime
    curl -sL -o external\ort.zip https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip || goto :error
    tar -xf external\ort.zip -C external || goto :error
    copy /y external\onnxruntime-win-x64-1.20.1\lib\onnxruntime.dll external\onnxruntime\ >nul
    rmdir /s /q external\onnxruntime-win-x64-1.20.1
    del external\ort.zip
)
cmake -B build -G "Visual Studio 17 2022" -A x64 || goto :error
cmake --build build --config Release || goto :error
if exist dist\Bathos rmdir /s /q dist\Bathos
mkdir dist\Bathos
for %%f in (Bathos.exe bathos-cli.exe SDL2.dll onnxruntime.dll) do copy /y build\Release\%%f dist\Bathos\ >nul
xcopy /e /i /q /y build\Release\models dist\Bathos\models >nul
xcopy /e /i /q /y build\Release\assets dist\Bathos\assets >nul
copy /y LICENSE dist\Bathos\ >nul
copy /y NOTICE dist\Bathos\ >nul
copy /y README.md dist\Bathos\ >nul
copy /y install.bat dist\Bathos\ >nul
copy /y install.ps1 dist\Bathos\ >nul
if exist dist\Bathos-win64.zip del dist\Bathos-win64.zip
"%SystemRoot%\System32\tar.exe" -a -c -f dist\Bathos-win64.zip -C dist Bathos || goto :error
echo Built: build\Release\Bathos.exe, bathos-cli.exe and dist\Bathos-win64.zip
rem installed on this PC: this build replaces that copy too
if exist "%LOCALAPPDATA%\Programs\Bathos\Bathos.exe" call "%~dp0install.bat" --quiet
exit /b 0
:error
echo Build failed, see above.
pause
exit /b 1
