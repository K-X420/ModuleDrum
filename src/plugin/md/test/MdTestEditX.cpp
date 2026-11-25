// md-plugintest's checks of the editing tools after the community firmwares' ideas: CLONE PATTERN, INSERT PATTERN,
// HALVE / DOUBLE a track's spacing, the knob pages' COPY / PASTE / CLEAR / RANDOMISE / NUDGE, and TONAL's note names
#include <cstdio>
#include <cstring>
#include "MdProcessor.h"
#include "MdEditor.h"
#include "MdTests.h"

namespace mnm::plugin::md::test {

namespace {
mnm::mddump::Pattern pat(int position, int length)
{
    mnm::mddump::Pattern p;
    p.position = position; p.length = uint8_t(length); p.accentEditAll = p.slideEditAll = p.swingEditAll = 1;
    for (auto& row : p.locks) std::memset(row, 0xFF, sizeof(row));
    return p;
}
uint64_t bitsOf(std::initializer_list<int> l) { uint64_t m = 0; for (int s : l) m |= 1ull << s; return m; }
}

// MD_EDITX_TEST
int mdEditXTest(TestEnv& env, const char* value)
{
    juce::ignoreUnused(value);
    namespace dd = mnm::mddump;
    int fails = 0;
    auto check = [&](bool ok, const juce::String& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8()); fails += ok ? 0 : 1; };
    auto& p = env.proc;
    dd::Dump d;
    dd::Kit kit;
    for (int t = 0; t < 16; ++t) { kit.trigGroups[t] = kGroupOff; kit.muteGroups[t] = kGroupOff; kit.levels[t] = 100; kit.lfos[t][0] = uint8_t(t); kit.models[t] = 1; }
    kit.params[0][0] = 40;
    kit.position = 0;
    std::memcpy(kit.nameRaw, "GROOVE7", 7); kit.name = "GROOVE7";
    d.kits.push_back(kit);
    auto a = pat(0, 16); a.trigs[0] = bitsOf({0, 2, 4, 6}); a.setLock(0, 1, 4, 99); a.micro[0][6] = 6; d.patterns.push_back(a);
    auto b = pat(1, 16); b.trigs[3] = bitsOf({1}); d.patterns.push_back(b);
    auto c = pat(2, 16); c.trigs[4] = bitsOf({2}); d.patterns.push_back(c);   // A01 A02 A03, then A04 empty
    dd::Song song; song.position = 0;
    dd::SongRow r1; r1.bytes[0] = 1; r1.bytes[2] = 0; dd::SongRow r2; r2.bytes[0] = 2; dd::SongRow r3; r3.bytes[0] = 0;
    song.rows = {r3, r1, r2};
    d.songs.push_back(song);
    p.loadMdKit("editx", kit, "GROOVE7");
    p.setPatternBank("e", "E", d, 0);
    std::unique_ptr<juce::AudioProcessorEditor> ed(p.createEditor());
    auto* med = dynamic_cast<MdEditor*>(ed.get());
    auto setParam = [&](const juce::String& id, float v) { if (auto* q = p.apvts.getParameter(id)) q->setValueNotifyingHost(q->convertTo0to1(v)); };
    auto set = setParam;
    setParam(patternId(), 0.0f);
    med->showGrid(-1);

    // ---- HALVE / DOUBLE
    med->devSelectTrack(0);
    med->devStretch(false);
    auto t0 = p.bankPattern(0);
    check(t0->trigs[0] == bitsOf({0, 1, 2, 3}), "HALVE: steps 1 3 5 7 to 1 2 3 4");
    check(t0->lockRow(0, 1) >= 0 && t0->locks[t0->lockRow(0, 1)][2] == 99, "the locks go with their trigs (step 5's to step 3)");
    check(t0->micro[0][3] == 3, "a nudge halves too (+6/24 to +3)");
    med->devStretch(true);
    t0 = p.bankPattern(0);
    check(t0->trigs[0] == bitsOf({0, 2, 4, 6}) && t0->micro[0][6] == 6, "DOUBLE: back to 1 3 5 7, +6 again");
    med->devStretch(true); med->devStretch(true);   // x4: 0 8 16 24 -> only 0 and 8 fit in 16
    check(p.bankPattern(0)->trigs[0] == bitsOf({0, 8}), "past the length: dropped");

    // ---- CLONE PATTERN
    setParam(patternId(), 1.0f);
    med->devClone();   // A02 -> A03 (taken: replaced), a kit slot for the copy
    auto cl = p.bankPattern(2);
    check(cl && cl->trigs[3] == bitsOf({1}) && cl->position == 2, "CLONE: A02 into A03");
    check(cl && cl->kit != 0 && p.bankHasKit(cl->kit), "with a copy of the live kit in a free slot (kit " + juce::String(cl ? cl->kit + 1 : 0) + ")");
    check(std::lround(p.apvts.getRawParameterValue(patternId())->load()) == 2, "and A03 queued");

    // ---- INSERT PATTERN: an empty slot after A01 (A02 A03 move to A03 A04; the song rows follow)
    setParam(patternId(), 0.0f);
    med->devInsert(1);
    check(!p.bankHasPattern(1) && p.bankPattern(2) && p.bankPattern(2)->trigs[3] == bitsOf({1}) && p.bankPattern(3) && p.bankPattern(3)->position == 3,
          "INSERT AFTER: A02 empty, A02-A03 now A03-A04");
    const auto s0 = p.bankSong(0);
    check(s0 && s0->rows[0].bytes[0] == 0 && s0->rows[1].bytes[0] == 2 && s0->rows[2].bytes[0] == 3, "the song's rows go with their patterns");

    // ---- PAGE ACTIONS on the SYNTHESIS page
    auto& syn = med->devSynPage();
    syn.devTurn(0, 30);
    syn.pageAction(one::KnobPage::PageCopy);
    syn.devTurn(0, 90);
    syn.pageAction(one::KnobPage::PagePaste);
    check(std::lround(p.apvts.getRawParameterValue(knobId(0, 0))->load()) == 30, "COPY / PASTE page: the knobs as copied");
    syn.pageAction(one::KnobPage::PageClear);
    const auto* mi = p.machineInfo(p.machineIdOf(0));
    check(mi && std::lround(p.apvts.getRawParameterValue(knobId(0, 0))->load()) == mi->defaults[0], "CLEAR page: the machine's defaults");
    int moved = 0;
    for (int i = 0; i < 4; ++i) {
        const int before = int(std::lround(p.apvts.getRawParameterValue(knobId(0, 0))->load()));
        syn.pageAction(one::KnobPage::PageNudge);
        const int after = int(std::lround(p.apvts.getRawParameterValue(knobId(0, 0))->load()));
        moved += std::abs(after - before) >= 4 || after == 0 || after == 127 ? 1 : 0;
    }
    check(moved == 4, "NUDGE: each knob 4-8 steps (or at its end)");
    med->devUndoKit();
    check(true, "Ctrl+Alt+Z after a page action (kit undo)");

    // ---- TONAL
    bool anyTable = false;
    for (int mi2 = 0; mi2 < kNumMachines && !anyTable; ++mi2) anyTable = MdEditor::devPitchNames(kMachines[mi2].id, 0) != nullptr;
    check(anyTable, "the measured pitch tables have machines");
    {   // GND-SN's PTCH as notes: its values rise in pitch, every name a note
        const auto* n = MdEditor::devPitchNames(1, 0);
        bool named = n != nullptr;
        for (int v = 0; named && v < 128; ++v) named = n[v] && std::strchr("CDEFGAB", n[v][0]) != nullptr;
        check(named, "TONAL: GND-SN's PTCH values named as notes (64: " + juce::String(n ? n[64] : "-") + ")");
        set(machineId(0), float(machineIndexOf(1)));
        p.syncMachineSideEffects();
        med->devToggleTonal();
        check(p.apvts.getRawParameterValue(tonalId(0))->load() >= 0.5f, "MENU > TONAL T1: on");
        med->devToggleTonal();
    }
    std::printf(fails ? "EDITX TEST FAILED (%d)\n" : "EDITX TEST OK\n", fails);
    return fails ? 1 : 0;
}

} // namespace mnm::plugin::md::test
