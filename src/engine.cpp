// The part that runs all the time: the hidden main window, tray icon,
// hotkeys, timers, and the watchdog that puts the colors back when something
// resets them. Candela's engine without the keyboard hook, the popups and
// the game checks: hotkeys answer through the tray icon's tint and tooltip.
#include "candela.h"

#include <wtsapi32.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <deque>
#include <optional>
#include <set>

HWND g_main = nullptr;

const std::vector<HotkeyField>& HotkeyFields() {
    static const std::vector<HotkeyField> fields = {
        {HK_WARMER, &core::Config::hotkey_warmer, L"Warmer", true},
        {HK_COOLER, &core::Config::hotkey_cooler, L"Cooler", true},
        {HK_DIMMER, &core::Config::hotkey_dimmer, L"Dimmer", true},
        {HK_BRIGHTER, &core::Config::hotkey_brighter, L"Brighter", true},
        {HK_DARKROOM, &core::Config::hotkey_darkroom, L"Afterglow on / off", false},
        {HK_TOGGLE, &core::Config::hotkey_off, L"Warmth off / on", false},
        {HK_BORDERLESS, &core::Config::hotkey_borderless, L"Borderless window", false},
        {HK_BLACKOUT, &core::Config::hotkey_blackout, L"Black out other screens", false},
    };
    return fields;
}

std::wstring HotkeyName(HotkeyId id) {
    if (Japanese()) {
        switch (id) {
            case HK_WARMER: return L"暖色へ";
            case HK_COOLER: return L"寒色へ";
            case HK_DIMMER: return L"減光";
            case HK_BRIGHTER: return L"増光";
            case HK_DARKROOM: return L"残光 ON/OFF";
            case HK_TOGGLE: return L"色温度補正 ON/OFF";
            case HK_BORDERLESS: return L"ボーダーレス化";
            case HK_BLACKOUT: return L"他画面をブラックアウト";
        }
    }
    for (auto& f : HotkeyFields())
        if (f.id == id) return f.label;
    return L"";
}

namespace engine {

namespace {

enum : UINT_PTR { TIMER_TICK = 1, TIMER_BURST, TIMER_FORCE, TIMER_SAVE };
constexpr double kBurstSeconds = 3.0;
constexpr int kSurelyWarmK = 3000;  // a color this warm only gets through once warm colors are unlocked
enum : UINT {
    MENU_TOGGLE = 1, MENU_RESET, MENU_SETTINGS, MENU_LOG, MENU_QUIT, MENU_DARKROOM, MENU_FULL, MENU_BLACKOUT, MENU_PAUSE, MENU_WARM,
};

struct Applied {
    int kelvin;
    double brightness;
    std::wstring fx;
    bool darkroom;
    double dr_brightness;
    bool dr_invert;
    bool operator==(const Applied& o) const {
        return kelvin == o.kelvin && brightness == o.brightness && fx == o.fx && darkroom == o.darkroom &&
               (!darkroom || (dr_brightness == o.dr_brightness && dr_invert == o.dr_invert));
    }
};

core::Controller g_ctl;
core::Status g_status;
std::optional<Applied> g_applied;
std::wstring g_filter_on = L"identity";
double g_filter_strength = 0;
std::map<std::wstring, std::wstring> g_problems;
bool g_restart_pending = false;  // warm colors allowed, Windows not restarted since (system.cpp)
// Windows refused the colors asked for: the closest it allows is on (display.cpp).
struct Shown {
    bool limited = false;
    int kelvin = core::kNeutralK;  // roughly what's on screen
    double brightness = 1;
};
Shown g_shown;
core::Ramp g_want_ramp = core::IdentityRamp();  // the ramp asked for last
// Warm-color notifications already shown this run: each one once, not every
// time a warm color is refused again (each evening, each warmth on / off).
std::set<std::wstring> g_noticed;
std::deque<double> g_recent_resets;
bool g_warned_fight = false;
double g_reset_logged_at = -1e9;  // resets are logged at most once per 5 s
int g_reset_unlogged = 0;
double g_burst_until = 0;
std::wstring g_foreground = L"?";
HICON g_icon = nullptr;
int g_icon_kelvin = -1;
bool g_icon_disabled = false;
std::wstring g_tip;
std::map<int, std::wstring> g_registered;
bool g_suspended = false, g_shut = false, g_blackout_ok = false;
std::optional<std::wstring> g_last_failure;
UINT g_taskbar_created = 0;
std::vector<HWINEVENTHOOK> g_hooks;
int g_file_checks = 0;
bool g_paused = false;

double Seconds() { return NowMs() / 1000.0; }

std::wstring HotkeyText(HotkeyId id) {
    for (auto& f : HotkeyFields())
        if (f.id == id) return g_cfg.*(f.field);
    return L"";
}

std::wstring HotkeyLabel(HotkeyId id) {
    std::wstring t = HotkeyText(id);
    return t.empty() ? L"" : L"\t" + core::DisplayHotkey(t);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// ---- tray ---------------------------------------------------------------------------------------------
NOTIFYICONDATAW Nid(UINT flags) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_main;
    nid.uID = 1;
    nid.uFlags = flags;
    nid.uCallbackMessage = WM_CANDELA_TRAY;
    return nid;
}

// What Windows held the screen to, short: "~3400K", "~3700K, 70%" or "" (Afterglow).
std::wstring LimitText(const core::Status& st) {
    if (!g_shown.limited || st.kelvin <= core::kDarkroom) return L"";
    std::wstring s;
    if (g_shown.kelvin > st.kelvin + 50) s = L"~" + lang::Kelvin(g_shown.kelvin);
    if (g_shown.brightness > st.brightness + 0.02)
        s += (s.empty() ? L"~" : L", ") + std::to_wstring((int)std::nearbyint(g_shown.brightness * 100)) + L"%";
    return s;
}

// The tooltip holds 127 characters: the summary, then one line for what's wrong.
std::wstring TrayTip(const core::Status& st) {
    if (g_paused) return Tr(L"Candela mini (paused)", L"Candela mini（一時停止中）");
    std::wstring tip = L"Candela mini - " + lang::Summary(st);
    std::wstring held = LimitText(st);
    bool registry = g_problems.count(L"registry") > 0;
    if (g_shown.limited && registry) {
        if (held.empty())
            tip += L"\n" + (g_restart_pending ? Tr(L"Limited by Windows until you restart your PC", L"PC再起動まではWindowsが制限")
                                             : Tr(L"Limited by Windows: right-click to unlock", L"Windowsが制限中：右クリックで解除"));
        else if (g_restart_pending)
            tip += L"\n" + (Japanese() ? L"PC再起動まで" + held + L"で停止" : L"Held at " + held + L" until you restart your PC");
        else
            tip += L"\n" + (Japanese() ? L"Windowsが" + held + L"で制限中：右クリックで解除"
                                       : L"Windows limit: " + held + L". Right-click to unlock");
    } else if (registry) {
        tip += L"\n" + (g_restart_pending ? Tr(L"Restart your PC to unlock warm colors", L"PCを再起動して暖色域を有効化")
                                         : Tr(L"Warm colors locked: right-click to unlock", L"暖色域が制限中：右クリックで解除"));
    } else if (g_shown.limited) {
        tip += L"\n" + (held.empty()  ? Tr(L"Limited by Windows", L"Windowsが制限中")
                        : Japanese() ? L"Windowsが" + held + L"で制限中"
                                     : L"Windows holds it at " + held);
    }
    return tip;
}

// The icon takes the color that's really on screen.
int IconKelvin(const core::Status& st) {
    return g_shown.limited && !st.disabled && st.kelvin > core::kDarkroom ? g_shown.kelvin : st.kelvin;
}

void UpdateTray(const core::Status& st, bool force = false) {
    std::wstring tip = TrayTip(st);
    int kelvin = IconKelvin(st);
    bool icon_changed = force || kelvin != g_icon_kelvin || st.disabled != g_icon_disabled;
    if (!icon_changed && tip == g_tip) return;
    NOTIFYICONDATAW nid = Nid(NIF_TIP | NIF_MESSAGE);
    HICON fresh = nullptr;
    if (icon_changed) {
        int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
        if (!size) size = 16;
        fresh = MakeTrayIcon(size, kelvin, st.disabled);
        if (fresh) {
            nid.uFlags |= NIF_ICON;
            nid.hIcon = fresh;
        }
    }
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (fresh) {
        if (g_icon) DestroyIcon(g_icon);
        g_icon = fresh;
        g_icon_kelvin = kelvin;
        g_icon_disabled = st.disabled;
    }
    g_tip = tip;
}

void AddTrayIcon() {
    NOTIFYICONDATAW nid = Nid(NIF_MESSAGE | NIF_TIP);
    wcsncpy_s(nid.szTip, CANDELA_NAME, _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) Log(L"Taskbar not ready yet; the icon will appear when it is");
    g_tip.clear();
    UpdateTray(g_status, true);
}

// Windows' own notification, only for something that went wrong.
void Balloon(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW nid = Nid(NIF_INFO);
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, text.c_str(), _TRUNCATE);
    nid.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// ---- hotkeys --------------------------------------------------------------------------------------------
void RegisterHotkeys() {
    for (auto& f : HotkeyFields()) {
        UnregisterHotKey(g_main, f.id);
        g_problems.erase(L"hotkey" + std::to_wstring(f.id));
    }
    g_registered.clear();
    for (auto& f : HotkeyFields()) g_registered[f.id] = g_cfg.*(f.field);
    if (g_suspended) return;
    for (auto& f : HotkeyFields()) {
        std::wstring text = g_cfg.*(f.field), error;
        std::optional<core::Hotkey> hk;
        std::wstring key = L"hotkey" + std::to_wstring(f.id);
        if (!core::ParseHotkey(text, &hk, &error)) {
            g_problems[key] = error;
            continue;
        }
        if (!hk) continue;
        UINT mods = hk->mods | (f.repeats ? 0 : MOD_NOREPEAT);
        if (!RegisterHotKey(g_main, f.id, mods, hk->vk)) {
            g_problems[key] = Japanese() ? L"警告：" + core::DisplayHotkey(text) + L" は他のアプリが使用中。"
                                         : core::DisplayHotkey(text) + L" is taken by another program.";
            Log(L"Could not register hotkey %ls", text.c_str());
        }
    }
}

bool HotkeysChanged() {
    for (auto& f : HotkeyFields())
        if (g_registered[f.id] != g_cfg.*(f.field)) return true;
    return false;
}

// ---- the core loop --------------------------------------------------------------------------------------------
void SetTickTimer() {
    SetTimer(g_main, TIMER_TICK, std::max(20, g_cfg.watchdog_ms), nullptr);
    if (g_cfg.force_reapply_ms > 0) SetTimer(g_main, TIMER_FORCE, std::max(50, g_cfg.force_reapply_ms), nullptr);
    else KillTimer(g_main, TIMER_FORCE);
}

void StartBurst() {
    g_burst_until = Seconds() + kBurstSeconds;
    SetTimer(g_main, TIMER_BURST, std::max(10, g_cfg.burst_ms), nullptr);
    gamma::FastWatch(kBurstSeconds, g_cfg.burst_fast_ms, g_main);
}

// Warm colors: locked by Windows, or allowed but waiting for a restart. Either
// way it's the first thing the settings footer, the tray tip and the tray menu say.
void CheckRegistry() {
    std::wstring before = g_problems.count(L"registry") ? g_problems[L"registry"] : L"";
    g_restart_pending = sys::WarmColorsRestartPending();
    if (!sys::WarmColorsAllowed() && !sys::WarmColorsWork())
        g_problems[L"registry"] = Tr(L"Warm colors are locked by Windows: stuck at 3400K. Click here to unlock.",
                                     L"Windowsが暖色域を制限中：3400K未満は不可。ここをクリックして解除。");
    else if (g_restart_pending)
        g_problems[L"registry"] = Tr(L"Restart your PC to finish unlocking warm colors. Until then: stuck at 3400K.",
                                     L"PCを再起動して暖色域の解除を完了。再起動まで3400K未満は不可。");
    else
        g_problems.erase(L"registry");
    std::wstring after = g_problems.count(L"registry") ? g_problems[L"registry"] : L"";
    if (after != before) {
        g_tip.clear();
        settings::Refresh();
    }
}

std::wstring Join(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (auto& x : v) s += (s.empty() ? L"" : L", ") + x;
    return s;
}

void OnResetCaught(const std::vector<std::wstring>& names) {
    double now = Seconds();
    if (now - g_reset_logged_at >= 5) {
        if (g_reset_unlogged > 0)
            Log(L"Colors were reset %d more times in the last few seconds (not logged one by one)", g_reset_unlogged);
        Log(L"Colors were reset on %ls; restored (foreground: %ls)", Join(names).c_str(), g_foreground.c_str());
        g_reset_logged_at = now;
        g_reset_unlogged = 0;
    } else {
        g_reset_unlogged++;
    }
    StartBurst();
    g_recent_resets.push_back(now);
    while (!g_recent_resets.empty() && now - g_recent_resets.front() > 60) g_recent_resets.pop_front();
    if (g_recent_resets.size() > 40 && !g_warned_fight) {
        g_warned_fight = true;
        Balloon(Tr(L"Something keeps resetting the screen colors", L"警告：画面の色が繰り返しリセットされている"),
                Tr(L"Candela mini keeps restoring them. If f.lux, Night Light or full Candela is on, turn it off; otherwise "
                   L"the log shows which app was in front.",
                   L"報告：当機が復元を継続中。推奨：f.lux、夜間モード、Candelaが有効なら無効化。原因のアプリはログで確認可能。"));
    }
}

// Roughly what's on screen, from display.cpp's state: the ramp asked for, or
// the part of the way to it that Windows allowed. True if that changed.
bool UpdateShown() {
    gamma::State s = gamma::GetState();
    Shown now;
    if (s.limited > 0) {
        core::Ramp ramp = core::Blend(core::IdentityRamp(), g_want_ramp, s.t);
        now.limited = true;
        now.kelvin = core::ApproxKelvin(ramp);
        now.brightness = core::ApproxBrightness(ramp);
    }
    bool changed = now.limited != g_shown.limited || now.kelvin != g_shown.kelvin ||
                   std::fabs(now.brightness - g_shown.brightness) > 0.005;
    g_shown = now;
    return changed;
}

// Proof that warm colors are unlocked: a display took, in full, a color well
// below the ~3400K Windows allows by default. Not counted when no display took
// anything (none attached, a screen switched off, quitting).
void NoteIfWarmWorked(const core::Status& st, int displays_full) {
    if (displays_full > 0 && !st.disabled && st.kelvin <= kSurelyWarmK && !sys::WarmColorsWork()) {
        sys::NoteWarmColorWorked();
        Log(L"Warm colors work");
        CheckRegistry();
    }
}

// Windows refused a change, and a later retry (display.cpp keeps trying,
// slower each time) went through: the warning goes.
void ClearAcceptedRefusals() {
    bool changed = false;
    bool was_limited = g_shown.limited;
    if (UpdateShown()) {
        g_tip.clear();
        changed = true;
        if (was_limited && !g_shown.limited) {
            g_problems.erase(L"limited");
            Log(L"Windows took the whole ramp after all");
            gamma::State s = gamma::GetState();
            if (s.limited == 0) NoteIfWarmWorked(g_status, s.full);
        }
    }
    if (g_problems.count(L"blocked") && !gamma::AnyRefusing()) {
        g_problems.erase(L"blocked");
        g_last_failure.reset();
        Log(L"Windows accepted the colors after all");
        changed = true;
    }
    if (g_problems.count(L"colorfx-refused") && colorfx::Available() && !colorfx::Pending()) {
        g_problems.erase(L"colorfx-refused");
        changed = true;
    }
    if (changed) settings::Refresh();
}

void CheckAll() {
    auto repaired = gamma::CheckAndRepair();
    if (colorfx::CheckAndRepair()) repaired.push_back(L"color filter");
    if (!repaired.empty()) OnResetCaught(repaired);
    ClearAcceptedRefusals();
}

// Windows didn't take the ramp everywhere. Limited (it took only part of the
// way): its range limit, so warm colors are locked or Windows hasn't restarted
// since they were unlocked. Refused (not even normal colors): something else.
// Each warm-color notification is shown once per run; the footer, the tray
// tip and the tray menu keep saying it.
void OnApplyFailed(const gamma::ApplyResult& res) {
    if (!res.limited.empty()) {
        bool allowed = sys::WarmColorsAllowed();
        // Held back although it's allowed: most likely Windows hasn't restarted
        // since it was (system.cpp decides).
        if (allowed) sys::NoteWarmColorRefused();
        CheckRegistry();
        std::wstring msg, key;
        if (!allowed) {
            msg = Tr(L"Windows is blocking warm colors (and deep dimming), so Candela mini put on the closest it allows. "
                     L"Right-click the tray icon, choose Unlock warm colors, then restart your PC.",
                     L"報告：Windowsが暖色域（と深い減光）を制限中。当機は許可範囲内で最も近い色を適用。"
                     L"推奨：トレイアイコンを右クリックし「暖色域を解除」を実行後、PCを再起動。");
            key = L"blocked-registry";
        } else if (g_restart_pending) {
            msg = Tr(L"Windows is still limiting the colors: it hasn't restarted since warm colors were unlocked. Restart your PC.",
                     L"報告：暖色域の解除後に未再起動のため、Windowsが制限を継続中。推奨：PCを再起動。");
            key = L"blocked-restart";
        } else {
            msg = Japanese() ? L"報告：" + Join(res.limited) + L" でWindowsが色を制限。当機は許可範囲内で最も近い色を適用。"
                             : L"Windows limited the colors on " + Join(res.limited) + L", so Candela mini put on the closest it allows.";
            g_problems[L"limited"] = msg;
            key = L"limited";
        }
        if (!g_noticed.count(key)) {
            g_noticed.insert(key);
            Log(L"%ls", msg.c_str());
            Balloon(Tr(L"Candela mini couldn't change the colors fully", L"警告：画面の色を完全には変更できず"), msg);
        }
    }
    if (!res.refused.empty()) {
        std::wstring msg = Japanese() ? L"報告：" + Join(res.refused) + L" で色の変更が拒否された。"
                                      : L"Windows refused the color change on " + Join(res.refused) + L".";
        g_problems[L"blocked"] = msg;
        if (g_last_failure != msg) {
            g_last_failure = msg;
            Log(L"%ls", msg.c_str());
            Balloon(Tr(L"Candela mini couldn't change the colors", L"警告：画面の色を変更できず"), msg);
        }
    }
}

core::Status CurrentStatus() {
    core::Status st = g_ctl.Get(NowLocal(), g_cfg);
    if (g_paused) {  // normal colors until resumed, shown as "off"
        st.disabled = true;
        st.kelvin = core::kNeutralK;
        st.brightness = 1;
        st.overridden = false;
    }
    return st;
}

void Tick(bool force = false) {
    if (g_shut) return;  // quitting: normal colors stay
    core::Status st = CurrentStatus();
    g_status = st;
    bool darkroom = st.kelvin <= core::kDarkroom;
    core::Effect fx = core::EffectFor(st.kelvin, g_cfg.darkroom_by_brightness);
    Applied want{st.kelvin, st.brightness, fx.key, darkroom, g_cfg.darkroom_brightness, g_cfg.darkroom_invert};
    if (force || !g_applied || !(want == *g_applied)) {
        core::Ramp ramp = core::BuildRamp(st.kelvin, g_cfg.darkroom_brightness, g_cfg.darkroom_invert, st.brightness);
        // Forced (display change, wake-up): put the filter back only if it's in
        // use. Re-sending "no filter" makes Windows redraw the screen for
        // nothing, which shows as a flash right when a game leaves fullscreen.
        bool fx_changes = fx.key != g_filter_on || (force && fx.key != L"identity");
        // Going warmer: filter first, then the ramp. Going cooler: the reverse.
        // Either way no frame in between drops green or blue text.
        bool filter_first = fx.strength >= g_filter_strength;
        bool fx_ok = true;
        if (fx_changes && filter_first) fx_ok = colorfx::Set(fx.matrix, fx.key == L"identity");
        auto res = gamma::Apply(ramp);
        g_want_ramp = ramp;
        if (fx_changes && !filter_first) fx_ok = colorfx::Set(fx.matrix, fx.key == L"identity");
        g_filter_on = fx.key;
        g_filter_strength = fx.strength;
        if (fx.key != L"identity" && !colorfx::Available())
            g_problems[L"colorfx"] = Tr(L"Windows' color filter isn't available, so Afterglow is red-only.",
                                        L"警告：Windowsのカラーフィルターが使用不可。残光は赤チャンネルのみ。");
        else if (!fx_ok)  // the color filter keeps trying again by itself, slowly (cleared in ClearAcceptedRefusals)
            g_problems[L"colorfx-refused"] = Tr(L"Windows refused the color filter (is Magnifier or a color filter on?).",
                                                L"警告：Windowsがカラーフィルターを拒否（拡大鏡やカラーフィルターが有効？）。");
        else if (fx_changes) {
            g_problems.erase(L"colorfx");
            g_problems.erase(L"colorfx-refused");
        }
        g_applied = want;
        Log(L"Applied %ls (%ls)", core::Describe(st.kelvin).c_str(), st.Summary().c_str());
        if (UpdateShown()) g_tip.clear();
        if (g_shown.limited)
            Log(L"Windows allowed only part of it: about %ls at %d%%", core::Describe(g_shown.kelvin).c_str(),
                (int)std::nearbyint(g_shown.brightness * 100));
        if (res.limited.empty()) g_problems.erase(L"limited");
        if (res.refused.empty()) {
            g_problems.erase(L"blocked");
            g_last_failure.reset();
        }
        if (!res.limited.empty() || !res.refused.empty()) OnApplyFailed(res);
        // Well below the ~3400K Windows allows by default, and a display took
        // it: warm colors are on, whatever else says otherwise.
        if (res.limited.empty()) NoteIfWarmWorked(st, res.full);
    } else {
        CheckAll();
    }
    UpdateTray(st);
}

void ToggleBlackout() {
    if (!g_blackout_ok) return;
    blackout::Toggle(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), g_cfg.blackout_always_on_top);
}

void ToggleBorderless() {
    HWND fg = GetForegroundWindow();
    bool on = false;
    std::wstring error;
    if (!borderless::Toggle(fg, g_cfg.borderless_overhang_px, &on, &error)) {
        Log(L"Borderless not possible: %ls", error.c_str());
        MessageBeep(MB_ICONWARNING);  // no popup in mini: a beep says it didn't happen
        return;
    }
    StartBurst();  // watch closely in case the switch resets the colors
}

// ---- the menu --------------------------------------------------------------------------------------------
void ShowMenu() {
    const core::Status& st = g_status;
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, (g_paused ? Tr(L"Paused", L"一時停止中") : lang::Summary(st)).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // Warm colors locked, or waiting for a restart: the fix, right at the top.
    if (g_problems.count(L"registry")) {
        std::wstring fix = g_restart_pending ? Tr(L"Restart to unlock warm colors...", L"再起動して暖色域を有効化...")
                                             : Tr(L"Unlock warm colors...", L"暖色域を解除...");
        AppendMenuW(m, MF_STRING, MENU_WARM, fix.c_str());
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    UINT warmth = g_paused ? MF_GRAYED : 0;
    std::wstring label;
    if (st.disabled) label = Tr(L"Turn warmth back on", L"色温度補正を再開") + HotkeyLabel(HK_TOGGLE);
    else if (st.next && Japanese())
        label = lang::Phase(st.next->name) + L"（" + core::FormatHM(st.next->start) + L"）まで停止" + HotkeyLabel(HK_TOGGLE);
    else if (st.next)
        label = L"Warmth off until " + Lower(core::Label(st.next->name)) + L" (" + core::FormatHM(st.next->start) + L")" +
                HotkeyLabel(HK_TOGGLE);
    else label = Tr(L"Warmth off", L"色温度補正を停止") + HotkeyLabel(HK_TOGGLE);
    AppendMenuW(m, MF_STRING | warmth, MENU_TOGGLE, label.c_str());
    AppendMenuW(m, MF_STRING | warmth | (st.Darkroom() ? MF_CHECKED : 0), MENU_DARKROOM,
                (Tr(L"Afterglow", L"残光") + HotkeyLabel(HK_DARKROOM)).c_str());
    if (g_blackout_ok)
        AppendMenuW(m, MF_STRING | (blackout::Active() ? MF_CHECKED : 0), MENU_BLACKOUT,
                    (Tr(L"Black out other screens", L"他画面をブラックアウト") + HotkeyLabel(HK_BLACKOUT)).c_str());
    if (st.brightness < 0.999)
        AppendMenuW(m, MF_STRING, MENU_FULL,
                    (Tr(L"Full brightness (now ", L"輝度を最大に（現在 ") + std::to_wstring((int)std::nearbyint(st.brightness * 100)) +
                     Tr(L"%)", L"%）"))
                        .c_str());
    if (st.overridden) AppendMenuW(m, MF_STRING, MENU_RESET, Tr(L"Back to schedule", L"スケジュールに戻す").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_paused ? MF_CHECKED : 0), MENU_PAUSE, Tr(L"Pause", L"一時停止").c_str());
    AppendMenuW(m, MF_STRING | MF_DEFAULT, MENU_SETTINGS, Tr(L"Settings...", L"設定...").c_str());
    AppendMenuW(m, MF_STRING, MENU_LOG, Tr(L"Open log folder", L"ログフォルダーを開く").c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, MENU_QUIT, Tr(L"Quit", L"終了").c_str());
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);  // without this the menu doesn't close when you click elsewhere
    UINT cmd = TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, g_main, nullptr);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case MENU_TOGGLE: Hotkey(HK_TOGGLE); break;
        case MENU_DARKROOM: Hotkey(HK_DARKROOM); break;
        case MENU_BLACKOUT: ToggleBlackoutAtCursor(); break;
        case MENU_FULL: FullBrightness(); break;
        case MENU_RESET: BackToSchedule(); break;
        case MENU_PAUSE: SetPaused(!g_paused); break;
        case MENU_WARM:
            if (g_restart_pending) sys::AskRestartForWarmColors(false);
            else sys::UnlockWarmColors();
            Wake();
            break;
        case MENU_SETTINGS: settings::Open(); break;
        case MENU_LOG: sys::OpenFolder(AppDir()); break;
        case MENU_QUIT: Quit(); break;
    }
    if (cmd && cmd != MENU_QUIT) settings::Refresh();
}

// ---- window -------------------------------------------------------------------------------------------------
void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD) {
    // First the colors: this is the moment a game leaving fullscreen resets them.
    CheckAll();
    StartBurst();  // resets often land a few frames after a window switch
    HWND fg = GetForegroundWindow();
    g_foreground = ProcessName(fg);
    if (event == EVENT_SYSTEM_FOREGROUND) blackout::OnForeground(fg);
}

void DoShutdown() {
    if (g_shut) return;
    g_shut = true;
    KillTimer(g_main, TIMER_SAVE);
    if (ConfigDirty()) SaveConfigNow();  // only what you changed; hand edits are never overwritten
    gamma::StopFastWatch();
    colorfx::Close();
    gamma::Apply(core::IdentityRamp());
    gamma::Close();
    Log(L"Restored normal colors");
    blackout::Clear();
    borderless::RestoreAll();
    for (auto h : g_hooks) UnhookWinEvent(h);
    g_hooks.clear();
    for (auto& f : HotkeyFields()) UnregisterHotKey(g_main, f.id);
    NOTIFYICONDATAW nid = Nid(0);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_icon) DestroyIcon(g_icon);
    g_icon = nullptr;
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            // After WM_ENDSESSION the window lives on until Windows ends the
            // process; nothing may put colors back then.
            if (g_shut) return 0;
            if (wp == TIMER_SAVE) {
                KillTimer(h, TIMER_SAVE);
                SaveConfigNow();
                return 0;
            }
            if (wp == TIMER_BURST && Seconds() > g_burst_until) KillTimer(h, TIMER_BURST);
            if (wp == TIMER_FORCE) {
                gamma::CheckAndRepair(0.001);
                return 0;
            }
            if (wp == TIMER_TICK && g_file_checks % 100 == 99 && ConfigDirty()) SaveConfigNow();  // a save that failed: try again
            // Every 10 s or so: warm colors may have been allowed meanwhile (an
            // admin question answered late, f.lux, regedit).
            if (wp == TIMER_TICK && g_file_checks % 100 == 49) CheckRegistry();
            if (wp == TIMER_TICK && ++g_file_checks % 20 == 0 && !ConfigDirty() && ConfigFileChanged()) {
                // config.json was edited by hand (Settings file -> Open): use it.
                auto look = [] {
                    return g_cfg.palette + L"|" + g_cfg.language + L"|" + g_cfg.font + L"|" + g_cfg.font_ja + L"|" +
                           g_cfg.color_paper + g_cfg.color_paper_dark + g_cfg.color_ink + g_cfg.color_ink_soft + g_cfg.color_shade;
                };
                std::wstring looked = look();
                if (LoadConfig(true)) {  // false: a mistake or unreadable; the settings stay as they were
                    Log(L"config.json changed; reloaded it");
                    SetTickTimer();
                    if (HotkeysChanged()) RegisterHotkeys();
                    if (look() != looked) StyleChanged();
                    settings::Refresh();
                }
            }
            Tick();
            return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION:
            // Signing out or shutting down: Windows ends Candela without WM_CLOSE.
            if (wp) {
                DoShutdown();
                // Asked to close for an update or install (Restart Manager), not a
                // sign-out: exit for real rather than linger with everything off.
                if (lp & ENDSESSION_CLOSEAPP) PostMessageW(h, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_CANDELA_QUIT: Quit(); return 0;
        case WM_HOTKEY: Hotkey((HotkeyId)wp); return 0;
        case WM_CANDELA_DO: Hotkey((HotkeyId)wp); return 0;
        case WM_CANDELA_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP) settings::Open();
            else if (LOWORD(lp) == WM_RBUTTONUP) ShowMenu();
            return 0;
        case WM_CANDELA_OPEN_SETTINGS: settings::Open(); return 0;
        case WM_CANDELA_REPAIRED: {
            auto names = gamma::TakeFastRepairs();
            if (!names.empty()) OnResetCaught(names);
            return 0;
        }
        case WM_TIMECHANGE:  // the clock or the time zone was changed: the day's sun times may be different now
            g_ctl.ForgetTimeline();
            Tick();
            settings::Refresh();
            return 0;
        case WM_DISPLAYCHANGE:
            Log(L"Display configuration changed");
            blackout::Clear();  // screens moved; don't leave black windows in odd places
            gamma::RefreshDisplays();
            Tick(true);
            StartBurst();
            return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
                Log(L"Resumed from sleep");
                gamma::RefreshDisplays();
                Tick(true);
                StartBurst();
            }
            return TRUE;
        case WM_WTSSESSION_CHANGE:
            // Unlocking, or coming back to this session after switching users or
            // a remote desktop: the colors may have been reset meanwhile.
            if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT || wp == WTS_REMOTE_CONNECT) {
                Tick(true);
                StartBurst();
            }
            return 0;
        case WM_CLOSE:
            DoShutdown();  // while the window still exists, so the tray icon goes away
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    if (msg == g_taskbar_created && msg) {  // Explorer restarted
        AddTrayIcon();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool Init() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = g_inst;
    wc.lpszClassName = CANDELA_MAIN_CLASS;
    if (!RegisterClassExW(&wc)) return false;
    // A real (never shown) top-level window, not message-only: those don't get
    // display-change, power or "taskbar restarted" broadcasts.
    g_main = CreateWindowExW(WS_EX_TOOLWINDOW, CANDELA_MAIN_CLASS, CANDELA_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    if (!g_main) return false;
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    // Candela mini may run as admin (Start with Windows): let a normal one
    // (the Start menu shortcut, --do) and Explorer reach it.
    for (UINT m : {g_taskbar_created, (UINT)WM_CANDELA_TRAY, (UINT)WM_CANDELA_OPEN_SETTINGS, (UINT)WM_CANDELA_DO,
                   (UINT)WM_CANDELA_QUIT})
        ChangeWindowMessageFilterEx(g_main, m, MSGFLT_ALLOW, nullptr);
    gamma::RefreshDisplays();
    colorfx::Init();
    g_blackout_ok = blackout::Init();
    RegisterHotkeys();
    // A game leaving fullscreen usually minimizes or loses the foreground: both
    // are when the driver resets the colors.
    for (DWORD ev : {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND})
        if (HWINEVENTHOOK hook = SetWinEventHook(ev, ev, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS))
            g_hooks.push_back(hook);
    WTSRegisterSessionNotification(g_main, NOTIFY_FOR_THIS_SESSION);
    g_status = CurrentStatus();
    AddTrayIcon();
    CheckRegistry();
    Tick(true);
    SetTickTimer();
    return true;
}

void Shutdown() { DoShutdown(); }

HWND Window() { return g_main; }
const core::Status& GetStatus() { return g_status; }

std::vector<std::wstring> Problems() {
    std::vector<std::wstring> out;
    if (g_problems.count(L"registry")) out.push_back(g_problems[L"registry"]);
    for (auto& [key, text] : g_problems)
        if (key != L"registry") out.push_back(text);
    return out;
}
bool RegistryProblem() { return g_problems.count(L"registry") > 0; }
bool WarmRestartPending() { return g_restart_pending; }

bool Limited(int* kelvin, double* brightness) {
    if (!g_shown.limited || g_paused) return false;
    if (kelvin) *kelvin = g_status.kelvin <= core::kDarkroom ? g_status.kelvin : g_shown.kelvin;
    if (brightness) *brightness = g_shown.brightness;
    return true;
}

void SkipWarmNotice() { g_noticed.insert(L"blocked-registry"); }
bool ShuttingDown() { return g_shut; }

void Wake() {
    if (g_shut) return;  // quit while a question was up: nothing more to apply
    SetTickTimer();
    CheckRegistry();
    if (HotkeysChanged()) RegisterHotkeys();
    Tick();
    SaveConfigSoon();
}

void Hotkey(HotkeyId id) {
    core::LocalTime now = NowLocal();
    if (id == HK_BORDERLESS) {
        ToggleBorderless();
        return;
    }
    if (id == HK_BLACKOUT) {
        ToggleBlackout();
        settings::Refresh();
        return;
    }
    if (g_paused) {  // warmth stays off until resumed
        MessageBeep(MB_ICONWARNING);
        return;
    }
    switch (id) {
        case HK_WARMER: g_ctl.Nudge(now, g_cfg, true); break;
        case HK_COOLER: g_ctl.Nudge(now, g_cfg, false); break;
        case HK_TOGGLE: g_ctl.ToggleDisabled(now, g_cfg); break;
        case HK_DARKROOM: g_ctl.ToggleDarkroom(now, g_cfg); break;
        case HK_DIMMER: g_ctl.Dim(now, g_cfg, true); break;
        case HK_BRIGHTER: g_ctl.Dim(now, g_cfg, false); break;
        default: break;
    }
    Tick();
    settings::Refresh();
}

void SuspendHotkeys(bool on) {
    g_suspended = on;
    RegisterHotkeys();
}

bool BlackoutActive() { return blackout::Active(); }

void ToggleBlackoutAtCursor() {
    if (!g_blackout_ok) return;
    if (blackout::Active()) {
        blackout::Clear();
        return;
    }
    // The menu or settings took focus, so "the screen you're using" = where the pointer is.
    POINT pt;
    GetCursorPos(&pt);
    blackout::Toggle(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), g_cfg.blackout_always_on_top);
}

void BackToSchedule() {
    g_ctl.Reset();
    Tick();
    settings::Refresh();
}

void FullBrightness() {
    g_ctl.FullBrightness();
    Tick();
    settings::Refresh();
}

bool Paused() { return g_paused; }

void SetPaused(bool on) {
    if (g_paused == on) return;
    g_paused = on;
    Log(on ? L"Paused" : L"Resumed");
    g_tip.clear();
    Tick();
    settings::Refresh();
}

void StyleChanged() {
    theme::Follow();  // grey or warm cursors, with the palette
    CheckRegistry();  // its text is in the language
    g_tip.clear();
    UpdateTray(g_status, true);
    RegisterHotkeys();  // its problem messages are in the language
}

void Quit() { PostMessageW(g_main, WM_CLOSE, 0, 0); }

}  // namespace engine

void SaveConfigSoon() {
    MarkConfigDirty();
    if (HWND h = engine::Window()) SetTimer(h, 4 /* TIMER_SAVE */, 500, nullptr);
    else SaveConfigNow();
}
