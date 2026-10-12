// md-plugintest's checks of the pattern-writing tools: several held steps, copy / cut / paste / duplicate, GENERATE,
// the lock LANE, a window's own Delete, a track's own length, humanise, mutate
#include <cstdio>
#include <cstring>
#include "MdProcessor.h"
#include "MdEditor.h"
#include "MdSequencer.h"
#include "MdTests.h"

namespace mnm::plugin::md::test {

// MD_TOOLS_TEST
int mdToolsTest(TestEnv& env, const char* value)
{
    juce::ignoreUnused(value);
    namespace dd = mnm::mddump;
    int fails = 0;
    auto check = [&](bool ok, const juce::String& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8()); fails += ok ? 0 : 1; };
    auto pHeap = std::make_unique<MdProcessor>();
    auto& p = *pHeap;
    p.setFirmwarePath(juce::String(env.argv[1]), false);
    p.prepareToPlay(env.rate, env.block);
    auto setParam = [&](const juce::String& id, float v) { if (auto* q = p.apvts.getParameter(id)) q->setValueNotifyingHost(q->convertTo0to1(v)); };
    std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
    auto* med = dynamic_cast<MdEditor*>(ed.get());
    med->showGrid(-1);
    auto pat = [&] { return p.bankPattern(0); };
    auto lockAt = [&](int param, int step) { const auto x = pat(); const int row = x ? x->lockRow(0, param) : -1; return row >= 0 && x->locks[row][step] <= 127 ? int(x->locks[row][step]) : -1; };
    auto bits = [](std::initializer_list<int> l) { uint64_t m = 0; for (int s : l) m |= 1ull << s; return m; };

    // ---- several held steps: a knob turn locks them all
    for (int s : {0, 2, 4}) med->devStep(s);
    med->devHold(0);
    med->devHoldAdd(4);
    check(med->devHeldSet() == bits({0, 4}), "Shift+click then Ctrl+Shift+click: two held steps");
    med->devSynPage().devTurn(0, 90);
    check(lockAt(0, 0) == 90 && lockAt(0, 4) == 90 && lockAt(0, 2) < 0, "a knob turn locks every held step (" + juce::String(lockAt(0, 0)) + ", " + juce::String(lockAt(0, 4)) + ")");
    med->devMarkMode(1);
    med->devMarkPaint(0, 0, true, true);   // the ACCENT window: a click on a held step
    med->devMarkMode(0);
    check(pat() && (pat()->accent & bits({0, 2, 4})) == bits({0, 4}), "an edit window's click on a held step marks them all");
    med->devHoldRange(8, 10);
    check(med->devHeldSet() == bits({8, 9, 10}), "Shift+drag holds a run of steps");

    // ---- copy / paste / cut / duplicate
    med->devHoldRange(0, 4);
    med->devCopyNote(0);
    med->devPasteNote(8);
    check(pat()->trigs[0] == bits({0, 2, 4, 8, 10, 12}) && lockAt(0, 8) == 90 && lockAt(0, 12) == 90, "copy three notes, paste at 9: as they were spaced, with their locks");
    med->devPasteNote(14);
    check(pat()->trigs[0] == bits({0, 2, 4, 8, 10, 12, 14}), "a paste near the end leaves out what does not fit");
    med->devUndo();
    med->devHoldRange(8, 12);
    med->keyPressed(juce::KeyPress('x', juce::ModifierKeys::ctrlModifier, 0));
    check(pat()->trigs[0] == bits({0, 2, 4}) && lockAt(0, 8) < 0 && med->devHeldSet() == 0, "cut takes the notes and their locks");
    med->devHoldRange(0, 1);   // steps 1-2 (a note and a gap)
    med->keyPressed(juce::KeyPress('d', juce::ModifierKeys::ctrlModifier, 0));
    check(pat()->trigs[0] == bits({0, 2, 4}) && lockAt(0, 2) == 90 && med->devHeldSet() == bits({2, 3}), "duplicate: the figure again right after itself (its gap and locks too), the copy held");
    med->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey));
    check(med->devHeldSet() == 0, "Esc lets the held steps go");

    // ---- GENERATE: from the track as it was, one undo step for the run
    const auto before = *pat();
    med->keyPressed(juce::KeyPress('e'));
    check(med->devMarkModeNow() == 7, "E opens GENERATE");
    med->devGenClick(3, false, false);   // 4 hits, even
    check(pat()->trigs[0] == 0x1111 && lockAt(0, 0) == 90, "key 4: four hits spread evenly; a step that was there keeps its lock");
    med->devGenWheel(1);
    check(pat()->trigs[0] == 0x2222, "the wheel turns them round");
    med->devGenWheel(-1);
    med->devGenClick(5, true, false);    // 6 at random
    int count = 0; for (int s = 0; s < 16; ++s) count += int((pat()->trigs[0] >> s) & 1);
    check(count == 6, "Shift+key 6: six hits at random (" + juce::String(count) + ")");
    med->devGenClick(3, false, true);    // every 4th
    check(pat()->trigs[0] == 0x1111, "Alt+key 4: every 4th step");
    med->devUndo();
    check(pat()->trigs[0] == before.trigs[0] && lockAt(0, 4) == 90, "the whole run is one undo step");
    med->keyPressed(juce::KeyPress('e'));

    // ---- LANE: one parameter's locks drawn across the page
    med->devStep(8);   // trigs on steps 1, 3, 5, 9
    med->keyPressed(juce::KeyPress('l'));
    check(med->devMarkModeNow() == 8, "L opens LANE");
    med->devLaneParam(1);
    med->devLaneSet(0, 100, true);
    med->devLaneSet(1, 50, false);   // no trig there
    check(lockAt(1, 0) == 100 && lockAt(1, 1) < 0, "a press sets the lock at that height; a step without a trig is left alone");
    med->devLaneRamp(0, 0, 8, 80);
    check(lockAt(1, 0) == 0 && lockAt(1, 4) == 40 && lockAt(1, 8) == 80, "Shift+drag: a straight ramp over the trigs (" + juce::String(lockAt(1, 4)) + ")");
    med->devLaneClear(4);
    check(lockAt(1, 4) < 0 && lockAt(1, 8) == 80, "Alt: that lock goes");
    med->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
    check(lockAt(1, 0) < 0 && lockAt(1, 8) < 0 && lockAt(0, 0) == 90 && pat()->trigs[0] == bits({0, 2, 4, 8}), "Delete in the window: only that parameter's locks on the page");
    med->keyPressed(juce::KeyPress('l'));

    // ---- EXTRAS: a track's own length, humanise
    med->devTrackLen(11);
    check(pat()->trackLen[0] == 0, "a track's length needs EXTRAS on");
    setParam(extrasId(), 1.0f);
    med->devTrackLen(11);
    check(pat()->trackLen[0] == 12, "Ctrl+Alt+click step 12: the track loops there");
    {
        auto x = *pat();
        std::memset(x.trigs, 0, sizeof(x.trigs));
        x.trigs[0] = 1; x.trigs[1] = 1;
        const mnm::md::PatternPlayer pl(x);
        std::vector<mnm::md::SeqTrig> on, off;
        mnm::md::TrigContext ctx; ctx.extras = true;
        pl.trigs(0, 6.0 * 48, on, -1, ctx);
        pl.trigs(0, 6.0 * 48, off);
        int a = 0, b = 0, c = 0;
        for (const auto& e : on) (e.track == 0 ? a : b)++;
        for (const auto& e : off) c += e.track == 0;
        check(a == 4 && b == 3 && c == 3, "48 steps: the 12-step track plays 4 times, a 16-step one 3 (off: " + juce::String(c) + ")");
        auto bytes = dd::encodePattern(x);
        const auto ex = dd::encodePatternExtras(x);
        bytes.insert(bytes.end(), ex.begin(), ex.end());
        const auto d = dd::parseDump(bytes.data(), bytes.size(), "x");
        check(d.patterns.size() == 1 && d.patterns[0].trackLen[0] == 12 && dd::encodeDump(d) == bytes, "the length travels with the pattern's extras");
    }
    med->devTrackLen(11);
    check(pat()->trackLen[0] == 0, "the same step again: back to the pattern's length");
    med->keyPressed(juce::KeyPress('t'));
    med->keyPressed(juce::KeyPress('h'));
    bool within = true, any = false, none = true;
    for (int s = 0; s < 16; ++s) {
        const int m = pat()->micro[0][s];
        if ((pat()->trigs[0] >> s) & 1) { within = within && std::abs(m) <= 4; any = any || m != 0; } else none = none && m == 0;
    }
    check(med->devMarkModeNow() == 5 && within && none, "H in the TIME window: the trigs nudged within +-4, nothing else" + juce::String(any ? "" : " (all zero this roll)"));
    med->keyPressed(juce::KeyPress(juce::KeyPress::deleteKey));
    bool cleared = true; for (int s = 0; s < 16; ++s) cleared = cleared && pat()->micro[0][s] == 0;
    check(cleared && pat()->trigs[0] == bits({0, 2, 4, 8}), "Delete in the TIME window: the timing only");
    med->keyPressed(juce::KeyPress('t'));

    // ---- mutate: a fifth of the way to random, again from the same start, the kit undo brings it back
    auto syn = [&] { std::array<int, 8> v{}; for (int k = 0; k < 8; ++k) v[size_t(k)] = int(std::lround(p.apvts.getRawParameterValue(knobId(0, k))->load())); return v; };
    const auto start = syn();
    med->devMutate(false);
    const auto m1 = syn();
    med->devMutate(false);
    const auto m2 = syn();
    bool near1 = true, near2 = true;
    for (int k = 0; k < 8; ++k) { near1 = near1 && std::abs(m1[size_t(k)] - start[size_t(k)]) <= 26; near2 = near2 && std::abs(m2[size_t(k)] - start[size_t(k)]) <= 26; }
    check(near1 && near2 && m1 != start, "U: the SYNTHESIS knobs move a little; U again rolls from the same start");
    med->devUndoKit();
    check(syn() == start, "Ctrl+Alt+Z: the sound before");

    std::printf(fails ? "TOOLS TEST FAILED (%d)\n" : "TOOLS TEST OK\n", fails);
    return fails ? 1 : 0;
}

} // namespace mnm::plugin::md::test
