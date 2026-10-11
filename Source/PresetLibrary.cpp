#include "PresetLibrary.h"

juce::File PresetLibrary::getFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("IK Multimedia/AmpliTube 5/Presets");
}

PresetLibrary::Preset PresetLibrary::describe (const juce::String& path)
{
    return { path.fromLastOccurrenceOf ("/", false, false).upToLastOccurrenceOf (".", false, false),
             path.upToLastOccurrenceOf ("/", false, false),
             path };
}

void PresetLibrary::scan()
{
    all.clear();
    const auto folder = getFolder();

    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*.at5p;*.at4p", juce::File::findFiles))
        all.push_back (describe (entry.getFile().getRelativePathFrom (folder).replaceCharacter ('\\', '/')));

    std::sort (all.begin(), all.end(), [] (const Preset& a, const Preset& b)
    {
        return a.path.compareNatural (b.path) < 0;
    });
}

std::vector<PresetLibrary::Preset> PresetLibrary::search (const juce::String& text) const
{
    auto words = juce::StringArray::fromTokens (text.trim(), " ", "");
    words.removeEmptyStrings();

    if (words.isEmpty())
        return all;

    std::vector<Preset> found;

    for (auto& p : all)
    {
        bool match = true;
        for (auto& w : words)
            match = match && (p.name.containsIgnoreCase (w) || p.folder.containsIgnoreCase (w));

        if (match)
            found.push_back (p);
    }

    return found;
}

void PresetLibrary::toggleFavorite (const juce::String& path)
{
    const int i = favorites.indexOf (path, true);
    if (i >= 0)
        favorites.remove (i);
    else
        favorites.add (path);
}

juce::String PresetLibrary::stepFavorite (int delta) const
{
    if (favorites.isEmpty())
        return {};

    const int i = favorites.indexOf (current, true);
    if (i < 0)
        return favorites[0];

    return favorites[juce::jlimit (0, favorites.size() - 1, i + delta)];
}

void PresetLibrary::loadFrom (const juce::XmlElement* root)
{
    favorites.clear();

    if (root != nullptr)
        current = root->getStringAttribute ("presetPath");

    if (auto* favs = root != nullptr ? root->getChildByName ("FAVORITES") : nullptr)
    {
        for (auto* f : favs->getChildIterator())
            favorites.add (f->getStringAttribute ("path"));
    }
    else
    {
        // Primera vez: los favoritos salen de la lista de Program Change de AmpliTube.
        importProgramChangeList();
    }
}

void PresetLibrary::saveTo (juce::XmlElement& root) const
{
    root.setAttribute ("presetPath", current);

    auto* favs = root.createNewChildElement ("FAVORITES");
    for (auto& path : favorites)
        favs->createNewChildElement ("FAV")->setAttribute ("path", path);
}

void PresetLibrary::importProgramChangeList()
{
    auto xml = juce::parseXML (getFolder().getSiblingFile ("ProgramChangePresetList.ik"));
    if (xml == nullptr)
        return;

    std::vector<std::pair<int, juce::String>> entries;

    for (int i = 0; i < xml->getNumAttributes(); ++i)
        if (xml->getAttributeName (i).startsWith ("PC"))
            entries.push_back ({ xml->getAttributeName (i).substring (2).getIntValue(),
                                 xml->getAttributeValue (i).replaceCharacter ('\\', '/') });

    std::sort (entries.begin(), entries.end());

    for (auto& [pc, path] : entries)
        if (! favorites.contains (path, true))
            favorites.add (path);
}
