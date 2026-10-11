#pragma once
#include <juce_core/juce_core.h>

// Presets de AmpliTube en Documentos\IK Multimedia\AmpliTube 5\Presets
// y la lista de favoritos de AmpBox. Las rutas son relativas a esa carpeta,
// con '/' como separador.
class PresetLibrary
{
public:
    struct Preset
    {
        juce::String name, folder, path;
    };

    static juce::File getFolder();
    static Preset describe (const juce::String& path);

    void scan();
    const std::vector<Preset>& getAll() const { return all; }

    // Todas las palabras tienen que aparecer en el nombre o la carpeta.
    std::vector<Preset> search (const juce::String& text) const;

    juce::StringArray favorites;
    juce::String current;

    bool isFavorite (const juce::String& path) const { return favorites.contains (path, true); }
    void toggleFavorite (const juce::String& path);

    // Ruta del favorito anterior/siguiente al actual (o el primero).
    juce::String stepFavorite (int delta) const;

    void loadFrom (const juce::XmlElement* root);
    void saveTo (juce::XmlElement& root) const;

private:
    void importProgramChangeList();

    std::vector<Preset> all;
};
