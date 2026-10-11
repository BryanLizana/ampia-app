@echo off
rem Abre AmpBox. Si no esta compilado, lo compila primero.
cd /d "%~dp0"
set EXE=%~dp0build\AmpBox_artefacts\Release\AmpBox.exe

if not exist "%EXE%" (
    echo Compilando AmpBox, espera unos minutos...
    call build.bat || (echo No se pudo compilar. & pause & exit /b 1)
)

start "" "%EXE%"
