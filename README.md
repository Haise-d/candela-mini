# Candela mini

### Candela, but simpler.

**Just the warmth.** The same sun-following schedule, the same Afterglow and the same colors that stay put as full Candela, without the extras. One small `candela-mini.exe` (about 0.7 MB), one settings page, and it never touches your keyboard or connects to the internet.

<p align="center">
  <a href="https://github.com/Haise-d/candela-mini/releases/latest"><b>Download the latest release</b></a> · Windows 10 and 11 (64-bit) · English and 日本語
</p>

**Pick mini** if all you want is a warm screen at night. **Pick [full Candela](https://github.com/Haise-d/candela)** if you also want its Alt+Tab switcher and volume and media popup.

> [!WARNING]
> **Out of the box, Windows won't let Candela mini go warmer than 3400K. Your screen won't get really orange until you do these 2 steps once.**
>
> 1. **Unlock.** The first time Candela mini starts, it asks. Click **Yes**, then **Yes** again when Windows asks.
>    Missed it? Right-click the sun icon in the tray and choose **Unlock warm colors**.
> 2. **Restart your PC.** Candela mini offers to do it for you. Or later: Start, then Power, then **Restart** (not Shut down, and not signing out: neither counts).
>
> **Step 2 is the one people skip.** Until you restart, Candela mini can't go warmer than about 3400K, or dim much below half: it puts on the closest Windows allows. It isn't broken, it's waiting for the restart, and it says **RESTART** at the bottom of its settings until you do.
>
> Already did this for full Candela or f.lux on this PC? Then you're done.

## Candela vs Candela mini

| | **[Candela](https://github.com/Haise-d/candela)** | **Candela mini** |
|:---|:---:|:---:|
| **The warmth** | | |
| Warmth that follows the sun (day, sunset, night) | ✅ | ✅ |
| Afterglow: the whole screen in red, for a dark room | ✅ | ✅ |
| Colors put back when a game or driver resets them | ✅ | ✅ |
| Normal colors back however it ends, even End task | ✅ | ✅ |
| Fades: changes come on gradually, with their own times for the schedule and hotkeys | ✅ | ✅ |
| **Hotkeys** | | |
| Warmer, cooler, dimmer, brighter, Afterglow, warmth off | ✅ | ✅ |
| Borderless window, black out the other screens | ✅ | ✅ |
| `--do` for scripts and macro keys | ✅ | ✅ |
| One click back to the default hotkeys | ❌ | ✅ |
| **The extras** | | |
| Alt+Tab switcher with live previews | ✅ | ❌ |
| Volume and media popup (song, artist, cover) | ✅ | ❌ |
| Popup when you press a hotkey | ✅ | ❌ *the tray icon shows it* |
| Game lists (no popups over games, anti-cheat) | ✅ | ❌ *not needed* |
| Glitch animations | ✅ | ❌ |
| Palettes and fonts in the settings | ✅ | ⚙️ *settings file only* |
| Candela's cursors | ✅ *with dark mode* | ✅ *cursors only* |
| **How it runs** | | |
| Settings | 8 pages | 1 page + hotkeys |
| Watches the keyboard | Yes, for Alt+Tab and media keys | **Never** |
| `.exe` size | ~1 MB | **~0.7 MB** |
| English and 日本語, Start with Windows, Start menu | ✅ | ✅ |

Without popups, the tray icon is how mini answers a hotkey: its sun takes the color on screen, and hovering it says what's on and until when. A beep means a hotkey couldn't do anything (paused, or a window that can't go borderless).

## Getting started

1. **Turn off Windows Night Light, quit f.lux, and quit full Candela** if it runs. Two programs changing the screen's colors fight each other, so mini won't start while full Candela is running.
2. **Unzip** the download somewhere permanent and keep the `theme` folder next to `candela-mini.exe`. `C:\Program Files\Candela mini` is the best place (see [Start with Windows](#start-with-windows)).
3. **Run `candela-mini.exe`.** It isn't code-signed, so Windows SmartScreen may warn about an unknown app: choose **More info → Run anyway**.
4. **Unlock warm colors, then restart.** Right away it asks to unlock warm colors: click **Yes**, then **Yes** again when Windows asks. Then let it restart your PC, or restart yourself later. Until you do, it stops at about 3400K (see [Warm colors](#warm-colors)).
5. **Set your town** (Latitude and Longitude). The settings open there, and open there again after the restart until it's set. In Google Maps, right-click your town and click the coordinates to copy them, then paste them into either field. If full Candela was set up on this PC, mini starts with its settings.

Candela mini lives in the tray, as a small sun tinted with the warmth on screen. **Click** it for the settings, **right-click** it for a quick menu. Since there are no popups, the icon's color and its tooltip are how a hotkey answers: hover it to see what's on screen and until when.

## The settings

Today's schedule runs across the top, each part in the color it puts on screen, with the switch times underneath; click a part to select its setting. Below are two pages:

| Page | What's there |
|---|---|
| **Settings** | Day and evening colors, sunset offset, night start, Afterglow or a color at night, when the day starts, your coordinates · blackout covers every window, black cursors · Afterglow now, its brightness, keep text readable, invert · language, Start with Windows, Start menu, allow warm colors, pause, the settings file and log, quit |
| **Hotkeys** | Every hotkey, how far warmer and cooler move · fade color changes, how long scheduled and hotkey fades take · default hotkeys |

Hover to select, click to change, right-click to go back. On a gauge, click where you want it; the mouse wheel and ‹ › step. Arrows and Enter work too, Tab switches page, Esc closes. Everything saves by itself.

## Hotkeys

| Default | What it does |
|---|---|
| Alt+Shift+PgDn | Warmer, 100K at a time (adjustable), down to 900K and then Afterglow |
| Alt+Shift+PgUp | Cooler |
| Alt+Shift+Del | Afterglow now, or leave it |
| Alt+Del | Warmth off (for a film or photo editing), or back on |
| Alt+PgDn / Alt+PgUp | Dimmer / brighter, 10% at a time, down to 10% (below about half only once warm colors are unlocked) |
| Ctrl+Alt+B | Make the window in front borderless and fill its screen, or put it back (a beep means it couldn't) |
| Ctrl+Alt+N | Black out every screen but the one you're using, or bring them back |

Warmer, cooler, Afterglow and warmth off last until the next scheduled change, then the schedule takes over again. Dimming lasts until the next morning. Each hotkey needs Ctrl, Alt or Win, so a plain key is never taken from your other programs.

## Good to know

### Warm colors

Windows refuses colors warmer than about 3400K, and dimming much below half, for every app, unless one registry value is set. It's the same value f.lux and full Candela set, so if either already did it on this PC, you're done. Until then Candela mini puts on the closest Windows allows, and the tray icon, its tooltip and the top of the settings show what's really on screen (for example *~3400K · limited*).

- **To set it:** say **Yes** when Candela mini asks at its first start, or right-click the tray icon and choose **Unlock warm colors**, or click **Allow warm colors** in the settings' **System** column. Windows asks for admin.
- **Then restart Windows.** Windows only picks the value up after a restart, so until the restart Candela mini stays at about 3400K however warm you set it. Candela mini offers to restart for you, and until you do, the settings say **RESTART** at the bottom (click it to restart), the row becomes **Restart to unlock warm colors**, and the tray icon's menu and tooltip say so too. Use **Restart**: Shut down and signing out don't count, and the reminder stays until a real restart. You only do this once per PC.
- **To check it worked:** after the restart, press **Alt+Shift+PgDn** a few times. If the screen keeps getting more orange below 3400K, it's working.

```
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ICM   GdiIcmGammaRange = 256 (DWORD)
```

Until the restart, a temporary key `HKLM\SOFTWARE\Candela warm colors pending` remembers that one is still needed. It's kept in memory only, so Windows removes it by itself at the restart.

### Fades

With **Hotkeys → Fade color changes** on, the screen moves to new colors gradually: scheduled changes take the **Scheduled fade** time (20 seconds unless you change it; up to an hour), and changes you make (hotkeys, the tray menu, the settings) take the **Hotkey fade** time (0.4 seconds; **Instant** turns it off for them). Pressing a hotkey again mid-fade carries on from wherever the screen is. Starting Candela mini, waking the PC and plugging in a screen put the colors on straight away.

While warm colors are still locked (see above), a fade stops where Windows does, at about 3400K. Inverted Afterglow is the one change that never fades: half way there the screen would be a flat red, so it switches at once.

### Start with Windows

**Start with Windows** registers a scheduled task named "Candela mini" that starts it when you sign in; Windows asks for admin once, when you turn it on or off. When `candela-mini.exe` sits in a folder only admins can change, such as `C:\Program Files\Candela mini`, the task starts it as admin, so **borderless** also works on admin windows. From any other folder it starts without admin rights, so another program can't swap the file for something else.

### Next to full Candela

Mini is its own app: its own settings folder, startup task, Start menu shortcut and tray icon, so both can be installed. They never run at the same time. On its first start mini copies full Candela's `config.json` if it finds one (your town, colors and hotkeys), then the two go their own ways.

If full Candela's **Cursors and dark mode** is on, turn it off there before switching, so your own cursors and light mode come back from full Candela's backup.

### Getting normal colors back

Press **Alt+Del**, choose **Pause**, or quit. Normal colors come back however Candela mini ends. You can also run `candela-mini.exe --reset`.

You'll see two `candela-mini.exe` in Task Manager. The second is a tiny guard that only waits; if mini is ended from outside, the guard puts normal colors back. **End process tree** ends both, and then the warmth stays until you start mini again or run `candela-mini.exe --reset`.

### Conflicts

Turn off Windows' color filters and Magnifier while you use Afterglow: to keep text readable, Afterglow uses the same part of Windows they do, and only one of them can use it at a time.

### Settings file and log

Both are in `%APPDATA%\Candela Mini`, and the settings window opens them.

- **`config.json`** holds every setting. Edits are picked up within a couple of seconds. If a hand edit has a mistake, mini keeps a copy as `config.json.bad` and carries on with the settings it had. A few rarely needed options live only here:
  - `force_reapply_ms`: for graphics drivers that reset the colors without mini noticing. Try `250` if you see flashes of full color.
  - `borderless_overhang_px`: for apps that still flicker when made borderless.
  - `dim_step`, `dim_min`: the dimming step and lowest level.
  - `palette` (`warm`, `grey` or `custom` with `color_paper`, `color_paper_dark`, `color_ink`, `color_ink_soft`, `color_shade` as `RRGGBB`), `font`, `font_ja`: the settings window's look. The cursors follow `grey` or `warm`.
  - Afterglow's settings use the key names `night_darkroom`, `darkroom_brightness`, `darkroom_by_brightness`, `darkroom_invert` and `hotkey_darkroom`.
- **`candela-mini.log`** notes every color reset mini fixed (and which app was in front), anything Windows refused, and crashes.

### Japanese

**Language · 言語** switches everything to Japanese. Fontworks' **Rodin** is used if it's installed (FOT-Rodin ProN in Adobe Fonts, for example); otherwise Yu Gothic UI.

## Command line

```
candela-mini.exe --settings     run it and open the settings
candela-mini.exe --schedule     show the next few days' switch times
candela-mini.exe --reset        put normal colors back and exit (closes a running Candela mini first)
candela-mini.exe --do <action>  act like a hotkey in the running Candela mini: warmer, cooler, off, afterglow,
                                dimmer, brighter, borderless or blackout (for scripts and macro keys)
```

## Uninstalling

Turn off **Start with Windows**, **In the Start menu** and **Black cursors**, quit Candela mini from the tray, then delete its folder and `%APPDATA%\Candela Mini`.

## Building from source

C++17 on the Win32 API, Direct2D and DirectWrite, with no third-party libraries. It builds with MinGW-w64 on Linux, WSL or MSYS2:

```
sudo apt install g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64   # Ubuntu / Debian / WSL
CXX=x86_64-w64-mingw32-g++-posix ./build.sh                             # writes out/candela-mini.exe
```

`build.sh` also runs the logic tests in `tests/` when a native `g++` is available. `candela-mini.exe --dump <folder> [--ja] [--grey]` saves the settings window as PNGs, for checking the look without a desktop; it runs under Wine too.

| Folder | Contents |
|---|---|
| `src/core/` | The logic, with no Windows code: sun times, colors, the schedule, hotkey text, the settings file (shared with full Candela) |
| `src/` | The app: the engine and tray (`engine.cpp`), screen colors (`display.cpp`), blackout and borderless (`windows.cpp`), the settings (`options.cpp`) and their window (`settings_window.cpp`) |
| `tests/` | Tests for `src/core/`, run in Madrid's time zone |
| `res/`, `theme/` | The app icon and the cursors |
| `tools/` | Scripts that draw the icon and the cursors (need Python and Pillow) |

## Credits

Made by **Haise**, coworked with **Claude** (Anthropic's AI assistant). Built from [Candela](https://github.com/Haise-d/candela).

Inspired by years of using [f.lux](https://justgetflux.com/). Candela isn't affiliated with it.

## License

MIT, see [LICENSE](LICENSE). `candela-mini.exe` includes parts of the MinGW-w64 runtime; their notices are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
