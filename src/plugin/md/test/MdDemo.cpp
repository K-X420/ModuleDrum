// md-plugintest MD_DEMO=footwork <os.syx> <out.wav>: a demo beat written in code (a kit from the default machines, a
// 64-step pattern with locks and the EXTRAS) and rendered through the plugin: two passes at 160 BPM and the tail.
#include <cstdio>
#include <cstring>
#include <juce_audio_formats/juce_audio_formats.h>
#include "MdProcessor.h"
#include "MdTests.h"

namespace mnm::plugin::md::test {

int mdDemo(TestEnv& env, const char* value)
{
    namespace dd = mnm::mddump;
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

    // ---- the kit: the default machines (T1 TRX-BD .. T12 TRX-XC, T13 EFM-BD, T14 EFM-SD, T15 E12-CP, T16 E12-TA), a few turned
    enum { BD = 0, SD, XT, CP, RS, CB, CH, OH, CY, MA, CL, XC, SUB, ESD, ECP, TA };
    enum { DEC = 1, DIST = 16, VOL = 17, PAN = 18, DEL = 19, REV = 20 };
    auto kit = p.captureMdKit();
    kit.params[BD][DEC] = 60; kit.params[BD][DIST] = 30; kit.params[BD][VOL] = 104;
    kit.params[SUB][DEC] = 76; kit.params[SUB][VOL] = 84;
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
    for (auto& l : kit.levels) l = uint8_t(l * 58 / 100);   // headroom: the 16 tracks together stay under the outputs' ceiling
    kit.delay[0] = 36;    // TIME: a dotted feel against the 16ths
    kit.delay[3] = 54;    // FB
    kit.reverb[2] = 58;   // DEC

    // ---- the pattern: 4 bars of 16ths
    dd::Pattern x;
    x.position = 0; x.length = 64; x.scale = 3; x.extended = true; x.kit = 0;
    x.accentEditAll = x.slideEditAll = x.swingEditAll = 0;
    x.accentAmount = 96;
    for (auto& row : x.locks) std::memset(row, 0xFF, sizeof(row));
    auto trig = [&](int t, int bar, int i) { x.trigs[t] |= 1ull << (16 * bar + i); return 16 * bar + i; };
    auto accent = [&](int t, int s) { x.accentPerTrack[t] |= 1ull << s; };
    const uint8_t every2of2 = uint8_t(dd::kCondRatio + 1);   // 2:2: the second pass

    // the kick: threes against the bar, its pitch stepping down; bar 4 a run and a roll into the turn
    const int kicks[4][6] = {{0, 3, 6, 10, 13, -1}, {0, 3, 6, 8, 11, 14}, {0, 3, 6, 10, 13, -1}, {0, 2, 4, 6, 9, 12}};
    const int kickPitch[6] = {64, 58, 52, 60, 55, 50};
    for (int bar = 0; bar < 4; ++bar)
        for (int k = 0; k < 6; ++k) {
            if (kicks[bar][k] < 0) continue;
            const int s = trig(BD, bar, kicks[bar][k]);
            if (k > 0) x.setLock(BD, 0, s, kickPitch[k]);
            if (k == 0) accent(BD, s);
        }
    { const int s = trig(BD, 3, 14); x.retrig[BD][s] = dd::makeRetrig(1, 2); x.setLock(BD, 0, s, 46); }   // 1/32 over two steps
    // the sub under it
    for (int s : {trig(SUB, 0, 0), trig(SUB, 1, 6), trig(SUB, 2, 0), trig(SUB, 2, 10)}) accent(SUB, s);
    { const int s = trig(SUB, 3, 6); x.cond[SUB][s] = every2of2; x.setLock(SUB, 0, s, 54); }
    // the clap on three, more of them as the bars go
    for (int bar = 0; bar < 4; ++bar) accent(CP, trig(CP, bar, 8));
    trig(CP, 1, 11); trig(CP, 1, 14);
    { const int s = trig(CP, 3, 10); x.retrig[CP][s] = dd::makeRetrig(2, 1); }   // a triplet roll
    trig(CP, 3, 12); trig(CP, 3, 14); trig(CP, 3, 15);
    // snare ghosts, a little late
    for (int s : {trig(SD, 0, 15), trig(SD, 1, 7), trig(SD, 2, 15)}) x.micro[SD][s] = 5;
    x.retrig[SD][16 * 2 + 15] = dd::makeRetrig(1, 1);
    { const int s = trig(SD, 3, 13); x.cond[SD][s] = every2of2; }
    { const int s = trig(ESD, 3, 4); x.cond[ESD][s] = every2of2; }
    // the tom line: a 12-step loop against the 16, falling
    x.trackLen[XT] = 12;
    const int tomPitch[4] = {84, 74, 64, 54};
    for (int k = 0; k < 4; ++k) { const int s = trig(XT, 0, 2 + 3 * k); x.setLock(XT, 0, s, tomPitch[k]); }
    // the rim, echoing
    trig(RS, 0, 10); trig(RS, 1, 2); trig(RS, 1, 13); trig(RS, 2, 10); trig(RS, 3, 5);
    // hats: eighths with the off ones a touch late, sixteenths and a roll at the end
    for (int bar = 0; bar < 3; ++bar)
        for (int i = 0; i < 16; i += 2) { const int s = trig(CH, bar, i); if (i % 4 == 2) { x.micro[CH][s] = 3; accent(CH, s); } }
    for (int i = 0; i < 8; i += 2) trig(CH, 3, i);
    for (int i = 8; i < 16; ++i) trig(CH, 3, i);
    x.retrig[CH][16 * 3 + 15] = dd::makeRetrig(3, 1);
    trig(OH, 0, 14); trig(OH, 1, 6); trig(OH, 2, 14); trig(OH, 3, 6);
    // the second pass: a cowbell figure and a tambourine
    for (int s : {trig(CB, 2, 3), trig(CB, 2, 11), trig(CB, 3, 3)}) x.cond[CB][s] = every2of2;
    for (int bar = 2; bar < 4; ++bar) for (int i = 2; i < 16; i += 4) { const int s = trig(TA, bar, i); x.cond[TA][s] = every2of2; }
    { const int s = trig(CY, 0, 0); x.cond[CY][s] = dd::kCondFirst; }

    dd::Dump d;
    kit.position = 0;
    d.kits.push_back(kit);
    d.patterns.push_back(x);
    p.loadMdKit("demo", kit, "FOOTWORK");
    p.setPatternBank("demo", "FOOTWORK", d, 0);
    auto set = [&](const juce::String& id, float v) { if (auto* q = p.apvts.getParameter(id)) q->setValueNotifyingHost(q->convertTo0to1(v)); };
    set(seqId(), 1.0f); set(patternId(), 0.0f); set(extrasId(), 1.0f);
    p.syncMachineSideEffects();

    {   // half a second stopped first: the levels and knobs glide from the kit before to this one
        juce::AudioBuffer<float> pre(2, block);
        head.playing = false;
        for (int i = 0; i < int(0.5 * rate / block); ++i) { juce::MidiBuffer midi; pre.clear(); p.processBlock(pre, midi); p.syncMachineSideEffects(); }
    }
    const double seconds = 2.0 * 64.0 * (60.0 / bpm / 4.0);   // two passes
    const int playBlocks = int(seconds * rate / block), tailBlocks = int(2.5 * rate / block);
    juce::AudioBuffer<float> all(2, (playBlocks + tailBlocks) * block), buf(2, block);
    for (int i = 0; i < playBlocks + tailBlocks; ++i) {
        head.playing = i < playBlocks;
        juce::MidiBuffer midi;
        buf.clear();
        p.processBlock(buf, midi);
        if (head.playing) head.ppq += double(block) / rate * bpm / 60.0;
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
    std::printf("DEMO footwork: %.1f s at %.0f BPM, peak before gain %.3f -> %s\n", double(all.getNumSamples()) / rate, bpm, double(peak), env.argv[2]);
    // a look at it: the level of each 16th of the first pass (the pattern's shape, for a check without ears)
    const double stepSamples = rate * 60.0 / bpm / 4.0;
    for (int bar = 0; bar < 4; ++bar) {
        std::printf("  bar %d:", bar + 1);
        for (int i = 0; i < 16; ++i) {
            const int a = int((16 * bar + i) * stepSamples), n = int(stepSamples);
            std::printf(" %2d", int(std::lround(60.0 + juce::Decibels::gainToDecibels(all.getRMSLevel(0, a, n), -60.0f))));
        }
        std::printf("\n");
    }
    return 0;
}

} // namespace mnm::plugin::md::test
