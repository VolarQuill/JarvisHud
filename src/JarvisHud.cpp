//======================================================================
//  JARVIS HUD  -  a JARVIS themed login greeting for Windows 11.
//
//  A single translation unit that renders a small, corner-docked, fully
//  click-through HUD which greets the user at logon and then gets out of
//  the way.  Every pixel is drawn in code with GDI+ primitives - no
//  bitmap, sprite or image asset is shipped.
//
//  Build (MSYS2 UCRT64):
//    g++ -std=c++17 -O2 -municode -mwindows -o JarvisHud.exe
//        src/JarvisHud.cpp -lgdiplus -luser32 -lgdi32 -lole32 -luuid
//
//  Behaviour summary
//  -----------------
//   * Ordinary window (deliberately NOT layered and NOT topmost) docked
//     to a corner of the primary monitor's WORK AREA (never over the
//     taskbar).
//   * The window is put into the desktop band: directly above the
//     shell's own desktop window, so it belongs to the desktop and is
//     below every running application.  It is only ever shown while it
//     is in that band.
//   * WS_EX_NOACTIVATE + WS_EX_TRANSPARENT + HTTRANSPARENT: it never
//     takes focus and never swallows a mouse click.
//   * Two layer rendering: a static base (panel, border, brackets, grid,
//     separator, ambient bloom) rendered once, and a dynamic layer (arc
//     reactor, sweep line, greeting, status, clock) redrawn per frame.
//   * GDI+ renders into a 32bpp ARGB surface; the frame is premultiplied
//     and composed in software, then flattened onto the colour of the
//     card and painted into the window.  A layered window would be
//     blended by the desktop window manager in a pass of its own and
//     would float above every application, which is not allowed.
//
//  Every failure path exits silently with exit code 0.
//======================================================================

#define NOMINMAX
#include <windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

//======================================================================
//  1.  Design tokens  (authoritative specification values)
//======================================================================
namespace {

struct Rgba {
    BYTE a, r, g, b;
};

//  Palette (ARGB, alpha 0-255)
constexpr Rgba kPanelFill   = { 232,   4,  14,  24};  // near black navy, ~91% opaque
constexpr Rgba kPanelEdge   = { 170,   0, 197, 232};  // 1px cyan border
constexpr Rgba kBracket     = { 255, 125, 249, 255};  // bright corner brackets
constexpr Rgba kTextPrimary = { 255, 184, 244, 255};  // main greeting line
constexpr Rgba kTextBright  = { 255, 232, 253, 255};  // highlighted words
constexpr Rgba kTextDim     = { 255,  78, 143, 160};  // status line, labels
constexpr Rgba kAccentAmber = { 255, 255, 179,  71};  // sparing accent (arc ticks)
constexpr Rgba kGridLine    = {  90,  14,  58,  74};  // faint background grid
constexpr Rgba kReactorCore = { 255, 255, 255, 255};  // white hot centre
constexpr Rgba kReactorGlow = { 190, 125, 249, 255};  // inner glow ring
constexpr Rgba kReactorRing = { 220,   0, 200, 224};  // outer rotating ring
constexpr Rgba kSweepLine   = {  70, 125, 249, 255};  // animated scan line

//  Geometry, logical pixels (panel 520 x 186)
constexpr float kPanelW       = 520.0f;
constexpr float kPanelH       = 186.0f;
constexpr float kCornerRadius = 6.0f;
constexpr float kBracketArm   = 20.0f;
constexpr float kBracketInset = 6.0f;
constexpr float kBracketThick = 2.0f;
constexpr float kReactorX     = 74.0f;
constexpr float kReactorY     = 93.0f;
constexpr float kROuter       = 42.0f;
constexpr float kRMid         = 31.0f;
constexpr float kRCore        = 13.0f;
constexpr float kRGlow        = 22.0f;
constexpr int   kReactorTicks = 8;
constexpr float kTickLength   = 4.0f;
constexpr float kTextX        = 138.0f;
constexpr float kTextRight    = 496.0f;
constexpr float kTextMidY     = 84.0f;
constexpr float kStatusY      = 132.0f;
constexpr float kSeparatorY   = 116.0f;

constexpr int   kRingSegments = 24;       // segments of the rotating outer ring
constexpr float kRingDuty     = 0.72f;    // fraction of each slot that is lit
constexpr int   kMidSegments  = 4;        // counter rotating arcs on the mid ring
constexpr float kGridStep     = 26.0f;    // logical spacing of the background grid
constexpr float kMinFontPx    = 12.0f;    // floor for the automatic font shrink
constexpr int   kFrameMs      = 33;      // ~30 fps animation timer
constexpr double kHoldRedrawMs = 90.0;   // redraw interval of the idle phase
constexpr UINT_PTR kTimerId   = 1;
constexpr const wchar_t* kRegSubKey = L"Software\\JarvisHud";
constexpr const wchar_t* kWinlogonKey =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";

inline Gdiplus::Color Col(const Rgba& c) {
    return Gdiplus::Color(c.a, c.r, c.g, c.b);
}

inline Gdiplus::Color ColScaled(const Rgba& c, int alpha) {
    int a = (c.a * alpha) / 255;
    if (a < 0)   a = 0;
    if (a > 255) a = 255;
    return Gdiplus::Color((BYTE)a, c.r, c.g, c.b);
}

inline double ClampD(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline BYTE ClampByte(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (BYTE)v;
}

//  Mixes a difference into a channel: v * fade / 255, rounded.
inline int MixRound(int delta, int fade) {
    const int p = delta * fade;
    return (p >= 0) ? ((p + 127) / 255) : ((p - 127) / 255);
}

}  // namespace

//======================================================================
//  2.  Small string / file helpers
//======================================================================
namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w;
    w.resize((size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string NarrowUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s;
    s.resize((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool ReadWholeFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    out.clear();
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1LL << 28)) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr)) out.clear();
        else out.resize(got);
    }
    CloseHandle(h);
    return true;
}

std::wstring TrimWs(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n')) --e;
    return s.substr(b, e - b);
}

std::wstring LowerAscii(const std::wstring& s) {
    std::wstring o = s;
    for (size_t i = 0; i < o.size(); ++i)
        if (o[i] >= L'A' && o[i] <= L'Z') o[i] = (wchar_t)(o[i] - L'A' + L'a');
    return o;
}

std::wstring DirNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L".";
    if (p == 0) return L"\\";
    return path.substr(0, p);
}

int ParseInt(const std::wstring& s, int fallback) {
    if (s.empty()) return fallback;
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    if (end == s.c_str()) return fallback;
    if (v < -1000000L) v = -1000000L;
    if (v > 1000000L) v = 1000000L;
    return (int)v;
}

//  Split a UTF-8 blob into lines, honouring LF and CRLF.
std::vector<std::string> SplitLines(const std::string& blob) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= blob.size(); ++i) {
        if (i == blob.size() || blob[i] == '\n') {
            size_t end = i;
            if (end > start && blob[end - 1] == '\r') --end;
            lines.push_back(blob.substr(start, end - start));
            start = i + 1;
        }
    }
    return lines;
}

std::string StripUtf8Bom(const std::string& s) {
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF)
        return s.substr(3);
    return s;
}

bool WriteTextFile(const std::wstring& path, const std::string& utf8, bool append = false) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    if (append) {
        LARGE_INTEGER end;
        end.QuadPart = 0;
        SetFilePointerEx(h, end, nullptr, FILE_END);
    }
    DWORD written = 0;
    if (!utf8.empty())
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(h);
    return true;
}

//======================================================================
//  3.  Configuration
//======================================================================
struct Config {
    std::wstring name          = L"sir";
    std::wstring font          = L"Cascadia Mono";
    int         fontSize       = 17;
    int         statusFontSize = 11;
    std::wstring position      = L"top-left";
    int         margin         = 32;
    //  The exact place of the panel, in logical pixels from the corner
    //  of the work area.  -1 means "use the corner named by position".
    //  The panel writes these itself when it is dragged somewhere else.
    int         posX           = -1;
    int         posY           = -1;
    int         panelWidth     = (int)kPanelW;
    int         panelHeight    = (int)kPanelH;
    int         fadeInMs       = 450;
    int         bootMs         = 1200;
    int         typeMs         = 28;
    int         holdMs         = 4200;
    int         fadeOutMs      = 700;
    int         dismissMs      = 0;
    int         persist        = 0;
    int         showClock      = 1;
    std::wstring statusText    = L"SYSTEM ONLINE";
    int         noticeText     = 0;
    //  greetingLine: the line of greetings.txt the panel greets you
    //  with when it comes up, counted from one.  0 picks one at random.
    int         greetingLine   = 1;
    //  The panel is always put into the desktop band, that is directly
    //  above the shell's desktop window, so that it belongs to the
    //  desktop and is therefore behind every application window.  There
    //  is deliberately no switch for it: the panel must never end up
    //  above a running application.
    //
    //  rotateSeconds: while the panel persists, swap to a new greeting
    //  every N seconds.  0 keeps the first greeting for ever.
    int         rotateSeconds  = 60;
    //  desktopWaitMs: how long to wait for the shell's desktop window to
    //  appear at logon before falling back to the top level popup.
    int         desktopWaitMs  = 3000;
    //  autostart: 1 keeps the "start with Windows" entry, 0 removes it.
    //  The panel puts the entry back on launch when it has gone missing,
    //  so that the greeting cannot silently stop coming up at logon.
    int         autostart      = 1;
    //  How fast the reactor turns, in percent of its own speed.  100 is
    //  the speed the panel was built with, 50 is half of it.
    int         reactorSpeed   = 50;
};

bool IsKnownPosition(const std::wstring& p) {
    return p == L"bottom-right" || p == L"bottom-left" || p == L"top-right" ||
           p == L"top-left" || p == L"center";
}

void ApplyConfigLine(const std::wstring& keyRaw, const std::wstring& val, Config& cfg) {
    const std::wstring key = LowerAscii(keyRaw);
    if (key == L"name")                  cfg.name = val;
    else if (key == L"font")             cfg.font = val;
    else if (key == L"font_size")        cfg.fontSize = ParseInt(val, cfg.fontSize);
    else if (key == L"status_font_size") cfg.statusFontSize = ParseInt(val, cfg.statusFontSize);
    else if (key == L"position") {
        std::wstring p = LowerAscii(val);
        cfg.position = IsKnownPosition(p) ? p : L"top-left";
    }
    else if (key == L"margin")           cfg.margin = ParseInt(val, cfg.margin);
    else if (key == L"pos_x")            cfg.posX = ParseInt(val, cfg.posX);
    else if (key == L"pos_y")            cfg.posY = ParseInt(val, cfg.posY);
    else if (key == L"reactor_speed")    cfg.reactorSpeed = ParseInt(val, cfg.reactorSpeed);
    else if (key == L"panel_width")      cfg.panelWidth = ParseInt(val, cfg.panelWidth);
    else if (key == L"panel_height")     cfg.panelHeight = ParseInt(val, cfg.panelHeight);
    else if (key == L"fade_in_ms")       cfg.fadeInMs = ParseInt(val, cfg.fadeInMs);
    else if (key == L"boot_ms")          cfg.bootMs = ParseInt(val, cfg.bootMs);
    else if (key == L"type_ms")          cfg.typeMs = ParseInt(val, cfg.typeMs);
    else if (key == L"hold_ms")          cfg.holdMs = ParseInt(val, cfg.holdMs);
    else if (key == L"fade_out_ms")      cfg.fadeOutMs = ParseInt(val, cfg.fadeOutMs);
    else if (key == L"dismiss_ms")       cfg.dismissMs = ParseInt(val, cfg.dismissMs);
    else if (key == L"persist")          cfg.persist = ParseInt(val, cfg.persist);
    else if (key == L"show_clock")       cfg.showClock = ParseInt(val, cfg.showClock);
    else if (key == L"status_text")      cfg.statusText = val;
    else if (key == L"notice_text")      cfg.noticeText = ParseInt(val, cfg.noticeText);
    else if (key == L"greeting_line")    cfg.greetingLine = ParseInt(val, cfg.greetingLine);
    else if (key == L"rotate_seconds")   cfg.rotateSeconds = ParseInt(val, cfg.rotateSeconds);
    else if (key == L"desktop_wait_ms")  cfg.desktopWaitMs = ParseInt(val, cfg.desktopWaitMs);
    else if (key == L"autostart")        cfg.autostart = ParseInt(val, cfg.autostart);
    //  Unknown keys are ignored on purpose.
}

bool LoadConfig(const std::wstring& path, Config& cfg) {
    std::string blob;
    if (!ReadWholeFile(path, blob)) return false;
    blob = StripUtf8Bom(blob);
    for (const std::string& raw : SplitLines(blob)) {
        std::wstring line = TrimWs(Widen(raw));
        if (line.empty() || line[0] == L'#') continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        ApplyConfigLine(TrimWs(line.substr(0, eq)), TrimWs(line.substr(eq + 1)), cfg);
    }
    return true;
}

//======================================================================
//  3b.  Autostart
//
//  The panel is started at logon by an entry in
//  HKCU\Software\Microsoft\Windows\CurrentVersion\Run.  The installer
//  writes that entry, but it can also go missing - a cleanup tool, or
//  an uninstall that was never finished - and the panel then simply
//  never comes up at logon, with nothing to say why.  So the panel
//  checks the entry on every launch and puts it back when it is gone.
//
//  The check is deliberately limited to the installed copy.  A panel
//  that is run from the project folder must never point the Run key
//  at itself, because that would take the greeting away from the
//  installed program.
//======================================================================

const wchar_t* const kRunKeyPath    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* const kRunValueName  = L"JarvisHud";

std::wstring RunValueFor(const std::wstring& exePath) {
    std::wstring cmd = L"\"";
    cmd.append(exePath);
    cmd += L"\"";
    return cmd;
}

bool IsAutostartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[2048];
    DWORD sz = sizeof(buf), type = 0;
    bool found = false;
    if (RegQueryValueExW(key, kRunValueName, nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS)
        found = (type == REG_SZ || type == REG_EXPAND_SZ);
    RegCloseKey(key);
    return found;
}

bool SetAutostartEnabled(bool enabled, const std::wstring& exePath) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = true;
    if (enabled) {
        const std::wstring cmd = RunValueFor(exePath);
        ok = (RegSetValueExW(key, kRunValueName, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                             (DWORD)((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
    } else {
        const LONG rc = RegDeleteValueW(key, kRunValueName);
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}

//  True when this copy is the one in %LOCALAPPDATA%\JarvisHud, that is
//  the copy the Run key is supposed to point at.
bool IsInstalledCopy(const std::wstring& exeDir) {
    wchar_t local[1024];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local,
                                            (DWORD)(sizeof(local) / sizeof(local[0])));
    if (n == 0 || n >= sizeof(local) / sizeof(local[0])) return false;
    std::wstring want(local, n);
    while (!want.empty() && (want.back() == L'\\' || want.back() == L'/')) want.pop_back();
    want += L"\\JarvisHud";
    return LowerAscii(exeDir) == LowerAscii(want);
}

//  Rewrites one key in a config file.  The comments and every other
//  line are kept exactly as they were.
bool SaveConfigKey(const std::wstring& path, const std::wstring& key, const std::wstring& value) {
    if (path.empty() || key.empty()) return false;
    const std::wstring wanted = LowerAscii(TrimWs(key));

    std::string blob;
    if (!ReadWholeFile(path, blob)) return false;
    blob = StripUtf8Bom(blob);

    const std::string eol = blob.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::string newLine = NarrowUtf8(TrimWs(key)) + "=" + NarrowUtf8(TrimWs(value));

    std::vector<std::string> lines = SplitLines(blob);
    bool replaced = false;
    for (std::string& line : lines) {
        const std::wstring trimmed = TrimWs(Widen(line));
        if (trimmed.empty() || trimmed[0] == L'#') continue;
        const size_t eq = trimmed.find(L'=');
        if (eq == std::wstring::npos) continue;
        if (LowerAscii(TrimWs(trimmed.substr(0, eq))) != wanted) continue;
        line = newLine;
        replaced = true;
        break;
    }
    if (!replaced) lines.push_back(newLine);

    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += eol;
        out += lines[i];
    }
    out += eol;
    return WriteTextFile(path, out, false);
}


//----------------------------------------------------------------------
//  A greeting line may carry an optional time-of-day tag, written as
//  "morning|" / "afternoon|" / "evening|" / "any|" in front of the text.
//  A tagged line is only ever shown during that part of the day; an
//  untagged line is always eligible.  When no line matches the current
//  part of the day the whole corpus is used as a fallback.
//----------------------------------------------------------------------
enum TimeBucket {
    kBucketAny       = -1,
    kBucketMorning   = 0,   // 05:00 - 11:59
    kBucketAfternoon = 1,   // 12:00 - 17:59
    kBucketEvening   = 2,   // 18:00 - 04:59
};

struct GreetingLine {
    std::wstring text;
    int         bucket = kBucketAny;
};

const wchar_t* const kBuiltinGreetings[] = {
    L"Good evening, {name}. All systems are nominal.",
    L"Welcome back, {name}. I trust your rest was adequate.",
    L"Good {time}, {name}. The workshop stands ready.",
    L"{weekday}, {name}. I am entirely at your disposal.",
    L"All systems online, {name}. Your command is awaited.",
};

int ParseBucketTag(const std::wstring& lowerTag) {
    if (lowerTag == L"morning")   return kBucketMorning;
    if (lowerTag == L"afternoon") return kBucketAfternoon;
    if (lowerTag == L"evening")   return kBucketEvening;
    return kBucketAny;
}

int CurrentTimeBucket() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (st.wHour >= 5 && st.wHour < 12)  return kBucketMorning;
    if (st.wHour >= 12 && st.wHour < 18) return kBucketAfternoon;
    return kBucketEvening;
}

std::vector<GreetingLine> LoadGreetings(const std::wstring& path) {
    std::vector<GreetingLine> out;
    std::string blob;
    if (ReadWholeFile(path, blob)) {
        blob = StripUtf8Bom(blob);
        for (const std::string& raw : SplitLines(blob)) {
            std::wstring line = TrimWs(Widen(raw));
            if (line.empty() || line[0] == L'#') continue;

            GreetingLine entry;
            entry.bucket = kBucketAny;

            //  Look for a "morning|" style prefix.  Only the four known
            //  tags count, so a greeting may still contain a pipe.
            const size_t bar = line.find(L'|');
            if (bar != std::wstring::npos && bar > 0) {
                std::wstring lower;
                for (wchar_t c : line.substr(0, bar)) {
                    lower += (c >= L'A' && c <= L'Z') ? (wchar_t)(c - L'A' + L'a') : c;
                }
                if (lower == L"morning" || lower == L"afternoon" ||
                    lower == L"evening" || lower == L"any") {
                    entry.bucket = ParseBucketTag(lower);
                    line = TrimWs(line.substr(bar + 1));
                }
            }
            if (line.empty()) continue;

            entry.text = line;
            out.push_back(entry);
        }
    }
    if (out.empty()) {
        for (const wchar_t* g : kBuiltinGreetings) {
            GreetingLine entry;
            entry.text  = g;
            entry.bucket = kBucketAny;
            out.push_back(entry);
        }
    }
    return out;
}

uint32_t MixSeed() {
    uint32_t h = 2166136261u;
    uint64_t tick = GetTickCount64();
    h = (h ^ (uint32_t)tick) * 16777619u;
    h = (h ^ (uint32_t)GetCurrentProcessId()) * 16777619u;
    uintptr_t stackMarker = (uintptr_t)&tick;
    h = (h ^ (uint32_t)(stackMarker >> 4)) * 16777619u;
    LARGE_INTEGER qc;
    if (QueryPerformanceCounter(&qc)) h = (h ^ (uint32_t)qc.QuadPart) * 16777619u;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h;
}

bool ReadLastIndex(int& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, value = 0, cb = sizeof(value);
    bool ok = (RegQueryValueExW(key, L"LastIndex", nullptr, &type, (LPBYTE)&value, &cb) == ERROR_SUCCESS) &&
              type == REG_DWORD;
    RegCloseKey(key);
    if (ok) out = (int)value;
    return ok;
}

void WriteLastIndex(int value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegSubKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = (DWORD)value;
    RegSetValueExW(key, L"LastIndex", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(key);
}

//  Picks a pseudo random corpus line from the supplied candidates; never
//  the same line twice in a row when more than one is eligible.  The
//  candidate list holds indices into the full corpus, so the persisted
//  "last index" stays meaningful as the eligible set changes with the
//  time of day.
size_t ChooseGreetingIndex(const std::vector<int>& candidates, int lastIndex, bool hasLast) {
    if (candidates.empty()) return 0;
    if (candidates.size() == 1) return (size_t)candidates[0];
    for (int attempt = 0; attempt < 8; ++attempt) {
        const size_t slot = (size_t)(MixSeed() % (uint32_t)candidates.size());
        const int picked = candidates[slot];
        if (!hasLast || picked != lastIndex) return (size_t)picked;
    }
    //  The random draw kept hitting the previous line: step to the first
    //  candidate that differs from it.
    for (size_t i = 0; i < candidates.size(); ++i) {
        const int c = candidates[i];
        if (!hasLast || c != lastIndex) return (size_t)c;
    }
    return (size_t)candidates[0];
}

//======================================================================
//  5.  Placeholder expansion
//======================================================================
struct Expanded {
    std::wstring text;
    std::vector<std::pair<int, int>> bright;  // [start,length) ranges drawn bright
};

Expanded ExpandGreeting(const std::wstring& tmpl, const Config& cfg,
                         const std::wstring& timeStr, const std::wstring& dateStr,
                         const std::wstring& weekdayStr) {
    Expanded out;
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == L'{') {
            size_t close = tmpl.find(L'}', i);
            if (close != std::wstring::npos && close > i + 1) {
                const std::wstring key = tmpl.substr(i + 1, close - i - 1);
                std::wstring repl;
                bool bright = false;
                if (key == L"name") {
                    repl = cfg.name;
                    bright = true;
                } else if (key == L"time") {
                    repl = timeStr;
                } else if (key == L"date") {
                    repl = dateStr;
                } else if (key == L"weekday") {
                    repl = weekdayStr;
                } else if (key == L"status") {
                    repl = cfg.statusText;
                }
                if (!repl.empty()) {
                    const int start = (int)out.text.size();
                    out.text += repl;
                    if (bright) out.bright.push_back(std::make_pair(start, (int)repl.size()));
                    i = close + 1;
                    continue;
                }
                //  Unknown placeholder: left verbatim.
            }
        }
        out.text.push_back(tmpl[i]);
        ++i;
    }
    if (out.text.empty()) out.text = L"SYSTEM ONLINE";
    return out;
}

std::wstring FormatTimeHHMM(const SYSTEMTIME& st) {
    wchar_t buf[16];
    swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%02d:%02d", st.wHour, st.wMinute);
    return buf;
}

const wchar_t* const kWeekdayNames[] = {L"Sunday",   L"Monday", L"Tuesday", L"Wednesday",
                                         L"Thursday", L"Friday", L"Saturday"};
const wchar_t* const kMonthNames[] = {L"January",   L"February", L"March",    L"April",
                                       L"May",       L"June",     L"July",     L"August",
                                       L"September", L"October",  L"November", L"December"};

//  The long date, e.g. "Tuesday, 20 October 2026".  The string is built
//  by hand because the %s conversion of the wide printf family expects a
//  narrow string, not a wide one.
std::wstring FormatLongDate(const SYSTEMTIME& st) {
    std::wstring s = kWeekdayNames[st.wDayOfWeek % 7];
    s += L", ";
    s += std::to_wstring(st.wDay);
    s += L" ";
    s += kMonthNames[(st.wMonth + 11) % 12];  //  wMonth is one based.
    s += L" ";
    s += std::to_wstring(st.wYear);
    return s;
}

//======================================================================
//  6.  Fonts
//======================================================================
const wchar_t* const kFontChain[] = {L"Cascadia Mono", L"Consolas", L"Lucida Console", L"Courier New"};

//  Exact monospace cell advance, measured with GraphicsPath so that no
//  MeasureString padding creeps in.  The width of "MM" minus the width of
//  "M" is the true advance because both glyphs share the same side
//  bearings.
float MeasureAdvance(const Gdiplus::FontFamily* family, float emPx) {
    if (!family || emPx <= 0.0f) return emPx * 0.6f;
    Gdiplus::GraphicsPath p1, p2;
    const Gdiplus::RectF layout(0.0f, 0.0f, 8000.0f, 400.0f);
    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    p1.AddString(L"M", -1, family, Gdiplus::FontStyleRegular, emPx, layout, &fmt);
    p2.AddString(L"MM", -1, family, Gdiplus::FontStyleRegular, emPx, layout, &fmt);
    Gdiplus::RectF b1, b2;
    p1.GetBounds(&b1);
    p2.GetBounds(&b2);
    const float adv = b2.Width - b1.Width;
    return adv > 0.1f ? adv : emPx * 0.6f;
}

//  Line height of a font, in pixels, for a pixel sized em square.
float MeasureLineHeight(const Gdiplus::FontFamily* family, float emPx) {
    if (!family) return emPx * 1.2f;
    const UINT16 em = family->GetEmHeight(Gdiplus::FontStyleRegular);
    const UINT16 ls = family->GetLineSpacing(Gdiplus::FontStyleRegular);
    if (em == 0) return emPx * 1.2f;
    return emPx * ((float)ls / (float)em);
}

//======================================================================
//  7.  Text layout (wrap to two lines, shrink, then ellipsize)
//======================================================================
struct TextLine {
    int start;  // first character index inside the expanded greeting
    int len;    // number of characters
};

struct TextLayout {
    float fontPx = 0.0f;      // device pixels
    float advance = 0.0f;     // device pixels per character
    float lineHeight = 0.0f;  // device pixels
    std::vector<TextLine> lines;
    bool ellipsized = false;
    int  logicalFontSize = 0;
};

//  Greedy word wrap; a word longer than the column is hard broken.
std::vector<TextLine> WrapText(const std::wstring& s, int maxChars) {
    std::vector<TextLine> lines;
    if (maxChars < 1) maxChars = 1;
    const int n = (int)s.size();
    int lineStart = 0;
    while (lineStart < n) {
        int limit = lineStart + maxChars;
        if (limit > n) limit = n;
        int lineEnd = limit;
        int nextStart = limit;
        if (limit < n) {
            bool found = false;
            for (int j = limit; j > lineStart; --j) {
                if (s[(size_t)j - 1] == L' ') {
                    lineEnd = j - 1;
                    nextStart = j;
                    found = true;
                    break;
                }
            }
            if (!found) {  //  Hard break inside a long word.
                lineEnd = limit;
                nextStart = limit;
            }
        }
        if (lineEnd <= lineStart) {
            lineEnd = (lineStart + 1 < n) ? lineStart + 1 : n;
            nextStart = lineEnd;
        }
        lines.push_back(TextLine{lineStart, lineEnd - lineStart});
        lineStart = nextStart;
        while (lineStart < n && s[(size_t)lineStart] == L' ') ++lineStart;
    }
    if (lines.empty()) lines.push_back(TextLine{0, 0});
    return lines;
}

TextLayout ComputeLayout(const std::wstring& text, const Config& cfg, float columnWidth,
                         const Gdiplus::FontFamily* family, float scale) {
    TextLayout best;
    bool found = false;
    for (int logical = cfg.fontSize; logical >= (int)kMinFontPx; --logical) {
        const float em = (float)logical * scale;
        const float adv = MeasureAdvance(family, em);
        const int maxChars = (int)std::floor(columnWidth / (adv > 0.1f ? adv : 1.0f));
        std::vector<TextLine> lines = WrapText(text, maxChars);
        if ((int)lines.size() <= 2) {
            best.fontPx = em;
            best.advance = adv;
            best.lineHeight = MeasureLineHeight(family, em);
            best.lines = lines;
            best.logicalFontSize = logical;
            found = true;
            break;
        }
    }
    if (!found) {
        //  Floor reached: keep two lines worth of characters and ellipsize.
        const float em = kMinFontPx * scale;
        const float adv = MeasureAdvance(family, em);
        const int maxChars = (int)std::floor(columnWidth / (adv > 0.1f ? adv : 1.0f));
        int budget = 2 * maxChars - 3;
        if (budget < 1) budget = 1;
        std::wstring cut = text.substr(0, (size_t)std::min((int)text.size(), budget));
        while (!cut.empty() && cut[cut.size() - 1] == L' ') cut.erase(cut.size() - 1);
        cut += L"...";
        best.fontPx = em;
        best.advance = adv;
        best.lineHeight = MeasureLineHeight(family, em);
        best.lines = WrapText(cut, maxChars);
        best.ellipsized = true;
        best.logicalFontSize = (int)kMinFontPx;
    }
    return best;
}

//======================================================================
//  8.  Drawing helpers
//======================================================================
void BuildRoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (r < 0.0f) r = 0.0f;
    float d = 2.0f * r;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d <= 0.0f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        path.CloseFigure();
        return;
    }
    //  Every corner needs its own rectangle; a single, cumulatively
    //  offset rectangle would make the path self intersect.
    path.AddArc(Gdiplus::RectF(x, y, d, d), 180.0f, 90.0f);
    path.AddArc(Gdiplus::RectF(x + w - d, y, d, d), 270.0f, 90.0f);
    path.AddArc(Gdiplus::RectF(x + w - d, y + h - d, d, d), 0.0f, 90.0f);
    path.AddArc(Gdiplus::RectF(x, y + h - d, d, d), 90.0f, 90.0f);
    path.CloseFigure();
}

//  Straight alpha -> premultiplied alpha, without any per pixel division.
//  round(c * a / 255) == (c * a * 257 + 32768) >> 16
inline BYTE PremulChannel(BYTE c, BYTE a) {
    const DWORD v = (DWORD)c * (DWORD)a;
    return (BYTE)((v * 257u + 32768u) >> 16);
}

//  Premultiplied "over" of two samples:  dst = src + dst * (1 - srcA).
//  The formula also holds for a fully transparent source, so the inner
//  loops need no branch at all.
inline DWORD OverPixel(DWORD src, DWORD dst) {
    const BYTE sa = (BYTE)(src >> 24);
    const BYTE da = (BYTE)(dst >> 24);
    const DWORD inv = 255u - sa;
    const BYTE r = (BYTE)((src & 0xFFu) + PremulChannel((BYTE)(dst & 0xFFu), (BYTE)inv));
    const BYTE g = (BYTE)(((src >> 8) & 0xFFu) + PremulChannel((BYTE)((dst >> 8) & 0xFFu), (BYTE)inv));
    const BYTE b = (BYTE)(((src >> 16) & 0xFFu) + PremulChannel((BYTE)((dst >> 16) & 0xFFu), (BYTE)inv));
    const BYTE a = (BYTE)(sa + PremulChannel(da, (BYTE)inv));
    return ((DWORD)a << 24) | ((DWORD)b << 16) | ((DWORD)g << 8) | (DWORD)r;
}

inline DWORD PremulPixel(DWORD px, BYTE a) {
    const BYTE r = (BYTE)((px >> 16) & 0xFF);
    const BYTE g = (BYTE)((px >> 8) & 0xFF);
    const BYTE b = (BYTE)(px & 0xFF);
    return ((DWORD)a << 24) | ((DWORD)PremulChannel(r, a) << 16) | ((DWORD)PremulChannel(g, a) << 8) |
           (DWORD)PremulChannel(b, a);
}

//======================================================================
//  9.  RAII helpers
//======================================================================
class GdiplusSession {
public:
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        m_ok = (Gdiplus::GdiplusStartup(&m_token, &input, nullptr) == Gdiplus::Ok);
    }
    ~GdiplusSession() {
        if (m_ok) Gdiplus::GdiplusShutdown(m_token);
    }
    GdiplusSession(const GdiplusSession&) = delete;
    GdiplusSession& operator=(const GdiplusSession&) = delete;
    bool ok() const { return m_ok; }

private:
    ULONG_PTR m_token = 0;
    bool m_ok = false;
};

//  A 32 bit DIB section plus the memory DC that holds it.
struct DibSurface {
    HDC      dc   = nullptr;
    HBITMAP  bmp  = nullptr;
    HGDIOBJ  old  = nullptr;
    BYTE*    bits = nullptr;
    int      w = 0, h = 0;
    bool     topDown = true;

    bool Create(HDC screen, int width, int height, bool topDownRows) {
        Destroy();
        w = width;
        h = height;
        topDown = topDownRows;
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width;
        bi.bmiHeader.biHeight = topDown ? -height : height;  //  Negative = top down.
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
        if (!bmp || !bits) {
            Destroy();
            return false;
        }
        dc = CreateCompatibleDC(screen);
        if (!dc) {
            Destroy();
            return false;
        }
        old = SelectObject(dc, bmp);
        ZeroMemory(bits, (size_t)w * (size_t)h * 4);
        return true;
    }

    void Destroy() {
        if (dc && old) SelectObject(dc, old);
        old = nullptr;
        if (dc) { DeleteDC(dc); dc = nullptr; }
        if (bmp) { DeleteObject(bmp); bmp = nullptr; }
        bits = nullptr;
        w = h = 0;
    }

    ~DibSurface() { Destroy(); }
    DibSurface() = default;
    DibSurface(const DibSurface&) = delete;
    DibSurface& operator=(const DibSurface&) = delete;
};

//======================================================================
//  10.  Minimal PNG writer (diagnostics only, never used at runtime)
//======================================================================
uint32_t Crc32Update(uint32_t crc, const BYTE* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t Adler32(const BYTE* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void PushBe32(std::vector<BYTE>& v, uint32_t x) {
    v.push_back((BYTE)(x >> 24));
    v.push_back((BYTE)(x >> 16));
    v.push_back((BYTE)(x >> 8));
    v.push_back((BYTE)x);
}

void PushChunk(std::vector<BYTE>& out, const char* type, const std::vector<BYTE>& data) {
    PushBe32(out, (uint32_t)data.size());
    std::vector<BYTE> typeBuf(type, type + 4);
    uint32_t crc = Crc32Update(0, typeBuf.data(), 4);
    if (!data.empty()) crc = Crc32Update(crc, data.data(), data.size());
    out.insert(out.end(), typeBuf.begin(), typeBuf.end());
    out.insert(out.end(), data.begin(), data.end());
    PushBe32(out, crc);
}

//  Writes an 8 bit RGB PNG.  The source is a premultiplied BGRA buffer
//  whose first pixel is the top left corner.
bool WritePngRgb(const std::wstring& path, const BYTE* bgra, int w, int h) {
    if (!bgra || w <= 0 || h <= 0) return false;
    std::vector<BYTE> raw;
    raw.reserve((size_t)h * ((size_t)w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);  //  Filter type none.
        for (int x = 0; x < w; ++x) {
            const BYTE* p = bgra + ((size_t)y * (size_t)w + (size_t)x) * 4;
            raw.push_back(p[2]);
            raw.push_back(p[1]);
            raw.push_back(p[0]);
        }
    }
    //  zlib stream with stored (uncompressed) deflate blocks.
    std::vector<BYTE> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        const size_t chunk = std::min<size_t>(65535, raw.size() - off);
        const bool last = (off + chunk) >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back((BYTE)(chunk & 0xFF));
        z.push_back((BYTE)((chunk >> 8) & 0xFF));
        z.push_back((BYTE)(~chunk & 0xFF));
        z.push_back((BYTE)((~chunk >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + (long)off, raw.begin() + (long)(off + chunk));
        off += chunk;
    }
    PushBe32(z, Adler32(raw.data(), raw.size()));

    std::vector<BYTE> png;
    const BYTE sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), sig, sig + 8);
    std::vector<BYTE> ihdr;
    PushBe32(ihdr, (uint32_t)w);
    PushBe32(ihdr, (uint32_t)h);
    ihdr.push_back(8);  //  Bit depth.
    ihdr.push_back(2);  //  Colour type: truecolour.
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    PushChunk(png, "IHDR", ihdr);
    PushChunk(png, "IDAT", z);
    PushChunk(png, "IEND", std::vector<BYTE>());

    HANDLE fh = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(fh, png.data(), (DWORD)png.size(), &written, nullptr);
    CloseHandle(fh);
    return true;
}

//======================================================================
//  11.  The application
//======================================================================
std::wstring CurrentClockText() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%02d:%02d", st.wHour, st.wMinute);
    return buf;
}

//  The performance counter frequency, queried once.
double CpuFrequency() {
    static double freq = 0.0;
    if (freq == 0.0) {
        LARGE_INTEGER q;
        if (QueryPerformanceFrequency(&q) && q.QuadPart > 0) freq = (double)q.QuadPart;
    }
    return freq;
}

//  Wall clock milliseconds, used for the setup profiling.  GetProcessTimes
//  only advances in 15.6 ms steps, which is far too coarse to attribute
//  the start up cost to the individual stages.
double WallMs() {
    LARGE_INTEGER c;
    if (!QueryPerformanceCounter(&c)) return 0.0;
    return (double)c.QuadPart * 1000.0 / CpuFrequency();
}

double ProcessCpuMs() {
    FILETIME creation, exitTime, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user)) return 0.0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime;
    k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime;
    u.HighPart = user.dwHighDateTime;
    //  FILETIME is in 100ns units.
    return ((double)k.QuadPart + (double)u.QuadPart) / 10000.0;
}

//  CPU time the process had already spent when the entry point was
//  reached: the loader, the C runtime and the static initialisers.
double g_startupCpuMs = 0.0;

class JarvisHud {
public:
    int  Run(HINSTANCE instance, const std::wstring& cmdLine);
    void RenderFrame();  //  Called from the window procedure.
    void PaintSurface(HDC dc);  //  Called from the window procedure.
    void CountTick() { ++m_timerTicks; }
    //  Dragging the panel.  These are called from the window procedure.
    bool  OnMouseDown(HWND hwnd);
    bool  OnMouseMove(HWND hwnd);
    bool  OnMouseUp(HWND hwnd);
    ~JarvisHud() {
        if (m_singleInstance) {
            ReleaseMutex(m_singleInstance);
            CloseHandle(m_singleInstance);
        }
    }

private:
    //  --- configuration / data -----------------------------------------------
    Config        m_cfg;
    std::wstring  m_exePath;
    std::wstring  m_exeDir;
    std::wstring  m_logPath;
    std::wstring  m_fontFamilyName;
    Expanded      m_greeting;
    std::vector<GreetingLine> m_corpus;
    //  The indices into m_corpus that are eligible for the current part of
    //  the day.  Kept so that a rotating greeting can pick a new line
    //  without re-reading the corpus.
    std::vector<int> m_candidates;
    size_t        m_greetingIndex = 0;
    bool          m_configLoaded = false;

    //  --- cli -----------------------------------------------------------------
    bool m_selftest = false;
    bool m_setNotice = false;
    bool m_installAutostart = false;
    bool m_uninstallAutostart = false;
    int  m_durationMs = 0;

    //  The named mutex that keeps a second copy of the panel off the
    //  screen.  Held for as long as the process lives.
    HANDLE m_singleInstance = nullptr;

    //  --- surfaces (destruction order matters, see the member order below) ----
    GdiplusSession                       m_gdi;         //  destroyed last
    DibSurface                           m_baseDib;    //  static layer, top down
    DibSurface                           m_outDib;     //  the frame, bottom up, premultiplied
    DibSurface                           m_blitDib;    //  the frame, top down, opaque, painted
    DibSurface                           m_rotorDib;   //  premultiplied ring sprite
    std::unique_ptr<Gdiplus::Bitmap>     m_baseBmp;
    std::unique_ptr<Gdiplus::Bitmap>     m_coreBmp;    //  cached white hot core
    std::unique_ptr<Gdiplus::Bitmap>     m_bloomBmp;   //  cached ambient bloom
    std::unique_ptr<Gdiplus::Bitmap>     m_glowBmp;    //  cached reactor glow
    std::unique_ptr<Gdiplus::Bitmap>     m_rotorBmp;   //  rotating ring sprite
    std::unique_ptr<Gdiplus::Bitmap>     m_textBmp;    //  cached greeting text
    std::unique_ptr<Gdiplus::Bitmap>     m_statusBmp;  //  cached status row
    std::unique_ptr<Gdiplus::FontFamily> m_family;
    std::unique_ptr<Gdiplus::Font>       m_fontGreeting;
    std::unique_ptr<Gdiplus::Font>       m_fontStatus;

    //  --- software compositing ----------------------------------------------
    //  The cached sprites are copied out of their GDI+ bitmaps once so
    //  that every frame can be composed without a GDI+ session at all.
    std::vector<DWORD> m_glowPx;
    std::vector<DWORD> m_corePx;
    std::vector<DWORD> m_textPx;
    std::vector<DWORD> m_statusPx;
    int m_glowW = 0, m_glowH = 0;
    int m_coreW = 0, m_coreH = 0;
    int m_textW = 0, m_textH = 0;
    int m_statusW = 0, m_statusH = 0;

    HWND  m_hwnd = nullptr;
    HDC   m_screenDc = nullptr;
    UINT  m_dpi = 96;
    float m_scale = 1.0f;
    int   m_panelW = 0, m_panelH = 0;
    POINT m_windowPos = {0, 0};
    bool  m_present = true;      //  false while self testing
    bool  m_gdiPremultiplies = false;
    BYTE  m_probeArgb[4] = {0, 0, 0, 0};  //  raw sample of the alpha probe
    //  How strongly the panel is drawn, 0 to 255.  A window that is not
    //  layered cannot be transparent, so the fade is done by mixing the
    //  panel into the colour of the card.
    int   m_frameFade = 255;
    //  The part of the frame that has to be flattened again.  A width
    //  of zero means the whole panel.
    int   m_flattenX = 0, m_flattenY = 0, m_flattenW = 0, m_flattenH = 0;

    //  --- the desktop band ----------------------------------------------------
    //  The panel is an ordinary top level window that is placed directly
    //  above the shell's desktop window in the z order.  That is the
    //  "desktop band": above the wallpaper and the desktop icons, and
    //  below every application window.
    HWND  m_desktopHost = nullptr;  //  the shell's "Progman" window
    HWND  m_iconHost   = nullptr;  //  "SHELLDLL_DefView", the icon container
    bool  m_inBand     = false;  //  the panel sits in the desktop band
    ULONGLONG m_hostCheckTick = 0;  //  when the host was last looked at
    //  The dock position in screen coordinates.  m_windowPos is what
    //  UpdateLayeredWindow is given; the panel is a top level window, so
    //  that is screen coordinates.
    POINT m_screenDock = {0, 0};

    //  --- dragging the panel ----------------------------------------------------
    //  The mouse went down at this point, the window was here, and the
    //  window is here now.  All in screen pixels.
    bool  m_dragging = false;
    bool  m_dragMoved = false;
    int   m_dragStartX = 0, m_dragStartY = 0;
    int   m_dragWindowX = 0, m_dragWindowY = 0;
    int   m_dragX = 0, m_dragY = 0;

    //  --- layout ---------------------------------------------------------------
    TextLayout   m_layout;
    float        m_statusFontPx = 0.0f;
    float        m_statusAdvance = 0.0f;
    float        m_statusLineHeight = 0.0f;
    std::wstring m_statusSpaced;
    std::wstring m_clockText;

    //  --- cached dynamic assets -----------------------------------------------
    int   m_rotorSize = 0;      //  edge length of the rotating ring sprite
    float m_textBmpX = 0.0f;    //  origin of the cached text bitmap on the panel
    float m_textBmpY = 0.0f;
    float m_textMargin = 0.0f;
    float m_textLineHeight = 0.0f;
    float m_statusBmpX = 0.0f;  //  origin of the cached status row
    float m_statusBmpY = 0.0f;

    //  --- animation -------------------------------------------------------------
    ULONGLONG m_startTick = 0;
    int       m_typedChars = -1;
    double    m_lastRotation = -1.0;
    int       m_lastAlpha = -1;
    bool      m_fullInit = true;
    int       m_framesDrawn = 0;
    int       m_timerTicks = 0;
    double    m_animMs = 0.0;
    ULONGLONG m_lastPresentTick = 0;
    double    m_holdIntervalMs = 0.0;

    //  --- greeting rotation -----------------------------------------------------
    //  The greeting has a clock of its own so that it can be replaced while
    //  the panel stays on the screen.  m_greetingTick is the moment the
    //  current greeting started, m_bootStart the offset at which its boot
    //  animation begins (the first greeting waits for the fade-in, every
    //  later one starts immediately).
    ULONGLONG m_greetingTick = 0;
    double    m_bootStart = 0.0;
    int       m_rotations = 0;
    //  The factor the whole timeline is scaled by, so that --duration can
    //  stretch or squeeze the animation into a fixed lifetime.  It is
    //  computed once, from the first greeting, and never changes.
    double    m_timeScale = 1.0;

    //  --- frame profiling (filled in by the self test) -------------------------
    double m_profRestore = 0.0;
    double m_profGdi = 0.0;
    double m_profRotor = 0.0;
    double m_profPremul = 0.0;
    double m_profPresent = 0.0;
    double m_profFlatten = 0.0;
    int    m_profFrames = 0;
    double m_cpuSetupMs = 0.0;

    //  --- setup profiling ----------------------------------------------------
    double m_cpuMark = 0.0;
    std::vector<std::pair<std::string, double>> m_setupMarks;
    void  Mark(const char* name);

    void  ResetProfile() {
        m_profRestore = m_profGdi = m_profRotor = m_profPremul = m_profPresent = 0.0;
        m_profFlatten = 0.0;
        m_profFrames = 0;
    }

    //  --- helpers ---------------------------------------------------------------
    void  ParseCommandLine(const std::wstring& cmdLine);
    void  ResolveFontFamily();
    void  MarkValue(const char* name, double ms);
    bool  SetupSurfaces();
    void  DestroyWindowSurface();
    bool  ProbeGdiAlpha();
    void  RenderStaticLayer();
    void  FillPanelBody();
    void  BuildCoreBitmap();
    void  BuildBloomBitmap();
    void  BuildGlowBitmap();
    bool  BuildRotorBitmap();
    bool  BuildTextBitmap();
    void  ExtractSprite(Gdiplus::Bitmap* bmp, std::vector<DWORD>& out, int& w, int& h);
    void  BlendRect(int dstX, int dstY, const DWORD* src, int srcStride, int srcW, int srcH,
                    int srcX, int srcY, int w, int h);
    void  BlendSolidRow(int imgY, int alpha, BYTE r, BYTE g, BYTE b);
    void  DrawRotorSprite(double rotationDeg);
    void  DrawSweepSoftware(double sweepY, double alpha);
    void  DrawGreetingRuns(Gdiplus::Graphics& g, int visibleChars, float originX, float originY,
                            float lineHeight);
    void  DrawGreetingSoftware(int visibleChars);
    void  DrawStatusSoftware();
    bool  BuildStatusBitmap();
    void  FlattenFrame(int fade);
    void  FlattenFrame(int fade, int rx, int ry, int rw, int rh);
    void  Present();
    bool  CheckLifetime(double tGlobal, double tLocal, double tLocalEnd);
    void  RestoreZone(int x, int y, int w, int h);
    void  PremultiplyZone(int x, int y, int w, int h, int alphaScale);
    bool  PrepareTypography();
    bool  CreateWindowSurface(HINSTANCE instance);
    void  ApplyPanelRegion();
    void  ComputeDockPosition();
    //  --- dragging the panel ---------------------------------------------------
    //  The panel can be taken hold of with the left mouse button and
    //  moved somewhere else.  The place it is put in is written into
    //  the config file, so the panel comes back in the same place the
    //  next time the machine starts.
    void  SavePanelPosition();
    //  --- the desktop band ----------------------------------------------------
    bool  FindDesktopHost(int waitMs);
    bool  PlaceInDesktopBand();
    void  UpdateWindowPlacement();
    void  RefreshDesktopHost();
    //  --- greeting rotation ---------------------------------------------------
    bool  ChooseNextGreeting();
    int   RunSelfTest();
    int   RunSetNotice();
    bool  ShowHud();
    void  WriteLog(const std::string& text);
};

std::vector<std::wstring> TokenizeCommandLine(const std::wstring& cmd) {
    std::vector<std::wstring> tokens;
    size_t i = 0;
    while (i < cmd.size()) {
        while (i < cmd.size() && (cmd[i] == L' ' || cmd[i] == L'\t')) ++i;
        if (i >= cmd.size()) break;
        std::wstring tok;
        if (cmd[i] == L'"') {
            ++i;
            while (i < cmd.size() && cmd[i] != L'"') tok.push_back(cmd[i++]);
            if (i < cmd.size()) ++i;
        } else {
            while (i < cmd.size() && cmd[i] != L' ' && cmd[i] != L'\t') tok.push_back(cmd[i++]);
        }
        tokens.push_back(tok);
    }
    return tokens;
}

void JarvisHud::ParseCommandLine(const std::wstring& cmdLine) {
    const std::vector<std::wstring> tokens = TokenizeCommandLine(cmdLine);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::wstring& t = tokens[i];
        if (t == L"--selftest") {
            m_selftest = true;
        } else if (t == L"--set-notice") {
            m_setNotice = true;
        } else if (t == L"--install-autostart") {
            m_installAutostart = true;
        } else if (t == L"--uninstall-autostart") {
            m_uninstallAutostart = true;
        } else if (t == L"--duration" && i + 1 < tokens.size()) {
            m_durationMs = ParseInt(tokens[++i], 0);
        } else if (t == L"--log" && i + 1 < tokens.size()) {
            m_logPath = tokens[++i];
        }
    }
}

//---------------------------------------------------------------------
//  Surfaces
//---------------------------------------------------------------------
bool JarvisHud::SetupSurfaces() {
    if (!m_screenDc) m_screenDc = GetDC(nullptr);
    if (!m_screenDc) return false;

    //  The static layer is kept top down (that is the orientation GDI+
    //  expects), the frame that is composed in software is stored bottom
    //  up and the frame that is painted on the screen is stored top down
    //  in a second buffer, because the window is an ordinary window and
    //  not a layered one.
    if (!m_baseDib.Create(m_screenDc, m_panelW, m_panelH, true)) return false;
    if (!m_outDib.Create(m_screenDc, m_panelW, m_panelH, false)) return false;
    if (!m_blitDib.Create(m_screenDc, m_panelW, m_panelH, true)) return false;

    //  PixelFormat32bppPARGB makes GDI+ store premultiplied samples,
    //  which is what the software blend of the rotating ring needs and
    //  what the flattening of the frame works on.
    m_baseBmp.reset(new Gdiplus::Bitmap(m_panelW, m_panelH, m_panelW * 4,
                                         PixelFormat32bppPARGB, m_baseDib.bits));
    if (!m_baseBmp) return false;
    if (m_baseBmp->GetLastStatus() != Gdiplus::Ok) return false;
    return true;
}

void JarvisHud::DestroyWindowSurface() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_screenDc) {
        ReleaseDC(nullptr, m_screenDc);
        m_screenDc = nullptr;
    }
}

//---------------------------------------------------------------------
//  Runtime detection of the alpha convention GDI+ uses for the surface
//---------------------------------------------------------------------
//  A 32bppARGB surface may either store straight alpha or premultiplied
//  alpha.  ULW_ALPHA always needs premultiplied data, so the convention is
//  probed once and the composer adapts.
bool JarvisHud::ProbeGdiAlpha() {
    m_gdiPremultiplies = false;
    if (!m_baseDib.bits || !m_baseBmp) return true;

    //  Write a known semi transparent colour with SourceCopy, then look at
    //  the raw bytes.
    ZeroMemory(m_baseDib.bits, 16);
    {
        Gdiplus::Graphics g(m_baseBmp.get());
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        Gdiplus::SolidBrush brush(Gdiplus::Color(200, 100, 150, 200));
        g.FillRectangle(&brush, 0.0f, 0.0f, 2.0f, 2.0f);
        g.Flush(Gdiplus::FlushIntentionSync);
    }
    const DWORD px = *(const DWORD*)m_baseDib.bits;
    const BYTE a = (BYTE)((px >> 24) & 0xFF);
    const BYTE r = (BYTE)((px >> 16) & 0xFF);
    const BYTE gch = (BYTE)((px >> 8) & 0xFF);
    const BYTE b = (BYTE)(px & 0xFF);
    m_probeArgb[0] = a;
    m_probeArgb[1] = r;
    m_probeArgb[2] = gch;
    m_probeArgb[3] = b;
    //  Straight alpha would keep the colour (100,150,200), premultiplied
    //  alpha would scale it by 200/255, so compare against both.
    const int straightDist = std::abs((int)r - 100) + std::abs((int)gch - 150) +
                             std::abs((int)b - 200);
    const int premulDist = std::abs((int)r - 78) + std::abs((int)gch - 117) +
                           std::abs((int)b - 156);
    m_gdiPremultiplies = premulDist < straightDist;
    ZeroMemory(m_baseDib.bits, 16);
    return true;
}

//---------------------------------------------------------------------
//  Cached bitmaps: white hot core and the ambient reactor bloom
//---------------------------------------------------------------------
void JarvisHud::BuildCoreBitmap() {
    const int size = (int)std::ceil(kRCore * 2.0f * m_scale) + 4;
    m_coreBmp.reset(new Gdiplus::Bitmap(size, size, PixelFormat32bppPARGB));
    if (!m_coreBmp || m_coreBmp->GetLastStatus() != Gdiplus::Ok) return;
    Gdiplus::Graphics g(m_coreBmp.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float cx = size * 0.5f, cy = size * 0.5f;
    const float r = kRCore * m_scale;
    //  White hot centre fading into the reactor glow colour.
    const int steps = 12;
    for (int i = steps; i >= 1; --i) {
        const float f = (float)i / (float)steps;
        const float rr = r * f;
        const float t = 1.0f - f;  //  0 at the rim.
        const int a = (int)(255.0f * std::pow(t, 0.55f));
        const BYTE cr = (BYTE)(kReactorCore.r + (kReactorGlow.r - kReactorCore.r) * (1.0f - t));
        const BYTE cg = (BYTE)(kReactorCore.g + (kReactorGlow.g - kReactorCore.g) * (1.0f - t));
        const BYTE cb = (BYTE)(kReactorCore.b + (kReactorGlow.b - kReactorCore.b) * (1.0f - t));
        Gdiplus::SolidBrush brush(Gdiplus::Color((BYTE)std::min(a, 255), cr, cg, cb));
        g.FillEllipse(&brush, cx - rr, cy - rr, rr * 2.0f, rr * 2.0f);
    }
    g.Flush(Gdiplus::FlushIntentionSync);
    ExtractSprite(m_coreBmp.get(), m_corePx, m_coreW, m_coreH);
}

void JarvisHud::BuildBloomBitmap() {
    const int size = (int)std::ceil(kROuter * 2.0f * m_scale) + 8;
    m_bloomBmp.reset(new Gdiplus::Bitmap(size, size, PixelFormat32bppPARGB));
    if (!m_bloomBmp || m_bloomBmp->GetLastStatus() != Gdiplus::Ok) return;
    const float cx = size * 0.5f, cy = size * 0.5f;
    const float r = size * 0.5f;
    //  Hand written radial falloff - GDI+ has no blur and a path gradient
    //  brush is an order of magnitude more expensive.
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = (x + 0.5f) - cx;
            const float dy = (y + 0.5f) - cy;
            const float d = std::sqrt(dx * dx + dy * dy) / r;
            if (d >= 1.0f) continue;
            const float f = 1.0f - d;
            const int a = (int)(70.0f * f * f * f);
            if (a <= 0) continue;
            m_bloomBmp->SetPixel(x, y, Gdiplus::Color((BYTE)std::min(a, 255), kReactorGlow.r,
                                                      kReactorGlow.g, kReactorGlow.b));
        }
    }
}

//---------------------------------------------------------------------
//  Cached dynamic assets: reactor glow, rotating ring sprite, greeting
//
//  GDI+ charges a fixed overhead of roughly 50-100 microseconds per
//  antialiased vector draw call, no matter how small the shape is.  A
//  frame that draws the reactor from scratch therefore costs more than
//  half a millisecond - by far the most expensive part of the whole
//  animation.  Everything that does not change between frames is
//  therefore rendered once into a cache and blitted afterwards, and the
//  rotating ring is turned once into a sprite that is rotated in
//  software (nearest neighbour plus alpha blend), which is about five
//  times cheaper than a transformed GDI+ blit.
//---------------------------------------------------------------------
void JarvisHud::BuildGlowBitmap() {
    const float s = m_scale;
    const float rGlow = kRGlow * s;
    //  The widest stroke reaches rGlow + half of the 5px pen.
    const int size = (int)std::ceil((rGlow + 2.5f * s + 2.0f) * 2.0f);
    m_glowBmp.reset(new Gdiplus::Bitmap(size, size, PixelFormat32bppPARGB));
    if (!m_glowBmp || m_glowBmp->GetLastStatus() != Gdiplus::Ok) {
        m_glowBmp.reset();
        return;
    }
    Gdiplus::Graphics g(m_glowBmp.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    const float cx = size * 0.5f, cy = size * 0.5f;
    //  Inner glow ring - three concentric strokes build the bloom.
    Gdiplus::Pen p1(ColScaled(kReactorGlow, 45), 5.0f * s);
    Gdiplus::Pen p2(ColScaled(kReactorGlow, 90), 2.4f * s);
    Gdiplus::Pen p3(ColScaled(kReactorGlow, 190), 1.2f * s);
    g.DrawEllipse(&p1, Gdiplus::RectF(cx - rGlow, cy - rGlow, rGlow * 2.0f, rGlow * 2.0f));
    const float r2 = rGlow - 1.5f * s;
    g.DrawEllipse(&p2, Gdiplus::RectF(cx - r2, cy - r2, r2 * 2.0f, r2 * 2.0f));
    const float r3 = rGlow - 3.0f * s;
    g.DrawEllipse(&p3, Gdiplus::RectF(cx - r3, cy - r3, r3 * 2.0f, r3 * 2.0f));
    g.Flush(Gdiplus::FlushIntentionSync);
    //  The pixels are copied out of the bitmap so that the frame can be
    //  composed without opening a GDI+ session.
    ExtractSprite(m_glowBmp.get(), m_glowPx, m_glowW, m_glowH);
}

bool JarvisHud::BuildRotorBitmap() {
    const float s = m_scale;
    const float rOut = kROuter * s;
    const float rMid = kRMid * s;
    //  Radius that has to be covered: ring radius plus the tick length.
    const float extent = rOut + kTickLength * s + 2.0f * s;
    const int size = (int)std::ceil(extent * 2.0f);
    if (size < 8) return false;

    if (!m_rotorDib.Create(m_screenDc, size, size, true)) return false;
    ZeroMemory(m_rotorDib.bits, (size_t)size * (size_t)size * 4);
    //  PixelFormat32bppPARGB guarantees premultiplied samples, which is
    //  what the software blend below expects.
    m_rotorBmp.reset(new Gdiplus::Bitmap(size, size, size * 4, PixelFormat32bppPARGB,
                                         m_rotorDib.bits));
    if (!m_rotorBmp || m_rotorBmp->GetLastStatus() != Gdiplus::Ok) {
        m_rotorBmp.reset();
        return false;
    }
    m_rotorSize = size;

    Gdiplus::Graphics g(m_rotorBmp.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    const float c = size * 0.5f;

    //  Mid ring: dim track plus four arcs.  A circle is rotationally
    //  symmetric, so the track may live inside the rotating sprite.
    {
        Gdiplus::Pen track(ColScaled(kReactorRing, 60), 1.0f);
        g.DrawEllipse(&track, Gdiplus::RectF(c - rMid, c - rMid, rMid * 2.0f, rMid * 2.0f));
        const float sweep = 360.0f / (float)kMidSegments * 0.55f;
        Gdiplus::Pen arcPen(ColScaled(kReactorRing, 150), 1.6f);
        for (int i = 0; i < kMidSegments; ++i) {
            const float start = (float)i * (360.0f / (float)kMidSegments);
            g.DrawArc(&arcPen, Gdiplus::RectF(c - rMid, c - rMid, rMid * 2.0f, rMid * 2.0f),
                      start, sweep);
        }
    }

    //  Segmented outer ring: one dashed arc produces 24 evenly spaced
    //  segments in a single draw call.
    {
        const float penW = 2.0f * s;
        const float circumference = 6.2831853f * rOut;
        const float slot = circumference / (float)kRingSegments;
        Gdiplus::Pen ringPen(Col(kReactorRing), penW);
        Gdiplus::REAL dash[2] = {(slot * kRingDuty) / penW, (slot * (1.0f - kRingDuty)) / penW};
        ringPen.SetDashStyle(Gdiplus::DashStyleCustom);
        ringPen.SetDashPattern(dash, 2);
        g.DrawArc(&ringPen, Gdiplus::RectF(c - rOut, c - rOut, rOut * 2.0f, rOut * 2.0f), 0.0f,
                  360.0f);
    }

    //  Eight amber tick marks riding on the outer ring.
    {
        Gdiplus::Pen tickPen(Col(kAccentAmber), 1.6f * s);
        const double stepDeg = 360.0 / (double)kReactorTicks;
        for (int i = 0; i < kReactorTicks; ++i) {
            const double a = stepDeg * i * 3.14159265358979323846 / 180.0;
            const float ca = (float)std::cos(a);
            const float sa = (float)std::sin(a);
            g.DrawLine(&tickPen, c + ca * rOut, c + sa * rOut, c + ca * (rOut + kTickLength * s),
                       c + sa * (rOut + kTickLength * s));
        }
    }
    g.Flush(Gdiplus::FlushIntentionSync);
    return true;
}

bool JarvisHud::BuildTextBitmap() {
    if (!m_fontGreeting || m_layout.lines.empty()) return false;
    const float s = m_scale;
    const int nLines = (int)m_layout.lines.size();
    const float colW = (kTextRight - kTextX) * s;
    //  Everything is snapped to whole device pixels so that the blit of
    //  the cache is an exact one to one copy.
    const float margin = 3.0f;
    m_textLineHeight = std::ceil(m_layout.lineHeight);
    if (m_textLineHeight < 1.0f) m_textLineHeight = 1.0f;
    const float originX = std::floor(kTextX * s);
    const float originY = std::floor(kTextMidY * s - m_layout.lineHeight * (float)nLines * 0.5f);
    const int bw = (int)std::ceil(colW + margin * 2.0f);
    const int bh = (int)std::ceil(m_textLineHeight * (float)nLines + margin * 2.0f);
    if (bw <= 0 || bh <= 0) return false;

    m_textMargin = margin;
    m_textBmpX = originX - margin;
    m_textBmpY = originY - margin;

    m_textBmp.reset(new Gdiplus::Bitmap(bw, bh, PixelFormat32bppPARGB));
    if (!m_textBmp || m_textBmp->GetLastStatus() != Gdiplus::Ok) {
        m_textBmp.reset();
        return false;
    }
    Gdiplus::Graphics g(m_textBmp.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    //  The complete greeting, so that the typewriter effect only has to
    //  blit a rectangle of it.
    DrawGreetingRuns(g, 0x7FFFFFFF, margin, margin, m_textLineHeight);
    g.Flush(Gdiplus::FlushIntentionSync);
    ExtractSprite(m_textBmp.get(), m_textPx, m_textW, m_textH);
    return true;
}

//---------------------------------------------------------------------
//  Typography
//---------------------------------------------------------------------
void JarvisHud::MarkValue(const char* name, double ms) {
    m_setupMarks.push_back(std::make_pair(std::string(name), ms));
}

//  Resolves the family name, honouring the configured preference first
//  and then walking the documented fallback chain.  The construction of
//  the first family loads the system font collection and is by far the
//  most expensive step of the start up, so construction and style query
//  are measured separately.
void JarvisHud::ResolveFontFamily() {
    std::vector<std::wstring> candidates;
    if (!m_cfg.font.empty()) candidates.push_back(m_cfg.font);
    for (const wchar_t* f : kFontChain) candidates.push_back(f);

    double ctorMs = 0.0;
    double styleMs = 0.0;
    bool found = false;
    for (const std::wstring& name : candidates) {
        const double t0 = WallMs();
        const bool ok = [&]() {
            Gdiplus::FontFamily ff(name.c_str());
            const double t1 = WallMs();
            ctorMs += t1 - t0;
            const bool usable = ff.IsAvailable() && ff.IsStyleAvailable(Gdiplus::FontStyleRegular);
            styleMs += WallMs() - t1;
            return usable;
        }();
        if (ok) {
            m_fontFamilyName = name;
            found = true;
            break;
        }
    }
    if (!found) {
        const Gdiplus::FontFamily* generic = Gdiplus::FontFamily::GenericMonospace();
        if (generic && generic->IsAvailable()) {
            WCHAR name[LF_FACESIZE];
            if (generic->GetFamilyName(name) == Gdiplus::Ok) m_fontFamilyName = name;
        }
        if (m_fontFamilyName.empty()) m_fontFamilyName = L"Courier New";
    }
    MarkValue("t-family", ctorMs);
    MarkValue("t-style", styleMs);
    Mark("t-resolve");
}

bool JarvisHud::PrepareTypography() {
    ResolveFontFamily();
    m_family.reset(new Gdiplus::FontFamily(m_fontFamilyName.c_str()));
    if (!m_family || m_family->GetLastStatus() != Gdiplus::Ok || !m_family->IsAvailable())
        return false;

    const int greetingPx = std::max(6, m_cfg.fontSize);
    const int statusPx = std::max(6, m_cfg.statusFontSize);
    m_fontGreeting.reset(new Gdiplus::Font(m_family.get(), (Gdiplus::REAL)(greetingPx * m_scale),
                                            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel));
    m_fontStatus.reset(new Gdiplus::Font(m_family.get(), (Gdiplus::REAL)(statusPx * m_scale),
                                         Gdiplus::FontStyleRegular, Gdiplus::UnitPixel));
    if (!m_fontGreeting || !m_fontStatus) return false;
    if (m_fontGreeting->GetLastStatus() != Gdiplus::Ok) return false;
    if (m_fontStatus->GetLastStatus() != Gdiplus::Ok) return false;
    Mark("t-font");

    const float column = (kTextRight - kTextX) * m_scale;
    m_layout = ComputeLayout(m_greeting.text, m_cfg, column, m_family.get(), m_scale);
    Mark("t-layout");

    m_statusFontPx = (float)statusPx * m_scale;
    m_statusAdvance = MeasureAdvance(m_family.get(), m_statusFontPx);
    m_statusLineHeight = MeasureLineHeight(m_family.get(), m_statusFontPx);
    Mark("t-measure");

    //  Letter spaced small caps look: a space between every character and
    //  three spaces between words.
    m_statusSpaced.clear();
    for (size_t i = 0; i < m_cfg.statusText.size(); ++i) {
        const wchar_t ch = m_cfg.statusText[i];
        if (ch == L' ' || ch == L'\t') {
            m_statusSpaced.append(L"   ");
        } else {
            m_statusSpaced.push_back(ch);
            m_statusSpaced.push_back(L' ');
        }
    }
    while (!m_statusSpaced.empty() && m_statusSpaced[m_statusSpaced.size() - 1] == L' ')
        m_statusSpaced.erase(m_statusSpaced.size() - 1);
    return true;
}

//---------------------------------------------------------------------
//  The panel body
//
//  A solid rounded rectangle is written straight into the static layer.
//  The straight part is one colour per pixel, the four corners are
//  antialiased with the signed distance of the corner circle.  The
//  samples are premultiplied, the same convention the compositor wants.
//---------------------------------------------------------------------
void JarvisHud::FillPanelBody() {
    if (!m_baseDib.bits) return;
    const int w = m_panelW, h = m_panelH;
    const size_t stride = (size_t)w * 4;
    ZeroMemory(m_baseDib.bits, stride * (size_t)h);
    if (w < 4 || h < 4) return;

    const double r = std::min((double)kCornerRadius * m_scale, (double)std::min(w, h) * 0.5);
    const int ri = (int)std::ceil(r);
    const BYTE fa = kPanelFill.a;
    const BYTE fr = PremulChannel(kPanelFill.r, fa);
    const BYTE fg = PremulChannel(kPanelFill.g, fa);
    const BYTE fb = PremulChannel(kPanelFill.b, fa);
    const DWORD solid = ((DWORD)fa << 24) | ((DWORD)fr << 16) | ((DWORD)fg << 8) | fb;

    //  The straight part of the panel.
    for (int y = 0; y < h; ++y) {
        DWORD* row = (DWORD*)(m_baseDib.bits + (size_t)y * stride);
        for (int x = 0; x < w; ++x) row[x] = solid;
    }

    //  The four corners.
    for (int cy = 0; cy < 2; ++cy) {
        const double oy = (cy == 0) ? 0.0 : (double)(h - ri);
        for (int cx = 0; cx < 2; ++cx) {
            const double ox = (cx == 0) ? 0.0 : (double)(w - ri);
            for (int y = 0; y < ri; ++y) {
                const double qy = r - (y + 0.5 - oy);
                if (qy <= 0.0) continue;
                DWORD* row = (DWORD*)(m_baseDib.bits + (size_t)y * stride);
                for (int x = 0; x < ri; ++x) {
                    const double qx = r - (x + 0.5 - ox);
                    if (qx <= 0.0) continue;
                    const double d = std::sqrt(qx * qx + qy * qy) - r;
                    double cov = 0.5 - d;
                    if (cov <= 0.0) {
                        row[x] = 0;
                        continue;
                    }
                    if (cov > 1.0) cov = 1.0;
                    const BYTE a = (BYTE)((solid >> 24) * cov + 0.5);
                    const BYTE rr = (BYTE)(((solid >> 16) & 0xFFu) * cov + 0.5);
                    const BYTE gg = (BYTE)(((solid >> 8) & 0xFFu) * cov + 0.5);
                    const BYTE bb = (BYTE)((solid & 0xFFu) * cov + 0.5);
                    row[x] = ((DWORD)a << 24) | ((DWORD)rr << 16) | ((DWORD)gg << 8) | bb;
                }
            }
        }
    }
}

//---------------------------------------------------------------------
//  Static layer
//---------------------------------------------------------------------
void JarvisHud::RenderStaticLayer() {
    //  The panel body is filled by hand.  GDI+ needs about fifteen
    //  milliseconds for a solid rounded rectangle of this size, the
    //  panel is the largest surface of the program and it never changes
    //  afterwards.
    FillPanelBody();
    Mark("s-fill");

    Gdiplus::Graphics g(m_baseBmp.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

    const float w = (float)m_panelW;
    const float h = (float)m_panelH;
    const float radius = kCornerRadius * m_scale;

    Gdiplus::GraphicsPath panel;
    BuildRoundedRect(panel, 0.0f, 0.0f, w, h, radius);

    //  Faint background grid, clipped to the panel body
    g.SetClip(&panel);
    const float step = kGridStep * m_scale;
    Gdiplus::Pen gridPen(Col(kGridLine), 1.0f);
    for (float x = step; x < w; x += step) {
        const float px = std::floor(x) + 0.5f;
        g.DrawLine(&gridPen, px, 0.0f, px, h);
    }
    for (float y = step; y < h; y += step) {
        const float py = std::floor(y) + 0.5f;
        g.DrawLine(&gridPen, 0.0f, py, w, py);
    }
    g.ResetClip();
    Mark("s-grid");

    //  1px border, inset so the stroke sits completely inside the panel
    Gdiplus::GraphicsPath border;
    BuildRoundedRect(border, 0.5f, 0.5f, w - 1.0f, h - 1.0f, std::max(0.5f, radius - 0.5f));
    Gdiplus::Pen borderPen(Col(kPanelEdge), 1.0f);
    g.DrawPath(&borderPen, &border);

    //  Corner brackets: open L shapes, 20px arms, 2px thick, 6px inset
    const float arm = kBracketArm * m_scale;
    const float ins = kBracketInset * m_scale;
    const float t = kBracketThick * m_scale;
    Gdiplus::Pen bracketPen(Col(kBracket), t);
    bracketPen.SetStartCap(Gdiplus::LineCapSquare);
    bracketPen.SetEndCap(Gdiplus::LineCapSquare);
    const float half = t * 0.5f;
    const float x0 = ins + half, y0 = ins + half;
    const float x1 = w - ins - half, y1 = h - ins - half;
    g.DrawLine(&bracketPen, x0, y0, x0 + arm, y0);
    g.DrawLine(&bracketPen, x0, y0, x0, y0 + arm);
    g.DrawLine(&bracketPen, x1 - arm, y0, x1, y0);
    g.DrawLine(&bracketPen, x1, y0, x1, y0 + arm);
    g.DrawLine(&bracketPen, x0, y1 - arm, x0, y1);
    g.DrawLine(&bracketPen, x0, y1, x0 + arm, y1);
    g.DrawLine(&bracketPen, x1 - arm, y1, x1, y1);
    g.DrawLine(&bracketPen, x1, y1 - arm, x1, y1);

    //  Separator hairline above the status line
    Gdiplus::Pen sepPen(ColScaled(kPanelEdge, 90), 1.0f);
    const float sx = std::floor(kTextX * m_scale) + 0.5f;
    const float ex = std::floor(kTextRight * m_scale) - 0.5f;
    const float sy = std::floor(kSeparatorY * m_scale) + 0.5f;
    g.DrawLine(&sepPen, sx, sy, ex, sy);

    //  Ambient reactor bloom (static, drawn once)
    if (m_bloomBmp) {
        const float bx = kReactorX * m_scale - m_bloomBmp->GetWidth() * 0.5f;
        const float by = kReactorY * m_scale - m_bloomBmp->GetHeight() * 0.5f;
        g.DrawImage(m_bloomBmp.get(), bx, by);
    }

    //  The glow ring and the white hot core never move either, so they
    //  are baked into the base layer.  Only the turning ring has to be
    //  composited per frame.
    if (!m_glowPx.empty() && m_glowBmp) {
        g.DrawImage(m_glowBmp.get(), std::floor(kReactorX * m_scale - m_glowW * 0.5f),
                    std::floor(kReactorY * m_scale - m_glowH * 0.5f));
    }
    if (!m_corePx.empty() && m_coreBmp) {
        g.DrawImage(m_coreBmp.get(), std::floor(kReactorX * m_scale - m_coreW * 0.5f),
                    std::floor(kReactorY * m_scale - m_coreH * 0.5f));
    }

    g.Flush(Gdiplus::FlushIntentionSync);
    Mark("s-rest");
}

//---------------------------------------------------------------------
//  Software compositing
//
//  The frame buffer holds premultiplied samples and is stored bottom
//  up, which is exactly what UpdateLayeredWindow(ULW_ALPHA) expects.
//  Every cached sprite is blended with  dst = src + dst * (1 - srcA),
//  which needs neither a division nor a temporary buffer, so a frame
//  can be composed without opening a GDI+ session at all.
//---------------------------------------------------------------------
void JarvisHud::ExtractSprite(Gdiplus::Bitmap* bmp, std::vector<DWORD>& out, int& w, int& h) {
    out.clear();
    w = 0;
    h = 0;
    if (!bmp) return;
    const int bw = (int)bmp->GetWidth();
    const int bh = (int)bmp->GetHeight();
    if (bw <= 0 || bh <= 0) return;
    Gdiplus::Rect r(0, 0, bw, bh);
    Gdiplus::BitmapData data;
    if (bmp->LockBits(&r, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok)
        return;
    const BYTE* src = (const BYTE*)data.Scan0;
    const int stride = (int)data.Stride;
    out.resize((size_t)bw * (size_t)bh);
    for (int y = 0; y < bh; ++y) {
        const BYTE* s = src + (size_t)y * stride;
        DWORD* d = &out[(size_t)y * bw];
        for (int x = 0; x < bw; ++x) {
            const DWORD px = *(const DWORD*)(s + (size_t)x * 4);
            //  Should GDI+ ever hand back straight alpha, the samples are
            //  converted to premultiplied here, once.
            d[x] = m_gdiPremultiplies ? px : PremulPixel(px, (BYTE)(px >> 24));
        }
    }
    bmp->UnlockBits(&data);
    w = bw;
    h = bh;
}

//  Blends a rectangle of a premultiplied sprite over the frame.  The
//  destination is given in image space (top left origin) while the
//  frame itself is stored bottom up.
void JarvisHud::BlendRect(int dstX, int dstY, const DWORD* src, int srcStride, int srcW, int srcH,
                           int srcX, int srcY, int w, int h) {
    if (!src || w <= 0 || h <= 0 || !m_outDib.bits) return;
    const int dstStride = m_outDib.w * 4;
    int x0 = 0;
    int x1 = w;
    if (dstX < 0) x0 = -dstX;
    if (dstX + x1 > m_outDib.w) x1 = m_outDib.w - dstX;
    if (x1 > srcW - srcX) x1 = srcW - srcX;
    if (x0 >= x1) return;

    for (int y = 0; y < h; ++y) {
        const int iy = dstY + y;
        if (iy < 0 || iy >= m_outDib.h) continue;
        const int sy = srcY + y;
        if (sy < 0 || sy >= srcH) continue;
        DWORD* row = (DWORD*)(m_outDib.bits + (size_t)(m_outDib.h - 1 - iy) * dstStride);
        const DWORD* s = src + (size_t)sy * srcStride + srcX;
        for (int x = x0; x < x1; ++x) {
            const DWORD sv = s[x];
            const BYTE sa = (BYTE)(sv >> 24);
            if (sa == 0) continue;
            const DWORD dv = row[dstX + x];
            const BYTE da = (BYTE)(dv >> 24);
            const BYTE inv = (BYTE)(255u - sa);
            const BYTE r = (BYTE)((sv & 0xFFu) + PremulChannel((BYTE)(dv & 0xFFu), inv));
            const BYTE gg = (BYTE)(((sv >> 8) & 0xFFu) + PremulChannel((BYTE)((dv >> 8) & 0xFFu), inv));
            const BYTE b = (BYTE)(((sv >> 16) & 0xFFu) + PremulChannel((BYTE)((dv >> 16) & 0xFFu), inv));
            const BYTE a2 = (BYTE)(sa + PremulChannel(da, inv));
            row[dstX + x] = ((DWORD)a2 << 24) | ((DWORD)b << 16) | ((DWORD)gg << 8) | (DWORD)r;
        }
    }
}

//  Blends a solid colour into a single row of the frame.
void JarvisHud::BlendSolidRow(int imgY, int alpha, BYTE r, BYTE g, BYTE b) {
    if (alpha <= 0 || !m_outDib.bits) return;
    if (alpha > 255) alpha = 255;
    if (imgY < 0 || imgY >= m_outDib.h) return;
    const int stride = m_outDib.w * 4;
    DWORD* row = (DWORD*)(m_outDib.bits + (size_t)(m_outDib.h - 1 - imgY) * stride);
    const BYTE sa = (BYTE)alpha;
    const BYTE sr = PremulChannel(r, sa);
    const BYTE sg = PremulChannel(g, sa);
    const BYTE sb = PremulChannel(b, sa);
    const BYTE inv = (BYTE)(255u - sa);
    for (int x = 0; x < m_outDib.w; ++x) {
        const DWORD dv = row[x];
        const BYTE da = (BYTE)(dv >> 24);
        //  The sample is 0xAARRGGBB: red is in bits 16-23, blue in bits
        //  0-7.  The named channels have to go into the same places.
        const BYTE nr = (BYTE)(sr + PremulChannel((BYTE)((dv >> 16) & 0xFFu), inv));
        const BYTE ng = (BYTE)(sg + PremulChannel((BYTE)((dv >> 8) & 0xFFu), inv));
        const BYTE nb = (BYTE)(sb + PremulChannel((BYTE)(dv & 0xFFu), inv));
        const BYTE na = (BYTE)(sa + PremulChannel(da, inv));
        row[x] = ((DWORD)na << 24) | ((DWORD)nr << 16) | ((DWORD)ng << 8) | (DWORD)nb;
    }
}

//  The frame used to be faded by scaling the alpha of every sample,
//  which only works for a layered window.  The window is not layered
//  any more, so the fade is done by FlattenFrame(), which mixes the
//  panel into the colour of the card.

//  The scan line that travels down the panel while the reactor spins up.
void JarvisHud::DrawSweepSoftware(double sweepY, double alpha) {
    if (alpha <= 0.0) return;
    const int a = (int)std::max(0.0, std::min(255.0, alpha * 255.0));
    const float tail = 46.0f * m_scale;
    const int headY = (int)std::floor(sweepY);
    const int tailY = (int)std::floor((float)sweepY - tail);
    if (headY > tailY) {
        const int rows = headY - tailY;
        for (int y = tailY; y < headY; ++y) {
            const int rowAlpha = (a * (y - tailY + 1)) / rows;
            if (rowAlpha <= 0) continue;
            BlendSolidRow(y, rowAlpha, kSweepLine.r, kSweepLine.g, kSweepLine.b);
        }
    }
    //  Bright head of the scan line.
    const int headAlpha = std::min(255, a + 60);
    BlendSolidRow(headY, headAlpha, kSweepLine.r, kSweepLine.g, kSweepLine.b);
    BlendSolidRow(headY + 1, headAlpha, kSweepLine.r, kSweepLine.g, kSweepLine.b);
}

//  The typewriter effect: the complete greeting lives in a cache, so
//  only the part that is already typed has to be blitted.
void JarvisHud::DrawGreetingSoftware(int visibleChars) {
    if (visibleChars <= 0 || m_textPx.empty()) return;
    const int dstX = (int)std::lround(m_textBmpX);
    const int dstY = (int)std::lround(m_textBmpY);
    const int margin = (int)std::lround(m_textMargin);
    int remaining = visibleChars;
    for (size_t li = 0; li < m_layout.lines.size() && remaining > 0; ++li) {
        const TextLine& line = m_layout.lines[li];
        const int shown = std::min(remaining, line.len);
        remaining -= shown;
        if (shown <= 0) continue;
        const int srcY = margin + (int)std::lround(m_textLineHeight * (float)li);
        int w = (int)std::ceil((float)shown * m_layout.advance + 0.75f);
        if (margin + w > m_textW) w = m_textW - margin;
        int h = (int)std::lround(m_textLineHeight);
        if (srcY + h > m_textH) h = m_textH - srcY;
        if (w <= 0 || h <= 0) continue;
        BlendRect(dstX + margin, dstY + srcY, m_textPx.data(), m_textW, m_textW, m_textH, margin,
                  srcY, w, h);
    }
}

//  The status row (letter spaced label and the clock) is cached and
//  only has to be blitted.
void JarvisHud::DrawStatusSoftware() {
    if (m_statusPx.empty()) return;
    const int dstX = (int)std::lround(m_statusBmpX);
    const int dstY = (int)std::lround(m_statusBmpY);
    BlendRect(dstX, dstY, m_statusPx.data(), m_statusW, m_statusW, m_statusH, 0, 0, m_statusW,
              m_statusH);
}

//---------------------------------------------------------------------
//  Software rotation of the ring sprite
//
//  The sprite holds the segmented outer ring, the amber ticks and the
//  mid ring.  It is sampled with an inverse rotation (nearest
//  neighbour) and alpha blended into the working surface.  Both the
//  sprite and the working surface hold premultiplied samples, so the
//  blend is  dst = src + dst * (1 - srcA)  and needs no division at
//  all.
//---------------------------------------------------------------------
void JarvisHud::DrawRotorSprite(double rotationDeg) {
    if (m_rotorSize <= 0 || !m_outDib.bits || !m_rotorDib.bits) return;
    const int sp = m_rotorSize;
    const BYTE* src = m_rotorDib.bits;
    const int srcStride = sp * 4;
    const int dstStride = m_outDib.w * 4;

    const int sc = sp / 2;
    const int cx = (int)std::lround(kReactorX * m_scale);
    const int cy = (int)std::lround(kReactorY * m_scale);
    const int dx0 = cx - sc;
    const int dy0 = cy - sc;

    //  The rotation is kept in 16.16 fixed point, so the inner loop only
    //  needs an addition and a shift per pixel.
    const double rad = rotationDeg * 3.14159265358979323846 / 180.0;
    const int cs = (int)std::lround(std::cos(rad) * 65536.0);
    const int sn = (int)std::lround(std::sin(rad) * 65536.0);

    //  The destination rectangle that actually lies on the surface.
    const int xBegin = (dx0 < 0) ? -dx0 : 0;
    const int xEnd = (dx0 + sp > m_outDib.w) ? (m_outDib.w - dx0) : sp;
    if (xBegin >= xEnd) return;

    for (int y = 0; y < sp; ++y) {
        const int py = dy0 + y;
        if (py < 0 || py >= m_outDib.h) continue;
        //  Source position of the first pixel of this row, in 16.16 fixed point.
        const int64_t rx = (((int64_t)(xBegin - sc)) << 16) + 32768;
        const int64_t ry = (((int64_t)(y - sc)) << 16) + 32768;
        int sx = (int)((((int64_t)cs * rx + (int64_t)sn * ry) >> 16) + ((int64_t)sc << 16));
        int sy = (int)(((-(int64_t)sn * rx + (int64_t)cs * ry) >> 16) + ((int64_t)sc << 16));
        DWORD* row = (DWORD*)(m_outDib.bits + (size_t)(m_outDib.h - 1 - py) * dstStride);
        for (int x = xBegin; x < xEnd; ++x, sx += cs, sy -= sn) {
            const int ix = sx >> 16;
            const int iy = sy >> 16;
            if ((unsigned)ix >= (unsigned)sp || (unsigned)iy >= (unsigned)sp) continue;
            const DWORD sv = *(const DWORD*)(src + (size_t)iy * srcStride + (size_t)ix * 4);
            const BYTE sa = (BYTE)(sv >> 24);
            if (sa == 0) continue;
            DWORD* dp = row + dx0 + x;
            const DWORD dv = *dp;
            const BYTE da = (BYTE)(dv >> 24);
            const BYTE inv = (BYTE)(255u - sa);
            //  Premultiplied "over".
            const BYTE r = (BYTE)((sv & 0xFFu) + PremulChannel((BYTE)(dv & 0xFFu), inv));
            const BYTE gg = (BYTE)(((sv >> 8) & 0xFFu) + PremulChannel((BYTE)((dv >> 8) & 0xFFu), inv));
            const BYTE b = (BYTE)(((sv >> 16) & 0xFFu) + PremulChannel((BYTE)((dv >> 16) & 0xFFu), inv));
            const BYTE a2 = (BYTE)(sa + PremulChannel(da, inv));
            *dp = ((DWORD)a2 << 24) | ((DWORD)b << 16) | ((DWORD)gg << 8) | (DWORD)r;
        }
    }
}

//  Draws the greeting as a set of bright / primary runs.  originX and
//  originY are the top left corner of the text block.
void JarvisHud::DrawGreetingRuns(Gdiplus::Graphics& g, int visibleChars, float originX,
                                  float originY, float lineHeight) {
    if (!m_fontGreeting) return;
    Gdiplus::SolidBrush bright(Col(kTextBright));
    Gdiplus::SolidBrush primary(Col(kTextPrimary));

    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip |
                       Gdiplus::StringFormatFlagsNoFitBlackBox);

    const int nLines = (int)m_layout.lines.size();

    int remaining = visibleChars;
    for (int li = 0; li < nLines && remaining > 0; ++li) {
        const TextLine& line = m_layout.lines[(size_t)li];
        const int shown = std::min(remaining, line.len);
        remaining -= shown;
        if (shown <= 0) continue;
        const float top = originY + lineHeight * (float)li;

        //  Split the visible part into bright / primary runs.
        int cursor = line.start;
        const int end = line.start + shown;
        while (cursor < end) {
            int runEnd = end;
            bool isBright = false;
            for (const auto& range : m_greeting.bright) {
                const int rs = range.first, re = range.first + range.second;
                if (cursor >= rs && cursor < re) {
                    isBright = true;
                    runEnd = std::min(end, re);
                    break;
                }
                if (rs > cursor) {
                    runEnd = std::min(end, rs);
                    break;
                }
            }
            const int len = runEnd - cursor;
            if (len > 0) {
                const float x = originX + (float)(cursor - line.start) * m_layout.advance;
                const Gdiplus::RectF layout(x, top, 8000.0f, lineHeight + 8.0f);
                g.DrawString(m_greeting.text.c_str() + cursor, len, m_fontGreeting.get(), layout, &fmt,
                             isBright ? &bright : &primary);
            }
            cursor = runEnd;
        }
    }
}

//  The status row is rendered once and reused; it only has to be drawn
//  again when the clock ticks over to the next minute.
bool JarvisHud::BuildStatusBitmap() {
    if (!m_fontStatus) return false;
    const float s = m_scale;
    const float lineHeight = m_statusLineHeight;
    const float top = kStatusY * s - lineHeight * 0.5f;
    const float originX = std::floor(kTextX * s);
    const float originY = std::floor(top);
    const float width = std::ceil(kTextRight * s - originX) + 2.0f;
    const float height = std::ceil(lineHeight + 8.0f);
    const int bw = (int)width;
    const int bh = (int)height;
    if (bw <= 0 || bh <= 0) return false;

    m_statusBmpX = originX;
    m_statusBmpY = originY;
    m_statusBmp.reset(new Gdiplus::Bitmap(bw, bh, PixelFormat32bppPARGB));
    if (!m_statusBmp || m_statusBmp->GetLastStatus() != Gdiplus::Ok) {
        m_statusBmp.reset();
        return false;
    }
    Gdiplus::Graphics g(m_statusBmp.get());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    Gdiplus::SolidBrush dim(Col(kTextDim));
    Gdiplus::SolidBrush value(Col(kTextPrimary));
    Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
    fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap | Gdiplus::StringFormatFlagsNoClip |
                       Gdiplus::StringFormatFlagsNoFitBlackBox);

    if (!m_statusSpaced.empty())
        g.DrawString(m_statusSpaced.c_str(), -1, m_fontStatus.get(),
                     Gdiplus::RectF(0.0f, 0.0f, 8000.0f, height), &fmt, &dim);

    if (m_cfg.showClock && !m_clockText.empty()) {
        const int chars = (int)m_clockText.size();
        const float cw = (float)chars * m_statusAdvance;
        const float x = (kTextRight * s - originX) - cw;
        g.DrawString(m_clockText.c_str(), -1, m_fontStatus.get(),
                     Gdiplus::RectF(x, 0.0f, cw + 8.0f, height), &fmt, &value);
    }
    g.Flush(Gdiplus::FlushIntentionSync);
    ExtractSprite(m_statusBmp.get(), m_statusPx, m_statusW, m_statusH);
    return true;
}

//---------------------------------------------------------------------
//  Zone restore
//
//  The static layer is kept in a top down DIB, the frame that is handed
//  to UpdateLayeredWindow is stored bottom up, so the rows have to be
//  mirrored while they are copied.
//---------------------------------------------------------------------
void JarvisHud::RestoreZone(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    const int stride = m_baseDib.w * 4;
    const int outStride = m_outDib.w * 4;
    for (int row = 0; row < h; ++row) {
        const int sy = y + row;
        if (sy < 0 || sy >= m_baseDib.h) continue;
        const BYTE* s = m_baseDib.bits + (size_t)sy * stride + (size_t)x * 4;
        //  The frame is bottom up, so image row sy lives in row h-1-sy.
        BYTE* d = m_outDib.bits + (size_t)(m_outDib.h - 1 - sy) * outStride + (size_t)x * 4;
        memcpy(d, s, (size_t)w * 4);
    }
}

//---------------------------------------------------------------------
//  Flattening and painting
//
//  The panel is composed in premultiplied alpha, but the window it is
//  painted into is an ordinary window.  That is not a detail: a
//  layered window (WS_EX_LAYERED) is blended by the desktop window
//  manager in a pass of its own, which puts it in front of every
//  ordinary window no matter what the z order says.  A panel that has
//  to stay behind the running applications therefore cannot be layered.
//
//  So the frame is flattened onto the colour of the card and painted
//  opaque.  The corners are cut away by the window region, and the
//  fade is a mix between the card and the finished panel, because a
//  window that is not layered cannot be made transparent.
//---------------------------------------------------------------------
void JarvisHud::FlattenFrame(int fade) {
    FlattenFrame(fade, 0, 0, m_panelW, m_panelH);
}

//  Flattens only the given rectangle of the frame.  The rest of the
//  flattened surface is left as it is, which is what makes this worth
//  doing: the panel is flattened thirty times a second for as long as
//  it is on the screen, and on a normal frame only the reactor has
//  moved.  The pixels outside the rectangle have not changed and the
//  fade has not changed either, so the flattened values that are
//  already there are still the right ones.
void JarvisHud::FlattenFrame(int fade, int rx, int ry, int rw, int rh) {
    if (!m_outDib.bits || !m_blitDib.bits) return;
    if (fade < 0) fade = 0;
    if (fade > 255) fade = 255;

    const int w = m_panelW, h = m_panelH;
    if (w <= 0 || h <= 0) return;

    //  The rectangle is clamped to the panel.
    int x0 = rx, y0 = ry, x1 = rx + rw - 1, y1 = ry + rh - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w - 1) x1 = w - 1;
    if (y1 > h - 1) y1 = h - 1;
    if (x1 < x0 || y1 < y0) return;

    const int srcStride = m_outDib.w * 4;
    const int dstStride = m_blitDib.w * 4;
    const int br = kPanelFill.r, bg = kPanelFill.g, bb = kPanelFill.b;

    //  Both steps of the flattening are tables.  The card under the
    //  panel depends only on the alpha of the sample, and the fade
    //  only on the value of the channel, so the whole frame is put
    //  together with lookups and without a single division.
    static BYTE overR[256], overG[256], overB[256];
    static bool overReady = false;
    if (!overReady) {
        for (int a = 0; a < 256; ++a) {
            overR[a] = (BYTE)((br * (255 - a) + 127) / 255);
            overG[a] = (BYTE)((bg * (255 - a) + 127) / 255);
            overB[a] = (BYTE)((bb * (255 - a) + 127) / 255);
        }
        overReady = true;
    }

    BYTE fadeR[256], fadeG[256], fadeB[256];
    for (int v = 0; v < 256; ++v) {
        fadeR[v] = ClampByte(br + MixRound(v - br, fade));
        fadeG[v] = ClampByte(bg + MixRound(v - bg, fade));
        fadeB[v] = ClampByte(bb + MixRound(v - bb, fade));
    }

    for (int y = y0; y <= y1; ++y) {
        //  The composed frame is stored bottom up, the painted one top
        //  down, so the rows have to be read the other way round.
        const DWORD* s = (const DWORD*)(m_outDib.bits + (size_t)(h - 1 - y) * (size_t)srcStride);
        DWORD* d = (DWORD*)(m_blitDib.bits + (size_t)y * (size_t)dstStride);
        for (int x = x0; x <= x1; ++x) {
            const DWORD p = s[x];
            const int sa = (int)(p >> 24);
            const int cr = (int)((p >> 16) & 0xFFu) + overR[sa];
            const int cg = (int)((p >> 8) & 0xFFu) + overG[sa];
            const int cb = (int)(p & 0xFFu) + overB[sa];
            d[x] = 0xFF000000u | ((DWORD)fadeR[cr] << 16) | ((DWORD)fadeG[cg] << 8) |
                   (DWORD)fadeB[cb];
        }
    }
}

//  Paints the finished frame into the window.
void JarvisHud::PaintSurface(HDC dc) {
    if (!dc || !m_blitDib.bits || m_panelW <= 0 || m_panelH <= 0) return;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = m_panelW;
    bi.bmiHeader.biHeight = -m_panelH;   //  Negative = top down.
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(dc, 0, 0, m_panelW, m_panelH, 0, 0, 0, m_panelH,
                      m_blitDib.bits, &bi, DIB_RGB_COLORS);
}

void JarvisHud::Present() {
    if (!m_present || !m_hwnd || !m_outDib.bits) return;
    LARGE_INTEGER f0, f1;
    QueryPerformanceCounter(&f0);
    if (m_flattenW > 0 && m_flattenH > 0)
        FlattenFrame(m_frameFade, m_flattenX, m_flattenY, m_flattenW, m_flattenH);
    else
        FlattenFrame(m_frameFade);
    QueryPerformanceCounter(&f1);
    m_profFlatten += (double)(f1.QuadPart - f0.QuadPart) * (1000.0 / CpuFrequency());    RedrawWindow(m_hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE | RDW_NOCHILDREN);
}

//---------------------------------------------------------------------
//  Frame
//---------------------------------------------------------------------
void JarvisHud::RenderFrame() {
    if (!m_hwnd || !m_baseBmp) return;

    const ULONGLONG now = GetTickCount64();

    //  ---- the desktop band ---------------------------------------------------
    //  The panel has to stay in the band: the shell raises its own desktop
    //  window from time to time, which would push the panel behind the
    //  wallpaper.  The check is cheap and runs once a second.
    //
    //  If the shell has not turned up yet the panel is not shown at all,
    //  and it is put on the screen as soon as the band is there.  That
    //  way the panel is never on the screen outside the band, which is
    //  the only place it is allowed to be.
    if (now - m_hostCheckTick >= 1000) {
        m_hostCheckTick = now;
        if (m_inBand) {
            RefreshDesktopHost();
        } else if (FindDesktopHost(0) && PlaceInDesktopBand()) {
            //  The shell turned up: the panel may go on the screen now.
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            UpdateWindowPlacement();
            m_fullInit = true;
        }
    }

    //  ---- a new greeting ----------------------------------------------------
    if (m_cfg.rotateSeconds > 0 && m_greetingTick != 0) {
        const ULONGLONG due = m_greetingTick + (ULONGLONG)m_cfg.rotateSeconds * 1000ULL;
        if (now >= due) {
            //  Only swap once the greeting has been typed out, so that a
            //  long line is never cut off half way through.
            const double since = (double)(now - m_greetingTick);
            const double typeDur = (double)std::max(1, m_cfg.typeMs) * m_timeScale *
                                   (double)m_greeting.text.size();
            const double spinUp = (double)std::max(0, m_cfg.bootMs) * m_timeScale;
            if (since >= m_bootStart + spinUp + typeDur) {
                ChooseNextGreeting();
            }
        }
    }

    //  Two clocks.  The global one runs from the moment the panel was
    //  shown and drives the fade in and the lifetime of the process.  The
    //  local one runs from the moment the greeting on screen started and
    //  drives the boot, the typing and the hold, so that a greeting can
    //  be replaced while the panel stays where it is.
    const double t = (double)(now - m_startTick);
    const double tg = m_greetingTick ? (double)(now - m_greetingTick) : t;

    //  ---- timeline -----------------------------------------------------------
    const double fadeIn = (double)std::max(0, m_cfg.fadeInMs) * m_timeScale;
    const double boot = (double)std::max(0, m_cfg.bootMs) * m_timeScale;
    const int charCount = (int)m_greeting.text.size();
    const double typeStep = (double)std::max(1, m_cfg.typeMs) * m_timeScale;
    const double typeDur = typeStep * (double)charCount;
    const double hold = (double)std::max(0, m_cfg.holdMs) * m_timeScale;
    const double fadeOut = (double)std::max(0, m_cfg.fadeOutMs) * m_timeScale;

    const double tBootEnd = m_bootStart + boot;
    const double tTypeEnd = tBootEnd + typeDur;
    const double tHoldEnd = tTypeEnd + hold;
    const double tEnd = tHoldEnd + fadeOut;

    int alpha = 255;
    bool fading = false;
    if (t < fadeIn) {
        alpha = (int)(255.0 * (fadeIn > 0.0 ? t / fadeIn : 1.0));
        fading = true;
    } else if (!m_cfg.persist && tg >= tHoldEnd && tg < tEnd) {
        const double f = fadeOut > 0.0 ? (tg - tHoldEnd) / fadeOut : 1.0;
        alpha = (int)(255.0 * (1.0 - ClampD(f, 0.0, 1.0)));
        fading = true;
    }
    alpha = std::max(0, std::min(255, alpha));

    //  Reactor rotation: spins up over the boot window, then idles.
    //  The speed is a setting, because the ring looked too busy at the
    //  speed it was built with.
    const double speed = (m_cfg.reactorSpeed > 0 ? m_cfg.reactorSpeed : 100) / 100.0;
    double spinT = tg - m_bootStart;
    if (spinT < 0.0) spinT = 0.0;
    double travelled;
    if (boot > 0.0 && spinT < boot) {
        const double rate = 60.0 * speed;  //  Degrees per second while spinning up.
        travelled = rate * spinT * spinT / (2.0 * boot);
    } else {
        const double rate = (m_cfg.persist ? 12.0 : (tg < tTypeEnd ? 30.0 : 18.0)) * speed;
        travelled = (boot > 0.0 ? 30.0 * speed * boot : 0.0) + rate * (spinT - boot);
    }
    const double rotationDeg = travelled;

    //  Sweep line travels top to bottom during the boot window.
    bool sweepActive = false;
    double sweepY = 0.0;
    double sweepAlpha = 0.0;
    if (tg >= m_bootStart && tg < tBootEnd && boot > 0.0) {
        sweepActive = true;
        const double f = (tg - m_bootStart) / boot;
        sweepY = f * (double)m_panelH;
        sweepAlpha = 1.0;
        const double edge = std::min(f, 1.0 - f) * 6.0;
        if (edge < 1.0) sweepAlpha = edge;
    }

    //  Typed character count
    int typed = 0;
    if (tg >= tBootEnd) {
        const double since = tg - tBootEnd;
        typed = (int)std::floor(since / typeStep);
        if (typed > charCount) typed = charCount;
    }

    //  While the greeting is complete and nothing but the slowly turning
    //  reactor changes, the panel does not have to be refreshed at the
    //  full animation rate.  Ten frames a second are plenty for a ring
    //  that turns at eighteen degrees per second and they save a third
    //  of the animation's CPU time.
    if (m_cfg.persist || tg < tTypeEnd || tg >= tHoldEnd) {
        m_holdIntervalMs = 0.0;
    } else {
        m_holdIntervalMs = kHoldRedrawMs;
    }
    if (m_holdIntervalMs > 0.0 && m_lastPresentTick != 0 &&
        (double)(now - m_lastPresentTick) < m_holdIntervalMs) {
        CheckLifetime(t, tg, tEnd);
        return;
    }

    //  ---- decide what has to be redrawn --------------------------------------
    const bool greetingChanged = (typed != m_typedChars);
    const std::wstring nowClock = CurrentClockText();
    const bool statusChanged = (nowClock != m_clockText);
    if (statusChanged) {
        m_clockText = nowClock;
        BuildStatusBitmap();  //  Only once a minute.
    }
    const bool reactorMoved =
        (m_lastRotation < 0.0) || (std::fabs(rotationDeg - m_lastRotation) >= 0.7);
    const bool alphaChanged = (alpha != m_lastAlpha);

    //  Nothing at all changed: skip the frame completely.
    if (!m_fullInit && !greetingChanged && !statusChanged && !reactorMoved && !alphaChanged &&
        !sweepActive) {
        return;
    }

    const float s = m_scale;
    //  Reactor box (rings, ticks, core)
    const int rx0 = (int)std::floor((kReactorX - kROuter - kTickLength - 3.0f) * s);
    const int ry0 = (int)std::floor((kReactorY - kROuter - kTickLength - 3.0f) * s);
    const int rx1 = (int)std::ceil((kReactorX + kROuter + kTickLength + 3.0f) * s);
    const int ry1 = (int)std::ceil((kReactorY + kROuter + kTickLength + 3.0f) * s);
    const int reactorW = rx1 - rx0, reactorH = ry1 - ry0;

    //  Greeting block
    const int gx0 = (int)std::floor(kTextX * s) - 4;
    const int gx1 = (int)std::ceil(kTextRight * s) + 4;
    const int gy0 = (int)std::floor(kTextMidY * s - m_layout.lineHeight) - 6;
    const int gy1 = (int)std::ceil(kTextMidY * s + m_layout.lineHeight) + 6;
    const int greetW = gx1 - gx0, greetH = gy1 - gy0;

    //  Status row
    const int sx0 = (int)std::floor(kTextX * s) - 4;
    const int sx1 = (int)std::ceil(kTextRight * s) + 4;
    const int sy0 = (int)std::floor(kStatusY * s - m_statusLineHeight) - 4;
    const int sy1 = (int)std::ceil(kStatusY * s + m_statusLineHeight) + 4;
    const int statusW = sx1 - sx0, statusH = sy1 - sy0;

    //  Sweep band (full width, tail plus head)
    struct Zone {
        int x, y, w, h;
    };
    std::vector<Zone> zones;
    if (reactorMoved || m_fullInit || fading) zones.push_back(Zone{rx0, ry0, reactorW, reactorH});
    if (greetingChanged || m_fullInit || fading) zones.push_back(Zone{gx0, gy0, greetW, greetH});
    if (statusChanged || m_fullInit || fading) zones.push_back(Zone{sx0, sy0, statusW, statusH});

    //  The sweep band spans the whole panel and overlaps the other zones;
    //  while it is active the entire surface is recomposed.
    const bool fullSurface = m_fullInit || fading || sweepActive;

    //  ---- the part of the frame that has to be flattened again -----------------
    //  Only the zones that were restored have changed, so only they have to
    //  be flattened again.  The fade is the exception: it changes every
    //  pixel of the panel, so when it moves the whole panel is done.
    if (fullSurface || alphaChanged || zones.empty()) {
        m_flattenX = 0; m_flattenY = 0; m_flattenW = 0; m_flattenH = 0;
    } else {
        int fx0 = zones[0].x, fy0 = zones[0].y;
        int fx1 = fx0 + zones[0].w, fy1 = fy0 + zones[0].h;
        for (const Zone& z : zones) {
            if (z.x < fx0) fx0 = z.x;
            if (z.y < fy0) fy0 = z.y;
            if (z.x + z.w > fx1) fx1 = z.x + z.w;
            if (z.y + z.h > fy1) fy1 = z.y + z.h;
        }
        m_flattenX = fx0; m_flattenY = fy0;
        m_flattenW = fx1 - fx0; m_flattenH = fy1 - fy0;
    }

    //  ---- restore the base under the dirty zones -----------------------------
    LARGE_INTEGER pc0, pc1;
    QueryPerformanceCounter(&pc0);
    if (fullSurface) {
        RestoreZone(0, 0, m_panelW, m_panelH);
    } else {
        for (const Zone& z : zones) RestoreZone(z.x, z.y, z.w, z.h);
    }
    QueryPerformanceCounter(&pc1);

    //  ---- dynamic layer ------------------------------------------------------
    //  Everything is composited in software: the cached sprites are
    //  blended into the frame with a premultiplied "over", which needs
    //  neither a GDI+ session nor a temporary buffer.
    const bool reactorDirty = fullSurface || reactorMoved;
    LARGE_INTEGER pc2, pc2b, pc3, pc4, pc5;
    QueryPerformanceCounter(&pc2);
    if (fullSurface || sweepActive) DrawSweepSoftware(sweepY, sweepAlpha);
    if (reactorDirty) DrawRotorSprite(rotationDeg);
    QueryPerformanceCounter(&pc2b);
    if (fullSurface || greetingChanged) DrawGreetingSoftware(typed);
    if (fullSurface || statusChanged) DrawStatusSoftware();
    QueryPerformanceCounter(&pc3);

    //  ---- fade and present ---------------------------------------------------
    //  The window is not layered, so it cannot be made transparent.  The
    //  fade is done when the frame is flattened: the panel is mixed into
    //  the colour of the card, which reads as the panel powering up.
    m_frameFade = alpha;
    QueryPerformanceCounter(&pc4);
    Present();
    QueryPerformanceCounter(&pc5);

    {
        const double f = 1000.0 / CpuFrequency();
        m_profRestore += (double)(pc1.QuadPart - pc0.QuadPart) * f;
        m_profGdi += (double)(pc3.QuadPart - pc2.QuadPart) * f;
        m_profRotor += (double)(pc2b.QuadPart - pc2.QuadPart) * f;
        m_profPremul += (double)(pc4.QuadPart - pc3.QuadPart) * f;
        m_profPresent += (double)(pc5.QuadPart - pc4.QuadPart) * f;
        ++m_profFrames;
    }

    m_typedChars = typed;
    m_lastRotation = rotationDeg;
    m_lastAlpha = alpha;
    m_fullInit = false;
    m_lastPresentTick = now;
    ++m_framesDrawn;

    //  ---- lifetime -----------------------------------------------------------
    CheckLifetime(t, tg, tEnd);
}

//  Ends the animation when the requested lifetime is over.  The hard
//  dismiss wins over the normal fade out, and --duration caps whatever
//  the timeline says.  The fade out is measured on the LOCAL clock, the
//  clock of the greeting that is on screen, so that a rotating greeting
//  gets its own complete cycle.
bool JarvisHud::CheckLifetime(double tGlobal, double tLocal, double tLocalEnd) {
    if (m_cfg.dismissMs > 0 && tGlobal >= (double)m_cfg.dismissMs) {
        PostQuitMessage(0);
        return true;
    }
    if (!m_cfg.persist && tLocal >= tLocalEnd) {
        PostQuitMessage(0);
        return true;
    }
    if (m_durationMs > 0 && tGlobal >= (double)m_durationMs) {
        PostQuitMessage(0);
        return true;
    }
    return false;
}

//---------------------------------------------------------------------
//  Window procedure
//---------------------------------------------------------------------
LRESULT CALLBACK HudWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER: {
            if (wp == kTimerId) {
                JarvisHud* app = (JarvisHud*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
                if (app) {
                    app->CountTick();
                    app->RenderFrame();
                }
            }
            return 0;
        }
        case WM_MOUSEACTIVATE:
            //  The panel never takes the focus, not even while it is
            //  being dragged.
            return MA_NOACTIVATE;
        case WM_NCHITTEST:
            //  The panel answers the mouse, so that it can be dragged.
            //  It sits in the desktop band, so it is only ever in the
            //  way of the desktop and never of an application.
            return HTCLIENT;
        case WM_LBUTTONDOWN: {
            JarvisHud* app = (JarvisHud*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (app) app->OnMouseDown(hwnd);
            return 0;
        }
        case WM_MOUSEMOVE: {
            JarvisHud* app = (JarvisHud*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (app) app->OnMouseMove(hwnd);
            return 0;
        }
        case WM_LBUTTONUP: {
            JarvisHud* app = (JarvisHud*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (app) app->OnMouseUp(hwnd);
            return 0;
        }
        case WM_CAPTURECHANGED: {
            //  The mouse was taken away, so the drag is over.
            JarvisHud* app = (JarvisHud*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (app) app->OnMouseUp(hwnd);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            JarvisHud* app = (JarvisHud*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (app) app->PaintSurface(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY: {
            //  The panel is an ordinary top level window, so a restart of
            //  the shell does not touch it and there is nothing to
            //  rebuild.  The program simply ends.
            PostQuitMessage(0);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

//---------------------------------------------------------------------
//  Window creation and docking
//---------------------------------------------------------------------
bool JarvisHud::CreateWindowSurface(HINSTANCE instance) {
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HudWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"JarvisHudWindowClass";
    //  No background brush: the panel is painted in one piece, so there
    //  is nothing to erase and the window never flickers.
    wc.hbrBackground = nullptr;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    //  The window is deliberately NOT layered and NOT topmost.
    //
    //  A layered window is blended by the desktop window manager in a
    //  pass of its own and ends up in front of every ordinary window,
    //  whatever the z order says, so a layered panel would cover the
    //  running applications.  An ordinary window obeys the z order, and
    //  the panel is put into the desktop band, so every application
    //  window is above it.
    //
    //  The panel is not click through any more: it has to be possible to
    //  take hold of it with the left mouse button and move it somewhere
    //  else.  WS_EX_NOACTIVATE still keeps it from ever taking the
    //  focus, so the person at the keyboard keeps typing into the
    //  program they are working in.
    DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;

    m_hwnd = CreateWindowExW(exStyle, L"JarvisHudWindowClass", L"JARVIS HUD", WS_POPUP, 0, 0, 1, 1,
                             nullptr, nullptr, instance, nullptr);
    if (!m_hwnd) return false;
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, (LONG_PTR)this);
    return true;
}

//---------------------------------------------------------------------
//  The shape of the panel
//
//  An ordinary window is a rectangle, so the rounded corners of the
//  card are cut out with a window region.  The radius is the same one
//  the card is drawn with.
//---------------------------------------------------------------------
void JarvisHud::ApplyPanelRegion() {
    if (!m_hwnd || m_panelW <= 0 || m_panelH <= 0) return;
    const int d = (int)std::lround((double)kCornerRadius * (double)m_scale * 2.0);
    HRGN region = CreateRoundRectRgn(0, 0, m_panelW + 1, m_panelH + 1, d, d);
    if (!region) return;
    if (!SetWindowRgn(m_hwnd, region, TRUE)) {
        DeleteObject(region);
        return;
    }
    //  The window owns the region now.
}

//---------------------------------------------------------------------
//  The desktop band
//
//  The panel has to be visible on the desktop and invisible over every
//  running application.  There are two ways to do that, and only one of
//  them works on this machine.
//
//  The obvious one is to make the panel a CHILD of the shell's desktop
//  window, so that it belongs to the desktop layer:
//
//      Progman                     the desktop window
//        +- SHELLDLL_DefView      the icon container
//          +- SysListView32       the icons
//        +- WorkerW               the wallpaper host
//
//  That does not work here.  Measured on this Windows 11 25H2 build
//  with exact colour sampling of the screen:
//
//    * a plain (not layered) child of Progman      never paints
//    * a layered child of Progman (ULW)             never paints
//    * a colorkey child of Progman                  never paints
//    * a global alpha child of Progman              never paints
//    * a child of SHELLDLL_DefView                  never paints
//    * a child of SysListView32                     never paints
//
//  UpdateLayeredWindow reports success, the window is positioned
//  correctly, and the mouse passes through it to the icon list view, but
//  not a single pixel of the panel ever reaches the screen.  Sampling
//  over two and a half seconds shows the wallpaper underneath the whole
//  time, so it is not a repaint race either: the child simply is not
//  composited.
//
//  The way that does work is to keep the panel a TOP LEVEL window and
//  to put it into the desktop band by the z order alone: the panel is
//  placed immediately above the shell's desktop window, and therefore
//  below every application window.  The shell's desktop window is at
//  the bottom of the z order, so "immediately above it" is the band that
//  belongs to the desktop: above the wallpaper and the icons, below
//  everything else.
//
//  The window must NOT be layered (WS_EX_LAYERED).  A layered window is
//  blended by the desktop window manager in a pass of its own, after
//  the ordinary windows have been composited, so it ends up in front
//  of every application window whatever the z order says.  That was
//  measured on this machine: with a layered panel an ordinary window
//  that was raised on top of it left none of its pixels visible.  An
//  ordinary window has a redirection surface like every other window
//  and obeys the z order, so it is covered as soon as anything is put
//  in front of it.
//
//  Two details make this work and both were measured:
//
//    * SetWindowPos(hud, progman) puts the panel BELOW the desktop
//      window, where the wallpaper hides it.  SetWindowPos(progman, hud)
//      puts the desktop window below the panel, which is what is wanted.
//    * The shell raises its desktop window again from time to time (a
//      desktop refresh, an explorer.exe restart), so the position has to
//      be re-asserted about once a second.  RefreshDesktopHost() does
//      that, and it only ever moves the panel within the desktop band.
//
//  The panel is never given WS_EX_TOPMOST.  Being topmost is exactly
//  what "must not overlay other applications" forbids.
//---------------------------------------------------------------------
bool JarvisHud::FindDesktopHost(int waitMs) {
    const ULONGLONG deadline = GetTickCount64() + (ULONGLONG)std::max(0, waitMs);
    for (;;) {
        HWND progman = FindWindowW(L"Progman", nullptr);
        if (progman && IsWindow(progman)) {
            m_desktopHost = progman;
            m_iconHost = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
            return true;
        }
        m_desktopHost = nullptr;
        m_iconHost = nullptr;
        if (GetTickCount64() >= deadline) return false;
        //  The wait is polled finely: at logon the shell is usually a
        //  fraction of a second away, and the panel should not sit
        //  there waiting for the next tick of a slow clock.
        Sleep(25);
    }
}

//---------------------------------------------------------------------
//  Putting the panel into the desktop band
//
//  The panel is moved to sit immediately above the shell's desktop
//  window in the z order.  Everything else - the wallpaper, the desktop
//  icons - is below that point, and every application window is above
//  it, so no application can ever end up underneath the panel.
//---------------------------------------------------------------------
bool JarvisHud::PlaceInDesktopBand() {
    if (!m_hwnd) return false;
    if (!m_desktopHost || !IsWindow(m_desktopHost)) {
        if (!FindDesktopHost(0)) return false;
    }

    //  Step 1: drop the panel to the very bottom of the z order.  Every
    //  other window - the wallpaper, the desktop icons, and every
    //  application - is above it from then on, which is what "must not
    //  overlay an application" means.
    SetWindowPos(m_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    //  Step 2: move the shell's desktop window directly below the panel.
    //  SetWindowPos(hwnd, insertAfter) places hwnd BELOW insertAfter, so
    //  this puts the desktop window under the panel: the panel is now
    //  above the wallpaper and the desktop icons, and still below every
    //  application window.  The other order - the panel below the
    //  desktop window - puts the panel behind the wallpaper, which was
    //  measured and does not paint at all.
    SetWindowPos(m_desktopHost, m_hwnd, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    //  Keep the panel where it belongs in the band as well.  The window
    //  is not shown here: it is only ever shown while it is in the
    //  band, so that it can never float above an application.
    SetWindowPos(m_hwnd, nullptr, m_screenDock.x, m_screenDock.y, m_panelW, m_panelH,
                 SWP_NOACTIVATE | SWP_NOZORDER);

    m_inBand = true;
    return true;
}

//---------------------------------------------------------------------
//  Placement
//
//  The panel is docked to the work area of the primary monitor in SCREEN
//  coordinates, which is what a top level window wants.
//
//  The panel is only ever shown while it is in the desktop band.  It is
//  never raised above the band, so no amount of z-order fiddling by
//  another program can put it on top of a running application.
//---------------------------------------------------------------------
void JarvisHud::UpdateWindowPlacement() {
    if (!m_hwnd) return;

    m_windowPos = m_screenDock;

    if (m_inBand && m_desktopHost && IsWindow(m_desktopHost)) {
        //  Already in the band: only the position is refreshed, the
        //  z order is left exactly as it is.
        SetWindowPos(m_hwnd, nullptr, m_windowPos.x, m_windowPos.y, m_panelW, m_panelH,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return;
    }

    //  The panel is not in the band, which only happens before the shell
    //  has been found.  It stays where it is and stays hidden: the
    //  desktop band is the only place the panel is allowed to be seen.
    SetWindowPos(m_hwnd, HWND_BOTTOM, m_windowPos.x, m_windowPos.y, m_panelW, m_panelH,
                 SWP_NOACTIVATE | SWP_NOZORDER);
}

//---------------------------------------------------------------------
//  Keeping the panel in the band
//
//  The shell raises its desktop window again from time to time: a
//  desktop refresh, an explorer.exe restart, a shell notification.  When
//  it does, the panel ends up underneath the desktop window and is
//  hidden by the wallpaper.
//
//  RefreshDesktopHost() therefore looks at the desktop window once a
//  second and puts the panel back into the band if it has fallen out of
//  it.  The re-assertion moves the desktop window below the panel again;
//  it never raises the panel above anything that was in front of it.
//---------------------------------------------------------------------
void JarvisHud::RefreshDesktopHost() {
    if (!m_inBand) return;
    if (!m_desktopHost || !IsWindow(m_desktopHost)) {
        //  The shell was restarted and the old desktop window is gone.
        //  Wait for the new one and drop into the band again.
        m_desktopHost = nullptr;
        m_iconHost = nullptr;
        if (FindDesktopHost(5000)) {
            PlaceInDesktopBand();
            UpdateWindowPlacement();
        }
        return;
    }

    //  Is the panel still directly above the desktop window?  If the
    //  shell has raised the desktop window again, the panel has fallen
    //  below it and has to be put back.
    //
    //  GW_HWNDNEXT is the window directly BELOW the one it is given, so
    //  the panel is in the band exactly when the desktop window is the
    //  window that follows it.
    HWND below = GetWindow(m_hwnd, GW_HWNDNEXT);
    if (below != m_desktopHost) {
        PlaceInDesktopBand();
        UpdateWindowPlacement();
    }
}

//---------------------------------------------------------------------
//  Replacing the greeting
//
//  While the panel persists it can swap the greeting it shows for another
//  one from the corpus.  The new line is picked with the same rules as
//  the first one (eligible for the time of day, never the same line
//  twice in a row), expanded afresh, laid out again and typed out from
//  the beginning.  The panel itself stays exactly where it is.
//---------------------------------------------------------------------
bool JarvisHud::ChooseNextGreeting() {
    if (m_corpus.empty() || m_candidates.empty()) return false;

    size_t chosen = m_greetingIndex;
    if (m_candidates.size() > 1) {
        for (int attempt = 0; attempt < 8; ++attempt) {
            const size_t slot = (size_t)(MixSeed() % (uint32_t)m_candidates.size());
            const size_t candidate = (size_t)m_candidates[slot];
            if (candidate != m_greetingIndex) { chosen = candidate; break; }
        }
        if (chosen == m_greetingIndex) {
            //  The random draw kept hitting the line on screen: step to
            //  the first candidate that differs from it.
            for (size_t i = 0; i < m_candidates.size(); ++i) {
                const size_t candidate = (size_t)m_candidates[i];
                if (candidate != m_greetingIndex) { chosen = candidate; break; }
            }
        }
    }
    if (chosen == m_greetingIndex) return false;

    m_greetingIndex = chosen;
    WriteLastIndex((int)m_greetingIndex);

    //  Expand the placeholders again: the clock has moved on.
    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::wstring timeStr = FormatTimeHHMM(st);
    const std::wstring dateStr = FormatLongDate(st);
    const std::wstring weekdayStr = kWeekdayNames[st.wDayOfWeek % 7];
    m_greeting = ExpandGreeting(m_corpus[m_greetingIndex].text, m_cfg, timeStr, dateStr,
                                 weekdayStr);

    //  The new line may be longer, so the layout and the cached text
    //  bitmap are built again.
    if (m_family) {
        const float column = (kTextRight - kTextX) * m_scale;
        m_layout = ComputeLayout(m_greeting.text, m_cfg, column, m_family.get(), m_scale);
    }
    if (!BuildTextBitmap()) return false;

    m_typedChars = -1;
    m_greetingTick = GetTickCount64();
    m_bootStart = 0.0;  //  a later greeting does not fade in again
    ++m_rotations;
    return true;
}

void JarvisHud::ComputeDockPosition() {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        mi.rcWork.left = 0;
        mi.rcWork.top = 0;
        mi.rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        mi.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const RECT& work = mi.rcWork;
    const int margin = (int)std::lround((float)m_cfg.margin * m_scale);
    int x = 0, y = 0;
    const std::wstring& p = m_cfg.position;
    if (p == L"bottom-left") {
        x = work.left + margin;
        y = work.bottom - margin - m_panelH;
    } else if (p == L"top-right") {
        x = work.right - margin - m_panelW;
        y = work.top + margin;
    } else if (p == L"top-left") {
        x = work.left + margin;
        y = work.top + margin;
    } else if (p == L"center") {
        x = work.left + (work.right - work.left - m_panelW) / 2;
        y = work.top + (work.bottom - work.top - m_panelH) / 2;
    } else {  //  bottom-right
        x = work.right - margin - m_panelW;
        y = work.bottom - margin - m_panelH;
    }
    //  An exact place wins over the named corner.  The panel writes
    //  these itself when it is dragged somewhere else, so that it
    //  comes back in the same place the next time the machine starts.
    if (m_cfg.posX >= 0) x = work.left + (int)std::lround((float)m_cfg.posX * m_scale);
    if (m_cfg.posY >= 0) y = work.top  + (int)std::lround((float)m_cfg.posY * m_scale);
    m_screenDock.x = x;
    m_screenDock.y = y;
    m_windowPos = m_screenDock;
}

//---------------------------------------------------------------------
//  Dragging the panel
//
//  The panel is taken hold of with the left mouse button and moved
//  somewhere else.  It stays in the desktop band while it is moved, so
//  it can never end up above an application, and it never takes the
//  focus: the window says MA_NOACTIVATE and the mouse is caught with
//  SetCapture, so the person at the keyboard keeps typing into the
//  program they are working in.
//
//  The place the panel is put in is written into the config file, so
//  it comes back in the same place the next time the machine starts.
//---------------------------------------------------------------------
bool JarvisHud::OnMouseDown(HWND hwnd) {
    if (!hwnd) return false;
    POINT cur;
    cur.x = cur.y = 0;
    GetCursorPos(&cur);
    RECT wr;
    wr.left = wr.top = wr.right = wr.bottom = 0;
    GetWindowRect(hwnd, &wr);

    m_dragging = true;
    m_dragMoved = false;
    m_dragStartX = cur.x;
    m_dragStartY = cur.y;
    m_dragWindowX = wr.left;
    m_dragWindowY = wr.top;
    m_dragX = wr.left;
    m_dragY = wr.top;
    SetCapture(hwnd);
    {
        char line[256];
        snprintf(line, sizeof(line), "down: cursor=%ld,%ld window=%ld,%ld",
                 (long)cur.x, (long)cur.y, (long)wr.left, (long)wr.top);
        WriteLog(line);
    }
    return true;
}

bool JarvisHud::OnMouseMove(HWND hwnd) {
    if (!m_dragging || !hwnd) return false;
    POINT cur;
    cur.x = cur.y = 0;
    GetCursorPos(&cur);
    const int dx = cur.x - m_dragStartX;
    const int dy = cur.y - m_dragStartY;
    //  A few pixels of jitter are a click, not a drag.
    if (!m_dragMoved && (dx > 4 || dx < -4 || dy > 4 || dy < -4)) m_dragMoved = true;
    if (!m_dragMoved) return false;

    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) {
        mi.rcWork.left = 0;
        mi.rcWork.top = 0;
        mi.rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        mi.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const RECT& work = mi.rcWork;
    int x = m_dragWindowX + dx;
    int y = m_dragWindowY + dy;
    //  The panel has to stay on the screen.
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    if (x + m_panelW > work.right) x = work.right - m_panelW;
    if (y + m_panelH > work.bottom) y = work.bottom - m_panelH;

    m_dragX = x;
    m_dragY = y;
    m_screenDock.x = x;
    m_screenDock.y = y;
    m_windowPos = m_screenDock;
    //  The z order is left alone: the panel stays in the desktop band.
    SetWindowPos(hwnd, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return true;
}

bool JarvisHud::OnMouseUp(HWND hwnd) {
    if (!m_dragging) return false;
    //  The drag is over before the capture is let go: letting go sends
    //  WM_CAPTURECHANGED, which comes straight back here.
    m_dragging = false;
    if (hwnd && GetCapture() == hwnd) ReleaseCapture();
    if (m_dragMoved) SavePanelPosition();
    {
        char line[256];
        snprintf(line, sizeof(line), "up: moved=%d drag=%d,%d", m_dragMoved ? 1 : 0,
                 m_dragX, m_dragY);
        WriteLog(line);
    }
    return true;
}

//  Writes the place of the panel into the config file, so that it is
//  put back in the same place the next time the machine starts.
void JarvisHud::SavePanelPosition() {
    HMONITOR mon = MonitorFromPoint(POINT{ m_dragX, m_dragY }, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) {
        mi.rcWork.left = 0;
        mi.rcWork.top = 0;
        mi.rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        mi.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const int posX = (int)std::lround((float)(m_dragX - mi.rcWork.left) / m_scale);
    const int posY = (int)std::lround((float)(m_dragY - mi.rcWork.top) / m_scale);
    m_cfg.posX = posX;
    m_cfg.posY = posY;

    //  Only the installed copy keeps the place.  A build that is run
    //  out of its own folder must not write into the sources.
    if (!IsInstalledCopy(m_exeDir)) return;
    const std::wstring path = m_exeDir + L"\\config.txt";
    SaveConfigKey(path, L"pos_x", std::to_wstring(posX));
    SaveConfigKey(path, L"pos_y", std::to_wstring(posY));
    WriteLog("position saved: pos_x=" + NarrowUtf8(std::to_wstring(posX)) +
             " pos_y=" + NarrowUtf8(std::to_wstring(posY)));
}

//---------------------------------------------------------------------
//  Legal notice
//---------------------------------------------------------------------
int JarvisHud::RunSetNotice() {
    //  Requires administrator rights; anything else exits silently.
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kWinlogonKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk,
                        nullptr) != ERROR_SUCCESS)
        return 0;
    const std::string text = NarrowUtf8(m_greeting.text);
    const std::string caption = "JARVIS";
    RegSetValueExW(hk, L"LegalNoticeText", 0, REG_SZ, (const BYTE*)text.c_str(),
                   (DWORD)((text.size() + 1) * sizeof(char)));
    RegSetValueExW(hk, L"LegalNoticeCaption", 0, REG_SZ, (const BYTE*)caption.c_str(),
                   (DWORD)((caption.size() + 1) * sizeof(char)));
    RegCloseKey(hk);
    return 0;
}

//---------------------------------------------------------------------
//  Self test
//---------------------------------------------------------------------
int JarvisHud::RunSelfTest() {
    m_present = false;
    std::string report;
    char line[640];

    report += "JarvisHud self test\r\n";

    if (!m_gdi.ok()) {
        report += "gdiplus=FAILED\r\n";
        WriteLog(report);
        return 0;
    }
    report += "gdiplus=OK\r\n";

    snprintf(line, sizeof(line), "exe_dir=%s", NarrowUtf8(m_exeDir).c_str());
    report += line; report += "\r\n";
    snprintf(line, sizeof(line), "config_loaded=%d", m_configLoaded ? 1 : 0);
    report += line; report += "\r\n";
    snprintf(line, sizeof(line), "config_name=%s", NarrowUtf8(m_cfg.name).c_str());
    report += line; report += "\r\n";
    snprintf(line, sizeof(line), "config_position=%s", NarrowUtf8(m_cfg.position).c_str());
    report += line; report += "\r\n";
    snprintf(line, sizeof(line), "greeting_count=%d", (int)m_corpus.size());
    report += line; report += "\r\n";
    snprintf(line, sizeof(line), "greeting_index=%d", (int)m_greetingIndex);
    report += line; report += "\r\n";
    snprintf(line, sizeof(line), "greeting=%s", NarrowUtf8(m_greeting.text).c_str());
    report += line; report += "\r\n";

    //  Create the window without showing it.
    if (!CreateWindowSurface(GetModuleHandleW(nullptr))) {
        report += "window=FAILED\r\n";
        WriteLog(report);
        return 0;
    }
    report += "window=OK\r\n";

    m_dpi = GetDpiForWindow(m_hwnd);
    if (m_dpi == 0) m_dpi = 96;
    m_scale = (float)m_dpi / 96.0f;
    snprintf(line, sizeof(line), "dpi=%u scale=%.3f", m_dpi, (double)m_scale);
    report += line; report += "\r\n";

    m_panelW = (int)std::lround((float)m_cfg.panelWidth * m_scale);
    m_panelH = (int)std::lround((float)m_cfg.panelHeight * m_scale);
    if (m_panelW < 64) m_panelW = 64;
    if (m_panelH < 64) m_panelH = 64;
    snprintf(line, sizeof(line), "panel=%dx%d", m_panelW, m_panelH);
    report += line; report += "\r\n";

    if (!SetupSurfaces()) {
        report += "surfaces=FAILED\r\n";
        WriteLog(report);
        return 0;
    }
    report += "surfaces=OK\r\n";

    ProbeGdiAlpha();
    snprintf(line, sizeof(line), "gdi_alpha=%s",
             m_gdiPremultiplies ? "premultiplied" : "straight");
    report += line; report += "\r\n";

    BuildBloomBitmap();
    BuildCoreBitmap();
    BuildGlowBitmap();
    BuildRotorBitmap();
    if (!PrepareTypography()) {
        report += "fonts=FAILED\r\n";
        WriteLog(report);
        return 0;
    }
    BuildTextBitmap();
    m_clockText = CurrentClockText();
    BuildStatusBitmap();
    snprintf(line, sizeof(line), "font=%s", NarrowUtf8(m_fontFamilyName).c_str());
    report += line; report += "\r\n";
    snprintf(line, sizeof(line),
             "font_px=%.2f advance=%.3f line_h=%.2f lines=%d ellipsized=%d",
             (double)m_layout.fontPx, (double)m_layout.advance, (double)m_layout.lineHeight,
             (int)m_layout.lines.size(), m_layout.ellipsized ? 1 : 0);
    report += line; report += "\r\n";

    RenderStaticLayer();

    //  Render one full frame with the whole greeting visible.
    m_startTick = GetTickCount64() - 4000;  //  Pretend the boot phase is over.
    m_fullInit = true;
    m_typedChars = -1;
    m_lastRotation = -1.0;
    m_lastAlpha = -1;
    RenderFrame();

    //  Dump the composed frames next to the log file for visual
    //  inspection: the finished greeting and a half typed one.  The
    //  frames are the flattened ones, that is exactly what is painted
    //  on the screen.
    if (!m_logPath.empty()) {
        const std::wstring dir = DirNameOf(m_logPath);
        FlattenFrame(255);
        const bool ok = WritePngRgb(dir + L"\\JarvisHud_frame.png", m_blitDib.bits, m_panelW,
                                    m_panelH);
        snprintf(line, sizeof(line), "frame_png=%s", ok ? "OK" : "FAILED");
        report += line; report += "\r\n";

        //  The same panel with the greeting half typed.
        const double bootEnd =
            (double)std::max(0, m_cfg.fadeInMs) + (double)std::max(0, m_cfg.bootMs);
        const double typeDur = (double)std::max(1, m_cfg.typeMs) * (double)m_greeting.text.size();
        m_startTick = GetTickCount64() - (ULONGLONG)(bootEnd + typeDur * 0.55);
        m_typedChars = -1;
        RenderFrame();
        FlattenFrame(255);
        const bool ok2 = WritePngRgb(dir + L"\\JarvisHud_frame_typing.png", m_blitDib.bits,
                                     m_panelW, m_panelH);
        snprintf(line, sizeof(line), "frame_png_typing=%s", ok2 ? "OK" : "FAILED");
        report += line; report += "\r\n";
    }

    //  Measure the cost of the four kinds of frame the animation uses:
    //   full    - the reactor and the greeting are redrawn (typing phase)
    //   light   - only the reactor is redrawn (idle phase)
    //   surface - the whole panel is recomposed (boot phase, scan line)
    //   fade    - the whole panel is recomposed and dimmed (fade in and out)
    const int kProbeFrames = 120;
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);

    //  The frames are measured with the window being painted, so that
    //  the numbers are the real cost of a frame and not only of the
    //  composing part of it.  The window itself is never shown, but
    //  RedrawWindow paints it all the same.
    m_present = true;

    const double bootEndMs =
        (double)std::max(0, m_cfg.fadeInMs) + (double)std::max(0, m_cfg.bootMs);
    const double typeSpanMs = (double)std::max(1, m_cfg.typeMs) * (double)m_greeting.text.size();

    //  A frame in the middle of the typing phase: the reactor turns and
    //  the greeting grows.
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kProbeFrames; ++i) {
        m_startTick = GetTickCount64() - (ULONGLONG)(bootEndMs + typeSpanMs * 0.5);
        m_lastPresentTick = 0;  //  Ignore the idle throttle.
        m_lastRotation = -1.0;  //  Force a reactor redraw every iteration.
        m_typedChars = -1;      //  Force a greeting redraw every iteration.
        m_lastAlpha = -1;
        RenderFrame();
    }
    QueryPerformanceCounter(&t1);
    const double fullFrameMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
                               ((double)freq.QuadPart * (double)kProbeFrames);

    //  A frame of the idle phase: only the ring turns.
    QueryPerformanceCounter(&t0);
    ResetProfile();
    for (int i = 0; i < kProbeFrames; ++i) {
        m_startTick = GetTickCount64() - (ULONGLONG)(bootEndMs + typeSpanMs + 100.0);
        m_lastPresentTick = 0;
        m_lastRotation = -1.0;
        m_typedChars = (int)m_greeting.text.size();  //  The greeting stays.
        m_lastAlpha = -1;
        RenderFrame();
    }
    QueryPerformanceCounter(&t1);
    const double lightFrameMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
                                ((double)freq.QuadPart * (double)kProbeFrames);
    {
        const double n = (double)(m_profFrames > 0 ? m_profFrames : 1);
        snprintf(line, sizeof(line),
                 "profile_light restore=%.4f compose=%.4f rotor=%.4f fade=%.4f present=%.4f flatten=%.4f n=%d",
                 m_profRestore / n, m_profGdi / n, m_profRotor / n, m_profPremul / n,
                 m_profPresent / n, m_profFlatten / n, m_profFrames);
        report += line; report += "\r\n";
    }

    //  A frame of the boot phase: the scan line sweeps the whole panel.
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kProbeFrames; ++i) {
        m_startTick =
            GetTickCount64() - (ULONGLONG)(std::max(0, m_cfg.fadeInMs) + std::max(1, m_cfg.bootMs) * 0.5);
        m_lastPresentTick = 0;
        m_lastRotation = -1.0;
        m_typedChars = -1;
        m_lastAlpha = -1;
        RenderFrame();
    }
    QueryPerformanceCounter(&t1);
    const double surfaceFrameMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
                                  ((double)freq.QuadPart * (double)kProbeFrames);

    //  A frame of the fade: the whole panel is recomposed and dimmed.
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < kProbeFrames; ++i) {
        m_startTick = GetTickCount64() - (ULONGLONG)std::max(1, m_cfg.fadeInMs / 2);
        m_lastPresentTick = 0;
        m_lastRotation = -1.0;
        m_typedChars = -1;
        m_lastAlpha = -1;
        RenderFrame();
    }
    QueryPerformanceCounter(&t1);
    const double fadeFrameMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
                               ((double)freq.QuadPart * (double)kProbeFrames);

    //  The cost of handing a finished frame to the screen: the frame is
    //  flattened onto the card and painted into the window.
    double presentMs = 0.0;
    {
        //  The whole panel is flattened here, so that the number is the
        //  worst case and stays comparable from one build to the next.
        m_flattenX = 0; m_flattenY = 0; m_flattenW = 0; m_flattenH = 0;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < kProbeFrames; ++i) Present();
        QueryPerformanceCounter(&t1);
        presentMs = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
                    ((double)freq.QuadPart * (double)kProbeFrames);
    }

    //  Estimate the number of frames of the default animation.
    const int charCount = (int)m_greeting.text.size();
    const double fadeInMs = (double)std::max(0, m_cfg.fadeInMs);
    const double bootMs = (double)std::max(0, m_cfg.bootMs);
    const double typeMs = (double)std::max(1, m_cfg.typeMs) * (double)charCount;
    const double holdMs = (double)std::max(0, m_cfg.holdMs);
    const double fadeOutMs = (double)std::max(0, m_cfg.fadeOutMs);
    const double totalMs = fadeInMs + bootMs + typeMs + holdMs + fadeOutMs;
    const double frameMs = (double)kFrameMs;
    const double fadeInFrames = fadeInMs / frameMs;
    const double bootFrames = bootMs / frameMs;
    const double typeFrames = typeMs / frameMs;
    const double fadeOutFrames = fadeOutMs / frameMs;
    //  The idle phase is refreshed at a slower rate: the ring turns at
    //  eighteen degrees per second and ten redraws a second are ample.
    const double holdRedraws = holdMs / kHoldRedrawMs;

    //  The measured frame times already contain the painting of the
    //  window, so the estimate must not add the present cost a second
    //  time.
    const double drawCost = fadeInFrames * fadeFrameMs + bootFrames * surfaceFrameMs +
                            typeFrames * fullFrameMs + holdRedraws * lightFrameMs +
                            fadeOutFrames * fadeFrameMs;
    const double presentFrames = fadeInFrames + bootFrames + typeFrames + fadeOutFrames +
                                 holdRedraws;
    const double estTotalMs = drawCost;

    snprintf(line, sizeof(line),
             "probe_frames=%d full_frame_ms=%.4f light_frame_ms=%.4f surface_frame_ms=%.4f "
             "fade_frame_ms=%.4f",
             kProbeFrames, fullFrameMs, lightFrameMs, surfaceFrameMs, fadeFrameMs);
    report += line; report += "\r\n";
    snprintf(line, sizeof(line), "present_ms=%.4f presents=%.0f", presentMs, presentFrames);
    report += line; report += "\r\n";
    snprintf(line, sizeof(line),
             "timeline_ms=%.0f frames=%.0f est_draw_ms=%.1f est_total_cpu_ms=%.1f", totalMs,
             totalMs / frameMs, drawCost, estTotalMs);
    report += line; report += "\r\n";
    snprintf(line, sizeof(line), "process_cpu_ms=%.1f", ProcessCpuMs());
    report += line; report += "\r\n";
    snprintf(line, sizeof(line), "alpha_probe_argb=%u,%u,%u,%u", (unsigned)m_probeArgb[0],
             (unsigned)m_probeArgb[1], (unsigned)m_probeArgb[2], (unsigned)m_probeArgb[3]);
    report += line; report += "\r\n";

    DestroyWindowSurface();
    report += "result=OK exit=0\r\n";
    WriteLog(report);
    return 0;
}

//---------------------------------------------------------------------
//  Show the HUD
//---------------------------------------------------------------------
void JarvisHud::Mark(const char* name) {
    const double now = WallMs();
    m_setupMarks.push_back(std::make_pair(std::string(name), now - m_cpuMark));
    m_cpuMark = now;
}

bool JarvisHud::ShowHud() {
    if (!m_gdi.ok()) return false;

    if (!CreateWindowSurface(GetModuleHandleW(nullptr))) return false;
    Mark("window");

    m_dpi = GetDpiForWindow(m_hwnd);
    if (m_dpi == 0) m_dpi = 96;
    m_scale = (float)m_dpi / 96.0f;

    m_panelW = (int)std::lround((float)m_cfg.panelWidth * m_scale);
    m_panelH = (int)std::lround((float)m_cfg.panelHeight * m_scale);
    if (m_panelW < 64) m_panelW = 64;
    if (m_panelH < 64) m_panelH = 64;

    if (!SetupSurfaces()) return false;
    Mark("surfaces");
    ProbeGdiAlpha();
    Mark("alpha-probe");
    BuildBloomBitmap();
    BuildCoreBitmap();
    BuildGlowBitmap();
    BuildRotorBitmap();
    Mark("sprites");
    if (!PrepareTypography()) return false;
    Mark("typography");
    BuildTextBitmap();
    m_clockText = CurrentClockText();
    BuildStatusBitmap();
    Mark("text-bitmaps");
    RenderStaticLayer();
    Mark("static-layer");

    ComputeDockPosition();
    ApplyPanelRegion();
    {
        char line[256];
        snprintf(line, sizeof(line),
                 "dock: x=%ld y=%ld scale=%.3f panel=%dx%d posX=%d posY=%d margin=%d",
                 (long)m_screenDock.x, (long)m_screenDock.y, (double)m_scale, m_panelW, m_panelH,
                 m_cfg.posX, m_cfg.posY, m_cfg.margin);
        WriteLog(line);
    }

    //  ---- the desktop band --------------------------------------------------
    //  The panel belongs to the desktop, so it is put into the band that
    //  the shell's own desktop window lives in: directly above it, and
    //  therefore below every application window.  At logon the shell may
    //  not be up yet, so the wait for it is bounded.
    //
    //  The window is only shown once it is in the band.  If the shell is
    //  late, the panel stays hidden and goes up as soon as the shell
    //  appears: a panel that is not in the band must never be seen, or
    //  it would cover the running applications.
    if (FindDesktopHost(m_cfg.desktopWaitMs) && PlaceInDesktopBand()) {
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    }
    Mark("desktop-band");

    //  The window is never raised above the band.  It is not topmost
    //  either: being topmost is exactly what "must not overlay other
    //  applications" forbids.
    m_cpuSetupMs = ProcessCpuMs();
    ResetProfile();
    m_animMs = (double)std::max(0, m_cfg.fadeInMs) + (double)std::max(0, m_cfg.bootMs) +
               (double)std::max(1, m_cfg.typeMs) * (double)m_greeting.text.size() +
               (double)std::max(0, m_cfg.holdMs) + (double)std::max(0, m_cfg.fadeOutMs);
    //  --duration stretches or squeezes the whole timeline into the
    //  requested lifetime.  The factor is fixed once, from the first
    //  greeting, so that the phases of a later greeting do not shift.
    m_timeScale = 1.0;
    if (m_durationMs > 0 && m_animMs > 0.0) {
        m_timeScale = (double)m_durationMs / m_animMs;
        m_animMs = (double)m_durationMs;
    }
    m_bootStart = (double)std::max(0, m_cfg.fadeInMs) * m_timeScale;
    m_startTick = GetTickCount64();
    m_greetingTick = m_startTick;
    m_typedChars = -1;
    m_fullInit = true;
    UpdateWindowPlacement();

    SetTimer(m_hwnd, kTimerId, kFrameMs, nullptr);
    UpdateWindow(m_hwnd);

    MSG msg;
    while (true) {
        const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    KillTimer(m_hwnd, kTimerId);
    return true;
}

//---------------------------------------------------------------------
//  Diagnostics
//---------------------------------------------------------------------
void JarvisHud::WriteLog(const std::string& text) {
    if (m_logPath.empty()) return;
    //  The log is appended to so that several diagnostics can be written
    //  by the same run without overwriting each other.
    WriteTextFile(m_logPath, text, true);
}

//---------------------------------------------------------------------
//  Entry point of the application logic
//---------------------------------------------------------------------
int JarvisHud::Run(HINSTANCE instance, const std::wstring& cmdLine) {
    m_cpuMark = WallMs();
    ParseCommandLine(cmdLine);
    Mark("cli");

    //  ---- only one panel at a time ------------------------------------------
    //  Two copies of the program would put two panels in the desktop band
    //  and rotate two greetings.  The named mutex is released by the
    //  kernel when the process dies, so a copy that was killed does not
    //  block the next one.  The diagnostic modes do not put a panel on
    //  the screen, so they are allowed to run alongside a live panel.
    if (!m_selftest && !m_setNotice && !m_installAutostart && !m_uninstallAutostart) {
        m_singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\JarvisHudPanel");
        if (m_singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(m_singleInstance);
            m_singleInstance = nullptr;
            return 0;   //  A panel is already on the desktop.
        }
    }

    //  ---- locate the executable and its data files ---------------------------
    wchar_t pathBuf[4096];
    const DWORD len = GetModuleFileNameW(nullptr, pathBuf, (DWORD)(sizeof(pathBuf) / sizeof(wchar_t)));
    if (len > 0) {
        m_exePath.assign(pathBuf, len);
        m_exeDir = DirNameOf(m_exePath);
    } else {
        m_exePath = L"JarvisHud.exe";
        m_exeDir = L".";
    }

    //  ---- configuration (a missing file is not fatal) -------------------------
    m_configLoaded = LoadConfig(m_exeDir + L"\\config.txt", m_cfg);
    Mark("config");

    //  ---- autostart ----------------------------------------------------------
    //  The Run key entry is what brings the panel up at logon.  It is
    //  written here when it has gone missing, so that the greeting
    //  cannot silently stop coming up and leave the desktop bare.
    if (m_installAutostart || m_uninstallAutostart) {
        const bool want = m_installAutostart;
        const bool ok = SetAutostartEnabled(want, m_exePath);
        //  The choice is remembered, so that the self healing below does
        //  not undo it on the next launch.
        SaveConfigKey(m_exeDir + L"\\config.txt", L"autostart", want ? L"1" : L"0");
        return ok ? 0 : 1;
    }
    if (m_cfg.autostart != 0 && !IsAutostartEnabled() && IsInstalledCopy(m_exeDir))
        SetAutostartEnabled(true, m_exePath);

    //  ---- greeting corpus ----------------------------------------------------
    m_corpus = LoadGreetings(m_exeDir + L"\\greetings.txt");
    Mark("greetings");

    //  ---- pick a greeting, never the same one twice in a row ------------------
    //  Lines tagged for the current part of the day compete with the
    //  untagged ones; if nothing matches, the whole corpus is used.
    const int bucket = CurrentTimeBucket();
    std::vector<int> candidates;
    candidates.reserve(m_corpus.size());
    for (size_t i = 0; i < m_corpus.size(); ++i) {
        const int b = m_corpus[i].bucket;
        if (b == kBucketAny || b == bucket) candidates.push_back((int)i);
    }
    if (candidates.empty()) {
        for (size_t i = 0; i < m_corpus.size(); ++i) candidates.push_back((int)i);
    }
    m_candidates = candidates;
    int lastIndex = -1;
    const bool hasLast = ReadLastIndex(lastIndex);
    m_greetingIndex = ChooseGreetingIndex(candidates, lastIndex, hasLast);

    //  The panel is the "Welcome back, sir." card, so that is the line
    //  it greets you with when it comes up.  The rest of the corpus
    //  follows as the panel rotates.
    if (m_cfg.greetingLine > 0) {
        const int wanted = m_cfg.greetingLine - 1;
        if (wanted >= 0 && wanted < (int)m_corpus.size()) {
            bool eligible = false;
            for (const int c : candidates) {
                if (c == wanted) { eligible = true; break; }
            }
            if (eligible) m_greetingIndex = (size_t)wanted;
        }
    }
    WriteLastIndex((int)m_greetingIndex);
    Mark("greeting-pick");

    //  ---- expand the placeholders ---------------------------------------------
    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::wstring timeStr = FormatTimeHHMM(st);
    const std::wstring dateStr = FormatLongDate(st);
    const std::wstring weekdayStr = kWeekdayNames[st.wDayOfWeek % 7];
    m_greeting = ExpandGreeting(m_corpus[m_greetingIndex].text, m_cfg, timeStr, dateStr, weekdayStr);

    //  ---- explicit legal notice mode ------------------------------------------
    if (m_setNotice) return RunSetNotice();

    //  ---- config driven legal notice (best effort, needs administrator) -------
    if (m_cfg.noticeText) RunSetNotice();

    //  ---- self test -----------------------------------------------------------
    if (m_selftest) return RunSelfTest();

    //  ---- normal run ----------------------------------------------------------
    const bool shown = ShowHud();
    if (!m_logPath.empty()) {
        char line[1024];
        const double cpuTotal = ProcessCpuMs();
        const double n = (double)(m_profFrames > 0 ? m_profFrames : 1);
        snprintf(line, sizeof(line),
                 "jarvishud ok shown=%d frames=%d ticks=%d anim_ms=%.0f fps=%.1f "
                 "cpu_ms=%.1f startup_cpu_ms=%.1f setup_cpu_ms=%.1f "
                 "render_cpu_ms=%.1f greeting=\"%s\" font=\"%s\" "
                 "dpi=%u panel=%dx%d scale=%.3f alpha=%s "
                 "band=%d host=%p rotations=%d",
                 shown ? 1 : 0, m_framesDrawn, m_timerTicks, m_animMs,
                 m_animMs > 0.0 ? (double)m_framesDrawn * 1000.0 / m_animMs : 0.0,
                 cpuTotal, g_startupCpuMs, m_cpuSetupMs,
                 cpuTotal - m_cpuSetupMs, NarrowUtf8(m_greeting.text).c_str(),
                 NarrowUtf8(m_fontFamilyName).c_str(), m_dpi, m_panelW, m_panelH,
                 (double)m_scale, m_gdiPremultiplies ? "premultiplied" : "straight",
                 m_inBand ? 1 : 0, (void*)m_desktopHost, m_rotations);
        WriteLog(std::string(line) + "\r\n");
        snprintf(line, sizeof(line),
                 "frame_profile n=%d restore=%.4f compose=%.4f rotor=%.4f fade=%.4f "
                 "present=%.4f",
                 m_profFrames, m_profRestore / n, m_profGdi / n, m_profRotor / n,
                 m_profPremul / n, m_profPresent / n);
        WriteLog(std::string(line) + "\r\n");
        {
            std::string s = "setup_profile";
            char buf[128];
            for (const auto& kv : m_setupMarks) {
                snprintf(buf, sizeof(buf), " %s=%.1f", kv.first.c_str(), kv.second);
                s += buf;
            }
            WriteLog(s + "\r\n");
        }
    }
    (void)instance;
    return 0;
}

}  // namespace

//======================================================================
//  Entry point
//======================================================================
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR lpCmdLine, int) {
    g_startupCpuMs = ProcessCpuMs();
    //  DPI awareness has to be established before any window or GDI+ work.
    //  SetProcessDpiAwarenessContext lives in user32 on Windows 10 1703+;
    //  it is resolved dynamically so the binary also runs on older systems.
    {
        typedef BOOL(WINAPI* SetCtxFn)(DPI_AWARENESS_CONTEXT);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            union {
                FARPROC p;
                SetCtxFn f;
            } proc;
            proc.p = GetProcAddress(user32, "SetProcessDpiAwarenessContext");
            const SetCtxFn setCtx = proc.f;
            if (setCtx) {
                if (setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) goto dpiReady;
                if (setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) goto dpiReady;
            }
        }
        SetProcessDPIAware();
    }
dpiReady:
    try {
        JarvisHud app;
        return app.Run(instance, lpCmdLine ? std::wstring(lpCmdLine) : std::wstring());
    } catch (...) {
        return 0;
    }
}
