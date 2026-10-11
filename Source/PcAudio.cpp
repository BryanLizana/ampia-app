#include "PcAudio.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmsystem.h>
#pragma comment (lib, "winmm.lib")

bool isWindowsAudioDevicePresent (const juce::String& nameFragment)
{
    for (UINT i = 0, n = waveInGetNumDevs(); i < n; ++i)
    {
        WAVEINCAPSW caps {};
        if (waveInGetDevCapsW (i, &caps, sizeof (caps)) == MMSYSERR_NOERROR
            && juce::String (caps.szPname).containsIgnoreCase (nameFragment))
            return true;
    }

    for (UINT i = 0, n = waveOutGetNumDevs(); i < n; ++i)
    {
        WAVEOUTCAPSW caps {};
        if (waveOutGetDevCapsW (i, &caps, sizeof (caps)) == MMSYSERR_NOERROR
            && juce::String (caps.szPname).containsIgnoreCase (nameFragment))
            return true;
    }

    return false;
}

namespace
{
    template <typename T>
    struct ComPtr
    {
        T* p = nullptr;
        ~ComPtr() { if (p != nullptr) p->Release(); }
        T* operator->() const { return p; }
        T** operator&() { return &p; }
    };

    // GUIDs escritos aquí para no depender de uuid.lib.
    const CLSID clsidEnumerator = { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
    const IID iidEnumerator     = { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
    const IID iidAudioClient    = { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
    const IID iidCaptureClient  = { 0xC8ADBD64, 0xE71E, 0x48A0, { 0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17 } };
    const GUID subtypeFloat     = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
}

PcAudio::PcAudio() : juce::Thread ("AmpBox PC audio") {}

PcAudio::~PcAudio()
{
    stop();
}

void PcAudio::start()
{
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::high);
}

void PcAudio::stop()
{
    stopThread (2000);
}

juce::String PcAudio::getStatus() const
{
    const juce::ScopedLock sl (statusLock);
    return status;
}

void PcAudio::setStatus (const juce::String& s)
{
    const juce::ScopedLock sl (statusLock);
    status = s;
}

void PcAudio::push (const float* left, const float* right, int n)
{
    // Si la cola se llena (nadie lee), se descarta lo nuevo.
    const auto scope = fifo.write (juce::jmin (n, fifo.getFreeSpace()));

    if (scope.blockSize1 > 0)
    {
        ring.copyFrom (0, scope.startIndex1, left, scope.blockSize1);
        ring.copyFrom (1, scope.startIndex1, right, scope.blockSize1);
    }

    if (scope.blockSize2 > 0)
    {
        ring.copyFrom (0, scope.startIndex2, left + scope.blockSize1, scope.blockSize2);
        ring.copyFrom (1, scope.startIndex2, right + scope.blockSize1, scope.blockSize2);
    }
}

void PcAudio::read (float* left, float* right, int numSamples)
{
    // Windows entrega paquetes de ~10 ms y este bloque es de pocos ms: con un
    // colchón de 40 ms no quedan huecos (chasquidos) cuando un paquete se atrasa.
    const double rate = targetRate.load();
    const int cushion = (int) (0.04 * rate);

    // Que no se acumule retraso: si hay más de 120 ms en cola, se vuelve al colchón.
    if (fifo.getNumReady() > (int) (0.12 * rate) + numSamples)
        fifo.read (fifo.getNumReady() - cushion);   // el ScopedRead descarta al destruirse

    // Tras vaciarse (silencio en la PC, o al empezar) se espera a juntar el colchón.
    if (! primed && fifo.getNumReady() < cushion + numSamples)
    {
        juce::FloatVectorOperations::clear (left, numSamples);
        juce::FloatVectorOperations::clear (right, numSamples);
        return;
    }

    primed = fifo.getNumReady() >= numSamples;   // si no alcanza, este bloque es el último hasta rellenar

    const auto scope = fifo.read (juce::jmin (numSamples, fifo.getNumReady()));
    int done = 0;

    for (auto [start, size] : { std::pair { scope.startIndex1, scope.blockSize1 },
                                std::pair { scope.startIndex2, scope.blockSize2 } })
    {
        if (size <= 0)
            continue;

        juce::FloatVectorOperations::copy (left + done, ring.getReadPointer (0, start), size);
        juce::FloatVectorOperations::copy (right + done, ring.getReadPointer (1, start), size);
        done += size;
    }

    if (done < numSamples)
    {
        juce::FloatVectorOperations::clear (left + done, numSamples - done);
        juce::FloatVectorOperations::clear (right + done, numSamples - done);
    }
}

void PcAudio::run()
{
    CoInitializeEx (nullptr, COINIT_MULTITHREADED);

    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioCaptureClient> capture;
        WAVEFORMATEX* format = nullptr;

        auto fail = [this] (const juce::String& what) { setStatus ("Error: " + what); };

        if (FAILED (CoCreateInstance (clsidEnumerator, nullptr, CLSCTX_ALL, iidEnumerator, (void**) &enumerator)))
            fail ("sin enumerador de audio");
        else if (FAILED (enumerator->GetDefaultAudioEndpoint (eRender, eConsole, &device)))
            fail ("no hay salida de audio en Windows");
        else if (FAILED (device->Activate (iidAudioClient, CLSCTX_ALL, nullptr, (void**) &client)))
            fail ("no se pudo abrir la salida");
        else if (FAILED (client->GetMixFormat (&format)))
            fail ("formato desconocido");
        else if (FAILED (client->Initialize (AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                             2000000 /* 200 ms */, 0, format, nullptr)))
            fail ("no se pudo capturar (loopback)");
        else if (FAILED (client->GetService (iidCaptureClient, (void**) &capture)))
            fail ("sin servicio de captura");
        else if (FAILED (client->Start()))
            fail ("no arrancó la captura");
        else
        {
            const int channels = format->nChannels;
            const double inRate = format->nSamplesPerSec;
            bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
                isFloat = IsEqualGUID (reinterpret_cast<WAVEFORMATEXTENSIBLE*> (format)->SubFormat, subtypeFloat) != 0;
            const bool is16 = ! isFloat && format->wBitsPerSample == 16;

            if (! isFloat && ! is16)
            {
                fail ("formato de Windows no soportado");
            }
            else
            {
                setStatus ("Capturando " + juce::String ((int) inRate) + " Hz");

                // Paso a la frecuencia de la interfaz (lineal, alcanza para grabar la PC).
                std::vector<float> inL, inR, outL, outR;
                double pos = 0.0;

                while (! threadShouldExit())
                {
                    UINT32 packet = 0;

                    while (SUCCEEDED (capture->GetNextPacketSize (&packet)) && packet > 0)
                    {
                        BYTE* data = nullptr;
                        UINT32 frames = 0;
                        DWORD flags = 0;

                        if (FAILED (capture->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
                            break;

                        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

                        for (UINT32 f = 0; f < frames; ++f)
                        {
                            float l = 0.0f, r = 0.0f;

                            if (! silent)
                            {
                                if (isFloat)
                                {
                                    auto* s = reinterpret_cast<const float*> (data) + f * (UINT32) channels;
                                    l = s[0];
                                    r = channels > 1 ? s[1] : s[0];
                                }
                                else
                                {
                                    auto* s = reinterpret_cast<const int16_t*> (data) + f * (UINT32) channels;
                                    l = s[0] / 32768.0f;
                                    r = (channels > 1 ? s[1] : s[0]) / 32768.0f;
                                }
                            }

                            inL.push_back (l);
                            inR.push_back (r);
                        }

                        capture->ReleaseBuffer (frames);
                    }

                    const double step = inRate / targetRate.load();
                    outL.clear();
                    outR.clear();

                    while (pos + 1.0 < (double) inL.size())
                    {
                        const auto i = (size_t) pos;
                        const auto frac = (float) (pos - (double) i);
                        outL.push_back (inL[i] + frac * (inL[i + 1] - inL[i]));
                        outR.push_back (inR[i] + frac * (inR[i + 1] - inR[i]));
                        pos += step;
                    }

                    const auto used = (size_t) pos;
                    inL.erase (inL.begin(), inL.begin() + (std::ptrdiff_t) used);
                    inR.erase (inR.begin(), inR.begin() + (std::ptrdiff_t) used);
                    pos -= (double) used;

                    if (! outL.empty())
                        push (outL.data(), outR.data(), (int) outL.size());

                    wait (5);
                }

                client->Stop();
                setStatus ("Apagado");
            }
        }

        if (format != nullptr)
            CoTaskMemFree (format);
    }

    CoUninitialize();
}
