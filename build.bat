@echo off
rem Builds internal.dll (ESP + executor merged) and injector.exe
rem MUST be run from "x86 Native Tools Command Prompt for VS"

where cl >nul 2>&1
if errorlevel 1 goto :no_cl

set "DXSDK=C:\Program Files (x86)\Microsoft DirectX SDK (June 2010)"
if not exist "%DXSDK%\Include\d3d9.h" goto :no_dx

echo.
echo ==== Building internal.dll ====
cl /nologo /EHa /O2 /MT /LD /Fe:internal.dll ^
   /D_CRT_SECURE_NO_WARNINGS ^
   /Ivendor ^
   /I"%DXSDK%\Include" ^
   dllmain.cpp hooks.cpp esp.cpp menu.cpp executor.cpp ^
   vendor\imgui\imgui.cpp ^
   vendor\imgui\imgui_draw.cpp ^
   vendor\imgui\imgui_tables.cpp ^
   vendor\imgui\imgui_widgets.cpp ^
   vendor\imgui\imgui_impl_dx9.cpp ^
   vendor\imgui\imgui_impl_win32.cpp ^
   /link /DLL /MACHINE:X86 ^
   /LIBPATH:"%DXSDK%\Lib\x86" ^
   user32.lib gdi32.lib kernel32.lib d3d9.lib
if errorlevel 1 goto :fail

echo.
echo ==== Building injector.exe ====
cl /nologo /EHsc /O2 /MT /Fe:injector.exe ^
   /D_CRT_SECURE_NO_WARNINGS ^
   injector.cpp ^
   /link /MACHINE:X86 kernel32.lib user32.lib
if errorlevel 1 goto :fail

del *.obj *.exp *.lib 2>nul
echo.
echo [OK] Built internal.dll and injector.exe
goto :end

:no_cl
echo [FAIL] cl.exe not on PATH. Open the x86 Native Tools Command Prompt.
goto :end

:no_dx
echo [FAIL] DirectX SDK not found at %DXSDK%
goto :end

:fail
del *.obj *.exp *.lib 2>nul
echo.
echo [FAIL] Build failed.

:end
pause
