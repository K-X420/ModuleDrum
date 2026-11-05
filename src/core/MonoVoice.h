// Glue: MIDI + parameters -> HostModel blocks -> DspEngine -> float stereo audio (44.1 kHz native).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include "dsp/DspEngine.h"
#include "host/HostModel.h"

namespace mnm {

class MonoVoice {
public:
    explicit MonoVoice(const fw::Firmware& fw);

    host::HostModel& host() { return m_host; }
    dsp::DspEngine& engine() { return *m_engine; }

    // Renders exactly one 16-frame block into the internal FIFO (advances the host tick).
    void renderBlock();
    // Pull n frames; renders blocks as needed. Events should be applied via host() between calls
    // (they take effect at the next block boundary, like the hardware's control rate).
    void process(float* left, float* right, int n);
    // Effect mode: host audio in (may alias out), processed in 16-frame blocks.
    void processFx(const float* inL, const float* inR, float* outL, float* outR, int n);
    int framesBuffered() const { return m_avail; }
    // Snaps the host words to their targets (HostModel::settle) and renders `blocks` blocks so the DSP
    // side is initialised; the FIFO is left empty.
    void warmUp(int blocks);
    // Back to the state of a freshly constructed voice: the DSP re-initialised (delay lines, envelopes and
    // every other kernel state cleared), a new HostModel, the FIFO empty. Used between offline renders.
    void reset();
    // RESTING: a voice that has put out exact silence (with silence in, as an effect) for kRestAfter blocks stops
    // running the DSP and puts out zeros, its host model still ticking (LFOs, slews, the frame count). It wakes on any
    // host change (a note, a knob, the machine...), on input, and every kProbeEvery blocks runs one block for real to
    // make sure it is still silent. Off by default (offline renders stay exact); the plugins turn it on.
    void setResting(bool allowed) { m_restAllowed = allowed; if (!allowed) m_resting = false; m_silent = 0; }
    bool resting() const { return m_resting; }
    static constexpr int kRestAfter = 5512;   // 2 s at 44.1 kHz: beyond the delay's longest echo
    static constexpr int kProbeEvery = 256;   // ~93 ms
    static constexpr int32_t kQuiet = 64;     // output steps (of 2^23) that still count as silence: -102 dBFS

private:
    bool restingBlock(bool inputSilent);   // true: this block is skipped (the FIFO holds zeros)
    void noteBlock(bool inputSilent);      // after a real render: counts the silent blocks
    bool m_restAllowed = false, m_resting = false;
    int m_silent = 0, m_probe = 0;
    uint32_t m_seenChanges = 0;
    bool m_inSilent = true;                // processFx: the input block about to render was all zero
    host::HostModel m_host;
    std::unique_ptr<dsp::DspEngine> m_engine;
    std::array<int32_t, 32> m_fifo{};
    std::array<int32_t, 32> m_in{};
    int m_inPos = 0;
    int m_pos = 0, m_avail = 0;
};

} // namespace mnm
