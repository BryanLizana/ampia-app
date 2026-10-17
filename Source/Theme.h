#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Colores y fuentes de toda la app: modo claro estilo Claude (crema, grises cálidos y
// naranja arcilla). Fuentes incrustadas: Inter (interfaz) y Source Serif 4 (títulos).
namespace theme
{
    const juce::Colour background (0xfffaf9f5),     // fondo crema de las ventanas
                       panel (0xffffffff),          // filas, tarjetas y listas
                       trough (0xffe8e4d9),         // fondo de medidores y barras
                       border (0xffdedad0),
                       control (0xffeeebe3),        // botones normales: relleno que se ve sobre blanco y crema
                       controlEdge (0xffcdc8bb),    // borde y sombra de los botones
                       ledOff (0xffb5b0a3),         // luz de un toggle apagado
                       text (0xff141413), textSecondary (0xff5e5d59), textMuted (0xff9a9893),
                       accent (0xffd97757),         // arcilla de Claude
                       accentDark (0xffc6613f),
                       onAccent (0xffffffff),       // texto sobre arcilla
                       recRed (0xffbf4d43), playGreen (0xff5b8a5f), amber (0xffd4a24c),
                       // Un color por tipo de acción: rojo grabar, verde sonar / escuchar, azul
                       // archivos, morado tempo, arcilla agregar, gris oscuro parar / silenciar.
                       fileBlue (0xff4a7fb5), tempoPurple (0xff7f6aae), stopDark (0xff4a4945),
                       loopBlue (0xff6a9bcc), loopBlueLight (0xffa3c3e3);

    constexpr float radius = 8.0f;   // esquinas de botones y tarjetas

    // Títulos en serif, como los de Claude.
    juce::FontOptions heading (float size);

    // Acción principal: relleno de color con texto blanco (los demás botones son gris cálido con borde).
    void primaryStyle (juce::Button& b, juce::Colour colour = accent);

    // Botón que se prende y apaga: gris apagado, de color prendido. Con "light", además
    // lleva una luz a la izquierda (hueca apagado, llena prendido).
    void toggleStyle (juce::Button& b, juce::Colour on = accent, bool light = true);

    // Botón de menos peso (X, plugin...): como los normales pero con texto gris.
    void secondaryStyle (juce::Button& b);

    // Icono en un TextButton: solo (sin texto) o a la izquierda del texto, del color del texto.
    // "filled": relleno; si no, trazo fino.
    void setIcon (juce::Button& b, const juce::Path& icon, bool filled = false);

    // Iconos dibujados en una caja de 24 x 24 (Inter no trae ↻ ni flechas).
    namespace icons
    {
        juce::Path record(), play(), stop(), undo(), redo(), trash(), folder(), reload(), plus(), minus();
    }

    // Color propio de cada entrada; se repite en las pistas grabadas con ella.
    juce::Colour channelColour (int channel);

    // Medidor: zonas verde / ámbar / rojo, marcas de -12 y -6 dB y una línea de pico que
    // se queda un segundo. "active" en falso: gris (entrada sin OIR, pista en mute).
    struct Meter
    {
        float level = 0.0f, hold = 0.0f;
        double holdUntil = 0.0;

        void push (float peak);
        void paint (juce::Graphics&, juce::Rectangle<float>, bool active = true, const juce::String& label = {}) const;
    };

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();

        juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;
        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                                   bool highlighted, bool down) override;
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
        void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
        void drawComboBox (juce::Graphics&, int width, int height, bool down, int buttonX, int buttonY,
                           int buttonW, int buttonH, juce::ComboBox&) override;

    private:
        juce::Typeface::Ptr sans, sansBold, serif;
    };
}
