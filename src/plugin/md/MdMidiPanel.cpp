// The MIDI settings panel (the MD's GLOBAL > MIDI pages, plus the note map)
#include "MdLibraryUi.h"
#include "MdNames.h"
#include "MdParams.h"
#include <cmath>

namespace mnm::plugin::md {

using namespace one;   // LcdCanvas and the LcdDraw helpers
namespace spec = mnm::uispec;

namespace {
juce::String noteName(int n)   // as Ableton names them: 60 = C3
{
    static const char* const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return n < 0 ? juce::String("--") : juce::String(names[n % 12]) + juce::String(n / 12 - 2);
}
constexpr int kMpTitleH = 10, kMpRowY = 16, kMpRowH = 14, kMpGridY = 78;
}

// INPUT TRIGS page cells: 30 + 5 x input + (track, gate, sens, vmin, vmax)
constexpr int kInX[5] = {28, 82, 124, 166, 208}, kInW[5] = {50, 38, 38, 38, 38}, kInY = 30, kInRowH = 18, kInMeterX = 254;

juce::Rectangle<int> MdMidiPanel::cellRect(int c) const
{
    if (c >= 30 && c < 40) { const int i = (c - 30) / 5, k = (c - 30) % 5; return {kInX[k], kInY + i * kInRowH, kInW[k], 14}; }
    if (c < 3) return {110, kMpRowY + c * kMpRowH - 2, 80, 12};
    if (c == 19) return {194, kMpRowY + kMpRowH - 2, 46, 12};   // PRG CHANGE's channel
    if (c == 25) return {194, kMpRowY + 2 * kMpRowH - 2, 54, 12};   // CTRL IN (next to MIDI OUT)
    if (c == 26) return {194, kMpRowY - 2, 62, 12};   // TEMPO OUT, CTRL OUT (next to BASE CHANNEL)
    if (c == 27) return {260, kMpRowY - 2, 58, 12};
    if (c >= 20) {   // the PATTERN NOTES row, under the note map
        static const int x[5] = {110, 194, 262, 302, 352}, w[5] = {80, 64, 36, 46, 40};
        return {x[c - 20], kMpGridY + 46, w[c - 20], 12};
    }
    const int t = c - 3;
    return {6 + (t % 8) * 48, kMpGridY + (t / 8) * 22, 44, 19};
}

int MdMidiPanel::cellAt(juce::Point<int> p) const
{
    for (int c = m_page ? 30 : 0; c < (m_page ? 40 : 28); ++c) if (cellRect(c).contains(p)) return c;
    return -1;
}

int MdMidiPanel::cellValue(const Values& v, int c) const
{
    if (c == 0) return v.baseChannel;
    if (c == 1) return v.programChange;
    if (c == 2) return v.midiOut;
    if (c == 19) return v.pcChannel;
    if (c == 20) return v.ptnMode;
    if (c == 21) return v.ptnFrom;
    if (c == 22) return v.ptnBank;
    if (c == 23) return v.startNote;
    if (c == 24) return v.stopNote;
    if (c == 25) return v.ctrlIn;
    if (c == 26) return v.tempoOut;
    if (c == 27) return v.ctrlOut;
    if (c >= 30 && c < 40) {
        const size_t i = size_t((c - 30) / 5);
        switch ((c - 30) % 5) { case 0: return v.inTrack[i]; case 1: return v.inGate[i]; case 2: return v.inSense[i]; case 3: return v.inMin[i]; default: return v.inMax[i]; }
    }
    return v.note[size_t(c - 3)];
}

void MdMidiPanel::change(int c, int to)
{
    if (!get || !set) return;
    auto v = get();
    if (c == 0) v.baseChannel = juce::jlimit(-1, 15, to);
    else if (c == 1) v.programChange = juce::jlimit(0, 3, to);
    else if (c == 2) v.midiOut = juce::jlimit(0, 2, to);
    else if (c == 19) v.pcChannel = juce::jlimit(0, 16, to);
    else if (c == 20) v.ptnMode = juce::jlimit(0, 2, to);
    else if (c == 21) {   // white keys only: a step is the next white key
        const bool black[12] = {false, true, false, true, false, false, true, false, true, false, true, false};
        int n = juce::jlimit(-1, 127, to);
        const int dir = to >= cellValue(get(), 21) ? 1 : -1;
        while (n >= 0 && n < 128 && black[n % 12]) n += dir;
        v.ptnFrom = juce::jlimit(-1, 127, n);
    }
    else if (c == 22) v.ptnBank = juce::jlimit(0, 7, to);
    else if (c == 23) v.startNote = juce::jlimit(-1, 127, to);
    else if (c == 24) v.stopNote = juce::jlimit(-1, 127, to);
    else if (c == 25) v.ctrlIn = juce::jlimit(0, 1, to);
    else if (c == 26) v.tempoOut = juce::jlimit(0, 1, to);
    else if (c == 27) v.ctrlOut = juce::jlimit(0, 1, to);
    else if (c >= 30 && c < 40) {
        const size_t i = size_t((c - 30) / 5);
        switch ((c - 30) % 5) {
            case 0: v.inTrack[i] = juce::jlimit(0, 16, to); break;
            case 1: v.inGate[i] = juce::jlimit(0, 127, to); break;
            case 2: v.inSense[i] = juce::jlimit(0, 127, to); break;
            case 3: v.inMin[i] = juce::jlimit(0, 127, to); break;
            default: v.inMax[i] = juce::jlimit(0, 127, to); break;
        }
    }
    else {
        const int t = c - 3, n = juce::jlimit(-1, 127, to);
        if (n >= 0) for (auto& o : v.note) if (o == n) o = -1;   // a note plays one track: taken from another
        v.note[size_t(t)] = n;
    }
    set(v);
    repaint();
}

void MdMidiPanel::paint(juce::Graphics& g)
{
    const int w = getWidth() / kS, h = getHeight() / kS;
    LcdCanvas cv(w, h);
    cv.fillRect(0, 0, w, h, false);
    frame(cv, {0, 0, w, h});
    cv.fillRect(0, 0, w, kMpTitleH, true);
    cv.text(spec::kFontBold8, m_page ? "INPUT TRIGS" : "MIDI SETTINGS", 4, 1, false);
    cv.text(spec::kFontBold8, "X", w - 10, 1, false);
    const auto v = get ? get() : Values{};
    const int by = h - 14;   // the buttons
    if (m_page == 1) {   // INPUT TRIGS: the side-chain's two inputs, each trigging a track
        static const char* const heads[5] = {"TRACK", "GATE", "SENS", "VMIN", "VMAX"};
        for (int k = 0; k < 5; ++k) cv.text(spec::kFontTiny3x5, heads[k], kInX[k] + 2, kInY - 8, true);
        cv.text(spec::kFontTiny3x5, "LEVEL", kInMeterX + 2, kInY - 8, true);
        for (int i = 0; i < 2; ++i) {
            const int y = kInY + i * kInRowH;
            cv.text(spec::kFontBold8, i ? "B" : "A", 6, y + 3, true);
            cv.text(spec::kFontTiny3x5, i ? "R" : "L", 16, y + 5, true);
            for (int k = 0; k < 5; ++k) {
                const auto r = cellRect(30 + 5 * i + k);
                frame(cv, r);
                const int val = cellValue(v, 30 + 5 * i + k);
                const juce::String s = k == 0 ? (val >= 16 ? juce::String("OFF") : "T" + juce::String(val + 1)) : juce::String(val);
                cv.text(spec::kFontBold8, s.toRawUTF8(), r.getX() + 4, r.getY() + 3, true);
            }
            const juce::Rectangle<int> m{kInMeterX, y + 3, w - 8 - kInMeterX, 8};   // the level, -60..0 dB
            frame(cv, m);
            const float lvl = inputLevel ? inputLevel(i) : 0.0f;
            const float db = lvl > 1e-6f ? 20.0f * std::log10(lvl) : -100.0f;
            const int fill = int(juce::jlimit(0.0f, 1.0f, (db + 60.0f) / 60.0f) * float(m.getWidth() - 4));
            if (fill > 0) cv.fillRect(m.getX() + 2, m.getY() + 2, fill, m.getHeight() - 4, true);
        }
        static const char* const lines[5] = {
            "THE SIDE-CHAIN INPUT PLAYS A TRACK: A = ITS LEFT CHANNEL; B = ITS RIGHT.",
            "A TRIG WHEN THE LEVEL JUMPS BY MORE THAN GATE WITHIN 0.7 MS; THEN IT RESTS 12 MS.",
            "VELOCITY (THE TRIG VOLUME): VMIN; A HARDER HIT ADDS MORE WITH SENS; UP TO VMAX.",
            "AS ON THE MD: VELOCITY 0 PLAYS NOTHING. RAISE SENS OR VMIN (FACTORY: BOTH 0).",
            "THE TRIG GROUP FOLLOWS; WHILE RECORDING THE TRIGS ARE RECORDED."};
        for (int l = 0; l < 5; ++l) cv.text(spec::kFontTiny3x5, lines[l], 6, kInY + 2 * kInRowH + 8 + l * 9, true);
        cv.dotsH(2, w - 3, by - 3);
        frame(cv, {76, by, 72, 10}); cv.text(spec::kFontTiny3x5, "FROM PROJECT...", 80, by + 3, true);
        frame(cv, {154, by, 60, 10}); cv.text(spec::kFontTiny3x5, "MIDI PAGE", 160, by + 3, true);
        cv.draw(g, 0, 0, kS);
        return;
    }
    static const char* const labels[3] = {"BASE CHANNEL", "PROGRAM CHANGE", "MIDI OUT"};
    static const char* const pcs[4] = {"OFF", "IN", "OUT", "IN+OUT"};
    static const char* const outs[3] = {"OFF", "TRIGS", "TRIGS+CCS"};
    const juce::String vals[3] = {v.baseChannel < 0 ? juce::String("OFF") : juce::String(v.baseChannel + 1), pcs[juce::jlimit(0, 3, v.programChange)], outs[juce::jlimit(0, 2, v.midiOut)]};
    static const char* const notes[3] = {"", "= NEXT PATTERN (AUTO: CH+0..3)", "TRIGS AS NOTES (+ KNOBS AS CCS)"};   // (BASE CHANNEL: its tooltip)
    for (int c = 0; c < 3; ++c) {
        const auto r = cellRect(c);
        cv.text(spec::kFontSmall4x5, labels[c], 6, r.getY() + 3, true);
        frame(cv, r);
        cv.text(spec::kFontBold8, vals[c].toRawUTF8(), r.getX() + 4, r.getY() + 2, true);
        cv.text(spec::kFontTiny3x5, notes[c], (c == 0 ? cellRect(27).getRight() : c == 1 ? cellRect(19).getRight() : c == 2 ? cellRect(25).getRight() : r.getRight()) + 8, r.getY() + 4, true);
    }
    {   // PATTERN NOTES: notes that play patterns (the unit's key map), START, STOP
        const int y = cellRect(20).getY();
        cv.text(spec::kFontSmall4x5, "PATTERN NOTES", 6, y + 3, true);
        static const char* const modes[3] = {"GATE", "MOMENTARY", "QUEUE"};
        auto cell = [&](int c, const char* label, const juce::String& value) {
            const auto r = cellRect(c);
            frame(cv, r);
            int x = r.getX() + 3;
            if (label) { cv.text(spec::kFontTiny3x5, label, x, r.getY() + 4, true); x += LcdCanvas::textWidth(spec::kFontTiny3x5, label) + 3; }
            cv.text(spec::kFontBold8, value.toRawUTF8(), x, r.getY() + 2, true);
        };
        cell(20, nullptr, modes[juce::jlimit(0, 2, v.ptnMode)]);
        cell(21, "FROM", m_learn == 21 ? juce::String("?") : v.ptnFrom == -2 ? juce::String("CUSTOM") : v.ptnFrom < 0 ? juce::String("OFF") : noteName(v.ptnFrom));
        cell(22, "BNK", juce::String::charToString(juce::juce_wchar('A' + juce::jlimit(0, 7, v.ptnBank))));
        cell(23, "START", m_learn == 23 ? juce::String("?") : v.startNote < 0 ? juce::String("--") : noteName(v.startNote));
        cell(24, "STOP", m_learn == 24 ? juce::String("?") : v.stopNote < 0 ? juce::String("--") : noteName(v.stopNote));
    }
    {   // CTRL IN: Ableton's play / stop run the sequencer (OFF: a sound module; PLAY runs it at Ableton's tempo)
        const auto r = cellRect(25);
        frame(cv, r);
        cv.text(spec::kFontTiny3x5, "CTRL IN", r.getX() + 3, r.getY() + 4, true);
        cv.text(spec::kFontBold8, v.ctrlIn ? "ON" : "OFF", r.getX() + 32, r.getY() + 2, true);
    }
    for (int c = 26; c <= 27; ++c) {   // TEMPO OUT (MIDI clock), CTRL OUT (START / STOP / CONTINUE, song position)
        const auto r = cellRect(c);
        frame(cv, r);
        const char* label = c == 26 ? "TEMPO OUT" : "CTRL OUT";
        cv.text(spec::kFontTiny3x5, label, r.getX() + 3, r.getY() + 4, true);
        cv.text(spec::kFontBold8, (c == 26 ? v.tempoOut : v.ctrlOut) ? "ON" : "OFF", r.getX() + 6 + LcdCanvas::textWidth(spec::kFontTiny3x5, label), r.getY() + 2, true);
    }
    {   // PRG CHANGE's channel
        const auto r = cellRect(19);
        frame(cv, r);
        cv.text(spec::kFontTiny3x5, "CH", r.getX() + 3, r.getY() + 4, true);
        const juce::String ch = v.pcChannel > 0 ? juce::String(v.pcChannel) : juce::String("AUTO");
        cv.text(spec::kFontBold8, ch.toRawUTF8(), r.getX() + 14, r.getY() + 2, true);
    }
    cv.dotsH(2, w - 3, kMpGridY - 17);
    cv.text(spec::kFontSmall4x5, "TRIG NOTES (THE NOTE MAP): CLICK, THEN PLAY A NOTE", 6, kMpGridY - 11, true);
    for (int t = 0; t < 16; ++t) {
        const auto r = cellRect(3 + t);
        frame(cv, r);
        const bool lrn = m_learn == 3 + t;
        if (lrn) cv.fillRect(r.getX(), r.getY(), r.getWidth(), r.getHeight(), true);
        cv.text(spec::kFontTiny3x5, ("T" + juce::String(t + 1)).toRawUTF8(), r.getX() + 3, r.getY() + 3, !lrn);
        cv.text(lrn ? spec::kFontTiny3x5 : spec::kFontBold8, lrn ? "PLAY A NOTE" : noteName(v.note[size_t(t)]).toRawUTF8(), r.getX() + 3, r.getY() + (lrn ? 11 : 10), !lrn);
    }
    cv.dotsH(2, w - 3, by - 3);
    frame(cv, {6, by, 64, 10}); cv.text(spec::kFontTiny3x5, "DEFAULT MAP", 10, by + 3, true);
    frame(cv, {76, by, 72, 10}); cv.text(spec::kFontTiny3x5, "FROM PROJECT...", 80, by + 3, true);
    frame(cv, {154, by, 60, 10}); cv.text(spec::kFontTiny3x5, "INPUT TRIGS", 162, by + 3, true);
    cv.draw(g, 0, 0, kS);
}

juce::String MdMidiPanel::getTooltip()
{
    switch (cellAt(getMouseXYRelative() / kS)) {
        case 0: return "BASE CHANNEL: trigs come in and go out on it; the CCs on it and the next three";
        case 1: return "PROGRAM CHANGE: a program change in picks the next pattern; OUT sends one at each pattern change";
        case 2: return "MIDI OUT: the trigs played as notes (TRIGS), and the knobs and locks as CCs too (TRIGS+CCS)";
        case 19: return "PROGRAM CHANGE's channel (AUTO: the base channel and the next three)";
        case 25: return "CTRL IN: the host's play / stop run the sequencer (OFF: only PLAY does)";
        case 26: return "TEMPO OUT: MIDI clock out, 24 a quarter note, also while stopped";
        case 27: return "CTRL OUT: START / STOP out, and the song position with CONTINUE when playback jumps";
        case 30: case 35: return "TRACK: the track this input trigs (OFF: none)";
        case 31: case 36: return "GATE: how sharp a jump in level trigs (higher: only louder hits)";
        case 32: case 37: return "SENS: how much a harder hit raises the velocity (0: always VMIN)";
        case 33: case 38: return "VMIN: the softest velocity";
        case 34: case 39: return "VMAX: the loudest velocity";
        default: return SettableTooltipClient::getTooltip();
    }
}

void MdMidiPanel::mouseDown(const juce::MouseEvent& e)
{
    const auto p = e.getPosition() / kS;
    const int w = getWidth() / kS, h = getHeight() / kS;
    if (p.y < kMpTitleH && p.x >= w - 14) { setVisible(false); if (onClose) onClose(); return; }
    const int by = h - 14;
    if (p.y >= by && p.y < by + 10) {
        if (p.x >= 6 && p.x < 70 && m_page == 0) { if (onDefault) onDefault(); repaint(); }
        else if (p.x >= 76 && p.x < 148) { if (onFromProject) onFromProject(); }
        else if (p.x >= 154 && p.x < 214) { m_page ^= 1; m_learn = -1; if (onLearnCancel) onLearnCancel(); repaint(); }
        return;
    }
    const int c = cellAt(p);
    if (c < 0 || !get) return;
    if (c == 25) { change(25, get().ctrlIn ? 0 : 1); return; }   // a switch
    if (c == 26) { change(26, get().tempoOut ? 0 : 1); return; }
    if (c == 27) { change(27, get().ctrlOut ? 0 : 1); return; }
    m_drag = c; m_dragY = e.getPosition().y; m_dragV = cellValue(get(), c);
    if (c >= 3 && c <= 18 && m_dragV < 0) m_dragV = 36 + (c - 3);
    if (c == 21 && m_dragV < 0) m_dragV = 64;   // E3, as the factory map
    if ((c == 23 || c == 24) && m_dragV < 0) m_dragV = c == 23 ? 91 : 93;
}

void MdMidiPanel::mouseUp(const juce::MouseEvent& e)
{
    const int c = m_drag;
    m_drag = -1;
    if (c < 0 || e.mouseWasDraggedSinceMouseDown()) return;
    const bool noteCell = (c >= 3 && c <= 18) || c == 21 || c == 23 || c == 24;
    if (!noteCell) return;
    if (m_learn == c) { m_learn = -1; if (onLearnCancel) onLearnCancel(); }   // again: cancelled
    else { m_learn = c; if (onLearn) onLearn(); }
    repaint();
}

void MdMidiPanel::learned(int note)
{
    if (m_learn < 0) return;
    const int c = m_learn;
    m_learn = -1;
    change(c, juce::jlimit(0, 127, note));
}

void MdMidiPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (m_drag < 0) return;
    if (std::abs(e.getDistanceFromDragStartY()) < 2) return;   // a click is LEARN, not a turn
    const int steps = (m_dragY - e.getPosition().y) / (2 * kS);
    change(m_drag, m_dragV + steps);
}

void MdMidiPanel::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& d)
{
    const int c = cellAt(e.getPosition() / kS);
    if (c < 0 || !get) return;
    const int dir = d.deltaY > 0 ? 1 : d.deltaY < 0 ? -1 : 0;
    int v = cellValue(get(), c);
    if (c >= 3 && c <= 18 && v < 0) v = 36 + (c - 3);
    if (c == 21 && v == -2) v = 64;
    change(c, v + dir);
}

bool MdMidiPanel::keyPressed(const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) {
        if (m_learn >= 0) { m_learn = -1; if (onLearnCancel) onLearnCancel(); repaint(); return true; }
        setVisible(false); if (onClose) onClose(); return true;
    }
    return false;
}

} // namespace mnm::plugin::md
