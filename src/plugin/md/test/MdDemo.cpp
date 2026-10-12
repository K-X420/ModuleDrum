// md-plugintest MD_DEMO=footwork <os.syx> <out.wav>: a demo beat written in code and rendered through the plugin: a kit
// from the default machines and six patterns (A01..A06) with parameter locks and the EXTRAS, chained one after the
// other at 160 BPM: an intro, the beat, the bass coming in, a two-bar break, then two heavier ones.
#include <cstdio>
#include <cstring>
#include <juce_audio_formats/juce_audio_formats.h>
#include "MdProcessor.h"
#include "MdTests.h"

namespace mnm::plugin::md::test {

namespace {
namespace dd = mnm::mddump;
enum { BD = 0, SD, XT, CP, RS, CB, CH, OH, CY, MA, CL, XC, SUB, ESD, ECP, TA };   // the default kit's tracks (SUB: T13 EFM-BD)
enum { PTCH = 0, DEC = 1, DIST = 16, VOL = 17, PAN = 18, DEL = 19, REV = 20 };

// A pattern being written: bars of 16 steps
struct Writer {
    dd::Pattern x;
    Writer(int position, int bars)
    {
        x.position = position; x.length = uint8_t(16 * bars); x.scale = uint8_t(bars - 1); x.extended = bars > 2; x.kit = 0;
        x.accentEditAll = x.slideEditAll = x.swingEditAll = 0;
        x.accentAmount = 96;
        for (auto& row : x.locks) std::memset(row, 0xFF, sizeof(row));
    }
    int trig(int t, int bar, int i) { const int s = 16 * bar + i; x.trigs[t] |= 1ull << s; return s; }
    int trig(int t, int bar, int i, int param, int value) { const int s = trig(t, bar, i); x.setLock(t, param, s, value); return s; }
    void accent(int t, int s) { x.accentPerTrack[t] |= 1ull << s; }
    void roll(int t, int s, int rate, int steps = 1) { x.retrig[t][s] = dd::makeRetrig(rate, steps); }

    // the tom line: a 12-step loop against the 16, falling (the track's own length)
    void toms(int top = 84)
    {
        x.trackLen[XT] = 12;
        for (int k = 0; k < 4; ++k) trig(XT, 0, 2 + 3 * k, PTCH, top - 10 * k);
    }
    // hats: eighths with the off ones a touch late and accented; from bar `sixteenthsFromBar` on, 16ths in each bar's second half
    void hats(int bars, int sixteenthsFromBar = 99)
    {
        for (int bar = 0; bar < bars; ++bar)
            for (int i = 0; i < 16; ++i) {
                const bool fast = bar >= sixteenthsFromBar && i >= 8;
                if (!fast && (i & 1)) continue;
                const int s = trig(CH, bar, i);
                if (i % 4 == 2) { x.micro[CH][s] = 3; accent(CH, s); }
            }
    }
    // the kick in threes against the bar, its pitch stepping down
    void kicks(int bar, std::initializer_list<int> steps, int dist = -1)
    {
        static const int pitch[8] = {64, 58, 52, 60, 55, 50, 57, 48};
        int k = 0;
        for (int i : steps) {
            const int s = trig(BD, bar, i);
            if (k > 0) x.setLock(BD, PTCH, s, pitch[k & 7]);
            if (dist >= 0) x.setLock(BD, DIST, s, dist);
            if (k == 0) accent(BD, s);
            ++k;
        }
    }
    // a sub note: its pitch, how long it rings, how hard it is driven
    void sub(int bar, int i, int pitch, int dec, int dist, int vol = -1)
    {
        const int s = trig(SUB, bar, i);
        x.setLock(SUB, PTCH, s, pitch); x.setLock(SUB, DEC, s, dec); x.setLock(SUB, DIST, s, dist);
        if (vol >= 0) x.setLock(SUB, VOL, s, vol);
        accent(SUB, s);
    }
};
}

int mdDemo(TestEnv& env, const char* value)
{
    if (std::strcmp(value, "footwork") != 0) { std::printf("MD_DEMO: footwork\n"); return 2; }
    const double rate = 44100.0, bpm = 160.0;
    const int block = 512;
    struct Head : juce::AudioPlayHead {
        double ppq = 0, bpm = 120; bool playing = true;
        juce::Optional<PositionInfo> getPosition() const override { PositionInfo q; q.setPpqPosition(ppq); q.setBpm(bpm); q.setIsPlaying(playing); return q; }
    } head;
    head.bpm = bpm;
    auto pHeap = std::make_unique<MdProcessor>();
    auto& p = *pHeap;
    p.setFirmwarePath(juce::String(env.argv[1]), false);
    if (!p.engineReady()) { std::printf("no OS\n"); return 1; }
    p.setPlayHead(&head);
    p.setPlayConfigDetails(0, 2, rate, block);
    p.prepareToPlay(rate, block);

    // ---- the kit: the default machines, a few turned (the sub starts modest: the patterns' locks bring it up)
    auto kit = p.captureMdKit();
    kit.params[BD][DEC] = 70; kit.params[BD][DIST] = 30; kit.params[BD][VOL] = 116;
    kit.params[SUB][DEC] = 80; kit.params[SUB][VOL] = 100;
    kit.params[XT][VOL] = 92; kit.params[XT][PAN] = 44; kit.params[XT][DEC] = 50; kit.params[XT][REV] = 18;
    kit.params[CP][REV] = 46; kit.params[CP][VOL] = 127; kit.levels[CP] = 127;
    kit.params[SD][REV] = 26; kit.params[SD][VOL] = 112; kit.levels[SD] = 120;
    kit.params[ESD][VOL] = 110;
    kit.params[RS][DEL] = 58; kit.params[RS][PAN] = 92; kit.params[RS][VOL] = 112;
    kit.params[CH][VOL] = 100; kit.params[CH][PAN] = 74; kit.levels[CH] = 120;
    kit.params[OH][VOL] = 96; kit.params[OH][PAN] = 54; kit.params[OH][DEC] = 44;
    kit.params[CB][VOL] = 90; kit.params[CB][PAN] = 104; kit.params[CB][DEL] = 40;
    kit.params[CY][VOL] = 84; kit.params[CY][REV] = 40;
    kit.params[TA][VOL] = 100; kit.params[TA][PAN] = 28;
    for (auto& l : kit.levels) l = uint8_t(l * 60 / 100);   // some headroom under the outputs' ceiling (the later patterns lean on it)
    kit.delay[0] = 36;    // TIME: a dotted feel against the 16ths
    kit.delay[3] = 54;    // FB
    kit.reverb[2] = 58;   // DEC
    kit.position = 0;

    dd::Dump d;
    d.kits.push_back(kit);

    {   // A01, the intro: hats, the tom loop, the rim's echoes; the kick only marks the bars, the clap arrives in bar 3
        Writer w(0, 4);
        w.hats(4, 3);
        w.toms();
        for (int bar = 0; bar < 4; ++bar) w.kicks(bar, {0});
        w.kicks(3, {6, 9, 12});
        w.roll(BD, w.trig(BD, 3, 14, PTCH, 46), 1, 2);
        w.trig(RS, 0, 10); w.trig(RS, 1, 2); w.trig(RS, 1, 13); w.trig(RS, 2, 10); w.trig(RS, 3, 5);
        w.accent(CP, w.trig(CP, 2, 8)); w.accent(CP, w.trig(CP, 3, 8)); w.trig(CP, 3, 14); w.trig(CP, 3, 15);
        w.trig(CY, 0, 0);
        d.patterns.push_back(w.x);
    }
    auto beat = [](Writer& w) {   // the beat the middle patterns share
        w.kicks(0, {0, 3, 6, 10, 13}); w.kicks(1, {0, 3, 6, 8, 11, 14}); w.kicks(2, {0, 3, 6, 10, 13}); w.kicks(3, {0, 2, 4, 6, 9, 12});
        w.roll(BD, w.trig(BD, 3, 14, PTCH, 46), 1, 2);
        for (int bar = 0; bar < 4; ++bar) w.accent(CP, w.trig(CP, bar, 8));
        w.trig(CP, 1, 11); w.trig(CP, 1, 14);
        w.roll(CP, w.trig(CP, 3, 10), 2);   // a triplet roll
        w.trig(CP, 3, 12); w.trig(CP, 3, 14); w.trig(CP, 3, 15);
        for (int s : {w.trig(SD, 0, 15), w.trig(SD, 1, 7), w.trig(SD, 2, 15)}) w.x.micro[SD][s] = 5;   // ghosts, a little late
        w.roll(SD, 16 * 2 + 15, 1);
        w.toms();
        w.trig(RS, 0, 10); w.trig(RS, 1, 2); w.trig(RS, 1, 13); w.trig(RS, 2, 10); w.trig(RS, 3, 5);
        w.hats(4, 3);
        w.roll(CH, 16 * 3 + 15, 3);
        w.trig(OH, 0, 14); w.trig(OH, 1, 6); w.trig(OH, 2, 14); w.trig(OH, 3, 6);
    };
    {   // A02, the beat: the sub only under the bar starts
        Writer w(1, 4);
        beat(w);
        w.sub(0, 0, 64, 80, 0); w.sub(2, 0, 64, 80, 0);
        d.patterns.push_back(w.x);
    }
    {   // A03, the bass comes in: a sub line under the kick, ringing longer, a cowbell figure and a tambourine on top
        Writer w(2, 4);
        beat(w);
        w.sub(0, 0, 64, 96, 20); w.sub(0, 10, 57, 90, 20); w.sub(1, 0, 64, 96, 20); w.sub(1, 6, 52, 100, 24); w.sub(1, 14, 60, 80, 24);
        w.sub(2, 0, 64, 96, 20); w.sub(2, 10, 57, 90, 20); w.sub(3, 0, 60, 96, 28); w.sub(3, 6, 52, 104, 28); w.sub(3, 12, 48, 110, 32);
        w.trig(CB, 2, 3); w.trig(CB, 2, 11); w.trig(CB, 3, 3);
        for (int bar = 2; bar < 4; ++bar) for (int i = 2; i < 16; i += 4) w.trig(TA, bar, i);
        d.patterns.push_back(w.x);
    }
    {   // A04, a two-bar break: no kick, one long sub note, the claps and snares winding up into a roll
        Writer w(3, 2);
        w.sub(0, 0, 52, 124, 30, 98);
        w.toms(90);
        w.hats(2, 0);
        w.trig(CP, 0, 8); w.trig(CP, 0, 12); w.trig(CP, 1, 0); w.trig(CP, 1, 4); w.trig(CP, 1, 6);
        for (int i = 8; i < 16; i += 2) w.roll(CP, w.trig(CP, 1, i), i < 12 ? 1 : 2);
        for (int i = 8; i < 16; ++i) w.trig(SD, 1, i);
        w.roll(SD, 16 + 15, 3);
        w.roll(BD, w.trig(BD, 1, 14, PTCH, 44), 1, 2);
        d.patterns.push_back(w.x);
    }
    auto heavy = [](Writer& w, bool more) {   // the kick running in threes right through, driven; the sub on every bar's corners, low and long
        const int dist = more ? 62 : 48;
        w.kicks(0, {0, 3, 6, 9, 12, 15}, dist); w.kicks(1, {2, 5, 8, 11, 14}, dist); w.kicks(2, {0, 3, 6, 9, 12, 15}, dist); w.kicks(3, {2, 5, 8, 10, 12}, dist);
        w.roll(BD, w.trig(BD, 3, 14, PTCH, 44), more ? 2 : 1, 2);
        const int sd = more ? 76 : 64, sv = more ? 127 : 110, len = more ? 124 : 104;   // (more drive than this and the sub gets quieter)
        w.sub(0, 0, 60, len, sd, sv); w.sub(0, 6, 52, len, sd, sv); w.sub(0, 12, 55, 96, sd, sv);
        w.sub(1, 2, 60, len, sd, sv); w.sub(1, 8, 48, len, sd, sv); w.sub(1, 14, 52, 90, sd, sv);
        w.sub(2, 0, 60, len, sd, sv); w.sub(2, 6, 52, len, sd, sv); w.sub(2, 12, 55, 96, sd, sv);
        w.sub(3, 2, 57, len, sd, sv); w.sub(3, 8, 45, 124, sd, sv);
        if (more) { w.sub(0, 9, 57, 100, sd, sv); w.sub(2, 9, 57, 100, sd, sv); w.sub(1, 11, 45, 110, sd, sv); w.roll(SUB, w.trig(SUB, 3, 14, PTCH, 40), 1, 2); }
        for (int bar = 0; bar < 4; ++bar) { w.accent(CP, w.trig(CP, bar, 8)); w.trig(CP, bar, (bar & 1) ? 12 : 4); }
        w.roll(CP, w.trig(CP, 3, 10), 2); w.trig(CP, 3, 14); w.trig(CP, 3, 15);
        for (int s : {w.trig(SD, 0, 15), w.trig(SD, 1, 7), w.trig(SD, 2, 15), w.trig(SD, 3, 13)}) w.x.micro[SD][s] = 5;
        w.trig(ESD, 1, 4); w.trig(ESD, 3, 4);
        w.toms(more ? 90 : 84);
        w.trig(RS, 0, 10); w.trig(RS, 1, 13); w.trig(RS, 2, 10); w.trig(RS, 3, 5);
        w.hats(4, more ? 0 : 2);
        w.roll(CH, 16 * 3 + 15, 3);
        w.trig(OH, 0, 14); w.trig(OH, 1, 6); w.trig(OH, 2, 14); w.trig(OH, 3, 6);
        w.trig(CB, 0, 3); w.trig(CB, 0, 11); w.trig(CB, 2, 3); w.trig(CB, 2, 11);
        for (int bar = 0; bar < 4; ++bar) for (int i = 2; i < 16; i += 4) w.trig(TA, bar, i);
        w.trig(CY, 0, 0);
    };
    { Writer w(4, 4); heavy(w, false); d.patterns.push_back(w.x); }   // A05
    { Writer w(5, 4); heavy(w, true); d.patterns.push_back(w.x); }    // A06: more of everything low
    int totalSteps = 0;
    for (const auto& pat : d.patterns) totalSteps += pat.length;

    p.loadMdKit("demo", kit, "FOOTWORK");
    p.setPatternBank("demo", "FOOTWORK", d, 0);
    auto set = [&](const juce::String& id, float v) { if (auto* q = p.apvts.getParameter(id)) q->setValueNotifyingHost(q->convertTo0to1(v)); };
    set(seqId(), 1.0f); set(patternId(), 0.0f); set(extrasId(), 1.0f);
    p.setChain({0, 1, 2, 3, 4, 5});   // A01 > A02 > ... > A06, a pass each
    p.syncMachineSideEffects();

    {   // half a second stopped first: the levels and knobs glide from the kit before to this one
        juce::AudioBuffer<float> pre(2, block);
        head.playing = false;
        for (int i = 0; i < int(0.5 * rate / block); ++i) { juce::MidiBuffer midi; pre.clear(); p.processBlock(pre, midi); p.syncMachineSideEffects(); }
    }
    const double stepSeconds = 60.0 / bpm / 4.0, seconds = totalSteps * stepSeconds;
    const int playBlocks = int(seconds * rate / block), tailBlocks = int(3.0 * rate / block);
    juce::AudioBuffer<float> all(2, (playBlocks + tailBlocks) * block), buf(2, block);
    std::vector<int> played;   // the pattern at each block (to see the chain go by)
    for (int i = 0; i < playBlocks + tailBlocks; ++i) {
        head.playing = i < playBlocks;
        juce::MidiBuffer midi;
        buf.clear();
        p.processBlock(buf, midi);
        if (head.playing) { head.ppq += double(block) / rate * bpm / 60.0; played.push_back(p.seqPattern()); }
        p.syncMachineSideEffects();
        for (int c = 0; c < 2; ++c) all.copyFrom(c, i * block, buf, c, 0, block);
    }
    const float peak = all.getMagnitude(0, all.getNumSamples());
    if (peak > 0) all.applyGain(0.89f / peak);   // to -1 dBFS
    const int fade = int(0.3 * rate);
    all.applyGainRamp(all.getNumSamples() - fade, fade, 1.0f, 0.0f);
    juce::File out{juce::String(env.argv[2])};
    out.deleteFile();
    juce::WavAudioFormat wav;
    auto stream = std::unique_ptr<juce::FileOutputStream>(out.createOutputStream());
    if (!stream) return 1;
    if (auto w = std::unique_ptr<juce::AudioFormatWriter>(wav.createWriterFor(stream.get(), rate, 2, 16, {}, 0))) {
        stream.release();
        w->writeFromAudioSampleBuffer(all, 0, all.getNumSamples());
    }
    std::printf("DEMO footwork: %.1f s at %.0f BPM, %d patterns, peak before gain %.3f -> %s\n", double(all.getNumSamples()) / rate, bpm, int(d.patterns.size()), double(peak), env.argv[2]);
    // a look at it without ears: when each pattern of the chain began, and each one's level
    std::printf("  chain:");
    for (size_t i = 0; i < played.size(); ++i)
        if (i == 0 || played[i] != played[i - 1]) std::printf(" A%02d at %.1f s", played[i] + 1, double(i) * block / rate);
    std::printf("\n  level by pattern (dB):");
    int at = 0;
    for (const auto& pat : d.patterns) {
        const int a = int(at * stepSeconds * rate), n = int(pat.length * stepSeconds * rate);
        std::printf(" %.1f", double(juce::Decibels::gainToDecibels(all.getRMSLevel(0, a, n), -99.0f)));
        at += pat.length;
    }
    std::printf("\n");
    return 0;
}

} // namespace mnm::plugin::md::test
