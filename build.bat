@echo off
rem Builds internal.dll and injector.exe via CMake (uses VS-bundled cmake)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vs

for /f "usebackq delims=" %%i in (
    `"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`
) do set "VS_INSTALL=%%i"

if not defined VS_INSTALL goto :no_vs

set "CMAKE=%VS_INSTALL%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" goto :no_cmake

echo [cmake] Configuring...
"%CMAKE%" -S . -B build -A Win32 -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 goto :fail

echo.
echo [cmake] Building...
"%CMAKE%" --build build --config Release
if errorlevel 1 goto :fail

echo.
echo [OK] Built internal.dll and injector.exe
goto :end

:no_vs
echo [FAIL] Visual Studio not found. Install VS with the C++ workload.
goto :end

:no_cmake
echo [FAIL] cmake not found inside VS install at:
echo        %CMAKE%
echo        Try installing the "C++ CMake tools for Windows" component in VS installer.
goto :end

:fail
echo.
echo [FAIL] Build failed.

:end
pause
