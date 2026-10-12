#include "Engine.h"
#include "AmpliTubePreset.h"
#include "DrumKit.h"
#include "VoiceChain.h"

namespace
{
    juce::File settingsFile() { return Engine::getDataFolder().getChildFile ("ampbox.xml"); }
    juce::File channelStateFile (int index) { return Engine::getDataFolder().getChildFile ("canal-" + juce::String (index) + ".bin"); }

    void log (const juce::String& text)
    {
        Engine::getDataFolder().getChildFile ("ampbox.log")
            .appendText (juce::Time::getCurrentTime().toString (true, true) + "  " + text + "\n");
    }

    float peakOf (const float* data, int n)
    {
        float p = 0.0f;
        for (int i = 0; i < n; ++i)
            p = juce::jmax (p, std::abs (data[i]));
        return p;
    }

    // Señal de entrada (sin efectos) que cuenta como "empezó a tocar": -48 dB.
    // Una guitarra directa a la interfaz con poca ganancia ronda -40..-25 dB; el
    // ruido de fondo de la Audient queda bastante más abajo.
    constexpr float onsetThreshold = 0.004f;

    void keepMax (std::atomic<float>& target, float value)
    {
        auto prev = target.load();
        while (value > prev && ! target.compare_exchange_weak (prev, value)) {}
    }

    juce::String typeId (Engine::ChannelType t)
    {
        return t == Engine::ChannelType::Guitar ? "guitar" : t == Engine::ChannelType::Midi ? "midi"
             : t == Engine::ChannelType::Pc ? "pc" : "audio";
    }

    Engine::ChannelType typeFromId (const juce::String& id)
    {
        return id == "guitar" ? Engine::ChannelType::Guitar : id == "midi" ? Engine::ChannelType::Midi
             : id == "pc" ? Engine::ChannelType::Pc : Engine::ChannelType::Audio;
    }
}

Engine::Engine()
{
    formatManager.addDefaultFormats();
    resetKeys();

    // Arranca sin entradas: cada uno agrega las que usa (+ GUITARRA, + AUDIO...).
}

const std::vector<Engine::KeyAction>& Engine::getKeyActions()
{
    using KP = juce::KeyPress;

    static const std::vector<KeyAction> actions
    {
        { "rec",       "Grabar / terminar toma", KP (KP::spaceKey) },
        { "play",      "Play / stop",            KP (KP::returnKey) },
        { "undo",      "Deshacer",               KP (KP::leftKey) },
        { "redo",      "Rehacer",                KP (KP::rightKey) },
        { "metro",     "Metronomo on/off",       KP ('m') },
        { "tap",       "Tap tempo",              KP ('t') },
        { "tempoUp",   "Tempo +1",               KP (KP::upKey) },
        { "tempoDown", "Tempo -1",               KP (KP::downKey) },
        { "clear",     "Borrar todo",            KP (KP::deleteKey) },
        { "guitarNext", "Siguiente guitarra",    KP (KP::tabKey) },
        { "arm1",      "Entrada 1 (armar + OIR)", KP ('1') },
        { "arm2",      "Entrada 2 (armar + OIR)", KP ('2') },
        { "arm3",      "Entrada 3 (armar + OIR)", KP ('3') },
        { "arm4",      "Entrada 4 (armar + OIR)", KP ('4') },
        { "arm5",      "Entrada 5 (armar + OIR)", KP ('5') },
        { "arm6",      "Entrada 6 (armar + OIR)", KP ('6') },
        { "arm7",      "Entrada 7 (armar + OIR)", KP ('7') },
        { "arm8",      "Entrada 8 (armar + OIR)", KP ('8') },
        { "presetPrev", "Preset anterior",       KP (KP::pageUpKey) },
        { "presetNext", "Preset siguiente",      KP (KP::pageDownKey) },
        { "preset1",   "Preset 1",               KP ('1', juce::ModifierKeys::ctrlModifier, 0) },
        { "preset2",   "Preset 2",               KP ('2', juce::ModifierKeys::ctrlModifier, 0) },
        { "preset3",   "Preset 3",               KP ('3', juce::ModifierKeys::ctrlModifier, 0) },
        { "preset4",   "Preset 4",               KP ('4', juce::ModifierKeys::ctrlModifier, 0) },
        { "preset5",   "Preset 5",               KP ('5', juce::ModifierKeys::ctrlModifier, 0) },
        { "preset6",   "Preset 6",               KP ('6', juce::ModifierKeys::ctrlModifier, 0) },
        { "preset7",   "Preset 7",               KP ('7', juce::ModifierKeys::ctrlModifier, 0) },
        { "preset8",   "Preset 8",               KP ('8', juce::ModifierKeys::ctrlModifier, 0) },
        { "preset9",   "Preset 9",               KP ('9', juce::ModifierKeys::ctrlModifier, 0) },
    };

    return actions;
}

bool Engine::selectPreset (const juce::String& path)
{
    if (path.isEmpty() || ! loadPresetFile (PresetLibrary::getFolder().getChildFile (path)))
        return false;

    library.current = path;
    saveSettings();
    return true;
}

// Los presets van al AmpliTube de la guitarra activa (o al de otra guitarra).
juce::AudioPluginInstance* Engine::getPresetTarget() const
{
    auto isAmpliTube = [] (const Channel& c)
    {
        return c.used && c.type == ChannelType::Guitar && c.plugin != nullptr
            && c.plugin->getName().containsIgnoreCase ("AmpliTube");
    };

    if (const int a = activeGuitar; juce::isPositiveAndBelow (a, maxChannels) && isAmpliTube (channels[(size_t) a]))
        return channels[(size_t) a].plugin.get();

    for (auto& c : channels)
        if (isAmpliTube (c))
            return c.plugin.get();

    return nullptr;
}

bool Engine::loadPresetFile (const juce::File& file)
{
    auto* target = getPresetTarget();

    if (target == nullptr)
        return false;

    if (! file.existsAsFile())
    {
        log ("preset: no existe " + file.getFullPathName());
        return false;
    }

    juce::MemoryBlock current, next;
    juce::String error;
    target->getStateInformation (current);

    if (! AmpliTubePreset::applyToState (current, file, next, error))
    {
        log ("preset " + file.getFileName() + ": " + error);
        return false;
    }

    const auto start = juce::Time::getMillisecondCounterHiRes();
    target->setStateInformation (next.getData(), (int) next.getSize());
    const auto ms = juce::Time::getMillisecondCounterHiRes() - start;

    juce::MemoryBlock check;
    target->getStateInformation (check);
    log ("preset " + file.getFileName() + ": " + juce::String (juce::roundToInt (ms)) + " ms, AmpliTube tiene ahora: "
         + AmpliTubePreset::getPresetName (check));
    return true;
}

void Engine::resetKeys()
{
    keys.clear();
    for (auto& a : getKeyActions())
        keys[a.id] = a.defaultKey;
}

Engine::~Engine()
{
    stop();
}

juce::File Engine::getDataFolder()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("AmpBox");
    dir.createDirectory();
    return dir;
}

void Engine::start()
{
    started = true;
    auto saved = juce::parseXML (settingsFile());
    auto* devState = saved != nullptr ? saved->getChildByName ("DEVICESETUP") : nullptr;

    if (saved != nullptr)
    {
        looper.setBpm ((float) saved->getDoubleAttribute ("bpm", 100.0));
        looper.setMetronome (saved->getBoolAttribute ("metronome", false));
        looper.setWaitForSound (saved->getBoolAttribute ("waitForSound", true));
        presetsHidden = saved->getBoolAttribute ("presetsHidden", false);
        preferredInterface = saved->getStringAttribute ("preferredInterface", preferredInterface);
        activeGuitar = juce::jlimit (-1, maxChannels - 1, saved->getIntAttribute ("activeGuitar", -1));

        // Hasta la versión 2 la entrada 0 era siempre la guitarra con AmpliTube (fija).
        // Desde la 3 se arranca vacío: esa entrada vieja no se restaura.
        const bool fixedGuitar = saved->getIntAttribute ("channelsVersion", 2) < 3;

        // Teclas guardadas. Las de antes de la versión 2 (1-9 = presets, F1-F4 = armar)
        // se dejan con los valores nuevos: 1-8 = entradas, Ctrl+1-9 = presets.
        const bool oldKeys = saved->getIntAttribute ("keysVersion", 1) < 2;

        if (auto* keyList = saved->getChildByName ("KEYS"))
        {
            for (auto* k : keyList->getChildIterator())
            {
                const auto action = k->getStringAttribute ("action");
                if (oldKeys && (action.startsWith ("arm") || action.startsWith ("preset1") || action.matchesWildcard ("preset?", true)))
                    continue;

                keys[action] = juce::KeyPress::createFromDescription (k->getStringAttribute ("key"));
            }
        }

        // Los plugins de los canales se abren después (restoreChannelPlugins).
        if (auto* list = saved->getChildByName ("CHANNELS"))
        {
            for (auto* e : list->getChildIterator())
            {
                const int i = e->getIntAttribute ("index", -1);
                if (i < 0 || i >= maxChannels)
                    continue;

                if (fixedGuitar && i == 0)
                    continue;

                auto& c = channels[(size_t) i];
                c.type = typeFromId (e->getStringAttribute ("type"));

                // La entrada PC no se restaura: al arrancar podría captarse a sí misma.
                if (c.type == ChannelType::Pc)
                    continue;

                c.name = e->getStringAttribute ("name", c.name);
                c.input = e->getIntAttribute ("input", c.input);
                c.midiDevice = e->getStringAttribute ("midi");
                c.pluginFile = juce::File (e->getStringAttribute ("plugin"));
                c.armed = e->getBoolAttribute ("armed", c.armed);
                c.monitor = e->getBoolAttribute ("monitor", true);
                c.volume = (float) e->getDoubleAttribute ("volume", 1.0);
                c.used = true;
            }
        }
    }

    // La guitarra activa guardada puede no existir (se quitó la entrada).
    if (const int a = activeGuitar; ! juce::isPositiveAndBelow (a, maxChannels)
        || ! channels[(size_t) a].used || channels[(size_t) a].type != ChannelType::Guitar)
        activeGuitar = firstGuitar();

    library.scan();
    library.loadFrom (saved.get());
    log ("presets: " + juce::String ((int) library.getAll().size()) + ", favoritos: "
         + juce::String (library.favorites.size()));

    auto error = deviceManager.initialise (8, 2, devState, true);
    log ("initialise: " + (error.isEmpty() ? juce::String ("ok") : error));

    // Primera vez, si lo guardado no abrió, o si no es la Audient y la Audient está:
    // buscar la mejor interfaz que haya.
    auto* opened = deviceManager.getCurrentAudioDevice();
    audientWasPresent = isAudientPresent();

    if (devState == nullptr || opened == nullptr
        || (audientWasPresent && ! opened->getName().containsIgnoreCase (preferredInterface)))
        reconnect();
    else
        enableMidiInputs();

    applySafeMonitoring();
    saveSettings();

    deviceManager.addMidiInputDeviceCallback ({}, this);
    deviceManager.addAudioCallback (this);
}

void Engine::enableMidiInputs()
{
    // Todos los teclados MIDI conectados; cada canal elige de cuál escucha.
    juce::StringArray names, ids;

    for (auto& d : juce::MidiInput::getAvailableDevices())
    {
        deviceManager.setMidiInputDeviceEnabled (d.identifier, true);
        names.add (d.name);
        ids.add (d.identifier);
    }

    knownMidiIds = ids;
    log ("midi: " + (names.isEmpty() ? juce::String ("ninguno") : names.joinIntoString (" | ")));
    connectDaw();
}

namespace
{
    // Puerto DAW del Launchkey MK3: "MIDIIN2 (LKMK3 MIDI)" / "MIDIOUT2 (LKMK3 MIDI)".
    bool isDawPort (const juce::String& name, const char* prefix)
    {
        return name.containsIgnoreCase (prefix) && (name.containsIgnoreCase ("LKMK3") || name.containsIgnoreCase ("Launchkey"));
    }
}

void Engine::connectDaw()
{
    juce::String inId, outId, mainId;

    for (auto& d : juce::MidiInput::getAvailableDevices())
    {
        if (isDawPort (d.name, "MIDIIN2"))
            inId = d.identifier;
        else if (d.name.containsIgnoreCase ("LKMK3") || d.name.containsIgnoreCase ("Launchkey"))
            mainId = d.identifier;
    }

    for (auto& d : juce::MidiOutput::getAvailableDevices())
        if (isDawPort (d.name, "MIDIOUT2"))
            outId = d.identifier;

    {
        const juce::SpinLock::ScopedLockType ml (midiLock);
        dawInId = inId;
        dawMainId = mainId;
    }

    if (outId.isEmpty() || inId.isEmpty())
    {
        dawOut = nullptr;
        return;
    }

    if (dawOut != nullptr && dawOut->getIdentifier() == outId)
        return;

    dawOut = juce::MidiOutput::openDevice (outId);
    if (dawOut == nullptr)
        return;

    // Modo DAW (nota 12, canal 16), pads en Session y perillas en Volumen.
    dawOut->sendMessageNow (juce::MidiMessage::noteOn (16, 12, (juce::uint8) 127));
    dawOut->sendMessageNow (juce::MidiMessage::controllerEvent (16, 3, 2));
    dawOut->sendMessageNow (juce::MidiMessage::controllerEvent (16, 9, 1));
    log ("launchkey: modo DAW activado");

    if (onDawMessage)
        onDawMessage (juce::MidiMessage::controllerEvent (16, 3, 2));   // repintar las luces
}

void Engine::logText (const juce::String& text)
{
    log (text);
}

void Engine::sendDaw (const juce::MidiMessage& m)
{
    if (dawOut != nullptr)
        dawOut->sendMessageNow (m);
}

bool Engine::checkMidiDevices()
{
    juce::StringArray ids;
    for (auto& d : juce::MidiInput::getAvailableDevices())
        ids.add (d.identifier);

    if (ids == knownMidiIds)
        return false;

    enableMidiInputs();
    return true;
}

bool Engine::openDevice (const juce::String& type, const juce::String& input, const juce::String& output)
{
    deviceManager.setCurrentAudioDeviceType (type, false);
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.inputDeviceName = input;
    setup.outputDeviceName = output;
    setup.useDefaultInputChannels = setup.useDefaultOutputChannels = true;
    auto error = deviceManager.setAudioDeviceSetup (setup, true);
    log ("abrir " + type + " / " + input + " -> " + output + ": " + (error.isEmpty() ? juce::String ("ok") : error));
    return error.isEmpty() && deviceManager.getCurrentAudioDevice() != nullptr;
}

// La interfaz preferida (la Audient, o la que diga preferredInterface en ampbox.xml)
// aparece en el audio de Windows cuando está enchufada (el driver ASIO figura
// siempre, aunque no esté).
bool Engine::isAudientPresent()
{
    return preferredInterface.isNotEmpty() && isWindowsAudioDevicePresent (preferredInterface);
}

bool Engine::checkAudioDevice()
{
    const bool present = isAudientPresent();
    if (present == audientWasPresent)
        return false;

    log (present ? "audient: conectada" : "audient: desconectada");
    reconnect();
    applySafeMonitoring();
    saveSettings();
    return true;
}

// Al arrancar o al cambiar de interfaz: solo la guitarra activa queda armada, y
// lo que entra por micrófono no se escucha si no es la Audient/ASIO (con los
// parlantes de la laptop se acopla). Las entradas MIDI no se tocan.
void Engine::applySafeMonitoring()
{
    const bool asio = isAsio();

    if (! asio && pcAudio.isActive())
        for (int i = 1; i < maxChannels; ++i)
            if (channels[(size_t) i].used && channels[(size_t) i].type == ChannelType::Pc)
                removeChannel (i);

    for (int i = 0; i < maxChannels; ++i)
    {
        auto& c = channels[(size_t) i];
        if (! c.used || c.type == ChannelType::Midi)
            continue;

        const bool activeGuitarHere = c.type == ChannelType::Guitar && i == activeGuitar;
        c.armed = activeGuitarHere;
        c.monitor = activeGuitarHere && asio;
    }

    log (activeGuitar < 0 ? juce::String ("entradas: sin guitarra") : "entradas: guitarra " + juce::String (activeGuitar.load() + 1)
         + " armada, OIR " + (asio ? "solo guitarra" : "apagado (sin ASIO)"));
}

void Engine::reconnect()
{
    juce::StringArray asio;

    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        log ("tipo " + type->getTypeName() + ": " + type->getDeviceNames().joinIntoString (" | "));

        if (type->getTypeName() == "ASIO")
            asio = type->getDeviceNames();
    }

    audientWasPresent = isAudientPresent();
    bool opened = false;

    // 1) La interfaz preferida por ASIO (latencia baja).
    if (audientWasPresent)
        for (auto& name : asio)
            if (! opened && name.containsIgnoreCase (preferredInterface))
                opened = openDevice ("ASIO", name, name);

    // 2) Otra interfaz ASIO. Los drivers de Native Instruments sin su hardware no sirven.
    for (auto& name : asio)
        if (! opened && (preferredInterface.isEmpty() || ! name.containsIgnoreCase (preferredInterface)) && ! name.containsIgnoreCase ("Guitar Rig")
            && ! name.containsIgnoreCase ("Rig Kontrol"))
            opened = openDevice ("ASIO", name, name);

    // 3) Audio de Windows: el micrófono y la salida por defecto (laptop, USB...).
    for (auto* typeName : { "Windows Audio (Low Latency Mode)", "Windows Audio" })
    {
        if (opened)
            break;

        for (auto* type : deviceManager.getAvailableDeviceTypes())
        {
            if (type->getTypeName() != typeName)
                continue;

            auto inputs = type->getDeviceNames (true), outputs = type->getDeviceNames (false);
            const int in = type->getDefaultDeviceIndex (true), out = type->getDefaultDeviceIndex (false);

            if (outputs.isEmpty())
                continue;

            opened = openDevice (typeName, juce::isPositiveAndBelow (in, inputs.size()) ? inputs[in] : juce::String(),
                                 outputs[juce::jlimit (0, outputs.size() - 1, out)]);
        }
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    log ("dispositivo: " + deviceManager.getCurrentAudioDeviceType() + " / "
         + (device != nullptr ? device->getName() : juce::String ("ninguno")));

    enableMidiInputs();
}

void Engine::stop()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.removeMidiInputDeviceCallback ({}, this);

    // Devolver el Launchkey a su modo normal.
    if (dawOut != nullptr)
    {
        dawOut->sendMessageNow (juce::MidiMessage::noteOn (16, 12, (juce::uint8) 0));
        dawOut = nullptr;
    }
}

void Engine::saveSettings() const
{
    if (! started)
        return;

    juce::XmlElement root ("AMPBOX");
    root.setAttribute ("channelsVersion", 3);
    root.setAttribute ("bpm", looper.getBpm());
    root.setAttribute ("metronome", looper.isMetronomeOn());
    root.setAttribute ("waitForSound", looper.isWaitForSound());
    root.setAttribute ("presetsHidden", presetsHidden);
    root.setAttribute ("preferredInterface", preferredInterface);
    root.setAttribute ("activeGuitar", activeGuitar.load());

    library.saveTo (root);

    root.setAttribute ("keysVersion", 2);
    auto* keyList = root.createNewChildElement ("KEYS");
    for (auto& [id, key] : keys)
    {
        auto* k = keyList->createNewChildElement ("KEY");
        k->setAttribute ("action", id);
        k->setAttribute ("key", key.getTextDescription());
    }

    auto* list = root.createNewChildElement ("CHANNELS");
    for (int i = 0; i < maxChannels; ++i)
    {
        auto& c = channels[(size_t) i];
        if (! c.used)
            continue;

        auto* e = list->createNewChildElement ("CHANNEL");
        e->setAttribute ("index", i);
        e->setAttribute ("type", typeId (c.type));
        e->setAttribute ("name", c.name);
        e->setAttribute ("input", c.input.load());
        e->setAttribute ("midi", c.midiDevice);
        e->setAttribute ("plugin", c.pluginFile.getFullPathName());
        e->setAttribute ("armed", c.armed.load());
        e->setAttribute ("monitor", c.monitor.load());
        e->setAttribute ("volume", c.volume.load());
    }

    if (auto dev = deviceManager.createStateXml())
        root.addChildElement (dev.release());

    root.writeTo (settingsFile());
}

void Engine::saveState() const
{
    saveSettings();

    for (int i = 0; i < maxChannels; ++i)
    {
        auto& c = channels[(size_t) i];
        if (c.used && c.plugin != nullptr)
        {
            juce::MemoryBlock state;
            c.plugin->getStateInformation (state);
            channelStateFile (i).replaceWithData (state.getData(), state.getSize());
        }
    }
}

std::unique_ptr<juce::AudioPluginInstance> Engine::createPlugin (const juce::File& file, juce::String& error)
{
    // La batería propia (sonidos de Ableton) no es un VST3.
    if (file == DrumKit::virtualFile())
    {
        if (! DrumKit::findSamplesFolder().isDirectory())
        {
            error = "No se encontraron los sonidos de Ableton Live (Core Library, One Shots\\Drums)";
            return nullptr;
        }

        return std::make_unique<DrumKit>();
    }

    // Cadenas de voz propias (Chester, Cobain, Natural).
    if (const int style = VoiceChain::styleForFile (file); style >= 0)
        return std::make_unique<VoiceChain> (style);

    juce::OwnedArray<juce::PluginDescription> types;

    for (auto* format : formatManager.getFormats())
        if (format->fileMightContainThisPluginType (file.getFullPathName()))
            format->findAllTypesForFile (types, file.getFullPathName());

    if (types.isEmpty())
    {
        error = "No se encontró un plugin en " + file.getFullPathName();
        return nullptr;
    }

    return formatManager.createPluginInstance (*types[0], sampleRate, blockSize, error);
}

void Engine::prepare (juce::AudioPluginInstance& p)
{
    p.releaseResources();
    p.setRateAndBufferSizeDetails (sampleRate, blockSize);
    p.prepareToPlay (sampleRate, blockSize);
}

// Con el lock tomado.
void Engine::sizeChannelBuffer (Channel& c)
{
    int channelsNeeded = 2;

    if (auto* p = getChannelProcessor ((int) (&c - channels.data())))
        channelsNeeded = juce::jmax (channelsNeeded, p->getTotalNumInputChannels(), p->getTotalNumOutputChannels());

    c.buffer.setSize (channelsNeeded, blockSize);
}

//==============================================================================
// Canales

juce::String Engine::typeName (ChannelType t)
{
    return t == ChannelType::Guitar ? "Guitarra" : t == ChannelType::Midi ? "MIDI"
         : t == ChannelType::Pc ? "PC" : "Audio";
}

juce::AudioProcessor* Engine::getChannelProcessor (int index) const
{
    return channels[(size_t) index].plugin.get();
}

int Engine::firstGuitar() const
{
    for (int i = 0; i < maxChannels; ++i)
        if (channels[(size_t) i].used && channels[(size_t) i].type == ChannelType::Guitar)
            return i;

    return -1;
}

int Engine::countGuitars() const
{
    int n = 0;
    for (auto& c : channels)
        if (c.used && c.type == ChannelType::Guitar)
            ++n;

    return n;
}

void Engine::selectGuitar (int index)
{
    if (! juce::isPositiveAndBelow (index, maxChannels) || ! channels[(size_t) index].used
        || channels[(size_t) index].type != ChannelType::Guitar)
        return;

    // Solo la activa suena y se graba (el resto de las entradas no se tocan).
    for (int i = 0; i < maxChannels; ++i)
    {
        auto& c = channels[(size_t) i];
        if (c.used && c.type == ChannelType::Guitar)
            c.armed = c.monitor = (i == index);
    }

    activeGuitar = index;
    saveSettings();
}

void Engine::nextGuitar()
{
    for (int k = 1; k <= maxChannels; ++k)
    {
        const int i = (juce::jmax (-1, activeGuitar.load()) + k) % maxChannels;
        auto& c = channels[(size_t) i];

        if (c.used && c.type == ChannelType::Guitar)
        {
            selectGuitar (i);
            return;
        }
    }
}

int Engine::addChannel (ChannelType type)
{

    if (type == ChannelType::Pc)
    {
        // Con el audio de Windows, la PC se captaría a sí misma (bucle).
        if (! isAsio())
            return -2;

        for (auto& c : channels)
            if (c.used && c.type == ChannelType::Pc)
                return -1; // una sola captura de la PC
    }

    // La guitarra nueva usa la misma entrada que las otras guitarras (si es la primera,
    // la 2: en las interfaces de 2 entradas el instrumento suele ir ahí).
    const int guitar = firstGuitar();
    const int guitarInput = guitar >= 0 ? channels[(size_t) guitar].input.load() : 1;

    for (int i = 0; i < maxChannels; ++i)
    {
        auto& c = channels[(size_t) i];
        if (c.used)
            continue;

        int sameType = 1;
        for (auto& other : channels)
            if (other.used && other.type == type)
                ++sameType;

        {
            const juce::ScopedLock sl (lock);
            const juce::SpinLock::ScopedLockType ml (midiLock);
            c.type = type;
            c.name = type == ChannelType::Pc ? juce::String ("PC")
                   : type == ChannelType::Guitar ? "Guitarra " + juce::String (sameType)
                   : (type == ChannelType::Midi ? "MIDI " : "Audio ") + juce::String (sameType);
            c.input = type == ChannelType::Guitar ? guitarInput : 0;
            c.midiDevice = {};
            c.pluginFile = {};
            c.plugin = nullptr;
            c.armed = false;
            c.volume = 1.0f;
            c.monitor = type != ChannelType::Pc;   // la PC ya se oye por Windows
            c.peak = 0.0f;
            c.collector.reset (sampleRate);
            sizeChannelBuffer (c);
            c.used = true;
        }

        if (type == ChannelType::Pc)
            pcAudio.start();

        channelStateFile (i).deleteFile();

        // La primera guitarra queda activa (armada y con OIR).
        if (type == ChannelType::Guitar && guitar < 0)
            selectGuitar (i);

        saveSettings();
        return i;
    }

    return -1;
}

void Engine::removeChannel (int index)
{
    if (index < 0 || index >= maxChannels)
        return;

    auto& c = channels[(size_t) index];
    std::unique_ptr<juce::AudioPluginInstance> old;

    {
        const juce::ScopedLock sl (lock);
        const juce::SpinLock::ScopedLockType ml (midiLock);
        c.used = false;
        c.armed = false;
        std::swap (old, c.plugin);
        c.pluginFile = {};
    }

    if (c.type == ChannelType::Pc)
        pcAudio.stop();

    if (activeGuitar == index)
    {
        activeGuitar = firstGuitar();
        selectGuitar (activeGuitar);
    }

    channelStateFile (index).deleteFile();
    saveSettings();
}

void Engine::setChannelName (int index, const juce::String& name)
{
    {
        const juce::ScopedLock sl (lock);
        channels[(size_t) index].name = name;
    }

    saveSettings();
}

void Engine::setChannelMidiDevice (int index, const juce::String& identifier)
{
    {
        const juce::SpinLock::ScopedLockType ml (midiLock);
        channels[(size_t) index].midiDevice = identifier;
    }

    saveSettings();
}

bool Engine::loadChannelPlugin (int index, const juce::File& file, juce::String& error)
{
    if (index < 0 || index >= maxChannels)
        return false;

    auto instance = createPlugin (file, error);
    if (instance == nullptr)
    {
        log ("canal " + juce::String (index) + ": " + error);
        return false;
    }

    // Solo la salida principal (los instrumentos de batería suelen traer muchas).
    instance->disableNonMainBuses();
    prepare (*instance);

    auto& c = channels[(size_t) index];

    {
        const juce::ScopedLock sl (lock);
        std::swap (c.plugin, instance);
        c.pluginFile = file;
        c.pluginFailed = false;
        sizeChannelBuffer (c);
    }

    log ("canal " + juce::String (index) + ": " + c.plugin->getName() + " (midi="
         + (c.plugin->acceptsMidi() ? "si" : "no") + ", " + juce::String (c.plugin->getTotalNumInputChannels())
         + " in / " + juce::String (c.plugin->getTotalNumOutputChannels()) + " out, latencia "
         + juce::String (c.plugin->getLatencySamples()) + ")");

    channelStateFile (index).deleteFile();
    saveSettings();
    return true; // el plugin anterior se destruye aquí, fuera del lock
}

void Engine::clearChannelPlugin (int index)
{
    auto& c = channels[(size_t) index];
    std::unique_ptr<juce::AudioPluginInstance> old;

    {
        const juce::ScopedLock sl (lock);
        std::swap (old, c.plugin);
        c.pluginFile = {};
        sizeChannelBuffer (c);
    }

    channelStateFile (index).deleteFile();
    saveSettings();
}

std::vector<int> Engine::getPendingPlugins() const
{
    std::vector<int> pending;

    for (int i = 0; i < maxChannels; ++i)
    {
        auto& c = channels[(size_t) i];
        if (c.used && c.plugin == nullptr && c.pluginFile != juce::File())
            pending.push_back (i);
    }

    return pending;
}

bool Engine::restoreChannelPlugin (int i)
{
    auto& c = channels[(size_t) i];
    if (! c.used || c.plugin != nullptr || c.pluginFile == juce::File())
        return false;

    const auto file = c.pluginFile;
    juce::String error;
    juce::MemoryBlock state;
    const bool hadState = channelStateFile (i).loadFileAsData (state) && state.getSize() > 0;

    if (! loadChannelPlugin (i, file, error))
    {
        c.pluginFailed = true;
        return false;
    }

    if (hadState)
    {
        c.plugin->setStateInformation (state.getData(), (int) state.getSize());
        channelStateFile (i).replaceWithData (state.getData(), state.getSize());
    }

    return true;
}

void Engine::toggleEntry (int number)
{
    for (auto& c : channels)
    {
        if (c.used && --number == 0)
        {
            const bool on = ! (c.armed && c.monitor);
            c.armed = c.monitor = on;
            saveSettings();
            return;
        }
    }
}

void Engine::toggleArm (int number)
{
    for (auto& c : channels)
    {
        if (c.used && --number == 0)
        {
            c.armed = ! c.armed;
            saveSettings();
            return;
        }
    }
}

int Engine::countChannels() const
{
    int n = 0;
    for (auto& c : channels)
        if (c.used)
            ++n;

    return n;
}

int Engine::countArmed() const
{
    int n = 0;
    for (auto& c : channels)
        if (c.used && c.armed)
            ++n;

    return n;
}

void Engine::handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message)
{
    const juce::SpinLock::ScopedLockType ml (midiLock);

    auto sourceId = source != nullptr ? source->getIdentifier() : juce::String();

    // Puerto DAW del Launchkey: controles de AmpBox, no notas para los instrumentos.
    // Excepto los pads en modo Drum (canal 10): van a la batería como si vinieran del teclado.
    if (dawInId.isNotEmpty() && sourceId == dawInId)
    {
        if (message.getChannel() != 10 || ! (message.isNoteOnOrOff() || message.isAftertouch()))
        {
            if (onDawMessage)
                juce::MessageManager::callAsync ([this, token = std::weak_ptr<bool> (alive), message]
                {
                    if (! token.expired() && onDawMessage)   // el Engine sigue vivo
                        onDawMessage (message);
                });

            return;
        }

        sourceId = dawMainId;
    }

    for (auto& c : channels)
    {
        // Como en Ableton: el MIDI llega a los canales armados o que se escuchan.
        if (! c.used || c.type != ChannelType::Midi || ! (c.armed || c.monitor))
            continue;

        if (c.midiDevice.isEmpty() || c.midiDevice == sourceId)
            c.collector.addMessageToQueue (message);
    }
}

//==============================================================================

double Engine::getLatencyMs() const
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return 0.0;

    const int guitar = activeGuitar;
    auto* amp = juce::isPositiveAndBelow (guitar, maxChannels) ? getChannelProcessor (guitar) : nullptr;
    auto samples = device->getInputLatencyInSamples() + device->getOutputLatencyInSamples()
                 + (amp != nullptr ? amp->getLatencySamples() : 0);

    return samples * 1000.0 / device->getCurrentSampleRate();
}

void Engine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    blockSize = device->getCurrentBufferSizeSamples();
    deviceInLatency = device->getInputLatencyInSamples();
    deviceOutLatency = device->getOutputLatencyInSamples();

    looper.prepare (sampleRate, blockSize);
    pcAudio.setTargetRate (sampleRate);

    const juce::ScopedLock sl (lock);

    for (auto& c : channels)
    {
        if (c.plugin != nullptr)
            prepare (*c.plugin);

        c.collector.reset (sampleRate);
        sizeChannelBuffer (c);
    }

    mix.setSize (2, blockSize);
}

void Engine::audioDeviceStopped()
{
    const juce::ScopedLock sl (lock);

    for (auto& c : channels)
        if (c.plugin != nullptr)
            c.plugin->releaseResources();
}

int Engine::readTuner (float* dest, int maxSamples)
{
    int start1, size1, start2, size2;
    tunerFifo.prepareToRead (maxSamples, start1, size1, start2, size2);
    std::copy_n (tunerData.data() + start1, size1, dest);
    std::copy_n (tunerData.data() + start2, size2, dest + size1);
    tunerFifo.finishedRead (size1 + size2);
    return size1 + size2;
}

void Engine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                               float* const* outputs, int numOutputs,
                                               int numSamples,
                                               const juce::AudioIODeviceCallbackContext&)
{
    for (int o = 0; o < numOutputs; ++o)
        if (outputs[o] != nullptr)
            juce::FloatVectorOperations::clear (outputs[o], numSamples);

    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked() || numSamples > mix.getNumSamples())
        return;

    mix.clear (0, numSamples);
    auto* mixL = mix.getWritePointer (0);
    auto* mixR = mix.getWritePointer (1);

    const int inLatency = deviceInLatency, outLatency = deviceOutLatency;
    std::array<Looper::Source, maxChannels> sources {};

    for (size_t i = 0; i < channels.size(); ++i)
    {
        auto& c = channels[i];

        // Afinador: la entrada tal cual, aunque no se esté oyendo ni grabando.
        if ((int) i == tunerChannel && c.used && (c.type == ChannelType::Guitar || c.type == ChannelType::Audio))
        {
            const int ch = c.input;
            if (ch >= 0 && ch < numInputs && inputs[ch] != nullptr)
            {
                int start1, size1, start2, size2;
                tunerFifo.prepareToWrite (numSamples, start1, size1, start2, size2);
                std::copy_n (inputs[ch], size1, tunerData.data() + start1);
                std::copy_n (inputs[ch] + size1, size2, tunerData.data() + start2);
                tunerFifo.finishedWrite (size1 + size2);   // si la ventana no lee, se pierde (da igual)
            }
        }

        // Sin OIR ni armada no hace falta procesarla (con el rack de guitarras,
        // las inactivas no gastan CPU en su amplificador).
        if (! c.used || ! (c.monitor || c.armed))
            continue;

        auto* proc = c.plugin.get();
        const int ins = proc != nullptr ? proc->getTotalNumInputChannels() : 0;
        const int outs = proc != nullptr ? proc->getTotalNumOutputChannels() : 2;
        const int needed = juce::jmax (2, ins, outs);

        if (needed > c.buffer.getNumChannels() || numSamples > c.buffer.getNumSamples())
            continue;

        juce::AudioBuffer<float> block (c.buffer.getArrayOfWritePointers(), needed, numSamples);
        block.clear();
        c.midi.clear();

        // Lo que se oye sale con la latencia de salida; lo que entra tarda además
        // la de entrada (audio) o un bloque (MIDI, por el colector).
        int latency = outLatency;

        bool onset = false;
        const float* dry = nullptr;   // voz AmpBox: la entrada sin efecto, para poder cambiarla después

        if (c.type == ChannelType::Pc)
        {
            // Ya viene estéreo; se oye a la vez que el loop.
            pcAudio.read (block.getWritePointer (0), block.getWritePointer (1), numSamples);
            onset = block.getMagnitude (0, numSamples) > onsetThreshold;
        }
        else if (c.type == ChannelType::Midi)
        {
            c.collector.removeNextBlockOfMessages (c.midi, numSamples);
            latency += numSamples;

            for (const auto meta : c.midi)
                onset = onset || meta.getMessage().isNoteOn();

            if (proc == nullptr)
                continue; // sin instrumento no suena
        }
        else
        {
            const int ch = c.input;
            const float* in = (ch >= 0 && ch < numInputs) ? inputs[ch] : nullptr;
            latency += inLatency;

            // La entrada es mono: la misma señal en todas las entradas del plugin
            // (sin plugin, en los dos lados).
            if (in != nullptr)
            {
                const float peak = peakOf (in, numSamples);
                keepMax (inputPeak, peak);
                onset = peak > onsetThreshold;

                for (int k = 0; k < (proc != nullptr ? ins : 2); ++k)
                    block.copyFrom (k, 0, in, numSamples);

                if (dynamic_cast<VoiceChain*> (proc) != nullptr)
                    dry = in;
            }
        }

        if (proc != nullptr)
        {
            proc->processBlock (block, c.midi);
            latency += proc->getLatencySamples();

            // Plugin mono: duplicar a estéreo.
            if (outs == 1)
                block.copyFrom (1, 0, block, 0, 0, numSamples);
        }

        keepMax (c.peak, block.getMagnitude (0, 0, numSamples));

        // Lo que se oye va con el volumen de la entrada; lo que se graba, entero.
        const float volume = c.volume;

        if (c.monitor)
        {
            juce::FloatVectorOperations::addWithMultiply (mixL, block.getReadPointer (0), volume, numSamples);
            juce::FloatVectorOperations::addWithMultiply (mixR, block.getReadPointer (1), volume, numSamples);
        }

        sources[i] = { block.getReadPointer (0), block.getReadPointer (1), latency, c.armed.load(), onset, volume, dry };
    }

    looper.setOutputLatency (outLatency);
    looper.process (sources.data(), (int) sources.size(), mixL, mixR, numSamples);

    for (int o = 0; o < numOutputs; ++o)
    {
        if (outputs[o] == nullptr)
            continue;

        juce::FloatVectorOperations::copy (outputs[o], mix.getReadPointer (juce::jmin (o, 1)), numSamples);
        keepMax (outputPeak, peakOf (outputs[o], numSamples));
    }
}
