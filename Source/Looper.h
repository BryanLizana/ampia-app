#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

// Looper multipista: la primera toma fija el largo del loop y cada toma
// siguiente queda en su propia pista (volumen, paneo, mute, borrar).
// Una toma graba a la vez todas las fuentes armadas (guitarra, voz, MIDI...),
// cada una en su pista; deshacer/rehacer va por toma completa.
// Las tomas siguientes duran lo mismo que el loop base: si se sigue
// grabando al pasar el final, se vuelve al inicio y se graba encima.
// Con el metrónomo activo la toma empieza al apretar REC (el click arranca
// ahí en el tiempo 1, sin cuenta previa) y la toma base se ajusta a compases enteros. Sin metrónomo el
// largo es libre y el tempo se ajusta al loop (como el Looper de Ableton).
// Con loop, el click sale de la posición dentro del loop: nunca se corre.
// Las fuentes de voz se graban también sin efecto (dry), para poder cambiar la
// voz después (ver VoiceTracks).
//
// Hilos: process() corre en el hilo de audio y nunca reserva memoria.
// maintain() corre en el hilo de mensajes y prepara los buffers libres.
class Looper
{
public:
    static constexpr int maxTracks = 16;
    static constexpr int maxSources = 8;
    // Toma base de hasta 5 min. Solo los buffers para la toma base tienen ese
    // tamaño; con un loop grabado, los libres se achican al largo del loop.
    static constexpr double maxSeconds = 300.0;
    static constexpr int beatsPerBar = 4;
    static constexpr float minBpm = 40.0f, maxBpm = 240.0f;

    enum class State
    {
        Empty,      // sin buffer
        Dirty,      // buffer usado, falta limpiarlo
        Ready,      // buffer limpio, listo para grabar
        Recording,
        Playing,
        Undone,     // deshecha: guarda el audio por si se rehace
        Deleting,   // pedido de borrado, el audio lo pasa a Dirty
        Loading     // el hilo de mensajes le está copiando audio (separar batería...)
    };

    struct Track
    {
        std::atomic<State> state { State::Empty };
        juce::AudioBuffer<float> buffer;    // estéreo, capacidad fija
        juce::AudioBuffer<float> dry;       // mono: la entrada sin efecto (misma capacidad)
        std::atomic<bool> hasDry { false }; // se grabó también la señal limpia
        int length = 0;                     // muestras (la base puede ser de varios compases)
        int order = 0;                      // orden de grabación (para deshacer); igual en toda la toma
        int source = 0;                     // fuente de la que se grabó
        std::atomic<int> stem { 0 };        // 0 = toma normal, si no Stem (sacada con Demucs)
        std::atomic<float> gain { 1.0f }, pan { 0.0f }, peak { 0.0f };
        std::atomic<bool> mute { false };
    };

    // Una entrada del motor, ya procesada (AmpliTube, instrumento...).
    struct Source
    {
        const float* left = nullptr;
        const float* right = nullptr;
        int latency = 0;      // muestras entre lo que se oye y lo que llega de esta fuente
        bool armed = false;
        bool onset = false;   // en este bloque empezó a sonar (nota MIDI o señal fuerte)
        float volume = 1.0f;  // volumen de la entrada: se aplica al reproducir, no a lo grabado
        const float* dry = nullptr;   // voz: la entrada sin efecto, alineada con left/right
    };

    // Hilo de mensajes
    void prepare (double sampleRate, int maxBlockSize);
    void maintain (int wantedReady);   // deja listos tantos buffers como fuentes armadas
    void toggleRecord() { recRequest = true; }
    void togglePlay()   { playRequest = true; }
    void seek (double fraction);       // ir a esa parte del loop (0..1); desde parado, arranca
    void undo();
    void redo();
    void deleteTrack (int index);
    void clearAll();

    void setMetronome (bool on)    { metronomeOn = on; }

    // REC al tocar (activo por defecto): sin metrónomo, REC queda esperando y la toma
    // empieza con el primer sonido de una entrada armada. Con metrónomo graba al apretar.
    void setWaitForSound (bool on) { waitForSound = on; }
    bool isWaitForSound() const    { return waitForSound; }
    bool isWaiting() const         { return waiting; }
    bool isMetronomeOn() const     { return metronomeOn; }
    void setBpm (float bpm);
    // Parado con el metrónomo: el próximo click (tiempo 1) cae ahora (tap tempo).
    void restartClick()            { clickRestart = true; }
    float getBpm() const           { return bpm; }
    bool isTempoLocked() const;    // con pistas sonando el tempo queda fijo

    Track& getTrack (int index)    { return tracks[(size_t) index]; }
    bool isPlaying() const         { return playing; }
    bool isRecording() const       { return recording; }
    bool hasFreeTrack (int needed) const;
    double getProgress() const;

    // Latencia de salida, para que lo visual vaya con lo que se oye.
    void setOutputLatency (int output) { outputLatency = output; }
    double getLoopSeconds() const  { return loopLength / sampleRate; }
    double getSampleRate() const   { return sampleRate; }

    // Pistas sacadas de otra con Demucs.
    enum Stem { None = 0, Drums = 1, Bass = 2 };

    // Copia el audio de una pista terminada (largo del loop). false si no se puede.
    bool copyTrack (int index, juce::AudioBuffer<float>& out) const;

    // Agrega una pista nueva del largo del loop con este audio (estéreo).
    // Devuelve el índice, o -1 si no hay pista libre o cambió el loop.
    int addTrack (const juce::AudioBuffer<float>& audio, int source, int stem, int expectedLength);

    int getLoopLength() const      { return loopLength; }

    // Voz re-editable: copia la señal limpia de una pista terminada (o deshecha).
    bool copyDry (int index, juce::AudioBuffer<float>& out) const;
    // Reemplaza el audio de esa pista (estéreo, largo del loop) si sigue siendo la misma toma.
    bool replaceAudio (int index, int order, const juce::AudioBuffer<float>& audio);

    // Mezcla de las pistas (volumen, paneo, mute) repetida "loops" veces.
    bool renderMix (juce::AudioBuffer<float>& out, int loops)              { return render (-1, out, loops); }
    // Una sola pista, igual que suena en la mezcla. false si no suena.
    bool renderTrack (int index, juce::AudioBuffer<float>& out, int loops) { return render (index, out, loops); }

    // Hilo de audio: graba las fuentes armadas y suma los loops y el click
    // a la salida (que ya trae lo que se está monitoreando).
    void process (const Source* sources, int numSources, float* left, float* right, int numSamples);

private:
    void requestRecord (const Source* sources, int numSources);
    bool startRecording (const Source* sources, int numSources);   // false si no hay pistas libres
    void stopRecording();
    void cancelRecording();
    bool render (int only, juce::AudioBuffer<float>& out, int loops);
    void fitTempo (int length);
    // wrap > 0: posición dentro del loop de ese largo (el pulso sale del loop).
    void addClick (float* left, float* right, int numSamples, juce::int64 startPos, int wrap);
    static void sizeBuffers (Track&, int size);
    double samplesPerBeat() const  { return sampleRate * 60.0 / bpm; }

    std::array<Track, maxTracks> tracks;
    juce::CriticalSection allocLock;

    std::atomic<bool> recRequest { false }, playRequest { false }, cancelRequest { false };
    std::atomic<double> seekRequest { -1.0 };   // -1 = ninguno
    std::atomic<bool> clickRestart { false };
    std::atomic<bool> playing { false }, recording { false };
    std::atomic<bool> metronomeOn { false }, waitForSound { true }, waiting { false };
    std::atomic<float> bpm { 100.0f };
    std::atomic<int> loopLength { 0 }, outputLatency { 0 };
    std::atomic<juce::int64> position { 0 };
    std::array<std::atomic<float>, maxSources> sourceVolume {};   // último volumen de cada entrada

    double sampleRate = 48000.0;
    int capacity = 0;
    int recCapacity = 0;   // el buffer más chico de la toma en curso
    int requiredSize() const { const int len = loopLength; return len > 0 ? len : capacity; }
    std::array<int, maxSources> recSlots {};     // pistas que se están grabando
    std::array<int, maxSources> recComps {};     // compensación de latencia de cada una
    int numRecording = 0;
    juce::int64 recIndex = 0, freeClickPos = 0;
    std::atomic<int> nextOrder { 1 };
};
