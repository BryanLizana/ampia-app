#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// Cadenas de voz para cantar en vivo (sin latencia):
//   EQ -> Compresor -> Compresor -> Saturador -> Delay al tempo -> Reverb
// Estilos: Chester (cadena tipo rack de Ableton), Cobain y Natural.
// Perillas: entrada, compresión, saturación, delay, reverb y salida.
class VoiceChain : public juce::AudioPluginInstance
{
public:
    // Valores de cada estilo.
    struct Style
    {
        const char* name;       // "Chester"
        const char* menuText;
        const char* chainText;  // descripción en la ventana
        double highpassHz, lowShelfHz, lowShelfDb, midHz, midDb, midQ, highShelfHz, highShelfDb;
        float comp1Threshold, comp1Ratio, comp1AttackMs, comp1ReleaseMs;
        float comp2Threshold, comp2Ratio, comp2AttackMs, comp2ReleaseMs;
        float satDriveDb, satOutDb;
        double delayBeats;      // 0.5 = corchea, 1 = negra
        float delayFeedback, delayFilterHz;
        float roomSize, damping, preDelayMs;
        float satDefault, delayDefault, reverbDefault;   // perillas al cargar
    };

    static constexpr int numStyles = 3;
    static const Style styles[numStyles];

    explicit VoiceChain (int style = 0);

    static juce::File virtualFile (int style = 0);   // identifica al efecto en la configuración
    static int styleForFile (const juce::File&);      // -1 si no es una voz AmpBox
    static std::atomic<float> bpm;            // tempo del looper (para el delay)

    const juce::String getName() const override { return juce::String ("Voz ") + style.name; }
    void fillInPluginDescription (juce::PluginDescription&) const override;

    void prepareToPlay (double sampleRate, int maxBlockSize) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;

    double getTailLengthSeconds() const override { return 2.0; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
    }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    int getNumPrograms() override                         { return 1; }
    int getCurrentProgram() override                      { return 0; }
    void setCurrentProgram (int) override                 {}
    const juce::String getProgramName (int) override      { return style.name; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Perillas (AudioPluginInstance no admite parámetros propios).
    struct Knob
    {
        const char* id;
        const char* name;
        float min, max, defaultValue;
        const char* suffix;
        std::atomic<float> value;
    };

    enum { inGain, compAmount, satMix, delayMix, reverbMix, outGain, numKnobs };
    std::array<Knob, numKnobs> knobs
    {{
        { "in",     "Entrada",    -12.0f,  12.0f,   0.0f, " dB", { 0.0f } },
        { "comp",   "Compresion",   0.0f, 150.0f, 100.0f, " %",  { 100.0f } },
        { "sat",    "Saturacion",   0.0f, 100.0f,  60.0f, " %",  { 60.0f } },
        { "delay",  "Delay",        0.0f, 100.0f,  11.5f, " %",  { 11.5f } },
        { "reverb", "Reverb",       0.0f, 100.0f,  15.0f, " %",  { 15.0f } },
        { "out",    "Salida",     -24.0f,  12.0f,   0.0f, " dB", { 0.0f } },
    }};

    float get (int knob) const { return knobs[(size_t) knob].value.load(); }

    const int styleIndex;
    const Style& style;

private:
    struct Compressor
    {
        float thresholdDb = 0.0f, ratio = 1.0f, kneeDb = 6.0f, makeupDb = 0.0f;
        float attack = 0.0f, release = 0.0f, env = 0.0f;   // coeficientes y estado
        void setTimes (double sr, double attackMs, double releaseMs);
        float gainFor (float level);
    };

    void updateFilters();

    double sr = 48000.0;
    std::array<juce::IIRFilter, 2> highpass, lowShelf, mid, highShelf, delayFilter;
    Compressor comp1, comp2;

    juce::AudioBuffer<float> delayLine, preDelayLine, wet;
    int delayPos = 0, preDelayPos = 0;
    juce::Reverb reverb;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceChain)
};
