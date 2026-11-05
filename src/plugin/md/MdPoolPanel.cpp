// The SOUND POOL (as the Analog Rytm's): 64 sounds kept with the set that a step's sound lock plays (GRID, P window)
#include "MdLibraryUi.h"
#include "MdNames.h"
#include "MdParams.h"

namespace mnm::plugin::md {

using namespace one;   // LcdCanvas and the LcdDraw helpers
namespace spec = mnm::uispec;

namespace { constexpr int kPpTitleH = 10, kPpGridY = 13, kPpRowH = 7; }

juce::Rectangle<int> MdPoolPanel::slotRect(int i) const
{
    const int w = getWidth() / kS, cw = (w - 6) / kCols;
    return {3 + (i / kRows) * cw, kPpGridY + (i % kRows) * kPpRowH, cw - 2, kPpRowH};
}

juce::Rectangle<int> MdPoolPanel::buttonRect(int b) const
{
    const int h = getHeight() / kS, by = h - 13;
    static const int x[4] = {6, 70, 116, 160}, wd[4] = {58, 40, 38, 58};
    return {x[b], by, wd[b], 10};
}

void MdPoolPanel::paint(juce::Graphics& g)
{
    const int w = getWidth() / kS, h = getHeight() / kS;
    LcdCanvas cv(w, h);
    cv.fillRect(0, 0, w, h, false);
    frame(cv, {0, 0, w, h});
    cv.fillRect(0, 0, w, kPpTitleH, true);
    cv.text(spec::kFontBold8, "SOUND POOL", 4, 1, false);
    cv.text(spec::kFontTiny3x5, "GRID P: A TRIG PLAYS THE SELECTED SOUND", 76, 3, false);
    cv.text(spec::kFontBold8, "X", w - 10, 1, false);
    for (int i = 0; i < kSlots; ++i) {
        const auto r = slotRect(i);
        const bool sel = i == m_sel;
        if (sel) cv.fillRect(r.getX(), r.getY(), r.getWidth(), r.getHeight(), true);
        const auto n = name ? name(i) : juce::String();
        cv.text(spec::kFontTiny3x5, juce::String(i + 1).paddedLeft('0', 2).toRawUTF8(), r.getX() + 1, r.getY() + 1, !sel);
        cv.text(spec::kFontTiny3x5, fit(spec::kFontTiny3x5, n.isEmpty() ? juce::String("--") : n.toUpperCase(), r.getWidth() - 12).toRawUTF8(), r.getX() + 11, r.getY() + 1, !sel);
    }
    const int by = h - 13;
    cv.dotsH(2, w - 3, by - 3);
    const bool used = name && name(m_sel).isNotEmpty();
    for (int b = 0; b < 4; ++b) {
        const auto r = buttonRect(b);
        const bool live = b == 0 || b == 3 || used;
        if (live) frame(cv, r); else dottedFrame(cv, r);
        const juce::String label = b == 0 ? "STORE T" + juce::String((track ? track() : 0) + 1)
                                 : b == 1 ? juce::String("RENAME") : b == 2 ? juce::String("CLEAR") : "PLAY ON T" + juce::String((track ? track() : 0) + 1);
        cv.text(spec::kFontTiny3x5, label.toRawUTF8(), r.getX() + 4, r.getY() + 3, true);
    }
    cv.draw(g, 0, 0, kS);
}

void MdPoolPanel::select(int i)
{
    m_sel = juce::jlimit(0, kSlots - 1, i);
    if (onSelect) onSelect(m_sel);
    repaint();
}

void MdPoolPanel::mouseDown(const juce::MouseEvent& e)
{
    const auto p = e.getPosition() / kS;
    const int w = getWidth() / kS;
    if (p.y < kPpTitleH) { if (p.x >= w - 14) { setVisible(false); if (onClose) onClose(); } return; }
    for (int i = 0; i < kSlots; ++i) if (slotRect(i).contains(p)) { select(i); return; }
    for (int b = 0; b < 4; ++b)
        if (buttonRect(b).contains(p)) {
            if (b == 0 && onStore) onStore(m_sel);
            else if (b == 1 && onRename) onRename(m_sel);
            else if (b == 2 && onClear) onClear(m_sel);
            else if (b == 3 && onPlay) onPlay(m_sel);
            repaint();
            return;
        }
}

void MdPoolPanel::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto p = e.getPosition() / kS;
    for (int i = 0; i < kSlots; ++i) if (slotRect(i).contains(p) && onRename) { select(i); onRename(i); return; }
}

bool MdPoolPanel::keyPressed(const juce::KeyPress& k)
{
    const int code = k.getKeyCode();
    if (code == juce::KeyPress::escapeKey) { setVisible(false); if (onClose) onClose(); return true; }
    if (code == juce::KeyPress::upKey) { select(m_sel - 1); return true; }
    if (code == juce::KeyPress::downKey) { select(m_sel + 1); return true; }
    if (code == juce::KeyPress::leftKey) { select(m_sel - kRows); return true; }
    if (code == juce::KeyPress::rightKey) { select(m_sel + kRows); return true; }
    if (code == juce::KeyPress::returnKey) { if (onRename) onRename(m_sel); return true; }
    if (code == juce::KeyPress::spaceKey) { if (onPlay) onPlay(m_sel); return true; }
    if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey) { if (onClear) onClear(m_sel); repaint(); return true; }
    return false;
}

} // namespace mnm::plugin::md
