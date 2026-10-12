#pragma once
#include "Engine.h"
#include "VoiceChain.h"

// Pistas de voz re-editables (como en Ableton, donde el efecto queda vivo en la pista).
// Lo grabado con una voz AmpBox guarda también la entrada limpia; cada pista tiene su
// propia voz (estilo y perillas). Al cambiarla, la voz limpia se vuelve a procesar en
// otro hilo y reemplaza el audio de la pista. La guitarra y lo demás quedan como se grabaron.
class VoiceTracks : private juce::Timer
{
public:
    explicit VoiceTracks (Engine&);
    ~VoiceTracks() override;

    bool canEdit (int slot) const;
    bool isRendering (int slot) const;
    void showEditor (int slot, const juce::String& title);

private:
    struct Entry
    {
        int order = 0;                          // toma (si la pista se reusa, es otra)
        std::unique_ptr<VoiceChain> voice;      // estilo y perillas de esta pista
        int renderedStyle = -1;                 // lo que tiene la pista ahora
        VoiceChain::KnobValues renderedKnobs {};
        bool rendering = false;
        std::unique_ptr<juce::DocumentWindow> window;
    };

    class Panel;

    void timerCallback() override;
    void adopt (int slot);
    void render (int slot);
    void renderFinished (int slot, int order, int style, const VoiceChain::KnobValues&, juce::AudioBuffer<float>&);
    void setStyle (int slot, int style);

    Engine& engine;
    std::array<Entry, Looper::maxTracks> entries;
    std::shared_ptr<bool> alive = std::make_shared<bool> (true);   // para los procesos que terminan al cerrar

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceTracks)
};
