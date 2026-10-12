#include "Tuner.h"

namespace
{
    const juce::Colour background (0xff1b1d21), panel (0xff26292f), accent (0xffff8a1f),
                       green (0xff3fb950), red (0xffe0393e);

    const char* const letterNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const char* const solfegeNames[] = { "DO", "DO#", "RE", "RE#", "MI", "FA", "FA#", "SOL", "SOL#", "LA", "LA#", "SI" };

    class TunerComponent : public juce::Component, private juce::Timer
    {
    public:
        TunerComponent (Engine& engineIn, int channelIn) : engine (engineIn), channel (channelIn)
        {
            // Se analiza a ~24 kHz (alcanza para notas de hasta 1.5 kHz y es 4 veces más liviano).
            const double sr = engine.getSampleRate();
            decimation = juce::jmax (1, (int) (sr / 22000.0));
            rate = sr / decimation;
            maxLag = (int) (rate / 40.0);     // nota más grave: 40 Hz (bajo)
            minLag = (int) (rate / 1500.0);
            history.assign ((size_t) (window + maxLag), 0.0f);
            diff.assign ((size_t) maxLag + 2, 0.0f);
            incoming.resize (8192);

            engine.setTunerChannel (channel);
            setSize (440, 260);
            startTimerHz (25);
        }

        ~TunerComponent() override { engine.setTunerChannel (-1); }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (background);
            auto r = getLocalBounds().reduced (16);

            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.setFont (juce::FontOptions (13.0f));
            g.drawText (engine.getChannel (channel).name, r.removeFromTop (18), juce::Justification::centredLeft);

            const bool sounding = note >= 0 && juce::Time::getMillisecondCounter() - lastHeard < 1500;
            const float alpha = sounding && juce::Time::getMillisecondCounter() - lastHeard < 300 ? 1.0f : 0.4f;
            const bool inTune = sounding && std::abs (cents) <= 5.0f;
            const auto colour = ! sounding ? juce::Colours::grey
                              : inTune ? green : std::abs (cents) <= 15.0f ? accent : red;

            // Nota grande.
            auto noteArea = r.removeFromTop (90);
            g.setColour (colour.withAlpha (alpha));
            g.setFont (juce::FontOptions (64.0f, juce::Font::bold));
            g.drawText (sounding ? juce::String (solfegeNames[note % 12]) : juce::String ("--"), noteArea,
                        juce::Justification::centred);

            g.setColour (juce::Colours::white.withAlpha (0.6f));
            g.setFont (juce::FontOptions (14.0f));
            g.drawText (sounding ? juce::String (letterNames[note % 12]) + juce::String (note / 12 - 1)
                                     + "   " + juce::String (frequency, 1) + " Hz"
                                 : juce::String ("Toca una nota (una cuerda sola)"),
                        r.removeFromTop (22), juce::Justification::centred);

            // Barra de -50 a +50 cents con la aguja.
            r.removeFromTop (10);
            auto bar = r.removeFromTop (36).toFloat();
            g.setColour (panel);
            g.fillRoundedRectangle (bar, 5.0f);

            const auto xFor = [&bar] (float c) { return bar.getCentreX() + c / 50.0f * bar.getWidth() * 0.5f; };
            g.setColour (green.withAlpha (0.25f));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xFor (-5.0f), bar.getY(), xFor (5.0f), bar.getBottom()));

            for (int c = -40; c <= 40; c += 10)
            {
                g.setColour (juce::Colours::white.withAlpha (c == 0 ? 0.6f : 0.2f));
                g.fillRect (xFor ((float) c) - 0.5f, bar.getY() + (c == 0 ? 2.0f : 10.0f), c == 0 ? 2.0f : 1.0f,
                            bar.getHeight() - (c == 0 ? 4.0f : 20.0f));
            }

            if (sounding)
            {
                g.setColour (colour.withAlpha (alpha));
                g.fillRoundedRectangle (xFor (juce::jlimit (-50.0f, 50.0f, cents)) - 3.0f, bar.getY() - 4.0f, 6.0f,
                                        bar.getHeight() + 8.0f, 3.0f);
            }

            r.removeFromTop (8);
            g.setColour (colour.withAlpha (alpha));
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText (! sounding ? juce::String()
                        : inTune ? juce::String ("AFINADO")
                        : cents < 0.0f ? "BAJO " + juce::String (juce::roundToInt (-cents)) + " cents: sube (aprieta)"
                                       : "ALTO " + juce::String (juce::roundToInt (cents)) + " cents: baja (afloja)",
                        r.removeFromTop (24), juce::Justification::centred);
        }

    private:
        void timerCallback() override
        {
            // Lo que llegó desde la última vez, promediado de a "decimation" muestras.
            int got;
            while ((got = engine.readTuner (incoming.data(), (int) incoming.size())) > 0)
            {
                for (int i = 0; i < got; ++i)
                {
                    sum += incoming[(size_t) i];
                    if (++summed == decimation)
                    {
                        history[(size_t) writePos] = sum / (float) decimation;
                        writePos = (writePos + 1) % (int) history.size();
                        sum = 0.0f;
                        summed = 0;
                    }
                }
            }

            const float hz = detect();

            if (hz > 0.0f)
            {
                const float midi = 69.0f + 12.0f * std::log2 (hz / 440.0f);
                const int nearest = juce::roundToInt (midi);
                const float c = (midi - (float) nearest) * 100.0f;

                // Suaviza la aguja mientras sea la misma nota.
                cents = nearest == note ? cents * 0.6f + c * 0.4f : c;
                frequency = nearest == note ? frequency * 0.6f + hz * 0.4f : hz;
                note = nearest;
                lastHeard = juce::Time::getMillisecondCounter();
            }

            repaint();
        }

        // YIN (de Cheveigné y Kawahara, 2002): 0 si no hay una nota clara.
        float detect()
        {
            const int size = (int) history.size();
            std::vector<float>& x = ordered;
            x.resize ((size_t) size);
            for (int i = 0; i < size; ++i)
                x[(size_t) i] = history[(size_t) ((writePos + i) % size)];

            // Muy bajo: no hay nada sonando.
            float energy = 0.0f;
            for (int i = size - window; i < size; ++i)
                energy += x[(size_t) i] * x[(size_t) i];

            if (std::sqrt (energy / (float) window) < 0.003f)
                return 0.0f;

            const float* s = x.data();
            for (int lag = 1; lag <= maxLag; ++lag)
            {
                float d = 0.0f;
                for (int j = 0; j < window; ++j)
                {
                    const float v = s[j] - s[j + lag];
                    d += v * v;
                }
                diff[(size_t) lag] = d;
            }

            // Diferencia normalizada: el primer mínimo bajo el umbral es el período.
            float running = 0.0f;
            diff[0] = 1.0f;
            for (int lag = 1; lag <= maxLag; ++lag)
            {
                running += diff[(size_t) lag];
                diff[(size_t) lag] = running > 0.0f ? diff[(size_t) lag] * (float) lag / running : 1.0f;
            }

            int lag = minLag;
            while (lag < maxLag && diff[(size_t) lag] > 0.15f)
                ++lag;

            if (lag >= maxLag)
                return 0.0f;

            while (lag + 1 < maxLag && diff[(size_t) lag + 1] < diff[(size_t) lag])
                ++lag;

            // Interpolación parabólica para afinar el período entre muestras.
            const float a = diff[(size_t) lag - 1], b = diff[(size_t) lag], c = diff[(size_t) lag + 1];
            const float denom = a - 2.0f * b + c;
            const float shift = std::abs (denom) > 1.0e-9f ? 0.5f * (a - c) / denom : 0.0f;

            return (float) (rate / ((float) lag + shift));
        }

        Engine& engine;
        const int channel;
        static constexpr int window = 1024;
        int decimation = 2, minLag = 16, maxLag = 600;
        double rate = 24000.0;
        std::vector<float> history, ordered, diff, incoming;
        int writePos = 0, summed = 0;
        float sum = 0.0f;

        int note = -1;
        float cents = 0.0f, frequency = 0.0f;
        juce::uint32 lastHeard = 0;
    };
}

TunerWindow::TunerWindow (Engine& engine, int channelIn, std::function<void()> onCloseIn)
    : DocumentWindow ("Afinador - " + engine.getChannel (channelIn).name, background, closeButton),
      channel (channelIn), onClose (std::move (onCloseIn))
{
    setUsingNativeTitleBar (true);
    setContentOwned (new TunerComponent (engine, channel), true);
    centreWithSize (getWidth(), getHeight());
    setAlwaysOnTop (true);   // que se vea mientras tocas con la ventana principal
    setVisible (true);
}
