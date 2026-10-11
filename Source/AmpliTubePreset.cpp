#include "AmpliTubePreset.h"

namespace AmpliTubePreset
{
namespace
{
    constexpr size_t vstwSize = 16;                 // "VstW" + tamaño + versión + bypass
    constexpr size_t fxbHeaderSize = 28 + 128;      // "CcnK" ... numPrograms + future[128]
    constexpr size_t chunkStart = vstwSize + fxbHeaderSize + 4;

    juce::uint32 readBE (const juce::uint8* p)
    {
        return ((juce::uint32) p[0] << 24) | ((juce::uint32) p[1] << 16) | ((juce::uint32) p[2] << 8) | p[3];
    }

    void writeBE (juce::uint8* p, juce::uint32 v)
    {
        p[0] = (juce::uint8) (v >> 24); p[1] = (juce::uint8) (v >> 16);
        p[2] = (juce::uint8) (v >> 8);  p[3] = (juce::uint8) v;
    }

    struct Inner
    {
        std::unique_ptr<juce::XmlElement> outer;   // XML de JUCE con <IComponent>
        juce::MemoryBlock component;               // bytes de IComponent
        juce::ValueTree tree;                      // "State"
        juce::MemoryBlock trailer;                 // bytes después del ValueTree (se conservan)
    };

    bool unpack (const juce::MemoryBlock& state, Inner& in, juce::String& error)
    {
        in.outer = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
        auto* comp = in.outer != nullptr ? in.outer->getChildByName ("IComponent") : nullptr;

        if (comp == nullptr || ! in.component.fromBase64Encoding (comp->getAllSubText().trim()))
        {
            error = "estado de AmpliTube no reconocido";
            return false;
        }

        auto* d = static_cast<const juce::uint8*> (in.component.getData());
        const size_t size = in.component.getSize();

        if (size < chunkStart || memcmp (d, "VstW", 4) != 0 || memcmp (d + vstwSize, "CcnK", 4) != 0)
        {
            error = "formato interno de AmpliTube no reconocido";
            return false;
        }

        const size_t chunkSize = readBE (d + chunkStart - 4);
        if (chunkStart + chunkSize > size)
        {
            error = "tamaño de bloque incorrecto";
            return false;
        }

        juce::MemoryInputStream stream (d + chunkStart, chunkSize, false);
        in.tree = juce::ValueTree::readFromStream (stream);

        if (! in.tree.hasType ("State") || ! in.tree.hasProperty ("PresetData"))
        {
            error = "no se encontró PresetData";
            return false;
        }

        const auto used = (size_t) stream.getPosition();
        in.trailer = juce::MemoryBlock (d + chunkStart + used, chunkSize - used);
        return true;
    }

    juce::MemoryBlock pack (Inner& in)
    {
        juce::MemoryOutputStream chunk;
        in.tree.writeToStream (chunk);
        chunk.write (in.trailer.getData(), in.trailer.getSize());

        juce::MemoryBlock component;
        component.append (in.component.getData(), chunkStart - 4);   // cabeceras VstW + FXB
        juce::uint8 size[4];
        writeBE (size, (juce::uint32) chunk.getDataSize());
        component.append (size, 4);
        component.append (chunk.getData(), chunk.getDataSize());

        // Tamaño del bloque CcnK: todo lo que sigue a sus primeros 8 bytes.
        writeBE (static_cast<juce::uint8*> (component.getData()) + vstwSize + 4,
                 (juce::uint32) (component.getSize() - vstwSize - 8));

        auto* comp = in.outer->getChildByName ("IComponent");
        comp->deleteAllTextElements();
        comp->addTextElement (component.toBase64Encoding());

        juce::MemoryBlock state;
        juce::AudioProcessor::copyXmlToBinary (*in.outer, state);
        return state;
    }

    std::unique_ptr<juce::XmlElement> presetXml (const juce::ValueTree& tree)
    {
        if (auto* data = tree.getProperty ("PresetData").getBinaryData())
            return juce::parseXML (juce::String::fromUTF8 (static_cast<const char*> (data->getData()),
                                                           (int) strnlen (static_cast<const char*> (data->getData()), data->getSize())));
        return nullptr;
    }
}

bool applyToState (const juce::MemoryBlock& currentState, const juce::File& presetFile,
                   juce::MemoryBlock& newState, juce::String& error)
{
    auto preset = juce::parseXML (presetFile);
    if (preset == nullptr)
    {
        error = "no se pudo leer " + presetFile.getFileName();
        return false;
    }

    Inner in;
    if (! unpack (currentState, in, error))
        return false;

    // En el estado la raíz se llama <Program> y lleva nombre y ruta.
    preset->setTagName ("Program");
    preset->setAttribute ("PresetName", presetFile.getFileNameWithoutExtension());
    preset->setAttribute ("PresetPath", presetFile.getFullPathName());

    auto text = preset->toString (juce::XmlElement::TextFormat().singleLine());
    juce::MemoryBlock data (text.toRawUTF8(), text.getNumBytesAsUTF8() + 1); // con el 0 final
    in.tree.setProperty ("PresetData", data, nullptr);

    newState = pack (in);
    return true;
}

juce::String getPresetName (const juce::MemoryBlock& state)
{
    Inner in;
    juce::String error;

    if (unpack (state, in, error))
        if (auto xml = presetXml (in.tree))
            return xml->getStringAttribute ("PresetName");

    return {};
}
}
