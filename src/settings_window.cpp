// The settings window: one page with every setting, and a second one for the
// hotkeys. Candela's look without its animations or its side panel:
// - Today's schedule runs across the top: a flat 24-hour band, each part in
//   the warmth it puts on screen, the switch times under it and a wedge at
//   now. Clicking a part selects its setting.
// - Two columns of rows in labelled groups. Every row shows what kind of
//   control it is: an ON | OFF pair, a block gauge with the warmth's swatch,
//   ‹ VALUE › with a pip per choice, a field for what you type, keycaps for
//   hotkeys, and a framed button for what runs something.
//
// Hover selects, click changes (right-click goes backwards, the wheel steps,
// a click on a gauge sets it there, ‹ and › step), and the keyboard works
// too: arrows, Enter/Space, Tab for the other page, Esc to close. Every
// change is saved straight away. Nothing moves by itself: every frame is
// drawn because something changed.
#include <algorithm>
#include <cmath>
#include <cwctype>

#include "options.h"
#include "render.h"

namespace {

using namespace gfx;

// Unscaled layout (px at 96 dpi).
constexpr float kW = 960, kH = 704, kMargin = 24;      // the window, and the shadow's room around it
constexpr float kPad = 36;
constexpr float kStripY = 76, kStripH = 24;            // today's band; its times sit under it
constexpr float kTabY = 132, kTabH = 30;
constexpr float kBodyY = 180;
constexpr float kColX[2][2] = {{60, 464}, {520, 924}};  // each column's rows; the selection wedge sits left of them
constexpr float kRowH = 30, kRowGap = 3, kGroupH = 28, kGroupGap = 12;
constexpr float kFootY = 654;

std::wstring Upper(std::wstring s) {
    CharUpperBuffW(s.data(), (DWORD)s.size());
    return s;
}

D2D1_COLOR_F Mix(const Color& a, const Color& b, float p, float alpha = 1.f) {
    return D2D1::ColorF(a.r + (b.r - a.r) * p, a.g + (b.g - a.g) * p, a.b + (b.b - a.b) * p, alpha);
}

enum Act { kTab, kRow, kDec, kInc, kGauge, kClose, kStrip };
struct Hit {
    D2D1_RECT_F r;  // unscaled window coordinates
    Act act;
    int a;
    std::wstring key;  // kStrip: the setting it selects
};

// A row on the current page: which option, and where.
struct Row {
    int group, opt;
    float x0, y0, x1, y1;  // unscaled
};

}  // namespace

// ---- where windows go ---------------------------------------------------------------------------------
namespace ui {

struct Placement {
    int x, y, w, h;
    float s;
};

// Where a window of w x h (unscaled, plus `margin` of shadow around it) goes:
// centred on the screen with the pointer, scaled for that screen's DPI and
// shrunk if the screen is too small.
Placement Place(float w, float h, float margin) {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = MonitorInfoOf(mon);
    const RECT& r = mi.rcWork;
    float s = DpiScaleForMonitor(mon);
    // Shrink to fit a small screen (a laptop at 150%, say).
    float fit = std::min((r.right - r.left) / (w + 2 * margin), (r.bottom - r.top) / (h + 2 * margin));
    s = std::max(0.5f, std::min(s, fit));
    int pw = (int)(std::round(w * s) + 2 * std::round(margin * s)), ph = (int)(std::round(h * s) + 2 * std::round(margin * s));
    return {(r.left + r.right - pw) / 2, (r.top + r.bottom - ph) / 2, pw, ph, s};
}

// Gives a window the Candela icon (taskbar, Alt+Tab).
void SetIcons(HWND h) {
    HICON big = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
    HICON small = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                    GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)small);
}

// ---- the window ---------------------------------------------------------------------------------------
class SettingsWindow {
public:
    void Open() {
        if (!EnsureWindow()) return;
        Rebuild();
        if (open_) {
            if (IsIconic(win_.hwnd)) ShowWindow(win_.hwnd, SW_RESTORE);
            SetForegroundWindow(win_.hwnd);
            Render();
            return;
        }
        open_ = true;
        strip_tz_++;  // the clock or time zone may have changed while it was closed
        Placement p = Place(kW, kH, kMargin);
        s_ = p.s;
        win_.Resize(p.w, p.h);
        SetWindowPos(win_.hwnd, nullptr, p.x, p.y, p.w, p.h, SWP_NOZORDER | SWP_NOACTIVATE);
        SetTimer(win_.hwnd, 1, 1000, nullptr);  // the clock, the status and notices that expire
        Render();
        SetForegroundWindow(win_.hwnd);
    }

    // A setting ("lat") to start on, before Open.
    void Select(const std::wstring& key) {
        pages_ = opts::Build();
        for (int p = 0; p < (int)pages_.size(); p++)
            for (auto& g : pages_[p].groups)
                for (auto& o : g.opts)
                    if (o.key == key) {
                        page_ = p;
                        select_key_ = key;
                    }
    }

    void Close() {
        if (!open_) return;
        opts::EndEdit(EditingOption(), true);
        opts::StopRecording();
        open_ = false;
        KillTimer(win_.hwnd, 1);
        win_.Hide();
    }

    bool Owns(HWND h) const { return h && h == win_.hwnd; }

    void Refresh() {
        if (!open_) return;
        strip_tz_++;  // the band is worked out again too (a clock change lands here)
        Rebuild();
        Render();
    }

    void Shutdown() { win_.Destroy(); }

    // Screenshots for checking the look without a desktop (--dump).
    void DebugDump(const std::wstring& dir) {
        if (!EnsureWindow()) return;
        Open();
        auto shot = [&](const wchar_t* name) {
            Render();
            win_.SavePng(dir + L"\\" + name + L".png");
        };
        auto select = [&](const std::wstring& key) {
            for (int i = 0; i < (int)rows_.size(); i++)
                if (Opt(i).key == key) sel_ = i;
        };
        sel_ = 0;
        shot(L"settings_main");
        select(L"evening");
        shot(L"settings_main_selected");
        select(L"lat");
        opts::BeginEdit(Opt(sel_));
        opts::g_edit.buffer = L"40.41";
        shot(L"settings_main_typing");
        opts::g_edit.key.clear();
        select(L"drbright");
        opts::Notice(Tr(L"Latitude must be between -90 and 90.", L"警告：緯度は−90〜90の範囲で入力。"));
        shot(L"settings_main_notice");
        opts::Notice(L"");
        g_fake_now += 10000;
        SwitchPage(1);
        shot(L"settings_hotkeys");
        select(L"hk" + std::to_wstring(HK_BLACKOUT));
        opts::g_recording = Opt(sel_).key;
        shot(L"settings_hotkeys_recording");
        opts::g_recording.clear();
        Close();
    }

private:
    bool EnsureWindow() {
        if (win_.hwnd) return true;
        if (!win_.Create(L"CandelaMiniSettings", Proc, this, 0, nullptr, false)) return false;
        SetWindowTextW(win_.hwnd, CANDELA_NAME);
        SetIcons(win_.hwnd);
        return true;
    }

    // ---- the model -----------------------------------------------------------------------------------------
    void Rebuild() {
        std::wstring keep = sel_ >= 0 && sel_ < (int)rows_.size() ? Opt(sel_).key : L"";
        if (!select_key_.empty()) keep = select_key_;
        select_key_.clear();
        pages_ = opts::Build();
        page_ = std::clamp(page_, 0, (int)pages_.size() - 1);
        // Where each row goes: the groups of each column one under the other.
        rows_.clear();
        group_y_.assign(Page().groups.size(), 0);
        float y[2] = {kBodyY, kBodyY};
        for (int g = 0; g < (int)Page().groups.size(); g++) {
            const auto& grp = Page().groups[g];
            int c = std::clamp(grp.column, 0, 1);
            if (y[c] > kBodyY) y[c] += kGroupGap;
            group_y_[g] = y[c];
            y[c] += kGroupH;
            for (int o = 0; o < (int)grp.opts.size(); o++) {
                rows_.push_back({g, o, kColX[c][0], y[c], kColX[c][1], y[c] + kRowH});
                y[c] += kRowH + kRowGap;
            }
            y[c] -= kRowGap;
        }
        column_end_[0] = y[0];
        column_end_[1] = y[1];
        // Keyboard order: down the left column, then the right.
        std::stable_sort(rows_.begin(), rows_.end(), [](const Row& a, const Row& b) { return a.x0 < b.x0; });
        sel_ = std::min(sel_, (int)rows_.size() - 1);
        for (int i = 0; i < (int)rows_.size(); i++)
            if (Opt(i).key == keep) sel_ = i;
    }

    const opts::Page& Page() const { return pages_[page_]; }
    const opts::Option& Opt(int row) const {
        const Row& r = rows_[row];
        return Page().groups[r.group].opts[r.opt];
    }
    const opts::Option* Find(const std::wstring& key) const {
        for (auto& p : pages_)
            for (auto& g : p.groups)
                for (auto& o : g.opts)
                    if (o.key == key) return &o;
        return nullptr;
    }
    const opts::Option* EditingOption() const { return opts::g_edit.Active() ? Find(opts::g_edit.key) : nullptr; }
    const opts::Option* RecordingOption() const { return opts::g_recording.empty() ? nullptr : Find(opts::g_recording); }

    float P(float v) const { return std::round(v * s_); }
    float Px() const { return std::max(1.f, P(1)); }

    // ---- drawing helpers ---------------------------------------------------------------------------------
    void Hitbox(float x0, float y0, float x1, float y1, Act act, int a, const std::wstring& key = L"") {
        hits_.push_back({D2D1::RectF(x0, y0, x1, y1), act, a, key});
    }
    // The same from scaled px (what the drawing code works in).
    void HitPx(float x0, float y0, float x1, float y1, Act act, int a) { Hitbox(x0 / s_, y0 / s_, x1 / s_, y1 / s_, act, a); }

    void Wedge(ID2D1RenderTarget* rt, float cx, float cy, float w, float h, bool down, D2D1_COLOR_F c) {
        rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        ID2D1PathGeometry* geo = nullptr;
        if (FAILED(d2d->CreatePathGeometry(&geo))) return;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(geo->Open(&sink))) {
            if (down) {
                sink->BeginFigure(D2D1::Point2F(cx - w / 2, cy - h / 2), D2D1_FIGURE_BEGIN_FILLED);
                D2D1_POINT_2F pts[] = {{cx + w / 2, cy - h / 2}, {cx, cy + h / 2}};
                sink->AddLines(pts, 2);
            } else {  // the cursor: pointing right, with a notch
                sink->BeginFigure(D2D1::Point2F(cx, cy - h / 2), D2D1_FIGURE_BEGIN_FILLED);
                D2D1_POINT_2F pts[] = {{cx + w, cy}, {cx, cy + h / 2}, {cx + w * 0.28f, cy}};
                sink->AddLines(pts, 3);
            }
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            sink->Release();
            if (auto* b = win_.Brush(c)) rt->FillGeometry(geo, b);
        }
        geo->Release();
        rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    }

    // A small framed box with text in it (a keycap, a button). Returns its width.
    float Box(ID2D1RenderTarget* rt, float x1, float cy, const std::wstring& text, D2D1_COLOR_F fg, bool filled, D2D1_COLOR_F fill_text,
              float min_w, float pad, float h) {
        TextStyle ts{P(10.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(1.5f), kCenter};
        float w = std::max(P(min_w), TextWidth(text, ts) + 2 * P(pad));
        float x0 = x1 - w, y0 = cy - P(h) / 2, y1 = y0 + P(h);
        if (filled) Fill(rt, win_, x0, y0, x1, y1, fg);
        else Frame(rt, win_, x0, y0, x1, y1, Px(), fg);
        Text(rt, win_, text, D2D1::RectF(x0, y0, x1, y1), ts, filled ? fill_text : fg);
        return w;
    }

    // ---- what each row shows on its right -------------------------------------------------------------------------
    // fg: the row's ink (paper on the selected row); bg: the row's own color.
    // Returns where the control starts, so the label stops short of it.
    float DrawControl(ID2D1RenderTarget* rt, int row, const opts::Option& o, float y0, float y1, float x1, D2D1_COLOR_F fg,
                      D2D1_COLOR_F bg, bool on_row) {
        const float cy = (y0 + y1) / 2, px = Px();
        const bool enabled = o.enabled();
        D2D1_COLOR_F soft = fg;
        soft.a *= 0.55f;
        TextStyle val{P(12.5f), DWRITE_FONT_WEIGHT_NORMAL, P(2), kRight};
        // ‹ VALUE ›, the arrows drawn rather than typed (not every Japanese font has them),
        // each one a click target of its own.
        auto chevron = [&](float cx, bool right) {
            float w = P(3), h = P(4), t = std::max(1.f, P(1.2f));
            float tip = right ? cx + w / 2 : cx - w / 2, back = right ? cx - w / 2 : cx + w / 2;
            Line(rt, win_, back, cy - h, tip, cy, t, fg);
            Line(rt, win_, tip, cy, back, cy + h, t, fg);
        };
        auto arrows_text = [&](const std::wstring& v, float right) {
            std::wstring s = Upper(v);
            float w = TextWidth(s, val), gap = P(14);
            chevron(right - P(4), true);
            float tx1 = right - gap;
            Text(rt, win_, s, D2D1::RectF(tx1 - w - P(4), y0, tx1, y1), val, fg);
            float lx = tx1 - w - gap + P(4);
            chevron(lx, false);
            if (enabled) {
                HitPx(lx - P(9), y0, lx + P(9), y1, kDec, row);
                HitPx(right - P(13), y0, right + P(6), y1, kInc, row);
            }
            return w + 2 * gap;
        };
        // Being typed or recorded: a filled field with the text and a cursor (it doesn't blink).
        if (opts::g_edit.key == o.key || opts::g_recording == o.key) {
            bool rec = opts::g_recording == o.key;
            std::wstring t = rec ? Tr(L"PRESS KEYS", L"キー入力待機中") : opts::g_edit.buffer + L"_";
            float w = P(rec ? 160 : 140), bx0 = x1 - w;
            Fill(rt, win_, bx0, cy - P(11), x1, cy + P(11), fg);
            Text(rt, win_, t, D2D1::RectF(bx0 + P(10), cy - P(11), x1 - P(10), cy + P(11)), {P(12.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(1.5f)}, bg);
            return bx0;
        }
        switch (o.kind) {
            case opts::kToggle: {
                // ON | OFF, the one that's set filled in.
                bool is_on = o.on ? o.on() : false;
                TextStyle ts{P(10.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(2), kCenter};
                float cw = P(40), ch = P(20), top = cy - ch / 2;
                float off_x0 = x1 - cw, on_x0 = off_x0 - cw;
                Frame(rt, win_, on_x0, top, x1, top + ch, px, soft);
                Fill(rt, win_, is_on ? on_x0 : off_x0, top, is_on ? off_x0 : x1, top + ch, fg);
                Text(rt, win_, L"ON", D2D1::RectF(on_x0, top, off_x0, top + ch), ts, is_on ? bg : soft);
                Text(rt, win_, L"OFF", D2D1::RectF(off_x0, top, x1, top + ch), ts, is_on ? soft : bg);
                return on_x0;
            }
            case opts::kSlider: {
                // [swatch] ▮▮▮▮▮▮▮▮▮▮---- ‹ 4800K ›
                float tw = arrows_text(o.value(), x1);
                const int n = 16;
                float bw = P(5), gap = P(2), gw = n * bw + (n - 1) * gap;
                float gx1 = x1 - tw - P(12), gx0 = gx1 - gw;
                double f = o.get ? std::clamp((o.get() - o.lo) / (o.hi - o.lo), 0.0, 1.0) : 0;
                int filled = (int)std::lround(f * n);
                if (f > 0 && filled == 0) filled = 1;
                for (int i = 0; i < n; i++) {
                    float bx = gx0 + i * (bw + gap);
                    if (i < filled) Fill(rt, win_, bx, cy - P(6), bx + bw, cy + P(6), fg);
                    else Fill(rt, win_, bx, cy - P(1), bx + bw, cy + P(1), soft);
                }
                if (enabled) HitPx(gx0 - P(3), y0, gx1 + P(3), y1, kGauge, row);
                if (o.swatch) {
                    Color t = o.swatch();
                    float sx1 = gx0 - P(10), sx0 = sx1 - P(10);
                    Fill(rt, win_, sx0, cy - P(5), sx1, cy + P(5), D2D1::ColorF(t.r, t.g, t.b, enabled ? 1.f : 0.4f));
                    return sx0;
                }
                return gx0;
            }
            case opts::kChoice: {
                float tw = arrows_text(o.value(), x1);
                // A pip per choice, the current one filled.
                int n = (int)o.choices.size(), cur = 0;
                for (int i = 0; i < n; i++)
                    if (o.current && o.choices[i].value == o.current()) cur = i;
                float ps = P(6), pg = P(4), pw = n * ps + (n - 1) * pg;
                float px1 = x1 - tw - P(10), px0 = px1 - pw;
                for (int i = 0; i < n; i++) {
                    float bx = px0 + i * (ps + pg);
                    if (i == cur) Fill(rt, win_, bx, cy - ps / 2, bx + ps, cy + ps / 2, fg);
                    else Frame(rt, win_, bx, cy - ps / 2, bx + ps, cy + ps / 2, px, soft);
                }
                return px0;
            }
            case opts::kStepper:
            case opts::kTime: return x1 - arrows_text(o.value(), x1);
            case opts::kText: {
                // A field: framed, with an underline where you'd type.
                float w = P(140), bx0 = x1 - w;
                Frame(rt, win_, bx0, cy - P(11), x1, cy + P(11), px, soft);
                Fill(rt, win_, bx0, cy + P(11) - P(2), x1, cy + P(11), fg);
                Text(rt, win_, o.value(), D2D1::RectF(bx0 + P(10), cy - P(11), x1 - P(10), cy + P(11)),
                     {P(12.5f), DWRITE_FONT_WEIGHT_NORMAL, P(1.5f), kRight}, fg);
                return bx0;
            }
            case opts::kHotkey: {
                // Keycaps: [ALT] + [SHIFT] + [PGDN].
                std::wstring keys = o.value();
                std::vector<std::wstring> parts;
                if (o.hotkey && !(g_cfg.*(o.hotkey)).empty()) {
                    std::wstring part;
                    for (wchar_t c : keys + L"+") {
                        if (c == L'+') {
                            if (!part.empty()) parts.push_back(Upper(part));
                            part.clear();
                        } else {
                            part += c;
                        }
                    }
                }
                if (parts.empty()) {
                    Text(rt, win_, Upper(keys), D2D1::RectF(x1 - P(200), y0, x1, y1), val, soft);
                    return x1 - TextWidth(Upper(keys), val);
                }
                float x = x1;
                TextStyle plus{P(11), DWRITE_FONT_WEIGHT_NORMAL, 0, kCenter};
                for (int i = (int)parts.size() - 1; i >= 0; i--) {
                    x -= Box(rt, x, cy, parts[i], fg, false, bg, 28, 7, 20);
                    if (i > 0) {
                        Text(rt, win_, L"+", D2D1::RectF(x - P(14), y0, x, y1), plus, soft);
                        x -= P(14);
                    }
                }
                return x;
            }
            case opts::kAction:
            default: {
                // A framed button; Quit fills in once it's armed.
                bool armed = o.danger && opts::QuitArmed();
                float w = Box(rt, x1, cy, Upper(o.value()), fg, armed, bg, 76, 12, 22);
                if (o.accent) {  // Afterglow: its red in the corner
                    Color red = FromTint(core::Tint(core::kDarkroom, false));
                    Fill(rt, win_, x1 - w + P(4), cy - P(11) + P(4), x1 - w + P(9), cy - P(11) + P(9), D2D1::ColorF(red.r, red.g, red.b, fg.a));
                }
                (void)on_row;
                return x1 - w;
            }
        }
    }

    // ---- today's schedule, across the top ------------------------------------------------------------------------
    // Each part in the warmth it puts on screen, toned down to sit in the palette.
    D2D1_COLOR_F PartColor(core::PhaseName p) const {
        int k = core::ScheduledKelvin(p, g_cfg);
        Color tint = FromTint(core::Tint(k, false));
        if (p == core::kDay) return Mix(g_pal.ink, tint, 0.35f);
        if (p == core::kEvening) return Mix(g_pal.paper_dark, tint, 0.62f);
        return Mix(g_pal.paper_dark, tint, 0.45f);
    }

    void DrawStrip(ID2D1RenderTarget* rt) {
        const float px = Px(), W = P(kW);
        const float x0 = P(kPad), x1 = W - P(kPad), y0 = P(kStripY), y1 = y0 + P(kStripH);
        core::LocalTime day0 = core::Midnight(NowLocal());
        const auto& events = TodaysEvents(day0);
        auto X = [&](double t) { return x0 + (x1 - x0) * (float)std::clamp((t - day0) / 86400.0, 0.0, 1.0); };
        Fill(rt, win_, x0, y0, x1, y1, C(g_pal.paper_dark));
        TextStyle seg{P(10), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(2.5f), kCenter};
        TextStyle tick{P(10), DWRITE_FONT_WEIGHT_NORMAL, P(1.5f), kCenter};
        std::vector<std::pair<float, std::wstring>> marks;  // x, label under the band
        for (size_t i = 0; i < events.size(); i++) {
            double a = std::max(events[i].start, day0);
            double b = std::min(i + 1 < events.size() ? events[i + 1].start : day0 + 172800, day0 + 86400);
            if (b <= a) continue;
            float sx0 = std::round(X(a)), sx1 = std::round(X(b));
            core::PhaseName p = events[i].name;
            Fill(rt, win_, sx0, y0, sx1, y1, PartColor(p));
            // Its name and warmth, as much as fits.
            int k = core::ScheduledKelvin(p, g_cfg);
            std::wstring name = Upper(lang::Phase(p)), warmth = Upper(lang::Kelvin(k));
            D2D1_COLOR_F text = p == core::kDay ? C(g_pal.paper) : C(g_pal.ink);
            for (const std::wstring& label : {name + L" · " + warmth, warmth, std::wstring()}) {
                if (label.empty() || TextWidth(label, seg) + P(16) > sx1 - sx0) continue;
                Text(rt, win_, label, D2D1::RectF(sx0, y0, sx1, y1), seg, text);
                break;
            }
            std::wstring go = p == core::kDay ? L"day" : p == core::kEvening ? L"evening" : g_cfg.night_darkroom ? L"nightdr" : L"nightk";
            Hitbox(sx0 / s_, kStripY, sx1 / s_, kStripY + kStripH, kStrip, 0, go);
            if (events[i].start > day0 && events[i].start < day0 + 86400) {
                std::wstring word = p == core::kDay ? (g_cfg.morning_mode == L"fixed" ? Tr(L"DAY", L"日中") : Tr(L"SUNRISE", L"日の出"))
                                    : p == core::kEvening ? (g_cfg.sunset_offset_min == 0 ? Tr(L"SUNSET", L"日の入") : Tr(L"EVENING", L"夕刻"))
                                                          : Tr(L"NIGHT", L"夜間");
                marks.push_back({sx0, word + L" " + core::FormatHM(events[i].start)});
                Fill(rt, win_, sx0, y0, sx0 + px, y1 + P(5), C(g_pal.ink));
            }
        }
        Frame(rt, win_, x0, y0, x1, y1, px, C(g_pal.ink_soft));
        // Times under the band: midnight at the ends, the switches where they are.
        float ly0 = y1 + P(5), ly1 = y1 + P(21);
        Text(rt, win_, L"00:00", D2D1::RectF(x0, ly0, x0 + P(60), ly1), {tick.size, tick.weight, tick.spacing, kLeft}, C(g_pal.ink_soft));
        Text(rt, win_, L"24:00", D2D1::RectF(x1 - P(60), ly0, x1, ly1), {tick.size, tick.weight, tick.spacing, kRight}, C(g_pal.ink_soft));
        // Each label centred on its switch, kept off the ends' times and the one
        // before it: the time alone when the words don't fit, nothing when even that doesn't.
        float last_end = x0 + P(50), right_limit = x1 - P(52);
        for (auto& [mx, label] : marks) {
            for (const std::wstring& shown : {label, label.substr(label.size() - 5)}) {
                float w = TextWidth(shown, tick);
                float a = std::max(mx - w / 2, x0 + P(52));
                if (a + w > right_limit) a = right_limit - w;
                if (a < last_end + P(8)) continue;
                Text(rt, win_, shown, D2D1::RectF(a - P(2), ly0, a + w + P(2), ly1), tick, C(g_pal.ink));
                last_end = a + w;
                break;
            }
        }
        // Now: a wedge above the band and a line through it.
        float nx = std::round(X(NowLocal()));
        Fill(rt, win_, nx - px, y0 - P(2), nx + px, y1 + P(2), C(g_pal.ink));
        Wedge(rt, nx, y0 - P(7), P(11), P(8), true, C(g_pal.ink));
    }

    // Today's switches for the band, kept until the day or one of the settings
    // they depend on changes (each one costs several time-zone lookups).
    const std::vector<core::Phase>& TodaysEvents(core::LocalTime day0) {
        const core::Config& c = g_cfg;
        std::wstring key = std::to_wstring(day0) + L"|" + std::to_wstring(c.latitude) + L"|" + std::to_wstring(c.longitude) +
                           L"|" + std::to_wstring(c.sunset_offset_min) + L"|" + c.morning_mode + L"|" + c.morning_time + L"|" +
                           c.night_start + L"|" + std::to_wstring(strip_tz_);
        if (key != strip_key_) {
            strip_events_ = core::Timeline(day0 + 43200, c);
            strip_key_ = key;
        }
        return strip_events_;
    }

    // ---- the window ------------------------------------------------------------------------------------------------
    void DrawHeader(ID2D1RenderTarget* rt) {
        const float W = P(kW), px = Px();
        const core::Status& status = engine::GetStatus();
        Color tint = FromTint(core::Tint(status.kelvin, status.disabled));
        Fill(rt, win_, P(kPad), P(33), P(kPad + 10), P(43), D2D1::ColorF(tint.r, tint.g, tint.b, 1));
        // The logo is the same in both languages: the font setting, widely spaced.
        TextStyle logo{P(24), DWRITE_FONT_WEIGHT_MEDIUM, P(7.5f), kLeft, false, 0, g_cfg.font.c_str()};
        Text(rt, win_, L"CANDELA", D2D1::RectF(P(56), P(18), P(330), P(58)), logo, C(g_pal.ink));
        float lw = TextWidth(L"CANDELA", logo);
        TextStyle mini{P(12), DWRITE_FONT_WEIGHT_NORMAL, P(4), kLeft, false, 0, g_cfg.font.c_str()};
        Text(rt, win_, L"MINI", D2D1::RectF(P(56) + lw + P(8), P(25), P(520), P(57)), mini, C(g_pal.ink_soft));
        // What's on screen now, and until when.
        std::wstring stat = Upper(opts::StatusLine()), until = Upper(opts::UntilLine());
        float sx1 = W - P(kPad + 46);
        Text(rt, win_, stat, D2D1::RectF(P(400), P(20), sx1, P(40)), {P(12), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(3), kRight}, C(g_pal.ink));
        Text(rt, win_, until, D2D1::RectF(P(300), P(39), sx1, P(56)), {P(10), DWRITE_FONT_WEIGHT_NORMAL, P(2), kRight}, C(g_pal.ink_soft));
        float cx0 = W - P(kPad + 30), cy0 = P(23), cx1 = W - P(kPad), cy1 = P(53);
        bool hot = hover_act_ == kClose;
        if (hot) Fill(rt, win_, cx0, cy0, cx1, cy1, C(g_pal.ink));
        else Frame(rt, win_, cx0, cy0, cx1, cy1, px, C(g_pal.ink_soft));
        D2D1_COLOR_F xc = C(hot ? g_pal.paper : g_pal.ink);
        Line(rt, win_, cx0 + P(10), cy0 + P(10), cx1 - P(10), cy1 - P(10), std::max(1.f, P(1.5f)), xc);
        Line(rt, win_, cx1 - P(10), cy0 + P(10), cx0 + P(10), cy1 - P(10), std::max(1.f, P(1.5f)), xc);
        Hitbox(kW - kPad - 30, 23, kW - kPad, 53, kClose, 0);
    }

    // The two pages, as tabs on the left of a rule.
    void DrawTabs(ID2D1RenderTarget* rt) {
        const float W = P(kW), px = Px();
        float x = P(kPad);
        for (int i = 0; i < (int)pages_.size(); i++) {
            bool on = i == page_;
            bool hot = !on && hover_act_ == kTab && hover_a_ == i;
            std::wstring label = Upper(pages_[i].label);
            TextStyle ts{P(12), on ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, P(2.5f)};
            TextStyle bold{P(12), DWRITE_FONT_WEIGHT_BOLD, P(2.5f)};
            float x1 = std::round(x + P(14) + P(6) + P(10) + TextWidth(label, bold) + P(18));
            if (on) Fill(rt, win_, x, P(kTabY), x1, P(kTabY + kTabH), C(g_pal.ink));
            else if (hot) Fill(rt, win_, x, P(kTabY), x1, P(kTabY + kTabH), C(g_pal.paper_dark));
            float mx = x + P(14);
            Fill(rt, win_, mx, P(kTabY + 12), mx + P(6), P(kTabY + 18), C(on ? g_pal.paper : hot ? g_pal.ink_soft : g_pal.paper_dark));
            Text(rt, win_, label, D2D1::RectF(mx + P(16), P(kTabY), x1, P(kTabY + kTabH)), ts,
                 C(on ? g_pal.paper : hot ? g_pal.ink : g_pal.ink_soft));
            Hitbox(x / s_, kTabY, x1 / s_, kTabY + kTabH, kTab, i);
            x = x1 + P(4);
        }
        Fill(rt, win_, P(kPad), P(kTabY + kTabH), W - P(kPad), P(kTabY + kTabH) + px, C(g_pal.ink));
    }

    void DrawGroups(ID2D1RenderTarget* rt) {
        const float px = Px();
        for (int g = 0; g < (int)Page().groups.size(); g++) {
            const auto& grp = Page().groups[g];
            int c = std::clamp(grp.column, 0, 1);
            float x0 = P(kColX[c][0]), x1 = P(kColX[c][1]), y = P(group_y_[g]);
            Text(rt, win_, Upper(grp.label), D2D1::RectF(x0, y, x1, y + P(18)), {P(10), DWRITE_FONT_WEIGHT_SEMI_BOLD, P(3)},
                 C(g_pal.ink_soft));
            Fill(rt, win_, x0, y + P(20), x1, y + P(20) + px, C(g_pal.paper_dark));
        }
    }

    void DrawRows(ID2D1RenderTarget* rt) {
        for (int i = 0; i < (int)rows_.size(); i++) {
            const opts::Option& o = Opt(i);
            const Row& r = rows_[i];
            bool on = i == sel_, enabled = o.enabled();
            float x0 = P(r.x0), x1 = P(r.x1), y0 = P(r.y0), y1 = P(r.y1);
            // Row first: the control's own targets (‹, ›, the gauge) go on top of it.
            Hitbox(r.x0, r.y0, r.x1, r.y1, kRow, i);
            D2D1_COLOR_F bg;
            if (on) {
                bg = C(g_pal.ink, enabled ? 1.f : 0.5f);
                Fill(rt, win_, x0, y0, x1, y1, C(g_pal.paper_dark));
                Fill(rt, win_, x0, y0, x1, y1, bg);
                Wedge(rt, x0 - P(20), (y0 + y1) / 2, P(12), P(14), false, C(g_pal.ink));
                bg = C(g_pal.ink);
            } else {
                bg = C(g_pal.paper_dark, 0.7f);
                Fill(rt, win_, x0, y0, x1, y1, bg);
            }
            D2D1_COLOR_F fg = on ? C(g_pal.paper) : C(g_pal.ink, enabled ? 1.f : 0.4f);
            if (on && !enabled) fg.a = 0.6f;
            float cx0 = DrawControl(rt, i, o, y0, y1, x1 - P(12), fg, bg, on);
            Text(rt, win_, o.label, D2D1::RectF(x0 + P(14), y0, std::max(x0 + P(100), cx0 - P(12)), y1),
                 {P(14), DWRITE_FONT_WEIGHT_NORMAL, P(0.3f)}, fg);
        }
        // The page's own note, under the right column (the hotkeys page says how to set one).
        if (!Page().note.empty()) {
            float y = P(column_end_[1] + 22);
            Text(rt, win_, Page().note, D2D1::RectF(P(kColX[1][0]), y, P(kColX[1][1]), y + P(80)),
                 {P(12.5f), DWRITE_FONT_WEIGHT_NORMAL, 0, kLeft, true, P(20)}, C(g_pal.ink_soft));
        }
    }

    // What went wrong, else what the mouse and keys do.
    void DrawFooter(ID2D1RenderTarget* rt) {
        const float W = P(kW), px = Px();
        float fy = P(kFootY);
        Fill(rt, win_, P(kPad), fy, W - P(kPad), fy + px, C(g_pal.ink));
        float hy = fy + P(24);
        bool err = true;
        std::wstring notice = opts::CurrentNotice(&err);
        bool warm = notice.empty() && engine::RegistryProblem();  // warm colors locked, or waiting for a restart
        if (notice.empty() && !warm) {
            auto problems = engine::Problems();
            if (!problems.empty()) notice = problems[0];
        }
        TextStyle lab{P(10.5f), DWRITE_FONT_WEIGHT_NORMAL, P(2)};
        if (warm) {
            // Said in the footer, where it stays, with a tag; a click goes to the fix.
            std::wstring problem = Upper(engine::Problems()[0]);
            std::wstring tag = engine::WarmRestartPending() ? Tr(L"RESTART", L"再起動") : Tr(L"WARNING", L"警告");
            TextStyle chip{P(10.5f), DWRITE_FONT_WEIGHT_BOLD, P(1)};
            float kw = TextWidth(tag, chip) + P(12), x0 = P(kPad);
            Fill(rt, win_, x0, hy - P(9), x0 + kw, hy + P(9), C(g_pal.ink));
            Text(rt, win_, tag, D2D1::RectF(x0, hy - P(9), x0 + kw, hy + P(9)), {chip.size, chip.weight, chip.spacing, kCenter}, C(g_pal.paper));
            TextStyle strong{lab.size, DWRITE_FONT_WEIGHT_SEMI_BOLD, lab.spacing};
            float tx0 = x0 + kw + P(9), tx1 = W - P(kPad);  // the whole width: "saved" waits
            Text(rt, win_, problem, D2D1::RectF(tx0, hy - P(10), tx1, hy + P(10)), strong, C(g_pal.ink));
            float tw = std::min(TextWidth(problem, strong), tx1 - tx0);
            if (hover_act_ == kStrip && hover_a_ == 1) Fill(rt, win_, tx0, hy + P(10), tx0 + tw, hy + P(10) + px, C(g_pal.ink));
            Hitbox(kPad, hy / s_ - 14, (tx0 + tw) / s_ + 4, hy / s_ + 14, kStrip, 1, L"warm");
        } else if (!notice.empty()) {
            Fill(rt, win_, P(kPad), hy - P(3), P(kPad) + P(6), hy + P(3), C(g_pal.ink));
            Text(rt, win_, Upper(notice), D2D1::RectF(P(kPad + 16), hy - P(10), W - P(250), hy + P(10)), lab, C(g_pal.ink));
        } else {
            float hx = P(kPad);
            auto hint = [&](const std::wstring& key, const std::wstring& what) {
                TextStyle chip{P(10.5f), DWRITE_FONT_WEIGHT_BOLD, P(1)};
                float kw = TextWidth(key, chip) + P(12);
                Fill(rt, win_, hx, hy - P(9), hx + kw, hy + P(9), C(g_pal.ink));
                Text(rt, win_, key, D2D1::RectF(hx, hy - P(9), hx + kw, hy + P(9)), {chip.size, chip.weight, chip.spacing, kCenter}, C(g_pal.paper));
                float ww = TextWidth(what, lab);
                Text(rt, win_, what, D2D1::RectF(hx + kw + P(7), hy - P(10), hx + kw + P(9) + ww, hy + P(10)), lab, C(g_pal.ink));
                hx += kw + P(9) + ww + P(20);
            };
            hint(L"CLICK", Tr(L"CHANGE", L"決定"));
            hint(Tr(L"R-CLICK", L"右クリック"), Tr(L"BACK", L"戻す"));
            hint(L"TAB", Tr(L"PAGE", L"ページ切替"));
            hint(L"ESC", Tr(L"CLOSE", L"閉じる"));
        }
        if (!warm)
            Text(rt, win_, Tr(L"SAVED AUTOMATICALLY", L"変更は自動保存"), D2D1::RectF(W - P(260), hy - P(10), W - P(kPad), hy + P(10)),
                 {P(10.5f), DWRITE_FONT_WEIGHT_NORMAL, P(2), kRight}, C(g_pal.ink_soft));
    }

    void Render() {
        if (!open_) return;
        ID2D1RenderTarget* rt = win_.Begin();
        if (!rt) return;
        hits_.clear();
        const float W = P(kW), H = P(kH), m = P(kMargin), off = P(6);
        // A flat shadow, then the window on top.
        Fill(rt, win_, m + off, m + off, m + W + off, m + H + off, C(g_pal.shade, 0.45f));
        rt->SetTransform(D2D1::Matrix3x2F::Translation(m, m));
        Fill(rt, win_, 0, 0, W, H, C(g_pal.paper));
        Frame(rt, win_, 0, 0, W, H, Px(), C(g_pal.paper_dark));
        DrawHeader(rt);
        DrawStrip(rt);
        DrawTabs(rt);
        DrawGroups(rt);
        DrawRows(rt);
        DrawFooter(rt);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        RECT r;
        GetWindowRect(win_.hwnd, &r);
        win_.End(r.left, r.top, 1.f);
    }

    // ---- input ---------------------------------------------------------------------------------------------------
    void SwitchPage(int i) {
        int n = (int)pages_.size();
        i = ((i % n) + n) % n;
        if (i == page_) return;
        opts::EndEdit(EditingOption(), true);
        opts::StopRecording();
        page_ = i;
        sel_ = 0;
        Rebuild();
        sel_ = 0;
        Render();
    }

    void SelectRow(int i) {
        if (rows_.empty()) return;
        i = std::clamp(i, 0, (int)rows_.size() - 1);
        if (i == sel_) return;
        opts::EndEdit(EditingOption(), true);
        opts::StopRecording();
        sel_ = i;
    }

    // Selects a setting wherever it is (a click on today's band).
    void GoTo(const std::wstring& key) {
        for (int p = 0; p < (int)pages_.size(); p++)
            for (auto& g : pages_[p].groups)
                for (auto& o : g.opts)
                    if (o.key == key) {
                        if (p != page_) SwitchPage(p);
                        for (int i = 0; i < (int)rows_.size(); i++)
                            if (Opt(i).key == key) SelectRow(i);
                        Render();
                        return;
                    }
    }

    // Changes the selected setting: +1 / -1 (click, Enter / right-click, arrows, wheel).
    void Step(int dir) {
        if (sel_ < 0 || sel_ >= (int)rows_.size()) return;
        const opts::Option& o = Opt(sel_);
        if (!o.enabled()) return;
        auto change = o.change;  // a copy: running it may rebuild the option list
        change(dir);
        if (open_) {
            Rebuild();
            Render();
        }
    }

    // Activates the selected setting: click, Enter or Space (dir +1), right-click (-1).
    void Activate(int dir) {
        if (sel_ < 0 || sel_ >= (int)rows_.size()) return;
        const opts::Option& o = Opt(sel_);
        if (!o.enabled()) return;
        switch (o.kind) {
            case opts::kTime:
                // Typed on a left click; stepped back on a right one.
                if (dir < 0 && opts::g_edit.key != o.key) return Step(-1);
                [[fallthrough]];
            case opts::kText:
                if (opts::g_edit.key == o.key) opts::EndEdit(&o, true);
                else opts::BeginEdit(o);
                break;
            case opts::kHotkey:
                if (opts::g_recording == o.key) opts::StopRecording();
                else opts::BeginRecording(o);
                break;
            case opts::kAction:
                if (dir > 0) return Step(1);
                return;
            default: return Step(dir);
        }
        Rebuild();
        Render();
    }

    int HitAt(float x, float y) const {
        for (int i = (int)hits_.size() - 1; i >= 0; i--) {
            const auto& h = hits_[i];
            if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return i;
        }
        return -1;
    }

    // Window coordinates (unscaled) from a client point.
    float Lx(int x) const { return (x - P(kMargin)) / s_; }
    float Ly(int y) const { return (y - P(kMargin)) / s_; }

    void Click(int x, int y, int dir) {
        float lx = Lx(x), ly = Ly(y);
        int i = HitAt(lx, ly);
        if (i < 0) {  // a click on nothing ends typing or recording
            opts::EndEdit(EditingOption(), true);
            opts::StopRecording();
            Render();
            return;
        }
        Hit h = hits_[i];
        switch (h.act) {
            case kTab: SwitchPage(h.a); break;
            case kRow:
                SelectRow(h.a);
                Activate(dir);
                break;
            case kDec:
            case kInc:
                SelectRow(h.a);
                opts::EndEdit(EditingOption(), true);
                Step(h.act == kInc ? 1 : -1);
                break;
            case kGauge: {
                SelectRow(h.a);
                const opts::Option& o = Opt(sel_);
                if (dir < 0) return Step(-1);
                // Where the click landed on the gauge, as a value.
                double f = std::clamp((lx - h.r.left - 3) / (h.r.right - h.r.left - 6), 0.f, 1.f);
                auto set = o.set;
                if (set) set(o.lo + f * (o.hi - o.lo));
                Rebuild();
                Render();
                break;
            }
            case kStrip:
                GoTo(h.key);
                // The warm-colors warning in the footer: a click runs its fix
                // (unlock, or the restart question), as it says.
                if (h.key == L"warm" && sel_ >= 0 && sel_ < (int)rows_.size() && Opt(sel_).key == L"warm") Activate(1);
                break;
            case kClose: PostMessageW(win_.hwnd, WM_CLOSE, 0, 0); break;
        }
    }

    bool Key(UINT vk) {
        if (vk == VK_F4 && GetKeyState(VK_MENU) < 0 && opts::g_recording.empty()) return false;  // Alt+F4 closes
        if (opts::EditKey(vk, EditingOption())) {
            Rebuild();
            Render();
            return true;
        }
        if (!opts::g_recording.empty()) {
            opts::RecordKey(vk, RecordingOption());
            Rebuild();
            Render();
            return true;
        }
        bool shift = GetKeyState(VK_SHIFT) < 0;
        switch (vk) {
            case VK_UP: SelectRow(sel_ - 1); Render(); return true;
            case VK_DOWN: SelectRow(sel_ + 1); Render(); return true;
            case VK_LEFT:
            case VK_RIGHT: {
                if (sel_ < 0 || sel_ >= (int)rows_.size()) return true;
                auto kind = Opt(sel_).kind;
                if (kind == opts::kToggle || kind == opts::kChoice || kind == opts::kSlider || kind == opts::kStepper || kind == opts::kTime)
                    Step(vk == VK_RIGHT ? 1 : -1);
                return true;
            }
            case VK_RETURN:
            case VK_SPACE: Activate(1); return true;
            case VK_TAB: SwitchPage(page_ + (shift ? -1 : 1)); return true;
            case VK_PRIOR: SwitchPage(page_ - 1); return true;
            case VK_NEXT: SwitchPage(page_ + 1); return true;
            case VK_ESCAPE: PostMessageW(win_.hwnd, WM_CLOSE, 0, 0); return true;
        }
        return false;
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = (SettingsWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
            case WM_TIMER:
                if (!IsIconic(hwnd)) self->Render();
                return 0;
            case WM_NCHITTEST: {
                POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
                ScreenToClient(hwnd, &pt);
                float lx = self->Lx(pt.x), ly = self->Ly(pt.y);
                if (lx < 0 || ly < 0 || lx > kW || ly > kH) return HTTRANSPARENT;  // the shadow
                if (ly < kStripY - 12 && self->HitAt(lx, ly) < 0) return HTCAPTION;  // drag by the header
                return HTCLIENT;
            }
            case WM_MOUSEMOVE: {
                POINT pt;
                GetCursorPos(&pt);
                if (pt.x == self->last_mouse_.x && pt.y == self->last_mouse_.y) return 0;
                self->last_mouse_ = pt;
                int i = self->HitAt(self->Lx((short)LOWORD(lp)), self->Ly((short)HIWORD(lp)));
                int act = i >= 0 ? self->hits_[i].act : -1, a = i >= 0 ? self->hits_[i].a : -1;
                int sel_before = self->sel_;
                // Hovering a row (or one of its parts) selects it, unless something is being typed.
                bool on_row = act == kRow || act == kDec || act == kInc || act == kGauge;
                if (on_row && !opts::g_edit.Active() && opts::g_recording.empty()) self->SelectRow(a);
                if (act != self->hover_act_ || a != self->hover_a_ || self->sel_ != sel_before) {
                    self->hover_act_ = act;
                    self->hover_a_ = a;
                    self->Render();
                }
                TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                return 0;
            }
            case WM_MOUSELEAVE:
                if (self->hover_act_ != -1) {
                    self->hover_act_ = self->hover_a_ = -1;
                    self->Render();
                }
                self->last_mouse_ = {-1, -1};
                return 0;
            case WM_MOUSEWHEEL: {
                POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
                ScreenToClient(hwnd, &pt);
                int i = self->HitAt(self->Lx(pt.x), self->Ly(pt.y));
                if (i < 0 || opts::g_edit.Active() || !opts::g_recording.empty()) return 0;
                Act act = self->hits_[i].act;
                if (act != kRow && act != kDec && act != kInc && act != kGauge) return 0;
                self->SelectRow(self->hits_[i].a);
                auto kind = self->Opt(self->sel_).kind;
                if (kind == opts::kToggle || kind == opts::kChoice || kind == opts::kSlider || kind == opts::kStepper || kind == opts::kTime)
                    self->Step(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
                return 0;
            }
            case WM_SETCURSOR:
                if (LOWORD(lp) == HTCLIENT) {
                    // A hand over what does something when clicked by itself (not the rows: hovering them selects).
                    bool link = self->hover_act_ == kTab || self->hover_act_ == kClose || self->hover_act_ == kStrip ||
                                self->hover_act_ == kDec || self->hover_act_ == kInc || self->hover_act_ == kGauge;
                    SetCursor(LoadCursorW(nullptr, link ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                break;
            case WM_LBUTTONDOWN: self->Click((short)LOWORD(lp), (short)HIWORD(lp), +1); return 0;
            case WM_RBUTTONDOWN: self->Click((short)LOWORD(lp), (short)HIWORD(lp), -1); return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (self->Key((UINT)wp)) return 0;
                break;
            case WM_SYSCHAR: return 0;  // Alt+letter: no beep (there's no menu to open)
            case WM_CHAR:
                if (opts::EditChar((wchar_t)wp)) self->Render();
                return 0;
            case WM_CLOSE: self->Close(); return 0;
            case WM_DPICHANGED: {
                self->s_ = LOWORD(wp) / 96.f;
                const RECT* r = (const RECT*)lp;
                int w = (int)(self->P(kW) + 2 * self->P(kMargin)), h = (int)(self->P(kH) + 2 * self->P(kMargin));
                self->win_.Resize(w, h);
                SetWindowPos(hwnd, nullptr, r->left, r->top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                self->Render();
                return 0;
            }
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) {
                    opts::EndEdit(self->EditingOption(), true);
                    opts::StopRecording();
                    self->Rebuild();
                }
                self->Render();
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    Surface win_;
    std::vector<opts::Page> pages_;
    std::vector<Row> rows_;
    std::vector<float> group_y_;
    float column_end_[2] = {kBodyY, kBodyY};
    std::vector<Hit> hits_;
    int page_ = 0, sel_ = 0, hover_act_ = -1, hover_a_ = -1;
    bool open_ = false;
    float s_ = 1;
    POINT last_mouse_ = {-1, -1};
    std::wstring select_key_;  // Select() before Open: the row to start on
    std::vector<core::Phase> strip_events_;  // TodaysEvents()
    std::wstring strip_key_;
    int strip_tz_ = 0;  // bumped when the clock or time zone may have changed (opened again, Refresh)
};

SettingsWindow& Main() {
    static SettingsWindow w;
    return w;
}

}  // namespace ui

namespace settings {
void Open() { ui::Main().Open(); }
void OpenOn(const std::wstring& key) {
    ui::Main().Select(key);
    Open();
}
bool IsOwnWindow(HWND h) { return ui::Main().Owns(h); }
void Refresh() { ui::Main().Refresh(); }
void DebugDump(const std::wstring& dir) { ui::Main().DebugDump(dir); }
void Shutdown() { ui::Main().Shutdown(); }
}  // namespace settings
