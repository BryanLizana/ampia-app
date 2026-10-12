#include "VoiceTracks.h"

namespace
{
    const juce::Colour background (0xff1b1d21), accent (0xffff8a1f);

    class VoiceWindow : public juce::DocumentWindow
    {
    public:
        VoiceWindow (const juce::String& title, juce::Component* content, std::function<void()> onCloseIn)
            : DocumentWindow (title, background, closeButton), onClose (std::move (onCloseIn))
        {
            setUsingNativeTitleBar (true);
            setContentOwned (content, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { onClose(); }

    private:
        std::function<void()> onClose;
    };
}

//==============================================================================
// Ventana de una pista: el estilo arriba y las perillas de la voz debajo.
class VoiceTracks::Panel : public juce::Component, private juce::Timer
{
public:
    Panel (VoiceTracks& ownerIn, int slotIn) : owner (ownerIn), slot (slotIn)
    {
        for (int i = 0; i < VoiceChain::numStyles; ++i)
            styleBox.addItem (juce::String ("Voz ") + VoiceChain::styles[i].name, i + 1);

        styleBox.setSelectedId (voice().styleIndex + 1, juce::dontSendNotification);
        styleBox.onChange = [this] { owner.setStyle (slot, styleBox.getSelectedId() - 1); };
        addAndMakeVisible (styleBox);

        hint.setText ("Cambia la voz de lo ya grabado", juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.6f));
        addAndMakeVisible (hint);

        status.setJustificationType (juce::Justification::centredRight);
        status.setColour (juce::Label::textColourId, accent);
        addAndMakeVisible (status);

        setEditor();
        startTimerHz (10);
    }

    ~Panel() override { clearEditor(); }

    // El editor apunta a la voz: se borra antes de cambiarla.
    void clearEditor() { editor = nullptr; }

    void setEditor()
    {
        editor.reset (voice().createEditor());
        addAndMakeVisible (*editor);
        setSize (editor->getWidth(), editor->getHeight() + header);
        resized();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (header).reduced (12, 6);
        styleBox.setBounds (top.removeFromLeft (160));
        top.removeFromLeft (10);
        status.setBounds (top.removeFromRight (170));
        hint.setBounds (top);

        if (editor != nullptr)
            editor->setBounds (r);
    }

    void paint (juce::Graphics& g) override { g.fillAll (background); }

private:
    void timerCallback() override
    {
        status.setText (owner.isRendering (slot) ? "Aplicando a la pista..." : "Listo", juce::dontSendNotification);
    }

    VoiceChain& voice() { return *owner.entries[(size_t) slot].voice; }

    static constexpr int header = 40;
    VoiceTracks& owner;
    const int slot;
    juce::ComboBox styleBox;
    juce::Label hint, status;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
};

//==============================================================================
VoiceTracks::VoiceTracks (Engine& engineIn) : engine (engineIn)
{
    startTimerHz (10);
}

VoiceTracks::~VoiceTracks()
{
    stopTimer();

    for (auto& e : entries)
    {
        e.window = nullptr;   // primero la ventana (su editor apunta a la voz)
        e.voice = nullptr;
    }
}

bool VoiceTracks::canEdit (int slot) const     { return entries[(size_t) slot].voice != nullptr; }
bool VoiceTracks::isRendering (int slot) const { return entries[(size_t) slot].rendering; }

void VoiceTracks::timerCallback()
{
    auto& looper = engine.looper;

    for (int slot = 0; slot < Looper::maxTracks; ++slot)
    {
        auto& e = entries[(size_t) slot];
        auto& t = looper.getTrack (slot);
        const auto state = t.state.load();
        const bool editable = t.hasDry && (state == Looper::State::Playing || state == Looper::State::Undone);

        if (! editable || (e.voice != nullptr && e.order != t.order))
        {
            // Pista borrada o reusada para otra toma.
            e.window = nullptr;
            e.voice = nullptr;
            e.order = 0;
        }

        if (editable && e.voice == nullptr)
            adopt (slot);

        // Si se movió algo, se vuelve a procesar (lo que se mueva mientras tanto, en la siguiente).
        if (e.voice != nullptr && ! e.rendering
            && (e.voice->styleIndex != e.renderedStyle || e.voice->getKnobValues() != e.renderedKnobs))
            render (slot);
    }
}

// Pista nueva: arranca con la voz con la que se grabó (la de su entrada).
void VoiceTracks::adopt (int slot)
{
    auto& e = entries[(size_t) slot];
    auto& t = engine.looper.getTrack (slot);
    auto* live = dynamic_cast<VoiceChain*> (engine.getChannelProcessor (t.source));

    e.order = t.order;
    e.voice = std::make_unique<VoiceChain> (live != nullptr ? live->styleIndex : VoiceChain::numStyles - 1);
    if (live != nullptr)
        e.voice->setKnobValues (live->getKnobValues());

    // Lo grabado ya suena con esta voz: no hace falta procesarlo.
    e.renderedStyle = e.voice->styleIndex;
    e.renderedKnobs = e.voice->getKnobValues();
}

void VoiceTracks::render (int slot)
{
    auto& e = entries[(size_t) slot];
    auto& looper = engine.looper;
    juce::AudioBuffer<float> dry;

    if (! looper.copyDry (slot, dry))
        return;

    e.rendering = true;
    engine.logText ("voz de la pista " + juce::String (slot) + ": reprocesando");
    const int order = e.order, style = e.voice->styleIndex;
    const auto knobs = e.voice->getKnobValues();
    const double sr = looper.getSampleRate();
    std::weak_ptr<bool> weak = alive;

    juce::Thread::launch ([this, weak, slot, order, style, knobs, sr, dry]
    {
        constexpr int block = 512;
        VoiceChain voice (style);
        voice.setKnobValues (knobs);
        voice.setRateAndBufferSizeDetails (sr, block);
        voice.prepareToPlay (sr, block);

        const int len = dry.getNumSamples();
        juce::AudioBuffer<float> out (2, len), buffer (2, block);
        juce::MidiBuffer midi;

        // Dos vueltas: la primera carga el eco y la reverb, así lo que suena al final
        // del loop sigue al empezar (como en vivo, donde el loop da vueltas).
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int pos = 0; pos < len; pos += block)
            {
                const int n = juce::jmin (block, len - pos);
                juce::AudioBuffer<float> chunk (buffer.getArrayOfWritePointers(), 2, n);
                chunk.copyFrom (0, 0, dry, 0, pos, n);
                chunk.copyFrom (1, 0, dry, 0, pos, n);
                voice.processBlock (chunk, midi);

                if (pass == 1)
                {
                    out.copyFrom (0, pos, chunk, 0, 0, n);
                    out.copyFrom (1, pos, chunk, 1, 0, n);
                }
            }
        }

        juce::MessageManager::callAsync ([this, weak, slot, order, style, knobs, out]() mutable
        {
            if (weak.lock() != nullptr)
                renderFinished (slot, order, style, knobs, out);
        });
    });
}

void VoiceTracks::renderFinished (int slot, int order, int style, const VoiceChain::KnobValues& knobs,
                                  juce::AudioBuffer<float>& audio)
{
    auto& e = entries[(size_t) slot];
    e.rendering = false;

    // Si cambió el loop o se borró la pista, no se usa.
    if (e.voice != nullptr && e.order == order && engine.looper.replaceAudio (slot, order, audio))
    {
        e.renderedStyle = style;
        e.renderedKnobs = knobs;
    }
}

void VoiceTracks::setStyle (int slot, int style)
{
    auto& e = entries[(size_t) slot];
    if (e.voice == nullptr || e.voice->styleIndex == style)
        return;

    auto* panel = e.window != nullptr ? dynamic_cast<Panel*> (e.window->getContentComponent()) : nullptr;

    if (panel != nullptr)
        panel->clearEditor();

    e.voice = std::make_unique<VoiceChain> (style);   // con las perillas de ese estilo

    if (panel != nullptr)
        panel->setEditor();
}

void VoiceTracks::showEditor (int slot, const juce::String& title)
{
    auto& e = entries[(size_t) slot];
    if (e.voice == nullptr)
        return;

    if (e.window != nullptr)
    {
        e.window->toFront (true);
        return;
    }

    e.window = std::make_unique<VoiceWindow> (title, new Panel (*this, slot), [this, slot]
    {
        juce::MessageManager::callAsync ([this, slot, weak = std::weak_ptr<bool> (alive)]
        {
            if (weak.lock() != nullptr)
                entries[(size_t) slot].window = nullptr;
        });
    });
}
