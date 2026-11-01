// md-plugintest MD_DEMO=reel | reel2 <os.syx> <out.wav>: a 30-second vertical video's worth of ModuleDrum (the two in
// docs/video). Seven (reel2: six) genres chained at their own tempos, each pattern with its own kit and colours, written
// in code and rendered through the plugin; with MD_REEL_FRAMES=<folder> also 30 frames a second of a 1080x1920 picture
// (the title, the editor showing the pattern, its keys large) for an encoder to put with the audio.
// Dev switches: MD_REEL_RAW (the mix only normalised, for mastering outside), MD_REEL_DRY (every send off),
// MD_REEL_SOLO=<track> (only that track), MD_REEL_UNTIL=<seconds>, MD_REEL_STOP=<frames>, MD_REEL_LEVEL=<percent>,
// MD_REEL_LABELS (every machine's knob names and defaults).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <juce_audio_formats/juce_audio_formats.h>
#include "MdProcessor.h"
#include "MdEditor.h"
#include "MdTests.h"
#include "MdNames.h"
#include "one/Lcd.h"
#include "one/RomArt.h"
#include "Skin.h"

namespace mnm::plugin::md::test {

namespace {
namespace dd = mnm::mddump;
// the kit parameters past the synthesis knobs: EFFECTS (AMD AMF EQF EQG FLTF FLTW FLTQ SRR) and ROUTING
enum { PTCH = 0, DEC = 1, AMD = 8, AMF = 9, EQF = 10, EQG = 11, FLTF = 12, FLTW = 13, FLTQ = 14, SRR = 15,
       DIST = 16, VOL = 17, PAN = 18, DEL = 19, REV = 20,
       RAMP = 2, HP = 2, CLPS = 2, HOLD = 3, MOD = 4, STRT = 4 };   // (a few machines' own knobs: GND-SN RAMP, E12 HP / STRT, EFM-CP CLPS, TRX-B2 HOLD, EFM-BD MOD)
// machines
enum { GND_SN = 1, GND_NS = 2, TRX_BD = 16, TRX_SD = 17, TRX_XT = 18, TRX_RS = 20, TRX_CH = 22, TRX_CL = 26, TRX_B2 = 28,
       EFM_BD = 32, EFM_SD = 33, EFM_XT = 34, EFM_CP = 35, EFM_CB = 37, EFM_HH = 38, EFM_CY = 39,
       E12_BD = 48, E12_SD = 49, E12_CP = 52, E12_RS = 53, E12_CH = 55, E12_OH = 56, E12_RC = 57, E12_TA = 60, E12_SH = 62,
       PI_SD = 65, PI_MT = 66, PI_RS = 69 };

struct Bars {
    dd::Pattern x;
    Bars(int position, int bars, int kit)
    {
        x.position = position; x.length = uint8_t(16 * bars); x.scale = uint8_t(bars - 1); x.extended = bars > 2; x.kit = uint8_t(kit);
        x.accentEditAll = x.slideEditAll = x.swingEditAll = 0;
        x.accentAmount = 44;   // an accent ~1.7x (not 2.5x: under the ceiling)
        for (auto& row : x.locks) std::memset(row, 0xFF, sizeof(row));
    }
    int trig(int t, int bar, int i) { const int s = 16 * bar + i; x.trigs[t] |= 1ull << s; return s; }
    int trig(int t, int bar, int i, int param, int value) { const int s = trig(t, bar, i); x.setLock(t, param, s, value); return s; }
    void lock(int t, int s, int param, int value) { x.setLock(t, param, s, value); }
    void accent(int t, int s) { x.accentPerTrack[t] |= 1ull << s; }
    void slide(int t, int s) { x.slidePerTrack[t] |= 1ull << s; }
    void roll(int t, int s, int rate, int steps, int fade = 0) { x.retrig[t][s] = dd::makeRetrig(rate, steps); x.retrigFade[t][s] = int8_t(fade); }
    void lockOnly(int t, int bar, int i, int param, int value) { const int s = 16 * bar + i; x.lockTrigs[t] |= 1ull << s; x.setLock(t, param, s, value); }
    void every(int t, int bars, std::initializer_list<int> steps) { for (int b = 0; b < bars; ++b) for (int i : steps) trig(t, b, i); }
    // the last `steps` steps send nothing into the delay and reverb (their tails would cross into the next pattern)
    void dryEnd(int steps)
    {
        for (int t = 0; t < 16; ++t)
            for (int s = x.length - steps; s < x.length; ++s)
                if (((x.trigs[t] | x.lockTrigs[t]) >> s) & 1) { x.setLock(t, DEL, s, 0); x.setLock(t, REV, s, 0); }
    }
    // an effect throw: the step's send (DEL / REV) up for that one hit
    void thrw(int t, int s, int send, int amount = 127) { x.setLock(t, send, s, amount); }
    uint64_t euclid(int t, int p1, int p2, int r1, int r2, int op)
    {
        dd::Pattern::Euclid e;
        e.pulses1 = uint8_t(p1); e.pulses2 = uint8_t(p2); e.rot1 = uint8_t(r1); e.rot2 = uint8_t(r2); e.op = uint8_t(op);
        x.euclid[t] = e;
        const uint64_t m = dd::euclidSteps(e, x.length);
        x.trigs[t] |= m;
        return m;
    }
};

struct Genre {
    const char* title;
    const char* caption;
    double bpm;
    juce::uint32 ink, paper;
    int view, track, laneParam;   // view: 0 trigs, 1 mixer, 5 TIME, 6 RETRIG, 7 GENERATE, 8 LANE (of laneParam)
};
const Genre kGenres[7] = {
    {"AMBIENT",    "DELAY THROWS ON LOCKS", 122, 0xff9ad7ff, 0xff0b1430, 8, 1, DEL},
    {"FUTURE",     "EUCLIDEAN SHAKERS",     134, 0xff1a0f2e, 0xffc9a7ff, 7, 3, 0},
    {"DEMBOW",     "TRIGLESS 808 DROPS",     98, 0xff0b0b0b, 0xffff2e88, 1, -1, 0},
    {"JERSEY",     "RETRIG FADE-INS",       140, 0xffffffff, 0xff1e40ff, 6, 1, 0},
    {"FOOTWORK",   "808 KICKS IN THREES",   160, 0xff39ff14, 0xff0a0a0a, 0, 0, 0},
    {"JUNGLE",     "PITCHED BREAK CHOPS",   166, 0xff0b0b0b, 0xffff7a00, 8, 1, PTCH},
    {"DRUM+BASS",  "HATS AT 2X SPEED",      174, 0xffff3b30, 0xff140000, 5, 2, 0},
};
const char* const kSubtitles[7] = {"TECHNO", "GARAGE", "", "CLUB", "", "", ""};

// A voice of a kit: the track's machine (its own knob defaults first), then the knobs and routing it is turned to
int kLevelScale = 54;   // all levels, percent (under the outputs' ceiling)
const int kBalance1[7] = {170, 124, 110, 116, 86, 96, 110};   // each genre of the first reel to one loudness (levels, percent)
const int* gBalance = kBalance1;
struct Voice { int t, id, level; std::initializer_list<std::pair<int, int>> p; };
dd::Kit makeKit(const dd::Kit& base, const MachineKnobInfo& ki, int position, std::initializer_list<Voice> voices,
                std::initializer_list<std::pair<int, int>> delay = {}, std::initializer_list<std::pair<int, int>> reverb = {})
{
    dd::Kit k = base;
    for (int t = 0; t < 16; ++t) k.levels[t] = 0;   // tracks the genre leaves out stay silent
    for (const auto& v : voices) {
        k.models[v.t] = uint32_t(v.id);
        for (int q = 0; q < 8; ++q) k.params[v.t][q] = ki.known[size_t(v.id)] ? ki.defaults[size_t(v.id)][size_t(q)] : uint8_t(64);
        for (int q = 8; q < 24; ++q) k.params[v.t][q] = base.params[0][q];   // the effects and routing of a plain track
        k.params[v.t][PAN] = 64; k.params[v.t][DEL] = 0; k.params[v.t][REV] = 0; k.params[v.t][VOL] = 110; k.params[v.t][DIST] = 0;
        for (const auto& [q, val] : v.p) k.params[v.t][q] = uint8_t(val);
        k.levels[v.t] = uint8_t(std::min(127, v.level * kLevelScale / 100 * gBalance[position] / 100));
    }
    for (const auto& [q, val] : delay) k.delay[q] = uint8_t(val);
    for (const auto& [q, val] : reverb) k.reverb[q] = uint8_t(val);
    k.position = uint8_t(position);
    return k;
}

// The master delay and reverb, the same in every kit (a change of them under a tail bends it): TIME, FB; DEC
const std::initializer_list<std::pair<int, int>> kDelay = {{0, 40}, {3, 58}}, kReverb = {{2, 76}};

std::vector<dd::Kit> kits(const dd::Kit& base, const MachineKnobInfo& ki)
{
    std::vector<dd::Kit> out;
    // AMBIENT TECHNO: a soft FM kick, sine stabs into a long delay, a sine drone, shakers, a rim into the reverb
    out.push_back(makeKit(base, ki, 0, {
        {0, EFM_BD, 96, {{PTCH, 28}, {DEC, 58}, {MOD, 30}, {VOL, 118}}},
        {1, GND_SN, 78, {{PTCH, 84}, {DEC, 62}, {DEL, 50}, {REV, 22}, {PAN, 50}}},
        {2, GND_SN, 54, {{PTCH, 61}, {DEC, 112}, {RAMP, 0}, {REV, 30}, {PAN, 78}}},
        {3, E12_SH, 74, {{DEC, 50}, {HP, 60}, {PAN, 84}, {DEL, 20}}},
        {4, PI_RS, 70, {{PTCH, 104}, {DEC, 40}, {REV, 28}, {PAN, 40}}},
        {5, E12_CH, 66, {{DEC, 40}, {PAN, 70}}},
        {6, GND_NS, 40, {{DEC, 100}, {FLTF, 90}, {FLTW, 20}, {REV, 20}}},
        {7, E12_CP, 62, {{DEC, 70}, {REV, 40}}},
    }, kDelay, kReverb));
    // FUTURE GARAGE: sampled 2-step drums, a sine sub, FM chimes into the delay, a wooden tom
    out.push_back(makeKit(base, ki, 1, {
        {0, E12_BD, 100, {{DEC, 70}, {VOL, 120}}},
        {1, E12_SD, 92, {{PTCH, 58}, {DEC, 60}, {REV, 64}}},
        {2, E12_RS, 70, {{DEC, 50}, {PAN, 44}}},
        {3, E12_SH, 76, {{DEC, 46}, {PAN, 86}}},
        {4, GND_SN, 100, {{PTCH, 40}, {DEC, 104}, {VOL, 124}}},
        {5, EFM_CB, 70, {{PTCH, 92}, {DEC, 50}, {DEL, 70}, {REV, 50}, {PAN, 80}}},
        {6, E12_OH, 66, {{DEC, 50}, {PAN, 60}}},
        {7, PI_MT, 76, {{PTCH, 40}, {DEC, 50}, {REV, 30}}},
    }, kDelay, kReverb));
    // DEMBOW: sampled kick, snare and hats, a TRX-B2 808 that holds, FM toms
    out.push_back(makeKit(base, ki, 2, {
        {0, E12_BD, 104, {{DEC, 60}, {DIST, 30}, {VOL, 122}}},
        {1, E12_SD, 96, {{PTCH, 70}, {DEC, 58}}},
        {2, E12_CH, 72, {{DEC, 46}, {PAN, 74}}},
        {3, E12_TA, 70, {{DEC, 60}, {PAN, 50}}},
        {4, TRX_B2, 104, {{PTCH, 22}, {DEC, 110}, {HOLD, 50}, {DIST, 20}, {VOL, 124}}},
        {5, EFM_XT, 80, {{DEC, 50}, {REV, 30}}},
        {6, TRX_RS, 78, {{DEL, 30}, {PAN, 90}}},
    }, kDelay, kReverb));
    // JERSEY: a punchy TRX kick, a sampled clap, the FM "bed squeak", hats, an 808 under it
    out.push_back(makeKit(base, ki, 3, {
        {0, TRX_BD, 104, {{DEC, 50}, {DIST, 30}, {VOL, 124}}},
        {1, E12_CP, 96, {{DEC, 64}, {REV, 34}, {VOL, 124}}},
        {2, EFM_CP, 70, {{PTCH, 124}, {DEC, 20}, {CLPS, 0}, {PAN, 90}}},
        {3, E12_CH, 70, {{DEC, 40}}},
        {4, E12_OH, 64, {{DEC, 60}, {PAN, 50}}},
        {5, TRX_B2, 96, {{PTCH, 26}, {DEC, 90}, {HOLD, 36}, {VOL, 118}}},
    }, kDelay, kReverb));
    // FOOTWORK: 808 kicks stepping down, sampled claps, FM toms looping in 12, hats, a rim into the delay
    out.push_back(makeKit(base, ki, 4, {
        {0, TRX_B2, 104, {{PTCH, 26}, {DEC, 96}, {HOLD, 40}, {DIST, 24}, {VOL, 124}}},
        {1, E12_CP, 96, {{DEC, 70}, {REV, 40}, {VOL, 124}}},
        {2, EFM_XT, 80, {{DEC, 54}, {REV, 20}, {PAN, 44}}},
        {3, E12_CH, 76, {{DEC, 40}, {PAN, 74}}},
        {4, TRX_RS, 78, {{DEL, 58}, {PAN, 92}}},
        {5, E12_SD, 72, {{PTCH, 76}, {DEC, 40}}},
    }, kDelay, kReverb));
    // JUNGLE: a sampled break (kick, snare, ride) chopped with pitch and start locks, a ringing P-I snare, a long 808
    out.push_back(makeKit(base, ki, 5, {
        {0, E12_BD, 100, {{DEC, 64}, {VOL, 120}}},
        {1, E12_SD, 96, {{DEC, 64}, {VOL, 124}}},
        {2, E12_RC, 66, {{DEC, 70}, {PAN, 76}}},
        {3, PI_SD, 76, {{DEC, 70}, {REV, 60}}},
        {4, TRX_B2, 104, {{PTCH, 20}, {DEC, 120}, {HOLD, 56}, {VOL, 124}}},
        {5, E12_CH, 66, {{DEC, 36}, {PAN, 50}}},
    }, kDelay, kReverb));
    // DRUM+BASS: an FM kick, a big sampled snare, TRX hats at 2X, a sine reese driven hard, an FM crash
    out.push_back(makeKit(base, ki, 6, {
        {0, EFM_BD, 104, {{PTCH, 40}, {DEC, 50}, {MOD, 70}, {VOL, 124}}},
        {1, E12_SD, 100, {{PTCH, 66}, {DEC, 70}, {REV, 36}, {VOL, 124}}},
        {2, TRX_CH, 74, {{DEC, 30}, {PAN, 76}}},
        {3, GND_SN, 92, {{PTCH, 36}, {DEC, 120}, {DIST, 96}, {VOL, 118}}},
        {4, EFM_CY, 66, {{DEC, 80}, {REV, 50}}},
        {5, GND_NS, 44, {{DEC, 110}, {FLTF, 40}, {FLTW, 20}, {REV, 70}}},
    }, kDelay, kReverb));
    return out;
}

std::vector<dd::Pattern> patterns()
{
    std::vector<dd::Pattern> out;
    {   // AMBIENT TECHNO 122: the kick on the floor, stabs thrown into the delay and reverb, a drone, a hiss rising
        Bars w(0, 2, 0);
        enum { K = 0, STAB, DRONE, SHK, RIM, HAT, HISS, CLAP };
        w.every(K, 2, {0, 4, 8, 12});
        const int st[4][2] = {{3, 84}, {10, 91}, {16 + 6, 79}, {16 + 14, 96}};
        for (const auto& s : st) w.trig(STAB, 0, s[0], PTCH, s[1]);
        w.thrw(STAB, 10, DEL, 110); w.thrw(STAB, 16 + 6, DEL); w.thrw(STAB, 16 + 6, REV, 76);
        w.trig(DRONE, 0, 0); w.trig(DRONE, 1, 0, PTCH, 56);
        for (int s = 0; s < 32; ++s) { w.trig(SHK, 0, s); if (s % 4 == 2) w.accent(SHK, s); w.lock(SHK, s, HP, 20 + s * 3); }
        w.trig(RIM, 0, 7); w.thrw(RIM, w.trig(RIM, 0, 13), REV, 90); w.trig(RIM, 1, 15);
        w.every(HAT, 2, {2, 6, 10, 14});
        w.trig(HISS, 1, 8, FLTF, 110);
        w.thrw(CLAP, w.trig(CLAP, 1, 4), DEL, 100); w.trig(CLAP, 1, 12);
        out.push_back(w.x);
    }
    {   // FUTURE GARAGE 134: a swung 2-step, Euclidean shakers, the sub sliding, chimes and the snare thrown out
        Bars w(1, 2, 1);
        enum { K = 0, SN, RIM, SHK, SUBB, CHIME, OPEN, TOM };
        w.x.swingEditAll = 1;
        w.x.swing = 0xAAAAAAAAAAAAAAAAull;
        w.x.swingAmount = uint32_t(12 * 16384 / 50);   // 62%
        w.trig(K, 0, 0); w.trig(K, 0, 10); w.trig(K, 1, 0); w.trig(K, 1, 9);
        w.every(SN, 2, {4, 12});
        w.thrw(SN, 12, REV);
        for (int s : {w.trig(RIM, 0, 3), w.trig(RIM, 0, 7), w.trig(RIM, 1, 14), w.trig(RIM, 1, 6)}) w.x.micro[RIM][s] = 4;
        w.euclid(SHK, 5, 3, 2, 1, 2);
        const int a = w.trig(SUBB, 0, 0, PTCH, 40); w.slide(SUBB, a);
        w.trig(SUBB, 0, 6, PTCH, 35); w.trig(SUBB, 1, 0, PTCH, 40); w.trig(SUBB, 1, 11, PTCH, 45);
        w.thrw(CHIME, w.trig(CHIME, 0, 9, PTCH, 98), DEL, 120); w.trig(CHIME, 1, 2, PTCH, 92); w.trig(CHIME, 1, 9, PTCH, 104);
        w.every(OPEN, 2, {6, 14});
        w.trig(TOM, 0, 13, PTCH, 46); w.trig(TOM, 1, 15, PTCH, 36);
        out.push_back(w.x);
    }
    {   // DEMBOW 98: the dembow snare; an 808 that slides, then drops twice on trigless locks; a rim thrown to the delay
        Bars w(2, 2, 2);
        enum { K = 0, SN, HAT, TAMB, B808, TOM, RIM };
        w.every(K, 2, {0, 4, 8, 12});
        for (int b = 0; b < 2; ++b) for (int i : {3, 6, 11, 14}) { const int s = w.trig(SN, b, i); if (i == 6 || i == 14) w.accent(SN, s); }
        for (int b = 0; b < 2; ++b) for (int i = 0; i < 16; i += 2) { const int s = w.trig(HAT, b, i); if (i % 4 == 2) w.x.micro[HAT][s] = 4; }
        w.every(TAMB, 2, {4, 12});
        const int a = w.trig(B808, 0, 0, PTCH, 22); w.slide(B808, a);
        w.trig(B808, 0, 8, PTCH, 16);
        w.accent(B808, w.trig(B808, 1, 0, PTCH, 22));
        w.lockOnly(B808, 1, 6, PTCH, 17);
        w.lockOnly(B808, 1, 10, PTCH, 12);
        w.trig(TOM, 0, 7, PTCH, 82); w.trig(TOM, 1, 13, PTCH, 74); w.trig(TOM, 1, 15, PTCH, 62);
        w.thrw(RIM, w.trig(RIM, 0, 10), DEL); w.trig(RIM, 1, 10);
        out.push_back(w.x);
    }
    {   // JERSEY 140: the bouncing kick, the squeak, claps rolling up into the next pattern, one thrown to the reverb
        Bars w(3, 2, 3);
        enum { K = 0, CLP, SQK, HAT, OPEN, B808 };
        for (int i : {0, 3, 6, 8, 10}) w.trig(K, 0, i);
        for (int i : {0, 3, 6, 8, 10, 12, 14}) { const int s = w.trig(K, 1, i); if (i >= 12) w.accent(K, s); }
        w.every(CLP, 2, {4, 12});
        w.thrw(CLP, 12, REV);
        w.roll(CLP, w.trig(CLP, 0, 14), 2, 2, 40);
        w.roll(CLP, 16 + 12, 3, 4, 63);
        w.lock(CLP, 16 + 12, VOL, 84);   // (the roll's hits stack: under the ceiling)
        for (int b = 0; b < 2; ++b) for (int i : {2, 7, 13}) w.trig(SQK, b, i, PTCH, i == 7 ? 118 : 124);
        for (int b = 0; b < 2; ++b) for (int i = 0; i < 16; i += 2) w.trig(HAT, b, i);
        w.trig(OPEN, 0, 14); w.trig(OPEN, 1, 6);
        w.trig(B808, 0, 0, PTCH, 26); w.trig(B808, 1, 0, PTCH, 22); w.trig(B808, 1, 8, PTCH, 26);
        out.push_back(w.x);
    }
    {   // FOOTWORK 160: 808 kicks in threes stepping down, the 12-step tom loop, rolls that fade, a rim thrown out
        Bars w(4, 3, 4);
        enum { K = 0, CLP, TOM, HAT, RIM, SN };
        static const int pitch[8] = {26, 22, 18, 24, 20, 16, 23, 14};
        auto kicks = [&](int bar, std::initializer_list<int> steps) { int k = 0; for (int i : steps) { const int s = w.trig(K, bar, i, PTCH, pitch[k & 7]); if (!k) w.accent(K, s); ++k; } };
        kicks(0, {0, 3, 6, 10, 13}); kicks(1, {0, 3, 6, 8, 11, 14}); kicks(2, {0, 2, 4, 6, 9, 12});
        w.roll(K, w.trig(K, 2, 14, PTCH, 14), 1, 2, 40);
        for (int b = 0; b < 3; ++b) w.accent(CLP, w.trig(CLP, b, 8));
        w.roll(CLP, w.trig(CLP, 2, 10), 2, 2, -48);
        w.x.trackLen[TOM] = 12;
        for (int k = 0; k < 4; ++k) w.trig(TOM, 0, 2 + 3 * k, PTCH, 86 - 10 * k);
        for (int b = 0; b < 3; ++b) for (int i = 0; i < 16; ++i) if (!(i & 1) || b == 2) { const int s = w.trig(HAT, b, i); if (i % 4 == 2) { w.x.micro[HAT][s] = 3; w.accent(HAT, s); } }
        w.trig(RIM, 0, 10); w.trig(RIM, 1, 2); w.thrw(RIM, w.trig(RIM, 1, 13), DEL); w.trig(RIM, 2, 5);
        for (int s : {w.trig(SN, 0, 15), w.trig(SN, 1, 7)}) w.x.micro[SN][s] = 5;
        out.push_back(w.x);
    }
    {   // JUNGLE 166: a break chopped with pitch and start locks, a ride, a long 808 bending on trigless locks
        Bars w(5, 3, 5);
        enum { K = 0, SN, RIDE, RING, B808, HAT };
        w.trig(K, 0, 0); w.trig(K, 0, 10); w.trig(K, 1, 0); w.trig(K, 1, 2); w.trig(K, 1, 10); w.trig(K, 2, 0); w.trig(K, 2, 10); w.trig(K, 2, 11);
        const int chops[3][5][3] = {   // step, pitch, sample start
            {{4, 64, 0}, {7, 70, 30}, {9, 60, 0}, {12, 64, 0}, {15, 76, 40}},
            {{4, 64, 0}, {6, 58, 20}, {9, 72, 0}, {12, 64, 0}, {14, 80, 50}},
            {{4, 64, 0}, {7, 68, 30}, {9, 60, 0}, {12, 70, 0}, {15, 88, 60}}};
        for (int b = 0; b < 3; ++b)
            for (const auto& c : chops[b]) {
                const int s = w.trig(SN, b, c[0], PTCH, c[1]);
                if (c[2]) { w.lock(SN, s, STRT, c[2]); w.x.micro[SN][s] = 3; } else w.accent(SN, s);
            }
        w.thrw(SN, 16 + 14, DEL);
        w.roll(SN, 32 + 12, 2, 4, 50);
        for (int s = 0; s < 48; s += 2) w.trig(RIDE, 0, s);
        w.thrw(RING, w.trig(RING, 1, 12), REV);
        w.trig(B808, 0, 0, PTCH, 20); w.lockOnly(B808, 0, 8, PTCH, 15);
        w.trig(B808, 1, 0, PTCH, 18); w.trig(B808, 2, 0, PTCH, 20); w.lockOnly(B808, 2, 6, PTCH, 26);
        for (int b = 0; b < 3; ++b) w.trig(HAT, b, 14);
        out.push_back(w.x);
    }
    {   // DRUM+BASS 174: the two-step; hats at 2X; the reese bending on trigless locks; a snare roll fading in, thrown out
        Bars w(6, 3, 6);
        enum { K = 0, SN, HAT, REESE, CRASH, NOISE };
        for (int b = 0; b < 3; ++b) { w.accent(K, w.trig(K, b, 0)); w.trig(K, b, 10); }
        w.trig(K, 2, 13);
        for (int b = 0; b < 3; ++b) for (int i : {4, 12}) w.accent(SN, w.trig(SN, b, i));
        for (int s : {w.trig(SN, 0, 7), w.trig(SN, 1, 15), w.trig(SN, 1, 9)}) w.x.micro[SN][s] = 6;
        w.roll(SN, w.trig(SN, 2, 12), 3, 4, 63);
        w.thrw(SN, 32 + 12, DEL);
        w.lock(SN, 32 + 12, VOL, 84); w.thrw(SN, 32 + 12, REV, 100);
        for (int s = 0; s < 48; s += 2) { w.trig(HAT, 0, s); if (s % 8 == 4) w.accent(HAT, s); }
        w.x.trackSpeed[HAT] = 1;
        w.roll(HAT, 46, 2, 1, -40);
        w.accent(REESE, w.trig(REESE, 0, 0, PTCH, 36)); w.lockOnly(REESE, 0, 8, PTCH, 39);
        w.trig(REESE, 1, 0, PTCH, 32); w.lockOnly(REESE, 1, 10, PTCH, 34);
        w.trig(REESE, 2, 0, PTCH, 36); w.lockOnly(REESE, 2, 6, PTCH, 41);
        w.trig(CRASH, 0, 0);
        w.trig(NOISE, 2, 0, FLTF, 30); w.lockOnly(NOISE, 2, 8, FLTF, 80); w.lockOnly(NOISE, 2, 12, FLTF, 120);
        out.push_back(w.x);
    }
    return out;
}


// ---------------------------------------------------------------------------------------------- the second reel
// Six more: electro, UK garage, Latin club, amapiano, baile funk, trap. The effects stay tasteful between them: one
// delay and reverb setting for every kit, modest sends, and nothing sent into them in a pattern's last beat (the
// tails that cross a change are only what was already dying away).
enum { TRX_CP = 19, TRX_CB = 21, TRX_OH = 23, TRX_XC = 27, E12_CB = 54 };
const Genre kGenres2[6] = {
    {"ELECTRO",    "808 ZAPS + EUCLID BELLS", 128, 0xff00f0ff, 0xff0a0012, 7, 3, 0},
    {"UK GARAGE",  "MICRO-TIMED SHUFFLE",     134, 0xff101010, 0xffb8ff3c, 5, 2, 0},
    {"\"LATIN CLUB\"", "TUMBAO CLAVE CASCARA",    126, 0xff3a0000, 0xffff5a36, 1, -1, 0},
    {"AMAPIANO",   "LOG DRUM SLIDES",         113, 0xff00221f, 0xff2ee6c9, 8, 1, PTCH},
    {"BAILE FUNK", "APITO TRILLS",            130, 0xffffe600, 0xff00873e, 6, 4, 0},
    {"TRAP",       "HI-HAT ROLLS",            140, 0xffffffff, 0xff6a00ff, 6, 3, 0},
};
const char* const kSubtitles2[6] = {"", "", "", "", "", ""};
const int kBalance2[6] = {108, 116, 100, 65, 95, 124};
const std::initializer_list<std::pair<int, int>> kDelay2 = {{0, 40}, {3, 48}}, kReverb2 = {{2, 60}};

std::vector<dd::Kit> kits2(const dd::Kit& base, const MachineKnobInfo& ki)
{
    std::vector<dd::Kit> out;
    out.push_back(makeKit(base, ki, 0, {   // ELECTRO: the 808 family, sine zaps (their pitch ramp), a sine robot bass
        {0, TRX_BD, 100, {{PTCH, 40}, {DEC, 80}, {VOL, 122}}},
        {1, TRX_CP, 90, {{VOL, 118}, {REV, 18}}},
        {2, TRX_CH, 72, {{DEC, 26}, {PAN, 74}}},
        {3, TRX_CB, 74, {{PTCH, 70}, {DEC, 40}, {PAN, 40}, {DEL, 22}}},
        {4, GND_SN, 78, {{PTCH, 90}, {DEC, 30}, {2, 110}, {3, 40}, {PAN, 84}}},
        {5, TRX_CL, 70, {{PTCH, 90}, {PAN, 30}}},
        {6, TRX_SD, 80, {{PTCH, 70}, {DEC, 40}}},
        {7, GND_SN, 96, {{PTCH, 36}, {DEC, 96}, {VOL, 122}}},
        {8, TRX_OH, 60, {{DEC, 40}, {PAN, 56}}},
    }, kDelay2, kReverb2));
    out.push_back(makeKit(base, ki, 1, {   // UK GARAGE: sampled 2-step drums, a driven sine bass, an FM organ stab
        {0, E12_BD, 100, {{DEC, 60}, {VOL, 122}}},
        {1, E12_SD, 94, {{PTCH, 64}, {DEC, 56}, {REV, 20}}},
        {2, E12_RS, 74, {{PAN, 40}}},
        {3, E12_CH, 74, {{DEC, 34}, {PAN, 72}}},
        {4, E12_OH, 62, {{DEC, 46}, {PAN, 56}}},
        {5, GND_SN, 100, {{PTCH, 38}, {DEC, 90}, {DIST, 70}, {VOL, 120}}},
        {6, EFM_CB, 66, {{PTCH, 88}, {DEC, 40}, {PAN, 84}, {DEL, 26}}},
        {7, E12_SH, 64, {{DEC, 40}, {PAN, 90}}},
    }, kDelay2, kReverb2));
    out.push_back(makeKit(base, ki, 2, {   // LATIN: quinto and tumba congas, clave, a timbale bell, bongos, a sine bass
        {0, E12_BD, 100, {{DEC, 56}, {VOL, 122}}},
        {1, TRX_XC, 92, {{PTCH, 78}, {DEC, 40}, {PAN, 44}}},
        {2, TRX_XC, 96, {{PTCH, 58}, {DEC, 58}, {PAN, 76}}},
        {3, TRX_CL, 78, {{PTCH, 92}, {DEC, 30}, {PAN, 36}}},
        {4, E12_CB, 70, {{PTCH, 84}, {HP, 40}, {PAN, 92}}},
        {5, E12_SH, 68, {{DEC, 40}, {PAN, 80}}},
        {6, GND_SN, 96, {{PTCH, 40}, {DEC, 70}, {VOL, 120}}},
        {7, PI_MT, 80, {{PTCH, 96}, {DEC, 30}, {2, 100}, {PAN, 58}}},
        {8, E12_CP, 78, {{DEC, 60}, {REV, 20}}},
        {9, E12_OH, 56, {{DEC, 36}, {PAN, 66}}},
        {10, TRX_XT, 82, {{PTCH, 70}, {DEC, 40}, {PAN, 50}}},
    }, kDelay2, kReverb2));
    out.push_back(makeKit(base, ki, 3, {   // AMAPIANO: the TRX tom as a log drum, swung shakers, an FM pluck
        {0, E12_BD, 96, {{DEC, 56}, {VOL, 112}}},
        {1, TRX_XT, 100, {{PTCH, 60}, {DEC, 92}, {DIST, 24}, {VOL, 120}}},
        {2, E12_SH, 74, {{DEC, 40}, {PAN, 70}}},
        {3, E12_CP, 84, {{DEC, 64}, {REV, 30}}},
        {4, PI_RS, 70, {{PTCH, 100}, {PAN, 40}}},
        {5, EFM_CB, 60, {{PTCH, 80}, {DEC, 30}, {PAN, 86}, {DEL, 24}}},
        {6, E12_OH, 56, {{DEC, 40}, {PAN, 56}}},
    }, kDelay2, kReverb2));
    out.push_back(makeKit(base, ki, 4, {   // BAILE FUNK: a driven 808 boom, atabaques, a clap, the apito (a sine whistle)
        {0, TRX_B2, 100, {{PTCH, 30}, {DEC, 86}, {HOLD, 40}, {DIST, 70}, {VOL, 124}}},
        {1, TRX_XC, 92, {{PTCH, 66}, {DEC, 40}, {DIST, 30}, {PAN, 40}}},
        {2, TRX_XC, 92, {{PTCH, 50}, {DEC, 50}, {DIST, 30}, {PAN, 80}}},
        {3, E12_CP, 94, {{DEC, 50}, {VOL, 124}}},
        {4, GND_SN, 62, {{PTCH, 120}, {DEC, 14}, {PAN, 70}, {DEL, 18}}},
        {5, E12_CH, 70, {{DEC, 30}, {PAN, 60}}},
        {6, EFM_CB, 64, {{PTCH, 76}, {DEC, 24}, {PAN, 30}}},
        {7, TRX_RS, 70, {{PAN, 90}}},
        {8, E12_SH, 66, {{DEC, 50}, {PAN, 74}}},
    }, kDelay2, kReverb2));
    out.push_back(makeKit(base, ki, 5, {   // TRAP: a long 808, sampled kick, clap and hats
        {0, E12_BD, 96, {{DEC, 56}, {VOL, 118}}},
        {1, TRX_B2, 70, {{PTCH, 26}, {DEC, 120}, {HOLD, 70}, {DIST, 40}, {VOL, 124}}},
        {2, E12_CP, 116, {{DEC, 70}, {REV, 24}, {VOL, 124}}},
        {3, E12_CH, 118, {{DEC, 30}, {PAN, 70}}},
        {4, E12_OH, 60, {{DEC, 50}, {PAN, 54}}},
        {5, E12_SD, 84, {{PTCH, 70}, {DEC, 50}}},
        {6, E12_RS, 66, {{PAN, 40}}},
    }, kDelay2, kReverb2));
    return out;
}

std::vector<dd::Pattern> patterns2()
{
    std::vector<dd::Pattern> out;
    {   // ELECTRO 128: the 808 kick broken up, Euclidean bells, zaps (their pitch ramp locked), a robot bass
        Bars w(0, 3, 0);
        enum { K = 0, CLP, HAT, BELL, ZAP, CLAV, SN, BASS, OPEN };
        for (int b = 0; b < 2; ++b) for (int i : {0, 6, 10}) w.trig(K, b, i);
        w.trig(K, 1, 13);
        for (int i : {0, 3, 6, 10, 12, 14}) w.trig(K, 2, i);
        w.every(CLP, 3, {4, 12});
        for (int s = 0; s < 48; ++s) { w.trig(HAT, 0, s); if (s % 4 == 2) w.accent(HAT, s); }
        const uint64_t bells = w.euclid(BELL, 5, 3, 0, 2, 0);
        int n = 0;
        for (int s = 0; s < 48; ++s) if ((bells >> s) & 1) w.lock(BELL, s, PTCH, (n++ % 3 == 2) ? 77 : 70);
        w.trig(ZAP, 0, 3, 2, 110); w.trig(ZAP, 1, 11, 2, 90); w.trig(ZAP, 2, 7, 2, 127); w.trig(ZAP, 2, 9, 2, 60);
        w.every(CLAV, 3, {7, 15});
        for (int b = 0; b < 3; ++b) {
            const int a = w.trig(BASS, b, 0, PTCH, 36); w.trig(BASS, b, 3, PTCH, 36);
            w.slide(BASS, w.trig(BASS, b, 6, PTCH, 43)); w.trig(BASS, b, 10, PTCH, 36); juce::ignoreUnused(a);
        }
        w.roll(SN, w.trig(SN, 2, 13), 2, 3, 50);
        w.trig(OPEN, 1, 14);
        w.dryEnd(4);
        out.push_back(w.x);
    }
    {   // UK GARAGE 134: the 2-step, swung and nudged: rims late, hats late on the off-beats; a speed-garage bass
        Bars w(1, 3, 1);
        enum { K = 0, SN, RIM, HAT, OPEN, BASS, STAB, SHK };
        w.x.swingEditAll = 1;
        w.x.swing = 0xAAAAAAAAAAAAAAAAull;
        w.x.swingAmount = uint32_t(16 * 16384 / 50);   // 66%
        w.trig(K, 0, 0); w.trig(K, 0, 10); w.trig(K, 1, 0); w.trig(K, 1, 7); w.trig(K, 1, 10); w.trig(K, 2, 0); w.trig(K, 2, 10); w.trig(K, 2, 13);
        w.every(SN, 3, {4, 12});
        for (int b = 0; b < 3; ++b) for (int i : {3, 6, 11, 15}) { const int s = w.trig(RIM, b, i); w.x.micro[RIM][s] = int8_t(i == 6 ? 9 : 5); }
        for (int b = 0; b < 3; ++b) for (int i = 2; i < 16; i += 4) { const int s = w.trig(HAT, b, i); w.x.micro[HAT][s] = 4; }
        for (int b = 0; b < 3; ++b) w.trig(OPEN, b, 14);
        for (int b = 0; b < 3; ++b) { w.slide(BASS, w.trig(BASS, b, 0, PTCH, 38)); w.trig(BASS, b, 3, PTCH, 38); w.trig(BASS, b, 8, PTCH, 45); }
        w.lockOnly(BASS, 2, 12, PTCH, 33);
        w.trig(STAB, 0, 7, PTCH, 88); w.trig(STAB, 0, 14, PTCH, 95); w.trig(STAB, 1, 2, PTCH, 91); w.thrw(STAB, w.trig(STAB, 1, 9, PTCH, 98), DEL, 70);
        for (int s = 0; s < 48; ++s) w.trig(SHK, 0, s);
        w.dryEnd(4);
        out.push_back(w.x);
    }
    {   // LATIN CLUB 126: the tumbao on two congas, a 2-3 clave, cascara on the bell, bongos, the bass anticipating
        Bars w(2, 3, 2);
        enum { K = 0, QUINTO, TUMBA, CLAV, BELL, SHK, BASS, BONGO, CLP, OPEN, TIMB };
        w.every(K, 3, {0, 4, 8, 12});
        // the clave, 2-3 (the 2 side, the 3 side, the 2 side)
        w.trig(CLAV, 0, 4); w.trig(CLAV, 0, 8); w.trig(CLAV, 1, 0); w.thrw(CLAV, w.trig(CLAV, 1, 6), DEL, 64); w.trig(CLAV, 1, 12); w.trig(CLAV, 2, 4); w.trig(CLAV, 2, 8);
        // the tumbao: quinto slaps on 2 and 4, its tips between (quiet), the tumba's two open tones at the end of the bar
        for (int b = 0; b < 3; ++b) {
            for (int i : {4, 12}) w.accent(QUINTO, w.trig(QUINTO, b, i, PTCH, 82));
            for (int i : {1, 3, 7, 9, 11, 15}) w.trig(QUINTO, b, i, VOL, 64);
            w.trig(TUMBA, b, 14, PTCH, 58); w.trig(TUMBA, b, 15, PTCH, 52);
            if (b) w.trig(TUMBA, b, 6, PTCH, 58);
        }
        for (int b = 0; b < 2; ++b) for (int i : {0, 2, 4, 5, 7, 9, 10, 12, 14}) { const int s = w.trig(BELL, b, i); if (i == 0 || i == 7) w.accent(BELL, s); }
        for (int s = 0; s < 48; ++s) { w.trig(SHK, 0, s); if (s % 4 == 2) w.accent(SHK, s); }
        const uint64_t bongos = w.euclid(BONGO, 7, 0, 1, 0, 0);
        int n = 0;
        for (int s = 0; s < 48; ++s) if ((bongos >> s) & 1) w.lock(BONGO, s, PTCH, (n++ & 1) ? 104 : 96);
        const int bass[3][2] = {{40, 47}, {45, 40}, {40, 38}};
        for (int b = 0; b < 3; ++b) { w.slide(BASS, w.trig(BASS, b, 6, PTCH, bass[b][0])); w.trig(BASS, b, 12, PTCH, bass[b][1]); }
        w.every(CLP, 3, {4, 12});
        w.every(OPEN, 2, {2, 6, 10, 14});
        for (int k = 0; k < 6; ++k) w.trig(TIMB, 2, 10 + k, PTCH, 82 - 4 * k);   // the timbale fill
        w.roll(TIMB, 32 + 15, 2, 1, -40);
        w.dryEnd(4);
        out.push_back(w.x);
    }
    {   // AMAPIANO 113: swung shakers, the log drum sliding between notes, a soft kick, a pluck
        Bars w(3, 2, 3);
        enum { K = 0, LOG, SHK, CLP, RIM, PLUCK, OPEN };
        w.x.swingEditAll = 1;
        w.x.swing = 0xAAAAAAAAAAAAAAAAull;
        w.x.swingAmount = uint32_t(8 * 16384 / 50);   // 58%
        for (int b = 0; b < 2; ++b) for (int i : {0, 4, 8, 12}) w.trig(K, b, i, VOL, 96);
        const int notes[2][5][2] = {{{0, 60}, {3, 52}, {7, 57}, {10, 48}, {14, 55}}, {{0, 60}, {3, 52}, {6, 64}, {10, 48}, {12, 45}}};
        for (int b = 0; b < 2; ++b)
            for (const auto& nt : notes[b]) { const int s = w.trig(LOG, b, nt[0], PTCH, nt[1]); if (nt[0] == 3 || nt[0] == 10) w.slide(LOG, s); }
        for (int s = 0; s < 32; ++s) { w.trig(SHK, 0, s); if (s % 4 == 2) w.accent(SHK, s); }
        w.every(CLP, 2, {4, 12});
        w.trig(RIM, 0, 3); w.trig(RIM, 0, 11); w.trig(RIM, 1, 9);
        w.trig(PLUCK, 0, 6, PTCH, 80); w.trig(PLUCK, 0, 14, PTCH, 87); w.trig(PLUCK, 1, 6, PTCH, 84);
        w.trig(OPEN, 0, 14);
        w.dryEnd(4);
        out.push_back(w.x);
    }
    {   // BAILE FUNK 130: the tamborzao (a driven 808 boom, atabaques in 3-3-2 with flams), the clap, apito trills
        Bars w(4, 3, 4);
        enum { K = 0, ATH, ATL, CLP, APITO, HAT, BELL, RIM, SHK };
        for (int s = 0; s < 48; ++s) { w.trig(SHK, 0, s); if (s % 4 == 2) w.accent(SHK, s); }
        const int boom[5][2] = {{0, 30}, {3, 28}, {6, 30}, {10, 34}, {12, 30}};
        for (int b = 0; b < 3; ++b) for (const auto& o : boom) { const int s = w.trig(K, b, o[0], PTCH, o[1]); if (!o[0]) w.accent(K, s); }
        w.roll(K, w.trig(K, 2, 14, PTCH, 26), 1, 2, 30);
        for (int b = 0; b < 3; ++b) {
            for (int i : {0, 3, 6}) w.trig(ATH, b, i, PTCH, i == 6 ? 70 : 66);
            for (int i : {8, 11, 14}) w.trig(ATL, b, i, PTCH, i == 14 ? 46 : 50);
            w.roll(ATH, 16 * b + 6, 1, 1, -30);   // the flams
            w.roll(ATL, 16 * b + 14, 1, 1, -30);
        }
        w.every(CLP, 3, {4, 12});
        w.trig(CLP, 1, 13);
        w.roll(APITO, w.trig(APITO, 0, 0), 2, 2, -40);
        w.roll(APITO, w.trig(APITO, 1, 8, PTCH, 116), 3, 1, 0);
        w.roll(APITO, w.trig(APITO, 2, 0), 3, 4, -50);
        for (int b = 0; b < 3; ++b) for (int i = 0; i < 16; i += 2) w.trig(HAT, b, i);
        w.trig(BELL, 1, 7); w.trig(BELL, 1, 15); w.trig(BELL, 2, 7);
        w.every(RIM, 3, {10});
        w.dryEnd(4);
        out.push_back(w.x);
    }
    {   // TRAP 140: half time, the 808 gliding and dropping on a trigless lock, hi-hat rolls fading in and out
        Bars w(5, 2, 5);
        enum { K = 0, B808, CLP, HAT, OPEN, SN, RIM };
        w.trig(K, 0, 0); w.trig(K, 0, 10); w.trig(K, 1, 0); w.trig(K, 1, 3); w.trig(K, 1, 11);
        w.slide(B808, w.trig(B808, 0, 0, PTCH, 26)); w.trig(B808, 0, 10, PTCH, 22);
        w.accent(B808, w.trig(B808, 1, 0, PTCH, 26)); w.lockOnly(B808, 1, 6, PTCH, 20);
        w.slide(B808, w.trig(B808, 1, 11, PTCH, 30)); w.trig(B808, 1, 14, PTCH, 24);
        w.every(CLP, 2, {8});
        w.thrw(CLP, 16 + 8, REV, 80);
        for (int i : {0, 2, 4, 6, 8, 10, 12}) w.trig(HAT, 0, i);
        w.roll(HAT, w.trig(HAT, 0, 7), 1, 1);
        w.roll(HAT, w.trig(HAT, 0, 11), 3, 1);
        w.roll(HAT, w.trig(HAT, 0, 14), 2, 2, 40);
        for (int i : {0, 2, 4, 6, 8}) w.trig(HAT, 1, i);
        w.roll(HAT, w.trig(HAT, 1, 5), 1, 1);
        w.roll(HAT, w.trig(HAT, 1, 10), 4, 2, -40);
        w.roll(HAT, w.trig(HAT, 1, 12), 5, 4, 63);
        w.trig(OPEN, 0, 14);
        w.roll(SN, w.trig(SN, 1, 13), 2, 3, 63);
        w.trig(RIM, 0, 13); w.trig(RIM, 1, 7);
        out.push_back(w.x);
    }
    return out;
}

// LCD-font text, each pixel `scale` px, in the skin's ink
void lcdText(juce::Graphics& g, const char* s, const mnm::uispec::Font& f, int scale, int y, int frameW)
{
    const int w = one::LcdCanvas::textWidth(f, s) + 2, h = f.h + 2;
    one::LcdCanvas c(w, h);
    c.text(f, s, 1, 1, true);
    g.saveState();
    g.reduceClipRegion(0, y, frameW, h * scale);
    c.draw(g, (frameW - w * scale) / 2, y, scale);
    g.restoreState();
}
}

int mdDemoReel(TestEnv& env, int which)
{
    const Genre* const genres = which == 2 ? kGenres2 : kGenres;
    const char* const* const subtitles = which == 2 ? kSubtitles2 : kSubtitles;
    const int count = which == 2 ? 6 : 7;
    gBalance = which == 2 ? kBalance2 : kBalance1;
    const double rate = 44100.0;
    const int block = 245, fps = 30, blocksPerFrame = int(rate / fps) / block;   // 1470 samples a frame
    struct Head : juce::AudioPlayHead {
        double ppq = 0, bpm = 120; bool playing = true;
        juce::Optional<PositionInfo> getPosition() const override { PositionInfo q; q.setPpqPosition(ppq); q.setBpm(bpm); q.setIsPlaying(playing); return q; }
    } head;
    auto pHeap = std::make_unique<MdProcessor>();
    auto& p = *pHeap;
    p.setFirmwarePath(juce::String(env.argv[1]), false);
    if (!p.engineReady()) { std::printf("no OS\n"); return 1; }
    p.setPlayHead(&head);
    p.setPlayConfigDetails(0, 2, rate, block);
    p.prepareToPlay(rate, block);

    if (std::getenv("MD_REEL_LABELS")) {   // dev: every machine's synthesis knobs and their defaults
        const auto& ki = p.knobInfo();
        for (int id = 0; id < 256; ++id) {
            if (!ki.known[size_t(id)]) continue;
            std::printf("%3d %-7s", id, mnm::mdnames::machineName(id).c_str());
            for (int k = 0; k < 8; ++k) std::printf(" %s=%d", ki.labels[size_t(id)][size_t(k)].toRawUTF8(), int(ki.defaults[size_t(id)][size_t(k)]));
            std::printf("\n");
        }
        return 0;
    }
    if (const char* ls = std::getenv("MD_REEL_LEVEL")) kLevelScale = std::atoi(ls);
    dd::Dump d;
    const auto base = p.captureMdKit();
    d.kits = which == 2 ? kits2(base, p.knobInfo()) : kits(base, p.knobInfo());
    d.patterns = which == 2 ? patterns2() : patterns();
    if (const char* solo = std::getenv("MD_REEL_SOLO")) {   // dev: only this track sounds (finding a sound)
        const int t = std::atoi(solo);
        for (auto& k : d.kits) for (int u = 0; u < 16; ++u) if (u != t) k.levels[u] = 0;
    }
    if (std::getenv("MD_REEL_DRY")) {
        for (auto& k : d.kits) for (int t = 0; t < 16; ++t) { k.params[t][DEL] = 0; k.params[t][REV] = 0; }
        for (auto& pat : d.patterns) for (int t = 0; t < 16; ++t) for (int st = 0; st < 64; ++st) { pat.clearLock(t, DEL, st); pat.clearLock(t, REV, st); }
    }
    std::vector<double> starts;   // each pattern's start in quarter notes
    double q = 0;
    for (const auto& pat : d.patterns) { starts.push_back(q); q += pat.length / 4.0; }
    const double endPpq = q;

    p.loadMdKit("reel", d.kits[0], "REEL");
    p.setPatternBank("reel", "REEL", d, 0);
    auto set = [&](const juce::String& id, float v) { if (auto* r = p.apvts.getParameter(id)) r->setValueNotifyingHost(r->convertTo0to1(v)); };
    set(seqId(), 1.0f); set(patternId(), 0.0f); set(extrasId(), 1.0f);
    std::vector<int> chain;
    for (int g = 0; g < count; ++g) chain.push_back(g);
    p.setChain(chain);
    p.syncMachineSideEffects();

    const juce::File frames = std::getenv("MD_REEL_FRAMES") ? juce::File(juce::String(std::getenv("MD_REEL_FRAMES"))) : juce::File();
    std::unique_ptr<juce::AudioProcessorEditor> ed;
    MdEditor* med = nullptr;
    if (frames != juce::File()) {
        frames.createDirectory();
        for (auto& f : frames.findChildFiles(juce::File::findFiles, false, "frame_*.png")) f.deleteFile();
        ed.reset(p.createEditor());
        med = dynamic_cast<MdEditor*>(ed.get());
    }
    auto genreAt = [&](double ppq) { int g = 0; while (g + 1 < count && ppq >= starts[size_t(g + 1)] - 1e-9) ++g; return g; };

    {   // half a second stopped first: the levels glide in
        juce::AudioBuffer<float> pre(2, block);
        head.playing = false;
        for (int i = 0; i < int(0.5 * rate / block); ++i) { juce::MidiBuffer midi; pre.clear(); p.processBlock(pre, midi); p.syncMachineSideEffects(); }
    }
    std::vector<float> L, R;
    juce::AudioBuffer<float> buf(2, block);
    int shown = -1, frameNo = 0, blockNo = 0;
    const int tailBlocks = int(1.2 * rate / block);
    int tail = 0;
    auto view = [&](int g) {   // the editor for the genre: its colours, its track and window
        const auto& G = genres[g];
        med->devSkin(mnm::plugin::skin::presetSkin(mnm::plugin::skin::Preset::Custom, juce::Colour(G.ink), juce::Colour(G.paper)));
        if (med->devMixerOpen()) med->devToggleMixer();
        med->showGrid(-1);
        med->devMarkMode(0);
        if (G.track >= 0) med->devSelectTrack(G.track);
        if (G.view == 1) med->devToggleMixer();
        else if (G.view == 7) med->keyPressed(juce::KeyPress('e'));
        else if (G.view == 8) { med->devLaneParam(G.laneParam); med->devMarkMode(8); }
        else if (G.view) med->devMarkMode(G.view);
    };
    while (true) {
        static const double until = std::getenv("MD_REEL_UNTIL") ? std::atof(std::getenv("MD_REEL_UNTIL")) : 1e9;   // dev: stop early (seconds)
        if (double(L.size()) / rate > until) break;
        const bool playing = head.ppq < endPpq - 1e-9;
        if (!playing && ++tail > tailBlocks) break;
        head.playing = playing;
        const int g = genreAt(head.ppq);
        head.bpm = genres[g].bpm;
        juce::MidiBuffer midi;
        buf.clear();
        p.processBlock(buf, midi);
        if (playing) head.ppq += double(block) / rate * head.bpm / 60.0;
        p.syncMachineSideEffects();
        L.insert(L.end(), buf.getReadPointer(0), buf.getReadPointer(0) + block);
        R.insert(R.end(), buf.getReadPointer(1), buf.getReadPointer(1) + block);
        if (med && ++blockNo % blocksPerFrame == 0) {
            if (g != shown) { view(g); shown = g; }
            med->refresh();
            const auto& G = genres[g];
            const auto snap = ed->createComponentSnapshot(ed->getLocalBounds(), true, 2.0f);
            juce::Image frame(juce::Image::RGB, 1080, 1920, true, juce::SoftwareImageType());   // (a native image is only filled when its Graphics goes)
            {
            juce::Graphics gr(frame);
            gr.fillAll(juce::Colour(G.paper));
            gr.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
            lcdText(gr, "MODULEDRUM", mnm::uispec::kFontBold8, 9, 90, 1080);
            lcdText(gr, G.title, mnm::uispec::kFontBold8, std::min(11, 1040 / (one::LcdCanvas::textWidth(mnm::uispec::kFontBold8, G.title) + 2)), 250, 1080);   // (a long title smaller, to fit)
            if (*subtitles[g]) lcdText(gr, subtitles[g], mnm::uispec::kFontBold8, 7, 362, 1080);
            lcdText(gr, (juce::String(int(G.bpm)) + " BPM").toRawUTF8(), mnm::uispec::kFontBold8, 5, *subtitles[g] ? 455 : 420, 1080);
            const int ew = 1040, eh = int(double(snap.getHeight()) * ew / snap.getWidth()), ey = 560;
            gr.drawImage(snap, 20, ey, ew, eh, 0, 0, snap.getWidth(), snap.getHeight());
            const auto keys = med->devKeysArea() * 2;   // the trig keys, large: 1-8 over 9-16
            const int half = keys.getWidth() / 2, kh = int(double(keys.getHeight()) * ew / half);
            for (int row = 0; row < 2; ++row)
                gr.drawImage(snap, 20, ey + eh + 40 + row * (kh + 16), ew, kh, keys.getX() + row * half, keys.getY(), half, keys.getHeight());
            const int capY = ey + eh + 40 + 2 * kh + 16 + 50;
            lcdText(gr, G.caption, mnm::uispec::kFontTiny3x5, 8, capY, 1080);
            {   // the genres as dots, the playing one solid
                gr.setColour(juce::Colour(G.ink));
                for (int k = 0; k < count; ++k) {
                    const float cx = 540.0f + (float(k) - float(count - 1) / 2.0f) * 60.0f, cy = 1745.0f;
                    if (k == g) gr.fillEllipse(cx - 14, cy - 14, 28, 28);
                    else gr.drawEllipse(cx - 12, cy - 12, 24, 24, 4.0f);
                }
            }
            lcdText(gr, "FREE  OPEN SOURCE  VST3 AU", mnm::uispec::kFontTiny3x5, 5, 1795, 1080);
            lcdText(gr, "GITHUB.COM/K-X420/MODULEDRUM/RELEASES", mnm::uispec::kFontTiny3x5, 5, 1840, 1080);
            }
            juce::PNGImageFormat png;
            const auto f = frames.getChildFile(juce::String::formatted("frame_%05d.png", frameNo++));
            if (auto st = std::unique_ptr<juce::FileOutputStream>(f.createOutputStream())) png.writeImageToStream(frame, *st);
            if (const char* stop = std::getenv("MD_REEL_STOP"); stop && frameNo >= std::atoi(stop)) break;   // dev: a quick look
        }
    }

    // loud: up to full scale, a soft clip driving the peaks round, then to -0.3 dBFS (MD_REEL_RAW: only normalised)
    const bool raw = std::getenv("MD_REEL_RAW") != nullptr;
    float peak = 0;
    for (size_t i = 0; i < L.size(); ++i) peak = std::max(peak, std::max(std::abs(L[i]), std::abs(R[i])));
    const float drive = raw ? 0.001f : 4.6f, norm = std::tanh(drive);
    double sum = 0;
    float peak2 = 0;
    for (size_t i = 0; i < L.size(); ++i) {
        for (auto* v : {&L[i], &R[i]}) { *v = std::tanh(drive * *v / peak) / norm; peak2 = std::max(peak2, std::abs(*v)); }
    }
    const float out = juce::Decibels::decibelsToGain(-0.3f) / peak2;
    for (size_t i = 0; i < L.size(); ++i) { L[i] *= out; R[i] *= out; sum += double(L[i]) * L[i] + double(R[i]) * R[i]; }
    const int fade = int(0.4 * rate);
    for (int i = 0; i < fade && i < int(L.size()); ++i) { const float k = float(i) / float(fade); L[L.size() - 1 - size_t(i)] *= k; R[R.size() - 1 - size_t(i)] *= k; }
    juce::AudioBuffer<float> all(2, int(L.size()));
    all.copyFrom(0, 0, L.data(), int(L.size())); all.copyFrom(1, 0, R.data(), int(R.size()));
    juce::File wavFile{juce::String(env.argv[2])};
    wavFile.deleteFile();
    juce::WavAudioFormat wav;
    auto stream = std::unique_ptr<juce::FileOutputStream>(wavFile.createOutputStream());
    if (!stream) return 1;
    if (auto w = std::unique_ptr<juce::AudioFormatWriter>(wav.createWriterFor(stream.get(), rate, 2, 24, {}, 0))) {
        stream.release();
        w->writeFromAudioSampleBuffer(all, 0, all.getNumSamples());
    }
    const double rms = std::sqrt(sum / double(2 * L.size()));
    std::printf("REEL: %.1f s, %d frames, RMS %.1f dBFS (peak before %.3f) -> %s\n", double(L.size()) / rate, frameNo,
                double(juce::Decibels::gainToDecibels(float(rms))), double(peak), env.argv[2]);
    for (int g = 0; g < count; ++g) std::printf("  %-10s at %5.2f s\n", genres[g].title, [&] { double t = 0; for (int k = 0; k < g; ++k) t += d.patterns[size_t(k)].length / 4.0 * 60.0 / genres[k].bpm; return t; }());
    return 0;
}

} // namespace mnm::plugin::md::test
