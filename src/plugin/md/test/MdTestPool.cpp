// md-plugintest's checks of the sound pool and sound locks, the scenes in the Library project and their undo, LOCAL
// and the pattern change modes
#include <cstdio>
#include <cstring>
#include "MdProcessor.h"
#include "MdEditor.h"
#include "MdSequencer.h"
#include "MdTests.h"

namespace mnm::plugin::md::test {

namespace {
mnm::mddump::Pattern blankPattern(int position, int length)
{
    mnm::mddump::Pattern p;
    p.position = position; p.length = uint8_t(length); p.accentEditAll = p.slideEditAll = p.swingEditAll = 1;
    for (auto& row : p.locks) std::memset(row, 0xFF, sizeof(row));
    return p;
}
uint64_t stepBits(std::initializer_list<int> l) { uint64_t m = 0; for (int s : l) m |= 1ull << s; return m; }

struct Head : juce::AudioPlayHead {
    double ppq = 0; bool playing = true;
    juce::Optional<PositionInfo> getPosition() const override { PositionInfo q; q.setPpqPosition(ppq); q.setBpm(120); q.setIsPlaying(playing); return q; }
};
}

// MD_POOL_TEST
int mdPoolTest(TestEnv& env, const char* value)
{
    juce::ignoreUnused(value);
    namespace dd = mnm::mddump;
    int fails = 0;
    auto check = [&](bool ok, const juce::String& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8()); fails += ok ? 0 : 1; };
    const int poolMachine = kMachines[5].id, ownMachine = 1;

    // ---- the codec
    {
        auto x = blankPattern(2, 16);
        x.trigs[0] = stepBits({0, 8}); x.cond[0][0] = 5;
        check(dd::encodePatternExtras(x)[5] == 1, "no sound lock: the extras message stays at version 1");
        x.soundLock[0][8] = 4;
        const auto m = dd::encodePatternExtras(x);
        check(m[5] == 3, "a sound lock: version 3");
        dd::Pattern back = blankPattern(2, 16);
        back.trigs[0] = x.trigs[0];
        int pos = -1;
        check(dd::decodePatternExtras(m.data(), m.size(), pos, back) && pos == 2 && back.soundLock[0][8] == 4 && back.cond[0][0] == 5,
              "it decodes back with the other extras");
        dd::SceneData sc; sc.index = 3; sc.locks = {{0, 100}, {uint16_t(dd::kSetSceneSlots - 1), 7}};
        dd::PoolSound ps; ps.slot = 9; ps.name = "SNARE ROLL"; ps.model = uint32_t(poolMachine); ps.params[0] = 99; ps.lfo[5] = 200; ps.track = 4;
        std::vector<uint8_t> bytes = dd::encodePattern(x);
        for (const auto& b : {dd::encodePatternExtras(x), dd::encodeScene(sc), dd::encodePoolSound(ps)}) bytes.insert(bytes.end(), b.begin(), b.end());
        const auto d = dd::parseDump(bytes.data(), bytes.size(), "t");
        check(d.scenes.size() == 1 && d.scenes[0].index == 3 && d.scenes[0].locks.size() == 2 && d.scenes[0].locks[1].first == dd::kSetSceneSlots - 1,
              "a scene message reads back");
        check(d.pool.size() == 1 && d.pool[0].slot == 9 && d.pool[0].name == "SNARE ROLL" && int(d.pool[0].model) == poolMachine && d.pool[0].params[0] == 99
              && d.pool[0].lfo[5] == 200 && d.pool[0].track == 4, "a pool sound reads back (name, machine, knobs, LFO bytes over 127)");
        const auto full = dd::encodeDump(d, true), pure = dd::encodeDump(d, false);
        const auto again = dd::parseDump(full.data(), full.size(), "t"), md = dd::parseDump(pure.data(), pure.size(), "t");
        check(again.scenes.size() == 1 && again.pool.size() == 1 && md.scenes.empty() && md.pool.empty(), "the set data stays in the Library: out of a pure MD export");
    }

    // ---- the processor: a sound lock plays the pool sound from its trig to the track's next
    Head head;
    dd::Dump d;
    dd::Kit kit;
    for (int tr = 0; tr < 16; ++tr) { kit.trigGroups[tr] = kGroupOff; kit.muteGroups[tr] = kGroupOff; kit.levels[tr] = 100; kit.params[tr][17] = 100; }
    kit.models[0] = uint32_t(ownMachine); kit.params[0][0] = 64;
    d.kits.push_back(kit);
    {
        auto x = blankPattern(0, 16);
        x.trigs[0] = stepBits({0, 8});
        x.soundLock[0][8] = 1;   // pool slot 0 on step 9
        d.patterns.push_back(x);
        auto y = blankPattern(1, 16);   // pattern 2: T2 on every step
        y.trigs[1] = ~0ull >> 48;
        d.patterns.push_back(y);
    }
    auto pHeap = std::make_unique<MdProcessor>();
    auto& p = *pHeap;
    p.setFirmwarePath(juce::String(env.argv[1]), false);
    p.setPlayHead(&head);
    p.prepareToPlay(env.rate, env.block);
    p.loadMdKit("pool", d.kits[0], "POOL");
    p.setPatternBank("test", "POOL", d, 0);
    auto setParam = [&](const juce::String& id, float v) { if (auto* q = p.apvts.getParameter(id)) q->setValueNotifyingHost(q->convertTo0to1(v)); };
    setParam(seqId(), 1.0f);
    setParam(patternId(), 0.0f);
    setParam(extrasId(), 1.0f);
    {
        dd::PoolSound ps; ps.name = "POOLED"; ps.model = uint32_t(poolMachine); ps.params[0] = 20; ps.track = 0;
        p.setPoolSound(0, &ps);
    }
    const double spc = env.rate * 60.0 / 120.0 / 24.0;
    juce::AudioBuffer<float> buf(2, env.block);
    std::vector<int> lockAt, machineAt, ptchAt;
    auto run = [&](double clocks, const std::function<void(juce::MidiBuffer&, int)>& midiAt = {}) {
        const int k = int(clocks * spc / env.block);
        for (int i = 0; i < k; ++i) {
            juce::MidiBuffer midi;
            if (midiAt) midiAt(midi, i);
            buf.clear(); p.processBlock(buf, midi);
            head.ppq += env.block / spc / 24.0;
            lockAt.push_back(p.soundLockOf(0)); machineAt.push_back(p.devTrackMachine(0)); ptchAt.push_back(p.devSceneParam(0, 0));
        }
    };
    run(96);   // one pass
    lockAt.clear(); machineAt.clear(); ptchAt.clear();
    run(96);   // the second pass: step 1 own sound, step 9 the pool's
    auto at = [&](const std::vector<int>& v, double clock) { return v[size_t(std::min(v.size() - 1, size_t(clock * spc / env.block)))]; };
    check(at(lockAt, 24) == -1 && at(machineAt, 24) == ownMachine && at(ptchAt, 24) == 64, "steps 1-8: the track's own sound (PTCH 64)");
    check(at(lockAt, 72) == 0 && at(machineAt, 72) == poolMachine && at(ptchAt, 72) == 20,
          "from step 9: the pool sound (its machine, PTCH 20; " + juce::String(at(machineAt, 72)) + ", " + juce::String(at(ptchAt, 72)) + ")");
    run(24);
    check(at(lockAt, 120) == -1 && at(machineAt, 120) == ownMachine, "the next trig without a lock: its own sound again");
    setParam(extendedId(), 0.0f);   // CLASSIC: no sound locks (as no locks)
    lockAt.clear(); machineAt.clear(); ptchAt.clear();
    run(96);
    check(std::none_of(lockAt.begin(), lockAt.end(), [](int v) { return v >= 0; }), "CLASSIC: the pattern plays without its sound locks");
    setParam(extendedId(), 1.0f);

    // ---- the plugin state, and the Library project, keep the pool and the scenes
    p.setSceneValue(2, MdProcessor::sceneSlotOf(0, 0), 111);
    {
        juce::MemoryBlock st;
        p.getStateInformation(st);
        MdProcessor other;
        other.setStateInformation(st.getData(), int(st.getSize()));
        const auto pool = other.soundPool();
        check(pool->used[0] && pool->s[0].name == "POOLED" && int(pool->s[0].model) == poolMachine, "the plugin state keeps the pool");
        const auto bank = p.bankDump();
        check(bank.pool.size() == 1 && bank.scenes.size() == 1 && bank.scenes[0].index == 2, "the bank's project dump carries the pool and the scenes");
        MdProcessor third;
        third.setPatternBank("x", "X", bank, 0);
        check(third.sceneValue(2, MdProcessor::sceneSlotOf(0, 0)) == 111 && third.soundPool()->used[0], "loading that project brings them back");
    }

    // ---- the editor: the P window, and scene undo
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
        auto* med = dynamic_cast<MdEditor*>(ed.get());
        med->showGrid(-1);
        med->devMarkMode(9);
        med->devExtraPaint(0, 3, true, true);
        check(p.bankPattern(0)->soundLock[0][0] == 1, "P window: a click gives the trig the selected pool sound (slot 1)");
        med->devExtraWheel(0, 3, 1, false);
        check(p.bankPattern(0)->soundLock[0][0] == 2, "the wheel steps to the next slot");
        check(med->devGrid().sound[0] == 2, "the key shows it");
        med->devExtraPaint(3, 3, true, true);
        check(p.bankPattern(0)->soundLock[0][3] == 0, "no trig on the step: no sound lock");
        med->devMarkMode(0);
        // scene undo
        med->devToggleScenes();
        med->devHoldScene(5);
        for (int v = 60; v <= 70; ++v) med->devSynPage().devTurn(0, v);
        check(p.sceneValue(5, MdProcessor::sceneSlotOf(0, 0)) == 70, "a scene lock set");
        med->devUndo();
        check(p.sceneValue(5, MdProcessor::sceneSlotOf(0, 0)) == -1, "Ctrl+Z: the whole turn undone in one step");
        med->devRedo();
        check(p.sceneValue(5, MdProcessor::sceneSlotOf(0, 0)) == 70, "redo: back");
        med->devHoldScene(-1);
        med->devToggleScenes();
    }

    // ---- LOCAL OFF: the knobs only go out; what comes in is the sound (stopped: no sound lock in the way)
    head.playing = false;
    {
        auto ms = p.midiSettings();
        ms.local = false; ms.baseChannel = 0;
        p.setMidiSettings(ms);
        run(2);                          // (a kit load still pending is the sound, LOCAL or not)
        setParam(knobId(0, 1), 90.0f);   // the plugin's own knob (DEC)
        run(12);
        check(p.devSceneParam(0, 1) != 90, "LOCAL OFF: the plugin's knob does not change the sound (" + juce::String(p.devSceneParam(0, 1)) + ")");
        run(6, [](juce::MidiBuffer& m, int i) { if (i == 0) m.addEvent(juce::MidiMessage::controllerEvent(1, 17, 33), 0); });   // T1 DEC in (CC 16 + 1)
        check(p.devSceneParam(0, 1) == 33, "a CC coming in sets the sound (" + juce::String(p.devSceneParam(0, 1)) + ")");
        ms.local = true;
        p.setMidiSettings(ms);
        setParam(knobId(0, 1), 50.0f);
        run(6);
        check(p.devSceneParam(0, 1) == 50, "LOCAL ON: the knob is the sound again");
    }

    head.playing = true;

    // ---- PATTERN CHANGE: pattern 2 (T2 on every step) picked at step 5 of pattern 1 (T1 on steps 1 and 9)
    struct After { int firstT2Step = -1; bool t1Before = false; bool t1Back = false; int slot = -1; };
    auto pick = [&](int mode) {
        setParam(ptnChangeId(), float(kChangeSequential));
        setParam(patternId(), 0.0f);
        run(96 * 2);                             // pattern 1 again (a SEQUENTIAL pick takes a pass)
        head.ppq = std::ceil(head.ppq / 4.0) * 4.0 + 4.0;   // a bar line (a jump: placed again from there)
        run(96 + 24);                            // a pass, then to step 5
        setParam(ptnChangeId(), float(mode));
        p.setTrigLogging(true);
        setParam(patternId(), 1.0f);
        run(72 + 96 + 12);
        After r;
        bool seenT2 = false;
        int t2Count = 0;
        for (const auto& e : p.trigLog()) {
            if (e.track == 1) { if (!seenT2) r.firstT2Step = e.step; seenT2 = true; ++t2Count; }
            if (e.track == 0 && !seenT2) r.t1Before = true;
            if (e.track == 0 && seenT2 && t2Count >= 15) r.t1Back = true;
        }
        r.slot = p.devPlayingSlot();
        p.setTrigLogging(false);
        return r;
    };
    {
        auto r = pick(kChangeSequential);
        check(r.t1Before && r.firstT2Step == 0, "SEQUENTIAL: pattern 1 plays on (its step 9), pattern 2 from step 1 at the end of the pass");
        r = pick(kChangeDirectStart);
        check(!r.t1Before && r.firstT2Step == 0, "DIRECT START: pattern 2 at once, from step 1 (first T2 step " + juce::String(r.firstT2Step + 1) + ")");
        r = pick(kChangeDirectJump);
        check(!r.t1Before && r.firstT2Step >= 4 && r.firstT2Step <= 6, "DIRECT JUMP: at once, at the same position (step " + juce::String(r.firstT2Step + 1) + ")");
        r = pick(kChangeTempJump);
        check(!r.t1Before && r.firstT2Step == 0 && r.t1Back && r.slot == 0,
              "TEMP JUMP: at once from step 1, one pass, then pattern 1 again (playing " + juce::String(r.slot + 1) + ")");
    }

    std::printf(fails ? "POOL TEST FAILED (%d)\n" : "POOL TEST OK\n", fails);
    return fails ? 1 : 0;
}

} // namespace mnm::plugin::md::test
