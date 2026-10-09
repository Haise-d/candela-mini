#include "options.h"
#include "render.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace opts {

Editor g_edit;
std::wstring g_recording;
static std::wstring g_notice;
static bool g_notice_error = false;
static double g_notice_at = -1e9, g_quit_armed = -1e9;

void Changed() { engine::Wake(); }

void Notice(const std::wstring& text, bool error) {
    g_notice = text;
    g_notice_error = error;
    g_notice_at = NowMs();
}

std::wstring CurrentNotice(bool* error) {
    if (NowMs() - g_notice_at > 6000) return L"";
    if (error) *error = g_notice_error;
    return g_notice;
}

bool QuitArmed() { return NowMs() - g_quit_armed < 2500; }

std::wstring StatusLine() {
    if (engine::Paused()) return Tr(L"Paused", L"一時停止中");
    const core::Status& st = engine::GetStatus();
    std::wstring s = lang::Phase(st.phase.name) + L" · ";
    if (st.disabled) return s + Tr(L"off", L"OFF");
    // Windows wouldn't take what was asked for: what's really on screen.
    int k = st.kelvin;
    double b = st.brightness;
    bool limited = engine::Limited(&k, &b);
    s += (limited && k > core::kDarkroom ? L"~" : L"") + lang::Kelvin(k);
    if (b < 0.995) s += L" · " + std::to_wstring((int)std::nearbyint(b * 100)) + L"%";
    if (limited) s += Tr(L" · limited", L" · 制限中");  // the footer says why
    return s;
}

std::wstring UntilLine() {
    if (engine::Paused()) return Tr(L"normal colors until resumed", L"再開まで通常の色");
    const core::Status& st = engine::GetStatus();
    std::wstring until = lang::Until(st);
    if (Japanese()) {
        if (st.disabled) return until.empty() ? L"OFF" : until + L" OFF";
        if (st.overridden) return until + L" 調整中（予定：" + lang::Kelvin(st.scheduled) + L"）";
        return until;
    }
    if (st.disabled) return L"off " + until;
    if (st.overridden) return L"adjusted " + until + L" (schedule: " + core::Describe(st.scheduled) + L")";
    return until;
}

// ---- builders ---------------------------------------------------------------------------------------
namespace {

Option Toggle(std::wstring key, std::wstring label, bool* flag) {
    Option o;
    o.key = key;
    o.label = label;
    o.kind = kToggle;
    o.value = [flag] { return lang::OnOff(*flag); };
    o.on = [flag] { return *flag; };
    o.change = [flag](int) {
        *flag = !*flag;
        Changed();
    };
    return o;
}

// A toggle whose state lives outside the settings (Windows, the cursors), read once per rebuild.
Option Live(std::wstring key, std::wstring label, bool on, std::function<void(bool)> set) {
    Option o;
    o.key = key;
    o.label = label;
    o.kind = kToggle;
    o.on = [on] { return on; };
    o.value = [on] { return lang::OnOff(on); };
    o.change = [on, set](int) { set(!on); };
    return o;
}

Option ChoiceOf(std::wstring key, std::wstring label, std::vector<Choice> choices, std::wstring* field,
                std::function<void()> after = nullptr) {
    Option o;
    o.key = key;
    o.label = label;
    o.kind = kChoice;
    o.choices = choices;
    o.current = [field] { return *field; };
    o.pick = [field, after](const std::wstring& v) {
        if (*field == v) return;
        *field = v;
        Changed();
        if (after) after();
    };
    o.value = [field, choices] {
        for (auto& c : choices)
            if (c.value == *field) return c.label;
        return *field;
    };
    o.change = [o](int dir) {
        int n = (int)o.choices.size(), i = 0;
        for (int k = 0; k < n; k++)
            if (o.choices[k].value == o.current()) i = k;
        o.pick(o.choices[((i + dir) % n + n) % n].value);
    };
    return o;
}

// A number stepped through a few values (stops at the first and last).
Option Steps(std::wstring key, std::wstring label, std::vector<int> values, int* field, std::function<std::wstring(int)> show) {
    Option o;
    o.key = key;
    o.label = label;
    o.kind = kStepper;
    o.value = [field, show] { return show(*field); };
    o.change = [field, values](int dir) {
        int n = (int)values.size(), i = -1;
        for (int k = 0; k < n; k++)
            if (values[k] == *field) i = k;
        if (i < 0) {  // a value typed into the file: go to its neighbour
            i = 0;
            while (i < n - 1 && values[i] < *field) i++;
            if (dir > 0 && values[i] > *field) i--;
        }
        *field = values[std::clamp(i + dir, 0, n - 1)];
        Changed();
    };
    return o;
}

Option Kelvin(std::wstring key, std::wstring label, int* field) {
    Option o;
    o.key = key;
    o.label = label;
    o.kind = kSlider;
    o.lo = core::kMinK;
    o.hi = core::kMaxK;
    o.step = 100;
    o.get = [field] { return (double)*field; };
    o.set = [field](double v) {
        int k = (int)std::clamp(std::nearbyint(v / 100) * 100, (double)core::kMinK, (double)core::kMaxK);
        if (k == *field) return;
        *field = k;
        Changed();
    };
    o.value = [field] { return std::to_wstring(*field) + L"K"; };
    o.change = [o](int dir) { o.set(o.get() + dir * o.step); };
    o.swatch = [field] { return FromTint(core::Tint(*field, false)); };
    return o;
}

Option TimeOf(std::wstring key, std::wstring label, std::wstring* field) {
    Option o;
    o.key = key;
    o.label = label;
    o.kind = kTime;
    o.value = [field] { return *field; };
    o.text = [field] { return *field; };
    o.commit = [field](const std::wstring& t, std::wstring* err) {
        auto hm = core::ParseHM(t);
        if (!hm) {
            *err = Tr(L"Times are written like 22:00.", L"警告：時刻の形式が不正。例：22:00");
            return false;
        }
        wchar_t buf[8];
        swprintf(buf, 8, L"%02d:%02d", hm->h, hm->m);
        *field = buf;
        Changed();
        return true;
    };
    o.change = [field](int dir) {  // 15 minutes at a time
        auto hm = core::ParseHM(*field);
        int m = hm ? hm->h * 60 + hm->m : 0;
        m = ((m / 15 * 15 + (dir > 0 ? 15 : (m % 15 ? 0 : -15))) % 1440 + 1440) % 1440;
        wchar_t buf[8];
        swprintf(buf, 8, L"%02d:%02d", m / 60, m % 60);
        *field = buf;
        Changed();
    };
    return o;
}

Option Action(std::wstring key, std::wstring label, std::function<std::wstring()> value, std::function<void()> run) {
    Option o;
    o.key = key;
    o.label = label;
    o.kind = kAction;
    o.value = value;
    o.change = [run](int dir) {
        if (dir > 0) run();
    };
    return o;
}

std::wstring Coord(double v) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.4f", v);
    return buf;
}

// "48.8566" or a pasted "48.8566, 2.3522" (as Google Maps copies it).
bool ParseCoords(const std::wstring& t, double* a, double* b, bool* pair) {
    std::wstring s = t;
    // "41,38" (a decimal comma) is one number; "41.38, 2.17" or "41,38; 2,17" are two.
    size_t semi = s.find(L';');
    if (semi != std::wstring::npos) {
        for (auto& c : s) {
            if (c == L',') c = L'.';
        }
        s[semi] = L',';
    } else {
        size_t first = s.find(L','), count = std::count(s.begin(), s.end(), L',');
        bool pair_like = count == 1 && (s.find(L", ") != std::wstring::npos ||
                                        (s.find(L'.') < first && s.find(L'.', first) != std::wstring::npos));
        if (count == 1 && !pair_like) s[first] = L'.';
        else if (count == 3) {  // "41,38, 2,17"
            size_t sep = s.find(L", ");
            if (sep == std::wstring::npos) return false;
            for (size_t i = 0; i < s.size(); i++)
                if (s[i] == L',' && i != sep) s[i] = L'.';
        }
    }
    size_t comma = s.find(L',');
    auto num = [](std::wstring x, double* out) {
        while (!x.empty() && iswspace(x.front())) x.erase(x.begin());
        while (!x.empty() && (iswspace(x.back()) || x.back() == L'°')) x.pop_back();
        if (x.empty()) return false;
        wchar_t* end = nullptr;
        *out = wcstod(x.c_str(), &end);
        return end && *end == 0 && std::isfinite(*out);
    };
    *pair = comma != std::wstring::npos;
    if (!*pair) return num(s, a);
    return num(s.substr(0, comma), a) && num(s.substr(comma + 1), b);
}

Option Coordinate(bool latitude) {
    Option o;
    o.key = latitude ? L"lat" : L"lon";
    o.label = latitude ? Tr(L"Latitude", L"緯度") : Tr(L"Longitude", L"経度");
    o.kind = kText;
    double* field = latitude ? &g_cfg.latitude : &g_cfg.longitude;
    o.value = [field] { return Coord(*field); };
    o.text = [field] { return Coord(*field); };
    o.commit = [latitude, field](const std::wstring& t, std::wstring* err) {
        double a = 0, b = 0;
        bool pair = false;
        if (!ParseCoords(t, &a, &b, &pair)) {
            *err = Tr(L"That isn't a number.", L"警告：数値として認識不可。");
            return false;
        }
        if (pair) {
            if (!(a >= -90 && a <= 90 && b >= -180 && b <= 180)) {
                *err = Tr(L"Latitude must be -90..90 and longitude -180..180.", L"警告：緯度は−90〜90、経度は−180〜180の範囲で入力。");
                return false;
            }
            g_cfg.latitude = a;
            g_cfg.longitude = b;
        } else {
            if (latitude ? !(a >= -90 && a <= 90) : !(a >= -180 && a <= 180)) {
                *err = latitude ? Tr(L"Latitude must be between -90 and 90.", L"警告：緯度は−90〜90の範囲で入力。")
                                : Tr(L"Longitude must be between -180 and 180.", L"警告：経度は−180〜180の範囲で入力。");
                return false;
            }
            *field = a;
        }
        Changed();
        return true;
    };
    o.change = [](int) {};
    return o;
}

}  // namespace

// ---- the list ------------------------------------------------------------------------------------------
// Labels come in pairs, English and Japanese. The Japanese reads like a terse
// system log: 報告： (report), 警告： (warning), and 当機 when Candela speaks of itself.
std::vector<Page> Build() {
    std::vector<Page> out;
    core::Config& c = g_cfg;

    // ======== the settings page ========
    Page main{L"settings", Tr(L"Settings", L"設定"), {}, L""};

    // ---- left: the schedule, and where you are ----
    Group sc{Tr(L"Schedule", L"スケジュール"), 0, {}};
    sc.opts.push_back(Kelvin(L"day", Tr(L"Day", L"日中"), &c.day_kelvin));
    sc.opts.push_back(Kelvin(L"evening", Tr(L"From sunset", L"日没以降"), &c.evening_kelvin));
    {
        Option o;
        o.key = L"offset";
        o.label = Tr(L"Sunset offset", L"日没補正");
        o.kind = kStepper;
        o.value = [] {
            int v = g_cfg.sunset_offset_min;
            std::wstring unit = Tr(L" min", L"分");
            return v == 0 ? L"0" + unit : (v > 0 ? L"+" : L"−") + std::to_wstring(std::abs(v)) + unit;
        };
        o.change = [](int dir) {
            g_cfg.sunset_offset_min = std::clamp(g_cfg.sunset_offset_min + dir * 5, -180, 180);
            Changed();
        };
        sc.opts.push_back(o);
    }
    sc.opts.push_back(TimeOf(L"night", Tr(L"Night starts at", L"夜間開始"), &c.night_start));
    sc.opts.push_back(Toggle(L"nightdr", Tr(L"Night uses Afterglow", L"夜間は残光"), &c.night_darkroom));
    {
        Option o = Kelvin(L"nightk", Tr(L"Night", L"夜間"), &c.night_kelvin);
        o.enabled = [] { return !g_cfg.night_darkroom; };
        sc.opts.push_back(o);
    }
    sc.opts.push_back(ChoiceOf(L"morning", Tr(L"Day starts", L"日中開始"),
                               {{L"sunrise", Tr(L"Sunrise", L"日の出")}, {L"fixed", Tr(L"Fixed time", L"指定時刻")}},
                               &c.morning_mode));
    {
        Option o = TimeOf(L"morningt", Tr(L"Day starts at", L"日中開始時刻"), &c.morning_time);
        o.enabled = [] { return g_cfg.morning_mode == L"fixed"; };
        sc.opts.push_back(o);
    }
    sc.opts.push_back(Coordinate(true));
    sc.opts.push_back(Coordinate(false));
    main.groups.push_back(sc);

    // ---- left: the screen ----
    Group scr{Tr(L"Screen", L"画面"), 0, {}};
    scr.opts.push_back(Toggle(L"cover", Tr(L"Blackout covers every window", L"ブラックアウトを最前面に固定"),
                              &c.blackout_always_on_top));
    scr.opts.push_back(Live(L"cursors", Tr(L"Black cursors", L"黒いカーソル"), theme::Applied(), [](bool on) {
        if (!on) theme::Restore();
        else if (!theme::Apply())
            Notice(Tr(L"The theme folder isn't next to candela-mini.exe.", L"警告：candela-mini.exe と同じ場所に theme フォルダーがない。"));
    }));
    main.groups.push_back(scr);

    // ---- right: Afterglow (red by brightness; "darkroom" in the code and the settings file) ----
    Group dr{Tr(L"Afterglow", L"残光"), 1, {}};
    {
        Option o = Action(L"drnow", Tr(L"Afterglow now", L"残光を起動"),
                          [] { return engine::GetStatus().Darkroom() ? Tr(L"Leave", L"解除") : Tr(L"Start", L"起動"); },
                          [] { engine::Hotkey(HK_DARKROOM); });
        o.accent = true;
        o.enabled = [] { return !engine::Paused(); };
        dr.opts.push_back(o);
    }
    {
        Option o;
        o.key = L"drbright";
        o.label = Tr(L"Brightness", L"輝度");
        o.kind = kSlider;
        o.lo = 10;
        o.hi = 100;
        o.step = 5;
        o.get = [] { return g_cfg.darkroom_brightness * 100; };
        o.set = [](double v) {
            double b = std::clamp(std::nearbyint(v / 5) * 5, 10.0, 100.0) / 100;
            if (std::fabs(b - g_cfg.darkroom_brightness) < 1e-6) return;
            g_cfg.darkroom_brightness = b;
            Changed();
        };
        o.value = [] { return std::to_wstring((int)std::nearbyint(g_cfg.darkroom_brightness * 100)) + L"%"; };
        o.change = [o](int dir) { o.set(o.get() + dir * o.step); };
        // Afterglow's red, as bright as it's set.
        o.swatch = [] {
            Color red = FromTint(core::Tint(core::kDarkroom, false));
            float b = (float)g_cfg.darkroom_brightness;
            return Color{red.r * b, red.g * b, red.b * b, 1.f};
        };
        dr.opts.push_back(o);
    }
    dr.opts.push_back(Toggle(L"readable", Tr(L"Keep all text readable", L"文字の判読性を維持"), &c.darkroom_by_brightness));
    dr.opts.push_back(Toggle(L"invert", Tr(L"Invert", L"反転"), &c.darkroom_invert));
    main.groups.push_back(dr);

    // ---- right: system ----
    Group sy{Tr(L"System", L"システム"), 1, {}};
    // Both languages in the label, so whoever switched by accident can find the way back.
    sy.opts.push_back(ChoiceOf(L"language", Tr(L"Language · 言語", L"言語 · LANGUAGE"), {{L"en", L"English"}, {L"ja", L"日本語"}},
                               &c.language, [] {
                                   gfx::WarmFonts();
                                   engine::StyleChanged();
                               }));
    {
        bool elsewhere = sys::StartupPointsElsewhere();
        std::wstring label = Tr(L"Start with Windows", L"Windows起動時に実行");
        if (elsewhere) label += Tr(L" (another copy)", L"（別の場所）");
        sy.opts.push_back(Live(L"startup", label, sys::StartupEnabled(), [](bool on) {
            if (!sys::SetStartup(on)) Notice(Tr(L"Windows didn't allow changing that.", L"警告：Windowsにより変更を拒否。"));
        }));
    }
    sy.opts.push_back(Live(L"startmenu", Tr(L"In the Start menu", L"スタートメニューに登録"), sys::StartMenuExists(), [](bool on) {
        std::wstring err;
        if (!sys::SetStartMenu(on, &err)) Notice(Tr(err.c_str(), L"警告：スタートメニューを変更できず。"));
    }));
    {
        // Locked: "Allow". Allowed but Windows hasn't restarted since: "Restart".
        bool pending = engine::WarmRestartPending();
        Option o = Action(
            L"warm", pending ? Tr(L"Restart to unlock warm colors", L"再起動して暖色域を有効化") : Tr(L"Allow warm colors", L"暖色域の許可"),
            [pending] { return pending ? Tr(L"Restart", L"再起動") : Tr(L"Allow", L"許可"); },
            [pending] {
                if (pending) sys::AskRestartForWarmColors(false);
                else sys::UnlockWarmColors();
                engine::Wake();
            });
        o.visible = [] { return engine::RegistryProblem(); };
        sy.opts.push_back(o);
    }
    {
        Option o = Live(L"pause", Tr(L"Pause", L"一時停止"), engine::Paused(), [](bool on) { engine::SetPaused(on); });
        o.on = [] { return engine::Paused(); };
        o.value = [] { return lang::OnOff(engine::Paused()); };
        sy.opts.push_back(o);
    }
    sy.opts.push_back(Action(L"file", Tr(L"Settings file", L"設定ファイル"), [] { return Tr(L"Open", L"開く"); }, [] {
        // Written first only if there's something to write. A file with a
        // mistake stays as it is, so it can be fixed.
        if (ConfigDirty() || GetFileAttributesW(ConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES) SaveConfigNow();
        sys::OpenFile(ConfigPath());
    }));
    sy.opts.push_back(Action(L"log", Tr(L"Log folder", L"ログフォルダー"), [] { return Tr(L"Open", L"開く"); },
                             [] { sys::OpenFolder(AppDir()); }));
    {
        Option o = Action(L"quit", Tr(L"Quit Candela mini", L"当機を終了"),
                          [] { return QuitArmed() ? Tr(L"Click again", L"再度クリック") : Tr(L"Quit", L"終了"); },
                          [] {
                              if (QuitArmed()) engine::Quit();
                              else g_quit_armed = NowMs();
                          });
        o.danger = true;
        sy.opts.push_back(o);
    }
    main.groups.push_back(sy);
    out.push_back(main);

    // ======== the hotkeys page ========
    Page hk{L"hotkeys", Tr(L"Hotkeys", L"ホットキー"), {}, L""};
    hk.note = Tr(L"Click a hotkey, then press the new keys. It needs Ctrl, Alt or Win. Esc cancels, Backspace removes it.",
                 L"報告：ホットキーをクリック後、新しいキーを入力。Ctrl・Alt・Winのいずれかが必須。Escで中止、Backspaceで削除。");
    auto hotkey = [](HotkeyId id) {
        for (auto& f : HotkeyFields())
            if (f.id == id) {
                Option o;
                o.key = L"hk" + std::to_wstring(f.id);
                o.label = HotkeyName(f.id);
                o.kind = kHotkey;
                o.hotkey = f.field;
                auto field = f.field;
                o.value = [field] { return lang::Keys(g_cfg.*field); };
                o.change = [](int) {};
                return o;
            }
        return Option();
    };
    Group warm{Tr(L"Warmth", L"色温度"), 0, {}};
    for (HotkeyId id : {HK_WARMER, HK_COOLER}) warm.opts.push_back(hotkey(id));
    warm.opts.push_back(Steps(L"step", Tr(L"Warmer / cooler step", L"色温度の変化幅"), {50, 100, 200, 250, 500, 1000}, &c.warmth_step_k,
                              [](int v) { return std::to_wstring(v) + L"K"; }));
    for (HotkeyId id : {HK_DIMMER, HK_BRIGHTER, HK_DARKROOM, HK_TOGGLE}) warm.opts.push_back(hotkey(id));
    hk.groups.push_back(warm);
    Group win{Tr(L"Windows", L"ウィンドウ"), 1, {}};
    for (HotkeyId id : {HK_BORDERLESS, HK_BLACKOUT}) win.opts.push_back(hotkey(id));
    win.opts.push_back(Action(L"hkdefault", Tr(L"Default hotkeys", L"初期設定に戻す"), [] { return Tr(L"Restore", L"復元"); }, [] {
        core::Config d;
        for (auto& f : HotkeyFields()) g_cfg.*(f.field) = d.*(f.field);
        Changed();
        Notice(Tr(L"Default hotkeys are back.", L"報告：ホットキーを初期設定に復元。"), false);
    }));
    hk.groups.push_back(win);
    out.push_back(hk);

    // Hidden options stay out of the lists entirely.
    for (auto& p : out)
        for (auto& g : p.groups)
            g.opts.erase(std::remove_if(g.opts.begin(), g.opts.end(), [](const Option& o) { return !o.visible(); }), g.opts.end());
    return out;
}

// ---- typing -------------------------------------------------------------------------------------------------
void BeginEdit(const Option& o) {
    StopRecording();
    g_edit.key = o.key;
    g_edit.buffer = o.text ? o.text() : L"";
}

bool EditChar(wchar_t c) {
    if (!g_edit.Active()) return false;
    if (c >= 0x20 && c != 0x7F && g_edit.buffer.size() < 40) g_edit.buffer += c;
    return true;
}

void EndEdit(const Option* o, bool keep) {
    if (!g_edit.Active()) return;
    if (keep && o && o->commit) {
        std::wstring err;
        if (!o->commit(g_edit.buffer, &err)) {
            Notice(err);
            g_edit.key.clear();
            return;
        }
    }
    g_edit.key.clear();
}

bool EditKey(UINT vk, const Option* o) {
    if (!g_edit.Active()) return false;
    if (vk == VK_RETURN || vk == VK_TAB) EndEdit(o, true);
    else if (vk == VK_ESCAPE) EndEdit(o, false);
    else if (vk == VK_BACK) {
        if (!g_edit.buffer.empty()) g_edit.buffer.pop_back();
    } else if (vk == 'V' && GetKeyState(VK_CONTROL) < 0) {
        if (OpenClipboard(nullptr)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = (const wchar_t*)GlobalLock(h)) {
                    g_edit.buffer = std::wstring(p).substr(0, 60);
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
    }
    return true;  // every key belongs to the field while typing
}

// ---- recording ---------------------------------------------------------------------------------------------------
void BeginRecording(const Option& o) {
    EndEdit(nullptr, false);
    g_recording = o.key;
    engine::SuspendHotkeys(true);  // so the old hotkeys don't swallow the keys
}

void StopRecording() {
    if (g_recording.empty()) return;
    g_recording.clear();
    engine::SuspendHotkeys(false);
}

bool RecordKey(UINT vk, const Option* o) {
    if (g_recording.empty()) return false;
    if (!o || !o->hotkey) {
        StopRecording();
        return true;
    }
    if (core::IsModifierVk(vk)) return true;  // wait for the real key
    auto down = [](int k) { return GetKeyState(k) < 0; };
    unsigned mods = (down(VK_CONTROL) ? core::kModCtrl : 0) | (down(VK_MENU) ? core::kModAlt : 0) |
                    (down(VK_SHIFT) ? core::kModShift : 0) | (down(VK_LWIN) || down(VK_RWIN) ? core::kModWin : 0);
    if (vk == VK_ESCAPE && !mods) {
        StopRecording();
        return true;
    }
    if (vk == VK_BACK && !mods) {
        g_cfg.*(o->hotkey) = L"";
        StopRecording();
        Changed();
        Notice(Tr(L"Hotkey removed.", L"報告：ホットキーを削除。"), false);
        return true;
    }
    std::wstring text = core::FormatHotkey(mods, vk), err;
    std::optional<core::Hotkey> parsed;
    if (!core::ParseHotkey(text, &parsed, &err)) {
        StopRecording();
        if (text.find(L"0x") != std::wstring::npos) Notice(Tr(L"That key can't be used for a hotkey.", L"警告：そのキーはホットキーに使用不可。"));
        else if (Japanese()) Notice(err.find(L"needs Ctrl") != std::wstring::npos ? L"警告：Ctrl・Alt・Winのいずれかが必要。"
                                                                                   : L"警告：そのキーの組み合わせは使用不可。");
        else Notice(err);
        return true;
    }
    for (auto& f : HotkeyFields()) {
        if (f.field == o->hotkey) continue;
        std::optional<core::Hotkey> other;
        std::wstring e;
        if (core::ParseHotkey(g_cfg.*(f.field), &other, &e) && other && other->mods == parsed->mods && other->vk == parsed->vk) {
            StopRecording();
            Notice(Japanese() ? L"警告：" + core::DisplayHotkey(text) + L" は「" + HotkeyName(f.id) + L"」に使用中。"
                              : core::DisplayHotkey(text) + L" is already used for " + f.label + L".");
            return true;
        }
    }
    g_cfg.*(o->hotkey) = text;
    StopRecording();
    Changed();
    return true;
}

}  // namespace opts
