"""Separa un loop de AmpBox con Demucs (htdemucs) y guarda las pistas pedidas.

Uso:  python separar.py <entrada.wav> <carpeta_salida> drums,bass

Deja <carpeta_salida>/<pista>.wav (float, misma frecuencia que la entrada).
El modelo (~80 MB) se descarga la primera vez en %APPDATA%\\AmpBox\\modelos.
Como un loop se repite, se separa con 3 s del otro extremo a cada lado para
que el principio y el final no queden cortados.
"""

import os
import sys
import time

os.environ.setdefault("TORCH_HOME", os.path.join(os.environ.get("APPDATA", "."), "AmpBox", "modelos"))

import numpy as np
import soundfile as sf
import torch
from demucs.apply import apply_model
from demucs.audio import convert_audio
from demucs.pretrained import get_model


def main():
    entrada, salida, pistas = sys.argv[1], sys.argv[2], sys.argv[3].split(",")
    inicio = time.time()

    audio, sr = sf.read(entrada, dtype="float32", always_2d=True)   # (muestras, canales)
    largo = audio.shape[0]

    torch.set_num_threads(os.cpu_count() or 4)
    modelo = get_model("htdemucs")
    modelo.eval()

    # Loop: se agrega el final antes y el principio después (3 s) para que
    # los bordes se separen con contexto; luego se recorta.
    margen = min(largo, 3 * sr)
    x = torch.from_numpy(np.concatenate([audio[largo - margen:], audio, audio[:margen]]).T.copy())
    x = convert_audio(x, sr, modelo.samplerate, modelo.audio_channels)
    ref = x.mean(0)
    media, desv = ref.mean(), ref.std() + 1e-8

    with torch.no_grad():
        fuentes = apply_model(modelo, ((x - media) / desv)[None], device="cpu",
                              split=True, overlap=0.25, progress=False)[0]
    fuentes = fuentes * desv + media

    os.makedirs(salida, exist_ok=True)
    for nombre in pistas:
        y = convert_audio(fuentes[modelo.sources.index(nombre)], modelo.samplerate, sr, 2)
        y = y.numpy().T
        medio = y[margen:margen + largo]
        if medio.shape[0] < largo:
            medio = np.pad(medio, ((0, largo - medio.shape[0]), (0, 0)))
        sf.write(os.path.join(salida, nombre + ".wav"), medio, sr, subtype="FLOAT")

    print(f"OK {time.time() - inicio:.1f} s", flush=True)


if __name__ == "__main__":
    main()
