@echo off
rem Builds bin\d3d9.dll (32-bit) with the Visual Studio x86 toolchain.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
    echo Visual Studio with C++ build tools not found.
    exit /b 1
)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1

cd /d "%~dp0"
if not exist bin mkdir bin
if not exist build mkdir build
cl /nologo /O2 /W4 /MT /LD src\d3d9shim.c /Fobuild\ /Fdbuild\ ^
   /link /OUT:bin\d3d9.dll /IMPLIB:build\d3d9shim.lib ^
   kernel32.lib user32.lib || exit /b 1
echo Built bin\d3d9.dll
