#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

// Batería propia de AmpBox con los sonidos sueltos de Ableton (Core Library,
// One Shots\Drums). Los 16 pads van en las notas 36-51 (General MIDI, como el
// modo Drum del Launchkey); el resto de las teclas repite esos 16 sonidos.
// Cada kit es un "programa" del plugin.
class DrumKit : public juce::AudioPluginInstance
{
public:
    DrumKit();
    ~DrumKit() override;

    // Ruta que identifica a la batería en la configuración (no existe en disco).
    static juce::File virtualFile();
    static juce::File findSamplesFolder();   // vacío si Ableton no está

    static constexpr int firstNote = 36, numPads = 16;
    static juce::String padName (int pad);
    juce::String getPadSample (int pad) const;   // nombre del archivo cargado ("" si falta)

    //==========================================================================
    const juce::String getName() const override { return "Bateria AmpBox"; }
    void fillInPluginDescription (juce::PluginDescription&) const override;

    void prepareToPlay (double sampleRate, int) override { deviceRate = sampleRate; }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioPluginInstance::processBlock;

    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return kitIndex; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

private:
    struct Sample
    {
        juce::AudioBuffer<float> audio;   // estéreo
        double rate = 44100.0;
        juce::String name;
    };

    struct Kit
    {
        std::array<std::unique_ptr<Sample>, numPads> pads;
    };

    struct Voice
    {
        const Sample* sample = nullptr;
        double pos = 0.0;
        float gain = 0.0f;
        int pad = -1;
    };

    void startVoice (int pad, float velocity);

    std::unique_ptr<Kit> kit;
    juce::SpinLock kitLock;               // el audio la toma en cada bloque; el cambio de kit, un instante
    std::array<Voice, 32> voices;
    int kitIndex = 0;
    double deviceRate = 48000.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumKit)
};
