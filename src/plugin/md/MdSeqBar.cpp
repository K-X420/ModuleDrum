// The sequencer bar: PLAY|REC, pattern / track pickers, pages, X2|DEL, GRID|MIX
#include "MdLibraryUi.h"
#include "MdNames.h"
#include "MdParams.h"

namespace mnm::plugin::md {

using namespace one;   // LcdCanvas and the LcdDraw helpers
namespace spec = mnm::uispec;

// the bold face (8 rows) in a 15-row box: one row nearer the top than the bottom (it cannot be centred exactly; a
// row high reads centred, a row low reads sunk)
static constexpr int kBoldY = 3;

void MdSeqBar::resized()
{
    const int arrowW = 12, gap = 6;
    int x = 0;
    auto take = [&](Part p, int pw) { m_rects[size_t(p)] = {x, 0, pw, kLcdH}; x += pw - 1; };
    take(Play, 33);
    x += 1 + gap;
    take(Rec, 26);
    x += 1 + gap;
    take(Grid, 30);
    x += 1 + gap;
    take(TrkPrev, arrowW); take(Trk, 52); take(TrkNext, arrowW); take(Mute, 13);
    x += 1 + gap;
    take(PtnPrev, arrowW); take(Ptn, 42); take(PtnNext, arrowW);
    x += 1 + gap;
    take(Pages, 3 + 4 * 9 + 1);
    x += 1 + gap;
    take(Edit, 22);
    x += 1 + gap;
    take(DelPg, 23);
    x += 1 + gap;
    take(Mix, 25);
    x += 1 + gap;
    take(Step, juce::jmax(40, getWidth() / kS - x));
    if (m_anchors.sndR > m_anchors.sndL && m_anchors.kitR > m_anchors.kitL && m_anchors.menuW > 0) {
        // in the header's columns, the same gap between every separate part:
        // PLAY|REC under the kit, TRK from under the sound selector, STEP under MENU, GRID|MIX ending under SYNC,
        // then X2|DEL, the pages and PTN right to left; TRK's name box takes the space left over
        const int headerGap = 4;   // the header's gap (MdKitStrip's, 8 px)
        x = m_anchors.kitL;   // PLAY and REC joined, spanning the KIT selector to its save icon
        const int pr = juce::jmax(52, m_anchors.kitR - x);
        take(Play, pr / 2 + 1); take(Rec, pr - pr / 2);
        m_rects[size_t(Step)] = {m_anchors.menuX, 0, m_anchors.menuW, kLcdH};
        int right = m_anchors.syncX + m_anchors.syncW;   // the free space ends here (exclusive)
        auto pair = [&](Part a, int aw, Part b, int bw) {   // joined: a shared edge
            m_rects[size_t(b)] = {right - bw, 0, bw, kLcdH};
            m_rects[size_t(a)] = {right - bw - aw + 1, 0, aw, kLcdH};
            right -= aw + bw - 1 + headerGap;
        };
        pair(Grid, 30, Mix, 24);
        pair(Edit, 22, DelPg, 24);
        const int pw = m_rects[size_t(Pages)].getWidth();
        m_rects[size_t(Pages)] = {right - pw, 0, pw, kLcdH};
        right -= pw + headerGap;
        const int ptnW = 2 * arrowW + 42 - 2, trkFixed = 2 * arrowW + 13 - 3;
        const int trkW = right - ptnW - headerGap - m_anchors.sndL - trkFixed;
        x = m_anchors.sndL;   // TRK's left arrow under the sound selector's
        take(TrkPrev, arrowW); take(Trk, juce::jmax(40, trkW)); take(TrkNext, arrowW); take(Mute, 13);
        x += 1 + headerGap;
        take(PtnPrev, arrowW); take(Ptn, 42); take(PtnNext, arrowW);
        if (trkW < 40) {   // too narrow: the pages and the pairs packed left to right after PTN
            x += 1 + headerGap;
            take(Pages, pw); x += 1 + headerGap;
            take(Edit, 22); take(DelPg, 24); x += 1 + headerGap;
            take(Grid, 30); take(Mix, 24); x += 1 + headerGap;
            take(Step, juce::jmax(40, getWidth() / kS - x));   // STEP after them too (not left under MENU, where they would run into it)
        }
    }
}

void MdSeqBar::paint(juce::Graphics& g)
{
    LcdCanvas cv(getWidth() / kS, kLcdH);
    auto box = [&](Part p, bool solid) {
        const auto r = m_rects[size_t(p)];
        if (solid) cv.fillRect(r.getX(), r.getY(), r.getWidth(), r.getHeight(), true);
        frame(cv, r);
        if (!solid && m_hover == p) frame(cv, r.reduced(1));
        return r;
    };
    // PLAY / REC: an icon and a word in the bold face (as GRID, MIX), centred in the box together
    auto iconWord = [&](juce::Rectangle<int> r, int iconW, const char* word, bool ink) {
        const int gap = 3, total = iconW + gap + LcdCanvas::textWidth(spec::kFontBold8, word);
        const int x = r.getX() + (r.getWidth() - total) / 2;
        cv.text(spec::kFontBold8, word, x + iconW + gap, r.getY() + kBoldY, ink);
        return x;   // where the icon goes
    };
    static const char* const disc7[7] = {"..###..", ".#####.", "#######", "#######", "#######", ".#####.", "..###.."};
    {   // PLAY: a triangle, a square while playing
        const auto r = box(Play, m_s.playing);
        const bool ink = !m_s.playing;
        const int cy = r.getCentreY();
        if (m_s.playing) { const int x = iconWord(r, 7, "STOP", ink); cv.fillRect(x, cy - 3, 7, 7, ink); }
        else { const int x = iconWord(r, 5, "PLAY", ink); for (int i = 0; i < 5; ++i) for (int y = -4 + i; y <= 4 - i; ++y) cv.set(x + i, cy + y, ink); }
    }
    {   // REC: a dot, the box solid while armed (the dot blinks while recording)
        const auto r = box(Rec, m_s.rec);
        const bool ink = !m_s.rec;
        const bool dot = !m_s.recording || (juce::Time::getMillisecondCounter() / 300) % 2 == 0;
        const int x = iconWord(r, 7, "REC", ink), y0 = r.getCentreY() - 3;
        if (dot) for (int y = 0; y < 7; ++y) for (int c = 0; c < 7; ++c) if (disc7[y][c] == '#') cv.set(x + c, y0 + y, ink);
    }
    {   // GRID: solid while the keys are steps
        const auto r = box(Grid, m_s.grid || m_s.scenes);
        static const char* const names[9] = {"GRID", "ACC", "SLD", "SWG", "COND", "TIME", "RTRG", "GEN", "LANE"};
        cv.textCentred(spec::kFontBold8, m_s.scenes ? "SCN" : names[juce::jlimit(0, 8, m_s.mark)], r.getX(), r.getWidth(), r.getY() + kBoldY, !(m_s.grid || m_s.scenes));
    }
    auto arrows = [&](Part prev, Part next) {
        auto r = box(prev, false); arrowH(cv, r.getCentreX(), r.getCentreY(), true, true);
        r = box(next, false); arrowH(cv, r.getCentreX(), r.getCentreY(), false, true);
    };
    auto labelled = [&](Part p, const char* label, const juce::String& text) {
        const auto r = box(p, false);
        cv.text(spec::kFontTiny3x5, label, r.getX() + 4, r.getY() + 5, true);
        const int lw = LcdCanvas::textWidth(spec::kFontTiny3x5, label), tx = r.getX() + (*label ? 8 + lw : 4);
        cv.text(spec::kFontBold8, fit(spec::kFontBold8, text, r.getRight() - tx - 3).toRawUTF8(), tx, r.getY() + kBoldY, true);
    };
    arrows(TrkPrev, TrkNext);
    {   // MUTE of the selected track: a speaker (crossed out and solid while muted)
        const auto r = box(Mute, m_s.muted);
        pixelIcon(cv, m_s.muted ? kIconMuted : kIconSpeaker, 7, r.getX() + 3, r.getY() + 4, !m_s.muted);
    }
    labelled(Trk, "", "T" + juce::String(m_s.track + 1) + " " + m_s.machine);   // "T1 BD" says it is the track
    arrows(PtnPrev, PtnNext);
    if (m_s.chain.isNotEmpty()) labelled(Ptn, "CHN", m_s.chain);
    else labelled(Ptn, "PTN", juce::String(kPatternNames[juce::jlimit(0, 127, m_s.pattern)]) + (m_s.empty ? "-" : ""));
    if (m_s.scenes) {   // SCENES: the crossfader, A's scene on the left, B's on the right
        const auto r = faderRect();
        if (m_hover == Pages || m_hover == Edit || m_hover == DelPg) frame(cv, r.reduced(1));
        frame(cv, r);
        const juce::String a = "A" + juce::String(m_s.sceneA + 1), b = "B" + juce::String(m_s.sceneB + 1);
        cv.text(spec::kFontBold8, a.toRawUTF8(), r.getX() + 4, r.getY() + kBoldY, true);
        cv.text(spec::kFontBold8, b.toRawUTF8(), r.getRight() - 4 - LcdCanvas::textWidth(spec::kFontBold8, b.toRawUTF8()), r.getY() + kBoldY, true);
        int x0 = 0, x1 = 0;
        faderSpan(x0, x1);
        const int cy = r.getCentreY();
        for (int x = x0; x <= x1; x += 2) cv.set(x, cy, true);
        cv.fillRect(x0, cy - 2, 1, 5, true); cv.fillRect(x1, cy - 2, 1, 5, true); cv.fillRect((x0 + x1) / 2, cy - 1, 1, 3, true);
        const int tx = x0 + int(std::lround(m_s.xfade * float(x1 - x0)));
        cv.fillRect(tx - 2, r.getY() + 3, 5, r.getHeight() - 6, true);
    }
    if (!m_s.scenes) {   // the pages: a ring each (a point past the length), solid when playing, underlined when GRID shows it
        const auto r = box(Pages, false);
        const int pages = (juce::jlimit(1, 64, m_s.length) + 15) / 16, playing = m_s.step >= 0 ? m_s.step / 16 : -1;
        for (int i = 0; i < 4; ++i) {
            const int x = dotX(i), y = r.getY() + (r.getHeight() - 7) / 2;   // centred in the box (the GRID underline goes in the gap below)
            if (i >= pages) { cv.fillRect(x + 3, y + 3, 1, 1, true); continue; }
            static const char* const ring[7] = {"..###..", ".#...#.", "#.....#", "#.....#", "#.....#", ".#...#.", "..###.."};
            static const char* const disc[7] = {"..###..", ".#####.", "#######", "#######", "#######", ".#####.", "..###.."};
            const auto& shape = i == playing && m_s.beat ? disc : ring;   // the playing page blinks on the beat
            for (int dy = 0; dy < 7; ++dy) for (int dx = 0; dx < 7; ++dx) if (shape[dy][dx] == '#') cv.set(x + dx, y + dy, true);
            if (m_s.grid && i == m_s.page) cv.fillRect(x + 1, y + 8, 5, 1, true);
        }
    }
    if (!m_s.scenes) {   // X2: the pattern doubled (two pages, x2)
        const auto r = box(Edit, false);
        cv.text(spec::kFontBold8, "X2", r.getX() + 4, r.getY() + kBoldY, true);
    }
    if (!m_s.scenes) {   // DEL PG: the shown page removed (the pages after it move up)
        const auto r = box(DelPg, false);
        cv.textCentred(spec::kFontBold8, "DEL", r.getX(), r.getWidth(), r.getY() + kBoldY, true);
    }
    {   // MIX: solid while the mixer is open
        const auto r = box(Mix, m_s.mix);
        cv.textCentred(spec::kFontBold8, "MIX", r.getX(), r.getWidth(), r.getY() + kBoldY, !m_s.mix);
    }
    {
        const juce::String s = m_s.flash.isNotEmpty() ? m_s.flash : m_s.seqOff ? juce::String("SEQ OFF")
                             : m_s.step >= 0 ? juce::String(m_s.step + 1).paddedLeft('0', 2) + "/" + juce::String(m_s.length) : "--/" + juce::String(m_s.length);
        if (m_s.flash.isNotEmpty()) {   // a confirmation: the whole box, no label
            const auto r = box(Step, true);
            cv.text(spec::kFontBold8, fit(spec::kFontBold8, s, r.getWidth() - 6).toRawUTF8(), r.getX() + 4, r.getY() + kBoldY, false);
        } else {
            // the longest form that fits whole: STEP 47/64  ROW 12, then without the label, then R12, then in the small font
            const auto r = box(Step, false);
            const juce::String row = m_s.row >= 0 ? juce::String(m_s.row + 1) : juce::String();
            const int labelW = LcdCanvas::textWidth(spec::kFontTiny3x5, "STEP");
            auto fits = [&](const spec::Font& f, const juce::String& t, int x0) { return LcdCanvas::textWidth(f, t.toRawUTF8()) <= r.getRight() - 3 - x0; };
            const juce::String longForm = row.isEmpty() ? s : s + "  ROW " + row, shortForm = row.isEmpty() ? s : s + " R" + row;
            // (without the label the value is centred in the box)
            auto centred = [&](const spec::Font& f, const juce::String& t, int y) { cv.textCentred(f, t.toRawUTF8(), r.getX(), r.getWidth(), r.getY() + y, true); };
            if (fits(spec::kFontBold8, longForm, r.getX() + 8 + labelW)) labelled(Step, "STEP", longForm);
            else if (fits(spec::kFontBold8, longForm, r.getX() + 4)) centred(spec::kFontBold8, longForm, kBoldY);
            else if (fits(spec::kFontBold8, shortForm, r.getX() + 4)) centred(spec::kFontBold8, shortForm, kBoldY);
            else centred(spec::kFontTiny3x5, fit(spec::kFontTiny3x5, shortForm, r.getWidth() - 7), 5);
        }
    }
    cv.draw(g, 0, 0, kS);
}

juce::Rectangle<int> MdSeqBar::faderRect() const
{
    return m_rects[size_t(Pages)].getUnion(m_rects[size_t(Edit)]).getUnion(m_rects[size_t(DelPg)]);
}

void MdSeqBar::faderSpan(int& x0, int& x1) const
{
    const auto r = faderRect();
    const int lw = LcdCanvas::textWidth(spec::kFontBold8, "A16");
    x0 = r.getX() + 4 + lw + 5;
    x1 = juce::jmax(x0 + 8, r.getRight() - 4 - lw - 5);
}

float MdSeqBar::faderValueAt(int lcdX) const
{
    int x0 = 0, x1 = 0;
    faderSpan(x0, x1);
    return juce::jlimit(0.0f, 127.0f, float(lcdX - x0) / float(x1 - x0) * 127.0f);
}

void MdSeqBar::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (!m_s.scenes || !faderRect().contains(e.getPosition() / kS) || !onXfade) { Component::mouseWheelMove(e, w); return; }
    const float d = std::abs(w.deltaY) >= std::abs(w.deltaX) ? w.deltaY : -w.deltaX;
    if (d == 0.0f) return;
    const float step = e.mods.isShiftDown() ? 1.0f : 4.0f;
    onXfade(juce::jlimit(0.0f, 127.0f, m_s.xfade * 127.0f + ((d > 0) != w.isReversed ? step : -step)));
}

MdSeqBar::Part MdSeqBar::partAt(juce::Point<int> lcd) const
{
    for (int i = 0; i < kParts; ++i) if (m_rects[size_t(i)].contains(lcd)) return Part(i);
    return None;
}

void MdSeqBar::mouseDrag(const juce::MouseEvent& e)
{
    if (m_xfDrag) { if (onXfade) onXfade(faderValueAt(e.getPosition().x / kS)); return; }
    if (m_dragged || (m_downPart != Ptn && m_downPart != Trk) || e.getDistanceFromDragStart() < 8) return;
    m_dragged = true;
    if (onDragOut) onDragOut(m_downPart);
}

void MdSeqBar::mouseDown(const juce::MouseEvent& e)
{
    const auto lcd = e.getPosition() / kS;
    const auto p = partAt(lcd);
    m_downPart = p; m_dragged = false;
    const bool edit = e.mods.isShiftDown() || e.mods.isCommandDown() || e.mods.isCtrlDown() || e.mods.isAltDown();
    if (m_s.scenes && (p == Pages || p == Edit || p == DelPg)) {   // the crossfader: a click jumps, a drag follows
        if (e.mods.isAltDown() || e.getNumberOfClicks() > 1) { if (onXfade) onXfade(lcd.x < faderRect().getCentreX() ? 0.0f : 127.0f); return; }
        m_xfDrag = true;
        if (onXfade) onXfade(faderValueAt(lcd.x));
        return;
    }
    if (p == Grid && e.mods.isShiftDown()) { if (onEditClick) onEditClick(Grid, -1, e.mods); return; }   // SCENES
    if (p == Pages) {
        for (int i = 0; i < 4; ++i)
            if (lcd.x >= dotX(i) - 2 && lcd.x <= dotX(i) + 8) {
                if (edit) { if (onEditClick) onEditClick(Pages, i, e.mods); }
                else if (onPage) onPage(i);
                return;
            }
        return;
    }
    if (edit && (p == Trk || p == Ptn || p == PtnPrev || p == PtnNext)) { if (onEditClick) onEditClick(p, -1, e.mods); return; }
    if (p != None && onPart) onPart(p);
}

void MdSeqBar::mouseMove(const juce::MouseEvent& e)
{
    const auto p = partAt(e.getPosition() / kS);
    if (p != m_hover) { m_hover = p; repaint(); }
    if (m_s.scenes && (p == Pages || p == Edit || p == DelPg)) {
        setTooltip("XFADE: morphs every knob locked in scene A or B between them (a host parameter). Drag or wheel it; double-click: "
                   "the nearer end. Pick A / B: Alt+click / Shift+click a scene key");
        return;
    }
    if (m_s.scenes && p == Grid) { setTooltip("SCENES (N): the keys are 16 scenes. Click to go back to the tracks"); return; }
    switch (p) {
        case Play: setTooltip(m_s.hostPlaying ? "The host's transport is running: the pattern follows it" : "Play / stop the pattern on the plugin's own clock (while the host is stopped)"); break;
        case Rec: setTooltip("REC: while the pattern plays, the trigs you play (MIDI notes, the track keys) go onto the step playing, "
                             "and a knob turned during a step locks its value on the next step's trig. Ctrl+Z takes a run back"); break;
        case Grid: setTooltip(m_s.grid ? "GRID (G): the keys are the selected track's steps (click a step to place a trig). Click to get the tracks back. Shift+click (N): SCENES"
                                       : "GRID (G): click to place steps with the keys (the selected track's steps; TRK < > picks the track). Shift+click (N): SCENES, the crossfader"); break;
        case Mute: setTooltip(m_s.muted ? "The track is muted: its trigs don't play. Click to unmute (Alt+click a key in GRID: that key's track)"
                                         : "Mute the track (Alt+click a key in GRID: that key's track)"); break;
        case TrkPrev: case TrkNext: setTooltip("The track whose steps GRID shows and edits"); break;
        case Trk: setTooltip("The track GRID edits. Drag it into Ableton: its MIDI clip. Shift+click: copy it. Ctrl+click: paste onto it. Alt+click: clear it"); break;
        case Ptn: setTooltip("The pattern. Drag it into Ableton: a MIDI clip. Shift+click: copy it. Ctrl+click: paste onto it. Alt+click: clear its steps"); break;
        case PtnPrev: case PtnNext: setTooltip("The pattern (PTN): played by SEQ, edited by GRID (\"-\" = an empty slot). Shift+click >: add the next pattern to a chain; Shift+click <: take the last one off"); break;
        case Pages: setTooltip("Pages of 16 steps: solid = playing, underlined = shown by GRID. Click: show it. Shift+click: copy it (all tracks). "
                               "Ctrl+click: paste onto it. Alt+click: clear it. Keys: Ctrl+C / Ctrl+V / Delete on the shown page"); break;
        case Step: setTooltip(m_s.seqOff ? "SEQ is OFF (OUT tab): the pattern does not play. PLAY, a placed step or REC turns it on" : "The playing step / the pattern's length"); break;
        case Edit: setTooltip("X2: the pattern doubled (its steps again after themselves, twice the length). Ctrl+D"); break;
        case DelPg: setTooltip("DEL PAGE: remove the shown page (the pages after it move up; a 1-page pattern is cleared). Ctrl+Z undoes it"); break;
        case Mix: setTooltip("MIX: the 16 tracks' levels, pans, mutes and solos (M)"); break;
        default: setTooltip({}); break;
    }
}

} // namespace mnm::plugin::md
