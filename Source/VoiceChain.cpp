#include "VoiceChain.h"
#include "Theme.h"

// Estilos. La reverb y el delay vienen bajos para cantar en vivo (con parlantes se acumulan).
const VoiceChain::Style VoiceChain::styles[VoiceChain::numStyles] =
{
    // Chester Bennington: brillante, muy comprimido, saturado, poco espacio.
    { "Chester", "Voz Chester (estilo Linkin Park)",
      "EQ brillante > Compresor 6:1 > Compresor 2:1 > Saturador > Delay 1/8 > Reverb corta",
      90.0, 100.0, -4.0, 2000.0, 5.0, 0.7, 8000.0, 3.0,
      -30.0f, 6.0f, 2.0f, 60.0f,   -12.0f, 2.0f, 10.0f, 100.0f,
      10.0f, -4.0f,
      0.5, 0.145f, 2000.0f,
      0.45f, 0.6f, 20.0f,
      40.0f, 8.0f, 8.0f },

    // Kurt Cobain (Nirvana): seca y cruda, medios al frente, compresión fuerte y algo de grano.
    { "Cobain", "Voz Cobain (Nirvana: seca y cruda)",
      "EQ medios > Compresor 4:1 > Compresor 3:1 > Saturador fuerte > Slap corto > Sala chica",
      110.0, 160.0, -2.0, 1500.0, 4.0, 0.8, 9000.0, 1.0,
      -24.0f, 4.0f, 3.0f, 80.0f,   -10.0f, 3.0f, 5.0f, 60.0f,
      14.0f, -6.0f,
      0.25, 0.05f, 2500.0f,
      0.3f, 0.7f, 5.0f,
      45.0f, 5.0f, 6.0f },

    // Natural y crudo para vivo: casi sin color, compresión que no se nota, un poco de grano,
    // slap corto en vez de reverb larga (no se acumula con los parlantes ni ensucia la letra).
    { "Natural", "Voz Natural (cruda, para vivo)",
      "EQ limpia (quita barro) > Compresor 3:1 suave > Limitador 2:1 > Grano leve > Slap corto > Sala chica",
      100.0, 120.0, -1.0, 350.0, -2.5, 1.0, 9000.0, 2.0,
      -18.0f, 3.0f, 10.0f, 100.0f,   -8.0f, 2.0f, 3.0f, 80.0f,
      6.0f, -2.0f,
      0.25, 0.08f, 2500.0f,
      0.3f, 0.6f, 10.0f,
      20.0f, 6.0f, 6.0f },
};

std::atomic<float> VoiceChain::bpm { 100.0f };

VoiceChain::VoiceChain (int index)
    : AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                            .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
      styleIndex (juce::jlimit (0, numStyles - 1, index)),
      style (styles[styleIndex])
{
    for (auto [knob, value] : { std::pair { (int) satMix, style.satDefault },
                                std::pair { (int) delayMix, style.delayDefault },
                                std::pair { (int) reverbMix, style.reverbDefault } })
    {
        knobs[(size_t) knob].defaultValue = value;
        knobs[(size_t) knob].value = value;
    }
}

// La Chester conserva su archivo de antes ("Voz Chester") para las entradas ya guardadas.
juce::File VoiceChain::virtualFile (int index)
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("AmpBox").getChildFile (juce::String ("Voz ") + styles[juce::jlimit (0, numStyles - 1, index)].name);
}

int VoiceChain::styleForFile (const juce::File& file)
{
    for (int i = 0; i < numStyles; ++i)
        if (file == virtualFile (i))
            return i;

    // La Frusciante se quitó: las entradas que la tenían pasan a la Natural.
    if (file == virtualFile (0).getSiblingFile ("Voz Frusciante"))
        return 2;

    return -1;
}

void VoiceChain::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = getName();
    d.descriptiveName = juce::CharPointer_UTF8 (style.menuText);
    d.pluginFormatName = "AmpBox";
    d.category = "Fx";
    d.manufacturerName = "AmpBox";
    d.version = "1.0";
    d.fileOrIdentifier = virtualFile (styleIndex).getFullPathName();
    d.isInstrument = false;
    d.numInputChannels = 2;
    d.numOutputChannels = 2;
}

void VoiceChain::Compressor::setTimes (double sampleRate, double attackMs, double releaseMs)
{
    attack = (float) std::exp (-1.0 / (attackMs * 0.001 * sampleRate));
    release = (float) std::exp (-1.0 / (releaseMs * 0.001 * sampleRate));
}

// Ganancia (lineal) para este nivel de entrada, con rodilla suave y makeup.
float VoiceChain::Compressor::gainFor (float level)
{
    env = level > env ? attack * env + (1.0f - attack) * level
                      : release * env + (1.0f - release) * level;

    const float over = juce::Decibels::gainToDecibels (env, -120.0f) - thresholdDb;
    const float slope = 1.0f - 1.0f / ratio;
    float reduction = 0.0f;

    if (over >= kneeDb * 0.5f)
        reduction = slope * over;
    else if (over > -kneeDb * 0.5f)
        reduction = slope * juce::square (over + kneeDb * 0.5f) / (2.0f * kneeDb);

    return juce::Decibels::decibelsToGain (makeupDb - reduction);
}

VoiceChain::KnobValues VoiceChain::getKnobValues() const
{
    KnobValues values {};
    for (size_t i = 0; i < knobs.size(); ++i)
        values[i] = knobs[i].value.load();

    return values;
}

void VoiceChain::setKnobValues (const KnobValues& values)
{
    for (size_t i = 0; i < knobs.size(); ++i)
        knobs[i].value = juce::jlimit (knobs[i].min, knobs[i].max, values[i]);
}

void VoiceChain::updateFilters()
{
    using C = juce::IIRCoefficients;

    // Cuerpo y brillo suben o bajan los estantes de graves y agudos del estilo.
    appliedBody = get (body);
    appliedBright = get (bright);

    for (int ch = 0; ch < 2; ++ch)
    {
        const auto db = [] (double v) { return (float) juce::Decibels::decibelsToGain (v); };
        highpass[(size_t) ch].setCoefficients (C::makeHighPass (sr, style.highpassHz));
        lowShelf[(size_t) ch].setCoefficients (C::makeLowShelf (sr, style.lowShelfHz, 0.7, db (style.lowShelfDb + appliedBody)));
        mid[(size_t) ch].setCoefficients (C::makePeakFilter (sr, style.midHz, style.midQ, db (style.midDb)));
        highShelf[(size_t) ch].setCoefficients (C::makeHighShelf (sr, style.highShelfHz, 0.7, db (style.highShelfDb + appliedBright)));
        delayFilter[(size_t) ch].setCoefficients (C::makeBandPass (sr, style.delayFilterHz, 0.35));
    }
}

void VoiceChain::prepareToPlay (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;
    updateFilters();

    for (auto* bank : { &highpass, &lowShelf, &mid, &highShelf, &delayFilter })
        for (auto& f : *bank)
            f.reset();

    comp1.setTimes (sr, style.comp1AttackMs, style.comp1ReleaseMs);
    comp2.setTimes (sr, style.comp2AttackMs, style.comp2ReleaseMs);
    comp1.env = comp2.env = 0.0f;

    gateOpen = 1.0f - (float) std::exp (-1.0 / (0.001 * sr));    // abre en ~1 ms (no se come el ataque)
    gateClose = 1.0f - (float) std::exp (-1.0 / (0.08 * sr));    // cierra en ~80 ms (sin cortes bruscos)
    gateFollow = (float) std::exp (-1.0 / (0.01 * sr));
    gateHoldLen = (int) (0.06 * sr);                             // espera 60 ms antes de cerrar (entre palabras)
    gateEnv = 0.0f;
    gateGain = 1.0f;
    gateHold = 0;

    delayLine.setSize (2, (int) (2.5 * sr));
    delayLine.clear();
    preDelayLine.setSize (2, (int) (style.preDelayMs * 0.001 * sr) + 1);
    preDelayLine.clear();
    delayPos = preDelayPos = 0;
    wet.setSize (2, maxBlockSize);

    reverb.setSampleRate (sr);
    juce::Reverb::Parameters p;
    p.roomSize = style.roomSize;
    p.damping = style.damping;
    p.wetLevel = 1.0f / 3.0f;   // juce::Reverb multiplica lo húmedo por 3: así la perilla es el % real
    p.dryLevel = 0.0f;
    p.width = 1.0f;
    reverb.setParameters (p);
    reverb.reset();
}

// 0 % = apagada; 100 % = umbral en -30 dB. Cerrada baja ~30 dB (no silencio total:
// suena más natural). Mientras cantas el ruido sigue, pero la voz lo tapa.
void VoiceChain::processGate (float* L, float* R, int n)
{
    const float amount = get (gate) / 100.0f;
    if (amount <= 0.0f)
    {
        gateGain = 1.0f;
        return;
    }

    const float threshold = juce::Decibels::decibelsToGain (-70.0f + 40.0f * amount);
    constexpr float floorGain = 0.03f;

    for (int i = 0; i < n; ++i)
    {
        const float level = juce::jmax (std::abs (L[i]), std::abs (R[i]));
        gateEnv = level > gateEnv ? level : gateEnv * gateFollow;

        float target = floorGain;
        if (gateEnv > threshold)
        {
            gateHold = gateHoldLen;
            target = 1.0f;
        }
        else if (gateHold > 0)
        {
            --gateHold;
            target = 1.0f;
        }

        gateGain += (target - gateGain) * (target > gateGain ? gateOpen : gateClose);
        L[i] *= gateGain;
        R[i] *= gateGain;
    }
}

void VoiceChain::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2 || n > wet.getNumSamples())
        return;

    auto* L = buffer.getWritePointer (0);
    auto* R = buffer.getWritePointer (1);

    buffer.applyGain (juce::Decibels::decibelsToGain (get (inGain)));

    if (get (body) != appliedBody || get (bright) != appliedBright)
        updateFilters();

    // 0) Anti-ruido (antes de comprimir: la compresión subiría el ruido).
    processGate (L, R, n);

    // 1) Channel EQ
    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        highpass[(size_t) ch].processSamples (d, n);
        lowShelf[(size_t) ch].processSamples (d, n);
        mid[(size_t) ch].processSamples (d, n);
        highShelf[(size_t) ch].processSamples (d, n);
    }

    // 2-3) Compresores. "Compresión" escala el umbral (100 % = el rack).
    const float k = get (compAmount) / 100.0f;
    comp1.thresholdDb = style.comp1Threshold * k;
    comp1.ratio = 1.0f + (style.comp1Ratio - 1.0f) * juce::jmin (1.0f, k);
    comp2.thresholdDb = style.comp2Threshold * k;
    comp2.ratio = 1.0f + (style.comp2Ratio - 1.0f) * juce::jmin (1.0f, k);

    // Ganancia de compensación automática (como "Makeup" de Ableton, aprox.).
    for (auto* c : { &comp1, &comp2 })
        c->makeupDb = 0.5f * -c->thresholdDb * (1.0f - 1.0f / c->ratio);

    // 4) Saturador (Analog Clip, tanh) con mezcla.
    const float drive = juce::Decibels::decibelsToGain (style.satDriveDb);
    const float satOut = juce::Decibels::decibelsToGain (style.satOutDb);
    const float sat = get (satMix) / 100.0f;

    for (int i = 0; i < n; ++i)
    {
        float l = L[i], r = R[i];

        float g = comp1.gainFor (juce::jmax (std::abs (l), std::abs (r)));
        l *= g; r *= g;
        g = comp2.gainFor (juce::jmax (std::abs (l), std::abs (r)));
        l *= g; r *= g;

        L[i] = l + sat * (std::tanh (l * drive) * satOut - l);
        R[i] = r + sat * (std::tanh (r * drive) * satOut - r);
    }

    // 5) Delay al tempo del looper, con filtro en lo repetido.
    const int delayLen = delayLine.getNumSamples();
    const int delaySamples = juce::jlimit (1, delayLen - 1, (int) (sr * 60.0 / juce::jmax (40.0f, bpm.load()) * style.delayBeats));
    const float dMix = get (delayMix) / 100.0f;

    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        auto* line = delayLine.getWritePointer (ch);
        auto* w = wet.getWritePointer (ch);
        int pos = delayPos;

        for (int i = 0; i < n; ++i)
        {
            const int readPos = (pos - delaySamples + delayLen) % delayLen;
            w[i] = line[readPos];
            line[pos] = d[i] + style.delayFeedback * w[i];
            pos = (pos + 1) % delayLen;
        }

        delayFilter[(size_t) ch].processSamples (w, n);
        juce::FloatVectorOperations::addWithMultiply (d, w, dMix, n);
    }

    delayPos = (delayPos + n) % delayLen;

    // 6) Reverb con predelay.
    const int preLen = preDelayLine.getNumSamples();

    for (int ch = 0; ch < 2; ++ch)
    {
        auto* d = buffer.getReadPointer (ch);
        auto* line = preDelayLine.getWritePointer (ch);
        auto* w = wet.getWritePointer (ch);
        int pos = preDelayPos;

        for (int i = 0; i < n; ++i)
        {
            w[i] = line[pos];
            line[pos] = d[i];
            pos = (pos + 1) % preLen;
        }
    }

    preDelayPos = (preDelayPos + n) % preLen;
    reverb.processStereo (wet.getWritePointer (0), wet.getWritePointer (1), n);

    const float rMix = get (reverbMix) / 100.0f;
    buffer.addFrom (0, 0, wet, 0, 0, n, rMix);
    buffer.addFrom (1, 0, wet, 1, 0, n, rMix);

    buffer.applyGain (juce::Decibels::decibelsToGain (get (outGain)));
}

void VoiceChain::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("VOZCHESTER");
    for (auto& k : knobs)
        xml.setAttribute (k.id, k.value.load());

    copyXmlToBinary (xml, dest);
}

void VoiceChain::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        for (auto& k : knobs)
            k.value = juce::jlimit (k.min, k.max, (float) xml->getDoubleAttribute (k.id, k.defaultValue));
}

//==============================================================================
// Ventana: nueve perillas (con qué hace cada una) y "Valores del estilo" para volver
// a los de fábrica.

namespace
{
    class VoiceChainEditor : public juce::AudioProcessorEditor
    {
    public:
        explicit VoiceChainEditor (VoiceChain& v) : AudioProcessorEditor (v), voice (v)
        {
            for (size_t i = 0; i < voice.knobs.size(); ++i)
            {
                auto& k = voice.knobs[i];
                auto* s = sliders.add (new juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow));
                s->setRange (k.min, k.max, 0.1);
                s->setTextValueSuffix (k.suffix);
                s->setValue (k.value.load(), juce::dontSendNotification);
                s->setDoubleClickReturnValue (true, k.defaultValue);
                s->onValueChange = [this, i, s] { voice.knobs[i].value = (float) s->getValue(); };
                addAndMakeVisible (s);

                auto* l = labels.add (new juce::Label ({}, k.name));
                l->setJustificationType (juce::Justification::centred);
                l->setFont (juce::FontOptions (14.0f, juce::Font::bold));
                addAndMakeVisible (l);

                auto* help = helps.add (new juce::Label ({}, k.help));
                help->setJustificationType (juce::Justification::centredTop);
                help->setFont (juce::FontOptions (11.0f));
                help->setColour (juce::Label::textColourId, theme::textSecondary);
                help->setMinimumHorizontalScale (1.0f);
                addAndMakeVisible (help);
            }

            resetButton.onClick = [this]
            {
                for (size_t i = 0; i < voice.knobs.size(); ++i)
                    sliders[(int) i]->setValue (voice.knobs[i].defaultValue);
            };
            addAndMakeVisible (resetButton);

            setSize (700, 400);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (theme::background);
            g.setColour (theme::accent);
            g.setFont (theme::heading (20.0f));
            g.drawText ("VOZ " + juce::String (voice.style.name).toUpperCase(), 16, 10, 300, 24,
                        juce::Justification::centredLeft);
            g.setColour (theme::textSecondary);
            g.setFont (juce::FontOptions (11.5f));
            g.drawText (juce::String (juce::CharPointer_UTF8 (voice.style.chainText)),
                        16, 34, getWidth() - 32, 18, juce::Justification::centredLeft);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (16);
            r.removeFromTop (44);
            resetButton.setBounds (r.removeFromBottom (28).removeFromRight (160));
            r.removeFromBottom (8);

            const int perRow = (sliders.size() + 1) / 2, w = r.getWidth() / perRow, h = r.getHeight() / 2;

            for (int i = 0; i < sliders.size(); ++i)
            {
                auto cell = juce::Rectangle<int> (r.getX() + (i % perRow) * w, r.getY() + (i / perRow) * h, w, h).reduced (4);
                labels[i]->setBounds (cell.removeFromTop (18));
                helps[i]->setBounds (cell.removeFromBottom (30));
                sliders[i]->setBounds (cell);
            }
        }

    private:
        VoiceChain& voice;
        juce::OwnedArray<juce::Slider> sliders;
        juce::OwnedArray<juce::Label> labels, helps;
        juce::TextButton resetButton { "Valores del estilo" };
    };
}

juce::AudioProcessorEditor* VoiceChain::createEditor()
{
    return new VoiceChainEditor (*this);
}
