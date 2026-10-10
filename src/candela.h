// Candela mini for Windows: screen warmth on a schedule, Afterglow, blackout
// and borderless windows, with hotkeys. Candela without the Alt+Tab switcher,
// the media popup, the popups and the animations. Shared declarations.
//
// Everything runs on the main thread (one message loop: the engine and the
// settings window) except the gamma fast-watch (display.cpp).
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// _WIN32_WINNT / WINVER (Windows 10+) are set by build.sh.

#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <wincodec.h>
#include <shellapi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "core/core.h"

// Its own app: its own single-instance mutex, window class, settings folder,
// startup task and Start menu shortcut, so it can sit next to full Candela
// (but never runs at the same time: two programs setting the colors fight).
#define CANDELA_MUTEX L"Local\\CandelaMiniSingleInstance"
#define CANDELA_MAIN_CLASS L"CandelaMiniMain"
#define CANDELA_FULL_CLASS L"CandelaMain"     // full Candela's hidden window
#define CANDELA_NAME L"Candela mini"          // the task, the shortcut, message boxes
#define CANDELA_DIR L"Candela Mini"           // %APPDATA%\Candela Mini
#define CANDELA_FULL_DIR L"Candela"           // full Candela's, read once to copy its settings
#define CANDELA_VERSION L"1.1.0"

// Messages posted to the main window.
enum : UINT {
    WM_CANDELA_TRAY = WM_APP + 1,  // lParam: mouse message (WM_LBUTTONUP opens the settings)
    WM_CANDELA_OPEN_SETTINGS,      // candela-mini.exe started again: open the settings
    WM_CANDELA_REPAIRED,           // the fast watch put the colors back
    WM_CANDELA_DO,                 // wParam: a HotkeyId (candela-mini.exe --do ...)
    WM_CANDELA_QUIT,               // --reset: quit (WM_CLOSE can't reach a Candela running as admin)
};

// ---- app.cpp: settings file, log, helpers ----------------------------------------------------
extern core::Config g_cfg;
extern HINSTANCE g_inst;
extern HWND g_main;                    // the hidden main window (engine.cpp)
std::wstring AppDir();                 // %APPDATA%\Candela Mini
std::wstring ExePath();
std::wstring ExeDir();                 // the folder candela-mini.exe is in (theme\ lives there)
std::wstring ConfigPath();             // %APPDATA%\Candela Mini\config.json
bool LoadConfig(bool reload = false);  // reload: a hand edit with a mistake changes nothing
bool FirstRun();                       // no config.json existed at start-up
bool ImportedFromFull();               // ...and full Candela's settings were copied in
void SaveConfigSoon();                 // after a short pause (several quick clicks save once)
void SaveConfigNow();
void MarkConfigDirty();
bool ConfigDirty();                    // changed in Candela, not saved yet
bool ConfigFileChanged();              // config.json was edited by hand since we read or wrote it
void Log(const wchar_t* fmt, ...);
double NowMs();                        // high-resolution clock
extern double g_fake_now;              // >= 0: frozen clock, only for --dump
core::LocalTime NowLocal();
extern core::LocalTime g_fake_local;   // >= 0: frozen wall clock, only for --dump
float DpiScaleForMonitor(HMONITOR mon);
MONITORINFO MonitorInfoOf(HMONITOR mon);  // never left empty: the primary screen's size if Windows can't say
std::wstring ProcessName(HWND hwnd);   // "chrome.exe", "pid 123" or "?" (for the log)
std::wstring ProcessExeName(HWND hwnd);  // "chrome.exe", or "" if unknown

struct Color { float r, g, b, a; };
inline Color Hex(uint32_t v, float a = 1.f) {
    return {((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f, a};
}
inline Color FromTint(core::Rgb8 t) { return {t.r / 255.f, t.g / 255.f, t.b / 255.f, 1.f}; }
// The five colors everything is drawn in, from g_cfg.palette.
struct Palette { Color paper, paper_dark, ink, ink_soft, shade; };
extern Palette g_pal;
void ApplyPalette();                   // g_cfg.palette (and the custom colors) -> g_pal

// ---- lang.cpp: English and Japanese -------------------------------------------------------------
// Tr() picks the string for the current language, so each text sits next to
// its translation where it's used.
bool Japanese();
inline std::wstring Tr(const wchar_t* en, const wchar_t* ja) { return Japanese() ? ja : en; }
namespace lang {
std::wstring Phase(core::PhaseName p);             // "Evening" / 夕刻
std::wstring Kelvin(int kelvin);                   // "2000K", "Afterglow" / 残光
std::wstring Until(const core::Status& st);        // "until night 22:00" / 夜間 22:00 まで ("" when nothing's next)
std::wstring Summary(const core::Status& st);      // the tray tooltip's line
std::wstring Keys(const std::wstring& hotkey);     // "Alt+Shift+PgDn", "none" / なし
std::wstring OnOff(bool on);                       // "On" / ON
}  // namespace lang

// ---- system.cpp ---------------------------------------------------------------------------------
namespace sys {
// Start with Windows: a scheduled task that starts Candela mini at sign-in, as
// admin when it sits in a folder only admins can change (so borderless works
// on admin windows too). Windows asks for admin once when it's turned on or off.
bool StartupEnabled();            // cached; Refresh() asks Task Scheduler again
bool StartupPointsElsewhere();    // the task starts a candela-mini.exe in another folder
bool SetStartup(bool on);
void RefreshStartup();
bool StartMenuExists();           // the shortcut exists and points at this candela-mini.exe
bool SetStartMenu(bool on, std::wstring* error);
bool WarmColorsAllowed();         // GdiIcmGammaRange = 256
bool RequestWarmColors();         // asks for admin
// Windows only uses that setting after a restart; until then warm colors are
// still refused. The prompts below say so in plain words, and the settings,
// the tray tip and the tray menu keep saying it until the restart.
bool WarmColorsRestartPending();  // allowed, but Windows hasn't restarted since
bool WarmColorsWork();            // a warm color got through since start (whatever the registry says)
void NoteWarmColorWorked();       // the engine saw a warm color go through
void NoteWarmColorRefused();      // ...or refused while allowed
bool UnlockWarmColors();          // Allow (Windows asks for admin), then offers to restart. True if allowed.
bool AskRestartForWarmColors(bool just_allowed);  // "restart now?"; true if Windows is restarting
void OfferWarmColorsAtFirstStart();               // the first-start question, if warm colors are locked
// What needs admin, done in candela-mini.exe started again as admin
// (--admin <what> [arg]): startup-on <user>, startup-off, warm-colors.
// Returns the exit code (0: done).
int RunAdmin(const std::wstring& what, const std::wstring& arg);
bool IsElevated();
void EnableDarkMenus();
void OpenFolder(const std::wstring& path);
void OpenFile(const std::wstring& path);
}  // namespace sys

// theme.cpp: Candela's black cursors, with backup and exact undo.
namespace theme {
bool Apply();                     // false: the theme folder isn't next to candela-mini.exe
bool Restore();
bool Applied();
void Follow();                    // on, and the palette changed between grey and warm: the matching set
}  // namespace theme

// ---- display.cpp (gamma, colorfx) / windows.cpp (blackout, borderless) ------------------------
namespace gamma {
void RefreshDisplays();
// Windows refuses ramps far from normal colors until warm colors are
// unlocked (and Windows restarted). A display that refuses gets the closest
// ramp it does take ("limited"), and the whole one is retried now and then.
struct ApplyResult {
    int full = 0;                       // displays that took the whole ramp
    std::vector<std::wstring> limited;  // ...only part of the way: the closest one they take is on
    std::vector<std::wstring> refused;  // ...nothing, not even normal colors
    double t = 1;                       // the least of the way a limited display got (0: normal colors)
};
ApplyResult Apply(const core::Ramp& ramp);
struct State { int full = 0, limited = 0, refused = 0; double t = 1; };  // as above, right now
State GetState();
std::vector<std::wstring> CheckAndRepair(double force_every_s = 0);
bool AnyRefusing();  // a display refused the last ramp and hasn't taken it since (it's retried, slower each time)
void Close();
// A few seconds of very frequent checks on a thread of their own, with a
// precise timer, for the moments resets usually happen (leaving a
// fullscreen game). Repairs are reported to `notify` as WM_CANDELA_REPAIRED.
void FastWatch(double seconds, int interval_ms, HWND notify);
std::vector<std::wstring> TakeFastRepairs();  // displays the fast watch repaired since last asked
void StopFastWatch();
}  // namespace gamma

namespace colorfx {
bool Init();
bool Available();
bool Pending();  // Windows refused the last filter; it's retried until it takes
bool Set(const core::Matrix& m, bool identity);
bool CheckAndRepair();
void Close();
}  // namespace colorfx

namespace blackout {
bool Init();
bool Active();
int Toggle(HMONITOR keep, bool always_on_top);  // screens blacked out (0 = off or only one)
void Clear();
bool Owns(HWND h);
void OnForeground(HWND fg);                     // a window was switched to: it goes above the black
}  // namespace blackout

namespace borderless {
// Returns true if the window is now borderless, false if put back; throws
// nothing: on failure `error` is set (short, for the log).
bool Toggle(HWND hwnd, int overhang, bool* on, std::wstring* error);
void RestoreAll();
}  // namespace borderless

// ---- icons.cpp ------------------------------------------------------------------------------------
// The tray icon, drawn in code in the current warmth's tint.
HICON MakeTrayIcon(int size, int kelvin, bool disabled);

// ---- engine.cpp -------------------------------------------------------------------------------------
enum HotkeyId { HK_WARMER = 1, HK_COOLER, HK_TOGGLE, HK_DARKROOM, HK_DIMMER, HK_BRIGHTER, HK_BORDERLESS, HK_BLACKOUT };
struct HotkeyField { HotkeyId id; std::wstring core::Config::*field; const wchar_t* label; bool repeats; };
const std::vector<HotkeyField>& HotkeyFields();
std::wstring HotkeyName(HotkeyId id);  // the field's label, in the current language

namespace engine {
bool Init();
void Shutdown();
HWND Window();
const core::Status& GetStatus();
std::vector<std::wstring> Problems();  // for people, in order of importance
bool RegistryProblem();                 // warm colors locked, or waiting for a restart
bool WarmRestartPending();              // ...waiting for a restart (cached)
// Windows refused the colors asked for, so the closest it allows is on:
// roughly what that is. False when what's on screen is what was asked for.
bool Limited(int* kelvin, double* brightness);
void SkipWarmNotice();                  // before Init on a first start: its own question says it, not a notification too
bool ShuttingDown();                    // quitting: nothing may put colors back any more
void Wake();                            // settings changed: apply now
void Hotkey(HotkeyId id);
void SuspendHotkeys(bool on);           // the settings window is recording a new hotkey
bool BlackoutActive();
void ToggleBlackoutAtCursor();
void BackToSchedule();
void FullBrightness();
// Pause: normal colors until resumed.
bool Paused();
void SetPaused(bool on);
void StyleChanged();                    // language changed: the tray follows
void Quit();
}  // namespace engine

// ---- the settings window ---------------------------------------------------------------------------------------
namespace settings {
void Open();
void OpenOn(const std::wstring& key);  // a setting ("lat") to start on
bool IsOwnWindow(HWND h);
void Refresh();               // the status changed: redraw if open
void DebugDump(const std::wstring& dir);
void Shutdown();
}  // namespace settings
