#include "render.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <unordered_map>

namespace gfx {

ID2D1Factory* d2d = nullptr;
IDWriteFactory* dwrite = nullptr;
IWICImagingFactory* wic = nullptr;

template <class T> static void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

bool Init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)&d2d)))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dwrite)))
        return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory),
                                (void**)&wic)))
        wic = nullptr;  // only screenshots (--dump) need it
    WarmFonts();
    return true;
}

struct CachedFormat {
    std::wstring font;
    float size;
    DWRITE_FONT_WEIGHT weight;
    bool ja;
    IDWriteTextFormat* fmt;
    // The "..." for text cut short, in this format's font and size (one shared
    // sign would draw every cut in the size of whichever text came first).
    IDWriteInlineObject* ellipsis;
};
static std::vector<CachedFormat> g_formats;

static void ClearFormats() {
    for (auto& f : g_formats) {
        SafeRelease(f.fmt);
        SafeRelease(f.ellipsis);
    }
    g_formats.clear();
}
// Layouts of text that is drawn again and again unchanged (most of it, every frame).
static std::unordered_map<std::wstring, IDWriteTextLayout*> g_layouts;

static void ClearLayouts() {
    for (auto& [key, l] : g_layouts) l->Release();
    g_layouts.clear();
}

void Shutdown() {
    ClearLayouts();
    ClearFormats();
    SafeRelease(wic);
    SafeRelease(dwrite);
    SafeRelease(d2d);
}

// ---- Surface ------------------------------------------------------------------------------
bool Surface::Create(const wchar_t* cls, WNDPROC proc, void* user, DWORD ex_extra, HWND owner, bool overlay) {
    overlay_ = overlay;
    WNDCLASSEXW wc = {sizeof(wc)};
    if (!GetClassInfoExW(g_inst, cls, &wc)) {
        wc = {sizeof(wc)};
        wc.lpfnWndProc = proc;
        wc.hInstance = g_inst;
        wc.lpszClassName = cls;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) return false;
    }
    DWORD ex = overlay ? WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | ex_extra
                       : WS_EX_LAYERED | WS_EX_APPWINDOW | ex_extra;
    DWORD style = overlay ? WS_POPUP : WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX;
    hwnd = CreateWindowExW(ex, cls, L"Candela", style, 0, 0, 1, 1, owner, nullptr, g_inst, nullptr);
    if (!hwnd) return false;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)user);
    return true;
}

bool Surface::Resize(int nw, int nh) {
    nw = std::max(1, nw);
    nh = std::max(1, nh);
    if (nw == w && nh == h && dc_) return true;
    if (dc_) {
        SelectObject(dc_, old_);
        DeleteObject(bmp_);
        DeleteDC(dc_);
        dc_ = nullptr;
        bmp_ = nullptr;
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = nw;
    bi.bmiHeader.biHeight = -nh;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    dc_ = CreateCompatibleDC(screen);
    bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bits_ = bits;
    ReleaseDC(nullptr, screen);
    if (!dc_ || !bmp_) {  // half made: don't keep what did get made
        if (bmp_) DeleteObject(bmp_);
        if (dc_) DeleteDC(dc_);
        dc_ = nullptr;
        bmp_ = nullptr;
        bits_ = nullptr;
        w = h = 0;
        return false;
    }
    old_ = (HBITMAP)SelectObject(dc_, bmp_);
    w = nw;
    h = nh;
    return true;
}

bool Surface::EnsureTarget() {
    if (rt_) return true;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (FAILED(d2d->CreateDCRenderTarget(&props, &rt_))) {
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;  // no GPU (e.g. remote desktop)
        if (FAILED(d2d->CreateDCRenderTarget(&props, &rt_))) return false;
    }
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return true;
}

void Surface::ReleaseTarget() {
    if (rt_) generation++;
    SafeRelease(brush_);
    SafeRelease(rt_);
}

ID2D1RenderTarget* Surface::Begin() {
    if (!dc_ || !EnsureTarget()) return nullptr;
    RECT r = {0, 0, w, h};
    if (FAILED(rt_->BindDC(dc_, &r))) return nullptr;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    return rt_;
}

void Surface::End(int x, int y, float alpha) {
    if (!rt_) return;
    HRESULT hr = rt_->EndDraw();
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        ReleaseTarget();  // the graphics driver restarted; build it again next frame
        return;
    }
    GdiFlush();
    x_ = x;
    y_ = y;
    SetAlpha(alpha);
}

void Surface::SetAlpha(float alpha) {
    if (!dc_) return;
    POINT dst = {x_, y_}, src = {0, 0};
    SIZE size = {w, h};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, (BYTE)std::clamp((int)std::lround(alpha * 255), 0, 255), AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, nullptr, &dst, &size, dc_, &src, 0, &blend, ULW_ALPHA);
    if (!visible_) {
        if (overlay_)
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        else
            ShowWindow(hwnd, SW_SHOW);
        visible_ = true;
    }
}

void Surface::Hide() {
    if (hwnd && visible_) ShowWindow(hwnd, SW_HIDE);
    visible_ = false;
}

void Surface::Destroy() {
    ReleaseTarget();
    if (dc_) {
        SelectObject(dc_, old_);
        DeleteObject(bmp_);
        DeleteDC(dc_);
        dc_ = nullptr;
    }
    if (hwnd) DestroyWindow(hwnd);
    hwnd = nullptr;
    visible_ = false;
}

bool Surface::SavePng(const std::wstring& path) {
    if (!dc_ || !bits_ || !wic) return false;
    GdiFlush();
    IWICBitmap* bmp = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppPBGRA, w * 4, w * h * 4,
                                                    (BYTE*)bits_, &bmp)) &&
              SUCCEEDED(wic->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(w, h));
    if (ok) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) && SUCCEEDED(frame->WriteSource(bmp, nullptr)) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }
    SafeRelease(frame);
    SafeRelease(enc);
    SafeRelease(stream);
    SafeRelease(bmp);
    return ok;
}

ID2D1SolidColorBrush* Surface::Brush(D2D1_COLOR_F c) {
    if (!brush_ && rt_) rt_->CreateSolidColorBrush(c, &brush_);
    if (brush_) brush_->SetColor(c);
    return brush_;
}

// ---- text ------------------------------------------------------------------------------------
static bool FontExists(const wchar_t* name) {
    IDWriteFontCollection* fonts = nullptr;
    if (!dwrite || FAILED(dwrite->GetSystemFontCollection(&fonts, FALSE))) return false;
    UINT32 index = 0;
    BOOL exists = FALSE;
    fonts->FindFamilyName(name, &index, &exists);
    fonts->Release();
    return exists;
}

// ---- the Japanese font ----------------------------------------------------------------------
// Fontworks' Rodin (ロダン) suits the wide, spaced look best. It's a
// commercial font, so Candela can't ship it, but uses it when it's installed:
// any Rodin family (FOT-Rodin Pro, FOT-NewRodin ProN, as one family or one per
// weight), picking the installed weight nearest to what's asked. Without it,
// Yu Gothic UI (every Windows 10 and 11). font_ja in config.json picks another.
struct JaFace {
    std::wstring family;
    DWRITE_FONT_WEIGHT weight;
};

// "FOT-Rodin Pro DB", "FOT-ニューロダン ProN", "Rodin Pro": Rodin itself, not
// its rounded or display cousins (Rodin Maria, Rodin Cattleya...).
static bool IsRodin(std::wstring name) {
    for (auto& c : name) c = (wchar_t)towlower(c);
    if (name.rfind(L"fot-", 0) == 0) name.erase(0, 4);
    bool found = false;
    for (const wchar_t* base : {L"newrodin", L"new rodin", L"rodin", L"ニューロダン", L"ロダン"})
        if (name.rfind(base, 0) == 0) {
            name.erase(0, wcslen(base));
            found = true;
            break;
        }
    if (!found) return false;
    static const wchar_t* known[] = {L"pro", L"pron", L"std", L"stdn", L"pr6", L"pr6n", L"plus",
                                     L"l",   L"m",    L"db",  L"b",    L"eb",  L"ub"};
    size_t i = 0;
    while (i < name.size()) {
        while (i < name.size() && (name[i] == L' ' || name[i] == L'-')) i++;
        size_t j = i;
        while (j < name.size() && name[j] != L' ' && name[j] != L'-') j++;
        if (j == i) break;
        std::wstring token = name.substr(i, j - i);
        bool ok = false;
        for (auto* k : known) ok = ok || token == k;
        if (!ok) return false;
        i = j;
    }
    return true;
}

// Every installed Rodin face: (family, weight), upright only.
static std::vector<JaFace> RodinFaces() {
    std::vector<JaFace> out;
    IDWriteFontCollection* fonts = nullptr;
    if (!dwrite || FAILED(dwrite->GetSystemFontCollection(&fonts, FALSE))) return out;
    for (UINT32 i = 0; i < fonts->GetFontFamilyCount(); i++) {
        IDWriteFontFamily* fam = nullptr;
        IDWriteLocalizedStrings* names = nullptr;
        if (FAILED(fonts->GetFontFamily(i, &fam))) continue;
        if (SUCCEEDED(fam->GetFamilyNames(&names))) {
            bool rodin = false;
            std::wstring first;
            for (UINT32 k = 0; k < names->GetCount(); k++) {
                UINT32 len = 0;
                if (FAILED(names->GetStringLength(k, &len))) continue;
                std::wstring n(len + 1, L'\0');
                if (FAILED(names->GetString(k, n.data(), len + 1))) continue;
                n.resize(len);
                if (first.empty()) first = n;
                rodin = rodin || IsRodin(n);
            }
            if (rodin) {
                // CreateTextFormat takes any of the family's names; the first will do.
                for (UINT32 f = 0; f < fam->GetFontCount(); f++) {
                    IDWriteFont* font = nullptr;
                    if (FAILED(fam->GetFont(f, &font))) continue;
                    if (font->GetStyle() == DWRITE_FONT_STYLE_NORMAL && font->GetSimulations() == DWRITE_FONT_SIMULATIONS_NONE)
                        out.push_back({first, font->GetWeight()});
                    font->Release();
                }
            }
            names->Release();
        }
        fam->Release();
    }
    fonts->Release();
    return out;
}

static const std::vector<JaFace>& Rodin() {
    static std::vector<JaFace> faces = RodinFaces();  // fonts installed later need a restart of Candela
    return faces;
}

bool RodinInstalled() { return dwrite && !Rodin().empty(); }
bool FontInstalled(const wchar_t* family) { return FontExists(family); }

std::wstring JapaneseFont() {
    static std::wstring chosen_for = L"\x01", chosen;
    if (chosen_for == g_cfg.font_ja) return chosen;
    chosen_for = g_cfg.font_ja;
    const std::wstring& want = g_cfg.font_ja;
    if (!want.empty() && want != L"rodin" && FontExists(want.c_str())) chosen = want;
    else if (RodinInstalled()) chosen = L"rodin";
    else {
        if (!want.empty() && want != L"rodin") Log(L"font_ja: no font called \"%ls\" is installed", want.c_str());
        chosen = L"Segoe UI";
        for (const wchar_t* f : {L"Yu Gothic UI", L"Meiryo UI", L"Meiryo"})
            if (FontExists(f)) {
                chosen = f;
                break;
            }
    }
    Log(L"Japanese font: %ls", chosen == L"rodin" ? (Rodin()[0].family + L" (Rodin)").c_str() : chosen.c_str());
    return chosen;
}

static JaFace JapaneseFace(DWRITE_FONT_WEIGHT want) {
    std::wstring font = JapaneseFont();
    if (font != L"rodin") return {font, want};
    const JaFace* best = &Rodin()[0];
    for (auto& f : Rodin()) {
        int d = std::abs((int)f.weight - (int)want), bd = std::abs((int)best->weight - (int)want);
        if (d < bd || (d == bd && f.weight > best->weight)) best = &f;  // a tie goes to the heavier: Rodin M over L
    }
    return *best;
}

static const CachedFormat* Format(float size, DWRITE_FONT_WEIGHT weight, const wchar_t* font) {
    std::wstring name = font ? font : g_cfg.font;
    // In Japanese, the default text uses the Japanese font, with Japanese as
    // the locale so any character it lacks falls back to a Japanese form.
    const bool ja = !font && Japanese();
    if (ja) {
        JaFace face = JapaneseFace(weight);
        name = face.family;
        weight = face.weight;
    }
    for (auto& f : g_formats)
        if (f.size == size && f.weight == weight && f.font == name && f.ja == ja) return &f;
    IDWriteTextFormat* fmt = nullptr;
    if (FAILED(dwrite->CreateTextFormat(name.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, ja ? L"ja-jp" : L"", &fmt)))
        return nullptr;
    if (g_formats.size() > 96) ClearFormats();  // font changed a lot; start over (layouts keep their own references)
    IDWriteInlineObject* ellipsis = nullptr;
    dwrite->CreateEllipsisTrimmingSign(fmt, &ellipsis);
    g_formats.push_back({name, size, weight, ja, fmt, ellipsis});
    return &g_formats.back();
}

// English capitals are spaced widely; kana and kanji are wide already,
// and the same spacing would pull Japanese words apart.
constexpr float kJaSpacing = 0.4f;

static IDWriteTextLayout* Layout(const std::wstring& text, const TextStyle& st, float width, float height,
                                 bool cache = true) {
    std::wstring key;
    if (cache) {
        wchar_t k[160];
        swprintf(k, 160, L"%g|%d|%g|%d|%d|%g|%g|%g|%d|", st.size, (int)st.weight, st.spacing, (int)st.align, (int)st.wrap,
                 st.line, width, height, (int)Japanese());
        key = k;
        key += st.font ? st.font : L"";
        key += L"|" + g_cfg.font + L"|" + g_cfg.font_ja + L"|" + text;
        auto it = g_layouts.find(key);
        if (it != g_layouts.end()) {
            it->second->AddRef();
            return it->second;
        }
    }
    const CachedFormat* cf = Format(st.size, st.weight, st.font);
    if (!cf) return nullptr;
    IDWriteTextLayout* layout = nullptr;
    if (FAILED(dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), cf->fmt, std::max(1.f, width),
                                        std::max(1.f, height), &layout)))
        return nullptr;
    layout->SetWordWrapping(st.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(st.align == kLeft     ? DWRITE_TEXT_ALIGNMENT_LEADING
                             : st.align == kCenter ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                   : DWRITE_TEXT_ALIGNMENT_TRAILING);
    layout->SetParagraphAlignment(st.wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (st.wrap && st.line > 0) layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, st.line, st.line * 0.8f);
    DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    layout->SetTrimming(&trim, cf->ellipsis);
    float spacing = !st.font && Japanese() ? st.spacing * kJaSpacing : st.spacing;
    if (spacing != 0 && !text.empty()) {
        IDWriteTextLayout1* l1 = nullptr;
        if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) {
            l1->SetCharacterSpacing(0, spacing, 0, {0, (UINT32)text.size()});
            l1->Release();
        }
    }
    if (cache) {
        if (g_layouts.size() > 400) ClearLayouts();  // far more than any screen shows: start over
        layout->AddRef();
        g_layouts[key] = layout;
    }
    return layout;
}

void WarmFonts() {
    if (!Japanese()) return;
    for (auto w : {DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_WEIGHT_BOLD})
        TextWidth(L"日本語 ABC", {14, w});  // a layout loads the font file too
}

void Text(ID2D1RenderTarget* rt, Surface& s, const std::wstring& text, D2D1_RECT_F box, const TextStyle& st,
          D2D1_COLOR_F color) {
    if (text.empty() || box.right <= box.left) return;
    IDWriteTextLayout* layout = Layout(text, st, box.right - box.left, box.bottom - box.top);
    if (!layout) return;
    ID2D1SolidColorBrush* brush = s.Brush(color);
    if (brush) rt->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    layout->Release();
}

float TextWidth(const std::wstring& text, const TextStyle& st) {
    if (text.empty()) return 0;
    TextStyle one = st;
    one.wrap = false;
    IDWriteTextLayout* layout = Layout(text, one, 10000.f, st.size * 2);
    if (!layout) return 0;
    DWRITE_TEXT_METRICS m = {};
    layout->GetMetrics(&m);
    layout->Release();
    return m.widthIncludingTrailingWhitespace;
}

// ---- shapes ------------------------------------------------------------------------------------
void Fill(ID2D1RenderTarget* rt, Surface& s, float x0, float y0, float x1, float y1, D2D1_COLOR_F c) {
    if (x1 <= x0 || y1 <= y0) return;
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    if (auto* b = s.Brush(c)) rt->FillRectangle(D2D1::RectF(x0, y0, x1, y1), b);
}

void Frame(ID2D1RenderTarget* rt, Surface& s, float x0, float y0, float x1, float y1, float w, D2D1_COLOR_F c) {
    Fill(rt, s, x0, y0, x1, y0 + w, c);
    Fill(rt, s, x0, y1 - w, x1, y1, c);
    Fill(rt, s, x0, y0, x0 + w, y1, c);
    Fill(rt, s, x1 - w, y0, x1, y1, c);
}

void ClearRect(ID2D1RenderTarget* rt, float x0, float y0, float x1, float y1) {
    if (x1 <= x0 || y1 <= y0) return;
    rt->PushAxisAlignedClip(D2D1::RectF(x0, y0, x1, y1), D2D1_ANTIALIAS_MODE_ALIASED);
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->PopAxisAlignedClip();
}

void Line(ID2D1RenderTarget* rt, Surface& s, float x0, float y0, float x1, float y1, float w, D2D1_COLOR_F c) {
    rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (auto* b = s.Brush(c)) rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), b, w);
}

}  // namespace gfx
