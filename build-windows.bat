@echo off
REM ---------------------------------------------------------------------------
REM  Local Windows build.
REM
REM  Needs: Visual Studio 2022 (or Build Tools) with the "Desktop development
REM  with C++" workload, CMake, and git. Inno Setup 6 is optional and only
REM  used for the final installer.
REM
REM  Usage:
REM    build-windows.bat            DSP only, fastest
REM    build-windows.bat neural     also builds the neural stage
REM ---------------------------------------------------------------------------

setlocal enabledelayedexpansion

set ONNX_VERSION=1.17.0
set CMAKE_ARGS=-B build -G "Visual Studio 17 2022" -A x64 -DVM_COPY_PLUGIN=ON

if /i "%~1"=="neural" goto :neural
goto :configure

:neural
set ORT_DIR=%CD%\third_party\onnxruntime-win-x64-%ONNX_VERSION%

if not exist "%ORT_DIR%" (
    echo Downloading ONNX Runtime %ONNX_VERSION% ...
    if not exist third_party mkdir third_party
    powershell -NoProfile -Command ^
        "$u='https://github.com/microsoft/onnxruntime/releases/download/v%ONNX_VERSION%/onnxruntime-win-x64-%ONNX_VERSION%.zip';" ^
        "Invoke-WebRequest -Uri $u -OutFile ort.zip;" ^
        "Expand-Archive ort.zip -DestinationPath third_party -Force;" ^
        "Remove-Item ort.zip"
    if errorlevel 1 (
        echo Download failed. Fetch it by hand and extract into third_party\.
        exit /b 1
    )
)

set CMAKE_ARGS=%CMAKE_ARGS% -DVM_ENABLE_ONNX=ON -DVM_ONNXRUNTIME_ROOT="%ORT_DIR%"

:configure
echo.
echo === Configuring ===
cmake %CMAKE_ARGS%
if errorlevel 1 exit /b 1

echo.
echo === Building (first run fetches JUCE, expect several minutes) ===
cmake --build build --config Release --parallel
if errorlevel 1 exit /b 1

REM The DLL must sit beside every binary that loads it.
if defined ORT_DIR (
    copy /y "%ORT_DIR%\lib\onnxruntime.dll" "build\VoiceMorph_artefacts\Release\Standalone\" >nul
    copy /y "%ORT_DIR%\lib\onnxruntime.dll" "build\VoiceMorph_artefacts\Release\VST3\VoiceMorph.vst3\Contents\x86_64-win\" >nul
    copy /y "%ORT_DIR%\lib\onnxruntime.dll" "installer\" >nul
)

echo.
echo === Done ===
echo   Standalone : build\VoiceMorph_artefacts\Release\Standalone\VoiceMorph.exe
echo   VST3       : build\VoiceMorph_artefacts\Release\VST3\VoiceMorph.vst3
echo.

set ISCC="%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist %ISCC% (
    echo Inno Setup 6 not found, skipping the installer.
    echo Install it from https://jrsoftware.org/isdl.php to get a setup .exe.
    exit /b 0
)

echo === Building the installer ===
if not exist dist mkdir dist
%ISCC% installer\VoiceMorph.iss
if errorlevel 1 exit /b 1

echo.
echo   Installer  : dist\VoiceMorph-0.1.0-Windows-Setup.exe
echo.

endlocal
