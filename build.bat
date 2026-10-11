@echo off
rem Compila AmpBox (Release). Uso: build.bat
rem Necesita Visual Studio 2022 (o Build Tools) con "Desarrollo para el escritorio con C++",
rem que trae CMake y Ninja.
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo No se encontro Visual Studio 2022: instalar Build Tools 2022 con Desarrollo para el escritorio con C++ & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Falta Desarrollo para el escritorio con C++ en Visual Studio 2022 & exit /b 1)

rem VsDevCmd busca vswhere en el PATH.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul

if not exist build\build.ninja cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
