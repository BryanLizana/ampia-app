#pragma once
#include "Engine.h"
#include "VoiceTracks.h"
#include "Tuner.h"

class TrackRow;
class ChannelRow;
class PresetPanel;

class MainComponent : public juce::Component,
                      private juce::ChangeListener,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void showLoading (const juce::String& title, const juce::String& detail, std::function<void()> work);
    void restoreNextPlugin();
    void paintLaunchkeyFrame (juce::Graphics&);
    void showAudioSettings();
    void showPluginEditor (int channel);
    void closePluginEditor (int channel);
    void addChannel (Engine::ChannelType);
    void removeChannel (int channel);
    void refreshChannels();
    void showKeySettings();
    void performAction (const juce::String& id);
    void changeBpm (float delta);
    void tapTempo();
    void checkAmpliTubes();
    bool ampliTubeWarned = false;
    void seekTo (const juce::MouseEvent&);
    void refreshHelp();
    void pickPreset (const juce::String& path);
    void refreshPresetTitle();
    void showExportMenu();
    void exportMp3 (bool separateTracks);
    void exportFinished (bool ok, const juce::File& file, int numFiles);
    juce::String trackName (int slot);
    void refreshInputs();
    void refreshStatus();
    void refreshTransport();
    void refreshTracks();
    void handleDaw (const juce::MidiMessage&);
    void refreshDawLights();
    void dawPad (int note);
    ChannelRow* nthChannelRow (int n);   // n-ésima entrada visible (0 = la primera)
    std::vector<int> visibleTracks() const;
    void separateTrack (int slot, int stems);   // bits: 1 = batería, 2 = bajo
    void separationFinished (int slot, int length, int stems, bool ok, const juce::String& message);
    void layoutTracks();

    Engine engine;
    VoiceTracks voiceTracks { engine };   // después del motor: se destruye antes
    std::unique_ptr<TunerWindow> tunerWindow;
    // Sin esto no se ven los tooltips. En el escritorio: sirve también en las otras ventanas.
    juce::TooltipWindow tooltipWindow { nullptr, 600 };
    void showTuner (int channel);

    juce::TextButton audioButton { "Audio..." },
                     reloadButton { juce::String (juce::CharPointer_UTF8 ("\xe2\x86\xbb")) }; // recargar
    juce::Label statusLabel, pluginLabel;

    juce::Label channelsTitle { {}, "ENTRADAS" };
    juce::TextButton addGuitarButton { "+ GUITARRA" }, addAudioButton { "+ AUDIO" }, addMidiButton { "+ MIDI" },
                     addPcButton { "+ PC" };
    std::vector<std::unique_ptr<ChannelRow>> channelRows;
    juce::Rectangle<int> channelsArea;

    juce::TextButton recButton { "REC" }, playButton { "PLAY" }, undoButton { "DESHACER" },
                     redoButton { "REHACER" },
                     metroButton { "METRONOMO" }, keysButton { "Teclas..." },
                     waitButton { "REC AL TOCAR" }, tapButton { "TAP" };
    juce::Label bpmLabel, helpLabel;

    std::unique_ptr<PresetPanel> presetPanel;
    juce::Label presetTitle { {}, "PRESETS" };
    juce::TextButton presetsToggle { juce::String (juce::CharPointer_UTF8 ("\xe2\x98\xb0")) };   // ☰

    juce::TextButton exportButton { "EXPORTAR MP3" };
    juce::DrawableButton folderButton { "Carpeta", juce::DrawableButton::ImageFitted };
    juce::DrawableButton clearButton { "Borrar todo", juce::DrawableButton::ImageOnButtonBackground };
    juce::ComboBox loopsBox;
    juce::Label exportLabel;

    std::vector<std::unique_ptr<TrackRow>> rows;
    juce::String trackSignature;

    float inLevel = 0.0f, outLevel = 0.0f;
    std::vector<double> tapTimes;         // últimos toques de TAP (ms)
    bool seeking = false;                 // arrastrando el cursor de la barra del loop
    int midiCheckTicks = 0;
    int separatingSlot = -1;              // pista que Demucs está separando
    int separatingOrder = 0;              // su toma (si se borra y se reusa la pista, no es la misma)
    std::array<int, 18> dawLights;        // últimos colores enviados (16 pads + play + rec)
    enum DawPage { pageLoops, pageInputs, pageConfig, pageAdd };
    int dawPage = pageInputs;             // sesión del Launchkey (flechas ↑ / ↓)
    int dawMenuNote = -1;                 // pad que abrió el menú que está abierto
    bool dawPan = false;                  // perillas del Launchkey en modo Pan
    juce::Rectangle<int> inMeter, outMeter, loopBar, tracksArea, emptyHint;

    juce::String loadingTitle, loadingDetail;   // aviso "Cargando..." (vacío = no hay)
    int loadingToken = 0;
    std::function<void()> loadingWork;          // lo que se hace cuando el aviso ya se ve
    void runLoadingWork();
    std::vector<int> pluginQueue;               // plugins de la sesión anterior por cargar
    int pluginTotal = 0;
    juce::StringArray pluginFailures;

    std::map<int, std::unique_ptr<juce::DocumentWindow>> editorWindows;   // por canal (0 = AmpliTube)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
