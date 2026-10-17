#include "Tuner.h"
#include "Theme.h"

namespace
{
    using theme::background, theme::panel, theme::accent, theme::text;
    const juce::Colour green (theme::playGreen), red (theme::recRed);

    const char* const letterNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const char* const solfegeNames[] = { "DO", "DO#", "RE", "RE#", "MI", "FA", "FA#", "SOL", "SOL#", "LA", "LA#", "SI" };

    // Estilo Vocal Pitch Monitor: las notas en vertical (como un piano) y la altura que
    // suena dibujada en el tiempo, de derecha (ahora) a izquierda (hace unos segundos).
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
            trace.assign ((size_t) traceLength, silence);

            engine.setTunerChannel (channel);
            setSize (760, 460);
            startTimerHz (ticksPerSecond);
        }

        ~TunerComponent() override { engine.setTunerChannel (-1); }

        // Clic: congela / sigue. Rueda: más o menos notas a la vista.
        void mouseDown (const juce::MouseEvent&) override { paused = ! paused; repaint(); }

        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
        {
            visibleNotes = juce::jlimit (6.0f, 48.0f, visibleNotes * (1.0f - wheel.deltaY * 0.6f));
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (background);
            auto r = getLocalBounds();

            const bool sounding = note >= 0 && juce::Time::getMillisecondCounter() - lastHeard < 400;
            const bool inTune = sounding && std::abs (cents) <= 5.0f;
            const auto colour = ! sounding ? theme::textMuted
                              : inTune ? green : std::abs (cents) <= 15.0f ? accent : red;

            // Arriba: nota, cents y frecuencia.
            auto header = r.removeFromTop (54).reduced (14, 6);
            g.setColour (colour);
            g.setFont (theme::heading (36.0f));
            g.drawText (sounding ? noteName (note) : juce::String ("--"), header.removeFromLeft (150),
                        juce::Justification::centredLeft);

            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            g.drawText (! sounding ? juce::String()
                        : inTune ? juce::String ("AFINADO")
                        : cents < 0.0f ? "BAJO " + juce::String (juce::roundToInt (-cents)) + " cents"
                                       : "ALTO " + juce::String (juce::roundToInt (cents)) + " cents",
                        header.removeFromLeft (170), juce::Justification::centredLeft);

            g.setColour (theme::textSecondary);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText (paused ? juce::String ("PAUSA (clic para seguir)")
                        : sounding ? juce::String (frequency, 1) + " Hz   " + juce::String (letterNames[note % 12])
                                         + juce::String (note / 12 - 1)
                                   : engine.getChannel (channel).name + "  -  toca o canta una nota",
                        header, juce::Justification::centredRight);

            // Teclas a la izquierda, gráfico a la derecha.
            auto keys = r.removeFromLeft (64);
            auto plot = r;
            const float rowH = (float) plot.getHeight() / visibleNotes;
            const float low = viewCentre - visibleNotes * 0.5f;
            const auto yFor = [&] (float midi) { return (float) plot.getBottom() - (midi - low + 0.5f) * rowH; };

            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (keys.getUnion (plot));

            for (int n = (int) std::floor (low) - 1; n <= (int) std::ceil (low + visibleNotes) + 1; ++n)
            {
                const float top = yFor ((float) n + 0.5f), bottom = yFor ((float) n - 0.5f);
                const bool black = isBlack (((n % 12) + 12) % 12);
                const auto row = [&] (juce::Rectangle<int> area)
                    { return juce::Rectangle<float>::leftTopRightBottom ((float) area.getX(), top, (float) area.getRight(), bottom); };

                g.setColour (black ? juce::Colour (0xfff2f2f2) : panel);
                g.fillRect (row (plot));
                g.setColour (black ? juce::Colour (0xff3a3a3a) : panel);
                g.fillRect (row (keys).withTrimmedRight (black ? 22.0f : 0.0f));

                if (sounding && n == note)
                {
                    g.setColour (colour.withAlpha (0.18f));
                    g.fillRect (row (plot));
                    g.setColour (colour);
                    g.fillRect (row (keys));
                }

                // Línea entre notas; más marcada entre SI y DO (cambio de octava).
                g.setColour (text.withAlpha (n % 12 == 0 ? 0.25f : 0.08f));
                g.fillRect ((float) keys.getX(), bottom - 0.5f, (float) (plot.getRight() - keys.getX()), 1.0f);

                if (! black && (rowH >= 11.0f || n % 12 == 0))
                {
                    g.setColour (sounding && n == note ? theme::onAccent : theme::textSecondary);
                    g.setFont (juce::FontOptions (juce::jlimit (9.0f, 13.0f, rowH * 0.8f),
                                                  n % 12 == 0 ? juce::Font::bold : juce::Font::plain));
                    g.drawText (noteName (n), row (keys).reduced (4.0f, 0.0f), juce::Justification::centredRight);
                }
            }

            // La línea de lo que sonó: cada punto es un instante; se corta en los silencios
            // y en los saltos de nota.
            const float step = (float) plot.getWidth() / (float) (traceLength - 1);
            juce::Path line;
            bool drawing = false;
            float previous = silence;

            for (int i = 0; i < traceLength; ++i)
            {
                const float m = trace[(size_t) i];
                const float x = (float) plot.getX() + (float) i * step, y = yFor (m);

                if (m == silence)
                    drawing = false;
                else if (drawing && std::abs (m - previous) < 1.0f)
                    line.lineTo (x, y);
                else
                {
                    line.startNewSubPath (x, y);
                    line.lineTo (x + 0.5f, y);
                    drawing = true;
                }

                previous = m;
            }

            g.setColour (theme::loopBlue);
            g.strokePath (line, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            // El punto de ahora, con el color de la afinación.
            if (sounding && trace.back() != silence)
            {
                g.setColour (colour);
                g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre ({ (float) plot.getRight() - 6.0f, yFor (trace.back()) }));
            }
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

            if (paused)
                return;

            const float hz = detect();
            float point = silence;

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

                // Un salto suelto de más de media octava (un armónico) no se dibuja: se espera a que se repita.
                point = midi;
                if (lastPoint != silence && std::abs (midi - lastPoint) > 6.0f && ! confirm)
                {
                    confirm = true;
                    point = silence;
                }
                else
                {
                    confirm = false;
                    lastPoint = midi;
                }
            }

            trace.erase (trace.begin());
            trace.push_back (point);

            // La vista sigue a la nota de a poco, sin saltar.
            if (point != silence && std::abs (point - viewCentre) > visibleNotes * 0.3f)
                followTarget = point;
            viewCentre += (followTarget - viewCentre) * 0.12f;

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

        static juce::String noteName (int midi)
        {
            return juce::String (solfegeNames[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
        }

        static bool isBlack (int pitchClass)
        {
            return pitchClass == 1 || pitchClass == 3 || pitchClass == 6 || pitchClass == 8 || pitchClass == 10;
        }

        static constexpr int ticksPerSecond = 50, traceLength = ticksPerSecond * 8;   // 8 segundos a la vista
        static constexpr float silence = -1000.0f;
        std::vector<float> trace;
        float visibleNotes = 18.0f, viewCentre = 52.0f, followTarget = 52.0f;   // MI3: la cuerda grave al aire
        float lastPoint = silence;
        bool confirm = false, paused = false;

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
    setResizable (true, false);
    setResizeLimits (480, 300, 2000, 1400);
    centreWithSize (getWidth(), getHeight());
    setAlwaysOnTop (true);   // que se vea mientras tocas con la ventana principal
    setVisible (true);
}
