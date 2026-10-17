#include "DrumKit.h"
#include "Theme.h"

namespace
{
    // Un kit: nombre del archivo (sin extensión) para cada pad, notas 36-51.
    struct KitDef
    {
        const char* name;
        std::array<const char*, DrumKit::numPads> files;
    };

    const std::array<KitDef, 4> kits
    {{
        { "Acustico (Dry Session)", {
            "Kick Felt Hard Dry Session", "Rim Sidestick Hard Dry Session", "Snare Stick Hit Hard Dry Session",
            "Clap Hit Hard 1", "Snare Stick Hit Medium Dry Session", "Tom Stick Hit Hard Dry Session 16",
            "Hihat Closed Stick Tip Hard 1", "Tom Stick Hit Soft Dry Session 16", "Hihat Foot Hard",
            "Tom Stick Hit Hard Dry Session 14", "Hihat Open Stick Tip Hard 1", "Tom Stick Hit Soft Dry Session 14",
            "Tom Stick Hit Hard Dry Session 12", "Crash Dry Session Hard 1", "Tom Stick Hit Hard Dry Session 10",
            "Ride Stick Mid Hard Dry Session" } },

        { "Rock (90s Heavy Rock)", {
            "Kick Plastic 90s Heavy Rock", "Rim Sidestick Pop Rock Video", "Snare Stick Hit L 90s Heavy Rock 39",
            "Clap Hit Hard 2", "Snare Stick Hit R 90s Heavy Rock 39", "Tom 90s Heavy Rock 4",
            "Hihat Closed Stick Tip Hard 2", "Tom 90s Heavy Rock 4", "Hihat Foot Acoustic",
            "Tom 90s Heavy Rock 3", "Hihat Open Stick Tip Hard 2", "Tom 90s Heavy Rock 3",
            "Tom 90s Heavy Rock 2", "Crash Heavy Rock", "Tom 90s Heavy Rock 1",
            "Ride Stick Edge 90s Heavy Rock" } },

        { "808", {
            "Kick 808 1", "Rim 808", "Snare 808", "Clap 808", "Snare 808 Tite", "Tom 808 Low",
            "Hihat Closed 808", "Tom 808 Low DMX", "Hihat Closed 808 Short Castle", "Tom 808 Mid",
            "Hihat Open 808", "Tom 808 Mid DMX", "Tom 808 Hi DMX", "Cymbal 808 Full", "Tom 808 Hi",
            "Cymbal 808 Soft" } },

        { "909", {
            "Kick 909 1", "Rim 909", "Snare 909 Hard 1", "Clap 909", "Snare 909 Tune 8", "Tom 909 Low",
            "Hihat Closed 909", "Tom 909 Low Alt", "Hihat Closed 909", "Tom 909 Mid",
            "Hihat Open 909", "Tom 909 Mid 2", "Tom 909 Hi", "Crash 909", "Tom 909 Hi 2",
            "Ride 909" } },
    }};

    const std::array<const char*, DrumKit::numPads> padNames
    {
        "Bombo", "Aro", "Caja", "Palmas", "Caja 2", "Tom piso", "Hi-hat cerrado", "Tom piso 2",
        "Hi-hat pedal", "Tom bajo", "Hi-hat abierto", "Tom medio", "Tom medio 2", "Crash", "Tom agudo", "Ride"
    };

    constexpr int closedHat = 6, pedalHat = 8, openHat = 10;
    constexpr double maxSampleSeconds = 8.0;

    // Nombre sin extensión -> archivo, de toda la carpeta de baterías (~2000).
    // Se recorre una sola vez (hilo de mensajes).
    const std::map<juce::String, juce::File>& indexSamples (const juce::File& folder)
    {
        static juce::File indexedFolder;
        static std::map<juce::String, juce::File> index;

        if (folder != indexedFolder)
        {
            index.clear();
            for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*.wav;*.aif;*.aiff"))
                index.emplace (entry.getFile().getFileNameWithoutExtension(), entry.getFile());

            indexedFolder = folder;
        }

        return index;
    }
}

DrumKit::DrumKit()
    : AudioPluginInstance (BusesProperties().withOutput ("Out", juce::AudioChannelSet::stereo(), true))
{
    setCurrentProgram (0);
}

DrumKit::~DrumKit() = default;

juce::File DrumKit::virtualFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("AmpBox").getChildFile ("Bateria AmpBox");
}

juce::File DrumKit::findSamplesFolder()
{
    // Ableton guarda la Core Library en ProgramData\Ableton\<versión>\Resources.
    auto versions = juce::File ("C:\\ProgramData\\Ableton").findChildFiles (juce::File::findDirectories, false);
    std::sort (versions.begin(), versions.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileName() > b.getFileName();
    });

    for (auto& v : versions)
    {
        auto drums = v.getChildFile ("Resources/Core Library/Samples/One Shots/Drums");
        if (drums.isDirectory())
            return drums;
    }

    return {};
}

juce::String DrumKit::padName (int pad)
{
    return juce::isPositiveAndBelow (pad, numPads) ? padNames[(size_t) pad] : "";
}

juce::String DrumKit::getPadSample (int pad) const
{
    const juce::SpinLock::ScopedLockType sl (kitLock);
    if (kit == nullptr || ! juce::isPositiveAndBelow (pad, numPads) || kit->pads[(size_t) pad] == nullptr)
        return {};

    return kit->pads[(size_t) pad]->name;
}

void DrumKit::fillInPluginDescription (juce::PluginDescription& d) const
{
    d.name = getName();
    d.descriptiveName = "Bateria con los sonidos de Ableton";
    d.pluginFormatName = "AmpBox";
    d.category = "Instrument";
    d.manufacturerName = "AmpBox";
    d.version = "1.0";
    d.fileOrIdentifier = virtualFile().getFullPathName();
    d.isInstrument = true;
    d.numInputChannels = 0;
    d.numOutputChannels = 2;
}

int DrumKit::getNumPrograms()
{
    return (int) kits.size();
}

const juce::String DrumKit::getProgramName (int index)
{
    return juce::isPositiveAndBelow (index, (int) kits.size()) ? kits[(size_t) index].name : "";
}

void DrumKit::setCurrentProgram (int index)
{
    index = juce::jlimit (0, (int) kits.size() - 1, index);
    auto next = std::make_unique<Kit>();
    auto folder = findSamplesFolder();

    if (folder.isDirectory())
    {
        const auto& index_ = indexSamples (folder);
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        for (int pad = 0; pad < numPads; ++pad)
        {
            auto it = index_.find (kits[(size_t) index].files[(size_t) pad]);
            if (it == index_.end())
                continue;

            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (it->second));
            if (reader == nullptr)
                continue;

            auto s = std::make_unique<Sample>();
            const auto len = (int) juce::jmin (reader->lengthInSamples, (juce::int64) (maxSampleSeconds * reader->sampleRate));
            s->audio.setSize (2, len);
            reader->read (&s->audio, 0, len, 0, true, true);   // mono: el mismo canal a los dos lados
            s->rate = reader->sampleRate;
            s->name = it->first;
            next->pads[(size_t) pad] = std::move (s);
        }
    }

    {
        const juce::SpinLock::ScopedLockType sl (kitLock);
        std::swap (kit, next);
        kitIndex = index;

        for (auto& v : voices)
            v = {};
    }
    // el kit anterior se libera aquí, fuera del lock
}

void DrumKit::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement xml ("BATERIA");
    xml.setAttribute ("kit", kitIndex);
    copyXmlToBinary (xml, dest);
}

void DrumKit::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        setCurrentProgram (xml->getIntAttribute ("kit", 0));
}

//==============================================================================
// Hilo de audio

void DrumKit::startVoice (int pad, float velocity)
{
    const auto* sample = kit->pads[(size_t) pad].get();
    if (sample == nullptr || sample->audio.getNumSamples() < 2)
        return;

    // El hi-hat cerrado o el pedal cortan el abierto.
    if (pad == closedHat || pad == pedalHat)
        for (auto& v : voices)
            if (v.pad == openHat)
                v.sample = nullptr;

    // Una voz libre, o la que lleva más tiempo sonando.
    Voice* target = &voices[0];
    for (auto& v : voices)
    {
        if (v.sample == nullptr)
        {
            target = &v;
            break;
        }

        if (v.pos > target->pos)
            target = &v;
    }

    target->sample = sample;
    target->pos = 0.0;
    target->gain = 0.15f + 0.85f * velocity * velocity;
    target->pad = pad;
}

void DrumKit::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    buffer.clear();

    const juce::SpinLock::ScopedTryLockType sl (kitLock);
    if (! sl.isLocked() || kit == nullptr || buffer.getNumChannels() < 2)
        return;

    auto* outL = buffer.getWritePointer (0);
    auto* outR = buffer.getWritePointer (1);
    const int numSamples = buffer.getNumSamples();

    auto render = [&] (int start, int end)
    {
        for (auto& v : voices)
        {
            if (v.sample == nullptr)
                continue;

            const auto& audio = v.sample->audio;
            const int len = audio.getNumSamples();
            const float* srcL = audio.getReadPointer (0);
            const float* srcR = audio.getReadPointer (1);
            const double step = v.sample->rate / deviceRate;

            for (int i = start; i < end; ++i)
            {
                const int idx = (int) v.pos;
                if (idx + 1 >= len)
                {
                    v.sample = nullptr;
                    break;
                }

                const auto frac = (float) (v.pos - idx);
                outL[i] += v.gain * (srcL[idx] + frac * (srcL[idx + 1] - srcL[idx]));
                outR[i] += v.gain * (srcR[idx] + frac * (srcR[idx + 1] - srcR[idx]));
                v.pos += step;
            }
        }
    };

    int done = 0;

    for (const auto meta : midi)
    {
        const auto msg = meta.getMessage();
        if (! msg.isNoteOn())
            continue;

        const int at = juce::jlimit (0, numSamples, meta.samplePosition);
        render (done, at);
        done = at;

        // Las 16 notas desde el 36 son los pads; el resto del teclado las repite.
        const int pad = ((msg.getNoteNumber() - firstNote) % numPads + numPads) % numPads;
        startVoice (pad, msg.getFloatVelocity());
    }

    render (done, numSamples);
}

//==============================================================================
// Ventana: elegir el kit y ver qué suena en cada pad.

namespace
{
    class DrumKitEditor : public juce::AudioProcessorEditor
    {
    public:
        explicit DrumKitEditor (DrumKit& k) : AudioProcessorEditor (k), drums (k)
        {
            addAndMakeVisible (kitBox);
            for (int i = 0; i < drums.getNumPrograms(); ++i)
                kitBox.addItem (drums.getProgramName (i), i + 1);

            kitBox.setSelectedItemIndex (drums.getCurrentProgram(), juce::dontSendNotification);
            kitBox.onChange = [this]
            {
                drums.setCurrentProgram (kitBox.getSelectedItemIndex());
                refresh();
            };

            addAndMakeVisible (folderLabel);
            folderLabel.setFont (juce::FontOptions (11.0f));
            folderLabel.setColour (juce::Label::textColourId, theme::textSecondary);

            for (int pad = 0; pad < DrumKit::numPads; ++pad)
                addAndMakeVisible (padLabels.add (new juce::Label()));

            refresh();
            setSize (460, 120 + 22 * DrumKit::numPads);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (theme::background);
            g.setColour (theme::accent);
            g.setFont (theme::heading (20.0f));
            g.drawText ("BATERIA AMPBOX", 16, 10, getWidth() - 32, 24, juce::Justification::centredLeft);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (16);
            r.removeFromTop (30);
            kitBox.setBounds (r.removeFromTop (28));
            folderLabel.setBounds (r.removeFromTop (22));
            r.removeFromTop (6);

            for (auto* l : padLabels)
                l->setBounds (r.removeFromTop (22));
        }

    private:
        void refresh()
        {
            auto folder = DrumKit::findSamplesFolder();
            folderLabel.setText (folder.isDirectory() ? "Sonidos: " + folder.getFullPathName()
                                                      : "No se encontraron los sonidos de Ableton",
                                 juce::dontSendNotification);

            for (int pad = 0; pad < DrumKit::numPads; ++pad)
            {
                auto sample = drums.getPadSample (pad);
                padLabels[pad]->setText (juce::String (DrumKit::firstNote + pad) + "   " + DrumKit::padName (pad)
                                           + ":   " + (sample.isEmpty() ? juce::String ("(falta)") : sample),
                                         juce::dontSendNotification);
                padLabels[pad]->setColour (juce::Label::textColourId,
                                           sample.isEmpty() ? theme::recRed : theme::text);
            }
        }

        DrumKit& drums;
        juce::ComboBox kitBox;
        juce::Label folderLabel;
        juce::OwnedArray<juce::Label> padLabels;
    };
}

juce::AudioProcessorEditor* DrumKit::createEditor()
{
    return new DrumKitEditor (*this);
}
