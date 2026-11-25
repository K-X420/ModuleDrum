// md-plugintest's checks of the sequencer extras after the community firmwares' ideas: what a condition decides (trig,
// locks, both), conditions on trigless locks, a track's own condition, WRAP AFTER, CHAIN AFTER, and the track swap
#include <cstdio>
#include <cstring>
#include "MdProcessor.h"
#include "MdEditor.h"
#include "MdSequencer.h"
#include "MdTests.h"

namespace mnm::plugin::md::test {

namespace {
mnm::mddump::Pattern emptyPattern(int position, int length)
{
    mnm::mddump::Pattern p;
    p.position = position; p.length = uint8_t(length); p.accentEditAll = p.slideEditAll = p.swingEditAll = 1;
    for (auto& row : p.locks) std::memset(row, 0xFF, sizeof(row));
    return p;
}
uint64_t steps(std::initializer_list<int> l) { uint64_t m = 0; for (int s : l) m |= 1ull << s; return m; }
std::vector<mnm::md::SeqTrig> play(const mnm::mddump::Pattern& x, double clocks, int64_t maxSteps = -1)
{
    const mnm::md::PatternPlayer pl(x);
    std::vector<mnm::md::SeqTrig> out;
    mnm::md::TrigContext ctx; ctx.extras = true; ctx.seed = 7;
    pl.trigs(0, clocks, out, maxSteps, ctx);
    return out;
}
}

// MD_SEQX_TEST
int mdSeqXTest(TestEnv& env, const char* value)
{
    juce::ignoreUnused(value);
    namespace dd = mnm::mddump;
    int fails = 0;
    auto check = [&](bool ok, const juce::String& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8()); fails += ok ? 0 : 1; };
    const int k12 = dd::kCondRatio;   // 1:2: plays on the first of every two passes

    // ---- what a condition decides
    {
        auto x = emptyPattern(0, 16);
        for (int t = 0; t < 3; ++t) { x.trigs[t] = steps({0}); x.cond[t][0] = uint8_t(k12); x.setLock(t, 0, 0, 99); }
        x.condMode[1][0] = dd::kCondTrigOnly;
        x.condMode[2][0] = dd::kCondLocksOnly;
        const auto out = play(x, 96.0 * 4);   // four passes: the condition passes on passes 1 and 3
        int voice[3] = {}, lockOnly[3] = {}, locked[3] = {};
        for (const auto& e : out) {
            if (e.track > 2) continue;
            if (e.lockOnly) ++lockOnly[e.track]; else ++voice[e.track];
            if (e.locks[0] == 99) ++locked[e.track];
        }
        check(voice[0] == 2 && lockOnly[0] == 0 && locked[0] == 2, "BOTH: on the failing passes neither the sound nor the locks");
        check(voice[1] == 2 && lockOnly[1] == 2 && locked[1] == 4, "TRIG: the failing passes still set the locks (no sound)");
        check(voice[2] == 4 && locked[2] == 2, "LOCKS: the sound every pass, the locks only when the condition passes");
    }
    // ---- a condition on a trigless lock; a track's own condition, and ALWAYS
    {
        auto x = emptyPattern(0, 16);
        x.lockTrigs[0] = steps({4}); x.setLock(0, 0, 4, 77); x.cond[0][4] = uint8_t(k12);
        x.trigs[1] = steps({0, 8}); x.trackCond[1] = uint8_t(k12); x.cond[1][8] = dd::kCondAlways;
        x.lockTrigs[2] = steps({2}); x.setLock(2, 0, 2, 50); x.trackCond[2] = uint8_t(k12);   // a trigless lock ignores the track's
        const auto out = play(x, 96.0 * 4);
        int t0 = 0, t1s0 = 0, t1s8 = 0, t2 = 0;
        for (const auto& e : out) {
            if (e.track == 0 && e.lockOnly) ++t0;
            if (e.track == 1 && e.step == 0) ++t1s0;
            if (e.track == 1 && e.step == 8) ++t1s8;
            if (e.track == 2 && e.lockOnly) ++t2;
        }
        check(t0 == 2, "a trigless lock with 1:2: on every other pass (" + juce::String(t0) + " of 4)");
        check(t1s0 == 2 && t1s8 == 4, "the track's 1:2 on a trig without one; ALWAYS opts out (" + juce::String(t1s0) + ", " + juce::String(t1s8) + ")");
        check(t2 == 4, "a trigless lock takes only its own condition, not the track's");
    }
    // ---- WRAP AFTER: every track starts over together
    {
        auto x = emptyPattern(0, 16);
        x.trigs[0] = steps({0}); x.trackLen[0] = 3;   // T1: a 3-step loop, its trig on step 1
        x.wrapAfter = uint16_t(8 + 1);                // every 8 steps
        auto at = [](const std::vector<mnm::md::SeqTrig>& v) { std::vector<double> c; for (const auto& e : v) if (e.track == 0) c.push_back(e.clock); return c; };
        const auto wrapped = at(play(x, 96.0));
        x.wrapAfter = 0;
        const auto free = at(play(x, 96.0));
        // free: steps 0 3 6 9 12 15 (clocks 0 18 36 54 72 90); wrapped at 8: 0 3 6 | 8 11 14 (clocks 0 18 36 48 66 84)
        check(free.size() == 6 && std::abs(free[3] - 54.0) < 1e-6, "WRAP OFF: the 3-step loop runs on (step 10 at clock 54)");
        check(wrapped.size() == 6 && std::abs(wrapped[3] - 48.0) < 1e-6 && std::abs(wrapped[5] - 84.0) < 1e-6,
              "WRAP 8: it starts over at step 9 (clock 48, then 66, 84)");
        x.wrapAfter = dd::kWrapLongest; x.trigs[1] = steps({0}); x.trackLen[1] = 5;   // the longest loop: 5 steps
        const mnm::md::PatternPlayer pl(x);
        check(std::abs(pl.wrapClocks(true) - 30.0) < 1e-6, "WRAP LONGEST: the 5-step track's loop (30 clocks)");
        x.chainAfter = uint16_t(4 + 1);
        check(std::abs(mnm::md::PatternPlayer(x).chainClocks(true) - 24.0) < 1e-6 && std::abs(mnm::md::PatternPlayer(x).chainClocks(false) - 24.0) < 1e-6,
              "CHAIN 4: a change every 4 steps");
    }
    // ---- the codec: version 3 carries the targets, the track's condition, WRAP and CHAIN
    {
        auto x = emptyPattern(5, 16);
        x.trigs[0] = steps({0}); x.cond[0][0] = uint8_t(k12); x.condMode[0][0] = dd::kCondLocksOnly;
        x.trackCond[3] = 11; x.wrapAfter = 1025; x.chainAfter = dd::kWrapShortest;
        const auto m = dd::encodePatternExtras(x);
        auto back = emptyPattern(5, 16);
        back.trigs[0] = x.trigs[0];
        int pos = -1;
        check(m[5] == 3 && dd::decodePatternExtras(m.data(), m.size(), pos, back) && back.condMode[0][0] == dd::kCondLocksOnly && back.trackCond[3] == 11
              && back.wrapAfter == 1025 && back.chainAfter == dd::kWrapShortest, "version 3 keeps the target, the track's condition, WRAP 1024, CHAIN SHORTEST");
        auto plain = emptyPattern(5, 16);
        plain.trigs[0] = 1; plain.cond[0][0] = 3;
        check(dd::encodePatternExtras(plain)[5] == 1, "without them: still version 1");
    }
    // ---- the track swap: a pattern's tracks
    {
        auto x = emptyPattern(0, 16);
        x.trigs[2] = steps({1, 5}); x.setLock(2, 3, 5, 44); x.cond[2][1] = 7; x.trackLen[2] = 12;
        x.trigs[6] = steps({0}); x.setLock(6, 0, 0, 10);
        x.swapTracks(2, 6);
        check(x.trigs[6] == steps({1, 5}) && x.trigs[2] == steps({0}), "the trigs trade tracks");
        check(x.lockRow(6, 3) >= 0 && x.locks[x.lockRow(6, 3)][5] == 44 && x.lockRow(2, 0) >= 0 && x.locks[x.lockRow(2, 0)][0] == 10 && x.lockRow(2, 3) < 0,
              "so do the locks");
        check(x.cond[6][1] == 7 && x.trackLen[6] == 12 && x.trackLen[2] == 0, "and the extras");
    }
    // ---- the processor: the kit side; the editor: a swap moves the sequence on the kit's patterns
    {
        auto& p = env.proc;
        dd::Dump d;
        dd::Kit kit;
        for (int tr = 0; tr < 16; ++tr) { kit.trigGroups[tr] = kGroupOff; kit.muteGroups[tr] = kGroupOff; kit.levels[tr] = 100; kit.lfos[tr][0] = uint8_t(tr); }
        kit.models[0] = 1; kit.params[0][0] = 11;
        kit.models[1] = uint32_t(kMachines[5].id); kit.params[1][0] = 22;
        kit.lfos[3][0] = 0;   // T4's LFO aims at T1
        kit.trigGroups[5] = 1;   // T6 trigs T2
        d.kits.push_back(kit);
        auto x = emptyPattern(0, 16); x.trigs[0] = steps({0, 4}); x.kit = 0; d.patterns.push_back(x);
        auto y = emptyPattern(1, 16); y.trigs[1] = steps({2}); y.kit = 0; d.patterns.push_back(y);
        auto z = emptyPattern(2, 16); z.trigs[0] = steps({9}); z.kit = 3; d.patterns.push_back(z);   // another kit: untouched
        p.loadMdKit("swap", kit, "SWAP");
        p.setPatternBank("t", "T", d, 0);
        p.setSceneValue(4, MdProcessor::sceneSlotOf(0, 2), 88);
        std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
        auto* med = dynamic_cast<MdEditor*>(ed.get());
        med->devSelectTrack(0);
        med->devSwapTrack(1);
        const auto k = p.captureMdKit();
        check(k.model(1) == 1 && k.params[1][0] == 11 && k.model(0) == kMachines[5].id && k.params[0][0] == 22, "T1 <> T2: the machines and knobs trade");
        check(k.lfos[3][0] == 1 && k.trigGroups[5] == 0, "an LFO and a trig group aimed at T1 now aim at T2 (and back)");
        check(p.bankPattern(0)->trigs[1] == steps({0, 4}) && p.bankPattern(1)->trigs[0] == steps({2}), "every pattern on the kit: its T1 / T2 steps trade");
        check(p.bankPattern(2)->trigs[0] == steps({9}), "a pattern on another kit: untouched");
        check(p.sceneValue(4, MdProcessor::sceneSlotOf(1, 2)) == 88 && p.sceneValue(4, MdProcessor::sceneSlotOf(0, 2)) == -1, "the scene locks follow");
        check(med->devTrack() == 1, "the selection follows the sound");
        med->devSwapTrack(-1);
        check(p.captureMdKit().model(0) == 1 && p.bankPattern(0)->trigs[0] == steps({0, 4}), "the swap again: back as it was");
        // the C window: Alt+wheel sets what the condition decides, Shift+wheel the track's own condition
        med->showGrid(-1);
        med->devMarkMode(4);
        med->devExtraPaint(0, 0, true, true);
        med->devExtraAltWheel(0, 0, 1);
        check(p.bankPattern(0)->condMode[0][0] == dd::kCondTrigOnly, "Alt+wheel: the condition decides the trig only");
        med->devExtraWheel(3, 0, 1, true);
        check(p.bankPattern(0)->trackCond[0] == 1, "Shift+wheel: the track's own condition (1%)");
        check(med->devGrid().trackCond == 1, "the keys show it");
    }
    // ---- CHAIN AFTER, live: pattern 2 picked at step 3 of pattern 1 takes over at step 5 (CHAIN 4), not after the pass
    {
        struct Head : juce::AudioPlayHead {
            double ppq = 0; bool playing = true;
            juce::Optional<PositionInfo> getPosition() const override { PositionInfo q; q.setPpqPosition(ppq); q.setBpm(120); q.setIsPlaying(playing); return q; }
        } head;
        dd::Dump d;
        dd::Kit kit;
        for (int tr = 0; tr < 16; ++tr) { kit.trigGroups[tr] = kGroupOff; kit.muteGroups[tr] = kGroupOff; kit.levels[tr] = 100; }
        kit.models[0] = 1; kit.models[1] = 1;
        d.kits.push_back(kit);
        auto x = emptyPattern(0, 16); x.trigs[0] = steps({0, 8}); x.chainAfter = uint16_t(4 + 1); d.patterns.push_back(x);
        auto y = emptyPattern(1, 16); y.trigs[1] = ~0ull >> 48; d.patterns.push_back(y);
        auto proc = std::make_unique<MdProcessor>();
        auto& p = *proc;
        p.setFirmwarePath(juce::String(env.argv[1]), false);
        p.setPlayHead(&head);
        p.prepareToPlay(env.rate, env.block);
        p.loadMdKit("chain", kit, "CHAIN");
        p.setPatternBank("c", "C", d, 0);
        auto setParam = [&](const juce::String& id, float v) { if (auto* q = p.apvts.getParameter(id)) q->setValueNotifyingHost(q->convertTo0to1(v)); };
        setParam(seqId(), 1.0f); setParam(patternId(), 0.0f); setParam(extrasId(), 1.0f);
        const double spc = env.rate * 60.0 / 120.0 / 24.0;
        juce::AudioBuffer<float> buf(2, env.block);
        auto run = [&](double clocks) {
            for (int i = 0; i < int(clocks * spc / env.block); ++i) { juce::MidiBuffer m; buf.clear(); p.processBlock(buf, m); head.ppq += env.block / spc / 24.0; }
        };
        run(96 + 12);   // a pass, then to step 3
        p.setTrigLogging(true);
        setParam(patternId(), 1.0f);
        run(60);
        bool t1Step9 = false; int firstT2 = -1;
        for (const auto& e : p.trigLog()) {
            if (e.track == 0 && e.step == 8) t1Step9 = true;
            if (e.track == 1 && firstT2 < 0) firstT2 = e.step;
        }
        check(!t1Step9 && firstT2 == 0, "CHAIN 4: pattern 2 from step 5 on, before pattern 1's step 9 (first T2 step " + juce::String(firstT2 + 1) + ")");
    }
    std::printf(fails ? "SEQX TEST FAILED (%d)\n" : "SEQX TEST OK\n", fails);
    return fails ? 1 : 0;
}

} // namespace mnm::plugin::md::test
