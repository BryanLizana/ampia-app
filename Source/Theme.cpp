#include "Theme.h"
#include "BinaryData.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace theme
{
    juce::FontOptions heading (float size)
    {
        return juce::FontOptions (juce::Font::getDefaultSerifFontName(), size, juce::Font::plain);
    }

    void primaryStyle (juce::Button& b, juce::Colour colour)
    {
        b.setColour (juce::TextButton::buttonColourId, colour);
        b.setColour (juce::TextButton::textColourOffId, onAccent);
    }

    void toggleStyle (juce::Button& b, juce::Colour on, bool light)
    {
        b.setColour (juce::TextButton::buttonOnColourId, on);
        b.setColour (juce::TextButton::textColourOnId, onAccent);
        b.getProperties().set ("light", light);
    }

    void secondaryStyle (juce::Button& b)
    {
        b.setColour (juce::TextButton::textColourOffId, textSecondary);
    }

    void setIcon (juce::Button& b, const juce::Path& icon, bool filled)
    {
        const auto encoded = icon.toString();
        if (b.getProperties()["icon"].toString() == encoded)   // se llama en cada refresco: solo si cambia
            return;

        b.getProperties().set ("icon", encoded);
        b.getProperties().set ("iconFilled", filled);
        b.repaint();
    }

    namespace icons
    {
        juce::Path record()  { juce::Path p; p.addEllipse (6.0f, 6.0f, 12.0f, 12.0f); return p; }
        juce::Path stop()    { juce::Path p; p.addRoundedRectangle (6.5f, 6.5f, 11.0f, 11.0f, 2.0f); return p; }
        juce::Path play()    { juce::Path p; p.addTriangle (8.0f, 5.5f, 19.0f, 12.0f, 8.0f, 18.5f); return p; }
        juce::Path minus()   { juce::Path p; p.startNewSubPath (6.0f, 12.0f); p.lineTo (18.0f, 12.0f); return p; }

        juce::Path plus()
        {
            auto p = minus();
            p.startNewSubPath (12.0f, 6.0f);
            p.lineTo (12.0f, 18.0f);
            return p;
        }

        // Flecha en U hacia la izquierda.
        juce::Path undo()
        {
            juce::Path p;
            p.startNewSubPath (18.0f, 19.0f);
            p.lineTo (18.0f, 14.0f);
            p.quadraticTo (18.0f, 9.0f, 13.0f, 9.0f);
            p.lineTo (6.0f, 9.0f);
            p.startNewSubPath (10.0f, 5.0f);
            p.lineTo (6.0f, 9.0f);
            p.lineTo (10.0f, 13.0f);
            return p;
        }

        juce::Path redo()
        {
            auto p = undo();
            p.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f).translated (24.0f, 0.0f));
            return p;
        }

        juce::Path trash()
        {
            juce::Path p;
            p.startNewSubPath (4.5f, 7.0f);
            p.lineTo (19.5f, 7.0f);
            p.startNewSubPath (9.5f, 7.0f);
            p.lineTo (9.5f, 4.5f);
            p.lineTo (14.5f, 4.5f);
            p.lineTo (14.5f, 7.0f);
            p.startNewSubPath (6.5f, 7.0f);
            p.lineTo (7.5f, 19.5f);
            p.lineTo (16.5f, 19.5f);
            p.lineTo (17.5f, 7.0f);
            p.startNewSubPath (10.5f, 10.5f);
            p.lineTo (10.5f, 16.0f);
            p.startNewSubPath (13.5f, 10.5f);
            p.lineTo (13.5f, 16.0f);
            return p;
        }

        juce::Path folder()
        {
            juce::Path p;
            p.startNewSubPath (3.5f, 6.0f);
            p.lineTo (9.5f, 6.0f);
            p.lineTo (11.5f, 8.5f);
            p.lineTo (20.5f, 8.5f);
            p.lineTo (20.5f, 18.5f);
            p.lineTo (3.5f, 18.5f);
            p.closeSubPath();
            return p;
        }

        // Flecha circular (recargar): arco de casi una vuelta que termina arriba.
        juce::Path reload()
        {
            juce::Path p;
            p.addCentredArc (12.0f, 12.5f, 7.0f, 7.0f, 0.0f, 0.7f, juce::MathConstants<float>::twoPi, true);
            p.startNewSubPath (9.0f, 2.5f);
            p.lineTo (12.0f, 5.5f);
            p.lineTo (9.0f, 8.5f);
            return p;
        }
    }

    juce::Colour channelColour (int channel)
    {
        static const juce::Colour colours[] = { juce::Colour (0xffd97757), juce::Colour (0xff6a9bcc),
                                                juce::Colour (0xff788c5d), juce::Colour (0xff8f7bb8),
                                                juce::Colour (0xffc9a04b), juce::Colour (0xff4e9a97),
                                                juce::Colour (0xffc46e8a), juce::Colour (0xff9c7356) };
        return colours[(size_t) juce::jmax (0, channel) % std::size (colours)];
    }

    void Meter::push (float peak)
    {
        level = juce::jmax (peak, level * 0.85f);
        const double now = juce::Time::getMillisecondCounterHiRes();

        if (level >= hold)
        {
            hold = level;
            holdUntil = now + 1000.0;
        }
        else if (now > holdUntil)
        {
            hold = juce::jmax (level, hold * 0.9f);
        }
    }

    void Meter::paint (juce::Graphics& g, juce::Rectangle<float> r, bool active, const juce::String& label) const
    {
        const auto xFor = [&] (float gain)
        {
            return r.getX() + r.getWidth() * juce::jmap (juce::Decibels::gainToDecibels (gain, -60.0f), -60.0f, 0.0f, 0.0f, 1.0f);
        };
        const auto xForDb = [&] (float db) { return xFor (juce::Decibels::decibelsToGain (db)); };
        const auto zoneColour = [&] (float x) { return ! active ? textMuted : x >= xForDb (-3.0f) ? recRed
                                                                  : x >= xForDb (-12.0f) ? amber : playGreen; };

        g.setColour (trough);
        g.fillRoundedRectangle (r, 4.0f);

        {
            juce::Graphics::ScopedSaveState clip (g);
            juce::Path shape;
            shape.addRoundedRectangle (r, 4.0f);
            g.reduceClipRegion (shape);
            g.reduceClipRegion (r.withRight (xFor (level)).toNearestInt());

            const float amberX = xForDb (-12.0f), redX = xForDb (-3.0f);
            g.setColour (zoneColour (r.getX()));
            g.fillRect (r.withRight (amberX));
            g.setColour (zoneColour (amberX));
            g.fillRect (r.withLeft (amberX).withRight (redX));
            g.setColour (zoneColour (redX));
            g.fillRect (r.withLeft (redX));
        }

        g.setColour (text.withAlpha (0.2f));
        for (float db : { -12.0f, -6.0f })
            g.fillRect (xForDb (db), r.getY() + 2.0f, 1.0f, r.getHeight() - 4.0f);

        if (hold > 0.001f)
        {
            const float x = juce::jmin (xFor (hold), r.getRight() - 2.0f);
            g.setColour (zoneColour (x).darker (0.3f));
            g.fillRect (x, r.getY() + 1.0f, 2.0f, r.getHeight() - 2.0f);
        }

        g.setColour (controlEdge);
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);

        if (label.isNotEmpty())
        {
            g.setColour (text);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (label, r.reduced (7.0f, 0.0f), juce::Justification::centredLeft);
        }
    }

    LookAndFeel::LookAndFeel()
    {
        sans = juce::Typeface::createSystemTypefaceFor (BinaryData::Inter400_ttf, BinaryData::Inter400_ttfSize);
        sansBold = juce::Typeface::createSystemTypefaceFor (BinaryData::Inter600_ttf, BinaryData::Inter600_ttfSize);
        serif = juce::Typeface::createSystemTypefaceFor (BinaryData::SourceSerif4600_ttf, BinaryData::SourceSerif4600_ttfSize);

        setColourScheme ({ background, panel, panel, border, text, accent, onAccent, accent, text });

        const std::pair<int, juce::Colour> defaults[] =
        {
            { juce::ResizableWindow::backgroundColourId,       background },
            { juce::DocumentWindow::textColourId,              text },

            // Botones: gris cálido con borde; los principales y los prendidos, en arcilla.
            { juce::TextButton::buttonColourId,                control },
            { juce::TextButton::buttonOnColourId,              accent },
            { juce::TextButton::textColourOffId,               text },
            { juce::TextButton::textColourOnId,                onAccent },
            { juce::ToggleButton::textColourId,                text },
            { juce::ToggleButton::tickColourId,                accent },
            { juce::ToggleButton::tickDisabledColourId,        textMuted },
            { juce::HyperlinkButton::textColourId,             accent },

            { juce::Label::textColourId,                       text },
            { juce::Label::textWhenEditingColourId,            text },
            { juce::Label::outlineWhenEditingColourId,         accent },

            { juce::TextEditor::backgroundColourId,            panel },
            { juce::TextEditor::textColourId,                  text },
            { juce::TextEditor::outlineColourId,               border },
            { juce::TextEditor::focusedOutlineColourId,        accent },
            { juce::TextEditor::highlightColourId,             accent.withAlpha (0.2f) },
            { juce::TextEditor::highlightedTextColourId,       text },
            { juce::CaretComponent::caretColourId,             accent },

            { juce::ComboBox::backgroundColourId,              panel },
            { juce::ComboBox::textColourId,                    text },
            { juce::ComboBox::outlineColourId,                 border },
            { juce::ComboBox::arrowColourId,                   textSecondary },
            { juce::ComboBox::focusedOutlineColourId,          accent },

            { juce::PopupMenu::backgroundColourId,             panel },
            { juce::PopupMenu::textColourId,                   text },
            { juce::PopupMenu::headerTextColourId,             textSecondary },
            { juce::PopupMenu::highlightedBackgroundColourId,  trough },
            { juce::PopupMenu::highlightedTextColourId,        text },

            { juce::Slider::backgroundColourId,                trough },
            { juce::Slider::trackColourId,                     accent },
            { juce::Slider::thumbColourId,                     accent },
            { juce::Slider::rotarySliderFillColourId,          accent },
            { juce::Slider::rotarySliderOutlineColourId,       trough },
            { juce::Slider::textBoxTextColourId,               text },
            { juce::Slider::textBoxBackgroundColourId,         panel },
            { juce::Slider::textBoxOutlineColourId,            border },
            { juce::Slider::textBoxHighlightColourId,          accent.withAlpha (0.2f) },

            { juce::ListBox::backgroundColourId,               panel },
            { juce::ListBox::outlineColourId,                  border },
            { juce::ListBox::textColourId,                     text },
            { juce::ScrollBar::thumbColourId,                  border.darker (0.1f) },

            { juce::TooltipWindow::backgroundColourId,         text },
            { juce::TooltipWindow::textColourId,               background },
            { juce::TooltipWindow::outlineColourId,            text },

            { juce::AlertWindow::backgroundColourId,           panel },
            { juce::AlertWindow::textColourId,                 text },
            { juce::AlertWindow::outlineColourId,              border },
        };

        for (auto& [id, colour] : defaults)
            setColour (id, colour);
    }

    // Sans: Inter (regular o semibold). Serif: Source Serif 4. El resto, lo de Windows.
    juce::Typeface::Ptr LookAndFeel::getTypefaceForFont (const juce::Font& font)
    {
        const auto name = font.getTypefaceName();

        if (name == juce::Font::getDefaultSansSerifFontName())
            return font.isBold() ? sansBold : sans;

        if (name == juce::Font::getDefaultSerifFontName())
            return serif;

        return juce::LookAndFeel_V4::getTypefaceForFont (font);
    }

    // Redondeado, con una sombra de 1 px abajo para que se lea como botón (al apretarlo
    // baja y la sombra desaparece). Los claros llevan borde.
    void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& backgroundColour,
                                            bool highlighted, bool down)
    {
        auto bounds = b.getLocalBounds().toFloat().reduced (0.5f).withTrimmedBottom (1.0f);
        const bool light = backgroundColour.getPerceivedBrightness() > 0.8f;
        const float alpha = b.isEnabled() ? 1.0f : 0.45f;
        auto colour = backgroundColour;

        if (down)
            colour = colour.darker (0.12f);
        else if (highlighted)
            colour = light ? colour.darker (0.05f) : colour.brighter (0.08f);

        if (! down)
        {
            g.setColour ((light ? controlEdge : backgroundColour.darker (0.35f)).withMultipliedAlpha (alpha));
            g.fillRoundedRectangle (bounds.translated (0.0f, 1.0f), radius);
        }
        else
        {
            bounds.translate (0.0f, 1.0f);
        }

        g.setColour (colour.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (bounds, radius);

        if (light)
        {
            g.setColour ((highlighted ? controlEdge.darker (0.15f) : controlEdge).withMultipliedAlpha (alpha));
            g.drawRoundedRectangle (bounds, radius, 1.0f);
        }
    }

    // Como el de JUCE, más la luz de los toggles: hueca apagado, llena prendido.
    void LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool down)
    {
        auto area = b.getLocalBounds().withTrimmedBottom (1).translated (0, down ? 1 : 0).reduced (6, 0);
        const bool on = b.getToggleState();
        const float alpha = b.isEnabled() ? 1.0f : 0.45f;

        if (b.getProperties()["light"])
        {
            auto led = area.removeFromLeft (16).toFloat().withSizeKeepingCentre (8.0f, 8.0f).translated (4.0f, 0.0f);

            if (on)
            {
                g.setColour (onAccent.withAlpha (0.35f * alpha));
                g.fillEllipse (led.expanded (2.5f));
                g.setColour (onAccent.withMultipliedAlpha (alpha));
                g.fillEllipse (led);
            }
            else
            {
                g.setColour (ledOff.withMultipliedAlpha (alpha));
                g.drawEllipse (led.reduced (0.75f), 1.5f);
            }
        }

        const auto font = getTextButtonFont (b, b.getHeight());
        const auto colour = b.findColour (on ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                                .withMultipliedAlpha (alpha);
        g.setFont (font);
        g.setColour (colour);

        const auto iconText = b.getProperties()["icon"].toString();
        if (iconText.isEmpty())
        {
            g.drawFittedText (b.getButtonText(), area, juce::Justification::centred, 2);
            return;
        }

        // Icono + texto, centrados juntos.
        juce::Path icon;
        icon.restoreFromString (iconText);
        const auto label = b.getButtonText();
        const float size = juce::jmin (18.0f, (float) area.getHeight() * 0.55f);
        const float gap = label.isEmpty() ? 0.0f : 7.0f;
        const float textW = label.isEmpty() ? 0.0f : juce::GlyphArrangement::getStringWidth (font, label) + 2.0f;
        const float total = juce::jmin ((float) area.getWidth(), size + gap + textW);
        const float x = (float) area.getCentreX() - total * 0.5f;

        icon.applyTransform (juce::AffineTransform::scale (size / 24.0f)
                                 .translated (x, (float) area.getCentreY() - size * 0.5f));

        if (b.getProperties()["iconFilled"])
            g.fillPath (icon);
        else
            g.strokePath (icon, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (label.isNotEmpty())
            g.drawFittedText (label, juce::Rectangle<float> (x + size + gap, (float) area.getY(), total - size - gap,
                                                             (float) area.getHeight()).toNearestInt(),
                              juce::Justification::centredLeft, 1);
    }

    juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
    {
        return juce::FontOptions (juce::jmin (13.5f, (float) buttonHeight * 0.5f), juce::Font::bold);
    }

    void LookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
    {
        const auto bounds = juce::Rectangle<float> ((float) width, (float) height).reduced (0.5f);

        g.setColour (box.findColour (juce::ComboBox::backgroundColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.5f));
        g.fillRoundedRectangle (bounds, radius);
        g.setColour (box.hasKeyboardFocus (true) ? accent : box.isMouseOver (true) ? border.darker (0.15f) : border);
        g.drawRoundedRectangle (bounds, radius, 1.0f);

        // Flecha: una V fina.
        const auto arrow = juce::Rectangle<float> ((float) width - 26.0f, 0.0f, 14.0f, (float) height).withSizeKeepingCentre (9.0f, 5.0f);
        juce::Path p;
        p.startNewSubPath (arrow.getTopLeft());
        p.lineTo (arrow.getCentreX(), arrow.getBottom());
        p.lineTo (arrow.getTopRight());
        g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f));
        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}
