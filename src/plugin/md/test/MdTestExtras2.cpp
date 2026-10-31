// md-plugintest's checks of the second set of EXTRAS: a retrig's fade, a track's own speed, GENERATE's Euclidean
// generators and trigless locks, from the codec through the sequencer and the processor to GRID
#include <cstdio>
#include <cstring>
#include "MdProcessor.h"
#include "MdEditor.h"
#include "MdSequencer.h"
#include "MdTests.h"

namespace mnm::plugin::md::test {

namespace {
mnm::mddump::Pattern blank(int position, int length)
{
    mnm::mddump::Pattern p;
    p.position = position; p.length = uint8_t(length); p.accentEditAll = p.slideEditAll = p.swingEditAll = 1;
    for (auto& row : p.locks) std::memset(row, 0xFF, sizeof(row));
    return p;
}
uint64_t bits(std::initializer_list<int> l) { uint64_t m = 0; for (int s : l) m |= 1ull << s; return m; }
}

// MD_XTRA2_TEST
int mdXtra2Test(TestEnv& env, const char* value)
{
    juce::ignoreUnused(value);
    namespace dd = mnm::mddump;
    int fails = 0;
    auto check = [&](bool ok, const juce::String& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8()); fails += ok ? 0 : 1; };

    // ---- the codec: version 1 while only the first set is used; version 2 carries the rest
    {
        auto x = blank(3, 16);
        x.trigs[0] = 1; x.retrig[0][0] = dd::makeRetrig(3, 1);
        check(dd::encodePatternExtras(x)[5] == 1, "only the first extras: a version 1 message (plugins before 1.4 read it)");
        x.retrigFade[0][0] = -40;
        x.lockTrigs[1] = bits({4});
        x.setLock(1, 0, 4, 99);
        x.trackSpeed[2] = 1;
        x.trackLen[2] = 12;
        x.euclid[3] = {5, 3, 1, 2, 2};
        const auto ex = dd::encodePatternExtras(x);
        check(ex[5] == 2, "with a fade, a trigless lock, a speed or generators: version 2");
        auto bytes = dd::encodePattern(x);
        bytes.insert(bytes.end(), ex.begin(), ex.end());
        const auto d = dd::parseDump(bytes.data(), bytes.size(), "x");
        const bool back = d.patterns.size() == 1 && d.patterns[0].retrigFade[0][0] == -40 && d.patterns[0].lockTrigs[1] == bits({4})
                       && d.patterns[0].trackSpeed[2] == 1 && d.patterns[0].trackLen[2] == 12 && d.patterns[0].euclid[3] == x.euclid[3]
                       && d.patterns[0].retrig[0][0] == dd::makeRetrig(3, 1);
        check(back && d.numUnknown == 0, "they parse back onto the pattern");
        check(dd::encodeDump(d) == bytes, "and re-encode byte for byte");
        auto c = blank(0, 16);
        c.copySteps(x, 0, 0, 16, -1, -1);
        check(c.retrigFade[0][0] == -40 && c.lockTrigs[1] == bits({4}), "copied steps take their fade and trigless locks");
        c.clearSteps(0, 16, -1);
        check(c.lockTrigs[1] == 0 && c.retrigFade[0][0] == 0, "cleared steps lose them");
    }

    // ---- the helpers: Euclidean sets, fades, speeds
    {
        dd::Pattern::Euclid e;
        e.pulses1 = 3;
        check(dd::euclidSteps(e, 8) == bits({0, 3, 6}), "EUCLID 3 of 8: x..x..x.");
        e.pulses1 = 4; e.pulses2 = 2;
        const uint64_t a = bits({0, 4, 8, 12}), b = bits({0, 8});
        e.op = 0; const bool orOk = dd::euclidSteps(e, 16) == (a | b);
        e.op = 1; const bool andOk = dd::euclidSteps(e, 16) == (a & b);
        e.op = 2; const bool xorOk = dd::euclidSteps(e, 16) == (a ^ b);
        e.op = 3; const bool subOk = dd::euclidSteps(e, 16) == (a & ~b);
        check(orOk && andOk && xorOk && subOk, "4 with 2: OR, AND, XOR, 1-2");
        e.pulses2 = 0; e.rot1 = 1;
        check(dd::euclidSteps(e, 16) == bits({1, 5, 9, 13}), "rotation 1 turns the first generator a step on");
        e.rot1 = 0;
        check(dd::euclidSteps(e, 32) == bits({0, 4, 8, 12, 16, 20, 24, 28}), "two pages: the pulses on each page");
        check(std::abs(dd::retrigGain(-63, 3, 4)) < 1e-9 && std::abs(dd::retrigGain(-63, 0, 4) - 1.0) < 1e-9
              && std::abs(dd::retrigGain(63, 0, 4)) < 1e-9 && std::abs(dd::retrigGain(63, 3, 4) - 1.0) < 1e-9 && dd::retrigGain(0, 2, 4) == 1.0,
              "fade -63: from the trig's level to silence; +63: from silence up to it");
        auto p = blank(0, 16);
        p.trackSpeed[0] = 1; p.trackSpeed[1] = 6; p.trackSpeed[2] = 3;
        check(mnm::md::PatternPlayer::trackStepClocks(p, 0, true) == 3.0 && mnm::md::PatternPlayer::trackStepClocks(p, 1, true) == 48.0
              && mnm::md::PatternPlayer::trackStepClocks(p, 2, true) == 8.0 && mnm::md::PatternPlayer::trackStepClocks(p, 0, false) == 6.0,
              "speeds at 1x: 2X 3 clocks a step, 1/8X 48, 3/4X 8; EXTRAS off: 6");
    }

    // ---- the sequencer: speeds, trigless locks, fades (16 steps of 6 clocks a pass)
    {
        auto x = blank(0, 16);
        x.trigs[0] = ~0ull >> 48; x.trackSpeed[0] = 1;                      // T1 every step, 2X
        x.trigs[1] = ~0ull >> 48; x.trackSpeed[1] = 4;                      // T2 every step, 1/2X
        x.trigs[2] = bits({0}); x.lockTrigs[2] = bits({8}); x.setLock(2, 0, 8, 77);   // T3: a trig, a trigless lock on 9
        x.trigs[3] = bits({0}); x.retrig[3][0] = dd::makeRetrig(3, 1); x.retrigFade[3][0] = -63;   // T4: 4 hits fading out
        x.trigs[4] = bits({2}); x.lockTrigs[4] = bits({2});                  // a trig wins over a trigless lock on its step
        const mnm::md::PatternPlayer pl(x);
        auto play = [&](bool extras, int64_t maxSteps) {
            std::vector<mnm::md::SeqTrig> out;
            mnm::md::TrigContext ctx; ctx.extras = extras; ctx.seed = 7;
            pl.trigs(0, 96.0 * 4, out, maxSteps, ctx);
            return out;
        };
        const auto on = play(true, -1), off = play(false, -1), ended = play(true, 16);
        int n[5] = {}, lockOnly = 0, t2late = 0;
        bool t1Ok = true, lockOk = false, fadeOk = true;
        std::vector<double> gains;
        for (const auto& e : on) {
            if (e.lockOnly) { ++lockOnly; if (e.track == 2) lockOk = std::abs(std::fmod(e.clock, 96.0) - 48.0) < 1e-9 && e.locks[0] == 77; continue; }
            if (e.track < 5) ++n[e.track];
            if (e.track == 0) t1Ok = t1Ok && std::abs(e.clock - 3.0 * double(e.stepIndex)) < 1e-9;
            if (e.track == 1 && e.clock >= 96.0) ++t2late;
            if (e.track == 3 && e.clock < 96.0) gains.push_back(e.gain);
        }
        check(n[0] == 4 * 32 && t1Ok, "2X: T1's 16 steps twice a pass, 3 clocks apart (" + juce::String(n[0]) + " in 4 passes)");
        check(n[1] == 4 * 8, "1/2X: T2's steps once every two passes (" + juce::String(n[1]) + " in 4 passes)");
        check(lockOnly == 4 && lockOk, "a trigless lock: its locks at step 9, no voice (" + juce::String(lockOnly) + " in 4 passes)");
        check(n[2] == 4 && n[4] == 4, "T3's trig, and T5's trig over the trigless lock on its step");
        fadeOk = gains.size() == 4 && std::abs(gains[0] - 1.0) < 1e-9 && std::abs(gains[3]) < 1e-9 && gains[1] > gains[2];
        check(fadeOk, "the fading retrig: 1, 2/3, 1/3, 0");
        int offN0 = 0, offLock = 0;
        for (const auto& e : off) { offN0 += e.track == 0; offLock += e.lockOnly; }
        check(offN0 == 4 * 16 && offLock == 0, "EXTRAS off: T1 at the pattern's speed, no trigless locks");
        int endN0 = 0;
        for (const auto& e : ended) endN0 += e.track == 0 && e.clock < 96.0 && !e.lockOnly;
        check(endN0 == 32 && std::none_of(ended.begin(), ended.end(), [](const auto& e) { return e.clock >= 96.0 && !e.retrig; }),
              "a segment of one pass: a 2X track ends with it");
        juce::ignoreUnused(t2late);
    }

    // ---- the processor and GRID
    {
        struct Head : juce::AudioPlayHead {
            double ppq = 0; bool playing = true;
            juce::Optional<PositionInfo> getPosition() const override { PositionInfo q; q.setPpqPosition(ppq); q.setBpm(120); q.setIsPlaying(playing); return q; }
        } head;
        dd::Dump d;
        dd::Kit kit;
        for (int tr = 0; tr < 16; ++tr) { kit.trigGroups[tr] = kGroupOff; kit.muteGroups[tr] = kGroupOff; kit.levels[tr] = 100; kit.params[tr][17] = 100; }
        for (int tr = 0; tr < 2; ++tr) { kit.models[tr] = 1; kit.params[tr][0] = 64; kit.params[tr][1] = 30; }
        d.kits.push_back(kit);
        auto x = blank(0, 16);
        x.trigs[0] = bits({0}); x.lockTrigs[0] = bits({8}); x.setLock(0, 0, 8, 100);   // T1: PTCH 100 from step 9, no trig there
        x.trigs[1] = ~0ull >> 48; x.trackSpeed[1] = 1;                              // T2: 2X
        d.patterns.push_back(x);
        auto pHeap = std::make_unique<MdProcessor>();
        auto& p = *pHeap;
        p.setFirmwarePath(juce::String(env.argv[1]), false);
        p.setPlayHead(&head);
        p.prepareToPlay(env.rate, env.block);
        p.loadMdKit("xtra2", d.kits[0], "XTRA2");
        p.setPatternBank("test", "XTRA2", d, 0);
        auto setParam = [&](const juce::String& id, float v) { if (auto* q = p.apvts.getParameter(id)) q->setValueNotifyingHost(q->convertTo0to1(v)); };
        setParam(seqId(), 1.0f);
        setParam(patternId(), 0.0f);
        setParam(extrasId(), 1.0f);
        const double spc = env.rate * 60.0 / 120.0 / 24.0;
        juce::AudioBuffer<float> buf(2, env.block);
        std::vector<int> base;
        auto run = [&](double clocks) {
            const int k = int(clocks * spc / env.block);
            for (int i = 0; i < k; ++i) {
                juce::MidiBuffer midi; buf.clear(); p.processBlock(buf, midi);
                head.ppq += env.block / spc / 24.0;
                base.push_back((p.engineForTests()->cpu().baseParam(0, 0) + 64) >> 7);
            }
        };
        run(144);   // a pass and a half in (the engine renders a little ahead of the log)
        p.setTrigLogging(true);
        base.clear();
        run(96);    // clocks 144..240: the trig at 192, the trigless lock at 240 - 48 = 192 + 48
        int t1 = 0, t2 = 0;
        for (const auto& e : p.trigLog()) { t1 += e.track == 0; t2 += e.track == 1; }
        auto baseAt = [&](double clock) { return base[size_t(std::min(base.size() - 1, size_t(clock * spc / env.block)))]; };
        check(t1 == 1, "a trigless lock plays no voice (T1: " + juce::String(t1) + " trig a pass)");
        check(baseAt(15) == 100 && baseAt(80) == 64, "it sets PTCH 100 at step 9, the next trig lets it go (step 11: " + juce::String(baseAt(15)) + ", step 6: " + juce::String(baseAt(80)) + ")");
        check(std::abs(t2 - 32) <= 1, "2X: T2 plays 32 trigs a pass (" + juce::String(t2) + ")");

        std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
        auto* med = dynamic_cast<MdEditor*>(ed.get());
        med->showGrid(-1);
        auto pat = [&] { return p.bankPattern(0); };
        // a held empty step + a knob turn: with EXTRAS a trigless lock (without: a trig, as the MD)
        med->devHold(4);
        med->devSynPage().devTurn(0, 90);
        med->devHold(-1);
        check(pat()->lockTrigs[0] == bits({4, 8}) && !((pat()->trigs[0] >> 4) & 1), "a knob turned on a held empty step: a trigless lock there");
        check((med->devGrid().lockTrigs & bits({4})) != 0, "GRID shows it");
        med->devPaint(4, true, true);
        check(!((pat()->lockTrigs[0] >> 4) & 1) && !((pat()->trigs[0] >> 4) & 1) && pat()->lockRow(0, 0) >= 0 && pat()->locks[pat()->lockRow(0, 0)][4] > 127,
              "a click on it takes it off, with its locks");
        setParam(extrasId(), 0.0f);
        med->devHold(6);
        med->devSynPage().devTurn(0, 80);
        med->devHold(-1);
        check(((pat()->trigs[0] >> 6) & 1) && !((pat()->lockTrigs[0] >> 6) & 1), "EXTRAS off: the turn places a trig, as the MD");
        setParam(extrasId(), 1.0f);
        // RETRIG: Alt+wheel fades; TIME: Alt+wheel the track's speed
        med->devExtraPaint(0, 2, true, true);   // a retrig on T1's step 1
        med->devExtraAltWheel(0, 2, -1);
        check(pat()->retrigFade[0][0] == -8, "RETRIG Alt+wheel down: fade out 8 (" + juce::String(pat()->retrigFade[0][0]) + ")");
        med->devExtraAltWheel(0, 1, 1);
        check(pat()->trackSpeed[0] == 2, "TIME Alt+wheel up: the track at 3/2X (" + juce::String(dd::trackSpeedName(pat()->trackSpeed[0])) + ")");
        med->devExtraAltWheel(0, 1, -1);
        check(pat()->trackSpeed[0] == 0, "and back to 1X");
        // GENERATE: two generators, combined
        med->keyPressed(juce::KeyPress('e'));
        med->devGenClick(3, false, false);   // 4
        med->devGenClick2(1);                // 2
        check((pat()->trigs[0] & 0xFFFF) == bits({0, 4, 8, 12}) && pat()->euclid[0].pulses1 == 4 && pat()->euclid[0].pulses2 == 2, "EUCLID 4 OR 2: 1 5 9 13 (the pattern keeps both)");
        med->keyPressed(juce::KeyPress('b'));
        med->keyPressed(juce::KeyPress('b'));
        check((pat()->trigs[0] & 0xFFFF) == bits({4, 12}) && pat()->euclid[0].op == 2, "B twice: XOR, 5 13");
        med->devGenShiftWheel(1);
        check((pat()->trigs[0] & 0xFFFF) == (bits({0, 4, 8, 12}) ^ bits({1, 9})), "Shift+wheel turns the second generator");
        check(!((pat()->lockTrigs[0] >> 8) & 1) && pat()->locks[pat()->lockRow(0, 0)][8] == 100, "a trig generated onto the trigless lock of step 9 takes its locks");
        med->devGenClick(0, false, false);   // EUCLID 1 XOR 2: no trig on step 9 any more
        check(((pat()->lockTrigs[0] >> 8) & 1) && pat()->locks[pat()->lockRow(0, 0)][8] == 100, "no trig there any more: the trigless lock is back");
        med->devGenClick(3, false, false);   // 4 XOR 2 again
        med->keyPressed(juce::KeyPress('e'));
        med->keyPressed(juce::KeyPress('e'));   // a new run: it goes on from the generators the pattern kept
        med->devGenWheel(1);
        check((pat()->trigs[0] & 0xFFFF) == (bits({1, 5, 9, 13}) ^ bits({1, 9})), "a later run: the wheel turns the first generator on from where it was");
        check(med->devGrid().eu1 == 4 && med->devGrid().eu2 == 2, "GEN marks keys 4 and 2 as E1 and E2");
        med->devGenClick(3, true, false);   // random: the generators go
        check(pat()->euclid[0].pulses1 == 0, "RANDOM: the generators go");

    }
    // ---- the fade as heard: 8 retrig hits of one track (1/32 over 4 steps), each hit's energy in its 3 clocks
    {
        struct Head : juce::AudioPlayHead {
            double ppq = 0;
            juce::Optional<PositionInfo> getPosition() const override { PositionInfo q; q.setPpqPosition(ppq); q.setBpm(120); q.setIsPlaying(true); return q; }
        } head;
        auto energies = [&](int fade) {
            auto q = std::make_unique<MdProcessor>();
            q->setFirmwarePath(juce::String(env.argv[1]), false);
            q->setPlayHead(&head);
            q->setPlayConfigDetails(0, 2, env.rate, env.block);
            q->prepareToPlay(env.rate, env.block);
            dd::Dump d;
            auto kit = q->captureMdKit();   // the default kit (T1 a kick)
            d.kits.push_back(kit);
            auto x = blank(0, 16);
            x.trigs[0] = bits({0}); x.retrig[0][0] = dd::makeRetrig(1, 4); x.retrigFade[0][0] = int8_t(fade);
            d.patterns.push_back(x);
            q->loadMdKit("fade", d.kits[0], "FADE");
            q->setPatternBank("test", "FADE", d, 0);
            for (auto [id, v] : {std::pair{seqId(), 1.0f}, std::pair{patternId(), 0.0f}, std::pair{extrasId(), 1.0f}})
                if (auto* pr = q->apvts.getParameter(id)) pr->setValueNotifyingHost(pr->convertTo0to1(v));
            head.ppq = 0;
            const double spc = env.rate * 60.0 / 120.0 / 24.0;
            std::vector<float> out;
            juce::AudioBuffer<float> b(2, env.block);
            for (int i = 0; i < int(96 * 3 * spc / env.block); ++i) {   // three passes; the third is measured
                juce::MidiBuffer midi; b.clear(); q->processBlock(b, midi);
                head.ppq += env.block / spc / 24.0;
                if (i * env.block >= int(192 * spc)) out.insert(out.end(), b.getReadPointer(0), b.getReadPointer(0) + env.block);
            }
            std::vector<double> e(8, 0.0);
            for (int h = 0; h < 8; ++h)
                for (int k = int(h * 3 * spc); k < int((h + 1) * 3 * spc) && k < int(out.size()); ++k) e[size_t(h)] += double(out[size_t(k)]) * out[size_t(k)];
            return e;
        };
        const auto flat = energies(0), faded = energies(-63);
        juce::String sf, sd;
        for (int h = 0; h < 8; ++h) { sf << juce::String(flat[size_t(h)], 2) << " "; sd << juce::String(faded[size_t(h)], 2) << " "; }
        std::printf("  (hit energies flat: %s / faded: %s)\n", sf.toRawUTF8(), sd.toRawUTF8());
        const bool firstSame = std::abs(faded[0] - flat[0]) < 0.05 * flat[0] + 1e-9;
        const bool falls = faded[2] < 0.8 * flat[2] && faded[5] < 0.4 * flat[5] && faded[7] < 0.1 * flat[7];
        check(flat[0] > 0.0 && firstSame && falls, "a fade of -63 is heard: the first hit as without it, then quieter to silence");
    }
    std::printf(fails ? "XTRA2 TEST FAILED (%d)\n" : "XTRA2 TEST OK\n", fails);
    return fails ? 1 : 0;
}

} // namespace mnm::plugin::md::test
