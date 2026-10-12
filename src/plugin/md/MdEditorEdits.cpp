// The editor's pattern edits: the keys, the step / page / track / pattern operations, undo
#include "MdEditorInternal.h"
#include <cmath>

namespace mnm::plugin::md {

void MdEditor::holdStep(int step) { holdSteps(step >= 0 ? 1ull << step : 0, step); }

void MdEditor::holdSteps(uint64_t mask, int primary)
{
    if (mask == 0) primary = -1;
    else if (primary < 0 || !((mask >> primary) & 1)) { primary = 0; while (!((mask >> primary) & 1)) ++primary; }   // the first one
    if (mask == m_heldSet && primary == m_heldStep) return;
    m_heldSet = mask;
    const int step = m_heldStep = primary;
    int count = 0; for (int s = 0; s < 64; ++s) count += int((mask >> s) & 1);
    const juce::String badge = step < 0 ? juce::String() : count > 1 ? "LOCK X" + juce::String(count) : "LOCK" + juce::String(step + 1);
    for (auto* pg : {&m_syn, &m_fx, &m_routing, &m_lfo}) pg->setBadge(step >= 0 ? badge.toRawUTF8() : nullptr);
    m_shownMachineId = -2;
    m_pagesMachineId = -2;
    rebuildSynPage();
    refreshGrid();
}

void MdEditor::stepMenu(int s)
{
    const int t = m_track, slot = editSlot();
    const auto p = m_proc.bankPattern(slot);
    auto maskBit = [&](uint32_t editAll, uint64_t global, uint64_t perTrack) { return (((editAll ? global : perTrack) >> s) & 1) != 0; };
    const bool trig = p && ((p->trigs[t] >> s) & 1);
    bool locks = false;
    if (p) for (int q = 0; q < 24; ++q) { const int row = p->lockRow(t, q); if (row >= 0 && p->locks[row][s] <= 127) locks = true; }
    juce::PopupMenu m;
    m.addSectionHeader("STEP " + juce::String(s + 1) + " - TRACK " + juce::String(t + 1));
    m.addItem(1, "Trig", true, trig);
    m.addItem(2, p && p->accentEditAll ? "Accent (all tracks)" : "Accent", true, p && maskBit(p->accentEditAll, p->accent, p->accentPerTrack[t]));
    m.addItem(3, p && p->slideEditAll ? "Slide (all tracks)" : "Slide", true, p && maskBit(p->slideEditAll, p->slide, p->slidePerTrack[t]));
    m.addItem(4, p && p->swingEditAll ? "Swing (all tracks)" : "Swing", true, p && maskBit(p->swingEditAll, p->swing, p->swingPerTrack[t]));
    m.addSeparator();
    m.addItem(5, m_heldStep == s ? "Release locks hold" : "Hold for locks (turn the knobs)");
    m.addItem(6, "Clear this step's locks", locks);
    m.addSeparator();
    m.addItem(7, "Accent / slide / swing per track", true, p && !p->accentEditAll);
    juce::PopupMenu tracks;
    for (int i = 0; i < kTracks; ++i) tracks.addItem(100 + i, "T" + juce::String(i + 1), true, i == t);
    m.addSubMenu("Select track", tracks);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&m_keys), [this, s, t, slot](int r) {
        if (r >= 100) { selectTrack(r - 100); return; }
        if (r == 5) { holdStep(m_heldStep == s ? -1 : s); return; }
        if (r <= 0) return;
        static const char* const names[8] = {"", "trig", "accent", "slide", "swing", "", "clear locks", "per-track marks"};
        doEdit(slot, names[juce::jlimit(0, 7, r)], [&](mnm::mddump::Pattern& p) {
            auto flip = [&](uint32_t editAll, uint64_t& global, uint64_t& perTrack) { (editAll ? global : perTrack) ^= 1ull << s; };
            switch (r) {
                case 1: if ((p.trigs[t] >> s) & 1) { p.trigs[t] &= ~(1ull << s); p.clearStepLocks(t, s); p.clearStepExtras(t, s); } else p.trigs[t] |= 1ull << s; break;
                case 2: flip(p.accentEditAll, p.accent, p.accentPerTrack[t]); break;
                case 3: flip(p.slideEditAll, p.slide, p.slidePerTrack[t]); break;
                case 4: flip(p.swingEditAll, p.swing, p.swingPerTrack[t]); break;
                case 6: p.clearStepLocks(t, s); break;
                case 7: {   // the masks of all tracks <-> each track's own (the current marks carry over)
                    const bool perTrack = p.accentEditAll != 0;
                    if (perTrack) for (int u = 0; u < 16; ++u) { p.accentPerTrack[u] = p.accent; p.slidePerTrack[u] = p.slide; p.swingPerTrack[u] = p.swing; }
                    else { p.accent = p.accentPerTrack[t]; p.slide = p.slidePerTrack[t]; p.swing = p.swingPerTrack[t]; }
                    p.accentEditAll = p.slideEditAll = p.swingEditAll = perTrack ? 0 : 1;
                    break;
                }
                default: break;
            }
        });
        if (r == 1) holdSteps(m_heldSet & ~(1ull << s));
        refreshGrid();
    });
}

void MdEditor::doEdit(int slot, const juce::String& label, const std::function<void(mnm::mddump::Pattern&)>& fn, int coalesce)
{
    seqOn();
    const auto now = juce::Time::currentTimeMillis();
    // (a GENERATE run, 50000 and up, is one step however long it takes)
    const bool merge = coalesce >= 0 && !m_undo.empty() && m_undo.back().slot == slot && m_lastCoalesce == coalesce && (now - m_lastEditMs < 1000 || coalesce >= 50000);
    if (!merge) {
        m_undo.push_back({slot, m_proc.bankPattern(slot), nullptr, label});
        if (m_undo.size() > 200) m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    m_lastCoalesce = coalesce;
    m_lastEditMs = now;
    m_proc.editPattern(slot, fn);
    m_undo.back().after = m_proc.bankPattern(slot);
}

void MdEditor::doEditSong(int slot, const juce::String& label, const std::function<void(mnm::mddump::Song&)>& fn, int coalesce)
{
    const auto now = juce::Time::currentTimeMillis();
    const bool merge = coalesce >= 0 && !m_undo.empty() && m_undo.back().song == slot && m_lastCoalesce == coalesce && now - m_lastEditMs < 1000;
    if (!merge) {
        UndoStep u;
        u.song = slot; u.songBefore = m_proc.bankSong(slot); u.label = label;
        m_undo.push_back(u);
        if (m_undo.size() > 200) m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    m_lastCoalesce = coalesce;
    m_lastEditMs = now;
    m_proc.editSong(slot, fn);
    m_undo.back().songAfter = m_proc.bankSong(slot);
    m_songEd.repaint();
}

void MdEditor::undo()
{
    if (m_undo.empty()) return;
    auto s = m_undo.back();
    m_undo.pop_back();
    if (s.song >= 0) { m_proc.setBankSong(s.song, s.songBefore); m_redo.push_back(s); m_lastCoalesce = -1; m_songEd.repaint(); return; }
    m_proc.setBankPattern(s.slot, s.before);
    m_redo.push_back(s);
    m_lastCoalesce = -1;
    refreshGrid();
    for (auto* pg : {&m_syn, &m_fx, &m_routing, &m_lfo, &m_out}) { pg->pull(); pg->repaint(); }
}

void MdEditor::redo()
{
    if (m_redo.empty()) return;
    auto s = m_redo.back();
    m_redo.pop_back();
    if (s.song >= 0) { m_proc.setBankSong(s.song, s.songAfter); m_undo.push_back(s); m_lastCoalesce = -1; m_songEd.repaint(); return; }
    m_proc.setBankPattern(s.slot, s.after);
    m_undo.push_back(s);
    m_lastCoalesce = -1;
    refreshGrid();
    for (auto* pg : {&m_syn, &m_fx, &m_routing, &m_lfo, &m_out}) { pg->pull(); pg->repaint(); }
}

bool MdEditor::keyPressed(const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    if ((k.getKeyCode() == juce::KeyPress::upKey || k.getKeyCode() == juce::KeyPress::downKey) && !mods.isAnyModifierKeyDown()) {
        // the previous / next kit or sound, as the strip's arrows step (the selector used last)
        if (m_drop.isVisible()) m_drop.close();
        const int dir = k.getKeyCode() == juce::KeyPress::upKey ? -1 : 1;
        if (m_browseKits) stepKit(dir); else stepSound(dir);
        return true;
    }
    if (!mods.isAnyModifierKeyDown() && (k.getKeyCode() == 'G' || k.getKeyCode() == 'g')) { toggleGrid(); return true; }   // GRID on / off
    if (!mods.isAnyModifierKeyDown() && (k.getKeyCode() == 'M' || k.getKeyCode() == 'm')) { toggleMixer(); return true; }   // the mixer
    const int page = juce::jlimit(0, 3, m_gridPage);
    if (m_gridOn && !mods.isAnyModifierKeyDown()) {   // A / S / W: accent / slide / swing marks for all tracks <-> per track
        const int code = k.getKeyCode();
        const int f = code == 'A' || code == 'a' ? 0 : code == 'S' || code == 's' ? 1 : code == 'W' || code == 'w' ? 2 : -1;
        if (f >= 0) {   // the ACCENT / SLIDE / SWING edit window (again: back to the trigs)
            static const char* const names[3] = {"ACCENT EDIT", "SLIDE EDIT", "SWING EDIT"};
            m_markMode = m_markMode == f + 1 ? 0 : f + 1;
            flash(m_markMode ? juce::String(names[f]) : juce::String("TRIGS"));
            refreshGrid();
            return true;
        }
        if (code == juce::KeyPress::escapeKey && m_markMode) { m_markMode = 0; refreshGrid(); return true; }
        if (code == juce::KeyPress::escapeKey && m_heldSet) { holdStep(-1); return true; }   // let the held steps go
        if (code == 'E' || code == 'e') {   // GENERATE: the keys are hit counts
            m_markMode = m_markMode == 7 ? 0 : 7;
            if (m_markMode) genBegin();
            flash(m_markMode ? juce::String("GENERATE") : juce::String("TRIGS"));
            return true;
        }
        if (code == 'L' || code == 'l') {   // LANE: one parameter's locks across the page
            m_markMode = m_markMode == 8 ? 0 : 8;
            const auto p = m_proc.bankPattern(editSlot());
            flash(m_markMode ? laneName() + " " + juce::String(p ? int(p->numLockedRows) : 0) + "/64" : juce::String("TRIGS"));   // the parameter, the lock rows in use
            return true;
        }
        if ((code == 'H' || code == 'h') && m_markMode == 5) { humanise(false); return true; }
        const int x = !m_proc.extrasOn() ? -1 : code == 'C' || code == 'c' ? 0 : code == 'T' || code == 't' ? 1 : code == 'R' || code == 'r' ? 2 : -1;
        if (x >= 0) {   // EXTRAS: the CONDITION / MICRO-TIMING / RETRIG edit window (again: back to the trigs)
            static const char* const names[3] = {"COND", "MICRO", "RETRIG"};
            m_markMode = m_markMode == x + 4 ? 0 : x + 4;
            flash(m_markMode ? juce::String(names[x]) : juce::String("TRIGS"));
            refreshGrid();
            return true;
        }
    }
    if (m_proc.extrasOn() && (k.getKeyCode() == 'F' || k.getKeyCode() == 'f') && !mods.isCtrlDown() && !mods.isCommandDown() && !mods.isAltDown()) {
        if (mods.isShiftDown()) { m_fillLatch = !m_fillLatch; flash(m_fillLatch ? "FILL ON" : "FILL OFF"); }   // latched
        m_proc.setFill(m_fillLatch || !mods.isShiftDown());   // held: the timer lets it go with the key
        refreshGrid();
        return true;
    }
    if (!mods.isCtrlDown() && !mods.isCommandDown() && !mods.isAltDown() && (k.getKeyCode() == 'U' || k.getKeyCode() == 'u')) { mutate(mods.isShiftDown()); return true; }
    if (m_gridOn && mods.isShiftDown() && !mods.isCtrlDown() && !mods.isCommandDown() && !mods.isAltDown()) {   // all tracks <-> this track
        const int code = k.getKeyCode();
        if ((code == 'H' || code == 'h') && m_markMode == 5) { humanise(true); return true; }
        const int f = code == 'A' || code == 'a' ? 0 : code == 'S' || code == 's' ? 1 : code == 'W' || code == 'w' ? 2 : -1;
        if (f >= 0) { togglePerTrack(f); return true; }
    }
    if (m_gridOn && (k.getKeyCode() == juce::KeyPress::leftKey || k.getKeyCode() == juce::KeyPress::rightKey)) {
        const int dir = k.getKeyCode() == juce::KeyPress::leftKey ? -1 : 1;
        if (mods.isShiftDown()) { shiftTrack(dir); return true; }   // the MD's FUNCTION + LEFT / RIGHT
        const auto p = m_proc.bankPattern(editSlot());
        const int pages = p ? (juce::jlimit(1, 64, int(p->length)) + 15) / 16 : 1;
        m_gridPage = juce::jlimit(0, pages - 1, m_gridPage + dir);
        m_pagePinned = m_proc.seqPlaying();
        refreshGrid();
        return true;
    }
    if (m_heldStep >= 0 && (k.getKeyCode() == juce::KeyPress::deleteKey || k.getKeyCode() == juce::KeyPress::backspaceKey)) {   // the held step's locks
        const int t = m_track;
        doEdit(editSlot(), "clear note locks", [&](mnm::mddump::Pattern& x) { for (int s = 0; s < 64; ++s) if ((m_heldSet >> s) & 1) x.clearStepLocks(t, s); });
        flash("LOCKS CLEARED");
        for (auto* pg : {&m_syn, &m_fx, &m_routing, &m_lfo}) pg->pull();
        refreshGrid();
        return true;
    }
    if ((mods.isCommandDown() || mods.isCtrlDown()) && mods.isShiftDown()) {   // the selected track's machine
        if (k.getKeyCode() == 'C' || k.getKeyCode() == 'c') { copyMachine(); return true; }
        if (k.getKeyCode() == 'V' || k.getKeyCode() == 'v') { pasteMachine(); return true; }
        if (k.getKeyCode() == juce::KeyPress::deleteKey || k.getKeyCode() == juce::KeyPress::backspaceKey) { clearMachine(); return true; }
    }
    if ((mods.isCommandDown() || mods.isCtrlDown()) && mods.isAltDown() && (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z')) { undoKit(); return true; }
    if ((mods.isCommandDown() || mods.isCtrlDown()) && !mods.isShiftDown() && (k.getKeyCode() == 'R' || k.getKeyCode() == 'r')) { reloadKit(); return true; }
    if (m_heldStep >= 0 && (mods.isCommandDown() || mods.isCtrlDown()) && (k.getKeyCode() == 'C' || k.getKeyCode() == 'c')) { copyNote(m_heldStep); return true; }
    if (m_heldStep >= 0 && (mods.isCommandDown() || mods.isCtrlDown()) && (k.getKeyCode() == 'V' || k.getKeyCode() == 'v')) { pasteNote(m_heldStep); return true; }
    if (m_heldStep >= 0 && (mods.isCommandDown() || mods.isCtrlDown()) && (k.getKeyCode() == 'X' || k.getKeyCode() == 'x')) { cutNotes(); return true; }
    if (m_heldStep >= 0 && (mods.isCommandDown() || mods.isCtrlDown()) && (k.getKeyCode() == 'D' || k.getKeyCode() == 'd')) { duplicateNotes(); return true; }
    if ((k.getKeyCode() == juce::KeyPress::deleteKey || k.getKeyCode() == juce::KeyPress::backspaceKey) && m_gridOn) {
        if (m_markMode >= 1 && m_markMode != 7) clearLayer(page); else pageOp(2, page);   // in an edit window: its layer only
        return true;
    }
    if (!mods.isCommandDown() && !mods.isCtrlDown()) return false;
    const int code = k.getKeyCode();
    if (code == 'C' || code == 'c') { pageOp(0, page); return true; }
    if (code == 'V' || code == 'v') { pageOp(1, page); return true; }
    if (code == 'D' || code == 'd') { doublePattern(); return true; }
    if ((code == 'Z' || code == 'z') && mods.isShiftDown()) { redo(); return true; }
    if (code == 'Z' || code == 'z') { undo(); return true; }
    if (code == 'Y' || code == 'y') { redo(); return true; }
    return false;
}

void MdEditor::genBegin()
{
    ++m_gen.session;
    m_gen.slot = editSlot(); m_gen.track = m_track; m_gen.mode = 0; m_gen.n = 0; m_gen.rot = 0;
    const auto p = m_proc.bankPattern(m_gen.slot);
    m_gen.base = p ? *p : mnm::mddump::Pattern();
}

void MdEditor::genApply(const juce::String& say)
{
    if (m_gen.slot != editSlot() || m_gen.track != m_track) genBegin();
    const Gen& g = m_gen;
    const int t = m_track;
    doEdit(g.slot, "generate", [&](mnm::mddump::Pattern& x) {
        const int len = juce::jlimit(1, 64, int(x.length)), pages = (len + 15) / 16;
        const int hits = juce::jmin(len, g.n * pages);
        uint64_t m = 0;
        if (g.mode == 1) { for (int i = 0; i < len; ++i) if ((i * hits) % len < hits) m |= 1ull << i; }   // spread evenly, the first on step 1
        else if (g.mode == 2) { juce::Random r(g.seed); for (int left = hits; left > 0;) { const int s = r.nextInt(len); if (!((m >> s) & 1)) { m |= 1ull << s; --left; } } }
        else if (g.mode == 3) for (int i = 0; i < len; i += juce::jmax(1, g.n)) m |= 1ull << i;
        x.clearSteps(0, len, t);
        const int rot = ((g.rot % len) + len) % len;
        for (int s = 0; s < len; ++s) {
            const int d = (s + rot) % len;
            if (g.mode == 0) { if ((g.base.trigs[t] >> s) & 1) x.copySteps(g.base, s, d, 1, t, t); continue; }   // only turned round
            if (!((m >> s) & 1)) continue;
            if ((g.base.trigs[t] >> d) & 1) x.copySteps(g.base, d, d, 1, t, t);   // a step that was there keeps its locks
            else x.trigs[t] |= 1ull << d;
        }
    }, 50000 + (g.session & 0xFFF));
    holdStep(-1);
    flash(say);
}

juce::String MdEditor::laneName() const
{
    if (auto* prm = m_proc.apvts.getParameter(trackParamId(m_track, m_laneParam))) return prm->getName(32).fromFirstOccurrenceOf(" ", false, false);
    return juce::String(m_laneParam);
}

int MdEditor::laneKitValue() const
{
    if (auto* a = m_proc.apvts.getParameter(trackParamId(m_track, m_laneParam))) return juce::jlimit(0, 127, int(std::lround(a->convertFrom0to1(a->getValue()))));
    return 0;
}

void MdEditor::clearLayer(int page)
{
    static const char* const names[9] = {"", "ACC", "SLD", "SWG", "COND", "TIME", "RTRG", "", "LANE"};
    const int t = m_track, mode = m_markMode, lane = m_laneParam;
    doEdit(editSlot(), "clear layer", [&](mnm::mddump::Pattern& x) {
        for (int s = page * 16; s < page * 16 + 16 && s < 64; ++s) {
            const uint64_t bit = 1ull << s;
            switch (mode) {
                case 1: (x.accentEditAll ? x.accent : x.accentPerTrack[t]) &= ~bit; break;
                case 2: (x.slideEditAll ? x.slide : x.slidePerTrack[t]) &= ~bit; break;
                case 3: (x.swingEditAll ? x.swing : x.swingPerTrack[t]) &= ~bit; break;
                case 4: x.cond[t][s] = 0; break;
                case 5: x.micro[t][s] = 0; break;
                case 6: x.retrig[t][s] = 0; break;
                case 8: x.clearLock(t, lane, s); break;
                default: break;
            }
        }
    });
    flash(juce::String(names[juce::jlimit(0, 8, mode)]) + " CLR");
    for (auto* pg : {&m_syn, &m_fx, &m_routing, &m_lfo}) pg->pull();
}

void MdEditor::humanise(bool more)
{
    if (more) { static const int steps[4] = {2, 4, 8, 12}; int i = 0; while (i < 4 && steps[i] <= m_humanise) ++i; m_humanise = steps[i % 4]; }
    const int t = m_track, amt = m_humanise;
    auto& r = juce::Random::getSystemRandom();
    doEdit(editSlot(), "humanise", [&](mnm::mddump::Pattern& x) {
        for (int s = 0; s < juce::jlimit(1, 64, int(x.length)); ++s)
            if ((x.trigs[t] >> s) & 1) x.micro[t][s] = int8_t(r.nextInt(2 * amt + 1) - amt);
    }, 46000 + t);
    flash("HUM +-" + juce::String(amt));
}

void MdEditor::mutate(bool withFx)
{
    const int t = m_track, n = withFx ? 16 : 8;
    auto id = [t](int i) { return i < 8 ? knobId(t, i) : fxId(t, i - 8); };
    std::array<int, 16> now{};
    for (int i = 0; i < 16; ++i)
        if (auto* a = m_proc.apvts.getParameter(id(i))) now[size_t(i)] = int(std::lround(a->convertFrom0to1(a->getValue())));
    if (m_mut.track != t || m_mut.fx != withFx || now != m_mut.last) {   // a new start (anything else: another roll from the same one)
        m_mut.track = t; m_mut.fx = withFx; m_mut.base = now;
        m_kitUndo = {true, m_proc.captureMdKit(), m_proc.loadedKitKey(), m_proc.kitName()};
    }
    auto& r = juce::Random::getSystemRandom();
    m_mut.last = now;
    for (int i = 0; i < n; ++i) {
        auto* a = m_proc.apvts.getParameter(id(i));
        if (!a) continue;
        const int base = m_mut.base[size_t(i)];
        const int v = juce::jlimit(0, 127, base + int(std::lround((r.nextInt(128) - base) * 0.2)));
        m_mut.last[size_t(i)] = v;
        if (v == now[size_t(i)]) continue;
        a->beginChangeGesture(); a->setValueNotifyingHost(a->convertTo0to1(float(v))); a->endChangeGesture();
    }
    flash(withFx ? "MUT S+FX" : "MUTATE");
    for (auto* pg : {&m_syn, &m_fx}) { pg->pull(); pg->repaint(); }
}

// DEL PAGE: the page goes, the pages after it move up and the pattern is 16 steps shorter (one page: its steps cleared)
void MdEditor::deletePage(int page)
{
    const int slot = editSlot();
    const auto cur = m_proc.bankPattern(slot);
    if (!cur) { flash("EMPTY"); return; }
    const int pages = (juce::jlimit(1, 64, int(cur->length)) + 15) / 16;
    if (page >= pages) return;
    doEdit(slot, "delete page", [&](mnm::mddump::Pattern& x) {
        if (pages == 1) { x.clearSteps(0, 16, -1); x.swing = cur->swing; return; }
        const auto src = x;
        for (int k = page; k < pages - 1; ++k) x.copySteps(src, (k + 1) * 16, k * 16, 16, -1, -1);
        x.clearSteps((pages - 1) * 16, 16, -1);
        x.length = uint8_t(juce::jmax(16, int(x.length) - 16));
        x.scale = uint8_t((x.length - 1) / 16);
    });
    m_gridPage = juce::jmin(page, pages - 2 < 0 ? 0 : pages - 2);
    flash(pages == 1 ? "CLEARED P1" : "DEL P" + juce::String(page + 1));
    refreshGrid();
}

void MdEditor::shiftTrack(int dir)
{
    const int slot = editSlot(), t = m_track;
    const auto cur = m_proc.bankPattern(slot);
    if (!cur) { flash("EMPTY"); return; }
    const int len = juce::jlimit(1, 64, int(cur->length));
    doEdit(slot, dir > 0 ? "shift track right" : "shift track left", [&](mnm::mddump::Pattern& x) {
        const auto src = x;
        for (int s = 0; s < len; ++s) x.copySteps(src, s, (s + dir + len) % len, 1, t, t);
    }, 30000 + t);
    flash(dir > 0 ? "T" + juce::String(t + 1) + " >>" : "<< T" + juce::String(t + 1));
    refreshGrid();
}

namespace {
int lowest(uint64_t m) { int s = 0; while (s < 64 && !((m >> s) & 1)) ++s; return s; }
int highest(uint64_t m) { int s = 63; while (s >= 0 && !((m >> s) & 1)) --s; return s; }
int countOf(uint64_t m) { int n = 0; for (int s = 0; s < 64; ++s) n += int((m >> s) & 1); return n; }
}

// The held steps that have a trig (step: one of them, or a single step) go to the clipboard, with where they sit
void MdEditor::copyNote(int step)
{
    const auto p = m_proc.bankPattern(editSlot());
    const uint64_t mask = p ? heldTargets(step) & p->trigs[m_track] : 0;
    if (mask == 0) { flash("NO NOTE"); return; }
    m_clip = {4, false, m_track, lowest(mask), *p, mask};
    flash(countOf(mask) > 1 ? "COPY " + juce::String(countOf(mask)) : "COPY NOTE " + juce::String(lowest(mask) + 1));
}

// The copied notes from `step` on, as they were spaced; the ones past the pattern's end are left out (and counted)
void MdEditor::pasteNote(int step)
{
    if (m_clip.kind != 4) { flash("NO NOTE"); return; }
    const auto clip = m_clip;
    const int t = m_track;
    int placed = 0, off = 0;
    doEdit(editSlot(), "paste note", [&](mnm::mddump::Pattern& x) {
        const int len = juce::jlimit(1, 64, int(x.length));
        for (int s = 0; s < 64; ++s) {
            if (!((clip.mask >> s) & 1)) continue;
            const int d = step + s - clip.page;
            if (d < 0 || d >= len) { ++off; continue; }
            x.copySteps(clip.pat, s, d, 1, clip.track, t);
            ++placed;
        }
    });
    flash(off ? "PST " + juce::String(placed) + " -" + juce::String(off) : placed > 1 ? "PASTE " + juce::String(placed) : "PASTE NOTE " + juce::String(step + 1));   // (-n: past the end)
    for (auto* pg : {&m_syn, &m_fx, &m_routing, &m_lfo}) pg->pull();
    refreshGrid();
}

void MdEditor::cutNotes()
{
    const int first = m_heldStep;
    copyNote(first);
    if (m_clip.kind != 4) return;
    const uint64_t mask = m_clip.mask;
    const int t = m_track;
    doEdit(editSlot(), "cut notes", [&](mnm::mddump::Pattern& x) { for (int s = 0; s < 64; ++s) if ((mask >> s) & 1) x.clearSteps(s, 1, t); });
    holdStep(-1);
    flash("CUT " + juce::String(countOf(mask)));
}

// The held steps again right after themselves, then those held: Ctrl + D over and over walks a figure along the page
void MdEditor::duplicateNotes()
{
    const auto p = m_proc.bankPattern(editSlot());
    const uint64_t held = m_heldSet;
    if (!p || held == 0) return;
    const int lo = lowest(held), hi = highest(held), span = hi - lo + 1, len = juce::jlimit(1, 64, int(p->length)), t = m_track;
    if (hi + 1 >= len) { flash("NO ROOM"); return; }
    const auto src = *p;
    uint64_t made = 0;
    doEdit(editSlot(), "duplicate notes", [&](mnm::mddump::Pattern& x) {
        for (int s = lo; s <= hi; ++s) {
            const int d = s + span;
            if (d >= len) continue;
            if ((held >> s) & 1) { x.copySteps(src, s, d, 1, t, t); made |= 1ull << d; }
            else x.clearSteps(d, 1, t);   // the gaps of the figure too
        }
    });
    holdSteps(made);
    flash("DUP");
    for (auto* pg : {&m_syn, &m_fx, &m_routing, &m_lfo}) pg->pull();
}

void MdEditor::applyMuteQueue()
{
    for (int t = 0; t < kTracks; ++t) if ((m_muteQueue >> t) & 1) toggleMute(t);
    m_muteQueue = 0;
    m_keys.setMuteQueue(0);
}

void MdEditor::undoKit()
{
    if (!m_kitUndo.valid) { flash("NO KIT UNDO"); return; }
    const KitUndo back = m_kitUndo;
    m_kitUndo = {true, m_proc.captureMdKit(), m_proc.loadedKitKey(), m_proc.kitName()};   // undo again = redo
    m_proc.loadMdKit(back.key, back.kit, back.name);
    flash("KIT UNDONE");
    selectTrack(m_track);
    timerCallback();
}

void MdEditor::reloadKit()
{
    const auto key = m_proc.loadedKitKey();
    mnm::mddump::Kit kit;
    if (key.isEmpty() || !m_lib->loadKit(key, kit)) { flash("NO SAVED KIT"); return; }
    m_kitUndo = {true, m_proc.captureMdKit(), key, m_proc.kitName()};
    m_proc.loadMdKit(key, kit, m_proc.kitName());
    flash("KIT RELOADED");
    selectTrack(m_track);
    timerCallback();
}

void MdEditor::copyMachine()
{
    m_soundClip = m_proc.captureSound(m_track);
    m_soundClipName = m_proc.loadedSoundName(m_track).isNotEmpty() ? m_proc.loadedSoundName(m_track) : "T" + juce::String(m_track + 1) + " COPY";
    flash("COPY T" + juce::String(m_track + 1));
}

void MdEditor::pasteMachine()
{
    if (!m_soundClip) { flash("NOTHING COPIED"); return; }
    if (!m_proc.loadSound(m_track, {}, *m_soundClip, m_soundClipName)) { flash("NO SUCH MACHINE"); return; }
    flash("PASTE T" + juce::String(m_track + 1));
    bindTrackPages();
    timerCallback();
}

void MdEditor::clearMachine()
{
    if (auto* p = m_proc.apvts.getParameter(machineId(m_track))) {
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1(float(machineIndexOf(0))));   // GND---: the empty machine
        p->endChangeGesture();
    }
    flash("CLEAR T" + juce::String(m_track + 1));
    timerCallback();
}

void MdEditor::flipStepFlag(int step, int f)
{
    static const char* const names[3] = {"accent", "slide", "swing"};
    const int t = m_track;
    doEdit(editSlot(), names[f], [&](mnm::mddump::Pattern& p) {
        uint32_t all[3] = {p.accentEditAll, p.slideEditAll, p.swingEditAll};
        uint64_t* global[3] = {&p.accent, &p.slide, &p.swing};
        uint64_t* own[3] = {&p.accentPerTrack[t], &p.slidePerTrack[t], &p.swingPerTrack[t]};
        *(all[f] ? global[f] : own[f]) ^= 1ull << step;
    });
    refreshGrid();
}

void MdEditor::togglePerTrack(int f)
{
    static const char* const names[3] = {"ACCENT", "SLIDE", "SWING"};
    bool nowAll = true;
    const int t = m_track;
    doEdit(editSlot(), "per-track marks", [&](mnm::mddump::Pattern& p) {
        uint32_t* all[3] = {&p.accentEditAll, &p.slideEditAll, &p.swingEditAll};
        uint64_t* global[3] = {&p.accent, &p.slide, &p.swing};
        uint64_t* own[3] = {p.accentPerTrack, p.slidePerTrack, p.swingPerTrack};
        if (*all[f]) { for (int u = 0; u < 16; ++u) own[f][u] = *global[f]; *all[f] = 0; nowAll = false; }   // the marks carry over
        else { *global[f] = own[f][t]; *all[f] = 1; nowAll = true; }
    });
    flash(juce::String(names[f]) + (nowAll ? ": ALL" : ": T" + juce::String(t + 1)));
    refreshGrid();
}

void MdEditor::toggleMute(int t)
{
    if (auto* p = m_proc.apvts.getParameter(muteId(t))) {
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->getValue() >= 0.5f ? 0.0f : 1.0f);
        p->endChangeGesture();
    }
    refreshGrid();
}

void MdEditor::pageOp(int op, int page)
{
    const int slot = editSlot();
    const auto p = m_proc.bankPattern(slot);
    const juce::String pg = "P" + juce::String(page + 1);
    if (op == 0) {
        if (!p) { flash("EMPTY"); return; }
        m_clip = {1, true, m_track, page, *p};
        flash("COPY " + pg);
        return;
    }
    if (op == 1 && m_clip.kind != 1) { flash("NO PAGE"); return; }
    const auto clip = m_clip;
    doEdit(slot, op == 1 ? "paste page" : "clear page", [&](mnm::mddump::Pattern& x) {
        if (op == 1) {
            x.copySteps(clip.pat, clip.page * 16, page * 16, 16, clip.all ? -1 : clip.track, m_track);
            if (x.length < (page + 1) * 16) { x.length = uint8_t((page + 1) * 16); x.scale = uint8_t(page); }
        } else {
            x.clearSteps(page * 16, 16, -1);
        }
    });
    if (op == 1) m_gridPage = page;
    holdStep(-1);
    flash(op == 1 ? "PASTE " + pg : "CLEAR " + pg);
}

void MdEditor::trackOp(int op)
{
    const int slot = editSlot(), t = m_track;
    const auto p = m_proc.bankPattern(slot);
    const juce::String tr = "T" + juce::String(t + 1);
    if (op == 0) {
        if (!p) { flash("EMPTY"); return; }
        m_clip = {2, false, t, 0, *p};
        flash("COPY " + tr);
        return;
    }
    if (op == 1 && m_clip.kind != 2) { flash("NO TRACK"); return; }
    const auto clip = m_clip;
    doEdit(slot, op == 1 ? "paste track" : "clear track", [&](mnm::mddump::Pattern& x) {
        if (op == 1) x.copySteps(clip.pat, 0, 0, 64, clip.track, t);
        else x.clearSteps(0, 64, t);
    });
    holdStep(-1);
    flash(op == 1 ? "PASTE " + tr : "CLEAR " + tr);
}

void MdEditor::patternOp(int op)
{
    const int slot = editSlot();
    const auto p = m_proc.bankPattern(slot);
    const juce::String pn = kPatternNames[slot];
    if (op == 0) {
        if (!p) { flash("EMPTY"); return; }
        m_clip = {3, true, 0, 0, *p};
        flash("COPY " + pn);
        return;
    }
    if (op == 1 && m_clip.kind != 3) { flash("NO PTN"); return; }
    const auto clip = m_clip;
    doEdit(slot, op == 1 ? "paste pattern" : "clear pattern", [&](mnm::mddump::Pattern& x) {
        if (op == 1) { const int pos = x.position; x = clip.pat; x.position = pos; }
        else x.clearSteps(0, 64, -1);
    });
    holdStep(-1);
    flash(op == 1 ? "PASTE " + pn : "CLEAR " + pn);
}

void MdEditor::doublePattern()
{
    const int slot = editSlot();
    const auto p = m_proc.bankPattern(slot);
    const int len = p ? juce::jlimit(1, 64, int(p->length)) : 16;
    if (!p) { flash("EMPTY"); return; }
    if (len > 32) { flash("64 = FULL"); return; }
    doEdit(slot, "double", [&](mnm::mddump::Pattern& x) {
        const auto copy = x;
        x.copySteps(copy, 0, len, len, -1, 0);
        x.length = uint8_t(juce::jmin(64, 2 * len));
        x.scale = uint8_t((x.length - 1) / 16);
    });
    flash("X2 = " + juce::String(2 * len));
}

void MdEditor::editMenu()
{
    const int slot = editSlot(), t = m_track;
    const auto p = m_proc.bankPattern(slot);
    const int len = p ? juce::jlimit(1, 64, int(p->length)) : 16;
    const int page = juce::jlimit(0, 3, m_gridPage);
    const juce::String pg = "page " + juce::String(page + 1), tr = "T" + juce::String(t + 1), pn = kPatternNames[slot];
    juce::String what;
    if (m_clip.kind == 1) what = " (page " + juce::String(m_clip.page + 1) + (m_clip.all ? ", all tracks)" : ", T" + juce::String(m_clip.track + 1) + ")");
    juce::PopupMenu m;
    m.addItem(20, m_undo.empty() ? juce::String("Undo") : "Undo " + m_undo.back().label + "  (Ctrl+Z)", !m_undo.empty());
    m.addItem(21, m_redo.empty() ? juce::String("Redo") : "Redo " + m_redo.back().label + "  (Ctrl+Shift+Z)", !m_redo.empty());
    m.addSeparator();
    m.addSectionHeader(pn + "  -  " + pg.toUpperCase() + "  -  " + tr);
    m.addItem(1, "Copy " + pg + " (all tracks)", p != nullptr);
    m.addItem(2, "Copy " + pg + " of " + tr, p != nullptr);
    m.addItem(3, "Paste onto " + pg + what, m_clip.kind == 1);
    m.addItem(4, "Clear " + pg + " (all tracks)", p != nullptr);
    m.addItem(5, "Clear " + pg + " of " + tr, p != nullptr);
    m.addSeparator();
    m.addItem(6, "Copy track " + tr, p != nullptr);
    m.addItem(7, "Paste track onto " + tr + (m_clip.kind == 2 ? " (T" + juce::String(m_clip.track + 1) + ")" : juce::String()), m_clip.kind == 2);
    m.addItem(8, "Clear track " + tr, p != nullptr);
    m.addSeparator();
    m.addItem(9, "Copy pattern " + pn, p != nullptr);
    m.addItem(10, "Paste pattern onto " + pn, m_clip.kind == 3);
    m.addItem(11, "Clear pattern " + pn + " (its steps; the settings stay)", p != nullptr);
    m.addSeparator();
    {
        const int sl = juce::jlimit(0, 31, int(std::lround(m_proc.apvts.getRawParameterValue(songId())->load())));
        m.addItem(31, "Edit song " + juce::String(sl + 1).paddedLeft('0', 2) + " (the SONG knob's)...");
    }
    m.addItem(30, m_proc.bankProjectId().isNotEmpty() ? "Save patterns and songs to the library (" + m_proc.bankName() + ": a new version)"
                                                      : juce::String("Save patterns and songs to the library (a new project)"),
              m_proc.bankHasPattern(slot) || m_proc.bankName().isNotEmpty());
    m.addSeparator();
    m.addItem(12, "Double: steps 1-" + juce::String(len) + " again after themselves (length " + juce::String(juce::jmin(64, 2 * len)) + ")", p != nullptr && len <= 32);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&m_seqBar), [this, slot, t, page, p, len](int r) {
        if (r <= 0) return;
        if (r == 20) { undo(); return; }
        if (r == 30) { saveBankToLibrary(); return; }
        if (r == 31) {
            m_songEd.setBounds(m_syn.getX(), m_syn.getY(), m_routing.getRight() - m_syn.getX(), m_out.getBottom() - m_syn.getY());
            m_songEd.open(juce::jlimit(0, 31, int(std::lround(m_proc.apvts.getRawParameterValue(songId())->load()))));
            m_songEd.grabKeyboardFocus();
            return;
        }
        if (r == 21) { redo(); return; }
        if (r == 1 || r == 2) { m_clip = {1, r == 1, t, page, *p}; return; }
        if (r == 6) { m_clip = {2, false, t, 0, *p}; return; }
        if (r == 9) { m_clip = {3, true, 0, 0, *p}; return; }
        const auto clip = m_clip;
        static const char* const names[13] = {"", "", "", "paste page", "clear page", "clear page", "", "paste track", "clear track", "", "paste pattern", "clear pattern", "double"};
        doEdit(slot, names[juce::jlimit(0, 12, r)], [&](mnm::mddump::Pattern& x) {
            switch (r) {
                case 3:   // the copied page onto this one (the pattern grows to reach it)
                    x.copySteps(clip.pat, clip.page * 16, page * 16, 16, clip.all ? -1 : clip.track, t);
                    if (x.length < (page + 1) * 16) { x.length = uint8_t((page + 1) * 16); x.scale = uint8_t(page); }
                    break;
                case 4: x.clearSteps(page * 16, 16, -1); break;
                case 5: x.clearSteps(page * 16, 16, t); break;
                case 7: x.copySteps(clip.pat, 0, 0, 64, clip.track, t); break;
                case 8: x.clearSteps(0, 64, t); break;
                case 10: { const int pos = x.position; x = clip.pat; x.position = pos; break; }
                case 11: x.clearSteps(0, 64, -1); break;
                case 12: {
                    const auto copy = x;
                    x.copySteps(copy, 0, len, len, -1, 0);
                    x.length = uint8_t(juce::jmin(64, 2 * len));
                    x.scale = uint8_t((x.length - 1) / 16);
                    break;
                }
                default: break;
            }
        });
        if (r == 3) m_gridPage = page;
        holdStep(-1);
        refreshGrid();
    });
}

} // namespace mnm::plugin::md
