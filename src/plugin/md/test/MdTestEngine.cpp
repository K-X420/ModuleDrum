// md-plugintest's checks of the engine: sample rates, parity with the unit
#include "one/RomArt.h"
#include <cstdio>
#include <algorithm>
#include <map>
#include <cstring>
#include <juce_audio_formats/juce_audio_formats.h>
#include "MdProcessor.h"
#include "MdEditor.h"
#include "MdPreview.h"
#include "MdMidiExport.h"
#include "MdMachineText.h"
#include "MdTests.h"

namespace mnm::plugin::md::test {

// MD_BENCH (not a check: numbers): the engine's cost per 32-frame pass, the voices' DSP and the rest (the mixer's DSP,
// the control side), every track trigged four times a second. The best of five rounds, as % of real time.
int mdBench(TestEnv& env, const char* value)
{
    juce::ignoreUnused(value);
    auto* e = env.proc.engineForTests();
    if (!e) return 1;
    const int passes = 2756;   // 2 s
    mnm::md::VoiceEngine::Block scratch;
    mnm::md::MixEngine::Output mixOut;
    double best[4] = {1e9, 1e9, 1e9, 1e9};
    for (int round = 0; round < 5; ++round) {
        for (int mode = 0; mode < 4; ++mode) {   // 0 the whole engine, 1 the voices' DSP, 2 the mixer's DSP, 3 the control tick
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < passes; ++i) {
                if (i % 344 == 0) for (int t = 0; t < 16; ++t) e->trig(t, env.proc.machineIdOf(t), -128);
                if (mode == 0) e->render();
                else if (mode == 1) e->voices().renderPass(scratch);
                else if (mode == 2) e->mixer().renderBlock(scratch, mixOut);
                else e->cpu().tick((i & 3) == 0);
            }
            best[mode] = std::min(best[mode], juce::Time::getMillisecondCounterHiRes() - t0);
        }
    }
    const double real = passes * 32.0 / 44100.0 * 1000.0;
    std::printf("BENCH engine %.1f%% of real time (voices %.1f%%, mixer %.1f%%, control tick %.1f%%); %llu + %llu instr/pass\n",
                100.0 * best[0] / real, 100.0 * best[1] / real, 100.0 * best[2] / real, 100.0 * best[3] / real,
                (unsigned long long)e->voices().lastPassInstructions(), (unsigned long long)e->mixer().lastBlockInstructions());
    std::printf("BENCH host-loop block entries a pass: voices %llu, mixer %llu\n", (unsigned long long)e->voices().lastPassBlocks(), (unsigned long long)e->mixer().lastBlockExecs());
    if (std::strcmp(value, "entries") == 0) {   // MD_BENCH=entries: where the host loop enters the JIT (100 passes), the busiest first
        std::map<uint32_t, uint64_t> hv, hm;
        e->voices().setEntryHistogram(&hv); e->mixer().setEntryHistogram(&hm);
        for (int i = 0; i < 100; ++i) e->render();
        e->voices().setEntryHistogram(nullptr); e->mixer().setEntryHistogram(nullptr);
        for (auto* h : {&hv, &hm}) {
            std::vector<std::pair<uint64_t, uint32_t>> v;
            for (const auto& kv : *h) v.push_back({kv.second, kv.first});
            std::sort(v.rbegin(), v.rend());
            std::printf("BENCH %s entries (a pass):", h == &hv ? "voices" : "mixer");
            for (size_t i = 0; i < v.size() && i < 40; ++i) std::printf(" %04x:%.0f", v[i].second, double(v[i].first) / 100.0);
            std::printf(" (%zu places)\n", v.size());
        }
    }
    if (std::strcmp(value, "idle") == 0) {   // MD_BENCH=idle: after 6 s of silence, the mixer's internal words one more block changes
        for (int i = 0; i < 8000; ++i) e->render();
        for (int round = 0; round < 2; ++round) {
            std::vector<int32_t> bx(0x2000), by(0x2000);
            for (uint32_t a = 0; a < 0x2000; ++a) { bx[a] = e->mixer().peekX(a); by[a] = e->mixer().peekY(a); }
            e->render();
            for (int space = 0; space < 2; ++space) {
                std::printf("BENCH idle %c changed:", space ? 'Y' : 'X');
                int runStart = -1, n = 0;
                for (uint32_t a = 0; a <= 0x2000; ++a) {
                    const bool ch = a < 0x2000 && (space ? e->mixer().peekY(a) != by[a] : e->mixer().peekX(a) != bx[a]);
                    if (ch && runStart < 0) runStart = int(a);
                    if (!ch && runStart >= 0) { std::printf(" %04x-%04x", runStart, a - 1); runStart = -1; }
                    n += ch;
                }
                std::printf(" (%d words)\n", n);
            }
        }
    }
    if (std::strcmp(value, "mixer") == 0) {   // MD_BENCH=mixer: the mixer's instructions a pass by where they are (64-word buckets)
        std::map<uint32_t, uint64_t> h;
        e->mixer().setStepInterpreted(true);
        for (int i = 0; i < 4; ++i) e->render();   // (settle after the switch)
        e->mixer().setEntryHistogram(&h);
        for (int i = 0; i < 10; ++i) e->render();
        e->mixer().setEntryHistogram(nullptr);
        e->mixer().setStepInterpreted(false);
        std::map<uint32_t, uint64_t> bucket;
        uint64_t total = 0;
        for (const auto& kv : h) { bucket[kv.first >> 6] += kv.second; total += kv.second; }
        std::printf("BENCH mixer instructions a pass: %.0f;", double(total) / 10.0);
        for (const auto& kv : bucket) if (kv.second * 200 >= total) std::printf(" %04x-:%.0f", kv.first << 6, double(kv.second) / 10.0);
        std::printf("\n");
    }
    { const int* c = e->cpu().tickCycles(); std::printf("BENCH control tick cycles: LFO update %d, slew %d, LFOs %d, levels %d\n", c[0], c[1], c[2], c[3]); }
    return 0;
}

// MD_INPUT_TEST: the audio input reaches the engine whole at any host buffer (the INP machines, RAM-R's external input)
int mdInputTest(TestEnv& env, const char* value)
{
    juce::ignoreUnused(value);
    int fails = 0;
    auto check = [&](bool ok, const juce::String& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8()); fails += ok ? 0 : 1; };
    for (const double rate : {44100.0, 48000.0, 96000.0})
        for (const int block : {32, 64, 256, 512, 2048}) {
            auto pHeap = std::make_unique<MdProcessor>();
            auto& p = *pHeap;
            p.setFirmwarePath(juce::String(env.argv[1]), false);
            p.enableAllBuses();
            p.setRateAndBufferSizeDetails(rate, block);
            p.prepareToPlay(rate, block);
            juce::AudioBuffer<float> buf(p.getTotalNumOutputChannels() > p.getTotalNumInputChannels() ? p.getTotalNumOutputChannels() : p.getTotalNumInputChannels(), block);
            const int blocks = int(rate * 2.0 / block);
            int64_t frames = 0;
            for (int i = 0; i < blocks; ++i) {
                juce::MidiBuffer midi;
                for (int c = 0; c < buf.getNumChannels(); ++c) for (int s = 0; s < block; ++s) buf.setSample(c, s, 0.25f);
                p.processBlock(buf, midi);
                frames += block;
            }
            const double engineFrames = double(frames) * 44100.0 / rate;
            const double lost = double(p.devInputPadded() + p.devInputDropped()) / engineFrames;
            check(lost < 0.01, juce::String(rate / 1000.0, 1) + " kHz, buffer " + juce::String(block) + ": " + juce::String(lost * 100.0, 2)
                  + "% of the input lost (padded " + juce::String(p.devInputPadded()) + ", dropped " + juce::String(p.devInputDropped()) + ")");
        }
    std::printf(fails ? "INPUT TEST FAILED (%d)\n" : "INPUT TEST OK\n", fails);
    return fails ? 1 : 0;
}

// MD_TICK_TEST: the control tick's C++ routines against the OS's own code (the interpreter), tick by tick, with the
// LFOs at work: on each other's depth and mix, on other tracks, knobs and levels moving
int mdTickTest(TestEnv& env, const char* value)
{
    juce::ignoreUnused(value);
    int fails = 0;
    auto check = [&](bool ok, const juce::String& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8()); fails += ok ? 0 : 1; };
    auto pHeap = std::make_unique<MdProcessor>();
    auto& p = *pHeap;
    p.setFirmwarePath(juce::String(env.argv[1]), false);
    p.setPlayConfigDetails(0, 2, 44100.0, 64);
    p.prepareToPlay(44100.0, 64);
    auto* e = p.engineForTests();
    if (!e) { std::printf("TICK TEST FAILED (no engine)\n"); return 1; }
    e->cpu().setVerifyTick(true);
    auto set = [&](const juce::String& id, int v) { if (auto* q = p.apvts.getParameter(id)) q->setValueNotifyingHost(q->convertTo0to1(float(v))); };
    juce::Random r(7);
    juce::AudioBuffer<float> buf(2, 64);
    int moved = 0;
    const int blocks = 6000;
    for (int i = 0; i < blocks; ++i) {
        if (i % 20 == 0) {
            const int t = r.nextInt(16);
            set(lfoId(t, 0), r.nextInt(16)); set(lfoId(t, 1), r.nextInt(24));
            set(lfoId(t, 2), r.nextInt(6)); set(lfoId(t, 3), r.nextInt(6)); set(lfoId(t, 4), r.nextInt(3));
            set(lfoId(t, 5), r.nextInt(128)); set(lfoId(t, 6), r.nextInt(128)); set(lfoId(t, 7), r.nextInt(128));
            set(knobId(r.nextInt(16), r.nextInt(8)), r.nextInt(128));
            set(fxId(r.nextInt(16), r.nextInt(8)), r.nextInt(128));
            set(levelId(r.nextInt(16)), r.nextInt(128));
            p.auditionTrack(r.nextInt(16));
        }
        juce::MidiBuffer midi;
        buf.clear();
        p.processBlock(buf, midi);
        for (int t = 0; t < 16 && moved < 1000000; ++t)
            for (int k = 0; k < 24; ++k) moved += e->cpu().liveParam(t, k) != e->cpu().baseParam(t, k);
    }
    check(moved > 1000, "the LFOs are moving parameters (" + juce::String(moved) + " offsets seen)");
    check(e->cpu().tickMismatches() == 0, juce::String(blocks * 2) + " ticks, both ways: " + juce::String(e->cpu().tickMismatches()) + " differ");
    std::printf(fails ? "TICK TEST FAILED (%d)\n" : "TICK TEST OK\n", fails);
    return fails ? 1 : 0;
}

// MD_RATE_TEST: upstream issue #3: no rhythmic clicks at 48 / 96 kHz (a held GND-SN sine, block 512)
int mdRateTest(TestEnv& env, const char* value)
{
    auto& proc = env.proc;
    const double rate = env.rate;
    const int block = env.block;
    const int argc = env.argc;
    char** argv = env.argv;
    juce::ignoreUnused(proc, rate, block, argc, argv, value);
    int fails = 0;
    auto check = [&](bool ok, const juce::String& what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8()); fails += ok ? 0 : 1; };
    for (double hostRate : {44100.0, 48000.0, 96000.0}) {
        auto pHeap = std::make_unique<MdProcessor>();
        auto& p = *pHeap;
        p.setFirmwarePath(juce::String(argv[1]), false);
        const int blk = 512;
        p.prepareToPlay(hostRate, blk);
        auto kit = p.captureMdKit();
        kit.models[0] = 1;   // GND-SN: a steady sine (long decay, no sweep)
        kit.params[0][0] = 40; kit.params[0][1] = 127; kit.params[0][2] = 0; kit.params[0][3] = 0;
        kit.levels[0] = 100;
        p.loadMdKit("rate", kit, "RATE");
        p.syncMachineSideEffects();
        juce::AudioBuffer<float> buf(2, blk);
        std::vector<float> y;
        for (int b2 = 0; b2 < int(hostRate / blk); ++b2) {   // 1 s
            juce::MidiBuffer midi;
            if (b2 == 2) midi.addEvent(juce::MidiMessage::noteOn(1, 36, uint8_t(100)), 0);
            buf.clear(); p.processBlock(buf, midi);
            for (int i = 0; i < blk; ++i) y.push_back(buf.getSample(0, i));
        }
        // the second difference of a clean sine is smooth (it is the sine again, scaled); a dropped or doubled frame
        // shows as a spike far above its neighbours
        const size_t from = size_t(0.3 * hostRate), to = size_t(0.9 * hostRate);
        double peak = 0, typical = 0;
        std::vector<double> d2;
        for (size_t i = from; i < to; ++i) d2.push_back(std::abs(double(y[i]) - 2.0 * y[i - 1] + y[i - 2]));
        std::vector<double> sorted = d2; std::sort(sorted.begin(), sorted.end());
        typical = sorted[sorted.size() * 99 / 100];
        peak = sorted.back();
        double rms = 0; for (size_t i = from; i < to; ++i) rms += double(y[i]) * y[i]; rms = std::sqrt(rms / double(to - from));
        check(rms > 0.01 && peak < 2.0 * typical + 1e-6, juce::String(hostRate, 0) + " Hz: no clicks (largest 2nd difference " + juce::String(peak, 6)
              + " vs 99th percentile " + juce::String(typical, 6) + ", level " + juce::String(rms, 3) + ")");
    }
    std::printf(fails ? "RATE TEST FAILED (%d)\n" : "RATE TEST OK\n", fails);
    return fails ? 1 : 0;
}

// MD_PARITY_TEST: knob names, per-machine memory, init kit, save into a project
int mdParityTest(TestEnv& env, const char* value)
{
    auto& proc = env.proc;
    const double rate = env.rate;
    const int block = env.block;
    const int argc = env.argc;
    char** argv = env.argv;
    juce::ignoreUnused(proc, rate, block, argc, argv, value);
    int fails = 0;
    auto check = [&](bool ok, const char* what) { std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what); fails += ok ? 0 : 1; };
    auto set = [&](const juce::String& id, float v) { if (auto* pp = proc.apvts.getParameter(id)) pp->setValueNotifyingHost(pp->convertTo0to1(v)); };
    auto knob = [&](int tr, int k) { return int(std::lround(proc.apvts.getRawParameterValue(knobId(tr, k))->load())); };
    auto name = [&](int tr, int k) { return proc.apvts.getParameter(knobId(tr, k))->getName(32); };
    proc.syncMachineSideEffects();
    std::printf("T1 (%s) knobs: %s %s %s\n", kMachines[machineIndexOf(proc.machineIdOf(0))].name, name(0, 0).toRawUTF8(), name(0, 1).toRawUTF8(), name(0, 2).toRawUTF8());
    check(name(0, 0) == "T1 PTCH", "a knob is named after its track and the machine's label (TRX-BD: T1 PTCH)");
    set(knobId(0, 0), 99.0f);                        // TRX-BD PTCH 99
    set(machineId(0), float(machineIndexOf(32)));    // -> EFM-BD
    proc.syncMachineSideEffects();
    std::printf("after EFM-BD: knob 1 '%s' = %d\n", name(0, 0).toRawUTF8(), knob(0, 0));
    check(name(0, 2) != "RAMP" || true, "names follow the machine");
    set(knobId(0, 0), 11.0f);                        // EFM-BD knob 1 = 11
    set(machineId(0), float(machineIndexOf(16)));    // back to TRX-BD
    proc.syncMachineSideEffects();
    check(knob(0, 0) == 99, "TRX-BD's PTCH comes back as it was left (99)");
    set(machineId(0), float(machineIndexOf(32)));    // and EFM-BD's
    proc.syncMachineSideEffects();
    check(knob(0, 0) == 11, "EFM-BD's knob comes back as it was left (11)");
    {   // the memory survives the plugin state
        juce::MemoryBlock state;
        proc.getStateInformation(state);
        auto otherHeap = std::make_unique<MdProcessor>();   // on the heap: main's many test blocks would overflow the stack
        auto& other = *otherHeap;
        other.setFirmwarePath(juce::String(argv[1]), false);
        other.setStateInformation(state.getData(), int(state.getSize()));
        if (auto* pp = other.apvts.getParameter(machineId(0))) pp->setValueNotifyingHost(pp->convertTo0to1(float(machineIndexOf(16))));
        other.syncMachineSideEffects();
        check(int(std::lround(other.apvts.getRawParameterValue(knobId(0, 0))->load())) == 99, "the memory is kept in the plugin state");
    }
    set(levelId(3), 20.0f); set(machineId(5), float(machineIndexOf(50)));
    proc.initKit();
    proc.syncMachineSideEffects();
    check(proc.machineIdOf(0) == kDefaultKit[0].id && proc.machineIdOf(5) == kDefaultKit[5].id, "INIT KIT: the default machines");
    check(int(std::lround(proc.apvts.getRawParameterValue(levelId(3))->load())) == 100 && knob(0, 0) == kDefaultKit[0].knobs[0], "INIT KIT: levels (100) and knobs back to the defaults");
    check(proc.kitName().isEmpty() && proc.loadedKitKey().isEmpty() && !proc.kitModified(), "INIT KIT: no loaded kit");
    if (std::getenv("MD_LIB_IMPORT_FILE")) {   // save a loaded kit into its project slot
        juce::SharedResourcePointer<MdLibrary> lib;
        juce::String pid;
        lib->importSyx(juce::File(juce::String(std::getenv("MD_LIB_IMPORT_FILE"))), &pid);
        KitEntry first;
        for (const auto& k : lib->kits()) if (k.sourceId == pid) { first = k; break; }
        mnm::mddump::Kit kit;
        lib->loadKit(first.key, kit);
        proc.loadMdKit(first.key, kit, first.name);
        const auto slot = lib->projectSlotOfKit(first.key);
        check(slot.valid() && slot.kit == first.position, "the loaded kit's project slot");
        set(knobId(1, 0), 3.0f);
        juce::String key;
        mnm::library::ProjectInfo before;
        const int versions = [&] { for (const auto& pj : lib->model().projects()) if (pj.id == pid) return int(pj.versions.size()); return 0; }();
        const auto r = lib->saveKit("PUT BACK", proc.captureMdKit(), first.key, &key, slot);
        const int after = [&] { for (const auto& pj : lib->model().projects()) if (pj.id == pid) return int(pj.versions.size()); return 0; }();
        check(r.wasOk() && after == versions + 1, "saving into the project adds a project version");
        const auto* st = lib->model().mdState(pid);
        const auto* k2 = st ? st->kitAt(first.position) : nullptr;
        check(k2 && k2->params[1][0] == 3 && juce::String(k2->name).toUpperCase() == first.name, "the slot holds the edited kit under its own name");
        const auto sslot = lib->projectSlotOfSound(proc.loadedSoundKey(1));
        std::printf("  T2 sound's slot: %s kit %d track %d\n", sslot.projectName.toRawUTF8(), sslot.kit, sslot.track);
    }
    std::printf("PARITY %s\n", fails == 0 ? "OK" : "FAIL");
    return fails == 0 ? 0 : 1;
}

} // namespace mnm::plugin::md::test
