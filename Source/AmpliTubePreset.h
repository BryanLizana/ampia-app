#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// Carga presets de AmpliTube (.at5p) en el plugin VST3.
//
// AmpliTube VST3 no expone programas ni responde a Program Change, pero su
// estado lleva dentro el XML del preset. El estado tiene estas capas:
//   "VC2!" + XML de JUCE  ->  <IComponent> en base64 de JUCE
//   -> "VstW" (16 bytes) + banco FXB "CcnK"/"FBCh" (172 bytes de cabecera + tamaño)
//   -> ValueTree "State" con PresetData = XML del preset (raíz <Program>)
// Se cambia solo PresetData y se recalculan los tamaños.
namespace AmpliTubePreset
{
    // Estado nuevo = estado actual con el preset del archivo.
    bool applyToState (const juce::MemoryBlock& currentState, const juce::File& presetFile,
                       juce::MemoryBlock& newState, juce::String& error);

    // Nombre del preset que lleva un estado (para comprobar que cargó).
    juce::String getPresetName (const juce::MemoryBlock& state);
}
