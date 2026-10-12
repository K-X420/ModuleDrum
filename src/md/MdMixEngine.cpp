#include "MdMixEngine.h"
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/memory.h"
#include "dsp56kEmu/peripherals.h"
#include "dsp56kEmu/jit.h"
#include "dsp56kEmu/assembler.h"

namespace mnm::md {

using namespace dsp56k;

namespace {
constexpr TWord kBridge = 0x100000;
constexpr TWord kMemSize = 0x180000;   // delay lines reach 0x117C15 + 0x17AFC
constexpr TWord kBlockStart = 0x44;    // after the DAC-half poll: input X:0x100, output X:0x400
constexpr TWord kParkPc = 0xF40;       // free internal P (the image uses P:0..0xA07)
constexpr TWord kVoiceRing = 0x600;
constexpr TWord kDacHalf = 0x400;
constexpr TWord kSkipStub = 0xF50;     // free internal P: the idle-strip test
constexpr TWord kSkipMask = 0xF60;     // free Y: bit t = track t's strip is left out of this block
constexpr TWord kStripHook = 0xA4;     // 'lua (r6+$22),r4', the strip's first instruction after the track's pointers
constexpr TWord kStripState = 0x200;   // Y: 0x40 words a track
constexpr TWord kStripSrrCount = 0x1C, kStripAmPhase = 0x25;   // in it: the two words that run on in silence
constexpr uint64_t kMaxExecBoot = 20'000'000;
constexpr uint64_t kMaxExecBlock = 4'000'000;
}

MixEngine::MixEngine(const Firmware& fw) : m_fw(fw)
{
    m_validator = std::make_unique<DefaultMemoryValidator>();
    m_mem = std::make_unique<Memory>(*m_validator, kMemSize, kMemSize, kBridge);
    if (!m_mem->hasMmuSupport()) throw std::runtime_error("dsp56300: MMU-backed memory allocation failed");
    m_periphX = std::make_unique<PeripheralsNop>();
    m_periphY = std::make_unique<PeripheralsNop>();
    m_dsp = std::make_unique<DSP>(*m_mem, m_periphX.get(), m_periphY.get());
    auto cfg = m_dsp->getJit().getConfig();
    cfg.dynamicFastInterrupts = true;
    cfg.interruptRegionIsCode = true;
    m_dsp->getJit().setConfig(cfg);
    reset();
}

MixEngine::~MixEngine() = default;

void MixEngine::reset()
{
    m_faulted = false; m_fault.clear();
    m_dsp->resetHW();
    for (const auto& r : m_fw.mixDsp.records)
        for (size_t k = 0; k < r.words.size(); ++k) {
            const TWord a = r.addr + TWord(k);
            if (r.space == fw::Space::P || a >= kBridge) m_dsp->memWriteP(a, r.words[k]);
            else m_dsp->memWrite(r.space == fw::Space::X ? MemArea_X : MemArea_Y, a, r.words[k]);
        }
    m_dsp->memWriteP(kParkPc, 0x0C0000 | kParkPc);   // park: jmp <itself
    m_dsp->memWriteP(0x39, 0x000000);                 // boot ADC wait 'bne $36' -> nop
    m_dsp->memWriteP(0x79, 0x000000);                 // voice wait 'blt $73' -> nop
    m_dsp->memWriteP(0x9A8, 0x0C0000 | kParkPc);      // end of block 'bra $3c' -> jmp <park
    {   // the idle-strip test, hooked in where a track's strip begins (A is free there: loaded again at P:0xA7):
        //   the mask's lowest bit into the carry (a track a turn, in track order); set: on to the next track
        Assembler as;
        TWord pc = kSkipStub;
        auto emit = [&](const char* text) {
            const auto r = as.assemble(text);
            if (!r.success()) throw std::runtime_error(std::string("MD mixer: cannot assemble '") + text + "'");
            m_dsp->memWriteP(pc++, r.word[0]);
            if (r.wordCount > 1) m_dsp->memWriteP(pc++, r.word[1]);
        };
        const TWord displaced = m_mem->get(MemArea_P, kStripHook);
        if (displaced != 0x041624) throw std::runtime_error("MD mixer: the strip is not where OS 1.63 has it");
        emit("move y:>$f60,a");
        m_dsp->memWriteP(pc++, 0x200023);            // lsr a
        emit("move a1,y:>$f60");
        m_dsp->memWriteP(pc++, 0x0E8000 | 0x06F);    // jcs <$6f: the next track
        m_dsp->memWriteP(pc++, displaced);           // lua (r6+$22),r4
        m_dsp->memWriteP(pc++, 0x0C0000 | (kStripHook + 1));   // jmp back into the strip
        m_dsp->memWriteP(kStripHook, 0x0C0000 | kSkipStub);
        m_dsp->memWrite(MemArea_Y, kSkipMask, 0);
    }
    for (auto& s : m_strips) s = Strip();
    m_dsp->setPC(m_fw.mixDsp.startAddr.value_or(0x24));
    if (!runToPark(kMaxExecBoot)) throw std::runtime_error("MD DSP1 boot did not finish: " + m_fault);
}

bool MixEngine::runToPark(uint64_t maxExec)
{
    const uint64_t start = m_dsp->getInstructionCounter();
    for (uint64_t n = 0; n < maxExec; ++n) {
        if (m_entryHist) ++(*m_entryHist)[m_dsp->getPC().toWord()];
        if (m_stepInterpreted) m_dsp->execInterpreter(); else m_dsp->exec();
        if (m_dsp->getJit().hasFailed()) { m_faulted = true; m_fault = "DSP JIT failed: " + m_dsp->getJit().failReason(); return false; }
        if (m_dsp->getPC().toWord() == kParkPc) { m_lastInstr = m_dsp->getInstructionCounter() - start; m_lastExecs = n + 1; return true; }
    }
    std::ostringstream o; o << "block did not finish (PC=$" << std::hex << m_dsp->getPC().toWord() << ")";
    m_faulted = true; m_fault = o.str();
    return false;
}

void MixEngine::setY(uint32_t addr, const uint32_t* words, int count)
{
    for (int k = 0; k < count; ++k) m_dsp->memWrite(MemArea_Y, addr + TWord(k), words[k] & 0xFFFFFF);
}

std::array<uint32_t, 5> MixEngine::routingWords(uint32_t level, uint32_t vol, uint32_t pan, uint32_t reverbSend, uint32_t delaySend, int route, int accent)
{
    const int64_t L = level & 0xFFFF, V = vol & 0xFFFF;
    int64_t volume = ((L * L) >> 8) * accent >> 17;
    volume *= (V * V) >> 17;
    const uint32_t panWord = ((pan & 0xFFFF) << 9) & 0x1FFFE00;
    const uint32_t rev = uint32_t((int64_t(reverbSend & 0xFFFF) * (reverbSend & 0xFFFF)) >> 5);
    const uint32_t del = uint32_t((int64_t(delaySend & 0xFFFF) * (delaySend & 0xFFFF)) >> 5);
    return {uint32_t(route), uint32_t(volume) & 0xFFFFFF, panWord & 0xFFFFFF, rev & 0xFFFFFF, del & 0xFFFFFF};
}

void MixEngine::setRouting(int track, const std::array<uint32_t, 5>& words)
{
    m_routing[size_t(track)] = words;
    auto w = words;
    if ((m_direct >> track) & 1) w[1] = 0;
    setY(0x100 + 5 * uint32_t(track), w.data(), 5);
}

void MixEngine::setDirect(uint32_t mask)
{
    const uint32_t changed = mask ^ m_direct;
    m_direct = mask;
    for (int t = 0; t < VoiceEngine::kTracks; ++t)
        if ((changed >> t) & 1) setRouting(t, m_routing[size_t(t)]);
}

void MixEngine::trackOutput(int track, float* left, float* right) const
{
    TWord mainBlock = 0x200, otherBlock = 0x3E0, block = 0;
    for (int k = 0; k <= track; ++k) {
        const bool main = (m_routing[size_t(k)][0] & 0xFFFFFF) == 6;
        block = main ? mainBlock : otherBlock;
        if (main) mainBlock += 0x20; else otherBlock -= 0x20;
    }
    auto frac = [](TWord w) { return double(int32_t(w << 8) >> 8) * (1.0 / 8388608.0); };
    const auto& w = m_routing[size_t(track)];
    TWord pan = w[2] & 0xFFFFFF;
    if (pan & 0x800000) pan = 0;
    if (pan > 0x7ECCCD) pan = 0x7FFFFF;
    const TWord n = pan >> 10;
    const double vol = frac(w[1]) * 8.0;
    const double gl = frac(m_mem->get(MemArea_X, 0x14A000 + n)) * vol, gr = frac(m_mem->get(MemArea_X, 0x148000 + n)) * vol;
    for (TWord i = 0; i < kFrames; ++i) {
        const double s = frac(m_mem->get(MemArea_X, block + i));
        left[i] = float(s * gl);
        right[i] = float(s * gr);
    }
}

void MixEngine::setTrackFx(int track, const std::array<uint16_t, 9>& raw)
{
    std::array<uint32_t, 9> w{};
    for (int k = 0; k < 9; ++k) w[size_t(k)] = raw[size_t(k)];
    setY(0x200 + 0x40 * uint32_t(track), w.data(), 9);
}

void MixEngine::masterReturn(int32_t* lr64) const
{
    for (TWord k = 0; k < 64; ++k) lr64[k] = int32_t(m_mem->get(MemArea_X, 0x688 + k) << 8) >> 8;
}

int32_t MixEngine::peekX(uint32_t addr) const { return int32_t(m_mem->get(MemArea_X, addr) << 8) >> 8; }
int32_t MixEngine::peekY(uint32_t addr) const { return int32_t(m_mem->get(MemArea_Y, addr) << 8) >> 8; }
uint32_t MixEngine::peekP(uint32_t addr) const { return m_mem->get(MemArea_P, addr); }

// An idle strip's block one block on. With a zero input everything in it stays as it is but two free-running words:
//   +0x25  the AM oscillator's place in its 0x8000-word table: 32 samples x the rate (word +1, >> 3) on (P:0xB9..0xD1)
//   +0x1C  the sample rate reducer's counter: 0x10 a sample, less its period whenever it reaches it (P:0x221..0x233),
//          the period from word +7 as the strip works it out: ((2 (w + 0x4000)^2 - 0x20) << 2) + 0x10 in A1's units
// (renderBlock only trusts this for a strip whose last run came out exactly so.)
std::array<uint32_t, 0x40> MixEngine::idleStep(const std::array<uint32_t, 0x40>& state)
{
    auto sext = [](uint32_t w) { return int64_t(int32_t(w << 8) >> 8); };
    auto next = state;
    const int64_t rate = sext(state[1]) >> 3;
    next[kStripAmPhase] = (state[kStripAmPhase] & 0xFF8000u) | uint32_t((int64_t(state[kStripAmPhase] & 0x7FFF) + 32 * rate) & 0x7FFF);
    const int64_t x0 = sext(state[7]) + 0x4000;                                   // A1, then X0
    const int64_t acc = (((x0 * x0 * 2) - (int64_t(0x20) << 24)) << 2) + (int64_t(0x10) << 24);   // A1:A0
    const int64_t period = acc >> 24;                                             // X1 = A1
    int64_t count = sext(state[kStripSrrCount]);
    for (int i = 0; i < 32; ++i) { count += 0x10; if (count >= period) count -= period; }
    next[kStripSrrCount] = uint32_t(count) & 0xFFFFFF;
    return next;
}

bool MixEngine::renderBlock(const VoiceEngine::Block& voices, Output& out)
{
    if (m_faulted) { for (auto& f : out) f.fill(0); return false; }
    for (int t = 0; t < VoiceEngine::kTracks; ++t)
        for (int i = 0; i < kFrames; ++i)
            m_dsp->memWrite(MemArea_Y, kVoiceRing + TWord(t * kFrames + i), TWord(voices[size_t(t)][size_t(i)]) & 0xFFFFFF);
    // which strips are idle (see setSkipIdleStrips): the track's output place as the DSP will count it out
    TWord mask = 0, would = 0, mainBlock = 0x200, otherBlock = 0x3E0;
    std::array<TWord, VoiceEngine::kTracks> outPtr{};
    m_skipped = 0;
    for (int t = 0; t < VoiceEngine::kTracks; ++t) {
        auto& s = m_strips[size_t(t)];
        const bool main = (m_mem->get(MemArea_Y, 0x100 + 5 * TWord(t)) & 0xFFFFFF) == 6;
        outPtr[size_t(t)] = main ? mainBlock : otherBlock;
        if (main) mainBlock += 0x20; else otherBlock -= 0x20;
        bool zero = true;
        for (int i = 0; i < kFrames && zero; ++i) zero = (TWord(voices[size_t(t)][size_t(i)]) & 0xFFFFFF) == 0;
        bool skip = zero && s.idle && s.outPtr == outPtr[size_t(t)];
        const TWord base = kStripState + 0x40 * TWord(t);
        for (TWord k = 0; skip && k < 0x40; ++k) skip = m_mem->get(MemArea_Y, base + k) == s.state[k];
        for (TWord k = 0; skip && k < 32; ++k) skip = m_mem->get(MemArea_X, s.outPtr + k) == s.out[k];
        s.ranZero = zero;
        if (skip && m_skipIdle && !m_verifySkips) {   // left out: its two free-running words go on as the strip would take them
            const auto next = idleStep(s.state);
            for (TWord k : {kStripSrrCount, kStripAmPhase})
                if (next[k] != s.state[k]) m_dsp->memWrite(MemArea_Y, base + k, next[k]);
            s.state = next;
            mask |= TWord(1) << t; ++m_skipped;
            continue;
        }
        if (skip) would |= TWord(1) << t;   // (verify: it runs, and has to come out as idleStep says)
        for (TWord k = 0; k < 0x40; ++k) s.state[k] = m_mem->get(MemArea_Y, base + k);   // it runs: the block as it goes in
    }
    m_dsp->memWrite(MemArea_Y, kSkipMask, mask);
    m_dsp->setPC(kBlockStart);
    if (!runToPark(kMaxExecBlock)) { for (auto& f : out) f.fill(0); for (auto& s : m_strips) s.idle = false; return false; }
    for (int t = 0; t < VoiceEngine::kTracks; ++t) {
        if ((mask >> t) & 1) continue;
        auto& s = m_strips[size_t(t)];
        const TWord base = kStripState + 0x40 * TWord(t);
        const auto expect = idleStep(s.state);
        bool same = s.ranZero && s.outPtr == outPtr[size_t(t)];
        for (TWord k = 0; k < 0x40; ++k) { const TWord w = m_mem->get(MemArea_Y, base + k); same = same && w == expect[k]; s.state[k] = w; }
        s.outPtr = outPtr[size_t(t)];
        for (TWord k = 0; k < 32; ++k) { const TWord w = m_mem->get(MemArea_X, s.outPtr + k); same = same && w == s.out[k]; s.out[k] = w; }
        if (((would >> t) & 1) && !same) ++m_skipMismatches;
        s.idle = same;   // zero in, the block as idleStep takes it, the output as it was: the next such block is known
    }
    for (int i = 0; i < kFrames; ++i)
        for (int c = 0; c < kChannels; ++c) {
            const TWord w = m_mem->get(MemArea_X, kDacHalf + TWord(i * kChannels + c)) & 0xFFFFFF;
            out[size_t(i)][size_t(c)] = int32_t(w << 8) >> 8;
        }
    return true;
}

} // namespace mnm::md
