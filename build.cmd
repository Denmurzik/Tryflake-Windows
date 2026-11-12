@echo off
rem Tryflake for Windows build script: build.cmd [build|test|run|diagnostics|clean]
rem Requires Visual Studio 2022+ or Build Tools with "Desktop development with C++".
setlocal EnableExtensions
set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=build"

if /I "%TARGET%"=="clean" goto :clean

rem ---- MSVC environment
where cl >nul 2>nul
if not errorlevel 1 goto :haveenv
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :novs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :novs
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
:haveenv

if not exist "%OUT%\obj" mkdir "%OUT%\obj"
if not exist "%OUT%\gen" mkdir "%OUT%\gen"
if not exist "%OUT%\test" mkdir "%OUT%\test"

set CFLAGS=/nologo /std:c++20 /O2 /EHsc /utf-8 /W4 /MT /GS /guard:cf /permissive- /DUNICODE /D_UNICODE /DNOMINMAX

if /I "%TARGET%"=="test" goto :test

rem ---- HLSL shaders compiled to bytecode headers
fxc /nologo /O3 /T vs_4_0 /E vsMain /Vn g_vsMain /Fh "%OUT%\gen\SnowVS.h" "%ROOT%shaders\Snow.hlsl" >nul
if errorlevel 1 goto :fail
fxc /nologo /O3 /T ps_4_0 /E psMain /Vn g_psMain /Fh "%OUT%\gen\SnowPS.h" "%ROOT%shaders\Snow.hlsl" >nul
if errorlevel 1 goto :fail

rem ---- resources: icon, manifest, version
rc /nologo /fo "%OUT%\obj\Tryflake.res" "%ROOT%res\Tryflake.rc"
if errorlevel 1 goto :fail

rem ---- application (static CRT: single exe, no redistributables)
cl %CFLAGS% /I"%OUT%\gen" /Fo"%OUT%\obj\\" /Fe"%OUT%\Tryflake.exe" "%ROOT%src\core\SnowCore.cpp" "%ROOT%src\app\main.cpp" "%ROOT%src\app\App.cpp" "%ROOT%src\app\Gpu.cpp" "%ROOT%src\app\Overlay.cpp" "%ROOT%src\app\WindowTracker.cpp" "%ROOT%src\app\Settings.cpp" "%ROOT%src\app\SettingsPanel.cpp" "%ROOT%src\app\Icons.cpp" "%OUT%\obj\Tryflake.res" /link /SUBSYSTEM:WINDOWS /GUARD:CF /DYNAMICBASE /NXCOMPAT /MANIFEST:NO d3d11.lib dxgi.lib dcomp.lib d2d1.lib dwrite.lib windowscodecs.lib dwmapi.lib shcore.lib user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib wtsapi32.lib
if errorlevel 1 goto :fail

copy /y "%ROOT%LICENSE" "%OUT%\LICENSE.txt" >nul
powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\Tryflake.exe','%OUT%\LICENSE.txt' -DestinationPath '%OUT%\Tryflake.zip' -Force"
if errorlevel 1 goto :fail
powershell -NoProfile -Command "(Get-FileHash '%OUT%\Tryflake.zip' -Algorithm SHA256).Hash.ToLower() + '  Tryflake.zip' | Set-Content -Encoding ascii '%OUT%\SHA256SUMS'"
echo.
echo Built:   %OUT%\Tryflake.exe
echo Archive: %OUT%\Tryflake.zip

if /I "%TARGET%"=="run" start "" "%OUT%\Tryflake.exe" --show-settings
if /I "%TARGET%"=="diagnostics" "%OUT%\Tryflake.exe" --diagnostics --windows --render-preview "%OUT%\snow-preview.png"
exit /b 0

:test
cl %CFLAGS% /Fo"%OUT%\test\\" /Fe"%OUT%\test\SnowCoreChecks.exe" "%ROOT%tests\SnowCoreChecks.cpp" "%ROOT%src\core\SnowCore.cpp" /link /SUBSYSTEM:CONSOLE >nul
if errorlevel 1 goto :fail
"%OUT%\test\SnowCoreChecks.exe"
exit /b %errorlevel%

:clean
if exist "%OUT%" rmdir /s /q "%OUT%"
echo Cleaned.
exit /b 0

:novs
echo MSVC not found. Install Visual Studio Build Tools with "Desktop development with C++".
exit /b 1

:fail
echo.
echo Build failed.
exit /b 1
