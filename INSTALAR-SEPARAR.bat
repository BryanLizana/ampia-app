@echo off
rem Deja listo el boton SEPARAR de AmpBox (saca bateria y bajo con Demucs).
rem Instala demucs, torch, soundfile y numpy en el Python de la PC (~2 GB)
rem y baja el modelo htdemucs (~80 MB) a %APPDATA%\AmpBox\modelos.
rem Se puede correr de nuevo sin problema: lo que ya esta no se vuelve a bajar.
title AmpBox - instalar SEPARAR
setlocal

echo === 1/3 Python ===
python -c "import sys; assert sys.version_info >= (3, 9)" >nul 2>nul
if errorlevel 1 (
    echo No se encontro Python 3.9 o mas nuevo en el PATH.
    echo Instalalo con:  winget install Python.Python.3.12
    echo o desde python.org marcando "Add python.exe to PATH". Despues vuelve a correr este archivo.
    pause
    exit /b 1
)
python --version

echo.
echo === 2/3 Librerias (demucs, torch, soundfile) - puede tardar varios minutos ===
python -m pip install demucs soundfile numpy
if errorlevel 1 (
    echo Fallo la instalacion con pip. Revisa la conexion a internet y vuelve a intentar.
    pause
    exit /b 1
)

echo.
echo === 3/3 Modelo htdemucs ===
python -c "import os; os.environ.setdefault('TORCH_HOME', os.path.join(os.environ['APPDATA'], 'AmpBox', 'modelos')); from demucs.pretrained import get_model; get_model('htdemucs'); print('Modelo listo')"
if errorlevel 1 (
    echo No se pudo bajar el modelo. Se intentara de nuevo la primera vez que uses SEPARAR.
    pause
    exit /b 1
)

echo.
echo Listo: SEPARAR ya funciona en AmpBox.
pause
