#pragma once
#include <juce_audio_basics/juce_audio_basics.h>

// A block whose engine was locked: its notes and controllers wait in `late` (at its start) for the next block, so a
// note-off is never lost. Bounded, and `late` is preallocated (prepareToPlay), so the audio thread does not allocate.
inline void keepLateMidi(juce::MidiBuffer& late, const juce::MidiBuffer& midi, int maxEvents)
{
    for (const auto meta : midi) {
        if (late.getNumEvents() >= maxEvents) break;
        const auto m = meta.getMessage();
        if (m.isNoteOnOrOff() || m.isController()) late.addEvent(m, 0);
    }
}

// The next block: the held events first (at its start), then its own.
inline void takeLateMidi(juce::MidiBuffer& late, juce::MidiBuffer& midi)
{
    if (late.isEmpty()) return;
    late.addEvents(midi, 0, -1, 0);
    midi.swapWith(late);
    late.clear();
}
