#include "Looper.h"

namespace
{
    bool swapState (std::atomic<Looper::State>& s, Looper::State from, Looper::State to)
    {
        return s.compare_exchange_strong (from, to);
    }

    juce::int64 positiveMod (juce::int64 a, juce::int64 b) { return ((a % b) + b) % b; }
}

//==============================================================================
// Hilo de mensajes

void Looper::prepare (double newSampleRate, int maxBlockSize)
{
    for (auto& v : sourceVolume)
        v = 1.0f;

    // Se llama con el audio detenido.
    juce::ignoreUnused (maxBlockSize);
    const juce::ScopedLock sl (allocLock);

    if (newSampleRate == sampleRate && capacity > 0)
        return;

    sampleRate = newSampleRate;
    capacity = (int) (maxSeconds * sampleRate);

    // Con otra frecuencia los loops grabados ya no sirven.
    for (auto& t : tracks)
    {
        t.buffer.setSize (0, 0);
        t.state = State::Empty;
    }

    loopLength = 0;
    position = 0;
    playing = recording = countingIn = false;
    numRecording = 0;
}

void Looper::maintain (int wantedReady)
{
    const juce::ScopedLock sl (allocLock);

    if (capacity == 0)
        return;

    // Tamaño que necesita la próxima toma: el loop, o 5 min si es la base.
    const int target = requiredSize();
    int ready = 0;

    for (auto& t : tracks)
    {
        // Libres o por limpiar: se reservan (Loading) para que el audio no las
        // tome mientras se limpian o cambian de tamaño.
        if (swapState (t.state, State::Dirty, State::Loading)
            || (t.buffer.getNumSamples() != target && swapState (t.state, State::Ready, State::Loading)))
        {
            if (t.buffer.getNumSamples() != target)
                t.buffer.setSize (2, target);   // achicar libera la memoria

            t.buffer.clear();
            t.state = State::Ready;
        }

        if (t.state == State::Ready)
            ++ready;
    }

    // Siempre un buffer listo por cada fuente armada (al menos uno).
    for (auto& t : tracks)
    {
        if (ready >= juce::jmax (1, wantedReady))
            break;

        if (t.state == State::Empty)
        {
            t.buffer.setSize (2, target);
            t.buffer.clear();
            t.state = State::Ready;
            ++ready;
        }
    }
}

bool Looper::hasFreeTrack (int needed) const
{
    const int size = juce::jmax (1, loopLength.load());
    int ready = 0;

    for (auto& t : tracks)
        if (t.state == State::Ready && t.buffer.getNumSamples() >= size)
            ++ready;

    return ready >= juce::jmax (1, needed);
}

void Looper::setBpm (float newBpm)
{
    // Con un loop grabado el tempo queda fijo (si no, el click se desfasa).
    if (! isTempoLocked())
        bpm = juce::jlimit (minBpm, maxBpm, newBpm);
}

bool Looper::isTempoLocked() const
{
    if (recording || countingIn)
        return true;

    for (auto& t : tracks)
        if (t.state == State::Playing)
            return true;

    return false;
}

void Looper::undo()
{
    if (recording || countingIn || waiting)
    {
        cancelRequest = true;
        return;
    }

    // Deshace la última toma sonando (todas sus pistas).
    int last = 0;

    for (auto& t : tracks)
        if (t.state == State::Playing)
            last = juce::jmax (last, t.order);

    for (auto& t : tracks)
        if (t.order == last)
            swapState (t.state, State::Playing, State::Undone);
}

void Looper::redo()
{
    // Rehace la última deshecha (la de menor orden entre las deshechas).
    int next = 0;

    for (auto& t : tracks)
        if (t.state == State::Undone && (next == 0 || t.order < next))
            next = t.order;

    for (auto& t : tracks)
        if (t.order == next)
            swapState (t.state, State::Undone, State::Playing);
}

void Looper::deleteTrack (int index)
{
    swapState (tracks[(size_t) index].state, State::Playing, State::Deleting);
}

void Looper::clearAll()
{
    if (recording || countingIn || waiting)
        cancelRequest = true;

    for (auto& t : tracks)
    {
        swapState (t.state, State::Playing, State::Deleting);
        swapState (t.state, State::Undone, State::Deleting);
    }
}

// Lo visual va atrasado la latencia de salida: muestra lo que se está oyendo.
double Looper::getProgress() const
{
    const int len = loopLength;
    return len > 0 ? (double) positiveMod (position - outputLatency, len) / len : 0.0;
}


bool Looper::copyTrack (int index, juce::AudioBuffer<float>& out) const
{
    auto& t = tracks[(size_t) index];
    const int len = loopLength;

    // Solo pistas terminadas: su buffer ya no se escribe.
    if (t.state != State::Playing || len <= 0 || t.length != len)
        return false;

    out.setSize (2, len);
    out.copyFrom (0, 0, t.buffer, 0, 0, len);
    out.copyFrom (1, 0, t.buffer, 1, 0, len);
    return true;
}

int Looper::addTrack (const juce::AudioBuffer<float>& audio, int source, int stem, int expectedLength)
{
    const int len = loopLength;
    if (len <= 0 || len != expectedLength || audio.getNumChannels() < 1)
        return -1;

    for (int i = 0; i < maxTracks; ++i)
    {
        auto& t = tracks[(size_t) i];

        // Reservarla para que el audio no la use para grabar mientras se copia.
        if (t.buffer.getNumSamples() < len || ! swapState (t.state, State::Ready, State::Loading))
            continue;

        const int n = juce::jmin (len, audio.getNumSamples(), t.buffer.getNumSamples());
        t.buffer.clear();
        t.buffer.copyFrom (0, 0, audio, 0, 0, n);
        t.buffer.copyFrom (1, 0, audio, juce::jmin (1, audio.getNumChannels() - 1), 0, n);
        t.length = len;
        t.order = nextOrder++;
        t.source = source;
        t.stem = stem;
        t.gain = 1.0f;
        t.pan = 0.0f;
        t.mute = false;
        t.state = State::Playing;
        return i;
    }

    return -1;
}

bool Looper::renderMix (juce::AudioBuffer<float>& out, int loops)
{
    const int len = loopLength;
    if (len <= 0 || loops <= 0)
        return false;

    // Como mucho 5 min de MP3 (un loop largo con muchas vueltas no entra en memoria).
    loops = juce::jlimit (1, loops, juce::jmax (1, capacity / len));
    out.setSize (2, len * loops);
    out.clear();

    for (auto& t : tracks)
    {
        // Solo pistas terminadas: su buffer ya no se escribe.
        if (t.state != State::Playing || t.mute || t.length != len)
            continue;

        const float gain = t.gain * sourceVolume[(size_t) t.source].load(), pan = t.pan;
        const float gl = gain * juce::jmin (1.0f, 1.0f - pan);
        const float gr = gain * juce::jmin (1.0f, 1.0f + pan);

        for (int k = 0; k < loops; ++k)
        {
            out.addFrom (0, k * len, t.buffer, 0, 0, len, gl);
            out.addFrom (1, k * len, t.buffer, 1, 0, len, gr);
        }
    }

    return true;
}

//==============================================================================
// Hilo de audio

void Looper::requestRecord (const Source* sources, int numSources)
{
    if (countingIn || waiting)
    {
        countingIn = waiting = false; // segunda pulsación durante la cuenta o la espera: cancelar
        return;
    }

    if (numRecording > 0)
    {
        stopRecording();
        return;
    }

    int armed = 0;
    for (int i = 0; i < numSources; ++i)
        if (sources[i].armed)
            ++armed;

    if (armed == 0 || ! hasFreeTrack (armed))
        return;

    // Todo deshecho: la próxima toma vuelve a ser la base (nuevo largo).
    bool anySounding = false;
    for (auto& t : tracks)
        anySounding = anySounding || t.state == State::Playing;

    if (! anySounding && loopLength > 0)
    {
        for (auto& t : tracks)
            swapState (t.state, State::Undone, State::Dirty);

        loopLength = 0;
        playing = false;
        position = 0;
    }

    // Desde parado y con metrónomo: un compás de cuenta antes de grabar
    // (el click ya marca cuándo empezar).
    if (metronomeOn && ! playing)
    {
        countInPos = 0;
        countInBeat = 1;
        countingIn = true;
        return;
    }

    // Si no, REC al tocar: esperar el primer sonido (ver process).
    if (waitForSound)
    {
        waiting = true;
        return;
    }

    startRecording (sources, numSources);
}

bool Looper::startRecording (const Source* sources, int numSources)
{
    // Una pista libre por cada fuente armada.
    std::array<int, maxSources> slots {};
    int count = 0, next = 0;

    for (int s = 0; s < numSources; ++s)
    {
        if (! sources[s].armed)
            continue;

        // Libre y con lugar para el loop (si hay); la base usa lo que tenga.
        const int size = juce::jmax (1, loopLength.load());
        while (next < maxTracks && (tracks[(size_t) next].state != State::Ready
                                    || tracks[(size_t) next].buffer.getNumSamples() < size))
            ++next;

        if (next >= maxTracks)
            return false; // sin pistas libres

        slots[(size_t) count++] = next++;
    }

    if (count == 0)
        return false;

    // Grabar algo nuevo descarta lo deshecho (como en Guitar Rig).
    for (auto& other : tracks)
        swapState (other.state, State::Undone, State::Dirty);

    const int len = loopLength;

    if (len == 0 || ! playing)
        position = 0;

    // Si se toca encima del loop o del click, lo que entra llega atrasado
    // su latencia (ida y vuelta, plugin...): se graba adelantado eso mismo.
    // Sin esa referencia solo se alinean las fuentes entre sí.
    const bool reference = len > 0 || metronomeOn;
    int minLatency = std::numeric_limits<int>::max();

    for (int s = 0; s < numSources; ++s)
        if (sources[s].armed)
            minLatency = juce::jmin (minLatency, sources[s].latency);

    const int order = nextOrder++;
    numRecording = 0;

    for (int s = 0; s < numSources; ++s)
    {
        if (! sources[s].armed)
            continue;

        const int slot = slots[(size_t) numRecording];
        auto& t = tracks[(size_t) slot];

        // Con loop base, la toma dura lo mismo y se graba en círculo (overdub).
        t.length = len;
        t.order = order;
        t.source = s;
        t.stem = None;
        t.gain = 1.0f;
        t.pan = 0.0f;
        t.mute = false;
        t.state = State::Recording;

        recSlots[(size_t) numRecording] = slot;
        recComps[(size_t) numRecording] = reference ? sources[s].latency : sources[s].latency - minLatency;
        ++numRecording;
    }

    recCapacity = capacity;
    for (int k = 0; k < numRecording; ++k)
        recCapacity = juce::jmin (recCapacity, tracks[(size_t) recSlots[(size_t) k]].buffer.getNumSamples());

    recIndex = 0;
    recording = true;
    playing = true;
    return true;
}

void Looper::stopRecording()
{
    const int count = numRecording;
    numRecording = 0;
    recording = false;

    // Tomas sobre el loop: ya se grabaron en círculo con su largo.
    if (loopLength > 0)
    {
        for (int k = 0; k < count; ++k)
            tracks[(size_t) recSlots[(size_t) k]].state = State::Playing;

        return;
    }

    // Toma base: el largo lo marca la fuente con menos compensación.
    int minComp = std::numeric_limits<int>::max();
    for (int k = 0; k < count; ++k)
        minComp = juce::jmin (minComp, recComps[(size_t) k]);

    const int raw = (int) juce::jlimit ((juce::int64) 0, (juce::int64) recCapacity, recIndex - minComp);

    // Menos de 0.2 s se descarta.
    if (raw < (int) (0.2 * sampleRate))
    {
        for (int k = 0; k < count; ++k)
            tracks[(size_t) recSlots[(size_t) k]].state = State::Dirty;

        playing = false;
        position = 0;
        return;
    }

    // Con metrónomo se ajusta a compases enteros; si no, largo libre.
    const int unit = metronomeOn ? juce::roundToInt (samplesPerBeat() * beatsPerBar) : raw;

    int cycles = juce::jmax (1, (raw + unit / 2) / unit);

    while (cycles > 1 && cycles * unit > recCapacity)
        --cycles;

    const int trackLen = cycles * unit;

    for (int k = 0; k < count; ++k)
    {
        auto& t = tracks[(size_t) recSlots[(size_t) k]];
        const int written = (int) juce::jlimit ((juce::int64) 0, (juce::int64) recCapacity,
                                                recIndex - recComps[(size_t) k]);

        // Lo que se pasó del último ciclo se suma al principio.
        if (written > trackLen)
            for (int ch = 0; ch < 2; ++ch)
                t.buffer.addFrom (ch, 0, t.buffer, ch, trackLen, juce::jmin (written - trackLen, trackLen));

        t.length = trackLen;
        t.state = State::Playing;
    }

    loopLength = trackLen;
}

void Looper::cancelRecording()
{
    for (int k = 0; k < numRecording; ++k)
        tracks[(size_t) recSlots[(size_t) k]].state = State::Dirty;

    numRecording = 0;
    recording = false;

    if (loopLength == 0)
    {
        playing = false;
        position = 0;
    }
}

void Looper::addClick (float* left, float* right, int numSamples, juce::int64 startPos)
{
    const double beatLen = samplesPerBeat();
    const double clickLen = 0.03 * sampleRate;

    for (int i = 0; i < numSamples; ++i)
    {
        const double p = (double) (startPos + i);
        const double inBeat = std::fmod (p, beatLen);

        if (inBeat >= clickLen)
            continue;

        const auto beat = (juce::int64) (p / beatLen);
        const double freq = (beat % beatsPerBar) == 0 ? 1600.0 : 1000.0;
        const auto s = (float) (0.3 * (1.0 - inBeat / clickLen)
                                * std::sin (juce::MathConstants<double>::twoPi * freq * inBeat / sampleRate));
        left[i] += s;
        right[i] += s;
    }
}

void Looper::process (const Source* sources, int numSources, float* left, float* right, int numSamples)
{
    numSources = juce::jmin (numSources, (int) maxSources);

    for (int s = 0; s < numSources; ++s)
        sourceVolume[(size_t) s] = sources[s].volume;

    if (recRequest.exchange (false))
        requestRecord (sources, numSources);

    if (cancelRequest.exchange (false))
    {
        countingIn = waiting = false;
        if (numRecording > 0)
            cancelRecording();
    }

    if (playRequest.exchange (false))
    {
        countingIn = waiting = false;

        if (numRecording > 0)
            stopRecording();

        playing = ! playing && loopLength > 0;
        position = 0;
    }

    bool anyLoop = numRecording > 0;

    for (auto& t : tracks)
    {
        if (t.state == State::Deleting)
            t.state = State::Dirty;

        anyLoop = anyLoop || t.state == State::Playing || t.state == State::Undone;
    }

    // Sin pistas, se empieza de cero (la próxima toma fija otro largo).
    if (! anyLoop)
    {
        loopLength = 0;
        position = 0;
        playing = false;
    }

    // Esperando el primer sonido de una entrada armada: la toma empieza en este bloque.
    if (waiting)
    {
        for (int s = 0; s < numSources; ++s)
        {
            // Si justo no hay pista libre (se está acomodando la memoria), sigue esperando.
            if (sources[s].armed && sources[s].onset)
            {
                waiting = ! startRecording (sources, numSources);
                break;
            }
        }
    }

    // Cuenta previa: solo click, sin avanzar el loop.
    if (countingIn)
    {
        const auto countInLen = (juce::int64) std::llround (samplesPerBeat() * beatsPerBar);
        addClick (left, right, numSamples, countInPos);
        countInPos += numSamples;
        countInBeat = (int) juce::jmin ((juce::int64) beatsPerBar, (juce::int64) (countInPos / samplesPerBeat()) + 1);

        if (countInPos >= countInLen)
        {
            countingIn = false;
            startRecording (sources, numSources); // empieza en el bloque siguiente
        }

        return;
    }

    const juce::int64 pos = position;
    const int len = loopLength;

    // 1) Sumar las pistas. Las que se están grabando encima también suenan
    //    (se leen antes de escribir este bloque).
    if (playing && len > 0)
    {
        for (auto& t : tracks)
        {
            const auto state = t.state.load();
            if ((state != State::Playing && state != State::Recording) || t.length <= 0)
                continue;

            const float gain = t.mute ? 0.0f : t.gain.load() * sourceVolume[(size_t) t.source].load();
            const float pan = t.pan;
            const float gl = gain * juce::jmin (1.0f, 1.0f - pan);
            const float gr = gain * juce::jmin (1.0f, 1.0f + pan);

            int idx = (int) positiveMod (pos, t.length);
            int done = 0;
            float peak = 0.0f;

            while (done < numSamples)
            {
                const int n = juce::jmin (numSamples - done, t.length - idx);
                const float* srcL = t.buffer.getReadPointer (0, idx);
                const float* srcR = t.buffer.getReadPointer (1, idx);

                juce::FloatVectorOperations::addWithMultiply (left + done, srcL, gl, n);
                juce::FloatVectorOperations::addWithMultiply (right + done, srcR, gr, n);

                auto range = juce::FloatVectorOperations::findMinAndMax (srcL, n);
                peak = juce::jmax (peak, gain * juce::jmax (-range.getStart(), range.getEnd()));

                done += n;
                idx = (idx + n) % t.length;
            }

            if (peak > t.peak)
                t.peak = peak;
        }
    }

    // 2) Escribir la toma: cada fuente armada en su pista.
    if (numRecording > 0)
    {
        bool full = false;

        for (int k = 0; k < numRecording; ++k)
        {
            auto& t = tracks[(size_t) recSlots[(size_t) k]];
            const int comp = recComps[(size_t) k];

            if (t.source >= numSources || sources[t.source].left == nullptr)
                continue; // la fuente ya no está: esa pista queda en silencio

            const float* inL = sources[t.source].left;
            const float* inR = sources[t.source].right != nullptr ? sources[t.source].right : inL;

            if (len > 0)
            {
                // Sobre el loop: en círculo, sumando encima de lo ya grabado.
                int idx = (int) positiveMod (pos - comp, len);
                int done = 0;

                while (done < numSamples)
                {
                    const int n = juce::jmin (numSamples - done, len - idx);
                    juce::FloatVectorOperations::add (t.buffer.getWritePointer (0, idx), inL + done, n);
                    juce::FloatVectorOperations::add (t.buffer.getWritePointer (1, idx), inR + done, n);
                    done += n;
                    idx = (idx + n) % len;
                }
            }
            else
            {
                // Toma base: lineal hasta que se termine.
                const juce::int64 dataStart = recIndex - comp;
                const int skip = (int) juce::jlimit ((juce::int64) 0, (juce::int64) numSamples, -dataStart);
                const int start = (int) juce::jlimit ((juce::int64) 0, (juce::int64) recCapacity, dataStart);
                const int n = juce::jmin (numSamples - skip, recCapacity - start);

                if (n > 0)
                {
                    t.buffer.copyFrom (0, start, inL + skip, n);
                    t.buffer.copyFrom (1, start, inR + skip, n);
                }

                full = full || n < numSamples - skip;
            }
        }

        if (len == 0)
        {
            recIndex += numSamples;

            if (full)
                stopRecording(); // se llenó la capacidad
        }
    }

    if (! playing)
    {
        // Parado con el metrónomo prendido: el click suena igual, para practicar o tomar el tempo.
        if (metronomeOn)
        {
            addClick (left, right, numSamples, freeClickPos);
            freeClickPos += numSamples;
        }
        else
        {
            freeClickPos = 0;   // al prenderlo, empieza en el tiempo 1
        }

        return;
    }

    // 3) Click del metrónomo (no se graba).
    if (metronomeOn)
        addClick (left, right, numSamples, pos);

    position = pos + numSamples;
}
