#pragma once
#include "Engine.h"

// Afinador de una entrada: muestra la nota que suena y cuántos cents está desafinada.
// Lee la entrada limpia (sin amplificador: la distorsión confunde la nota) y la
// analiza con YIN en el hilo de mensajes.
class TunerWindow : public juce::DocumentWindow
{
public:
    TunerWindow (Engine&, int channel, std::function<void()> onClose);
    void closeButtonPressed() override { onClose(); }

    int getChannel() const { return channel; }

private:
    const int channel;
    std::function<void()> onClose;
};
