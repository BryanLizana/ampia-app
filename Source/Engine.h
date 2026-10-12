#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "Looper.h"
#include "PresetLibrary.h"
#include "PcAudio.h"

// Motor de audio. Varias entradas (canales) a la vez, como pistas de Ableton:
//   Guitarra: entrada de audio -> amplificador (AmpliTube, Guitar Rig...)
//   Audio:    entrada de audio (voz, línea) -> efecto VST3 opcional
//   MIDI:     teclado / batería MIDI -> instrumento VST3
//   PC:       lo que suena en Windows (YouTube, música...)
// Las armadas se graban juntas en el looper, cada una en su pista.
class Engine : public juce::AudioIODeviceCallback,
               private juce::MidiInputCallback
{
public:
    Engine();
    ~Engine() override;

    juce::AudioDeviceManager deviceManager;
    Looper looper;

    // Acciones que se pueden asignar a una tecla.
    struct KeyAction
    {
        juce::String id, name;
        juce::KeyPress defaultKey;
    };

    static const std::vector<KeyAction>& getKeyActions();
    std::map<juce::String, juce::KeyPress> keys;   // id de acción -> tecla
    void resetKeys();

    // Abre la interfaz (prefiere ASIO y preferredInterface) y arranca el audio.
    void start();
    void stop();

    // Vuelve a buscar interfaces y teclados MIDI (por si se conectaron después).
    // Orden: interfaz preferida (ASIO) -> otra interfaz ASIO -> audio de Windows (micrófono y
    // parlantes de la laptop, o lo que esté por defecto).
    void reconnect();

    // Cada pocos segundos: si la interfaz preferida se conectó o desconectó, cambia de
    // interfaz sola. true si cambió.
    bool checkAudioDevice();

    // Activa los teclados MIDI que se conectaron o desconectaron. true si cambió algo.
    bool checkMidiDevices();

    // Launchkey MK3 en modo DAW (puerto MIDIIN2/MIDIOUT2), como con Ableton:
    // lo que llega por ese puerto va a onDawMessage (hilo de mensajes) y no a
    // los instrumentos; sendDaw manda luces de pads y botones.
    std::function<void (const juce::MidiMessage&)> onDawMessage;
    void sendDaw (const juce::MidiMessage&);
    bool isDawConnected() const { return dawOut != nullptr; }
    void logText (const juce::String& text);   // escribe en ampbox.log

    // Presets de AmpliTube: biblioteca completa + favoritos de AmpBox.
    // Se cargan escribiendo el .at5p en el estado del plugin (ver AmpliTubePreset).
    PresetLibrary library;
    bool selectPreset (const juce::String& path);   // relativa a la carpeta Presets
    bool loadPresetFile (const juce::File& file);

    //==========================================================================
    // Canales de entrada. Se arranca sin ninguno; se agregan con addChannel.
    static constexpr int maxChannels = Looper::maxSources;

    enum class ChannelType { Guitar, Audio, Midi, Pc };

    struct Channel
    {
        std::atomic<bool> used { false };
        ChannelType type = ChannelType::Audio;
        juce::String name;
        std::atomic<int> input { 0 };     // Guitarra/Audio: índice entre las entradas activas
        juce::String midiDevice;          // MIDI: identificador del teclado; vacío = todos
        std::unique_ptr<juce::AudioPluginInstance> plugin;   // Audio: efecto; MIDI: instrumento
        juce::File pluginFile;
        bool pluginFailed = false;        // el guardado no se pudo abrir (no está instalado...)
        int shareGroup = -1;              // Guitarra: grupo que comparte un amplificador (-1 = propio)
        juce::MemoryBlock ampState;       // su preset mientras la copia compartida la tiene otra
        std::atomic<bool> armed { false }, monitor { true };
        std::atomic<float> volume { 1.0f };   // como el fader de Ableton: no cambia lo grabado
        std::atomic<float> peak { 0.0f };

        // Solo para el motor
        juce::AudioBuffer<float> buffer;
        juce::MidiBuffer midi;
        juce::MidiMessageCollector collector;
    };

    Channel& getChannel (int index)             { return channels[(size_t) index]; }
    int addChannel (ChannelType type);          // índice, -1 si no hay lugar, -2 PC sin ASIO
    bool isAsio() const { return deviceManager.getCurrentAudioDeviceType() == "ASIO"; }
    void removeChannel (int index);
    void setChannelName (int index, const juce::String& name);
    void setChannelMidiDevice (int index, const juce::String& identifier);
    void setChannelInput (int index, int input) { channels[(size_t) index].input = input; saveSettings(); }
    bool loadChannelPlugin (int index, const juce::File& file, juce::String& error);
    void clearChannelPlugin (int index);
    // Plugins guardados en la sesión anterior: se cargan de a uno (AmpliTube tarda
    // hasta 1 min) para que la pantalla avise cuál se está cargando.
    std::vector<int> getPendingPlugins() const;
    bool restoreChannelPlugin (int index);
    juce::AudioProcessor* getChannelProcessor (int index) const;

    // Rack de guitarras: varias entradas Guitarra (cada una con su amplificador)
    // y una activa, que es la que suena y se graba (-1 = no hay guitarras).
    void selectGuitar (int index);
    void nextGuitar();
    int getActiveGuitar() const { return activeGuitar; }
    int countGuitars() const;
    int firstGuitar() const;                    // -1 si no hay

    // Amplificador compartido: varias guitarras usan una sola copia del plugin (un solo
    // AmpliTube), cada una con su preset. La copia la tiene la guitarra activa del grupo;
    // al cambiar de guitarra se mueve y se le carga el preset de la nueva (corte breve).
    void shareAmp (int index, int from);         // index pasa a usar el amplificador de from
    // Por defecto AmpliTube se comparte: si otra guitarra ya lo tiene, la guitarra que lo
    // elige usa esa copia. Devuelve esa guitarra, o -1 si hay que abrir uno nuevo.
    int ampliTubeToShare (int index, const juce::File& file) const;
    void unshareAmp (int index);                 // deja el grupo (queda sin amplificador)
    void takeSharedAmp (int index);              // trae la copia a esta guitarra (apaga la que la tenía)
    bool isSharedWaiting (int index) const;      // es del grupo, pero la copia la tiene otra
    bool isShared (int index) const { return channels[(size_t) index].shareGroup >= 0; }
    std::function<void (int from, int to)> onSharedAmpMoved;   // hilo de mensajes (mover la ventana del plugin)

    // Entradas con AmpliTube que se están procesando (OIR o armadas). Dos copias a la vez
    // de AmpliTube 5 pueden cerrar la app.
    juce::StringArray getActiveAmpliTubes() const;

    // Armar por número de pista visible (1 = la primera usada).
    void toggleArm (int number);

    // Teclas 1-8: armar y OIR juntos (si ya están los dos, apaga los dos).
    void toggleEntry (int number);
    int countArmed() const;
    int countChannels() const;

    static juce::String typeName (ChannelType);
    juce::String getPcStatus() const { return pcAudio.getStatus(); }

    // Afinador: la entrada (sin efecto) de un canal va a una cola que lee la ventana.
    void setTunerChannel (int index) { tunerChannel = index; }   // -1 = apagado
    int getTunerChannel() const      { return tunerChannel; }
    int readTuner (float* dest, int maxSamples);                  // hilo de mensajes
    double getSampleRate() const     { return sampleRate; }

    float takeInputPeak()  { return inputPeak.exchange (0.0f); }
    float takeOutputPeak() { return outputPeak.exchange (0.0f); }

    // Latencia total ida y vuelta en ms (entrada + salida + amplificador activo).
    double getLatencyMs() const;

    bool presetsHidden = false;  // columna de presets oculta (se guarda)

    // Nombre (o parte) de la interfaz que se prefiere si está conectada. Se guarda en
    // ampbox.xml; vacío = ninguna en especial (la primera ASIO que haya).
    juce::String preferredInterface { "Audient" };

    void saveSettings() const;   // interfaz, canales, teclas... (nada antes de start())
    void saveState() const;      // lo anterior + estado de los plugins

    static juce::File getDataFolder();

    void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                           float* const* outputs, int numOutputs,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void enableMidiInputs();
    std::unique_ptr<juce::AudioPluginInstance> createPlugin (const juce::File& file, juce::String& error);
    void prepare (juce::AudioPluginInstance&);
    void sizeChannelBuffer (Channel&);
    int sharedHolder (int group) const;          // guitarra que tiene la copia (-1 = ninguna)
    int groupLeader (int group) const;           // la que la carga al arrancar
    void moveSharedAmp (int from, int to);
    int nextShareGroup = 0;

    juce::AudioPluginFormatManager formatManager;
    std::array<Channel, maxChannels> channels;
    juce::CriticalSection lock;          // canales y plugins (el audio solo lo intenta)
    juce::SpinLock midiLock;             // lo que lee el hilo MIDI
    juce::StringArray knownMidiIds;      // teclados ya activados
    void connectDaw();
    bool openDevice (const juce::String& type, const juce::String& input, const juce::String& output);
    bool isAudientPresent();
    void applySafeMonitoring();
    bool audientWasPresent = false;
    bool started = false;                // si se cierra antes de start(), no pisar lo guardado
    std::unique_ptr<juce::MidiOutput> dawOut;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);   // para los mensajes MIDI en cola al cerrar
    juce::String dawInId, dawMainId;     // leídos por el hilo MIDI (con midiLock)
    juce::AudioBuffer<float> mix;
    std::atomic<int> activeGuitar { -1 };
    juce::AudioPluginInstance* getPresetTarget() const;
    PcAudio pcAudio;                     // para la entrada PC (solo puede haber una)

    double sampleRate = 48000.0;
    std::atomic<int> deviceInLatency { 0 }, deviceOutLatency { 0 };
    int blockSize = 256;
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };

    std::atomic<int> tunerChannel { -1 };
    juce::AbstractFifo tunerFifo { 32768 };
    std::vector<float> tunerData = std::vector<float> (32768);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Engine)
};
