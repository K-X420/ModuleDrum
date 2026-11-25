// md-plugintest MD_TONALSCAN=<out.h>: measures the pitch every PTCH value of every machine plays (the OS's own pitch laws,
// through the plugin), for TONAL mode's note names. Each machine with a pitch knob (PTCH, PTC1-4) is put on track 1 at its
// defaults, trigged at each of the 128 values, and its pitch read (YIN) a little after the attack. A knob is kept when
// most of its values give a clear pitch that rises with the value; gaps are filled in between. Writes MdPitchTable.h.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include "MdProcessor.h"
#include "MdTests.h"

namespace mnm::plugin::md::test {

namespace {
// YIN (de Cheveigne & Kawahara): the period of x with its aperiodicity; 0 = none found
double yin(const std::vector<float>& x, double rate, double& aperiodic)
{
    const int n = int(x.size()) / 2, minLag = int(rate / 12000.0), maxLag = std::min(n - 1, int(rate / 25.0));
    std::vector<double> d(size_t(maxLag + 1), 0.0);
    for (int lag = 1; lag <= maxLag; ++lag) {
        double s = 0;
        for (int i = 0; i < n; ++i) { const double v = double(x[size_t(i)]) - double(x[size_t(i + lag)]); s += v * v; }
        d[size_t(lag)] = s;
    }
    double run = 0;
    std::vector<double> dn(d.size(), 1.0);
    for (int lag = 1; lag <= maxLag; ++lag) { run += d[size_t(lag)]; dn[size_t(lag)] = run > 0 ? d[size_t(lag)] * lag / run : 1.0; }
    int best = -1;
    for (int lag = std::max(2, minLag); lag < maxLag; ++lag)
        if (dn[size_t(lag)] < 0.2) { while (lag + 1 < maxLag && dn[size_t(lag + 1)] < dn[size_t(lag)]) ++lag; best = lag; break; }
    if (best < 0) { aperiodic = 1.0; return 0.0; }
    aperiodic = dn[size_t(best)];
    const double a = dn[size_t(best - 1)], b = dn[size_t(best)], c = dn[size_t(best + 1)];   // parabolic refinement
    const double den = a - 2 * b + c;
    const double lag = double(best) + (std::abs(den) > 1e-12 ? 0.5 * (a - c) / den : 0.0);
    return rate / lag;
}
bool isPitchLabel(const std::string& l) { return l == "PTCH" || l == "PTC1" || l == "PTC2" || l == "PTC3" || l == "PTC4" || l == "PITCH"; }
}

int mdTonalScan(TestEnv& env, const char* value)
{
    auto& p = env.proc;
    const double rate = env.rate;
    std::map<std::pair<int, int>, std::vector<double>> tables;   // (machine id, knob) -> Hz per value
    for (int mi = 0; mi < kNumMachines; ++mi) {
        const int id = kMachines[mi].id;
        const auto* m = p.machineInfo(id);
        if (!m || !m->isSynth() || id == 0) continue;
        for (int k = 0; k < 8; ++k) {
            if (!isPitchLabel(m->labels[size_t(k)])) continue;
            std::vector<double> hz(128, 0.0);
            int clear = 0;
            for (int v = 0; v < 128; ++v) {
                // track 1 as the plugin sets a machine: its defaults (syncMachineSideEffects), then the pitch knob
                auto set = [&](const juce::String& pid, float x) { if (auto* q = p.apvts.getParameter(pid)) q->setValueNotifyingHost(q->convertTo0to1(x)); };
                if (v == 0) {
                    set(machineId(0), float(machineIndexOf(id)));
                    p.syncMachineSideEffects();
                    for (int t = 1; t < 16; ++t) set(levelId(t), 0.0f);
                    set(levelId(0), 127.0f);
                    for (int q = 0; q < 8; ++q) set(knobId(0, q), float(m->defaults[size_t(q)]));
                }
                set(knobId(0, k), float(v));
                juce::AudioBuffer<float> buf(std::max(2, p.getTotalNumOutputChannels()), env.block);
                for (int b = 0; b < 4; ++b) { buf.clear(); juce::MidiBuffer none; p.processBlock(buf, none); }   // settle
                std::vector<float> out;
                for (int b = 0; b < 20; ++b) {
                    buf.clear(); juce::MidiBuffer midi; if (b == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 36, uint8_t(100)), 0); p.processBlock(buf, midi);
                    for (int i = 0; i < env.block; ++i) out.push_back(buf.getSample(0, i));
                }
                const size_t from = size_t(rate * 0.06);   // past the attack (and a drum's pitch sweep)
                if (out.size() < from + 4096) continue;
                std::vector<float> win(out.begin() + std::ptrdiff_t(from), out.begin() + std::ptrdiff_t(from + 4096));
                float peak = 0; for (float s : win) peak = std::max(peak, std::abs(s));
                if (v == 64 && std::getenv("MD_TONALSCAN_DEBUG")) {
                    float all = 0; for (float s : out) all = std::max(all, std::abs(s));
                    std::printf("  %s v64 window peak %.5f, whole peak %.5f, %d samples\n", m->name().c_str(), double(peak), double(all), int(out.size()));
                }
                if (peak < 1e-4f) continue;
                double ap = 1.0;
                const double f = yin(win, rate, ap);
                if (v == 64 && std::getenv("MD_TONALSCAN_DEBUG")) std::printf("  %s v64 peak %.4f f %.1f ap %.3f\n", m->name().c_str(), double(peak), f, ap);
                if (f > 0 && ap < 0.2) { hz[size_t(v)] = f; ++clear; }
            }
            // kept: most values clear, and rising with the value (octave slips of the detector dropped)
            int rises = 0, pairs = 0;
            double last = 0;
            for (int v = 0; v < 128; ++v) {
                if (hz[size_t(v)] <= 0) continue;
                if (last > 0) { ++pairs; if (hz[size_t(v)] >= last * 0.995) ++rises; else hz[size_t(v)] = 0; }
                if (hz[size_t(v)] > 0) last = hz[size_t(v)];
            }
            double lo = 0, hi = 0;
            for (int v = 0; v < 128; ++v) if (hz[size_t(v)] > 0) { if (lo <= 0) lo = hz[size_t(v)]; hi = hz[size_t(v)]; }
            const bool keep = clear >= 96 && pairs > 0 && rises * 10 >= pairs * 9 && lo > 0 && hi / lo > std::pow(2.0, 1.0 / 12.0);   // (and it spans a semitone)
            std::printf("%-8s %-5s clear %3d/128 rising %3d/%3d %s\n", m->name().c_str(), m->labels[size_t(k)].c_str(), clear, rises, pairs, keep ? "KEPT" : "-");
            std::fflush(stdout);
            if (!keep) continue;
            // gaps: in between on a log scale (and held at the ends)
            for (int v = 0; v < 128; ++v) {
                if (hz[size_t(v)] > 0) continue;
                int a = v - 1, b = v + 1;
                while (a >= 0 && hz[size_t(a)] <= 0) --a;
                while (b < 128 && hz[size_t(b)] <= 0) ++b;
                if (a >= 0 && b < 128) hz[size_t(v)] = std::exp(std::log(hz[size_t(a)]) + (std::log(hz[size_t(b)]) - std::log(hz[size_t(a)])) * (v - a) / double(b - a));
                else hz[size_t(v)] = a >= 0 ? hz[size_t(a)] : hz[size_t(b)];
            }
            tables[{id, k}] = hz;
        }
    }
    FILE* f = std::fopen(value, "w");
    if (!f) { std::printf("cannot write %s\n", value); return 1; }
    std::fprintf(f, "// Generated by md-plugintest MD_TONALSCAN (test/MdTonalScan.cpp): the pitch (Hz) each value of a machine's pitch knob\n"
                    "// plays, measured through the OS's own laws at the machine's defaults. TONAL mode names them as notes.\n#pragma once\n\n"
                    "namespace mnm::plugin::md {\n\nstruct PitchTable { int machine, knob; float hz[128]; };\n\ninline constexpr PitchTable kPitchTables[] = {\n");
    for (const auto& [key, hz] : tables) {
        std::fprintf(f, "    {%d, %d, {", key.first, key.second);
        for (int v = 0; v < 128; ++v) std::fprintf(f, "%s%.2ff", v ? ", " : "", hz[size_t(v)]);
        std::fprintf(f, "}},\n");
    }
    std::fprintf(f, "};\n\n} // namespace mnm::plugin::md\n");
    std::fclose(f);
    std::printf("%d pitch knobs written to %s\n", int(tables.size()), value);
    return 0;
}

} // namespace mnm::plugin::md::test
