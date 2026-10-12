#include "MainComponent.h"
#include "DrumKit.h"
#include "VoiceChain.h"

namespace
{
    const juce::Colour background (0xff1b1d21), panel (0xff26292f), accent (0xffff8a1f),
                       recRed (0xffe0393e), playGreen (0xff3fb950),
                       loopBlue (0xff2f81f7), loopBlueLight (0xff79c0ff);

    // Ventana que muestra la interfaz de AmpliTube.
    class PluginWindow : public juce::DocumentWindow
    {
    public:
        PluginWindow (const juce::String& title, juce::AudioProcessorEditor* editor, std::function<void()> onCloseIn)
            : DocumentWindow (title, background, closeButton), onClose (std::move (onCloseIn))
        {
            setUsingNativeTitleBar (true);
            setContentOwned (editor, true);
            setResizable (editor->isResizable(), false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { onClose(); }

    private:
        std::function<void()> onClose;
    };

    void noKeyboardFocus (juce::Component& c)
    {
        c.setWantsKeyboardFocus (false);
        c.setMouseClickGrabsKeyboardFocus (false);
    }

    juce::File exportFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("AmpBox Exports");
    }

    // ffmpeg: en el PATH o donde lo deja winget.
    juce::File findFfmpeg()
    {
        auto path = juce::SystemStats::getEnvironmentVariable ("PATH", {});

        for (auto dir : juce::StringArray::fromTokens (path, ";", ""))
        {
            dir = dir.trim().unquoted();
            if (dir.isNotEmpty() && juce::File::isAbsolutePath (dir))
            {
                auto f = juce::File (dir).getChildFile ("ffmpeg.exe");
                if (f.existsAsFile())
                    return f;
            }
        }

        auto winget = juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("Microsoft/WinGet/Packages");
        for (auto& f : winget.findChildFiles (juce::File::findFiles, true, "ffmpeg.exe"))
            return f;

        return {};
    }

    // Icono de carpeta (botón de exportados).
    std::unique_ptr<juce::Drawable> folderIcon (juce::Colour colour)
    {
        juce::Path p;
        p.startNewSubPath (2.0f, 6.0f);
        p.lineTo (9.0f, 6.0f);
        p.lineTo (11.0f, 8.5f);
        p.lineTo (22.0f, 8.5f);
        p.lineTo (22.0f, 20.0f);
        p.lineTo (2.0f, 20.0f);
        p.closeSubPath();

        auto d = std::make_unique<juce::DrawablePath>();
        d->setPath (p);
        d->setFill (colour);
        return d;
    }

    // Icono de papelera (botón Borrar todo).
    std::unique_ptr<juce::Drawable> trashIcon (juce::Colour colour)
    {
        juce::Path p;
        p.addRectangle (3.0f, 5.0f, 18.0f, 2.0f);              // tapa
        p.addRectangle (9.0f, 2.5f, 6.0f, 2.5f);               // asa
        p.startNewSubPath (5.0f, 8.5f);                        // cuerpo
        p.lineTo (19.0f, 8.5f);
        p.lineTo (17.5f, 22.0f);
        p.lineTo (6.5f, 22.0f);
        p.closeSubPath();

        auto d = std::make_unique<juce::DrawablePath>();
        d->setPath (p);
        d->setFill (colour);
        return d;
    }

    // Plugins VST3 instalados (las carpetas .vst3, sin entrar en ellas).
    void findVst3 (const juce::File& dir, juce::Array<juce::File>& found, int depth = 0)
    {
        for (auto& f : dir.findChildFiles (juce::File::findFilesAndDirectories, false))
        {
            if (f.hasFileExtension ("vst3"))
                found.add (f);
            else if (f.isDirectory() && depth < 3)
                findVst3 (f, found, depth + 1);
        }
    }

    // VST3 y, si no hay versión VST3 con el mismo nombre, los VST2 de 64 bits
    // (Guitar Rig 5 solo existe como VST2).
    juce::Array<juce::File> installedPlugins()
    {
        juce::Array<juce::File> found;
        findVst3 (juce::File ("C:\\Program Files\\Common Files\\VST3"), found);

        juce::StringArray names;
        for (auto& f : found)
            names.add (f.getFileNameWithoutExtension());

       #if JUCE_PLUGINHOST_VST   // solo si se compiló con vst2sdk/
        for (auto* dir : { "C:\\Program Files\\VSTPlugins", "C:\\Program Files\\Native Instruments\\VSTPlugins 64 bit",
                           "C:\\Program Files\\Steinberg\\VSTPlugins" })
        {
            for (auto& f : juce::File (dir).findChildFiles (juce::File::findFiles, false, "*.dll"))
            {
                if (names.contains (f.getFileNameWithoutExtension(), true))
                    continue;

                names.add (f.getFileNameWithoutExtension());
                found.add (f);
            }
        }
       #endif

        return found;
    }
}

//==============================================================================
// Presets: pestañas FAVORITOS / TODOS, buscador y lista con estrella.
class PresetPanel : public juce::Component, private juce::ListBoxModel
{
public:
    PresetPanel (Engine& engineIn, std::function<void()> onDoneIn)
        : engine (engineIn), onDone (std::move (onDoneIn))
    {
        for (auto* b : { &favButton, &allButton })
        {
            addAndMakeVisible (b);
            noKeyboardFocus (*b);
            b->setColour (juce::TextButton::buttonOnColourId, accent);
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        }

        favButton.onClick = [this] { showFavorites = true; refresh(); };
        allButton.onClick = [this] { showFavorites = false; refresh(); };

        addAndMakeVisible (search);
        search.setTextToShowWhenEmpty ("Buscar en todos los presets...", juce::Colours::grey);
        search.onTextChange = [this]
        {
            showFavorites = search.isEmpty() && showFavorites;
            refresh();
        };
        search.onReturnKey = [this] { onDone(); };
        search.onEscapeKey = [this] { search.clear(); refresh(); onDone(); };

        addAndMakeVisible (list);
        list.setModel (this);
        list.setRowHeight (38);
        list.setColour (juce::ListBox::backgroundColourId, panel);
        noKeyboardFocus (list);

        refresh();
    }

    void refresh()
    {
        auto& lib = engine.library;
        rows.clear();

        if (showFavorites)
            for (auto& path : lib.favorites)
                rows.push_back (PresetLibrary::describe (path));
        else
            rows = lib.search (search.getText());

        favButton.setToggleState (showFavorites, juce::dontSendNotification);
        allButton.setToggleState (! showFavorites, juce::dontSendNotification);
        allButton.setButtonText ("TODOS (" + juce::String ((int) (showFavorites ? lib.getAll().size() : rows.size())) + ")");

        list.updateContent();
        list.repaint();
    }

    // Tras cambiar de preset con el teclado: marcarlo y mostrarlo.
    void showCurrent()
    {
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].path.equalsIgnoreCase (engine.library.current))
                list.scrollToEnsureRowIsOnscreen ((int) i);

        list.repaint();
    }

    int getNumRows() override { return (int) rows.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool) override
    {
        if (row < 0 || row >= (int) rows.size())
            return;

        auto& p = rows[(size_t) row];
        auto& lib = engine.library;
        const bool current = p.path.equalsIgnoreCase (lib.current);
        const bool favorite = lib.isFavorite (p.path);
        const auto text = current ? juce::Colours::black : juce::Colours::white;

        if (current)
        {
            g.setColour (accent);
            g.fillRect (0, 0, w, h);
        }

        g.setColour (favorite ? (current ? juce::Colours::black : accent) : text.withAlpha (0.3f));
        g.setFont (juce::FontOptions (18.0f));
        g.drawText (juce::String (juce::CharPointer_UTF8 (favorite ? "\xe2\x98\x85" : "\xe2\x98\x86")),
                    0, 0, starWidth, h, juce::Justification::centred);

        int x = starWidth;

        // En favoritos, el número es la tecla.
        if (showFavorites)
        {
            g.setFont (juce::FontOptions (13.0f));
            g.setColour (text.withAlpha (0.5f));
            g.drawText (juce::String (row + 1), x, 0, 22, h, juce::Justification::centredLeft);
            x += 24;
        }

        g.setFont (juce::FontOptions (15.0f));
        g.setColour (text);
        g.drawText (p.name, x, 3, w - x - 4, h / 2, juce::Justification::centredLeft, true);
        g.setFont (juce::FontOptions (11.5f));
        g.setColour (text.withAlpha (0.55f));
        g.drawText (p.folder, x, h / 2, w - x - 4, h / 2 - 3, juce::Justification::centredLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (row < 0 || row >= (int) rows.size())
            return;

        const auto path = rows[(size_t) row].path;

        if (e.x < starWidth)
        {
            engine.library.toggleFavorite (path);
            engine.saveSettings();

            if (showFavorites)
                refresh();
            else
                list.repaint();
        }
        else
        {
            engine.selectPreset (path);
            list.repaint();
        }

        onDone();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto tabs = r.removeFromTop (30);
        favButton.setBounds (tabs.removeFromLeft (tabs.getWidth() / 2).withTrimmedRight (3));
        allButton.setBounds (tabs.withTrimmedLeft (3));
        r.removeFromTop (6);
        search.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        list.setBounds (r);
    }

private:
    static constexpr int starWidth = 30;

    Engine& engine;
    std::function<void()> onDone;
    juce::TextButton favButton { "FAVORITOS" }, allButton { "TODOS" };
    juce::TextEditor search;
    juce::ListBox list;
    std::vector<PresetLibrary::Preset> rows;
    bool showFavorites = true;
};

//==============================================================================
// Una pista del looper: nombre, nivel, volumen, paneo, mute y borrar.
class TrackRow : public juce::Component
{
public:
    TrackRow (Looper& looperIn, int slotIn) : looper (looperIn), slot (slotIn), track (looperIn.getTrack (slotIn))
    {
        addAndMakeVisible (nameLabel);
        addAndMakeVisible (gainSlider);
        addAndMakeVisible (panSlider);
        addAndMakeVisible (muteButton);
        addAndMakeVisible (deleteButton);
        addAndMakeVisible (separateButton);
        addChildComponent (voiceButton);

        voiceButton.setColour (juce::TextButton::buttonColourId, accent.withAlpha (0.35f));
        voiceButton.setTooltip ("Cambiar la voz de esta pista (estilo, brillo, reverb...)");
        voiceButton.onClick = [this] { if (onEditVoice) onEditVoice (slot); };

        separateButton.setTooltip ("Sacar la bateria o el bajo de esta pista (Demucs)");
        separateButton.onClick = [this]
        {
            juce::PopupMenu menu;
            menu.addItem (1, "Sacar bateria");
            menu.addItem (2, "Sacar bajo");
            menu.addItem (3, "Sacar bateria y bajo");

            juce::Component::SafePointer<TrackRow> safe (this);
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&separateButton), [safe] (int result)
            {
                if (safe != nullptr && result > 0 && safe->onSeparate)
                    safe->onSeparate (safe->slot, result);
            });
        };

        gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 20);
        // Hasta 300 % (+9.5 dB): la batería y el bajo separados salen más bajos que la mezcla.
        // 100 % queda en el medio de la barra.
        gainSlider.setRange (0.0, 3.0, 0.01);
        gainSlider.setSkewFactorFromMidPoint (1.0);
        gainSlider.setDoubleClickReturnValue (true, 1.0);
        gainSlider.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + " %"; };
        gainSlider.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("0123456789.").getDoubleValue() / 100.0; };
        gainSlider.setTooltip ("Volumen de la pista (100 % = como se grabo; doble clic = 100 %)");
        gainSlider.onValueChange = [this] { track.gain = (float) gainSlider.getValue(); };

        panSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        panSlider.setTextBoxStyle (juce::Slider::TextBoxRight, true, 38, 20);
        panSlider.setRange (-1.0, 1.0, 0.01);
        panSlider.textFromValueFunction = [] (double v)
        {
            const int pct = juce::roundToInt (std::abs (v) * 100.0);
            return pct == 0 ? juce::String ("C") : (v < 0 ? "I " : "D ") + juce::String (pct);
        };
        panSlider.setColour (juce::Slider::thumbColourId, accent);
        panSlider.setDoubleClickReturnValue (true, 0.0);
        panSlider.setTooltip ("Paneo: izquierda / centro / derecha (doble clic = centro)");
        panSlider.onValueChange = [this] { track.pan = (float) panSlider.getValue(); };

        muteButton.setClickingTogglesState (true);
        muteButton.setColour (juce::TextButton::buttonOnColourId, accent);
        muteButton.onClick = [this] { track.mute = muteButton.getToggleState(); };

        deleteButton.setTooltip ("Borrar esta pista");
        deleteButton.onClick = [this] { looper.deleteTrack (slot); };

        for (auto* c : std::initializer_list<juce::Component*> { &gainSlider, &panSlider, &muteButton, &deleteButton,
                                                                 &separateButton, &voiceButton })
            noKeyboardFocus (*c);

        // La rueda del mouse controla el tempo, no los sliders.
        gainSlider.setScrollWheelEnabled (false);
        panSlider.setScrollWheelEnabled (false);
    }

    // Copia los valores de la pista a los controles (al aparecer la fila).
    void sync (int number, const juce::String& sourceName)
    {
        nameLabel.setText (juce::String (number) + ". " + sourceName, juce::dontSendNotification);
        nameLabel.setTooltip ("Pista " + juce::String (number) + " (grabada de " + sourceName + ")");
        gainSlider.setValue (track.gain, juce::dontSendNotification);
        panSlider.setValue (track.pan, juce::dontSendNotification);
        muteButton.setToggleState (track.mute, juce::dontSendNotification);
    }

    void tick()
    {
        level = juce::jmax (track.peak.exchange (0.0f), level * 0.85f);
        repaint (meter);

        // Cambios hechos desde el Launchkey (perillas y pads).
        if (std::abs (gainSlider.getValue() - track.gain) > 0.005)
            gainSlider.setValue (track.gain, juce::dontSendNotification);

        if (muteButton.getToggleState() != track.mute)
            muteButton.setToggleState (track.mute, juce::dontSendNotification);

        if (std::abs (panSlider.getValue() - track.pan) > 0.005)
            panSlider.setValue (track.pan, juce::dontSendNotification);
    }

    // Mientras Demucs separa esta pista.
    void setSeparating (bool on)
    {
        if (separating != on)
        {
            separating = on;
            separateButton.setEnabled (! on);
            repaint();
        }
    }

    // Pista de voz re-editable (botón VOZ) y si se está volviendo a procesar.
    void setVoice (bool editable, bool rendering)
    {
        voiceButton.setVisible (editable);

        if (voiceRendering != rendering)
        {
            voiceRendering = rendering;
            repaint();
        }
    }

    std::function<void (int slot, int stems)> onSeparate;
    std::function<void (int slot)> onEditVoice;

    void paint (juce::Graphics& g) override
    {
        const bool recording = track.state == Looper::State::Recording;

        g.setColour (panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

        if (recording)
        {
            g.setColour (recRed);
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f, 2.0f);
        }

        g.setColour (background);
        g.fillRect (meter);


        auto db = juce::Decibels::gainToDecibels (level, -60.0f);
        g.setColour (track.mute ? juce::Colours::grey : playGreen);
        g.fillRect (meter.withWidth ((int) juce::jmap (db, -60.0f, 0.0f, 0.0f, (float) meter.getWidth())));

        if (recording)
        {
            g.setColour (recRed);
            g.drawText ("GRABANDO", meter, juce::Justification::centred);
        }
        else if (separating)
        {
            g.setColour (accent);
            g.drawText ("SEPARANDO...", meter, juce::Justification::centred);
        }
        else if (voiceRendering)
        {
            g.setColour (accent);
            g.drawText ("APLICANDO VOZ...", meter, juce::Justification::centred);
        }

        // Marca del centro del paneo.
        auto pan = panSlider.getBounds().withTrimmedRight (38);
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.drawVerticalLine (pan.getCentreX(), (float) pan.getY() + 6.0f, (float) pan.getBottom() - 6.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 4);
        nameLabel.setBounds (r.removeFromLeft (130));
        deleteButton.setBounds (r.removeFromRight (32).reduced (0, 4));
        r.removeFromRight (6);
        muteButton.setBounds (r.removeFromRight (32).reduced (0, 4));
        r.removeFromRight (6);
        separateButton.setBounds (r.removeFromRight (74).reduced (0, 4));
        r.removeFromRight (6);
        voiceButton.setBounds (r.removeFromRight (46).reduced (0, 4));
        r.removeFromRight (6);
        panSlider.setBounds (r.removeFromRight (120));
        r.removeFromRight (6);
        meter = r.removeFromLeft (120).reduced (0, 10);
        r.removeFromLeft (8);
        gainSlider.setBounds (r);
    }

private:
    Looper& looper;
    const int slot;
    Looper::Track& track;

    juce::Label nameLabel;
    juce::Slider gainSlider, panSlider;
    juce::TextButton muteButton { "M" }, deleteButton { "X" }, separateButton { "SEPARAR" }, voiceButton { "VOZ" };
    juce::Rectangle<int> meter;
    bool separating = false, voiceRendering = false;
    float level = 0.0f;
};

//==============================================================================
// Una entrada (canal): armar, nombre, de dónde viene, plugin, oír, nivel, quitar.
class ChannelRow : public juce::Component
{
public:
    struct Callbacks
    {
        std::function<void (int)> openEditor, closeEditor, removed, openTuner;
        std::function<void()> changed;   // devolver el teclado a la ventana principal
        // Muestra "Cargando <plugin>..." y después llama a la función (la carga
        // bloquea la ventana; así se ve qué está pasando).
        std::function<void (const juce::String&, std::function<void()>)> loading;
    };

    ChannelRow (Engine& engineIn, int indexIn, Callbacks callbacksIn)
        : engine (engineIn), index (indexIn), channel (engineIn.getChannel (indexIn)), callbacks (std::move (callbacksIn))
    {
        for (auto* c : std::initializer_list<juce::Component*> { &armButton, &numberLabel, &nameLabel, &typeLabel,
                                                                 &sourceBox, &pluginButton, &editButton, &tunerButton,
                                                                 &monitorButton, &removeButton })
            addAndMakeVisible (c);

        for (auto* c : std::initializer_list<juce::Component*> { &armButton, &sourceBox, &pluginButton, &editButton,
                                                                 &monitorButton, &removeButton })
            noKeyboardFocus (*c);

        armButton.setClickingTogglesState (true);
        armButton.setColour (juce::TextButton::buttonOnColourId, recRed);
        armButton.setTooltip ("Armar: graba esta entrada al pulsar REC");
        armButton.onClick = [this]
        {
            channel.armed = armButton.getToggleState();
            engine.saveSettings();
        };

        monitorButton.setClickingTogglesState (true);
        monitorButton.setColour (juce::TextButton::buttonOnColourId, playGreen.withAlpha (0.7f));
        monitorButton.setTooltip ("Escuchar esta entrada en vivo");
        monitorButton.onClick = [this]
        {
            channel.monitor = monitorButton.getToggleState();
            engine.saveSettings();
        };

        numberLabel.setJustificationType (juce::Justification::centred);
        numberLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.5f));

        nameLabel.setEditable (false, true, false);
        nameLabel.addMouseListener (this, false);
        nameLabel.setTooltip ("Doble clic para cambiar el nombre");
        nameLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        nameLabel.onTextChange = [this]
        {
            auto name = nameLabel.getText().trim();
            if (name.isEmpty())
                name = Engine::typeName (channel.type);

            engine.setChannelName (index, name);
            nameLabel.setText (name, juce::dontSendNotification);
            callbacks.changed();
        };

        typeLabel.setFont (juce::FontOptions (11.0f));
        typeLabel.setColour (juce::Label::textColourId, accent);

        sourceBox.onChange = [this] { sourceChanged(); };

        addAndMakeVisible (volumeSlider);
        noKeyboardFocus (volumeSlider);
        volumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volumeSlider.setRange (0.0, 1.5, 0.01);
        volumeSlider.setDoubleClickReturnValue (true, 1.0);
        volumeSlider.setPopupDisplayEnabled (true, true, nullptr);
        volumeSlider.setScrollWheelEnabled (false);   // la rueda es el tempo
        volumeSlider.setColour (juce::Slider::thumbColourId, accent);
        volumeSlider.textFromValueFunction = [] (double v)
        {
            return v <= 0.001 ? juce::String ("-inf") : juce::String (juce::Decibels::gainToDecibels (v), 1) + " dB";
        };
        volumeSlider.setTooltip ("Volumen de esta entrada y de lo grabado con ella (no cambia lo grabado). Doble clic = 0 dB");
        volumeSlider.onValueChange = [this] { channel.volume = (float) volumeSlider.getValue(); };
        volumeSlider.onDragEnd = [this] { engine.saveSettings(); };

        pluginButton.onClick = [this] { showPluginMenu(); };
        editButton.onClick = [this] { callbacks.openEditor (index); };
        editButton.setTooltip ("Abrir la ventana del plugin");
        tunerButton.onClick = [this] { callbacks.openTuner (index); };
        tunerButton.setTooltip ("Afinador: muestra la nota que tocas o cantas y si estas afinado");

        removeButton.setTooltip ("Quitar esta entrada");
        removeButton.onClick = [this] { callbacks.removed (index); };
    }

    // Copia el estado del canal a los controles.
    void sync (int numberIn)
    {
        number = numberIn;
        const bool guitar = channel.type == Engine::ChannelType::Guitar;

        numberLabel.setText (juce::String (number), juce::dontSendNotification);
        nameLabel.setText (channel.name, juce::dontSendNotification);
        typeLabel.setText (Engine::typeName (channel.type).toUpperCase(), juce::dontSendNotification);
        armButton.setToggleState (channel.armed, juce::dontSendNotification);
        monitorButton.setToggleState (channel.monitor, juce::dontSendNotification);
        volumeSlider.setValue (channel.volume, juce::dontSendNotification);
        tunerButton.setVisible (guitar || channel.type == Engine::ChannelType::Audio);

        auto* proc = engine.getChannelProcessor (index);
        const auto none = channel.type == Engine::ChannelType::Midi ? juce::String ("Elegir instrumento...")
                        : guitar ? juce::String ("Elegir amplificador...") : juce::String ("Sin efecto");
        // Guardado pero sin abrir: esperando su turno al arrancar, o falló (no está instalado...).
        const bool missing = proc == nullptr && channel.pluginFailed;
        const bool pending = proc == nullptr && ! missing && channel.pluginFile != juce::File();
        pluginButton.setButtonText (proc != nullptr ? proc->getName()
                                    : missing ? "(no cargo) " + channel.pluginFile.getFileNameWithoutExtension()
                                    : pending ? "Cargando " + channel.pluginFile.getFileNameWithoutExtension() + "..." : none);
        pluginButton.setColour (juce::TextButton::textColourOffId, missing ? recRed.brighter (0.4f) : juce::Colours::white);
        pluginButton.setTooltip (missing ? "No se pudo cargar " + channel.pluginFile.getFullPathName() + ". Clic para elegir otro"
                                 : channel.type == Engine::ChannelType::Midi ? "Instrumento VST3 (piano, bateria...)"
                                 : guitar ? "Amplificador VST3 / VST2 (AmpliTube, Guitar Rig...)" : "Efecto VST3 opcional");
        nameLabel.setTooltip (guitar ? "Clic: usar esta guitarra (Tab cambia). Doble clic: nombre"
                                     : "Doble clic para cambiar el nombre");
        editButton.setEnabled (proc != nullptr);

        refreshSources();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == &nameLabel && channel.type == Engine::ChannelType::Guitar && e.getNumberOfClicks() == 1)
        {
            engine.selectGuitar (index);
            callbacks.changed();
        }
    }

    // Entradas de audio activas, o teclados MIDI.
    void refreshSources()
    {
        sourceBox.clear (juce::dontSendNotification);
        sourceBox.setEnabled (channel.type != Engine::ChannelType::Pc);
        midiIds.clear();

        if (channel.type == Engine::ChannelType::Pc)
        {
            sourceBox.addItem ("Sonido de Windows", 1);
            sourceBox.setSelectedId (1, juce::dontSendNotification);
            sourceBox.setTooltip ("Lo que suena en la PC (YouTube, musica...). " + engine.getPcStatus());
            return;
        }

        if (channel.type == Engine::ChannelType::Midi)
        {
            sourceBox.addItem ("Todos los MIDI", 1);
            midiIds.add ({});

            for (auto& d : juce::MidiInput::getAvailableDevices())
            {
                sourceBox.addItem (d.name, midiIds.size() + 1);
                midiIds.add (d.identifier);
            }

            // El guardado puede no estar conectado ahora: se muestra igual.
            if (! midiIds.contains (channel.midiDevice))
            {
                sourceBox.addItem ("(desconectado)", midiIds.size() + 1);
                midiIds.add (channel.midiDevice);
            }

            sourceBox.setSelectedItemIndex (midiIds.indexOf (channel.midiDevice), juce::dontSendNotification);
            return;
        }

        if (auto* device = engine.deviceManager.getCurrentAudioDevice())
        {
            auto names = device->getInputChannelNames();
            auto active = device->getActiveInputChannels();
            int id = 1;

            for (int i = 0; i < names.size(); ++i)
                if (active[i])
                    sourceBox.addItem (names[i], id++);
        }

        if (sourceBox.getNumItems() > 0)
            sourceBox.setSelectedItemIndex (juce::jlimit (0, sourceBox.getNumItems() - 1, channel.input.load()),
                                            juce::dontSendNotification);
    }

    void tick()
    {
        level = juce::jmax (channel.peak.exchange (0.0f), level * 0.85f);
        repaint (meter.expanded (1));

        if (armButton.getToggleState() != channel.armed)   // armado con el teclado
            armButton.setToggleState (channel.armed, juce::dontSendNotification);

        if (monitorButton.getToggleState() != channel.monitor)
            monitorButton.setToggleState (channel.monitor, juce::dontSendNotification);

        if (std::abs (volumeSlider.getValue() - channel.volume) > 0.005)   // perillas del Launchkey
            volumeSlider.setValue (channel.volume, juce::dontSendNotification);

        if (channel.type == Engine::ChannelType::Guitar)
            repaint (0, 0, 4, getHeight());
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

        if (channel.armed)
        {
            g.setColour (recRed.withAlpha (engine.looper.isRecording() ? 1.0f : 0.5f));
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 4.0f, 1.5f);
        }

        // Rack de guitarras: barra naranja en la activa.
        if (channel.type == Engine::ChannelType::Guitar && engine.countGuitars() > 1 && engine.getActiveGuitar() == index)
        {
            g.setColour (accent);
            g.fillRoundedRectangle (getLocalBounds().removeFromLeft (4).toFloat(), 2.0f);
        }

        g.setColour (background);
        g.fillRect (meter);
        auto db = juce::Decibels::gainToDecibels (level, -60.0f);
        g.setColour (level >= 0.99f ? juce::Colours::red : channel.monitor ? accent : juce::Colours::grey);
        g.fillRect (meter.withWidth ((int) juce::jmap (db, -60.0f, 0.0f, 0.0f, (float) meter.getWidth())));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 5);
        armButton.setBounds (r.removeFromLeft (34));
        numberLabel.setBounds (r.removeFromLeft (22));
        auto names = r.removeFromLeft (130);
        nameLabel.setBounds (names.removeFromTop (names.getHeight() * 3 / 5));
        typeLabel.setBounds (names);
        r.removeFromLeft (6);
        sourceBox.setBounds (r.removeFromLeft (140));
        r.removeFromLeft (6);
        pluginButton.setBounds (r.removeFromLeft (160));
        r.removeFromLeft (4);
        editButton.setBounds (r.removeFromLeft (56));
        r.removeFromLeft (4);
        tunerButton.setBounds (r.removeFromLeft (62));
        r.removeFromLeft (6);
        removeButton.setBounds (r.removeFromRight (30));
        r.removeFromRight (6);
        monitorButton.setBounds (r.removeFromLeft (48));
        r.removeFromLeft (6);
        volumeSlider.setBounds (r.removeFromLeft (100));
        r.removeFromLeft (6);
        meter = r.reduced (0, 8);
    }

    // Para el Launchkey.
    void openSourceMenu()   { if (sourceBox.isEnabled()) sourceBox.showPopup(); }
    void openPluginMenu()   { showPluginMenu(); }
    juce::Rectangle<int> configBounds() const
    {
        return sourceBox.getBounds().getUnion (pluginButton.getBounds()) + getPosition();
    }

private:
    void sourceChanged()
    {
        const int i = sourceBox.getSelectedItemIndex();

        if (channel.type == Engine::ChannelType::Midi)
        {
            if (juce::isPositiveAndBelow (i, midiIds.size()))
                engine.setChannelMidiDevice (index, midiIds[i]);
        }
        else if (i >= 0)
        {
            engine.setChannelInput (index, i);
        }

        callbacks.changed();
    }

    void showPluginMenu()
    {
        juce::PopupMenu menu;
        auto plugins = installedPlugins();
        const auto current = channel.pluginFile;

        menu.addSectionHeader (channel.type == Engine::ChannelType::Midi ? "Instrumento" : "Efecto");

        if (channel.type == Engine::ChannelType::Midi)
        {
            menu.addItem (999, "Bateria AmpBox (sonidos de Ableton)", true, current == DrumKit::virtualFile());
            menu.addSeparator();
        }
        else
        {
            for (int i = 0; i < VoiceChain::numStyles; ++i)
                menu.addItem (990 + i, juce::String (juce::CharPointer_UTF8 (VoiceChain::styles[i].menuText)), true,
                              current == VoiceChain::virtualFile (i));
            menu.addSeparator();
        }

        for (int i = 0; i < plugins.size(); ++i)
            menu.addItem (i + 1, plugins[i].getFileNameWithoutExtension() + (plugins[i].hasFileExtension ("dll") ? " (VST2)" : ""),
                          true, plugins[i] == current);

        menu.addSeparator();
        menu.addItem (1000, "Otro archivo .vst3...");
        menu.addItem (1001, "Quitar", channel.plugin != nullptr);

        juce::Component::SafePointer<ChannelRow> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&pluginButton),
                            [safe, plugins] (int result)
        {
            if (safe == nullptr || result == 0)
                return;

            if (result == 1001)
            {
                safe->callbacks.closeEditor (safe->index);
                safe->engine.clearChannelPlugin (safe->index);
                safe->sync (safe->number);
            }
            else if (result == 999)
            {
                safe->loadPlugin (DrumKit::virtualFile());
            }
            else if (result >= 990 && result < 990 + VoiceChain::numStyles)
            {
                safe->loadPlugin (VoiceChain::virtualFile (result - 990));
            }
            else if (result == 1000)
            {
                safe->chooser = std::make_unique<juce::FileChooser> ("Plugin VST3 / VST2", juce::File ("C:\\Program Files\\Common Files\\VST3"), "*.vst3;*.dll");
                safe->chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                              | juce::FileBrowserComponent::canSelectDirectories,
                                            [safe] (const juce::FileChooser& fc)
                {
                    if (safe != nullptr && fc.getResult() != juce::File())
                        safe->loadPlugin (fc.getResult());
                });
            }
            else if (juce::isPositiveAndNotGreaterThan (result, plugins.size()))
            {
                safe->loadPlugin (plugins[result - 1]);
            }

            if (safe != nullptr)
                safe->callbacks.changed();
        });
    }

    void loadPlugin (const juce::File& file)
    {
        callbacks.closeEditor (index);

        juce::Component::SafePointer<ChannelRow> safe (this);
        auto load = [safe, file]
        {
            if (safe == nullptr)
                return;

            juce::String error;
            if (! safe->engine.loadChannelPlugin (safe->index, file, error))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "No se pudo cargar", error);

            safe->sync (safe->number);
            safe->callbacks.changed();
        };

        // Las voces y la batería propias son instantáneas: sin aviso.
        if (file.exists())
            callbacks.loading (file.getFileNameWithoutExtension(), load);
        else
            load();
    }

    Engine& engine;
    const int index;
    Engine::Channel& channel;
    Callbacks callbacks;

    juce::TextButton armButton { juce::String (juce::CharPointer_UTF8 ("\xe2\x97\x8f")) },   // ●
                     pluginButton, editButton { "Editar" }, tunerButton { "AFINAR" }, monitorButton { "OIR" }, removeButton { "X" };
    juce::Label numberLabel, nameLabel, typeLabel;
    juce::ComboBox sourceBox;
    juce::Slider volumeSlider;
    juce::StringArray midiIds;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> meter;
    float level = 0.0f;
    int number = 0;
};

//==============================================================================
// Ventana para asignar teclas: clic en una acción y pulsar la tecla nueva.
class KeysPanel : public juce::Component
{
public:
    KeysPanel (Engine& engineIn, std::function<void()> onChangeIn)
        : engine (engineIn), onChange (std::move (onChangeIn))
    {
        for (auto& action : Engine::getKeyActions())
        {
            auto* label = labels.add (new juce::Label ({}, action.name));
            addAndMakeVisible (label);

            auto* button = buttons.add (new juce::TextButton());
            addAndMakeVisible (button);
            noKeyboardFocus (*button);
            button->onClick = [this, id = action.id, button]
            {
                waitingFor = id;
                refresh();
                button->setButtonText ("Pulsa una tecla... (Esc cancela)");
                grabKeyboardFocus();
            };
        }

        addAndMakeVisible (resetButton);
        noKeyboardFocus (resetButton);
        resetButton.onClick = [this]
        {
            engine.resetKeys();
            saveAndRefresh();
        };

        setWantsKeyboardFocus (true);
        refresh();
        setSize (880, 40 + 28 * ((labels.size() + 1) / 2) + 50);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (waitingFor.isEmpty())
            return false;

        if (key != juce::KeyPress (juce::KeyPress::escapeKey))
        {
            // Una tecla solo puede tener una acción.
            for (auto& [id, other] : engine.keys)
                if (other == key)
                    other = juce::KeyPress();

            engine.keys[waitingFor] = key;
        }

        waitingFor = {};
        saveAndRefresh();
        return true;
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);

        // Dos columnas: con todas las acciones no entra en una pantalla de 768 px.
        const int perColumn = (labels.size() + 1) / 2;
        auto columns = r.removeFromTop (perColumn * 28);
        auto leftColumn = columns.removeFromLeft (columns.getWidth() / 2 - 8);
        columns.removeFromLeft (16);

        for (int i = 0; i < labels.size(); ++i)
        {
            auto& column = i < perColumn ? leftColumn : columns;
            auto row = column.removeFromTop (24);
            column.removeFromTop (4);
            labels[i]->setBounds (row.removeFromLeft (170));
            buttons[i]->setBounds (row);
        }

        r.removeFromTop (8);
        resetButton.setBounds (r.removeFromTop (30).removeFromLeft (200));
    }

private:
    void saveAndRefresh()
    {
        engine.saveSettings();
        refresh();
        onChange();
    }

    void refresh()
    {
        auto& actions = Engine::getKeyActions();

        for (size_t i = 0; i < actions.size(); ++i)
        {
            auto key = engine.keys[actions[i].id];
            buttons[(int) i]->setButtonText (key.isValid() ? key.getTextDescription() : "(sin tecla)");
        }
    }

    Engine& engine;
    std::function<void()> onChange;
    juce::OwnedArray<juce::Label> labels;
    juce::OwnedArray<juce::TextButton> buttons;
    juce::TextButton resetButton { "Restaurar teclas" };
    juce::String waitingFor;
};

//==============================================================================
MainComponent::MainComponent()
{
    for (auto* c : std::initializer_list<juce::Component*> { &audioButton, &reloadButton, &recButton,
                                                             &playButton, &undoButton, &redoButton, &clearButton,
                                                             &metroButton, &keysButton, &waitButton, &tapButton })
    {
        addAndMakeVisible (c);
        noKeyboardFocus (*c);
    }

    addAndMakeVisible (statusLabel);
    addAndMakeVisible (pluginLabel);
    addAndMakeVisible (helpLabel);
    addAndMakeVisible (bpmLabel);

    audioButton.onClick = [this] { showAudioSettings(); };
    reloadButton.onClick = [this]
    {
        engine.reconnect();
        refreshInputs();
        refreshStatus();
    };
    reloadButton.setTooltip ("Volver a buscar la interfaz de audio y los teclados MIDI");

    recButton.onClick = [this] { engine.looper.toggleRecord(); };
    playButton.onClick = [this] { engine.looper.togglePlay(); };
    undoButton.onClick = [this] { performAction ("undo"); };
    redoButton.onClick = [this] { performAction ("redo"); };
    clearButton.onClick = [this] { performAction ("clear"); };
    clearButton.setTooltip ("Borrar todo el loop (Supr)");
    clearButton.setImages (trashIcon (juce::Colours::white.withAlpha (0.85f)).get(), trashIcon (recRed).get());
    metroButton.onClick = [this] { performAction ("metro"); };
    tapButton.onClick = [this] { performAction ("tap"); };
    tapButton.setTooltip ("Toca al ritmo (3 o 4 veces) para fijar el tempo (tecla T)");
    keysButton.onClick = [this] { showKeySettings(); };

    metroButton.setColour (juce::TextButton::buttonOnColourId, accent);
    waitButton.setColour (juce::TextButton::buttonOnColourId, accent);
    waitButton.setTooltip ("Encendido: REC espera y la toma empieza con la primera nota. "
                           "Apagado: graba en cuanto pulsas REC");
    waitButton.onClick = [this] { performAction ("wait"); };
    bpmLabel.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    bpmLabel.setJustificationType (juce::Justification::centred);
    bpmLabel.setInterceptsMouseClicks (false, false);

    helpLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.5f));
    helpLabel.setFont (juce::FontOptions (12.0f));
    statusLabel.setFont (juce::FontOptions (12.5f));

    pluginLabel.setColour (juce::Label::textColourId, accent);

    presetPanel = std::make_unique<PresetPanel> (engine, [this]
    {
        refreshPresetTitle();
        grabKeyboardFocus(); // que las teclas sigan llegando aquí
    });
    addAndMakeVisible (*presetPanel);
    addAndMakeVisible (presetTitle);
    addAndMakeVisible (presetsToggle);
    noKeyboardFocus (presetsToggle);
    presetsToggle.setTooltip ("Mostrar / ocultar los presets");
    presetsToggle.onClick = [this]
    {
        engine.presetsHidden = ! engine.presetsHidden;
        engine.saveSettings();
        resized();
    };
    presetTitle.setFont (juce::FontOptions (18.0f, juce::Font::bold));
    refreshPresetTitle();
    presetTitle.setColour (juce::Label::textColourId, accent);

    for (auto* c : std::initializer_list<juce::Component*> { &exportButton, &folderButton })
    {
        addAndMakeVisible (c);
        noKeyboardFocus (*c);
    }

    addAndMakeVisible (loopsBox);
    addAndMakeVisible (exportLabel);
    noKeyboardFocus (loopsBox);
    for (int n : { 1, 2, 4, 8 })
        loopsBox.addItem (juce::String (n) + (n == 1 ? " vuelta" : " vueltas"), n);
    loopsBox.setSelectedId (4, juce::dontSendNotification);
    loopsBox.setTooltip ("Cuantas veces se repite el loop en el MP3");

    exportButton.onClick = [this] { showExportMenu(); };
    folderButton.onClick = [] { exportFolder().createDirectory(); exportFolder().startAsProcess(); };
    folderButton.setTooltip ("Abrir la carpeta de exportados");
    folderButton.setImages (folderIcon (juce::Colours::white.withAlpha (0.8f)).get(),
                            folderIcon (accent).get());
    exportLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.7f));

    for (int i = 0; i < Looper::maxTracks; ++i)
    {
        rows.push_back (std::make_unique<TrackRow> (engine.looper, i));
        rows.back()->onSeparate = [this] (int slot, int stems) { separateTrack (slot, stems); };
        rows.back()->onEditVoice = [this] (int slot) { voiceTracks.showEditor (slot, "Voz de la pista - " + trackName (slot)); };
        addChildComponent (*rows.back());
    }

    // Entradas: una fila por canal usado.
    addAndMakeVisible (channelsTitle);
    channelsTitle.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    channelsTitle.setColour (juce::Label::textColourId, accent);

    for (auto* b : { &addGuitarButton, &addAudioButton, &addMidiButton, &addPcButton })
    {
        addAndMakeVisible (b);
        noKeyboardFocus (*b);
    }

    addAudioButton.setTooltip ("Nueva entrada de audio (voz, microfono, linea)");
    addMidiButton.setTooltip ("Nueva entrada MIDI (teclado o bateria) con un instrumento VST3");
    addAudioButton.onClick = [this] { addChannel (Engine::ChannelType::Audio); };
    addGuitarButton.setTooltip ("Otra guitarra con su amplificador (Tab cambia entre guitarras)");
    addGuitarButton.onClick = [this] { addChannel (Engine::ChannelType::Guitar); };
    addMidiButton.onClick = [this] { addChannel (Engine::ChannelType::Midi); };
    addPcButton.setTooltip ("Grabar lo que suena en la PC (YouTube, musica...)");
    addPcButton.onClick = [this] { addChannel (Engine::ChannelType::Pc); };

    ChannelRow::Callbacks callbacks;
    callbacks.openEditor = [this] (int i) { showPluginEditor (i); };
    callbacks.closeEditor = [this] (int i) { closePluginEditor (i); };
    callbacks.removed = [this] (int i) { removeChannel (i); };
    callbacks.openTuner = [this] (int i) { showTuner (i); };
    callbacks.changed = [this] { grabKeyboardFocus(); };
    callbacks.loading = [this] (const juce::String& name, std::function<void()> work)
    {
        showLoading ("Cargando " + name, "Los amplificadores grandes (AmpliTube) tardan hasta 1 minuto.", std::move (work));
    };

    for (int i = 0; i < Engine::maxChannels; ++i)
    {
        channelRows.push_back (std::make_unique<ChannelRow> (engine, i, callbacks));
        addChildComponent (*channelRows.back());
    }

    dawLights.fill (-1);
    engine.onDawMessage = [this] (const juce::MidiMessage& m) { handleDaw (m); };

    refreshHelp();

    setWantsKeyboardFocus (true);
    setSize (1180, 700);

    // Abrir el audio y los plugins después de mostrar la ventana, con un aviso:
    // si no, al abrir parece trabada.
    showLoading ("Abriendo AmpBox", "Buscando la interfaz de audio y los teclados MIDI...", [this]
    {
        engine.deviceManager.addChangeListener (this);
        engine.start();
        refreshChannels();
        refreshStatus();
        presetPanel->refresh();   // la biblioteca se lee en engine.start()
        refreshPresetTitle();
        startTimerHz (30);

        pluginQueue = engine.getPendingPlugins();
        pluginTotal = (int) pluginQueue.size();
        restoreNextPlugin();
    });
}

MainComponent::~MainComponent()
{
    stopTimer();
    engine.onDawMessage = nullptr;
    engine.deviceManager.removeChangeListener (this);
    engine.saveState();
    editorWindows.clear();
    engine.stop();
}

void MainComponent::showLoading (const juce::String& title, const juce::String& detail, std::function<void()> work)
{
    loadingTitle = title;
    loadingDetail = detail;
    loadingWork = std::move (work);   // arranca en paintOverChildren, ya con el aviso en pantalla
    ++loadingToken;
    repaint();

    // Por si la ventana no se pinta (minimizada): arrancar igual.
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Timer::callAfterDelay (1500, [safe]
    {
        if (safe != nullptr && safe->loadingWork != nullptr)
            safe->runLoadingWork();
    });
}

// Se llama cuando el aviso ya se pintó: recién ahí se hace lo que bloquea la ventana.
// Con Direct2D el cuadro llega a la pantalla un poco después del paint: 300 ms de margen.
void MainComponent::runLoadingWork()
{
    const int token = loadingToken;
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Timer::callAfterDelay (300, [safe, token, work = std::exchange (loadingWork, nullptr)]
    {
        if (safe == nullptr)
            return;

        work();

        // Si work() no pidió otro aviso, se quita.
        if (safe != nullptr && safe->loadingToken == token)
        {
            safe->loadingTitle = {};
            safe->repaint();
        }
    });
}

// Los plugins guardados de la sesión anterior, de a uno, con aviso.
void MainComponent::restoreNextPlugin()
{
    if (pluginQueue.empty())
    {
        pluginLabel.setText (pluginFailures.isEmpty() ? juce::String()
                                                      : "No se pudo cargar: " + pluginFailures.joinIntoString (", "),
                             juce::dontSendNotification);
        refreshChannels();
        refreshStatus();
        grabKeyboardFocus();
        return;
    }

    const int index = pluginQueue.front();
    pluginQueue.erase (pluginQueue.begin());

    auto& c = engine.getChannel (index);
    const auto name = c.pluginFile.getFileNameWithoutExtension();
    const int done = pluginTotal - (int) pluginQueue.size();

    showLoading ("Cargando " + name,
                 c.name + "  (" + juce::String (done) + " de " + juce::String (pluginTotal) + ")\n"
                     + "Los amplificadores grandes (AmpliTube) tardan hasta 1 minuto.",
                 [this, index, name]
    {
        if (! engine.restoreChannelPlugin (index))
            pluginFailures.add (name);

        refreshChannels();
        restoreNextPlugin();
    });
}

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (engine.deviceManager, 1, 8, 2, 2, false, false, true, false);
    selector->setSize (500, 420);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio";
    o.dialogBackgroundColour = background;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

void MainComponent::showPluginEditor (int channel)
{
    if (auto it = editorWindows.find (channel); it != editorWindows.end())
    {
        it->second->toFront (true);
        return;
    }

    if (auto* p = engine.getChannelProcessor (channel))
    {
        if (auto* editor = p->createEditorIfNeeded())
        {
            auto title = engine.getChannel (channel).name + " - " + p->getName();
            editorWindows[channel] = std::make_unique<PluginWindow> (title, editor, [this, channel]
            {
                juce::MessageManager::callAsync ([this, channel] { editorWindows.erase (channel); });
            });
        }
    }
}

void MainComponent::closePluginEditor (int channel)
{
    editorWindows.erase (channel);
}

void MainComponent::addChannel (Engine::ChannelType type)
{
    const int result = engine.addChannel (type);

    if (result == -2)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Entrada PC",
                                                "La entrada PC necesita una interfaz ASIO.\n"
                                                "Con el audio de Windows se captaria a si misma y se acopla.");
    else if (result < 0)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Entradas",
                                                type == Engine::ChannelType::Pc
                                                    ? juce::String ("Ya hay una entrada PC.")
                                                    : "Ya hay " + juce::String (Engine::maxChannels) + " entradas.");
    refreshChannels();
    grabKeyboardFocus();
}

// Una sola ventana de afinador: abrirla en otra entrada la cambia.
void MainComponent::showTuner (int channel)
{
    if (tunerWindow != nullptr && tunerWindow->getChannel() == channel)
    {
        tunerWindow->toFront (true);
        return;
    }

    tunerWindow = nullptr;
    juce::Component::SafePointer<MainComponent> safe (this);
    tunerWindow = std::make_unique<TunerWindow> (engine, channel, [safe]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->tunerWindow = nullptr;
        });
    });
}

void MainComponent::removeChannel (int channel)
{
    if (tunerWindow != nullptr && tunerWindow->getChannel() == channel)
        tunerWindow = nullptr;

    closePluginEditor (channel);
    engine.removeChannel (channel);
    refreshChannels();
    grabKeyboardFocus();
}

void MainComponent::refreshChannels()
{
    int number = 0;

    for (int i = 0; i < Engine::maxChannels; ++i)
    {
        auto& row = *channelRows[(size_t) i];
        const bool used = engine.getChannel (i).used;

        if (used)
            row.sync (++number);

        row.setVisible (used);
    }

    resized();
    repaint();
}

void MainComponent::showKeySettings()
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new KeysPanel (engine, [this] { refreshHelp(); }));
    o.dialogTitle = "Teclas";
    o.dialogBackgroundColour = background;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

void MainComponent::performAction (const juce::String& id)
{
    auto& looper = engine.looper;

    if (id == "rec")            looper.toggleRecord();
    else if (id == "play")      looper.togglePlay();
    else if (id == "undo")      looper.undo();
    else if (id == "redo")      looper.redo();
    else if (id == "clear")     looper.clearAll();
    else if (id == "tempoUp")   changeBpm (1.0f);
    else if (id == "tempoDown") changeBpm (-1.0f);
    else if (id.startsWith ("arm")) engine.toggleEntry (id.getTrailingIntValue());
    else if (id == "guitarNext") { engine.nextGuitar(); refreshPresetTitle(); }
    else if (id == "presetNext") pickPreset (engine.library.stepFavorite (1));
    else if (id == "presetPrev") pickPreset (engine.library.stepFavorite (-1));
    else if (id.startsWith ("preset")) pickPreset (engine.library.favorites[id.getTrailingIntValue() - 1]);
    else if (id == "wait")
    {
        looper.setWaitForSound (! looper.isWaitForSound());
        engine.saveSettings();
    }
    else if (id == "tap")       tapTempo();
    else if (id == "metro")
    {
        looper.setMetronome (! looper.isMetronomeOn());
        engine.saveSettings();
    }
}

void MainComponent::pickPreset (const juce::String& path)
{
    if (engine.selectPreset (path))
    {
        presetPanel->showCurrent();
        refreshPresetTitle();
    }
}

void MainComponent::refreshPresetTitle()
{
    auto& current = engine.library.current;
    presetTitle.setText (current.isEmpty() ? juce::String ("PRESETS") : PresetLibrary::describe (current).name,
                         juce::dontSendNotification);
}

void MainComponent::showExportMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Mezcla (un MP3)");
    menu.addItem (2, "Multipista (un MP3 por pista + la mezcla)");

    juce::Component::SafePointer<MainComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton), [safe] (int result)
    {
        if (safe != nullptr && result > 0)
            safe->exportMp3 (result == 2);
    });
}

void MainComponent::exportMp3 (bool separateTracks)
{
    auto& looper = engine.looper;
    const int loops = loopsBox.getSelectedId();
    juce::AudioBuffer<float> mix;

    if (! looper.renderMix (mix, loops))
    {
        exportLabel.setText ("No hay loop para exportar", juce::dontSendNotification);
        return;
    }

    // Que no sature: si pasa de 0 dB se baja todo un poco. Las pistas sueltas se bajan
    // lo mismo, para que sumadas en otro programa den la mezcla.
    const auto peak = mix.getMagnitude (0, mix.getNumSamples());
    const float gain = peak > 0.98f ? 0.98f / peak : 1.0f;
    mix.applyGain (gain);

    // Multipista: una carpeta por exportación, con un archivo por pista.
    const auto base = "AmpBox " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S");
    auto dir = separateTracks ? exportFolder().getChildFile (base) : exportFolder();
    dir.createDirectory();

    juce::Array<juce::File> wavs;
    auto writeWav = [&] (const juce::AudioBuffer<float>& audio, const juce::String& name)
    {
        auto wav = dir.getChildFile (juce::File::createLegalFileName (name) + ".wav");
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream (wav.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> writer;

        if (stream != nullptr)
            writer.reset (format.createWriterFor (stream.get(), looper.getSampleRate(), 2, 24, {}, 0));

        if (writer == nullptr)
        {
            exportLabel.setText ("No se pudo escribir " + wav.getFileName(), juce::dontSendNotification);
            return false;
        }

        stream.release(); // ahora es del writer
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
        wavs.add (wav);
        return true;
    };

    if (! writeWav (mix, separateTracks ? juce::String ("00 Mezcla") : base))
        return;

    if (separateTracks)
    {
        int number = 0;
        for (auto slot : visibleTracks())
        {
            juce::AudioBuffer<float> audio;
            if (! looper.renderTrack (slot, audio, loops))
                continue; // muteada o grabándose

            audio.applyGain (gain);
            if (! writeWav (audio, juce::String (++number).paddedLeft ('0', 2) + " " + trackName (slot)))
                return;
        }
    }

    const auto shown = separateTracks ? dir : wavs[0];
    auto ffmpeg = findFfmpeg();
    if (! ffmpeg.existsAsFile())
    {
        exportFinished (false, shown, wavs.size());
        return;
    }

    exportButton.setEnabled (false);
    exportLabel.setText ("Convirtiendo a MP3...", juce::dontSendNotification);

    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Thread::launch ([safe, ffmpeg, wavs, shown]
    {
        bool ok = true;

        for (auto& wav : wavs)
        {
            const auto mp3 = wav.withFileExtension ("mp3");
            juce::ChildProcess process;
            juce::StringArray args { ffmpeg.getFullPathName(), "-y", "-loglevel", "error", "-i", wav.getFullPathName(),
                                     "-codec:a", "libmp3lame", "-b:a", "320k", mp3.getFullPathName() };

            bool converted = process.start (args);
            if (converted)
            {
                process.readAllProcessOutput();
                converted = process.getExitCode() == 0 && mp3.existsAsFile();
            }

            if (converted)
                wav.deleteFile();

            ok = ok && converted;
        }

        const auto result = ok && ! shown.isDirectory() ? shown.withFileExtension ("mp3") : shown;
        const int count = wavs.size();

        juce::MessageManager::callAsync ([safe, ok, result, count]
        {
            if (safe != nullptr)
                safe->exportFinished (ok, result, count);
        });
    });
}

void MainComponent::exportFinished (bool ok, const juce::File& file, int numFiles)
{
    exportButton.setEnabled (true);

    if (file.isDirectory())
        exportLabel.setText ((ok ? "Guardado en " : "Sin MP3 (ffmpeg fallo), quedaron WAV en ") + file.getFileName()
                               + " (" + juce::String (numFiles) + " archivos)",
                             juce::dontSendNotification);
    else
        exportLabel.setText (ok ? "Guardado: " + file.getFileName()
                                : "Sin MP3 (ffmpeg fallo), quedo el WAV: " + file.getFileName(),
                             juce::dontSendNotification);
}

void MainComponent::changeBpm (float delta)
{
    auto& looper = engine.looper;
    looper.setBpm (std::round (looper.getBpm()) + delta);
}

// Promedio de los últimos toques; si pasan más de 2 s, se empieza de nuevo.
void MainComponent::tapTempo()
{
    auto& looper = engine.looper;
    if (looper.isTempoLocked())
    {
        exportLabel.setText ("Con el loop sonando el tempo queda fijo", juce::dontSendNotification);
        return;
    }

    const double now = juce::Time::getMillisecondCounterHiRes();
    if (! tapTimes.empty() && now - tapTimes.back() > 2000.0)
        tapTimes.clear();

    tapTimes.push_back (now);
    if (tapTimes.size() > 5)
        tapTimes.erase (tapTimes.begin());

    if (tapTimes.size() >= 2)
    {
        const double beatMs = (tapTimes.back() - tapTimes.front()) / (double) (tapTimes.size() - 1);
        looper.setBpm ((float) std::round (60000.0 / beatMs));
    }
}

void MainComponent::refreshHelp()
{
    juce::StringArray parts;

    for (auto& a : Engine::getKeyActions())
    {
        auto key = engine.keys[a.id];
        if (key.isValid())
            parts.add (key.getTextDescription() + ": " + a.name);
    }

    helpLabel.setText (parts.joinIntoString ("   |   ") + "   |   Rueda del mouse: tempo   |   Clic en la barra del loop: ir a esa parte",
                       juce::dontSendNotification);
}

void MainComponent::refreshInputs()
{
    for (auto& row : channelRows)
        if (row->isVisible())
            row->refreshSources();
}

void MainComponent::refreshStatus()
{
    auto* device = engine.deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        statusLabel.setText ("Sin interfaz de audio", juce::dontSendNotification);
        return;
    }

    statusLabel.setText (engine.deviceManager.getCurrentAudioDeviceType() + " - " + device->getName()
                           + "\n" + juce::String (device->getCurrentSampleRate(), 0) + " Hz, buffer "
                           + juce::String (device->getCurrentBufferSizeSamples())
                           + "  |  latencia " + juce::String (engine.getLatencyMs(), 1) + " ms",
                         juce::dontSendNotification);
}

void MainComponent::refreshTransport()
{
    auto& looper = engine.looper;

    const bool recording = looper.isRecording();
    const bool countIn = looper.isCountingIn();
    const bool waiting = looper.isWaiting();
    recButton.setButtonText (waiting ? "ESPERANDO SONIDO..."
                           : countIn ? "CUENTA " + juce::String (looper.getCountInBeat()) + " / 4"
                                     : recording ? "TERMINAR TOMA"
                                     : engine.countArmed() == 0 ? "ARMA UNA ENTRADA"
                                     : looper.hasFreeTrack (engine.countArmed()) ? "REC" : "SIN PISTAS");
    recButton.setColour (juce::TextButton::buttonColourId,
                         recording || countIn ? recRed
                         : waiting ? (juce::Time::getMillisecondCounter() % 600 < 300 ? recRed : recRed.withAlpha (0.35f))
                                   : recRed.withAlpha (0.35f));

    metroButton.setToggleState (looper.isMetronomeOn(), juce::dontSendNotification);
    waitButton.setToggleState (looper.isWaitForSound(), juce::dontSendNotification);
    bpmLabel.setText (juce::String (juce::roundToInt (looper.getBpm())) + " BPM", juce::dontSendNotification);
    bpmLabel.setColour (juce::Label::textColourId, looper.isTempoLocked() ? juce::Colours::white.withAlpha (0.5f)
                                                                          : juce::Colours::white);

    const bool playing = looper.isPlaying();
    playButton.setButtonText (playing ? "STOP" : "PLAY");
    playButton.setColour (juce::TextButton::buttonColourId, playing ? playGreen.withAlpha (0.6f) : panel);
}

void MainComponent::refreshTracks()
{
    // Las filas visibles son las pistas grabando o sonando, en orden de grabación.
    std::vector<int> active;

    for (int i = 0; i < Looper::maxTracks; ++i)
    {
        auto state = engine.looper.getTrack (i).state.load();
        if (state == Looper::State::Recording || state == Looper::State::Playing)
            active.push_back (i);
    }

    std::sort (active.begin(), active.end(), [this] (int a, int b)
    {
        return engine.looper.getTrack (a).order < engine.looper.getTrack (b).order;
    });

    juce::String signature;
    for (auto i : active)
        signature << i << ":" << engine.looper.getTrack (i).order << " ";

    if (signature != trackSignature)
    {
        trackSignature = signature;

        for (auto& row : rows)
            row->setVisible (false);

        for (size_t n = 0; n < active.size(); ++n)
        {
            auto& row = *rows[(size_t) active[n]];
            row.sync ((int) n + 1, trackName (active[n]));
            row.setVisible (true);
        }

        layoutTracks();
    }

    for (size_t i = 0; i < rows.size(); ++i)
    {
        rows[i]->setSeparating ((int) i == separatingSlot);
        rows[i]->setVoice (voiceTracks.canEdit ((int) i), voiceTracks.isRendering ((int) i));

        if (rows[i]->isVisible())
            rows[i]->tick();
    }
}

//==============================================================================
// Separar con Demucs (tools/separar.py, Python del sistema). Tarda ~2 veces el loop.

namespace
{
    juce::File findSeparateScript()
    {
        // build/AmpBox_artefacts/Release/AmpBox.exe -> subir hasta encontrar tools/separar.py
        auto dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

        for (int i = 0; i < 6 && dir.exists(); ++i, dir = dir.getParentDirectory())
            if (auto f = dir.getChildFile ("tools/separar.py"); f.existsAsFile())
                return f;

        return {};
    }
}

void MainComponent::separateTrack (int slot, int stems)
{
    if (separatingSlot >= 0)
    {
        exportLabel.setText ("Ya se esta separando otra pista", juce::dontSendNotification);
        return;
    }

    auto script = findSeparateScript();
    if (! script.existsAsFile())
    {
        exportLabel.setText ("Falta tools\\separar.py", juce::dontSendNotification);
        return;
    }

    auto& looper = engine.looper;
    juce::AudioBuffer<float> audio;

    if (! looper.copyTrack (slot, audio))
    {
        exportLabel.setText ("Esa pista no se puede separar todavia", juce::dontSendNotification);
        return;
    }

    auto folder = Engine::getDataFolder().getChildFile ("separar");
    folder.deleteRecursively();
    folder.createDirectory();
    auto wav = folder.getChildFile ("loop.wav");

    {
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream (wav.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> writer;

        if (stream != nullptr)
            writer.reset (format.createWriterFor (stream.get(), looper.getSampleRate(), 2, 32, {}, 0));

        if (writer == nullptr)
        {
            exportLabel.setText ("No se pudo escribir " + wav.getFullPathName(), juce::dontSendNotification);
            return;
        }

        stream.release(); // ahora es del writer
        writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples());
    }

    juce::StringArray names;
    if (stems & 1) names.add ("drums");
    if (stems & 2) names.add ("bass");

    separatingSlot = slot;
    separatingOrder = looper.getTrack (slot).order;
    const int length = audio.getNumSamples();
    const auto seconds = juce::roundToInt (2.0 * length / looper.getSampleRate()) + 5;
    exportLabel.setText ("Separando con Demucs... (~" + juce::String (seconds) + " s)", juce::dontSendNotification);

    juce::Component::SafePointer<MainComponent> safe (this);
    juce::Thread::launch ([safe, script, wav, folder, names, slot, length, stems]
    {
        juce::ChildProcess process;
        juce::StringArray args { "python", script.getFullPathName(), wav.getFullPathName(),
                                 folder.getFullPathName(), names.joinIntoString (",") };

        bool ok = process.start (args);
        juce::String output;

        if (ok)
        {
            output = process.readAllProcessOutput();
            ok = process.getExitCode() == 0;
        }
        else
        {
            output = "No se pudo ejecutar Python";
        }

        for (auto& n : names)
            ok = ok && folder.getChildFile (n + ".wav").existsAsFile();

        juce::MessageManager::callAsync ([safe, slot, length, stems, ok, output]
        {
            if (safe != nullptr)
                safe->separationFinished (slot, length, stems, ok, output);
        });
    });
}

void MainComponent::separationFinished (int slot, int length, int stems, bool ok, const juce::String& message)
{
    separatingSlot = -1;
    auto& looper = engine.looper;

    if (! ok)
    {
        // Sin Python o sin las librerías: decir cómo arreglarlo.
        if (message == "No se pudo ejecutar Python" || message.containsIgnoreCase ("Python was not found")
            || message.contains ("ModuleNotFoundError")
            || message.contains ("No module named"))
        {
            exportLabel.setText ("SEPARAR no esta instalado: corre INSTALAR-SEPARAR.bat (carpeta de AmpBox)",
                                 juce::dontSendNotification);
            return;
        }

        auto last = juce::StringArray::fromLines (message.trim());
        exportLabel.setText ("Demucs fallo: " + (last.isEmpty() ? juce::String() : last[last.size() - 1]),
                             juce::dontSendNotification);
        return;
    }

    auto folder = Engine::getDataFolder().getChildFile ("separar");
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    // La pista original pudo borrarse (y reusarse) mientras Demucs trabajaba.
    auto& original = looper.getTrack (slot);
    const bool originalStillThere = original.order == separatingOrder
                                 && (original.state == Looper::State::Playing || original.state == Looper::State::Undone);
    const int source = original.source;
    int added = 0;

    for (auto [bit, name, stem] : { std::tuple { 1, "drums", (int) Looper::Drums },
                                    std::tuple { 2, "bass", (int) Looper::Bass } })
    {
        if ((stems & bit) == 0)
            continue;

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (folder.getChildFile (juce::String (name) + ".wav")));
        if (reader == nullptr)
            continue;

        juce::AudioBuffer<float> audio (2, (int) reader->lengthInSamples);
        reader->read (&audio, 0, audio.getNumSamples(), 0, true, true);

        if (looper.addTrack (audio, source, stem, length) >= 0)
            ++added;
    }

    if (added == 0)
    {
        exportLabel.setText ("No se agrego la pista (cambio el loop o no hay pistas libres)", juce::dontSendNotification);
        return;
    }

    // La original queda muteada: se escucha solo lo separado (M para volver a oirla).
    if (originalStillThere)
        original.mute = true;
    trackSignature = {};   // re-sincronizar las filas (mute y nombres)
    exportLabel.setText ("Listo: pista separada agregada", juce::dontSendNotification);
}

//==============================================================================
// Launchkey en modo DAW, como con Ableton:
//   flechas ↑ (CC 106) / ↓ (CC 107): cambian de sesión, de abajo hacia arriba:
//     LOOPS      pads de arriba mute de los loops 1-8, perillas volumen de los loops
//     ENTRADAS   (por defecto) pads de arriba (notas 96-103) armar + OIR de las entradas 1-8,
//                pads de abajo (112-119) abrir/cerrar Editar, perillas volumen de las entradas
//     CONFIGURAR pads de arriba abren la entrada de audio/MIDI, los de abajo el plugin;
//                se elige con las flechas del teclado y el mismo pad confirma
//     AGREGAR    pads de arriba 1-4: + GUITARRA, + AUDIO, + MIDI, + PC
//   Perillas en modo Pan del Launchkey (Shift + Pan, avisa con CC 9 = 3): paneo de los loops
//   Play (CC 115) / Stop (CC 116) / Record (CC 117)
//   flechas de track: izquierda (CC 103) deshacer, derecha (CC 102) rehacer
//   Undo (CC 77): borrar todos los loops; Click (CC 76): metrónomo

namespace
{
    constexpr int topRow = 96, bottomRow = 112;
    constexpr int purple = 49, purpleHalf = 51, red = 5, yellow = 13, yellowHalf = 15,
                  blue = 45, blueHalf = 47, cyan = 37, orange = 9, green = 21;
}

juce::String MainComponent::trackName (int slot)
{
    auto& track = engine.looper.getTrack (slot);
    auto& channel = engine.getChannel (track.source);
    const int stem = track.stem;
    return (channel.used ? channel.name : "Entrada " + juce::String (track.source + 1))
             + (stem == Looper::Drums ? " - bateria" : stem == Looper::Bass ? " - bajo" : "");
}

std::vector<int> MainComponent::visibleTracks() const
{
    // Mismo orden que las filas: pistas grabando o sonando, por orden de grabación.
    std::vector<int> active;
    auto& looper = const_cast<Looper&> (engine.looper);

    for (int i = 0; i < Looper::maxTracks; ++i)
    {
        auto state = looper.getTrack (i).state.load();
        if (state == Looper::State::Recording || state == Looper::State::Playing)
            active.push_back (i);
    }

    std::sort (active.begin(), active.end(), [&looper] (int a, int b)
    {
        return looper.getTrack (a).order < looper.getTrack (b).order;
    });

    return active;
}

void MainComponent::handleDaw (const juce::MidiMessage& m)
{
    auto& looper = engine.looper;

    // Canal 16: el Launchkey avisa cuando cambia el modo de pads (CC 3, borra las luces:
    // hay que repintarlas) o de perillas (CC 9). Las perillas también llegan por el canal 16.
    if (m.isController() && m.getChannel() == 16)
    {
        if (m.getControllerNumber() == 3)
            dawLights.fill (-1);

        if (m.getControllerNumber() == 9)
        {
            dawPan = m.getControllerValue() == 3;
            engine.logText ("launchkey: perillas en modo " + juce::String (m.getControllerValue()));
            repaint();
        }

        if (m.getControllerNumber() == 3 || m.getControllerNumber() == 9)
            return;
    }

    if (m.isNoteOn())
    {
        dawPad (m.getNoteNumber());
        return;
    }

    if (! m.isController())
        return;

    const int cc = m.getControllerNumber(), value = m.getControllerValue();

    if (cc >= 21 && cc <= 28 && (dawPage == pageLoops || dawPan))
    {
        // Loops: volumen (100 = 0 dB) o paneo (64 = centro).
        auto tracks = visibleTracks();
        const auto k = (size_t) (cc - 21);
        if (k < tracks.size())
        {
            auto& t = looper.getTrack (tracks[k]);
            if (dawPan)
                t.pan = juce::jlimit (-1.0f, 1.0f, (value - 64) / 63.0f);
            else
                t.gain = value / 100.0f;
        }
    }
    else if (cc >= 21 && cc <= 28)
    {
        // Volumen de la entrada N (mismo orden que los pads de arriba). 100 = 0 dB, 127 = +2 dB.
        int n = cc - 21;
        for (int i = 0; i < Engine::maxChannels; ++i)
        {
            auto& c = engine.getChannel (i);
            if (c.used && n-- == 0)
            {
                c.volume = value / 100.0f;
                break;
            }
        }
    }
    else if (cc == 115 && value > 0)        // Play: solo arranca
    {
        if (! looper.isPlaying())
            performAction ("play");
    }
    else if (cc == 116 && value > 0)        // Stop
    {
        if (looper.isPlaying())
            performAction ("play");
    }
    else if ((cc == 106 || cc == 107) && value > 0)   // flechas ↑ / ↓: sesión
    {
        dawPage = juce::jlimit ((int) pageLoops, (int) pageAdd, dawPage + (cc == 106 ? 1 : -1));
        dawLights.fill (-1);
        refreshDawLights();
        repaint();
    }
    else if (cc == 77 && value > 0)         // Undo: borra todos los loops
    {
        performAction ("clear");
    }
    else if (cc == 76 && value > 0)         // Click: metrónomo
    {
        performAction ("metro");
    }
    else if (cc == 103 && value > 0)        // flecha de track izquierda
    {
        performAction ("undo");
    }
    else if (cc == 102 && value > 0)        // flecha de track derecha
    {
        performAction ("redo");
    }
    else if (cc == 117 && value > 0)
    {
        performAction ("rec");
    }
    else if (value > 0)
    {
        // Botón sin uso todavía: queda en el log para saber qué manda cada uno.
        engine.logText ("launchkey: CC " + juce::String (cc) + " canal " + juce::String (m.getChannel()));
    }
}

ChannelRow* MainComponent::nthChannelRow (int n)
{
    for (int i = 0; i < Engine::maxChannels; ++i)
        if (engine.getChannel (i).used && n-- == 0)
            return channelRows[(size_t) i].get();

    return nullptr;
}

void MainComponent::dawPad (int note)
{
    const bool top = note >= topRow && note < topRow + 8;
    const bool bottom = note >= bottomRow && note < bottomRow + 8;
    if (! top && ! bottom)
        return;

    const int k = note - (top ? topRow : bottomRow);

    // Un menú abierto desde un pad: el mismo pad elige lo marcado; otro pad lo cierra.
    if (auto* modal = juce::Component::getCurrentlyModalComponent(); modal != nullptr && dawMenuNote >= 0)
    {
        const bool same = note == dawMenuNote;
        dawMenuNote = -1;

        if (same)
        {
            modal->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
            return;
        }

        juce::PopupMenu::dismissAllActiveMenus();
    }

    auto& looper = engine.looper;

    switch (dawPage)
    {
        case pageLoops:
            if (top)
            {
                auto tracks = visibleTracks();
                if ((size_t) k < tracks.size())
                {
                    auto& t = looper.getTrack (tracks[(size_t) k]);
                    t.mute = ! t.mute;
                }
            }
            break;

        case pageInputs:
            if (top)
            {
                engine.toggleEntry (k + 1);   // armar y OIR juntos
            }
            else if (auto* row = nthChannelRow (k))
            {
                const int channel = (int) (std::find_if (channelRows.begin(), channelRows.end(),
                                                         [row] (auto& r) { return r.get() == row; }) - channelRows.begin());
                if (editorWindows.count (channel) > 0)
                    closePluginEditor (channel);
                else
                    showPluginEditor (channel);
            }
            break;

        case pageConfig:
            if (auto* row = nthChannelRow (k))
            {
                getTopLevelComponent()->toFront (true);   // para que las flechas del teclado lleguen al menú
                dawMenuNote = note;
                if (top)
                    row->openSourceMenu();
                else
                    row->openPluginMenu();
            }
            break;

        case pageAdd:
            if (top && k < 4)
            {
                juce::Button* buttons[] = { &addGuitarButton, &addAudioButton, &addMidiButton, &addPcButton };
                if (buttons[k]->isEnabled())
                    buttons[k]->triggerClick();
            }
            break;

        default:
            break;
    }
}

void MainComponent::refreshDawLights()
{
    if (! engine.isDawConnected())
        return;

    auto& looper = engine.looper;
    std::array<int, 18> wanted {};

    auto tracks = visibleTracks();

    if (dawPage == pageLoops)
    {
        for (size_t k = 0; k < 8 && k < tracks.size(); ++k)
            wanted[k] = looper.getTrack (tracks[k]).mute ? yellowHalf : yellow;
    }
    else if (dawPage == pageAdd)
    {
        juce::Button* buttons[] = { &addGuitarButton, &addAudioButton, &addMidiButton, &addPcButton };
        for (size_t k = 0; k < 4; ++k)
            wanted[k] = buttons[k]->isEnabled() ? green : 0;
    }
    else
    {
        // ENTRADAS / CONFIGURAR: una columna por entrada, en el orden visible.
        int n = 0;
        for (int i = 0; i < Engine::maxChannels && n < 8; ++i)
        {
            auto& c = engine.getChannel (i);
            if (! c.used)
                continue;

            if (dawPage == pageInputs)
            {
                wanted[(size_t) n] = c.armed ? (looper.isRecording() ? red : purple) : purpleHalf;
                wanted[(size_t) n + 8] = editorWindows.count (i) > 0 ? blue : blueHalf;
            }
            else
            {
                wanted[(size_t) n] = cyan;
                wanted[(size_t) n + 8] = orange;
            }

            ++n;
        }
    }

    wanted[16] = looper.isPlaying() ? 127 : 0;
    wanted[17] = looper.isRecording() || looper.isWaiting() || looper.isCountingIn() ? 127 : 0;

    for (size_t i = 0; i < wanted.size(); ++i)
    {
        if (wanted[i] == dawLights[i])
            continue;

        dawLights[i] = wanted[i];

        if (i < 16)
            engine.sendDaw (juce::MidiMessage::noteOn (1, (int) (i < 8 ? topRow + i : bottomRow + i - 8), (juce::uint8) wanted[i]));
        else
            engine.sendDaw (juce::MidiMessage::controllerEvent (1, i == 16 ? 115 : 117, wanted[i]));
    }
}

void MainComponent::layoutTracks()
{
    // Filas visibles en orden de grabación.
    std::vector<int> order;
    for (int i = 0; i < Looper::maxTracks; ++i)
        if (rows[(size_t) i]->isVisible())
            order.push_back (i);

    std::sort (order.begin(), order.end(), [this] (int a, int b)
    {
        return engine.looper.getTrack (a).order < engine.looper.getTrack (b).order;
    });

    auto r = tracksArea;
    for (auto i : order)
    {
        rows[(size_t) i]->setBounds (r.removeFromTop (38));
        r.removeFromTop (4);
    }
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshInputs();
    refreshStatus();
}

void MainComponent::timerCallback()
{
    engine.looper.maintain (engine.countArmed());
    VoiceChain::bpm = engine.looper.getBpm();   // delay de la voz al tempo

    // Cada 2 s: teclados MIDI conectados después de abrir, y la Audient
    // (si se enchufa o se desenchufa, cambia de interfaz sola).
    if (++midiCheckTicks >= 60)
    {
        midiCheckTicks = 0;
        if (engine.checkMidiDevices())
            refreshInputs();

        if (engine.checkAudioDevice())
        {
            refreshChannels();
            refreshStatus();
        }
    }

    for (auto& row : channelRows)
        if (row->isVisible())
            row->tick();

    // Caída suave de los medidores.
    inLevel = juce::jmax (engine.takeInputPeak(), inLevel * 0.85f);
    outLevel = juce::jmax (engine.takeOutputPeak(), outLevel * 0.85f);

    refreshTransport();
    refreshTracks();
    refreshDawLights();
    repaint (inMeter.getUnion (outMeter).getUnion (loopBar).expanded (2));
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    for (auto& [id, assigned] : engine.keys)
    {
        if (assigned.isValid() && assigned == key)
        {
            performAction (id);
            return true;
        }
    }

    return false;
}

void MainComponent::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY != 0.0f)
        changeBpm (wheel.deltaY > 0.0f ? 1.0f : -1.0f);
}

// Barra del loop: clic o arrastre para ir a esa parte (como en un reproductor).
void MainComponent::mouseMove (const juce::MouseEvent& e)
{
    const bool overBar = loopBar.contains (e.getPosition()) && engine.looper.getLoopSeconds() > 0.0;
    setMouseCursor (overBar ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void MainComponent::mouseDown (const juce::MouseEvent& e)
{
    seeking = loopBar.contains (e.getPosition()) && engine.looper.getLoopSeconds() > 0.0;
    if (seeking)
        seekTo (e);
}

void MainComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (seeking)
        seekTo (e);
}

void MainComponent::mouseUp (const juce::MouseEvent&)
{
    seeking = false;
}

void MainComponent::seekTo (const juce::MouseEvent& e)
{
    if (engine.looper.isRecording())
    {
        exportLabel.setText ("Grabando no se puede mover el loop", juce::dontSendNotification);
        return;
    }

    engine.looper.seek ((e.position.x - (float) loopBar.getX()) / (float) juce::jmax (1, loopBar.getWidth()));
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    paintLaunchkeyFrame (g);

    if (loadingTitle.isEmpty())
        return;

    if (loadingWork != nullptr)
        runLoadingWork();

    // Aviso de carga: la ventana no responde mientras se abre un plugin.
    g.fillAll (background.withAlpha (0.82f));

    auto card = getLocalBounds().withSizeKeepingCentre (520, 150).toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (card, 10.0f);
    g.setColour (accent);
    g.drawRoundedRectangle (card.reduced (0.5f), 10.0f, 1.5f);

    // Reloj de arena dibujado (estático: el hilo de la ventana está ocupado).
    auto icon = card.removeFromLeft (90.0f).withSizeKeepingCentre (34.0f, 44.0f);
    juce::Path glass;
    glass.startNewSubPath (icon.getTopLeft());
    glass.lineTo (icon.getTopRight());
    glass.lineTo (icon.getCentre());
    glass.lineTo (icon.getBottomRight());
    glass.lineTo (icon.getBottomLeft());
    glass.lineTo (icon.getCentre());
    glass.closeSubPath();
    g.strokePath (glass, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.fillRect (icon.withTop (icon.getBottom() - 10.0f).reduced (8.0f, 2.0f));

    auto text = card.reduced (0.0f, 22.0f).withTrimmedRight (20.0f);
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (20.0f, juce::Font::bold));
    g.drawText (loadingTitle + "...", text.removeFromTop (30.0f), juce::Justification::centredLeft, true);
    g.setColour (juce::Colours::white.withAlpha (0.65f));
    g.setFont (juce::FontOptions (13.5f));
    g.drawFittedText (loadingDetail + "\nSi Windows dice \"No responde\", es normal: espera.",
                      text.toNearestInt(), juce::Justification::topLeft, 4);
}

// Marco naranja en la zona que maneja el Launchkey (ENTRADAS o LOOPS).
void MainComponent::paintLaunchkeyFrame (juce::Graphics& g)
{
    if (! engine.isDawConnected())
        return;

    auto area = tracksArea;
    if (dawPage == pageInputs)
    {
        area = channelsTitle.getBounds().withRight (addPcButton.getRight());
        for (auto& row : channelRows)
            if (row->isVisible())
                area = area.getUnion (row->getBounds());
    }
    else if (dawPage == pageConfig)
    {
        area = {};
        for (auto& row : channelRows)
            if (row->isVisible())
                area = area.isEmpty() ? row->configBounds() : area.getUnion (row->configBounds());
    }
    else if (dawPage == pageAdd)
    {
        area = addGuitarButton.getBounds().getUnion (addPcButton.getBounds());
    }

    const auto box = area.expanded (4).toFloat();
    g.setColour (accent);
    g.drawRoundedRectangle (box, 6.0f, 2.0f);

    const char* names[] = { "LAUNCHKEY: LOOPS", "LAUNCHKEY: ENTRADAS", "LAUNCHKEY: CONFIGURAR", "LAUNCHKEY: AGREGAR" };
    const juce::String text = juce::String (names[dawPage])
                              + (dawPan ? " (PANEO)" : "");
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    auto tag = juce::Rectangle<float> (box.getRight() - 190.0f, box.getBottom() - 8.0f, 180.0f, 16.0f);
    if (dawPage == pageAdd)   // los botones están arriba a la derecha: la etiqueta va debajo
        tag = tag.withY (box.getBottom() + 2.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (tag, 4.0f);
    g.setColour (juce::Colours::black);
    g.drawText (text, tag, juce::Justification::centred);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (background);

    auto drawMeter = [&] (juce::Rectangle<int> r, float level, const juce::String& name)
    {
        g.setColour (panel);
        g.fillRect (r);
        auto db = juce::Decibels::gainToDecibels (level, -60.0f);
        auto w = juce::jmap (db, -60.0f, 0.0f, 0.0f, (float) r.getWidth());
        g.setColour (level >= 0.99f ? juce::Colours::red : accent);
        g.fillRect (r.withWidth ((int) w));
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawText (name, r.reduced (6, 0), juce::Justification::centredLeft);
    };

    drawMeter (inMeter, inLevel, "IN");
    drawMeter (outMeter, outLevel, "OUT");

    // Sin entradas: explicar cómo empezar.
    if (! emptyHint.isEmpty())
    {
        const auto box = emptyHint.toFloat();
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawRoundedRectangle (box.reduced (1.0f), 6.0f, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText ("Sin entradas", emptyHint.withTrimmedBottom (emptyHint.getHeight() / 2), juce::Justification::centredBottom);
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Agrega lo que vas a tocar con + GUITARRA, + AUDIO, + MIDI o + PC (arriba a la derecha)",
                    emptyHint.withTrimmedTop (emptyHint.getHeight() / 2 + 4), juce::Justification::centredTop);
    }

    // Avance del loop base.
    auto& looper = engine.looper;
    // Barra del loop en azul (el verde es de medidores y Play), con pulsos y cursor.
    const auto bar = loopBar.toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (bar, 5.0f);

    if (looper.getLoopSeconds() > 0.0)
    {
        const bool rec = looper.isRecording();
        const auto dark = rec ? recRed.darker (0.3f) : loopBlue;
        const auto light = rec ? recRed.brighter (0.4f) : loopBlueLight;
        const float x = bar.getX() + (float) looper.getProgress() * bar.getWidth();

        g.saveState();
        g.reduceClipRegion (loopBar.withRight ((int) x));
        g.setGradientFill (juce::ColourGradient (dark, bar.getX(), 0.0f, light, x, 0.0f, false));
        g.fillRoundedRectangle (bar, 5.0f);
        g.restoreState();

        // Marcas de pulso (y de compás más fuertes) si el loop cae en pulsos enteros.
        const double beats = looper.getLoopSeconds() * looper.getBpm() / 60.0;
        const int numBeats = juce::roundToInt (beats);
        int beatNow = -1;
        if (numBeats >= 2 && numBeats <= 64 && std::abs (beats - numBeats) < 0.05)
        {
            beatNow = (int) (looper.getProgress() * numBeats);
            for (int b = 1; b < numBeats; ++b)
            {
                const bool barLine = b % Looper::beatsPerBar == 0;
                const float bx = bar.getX() + bar.getWidth() * (float) b / (float) numBeats;
                g.setColour (juce::Colours::white.withAlpha (barLine ? 0.45f : 0.15f));
                g.fillRect (bx, bar.getY() + (barLine ? 2.0f : 7.0f), barLine ? 2.0f : 1.0f,
                            bar.getHeight() - (barLine ? 4.0f : 14.0f));
            }
        }

        // Cursor: línea clara con brillo.
        g.setColour (light.withAlpha (0.35f));
        g.fillRect (x - 4.0f, bar.getY(), 8.0f, bar.getHeight());
        g.setColour (juce::Colours::white);
        g.fillRect (x - 1.0f, bar.getY(), 3.0f, bar.getHeight());

        g.setColour (light.withAlpha (0.9f));
        g.drawRoundedRectangle (bar.reduced (0.5f), 5.0f, 1.0f);

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText ("LOOP  " + juce::String (looper.getLoopSeconds(), 2) + " s", loopBar.reduced (10, 0),
                    juce::Justification::centredLeft);
        if (beatNow >= 0)
            g.drawText ("Compás " + juce::String (beatNow / Looper::beatsPerBar + 1) + "/"
                            + juce::String ((numBeats + Looper::beatsPerBar - 1) / Looper::beatsPerBar)
                            + "   Pulso " + juce::String (beatNow % Looper::beatsPerBar + 1),
                        loopBar.reduced (10, 0), juce::Justification::centredRight);
    }
    else
    {
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.drawText (looper.isRecording() ? "Grabando la toma base..." : "Sin loop: la primera toma fija el largo",
                    loopBar.reduced (8, 0), juce::Justification::centredLeft);
    }
}

void MainComponent::resized()
{
    // Compacto: tiene que entrar en una pantalla de 1366x768.
    auto r = getLocalBounds().reduced (12);

    // Sin columna de presets ni "Abrir AmpliTube": el amplificador y su preset se eligen
    // en cada entrada (Editar). Las teclas Ctrl+1-9 siguen cargando los favoritos.
    for (auto* c : std::initializer_list<juce::Component*> { presetPanel.get(), &presetTitle, &presetsToggle })
        c->setVisible (false);

    auto top = r.removeFromTop (32);
    audioButton.setBounds (top.removeFromLeft (100));
    top.removeFromLeft (8);
    reloadButton.setBounds (top.removeFromLeft (36));
    top.removeFromLeft (16);
    pluginLabel.setBounds (top.removeFromRight (360));
    statusLabel.setBounds (top);

    r.removeFromTop (10);

    auto meters = r.removeFromTop (14);
    inMeter = meters.removeFromLeft (meters.getWidth() / 2 - 4);
    meters.removeFromLeft (8);
    outMeter = meters;

    // Entradas
    r.removeFromTop (8);
    auto channelsHeader = r.removeFromTop (24);
    channelsTitle.setBounds (channelsHeader.removeFromLeft (110));
    addPcButton.setBounds (channelsHeader.removeFromRight (70));
    channelsHeader.removeFromRight (6);
    addMidiButton.setBounds (channelsHeader.removeFromRight (90));
    channelsHeader.removeFromRight (6);
    addAudioButton.setBounds (channelsHeader.removeFromRight (90));
    channelsHeader.removeFromRight (6);
    addGuitarButton.setBounds (channelsHeader.removeFromRight (110));
    r.removeFromTop (4);

    emptyHint = {};
    for (auto& row : channelRows)
    {
        if (! row->isVisible())
            continue;

        row->setBounds (r.removeFromTop (40));
        r.removeFromTop (4);
    }

    if (engine.countChannels() == 0)
    {
        emptyHint = r.removeFromTop (64);
        r.removeFromTop (4);
    }

    r.removeFromTop (8);
    auto transport = r.removeFromTop (40);
    recButton.setBounds (transport.removeFromLeft (170));
    transport.removeFromLeft (8);
    playButton.setBounds (transport.removeFromLeft (110));
    transport.removeFromLeft (8);
    undoButton.setBounds (transport.removeFromLeft (100));
    transport.removeFromLeft (8);
    redoButton.setBounds (transport.removeFromLeft (100));
    transport.removeFromLeft (8);
    clearButton.setBounds (transport.removeFromLeft (48));
    transport.removeFromLeft (8);
    waitButton.setBounds (transport.removeFromLeft (130));

    // Tempo y exportar en una fila.
    r.removeFromTop (8);
    auto tempo = r.removeFromTop (30);
    metroButton.setBounds (tempo.removeFromLeft (120));
    tempo.removeFromLeft (4);
    bpmLabel.setBounds (tempo.removeFromLeft (90));
    tapButton.setBounds (tempo.removeFromLeft (50));
    tempo.removeFromLeft (12);
    keysButton.setBounds (tempo.removeFromRight (90));
    tempo.removeFromRight (8);
    loopsBox.setBounds (tempo.removeFromLeft (105));
    tempo.removeFromLeft (6);
    exportButton.setBounds (tempo.removeFromLeft (130));
    tempo.removeFromLeft (6);
    folderButton.setBounds (tempo.removeFromLeft (34).reduced (2));
    tempo.removeFromLeft (8);
    exportLabel.setBounds (tempo);

    r.removeFromTop (8);
    loopBar = r.removeFromTop (28);

    helpLabel.setBounds (r.removeFromBottom (34));

    r.removeFromTop (6);
    tracksArea = r;
    layoutTracks();
}
