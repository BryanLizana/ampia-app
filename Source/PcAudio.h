#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

// true si Windows tiene un dispositivo de audio (entrada o salida) con ese nombre.
// Consulta liviana (winmm): no toca los dispositivos que AmpBox tiene abiertos.
bool isWindowsAudioDevicePresent (const juce::String& nameFragment);

// Sonido de la PC (YouTube, música...): captura lo que Windows manda a la
// salida predeterminada (WASAPI loopback) en un hilo propio, lo pasa a la
// frecuencia de la interfaz y lo deja en una cola para el hilo de audio.
// No incluye lo que sale por ASIO (AmpBox), así que no se realimenta; con el
// audio de Windows sí, por eso la entrada PC solo se permite con ASIO.
class PcAudio : private juce::Thread
{
public:
    PcAudio();
    ~PcAudio() override;

    void start();
    void stop();
    bool isActive() const { return isThreadRunning(); }

    void setTargetRate (double rate) { targetRate = rate; }
    juce::String getStatus() const;

    // Hilo de audio: llena left/right (con ceros si no hay nada sonando).
    void read (float* left, float* right, int numSamples);

private:
    void run() override;
    void push (const float* left, const float* right, int n);
    void setStatus (const juce::String&);

    std::atomic<double> targetRate { 48000.0 };
    juce::AbstractFifo fifo { 1 << 17 };
    bool primed = false;   // solo el hilo de audio: hay colchón suficiente para leer
    juce::AudioBuffer<float> ring { 2, 1 << 17 };

    mutable juce::CriticalSection statusLock;
    juce::String status { "Apagado" };
};
