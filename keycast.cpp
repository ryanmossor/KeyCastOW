// Copyright © 2014 Brook Hong. All Rights Reserved.
//

// msbuild /p:platform=win32 /p:Configuration=Release
// msbuild keycastow.vcxproj /t:Clean
// rc keycastow.rc && cl -DUNICODE -D_UNICODE keycast.cpp keylog.cpp keycastow.res user32.lib shell32.lib gdi32.lib Comdlg32.lib comctl32.lib

#include <windows.h>
#include <windowsx.h>
#include <Commctrl.h>
#include <stdio.h>
#include <DbgHelp.h>
#pragma comment(lib, "DbgHelp.lib")

#include <gdiplus.h>
using namespace Gdiplus;

#include "resource.h"
#include "timer.h"
CTimer showTimer;
CTimer previewTimer;

WCHAR iniFile[MAX_PATH];

#define MAXCHARS 4096
WCHAR textBuffer[MAXCHARS];
LPCWSTR textBufferEnd = textBuffer + MAXCHARS;

struct KeyLabel {
    RectF rect;
    WCHAR *text;
    DWORD length;
    int time;
    BOOL fade;
    KeyLabel() {
        text = textBuffer;
        length = 0;
    }
};

const WCHAR SPECIAL_KEYS[] = {
    L'>',       // (imperfect) check for modifier key combo (e.g., <Ctrl + a>)
    L'\u232B',  // backspace
    L'\u2B7E',  // tab
    L'\u23CE',  // enter
    L'\u2190',  // left
    L'\u2191',  // up
    L'\u2192',  // right
    L'\u2193',  // down
    L'\u2326',  // delete
};

struct LabelSettings {
    DWORD keyStrokeDelay;
    DWORD lingerTime;
    DWORD fadeDuration;
    LOGFONT font;
    COLORREF bgColor, textColor;
    DWORD bgOpacity, textOpacity = 255;
};

LabelSettings labelSettings, previewLabelSettings;
BOOL visibleShift = FALSE;
BOOL visibleModifier = FALSE;
BOOL mouseCapturing = FALSE;
BOOL mouseCapturingMod = FALSE;
BOOL keyAutoRepeat = TRUE;
BOOL mergeMouseActions = TRUE;
int alignment = 1;
BOOL onlyCommandKeys = FALSE;
BOOL positioning = FALSE;
BOOL draggableLabel = FALSE;
UINT tcModifiers = MOD_SHIFT | MOD_WIN;
UINT tcKey = 0x4b;      // 0x4b is 'k'
Color clearColor(0, 127, 127, 127);
WCHAR comboChars[4] = L"<+>";
POINT deskOrigin;

#define MAXLABELS 60
KeyLabel keyLabels[MAXLABELS];
DWORD labelCount = 0;
RECT desktopRect;
SIZE canvasSize;
POINT canvasOrigin;

#include "keycast.h"
#include "keylog.h"

WCHAR *szWinName = L"KeyCastOW";
HWND hMainWnd;
HWND hDlgSettings;
RECT settingsDlgRect;
HWND hWndStamp;
HINSTANCE hInstance;
Graphics * gCanvas = NULL;
Font * fontPlus = NULL;

#define IDI_TRAY       100
#define WM_TRAYMSG     101
#define MENU_CONFIG    32
#define MENU_EXIT      33
#define MENU_RESTORE   34

void showText(LPCWSTR text, DisplayBehavior behavior);

#ifdef _DEBUG
WCHAR capFile[MAX_PATH];
FILE *capStream = NULL;
WCHAR recordFN[MAX_PATH];
int replayStatus = 0;
#define MENU_REPLAY    35
struct Displayed {
    DWORD tm;
    int behavior;
    size_t len;
    Displayed(DWORD t, int b, size_t l) {
        tm = t;
        behavior = b;
        len = l;
    }
};

DWORD WINAPI replay(LPVOID ptr) {
    replayStatus = 1;
    FILE *stream;
    WCHAR tmp[256];
    errno_t err = _wfopen_s(&stream, (LPCWSTR)ptr, L"rb");
    Displayed dp(0, 0, 0);
    fread(&dp, sizeof(Displayed), 1, stream);
    fread(tmp, sizeof(WCHAR), dp.len, stream);
    showText(tmp, AppendToLastLabel);
    DWORD lastTm = dp.tm;
    while(replayStatus == 1 && fread(&dp, sizeof(Displayed), 1, stream) == 1) {
        Sleep(dp.tm - lastTm);
        lastTm = dp.tm;
        fread(tmp, sizeof(WCHAR), dp.len, stream);
        tmp[dp.len] = '\0';
        showText(tmp, AppendToLastLabel);
    }
    fclose(stream);
    replayStatus = 0;
    return 0;
}

#include <sstream>
WCHAR logFile[MAX_PATH];
FILE *logStream;
void log(const std::stringstream & line) {
    fprintf(logStream,"%s",line.str().c_str());
}
#endif

void updateLayeredWindow(HWND hwnd) {
    POINT ptSrc = {0, 0};
    BLENDFUNCTION blendFunction;
    blendFunction.AlphaFormat = AC_SRC_ALPHA;
    blendFunction.BlendFlags = 0;
    blendFunction.BlendOp = AC_SRC_OVER;
    blendFunction.SourceConstantAlpha = 255;
    HDC hdcBuf = gCanvas->GetHDC();
    HDC hdc = GetDC(hwnd);
    ::UpdateLayeredWindow(hwnd,hdc,&canvasOrigin,&canvasSize,hdcBuf,&ptSrc,0,&blendFunction,2);
    ReleaseDC(hwnd, hdc);
    gCanvas->ReleaseHDC(hdcBuf);
}

void eraseLabel(int i) {
    RectF &rt = keyLabels[i].rect;
    RectF rc(rt.X, rt.Y, rt.Width + 1, rt.Height + 1);
    gCanvas->SetClip(rc);
    gCanvas->Clear(clearColor);
    gCanvas->ResetClip();
}

void drawLabelFrame(Graphics* g, const Pen* pen, const Brush* brush, RectF &rc) {
    g->DrawRectangle(pen, rc.X, rc.Y, rc.Width, rc.Height);
    g->FillRectangle(brush, rc.X, rc.Y, rc.Width, rc.Height);
}

#define BR(alpha, bgr) (alpha<<24|bgr>>16|(bgr&0x0000ff00)|(bgr&0x000000ff)<<16)
void updateLabel(int i) {
    eraseLabel(i);

    if (keyLabels[i].length > 0) {
        RectF &rc = keyLabels[i].rect;
        REAL r = 1.0f * keyLabels[i].time / labelSettings.fadeDuration;
        r = (r > 1.0f) ? 1.0f : r;
        PointF origin(rc.X, rc.Y);
        gCanvas->MeasureString(keyLabels[i].text, keyLabels[i].length, fontPlus, origin, &rc);
        if (alignment) {
            rc.X = canvasSize.cx - rc.Width;
        } else {
            rc.X = (REAL)0;
        }

        int bgAlpha = (int)(r * labelSettings.bgOpacity * 2.55);
        int textAlpha = (int)(r * labelSettings.textOpacity);
        Pen penPlus(Color::Color(BR(0, 0)), 0.0f);
        SolidBrush brushPlus(Color::Color(BR(bgAlpha, labelSettings.bgColor)));
        drawLabelFrame(gCanvas, &penPlus, &brushPlus, rc);
        SolidBrush textBrushPlus(Color(BR(textAlpha, labelSettings.textColor)));
        gCanvas->DrawString(keyLabels[i].text, keyLabels[i].length, fontPlus, PointF(rc.X, rc.Y), &textBrushPlus);
    }
}

void fadeLastLabel(BOOL whether) {
    keyLabels[labelCount-1].fade = whether;
}

static int newStrokeCount = 0;
#define SHOWTIMER_INTERVAL 40
static int deferredTime;
WCHAR deferredLabel[64];

static void startFade() {
    if (newStrokeCount > 0) {
        newStrokeCount -= SHOWTIMER_INTERVAL;
    }
    DWORD i = 0;
    BOOL dirty = FALSE;

    if (wcslen(deferredLabel) > 0) {
        // update deferred label if it exists
        if (deferredTime > 0) {
            deferredTime -= SHOWTIMER_INTERVAL;
        } else {
            showText(deferredLabel, AppendToLastLabel);
            fadeLastLabel(FALSE);
            deferredLabel[0] = '\0';
        }
    }

    for(i = 0; i < labelCount; i++) {
        RectF &rt = keyLabels[i].rect;
        if (keyLabels[i].time > labelSettings.fadeDuration) {
            if (keyLabels[i].fade) {
                keyLabels[i].time -= SHOWTIMER_INTERVAL;
            }
        } else if (keyLabels[i].time >= SHOWTIMER_INTERVAL) {
            if (keyLabels[i].fade) {
                keyLabels[i].time -= SHOWTIMER_INTERVAL;
            }
            updateLabel(i);
            dirty = TRUE;
        } else {
            keyLabels[i].time = 0;
            if (keyLabels[i].length){
                eraseLabel(i);
                // erase keyLabels[i].length times to avoid remaining shadow
                keyLabels[i].length--;
                dirty = TRUE;
            }
        }
    }
    if (dirty) {
        updateLayeredWindow(hMainWnd);
    }
}

bool outOfLine(LPCWSTR text) {
    size_t newLen = wcslen(text);
    if (keyLabels[labelCount - 1].text + keyLabels[labelCount - 1].length + newLen >= textBufferEnd) {
        wcscpy_s(textBuffer, MAXCHARS, keyLabels[labelCount - 1].text);
        keyLabels[labelCount - 1].text = textBuffer;
    }
    LPWSTR tmp = keyLabels[labelCount - 1].text + keyLabels[labelCount - 1].length;
    wcscpy_s(tmp, (textBufferEnd - tmp), text);
    RectF box;
    PointF origin(0, 0);
    gCanvas->MeasureString(keyLabels[labelCount - 1].text, keyLabels[labelCount - 1].length, fontPlus, origin, &box);
    bool out = (int)box.Width >= canvasSize.cx;
    return out;
}

bool isSpecialChar(WCHAR target) {
    size_t size = sizeof(SPECIAL_KEYS) / sizeof(WCHAR);

    for (size_t i = 0; i < size; ++i) {
        if (SPECIAL_KEYS[i] == target) {
            return true;
        }
    }

    return false;
}

/*
 * behavior 0: append text to last label
 * behavior 1: create a new label with text
 * behavior 2: replace last label with text
 * behavior 3: show label later
 */
void showText(LPCWSTR text, DisplayBehavior behavior = AppendToLastLabel) {
    SetWindowPos(hMainWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
    size_t newLen = wcslen(text);

#ifdef _DEBUG
    if (replayStatus == 0 && capStream) {
        Displayed dp(GetTickCount(), behavior, newLen);
        fwrite(&dp, sizeof(Displayed), 1, capStream);
        fwrite(text, sizeof(WCHAR), newLen, capStream);
        fflush(capStream);
    }
#endif

    DWORD i;

    WCHAR last = keyLabels[labelCount - 1].text[keyLabels[labelCount - 1].length - 1];

    bool currentIsBackspace = wcscmp(text, L"\u232B") == 0;
    bool lastIsSpecial = isSpecialChar(last);

    // Delete last char in label if char is not backspace or special char (tab, return, etc.)
    // Hacky, but also doesn't delete if last char is '>' (e.g., <Ctrl+a>)
    if (currentIsBackspace && !lastIsSpecial && keyLabels[labelCount - 1].length > 0) {
        last = L'\0';
        keyLabels[labelCount - 1].length--;
    } else {
        if (behavior == ReplaceLastLabel) {
            wcscpy_s(keyLabels[labelCount - 1].text, textBufferEnd - keyLabels[labelCount - 1].text, text);
            keyLabels[labelCount - 1].length = newLen;
        } else if (behavior == ShowLabelLater) {
            wcscpy_s(deferredLabel, 64, text);
            deferredTime = 120;
        } else if (behavior == CreateNewLabel || (newStrokeCount <= 0) /* || outOfLine(text) */) {
            for (i = 1; i < labelCount; i++) {
                if (keyLabels[i].time > 0) {
                    break;
                }
            }
            for (; i < labelCount; i++) {
                eraseLabel(i - 1);
                keyLabels[i - 1].text = keyLabels[i].text;
                keyLabels[i - 1].length = keyLabels[i].length;
                keyLabels[i - 1].time = keyLabels[i].time;
                keyLabels[i - 1].rect.X = keyLabels[i].rect.X;
                keyLabels[i - 1].fade = TRUE;
                updateLabel(i - 1);
                eraseLabel(i);
            }
            if (labelCount > 1) {
                keyLabels[labelCount - 1].text = keyLabels[labelCount - 2].text + keyLabels[labelCount - 2].length;
            }
            if (keyLabels[labelCount - 1].text+newLen >= textBufferEnd) {
                keyLabels[labelCount - 1].text = textBuffer;
            }
            wcscpy_s(keyLabels[labelCount - 1].text, textBufferEnd-keyLabels[labelCount - 1].text, text);
            keyLabels[labelCount - 1].length = newLen;
        } else {
            LPWSTR tmp = keyLabels[labelCount - 1].text + keyLabels[labelCount - 1].length;
            if (tmp + newLen >= textBufferEnd) {
                tmp = textBuffer;
                keyLabels[labelCount - 1].text = tmp;
                keyLabels[labelCount - 1].length = newLen;
            } else {
                keyLabels[labelCount - 1].length += newLen;
            }
            wcscpy_s(tmp, (textBufferEnd - tmp), text);
        }
    }

    keyLabels[labelCount - 1].time = labelSettings.lingerTime + labelSettings.fadeDuration;
    keyLabels[labelCount - 1].fade = TRUE;
    updateLabel(labelCount - 1);
    newStrokeCount = labelSettings.keyStrokeDelay;
    updateLayeredWindow(hMainWnd);
}

void updateCanvasSize(const POINT &pt) {
    for(DWORD i = 0; i < labelCount; i++) {
        if (keyLabels[i].time > 0) {
            eraseLabel(i);
            keyLabels[i].time = 0;
        }
    }
    canvasSize.cy = desktopRect.bottom - desktopRect.top;
    canvasOrigin.y = pt.y - desktopRect.bottom + desktopRect.top;
    canvasSize.cx = pt.x - desktopRect.left;
    canvasOrigin.x = desktopRect.left + 700; // TODO: make width of label configurable?

#ifdef _DEBUG
    std::stringstream line;
    line << "desktopRect: {left: " << desktopRect.left << ", top: " <<  desktopRect.top << ", right: " <<  desktopRect.right << ", bottom: " <<  desktopRect.bottom << "};\n";
    line << "canvasSize: {" << canvasSize.cx << "," <<  canvasSize.cy << "};\n";
    line << "canvasOrigin: {" << canvasOrigin.x << "," <<  canvasOrigin.y << "};\n";
    line << "pt: {" << pt.x << "," <<  pt.y << "};\n";
    log(line);
#endif
}

void createCanvas() {
    HDC hdc = GetDC(hMainWnd);
    HDC hdcBuffer = CreateCompatibleDC(hdc);
    HBITMAP hbitmap = CreateCompatibleBitmap(hdc, desktopRect.right - desktopRect.left, desktopRect.bottom - desktopRect.top);
    HBITMAP hBitmapOld = (HBITMAP)SelectObject(hdcBuffer, (HGDIOBJ)hbitmap);
    ReleaseDC(hMainWnd, hdc);
    DeleteObject(hBitmapOld);
    if (gCanvas) {
        delete gCanvas;
    }
    gCanvas = new Graphics(hdcBuffer);
    gCanvas->SetSmoothingMode(SmoothingModeAntiAlias);
    gCanvas->SetTextRenderingHint(TextRenderingHintAntiAlias);
}

void prepareLabels() {
    HDC hdc = GetDC(hMainWnd);
    HFONT hlabelFont = CreateFontIndirect(&labelSettings.font);
    HFONT hFontOld = (HFONT)SelectObject(hdc, hlabelFont);
    DeleteObject(hFontOld);

    if (fontPlus) {
        delete fontPlus;
    }
    fontPlus = new Font(hdc, hlabelFont);
    ReleaseDC(hMainWnd, hdc);
    RectF box;
    PointF origin(0, 0);
    gCanvas->MeasureString(L"\u263b - KeyCastOW OFF", 16, fontPlus, origin, &box);
    REAL unitH = box.Height;
    labelCount = (desktopRect.bottom - desktopRect.top) / (int)unitH;
    REAL paddingH = (desktopRect.bottom - desktopRect.top) - unitH*labelCount;

    DWORD offset = 0;
    offset = labelCount - 1;
    labelCount = 1;

    gCanvas->Clear(clearColor);
    for(DWORD i = 0; i < labelCount; i ++) {
        keyLabels[i].rect.X = (REAL)0;
        keyLabels[i].rect.Y = paddingH + unitH * (i + offset);
        if (keyLabels[i].time > labelSettings.lingerTime + labelSettings.fadeDuration) {
            keyLabels[i].time = labelSettings.lingerTime + labelSettings.fadeDuration;
        }
        if (keyLabels[i].time > 0) {
            updateLabel(i);
        }
    }
}

void GetWorkAreaByOrigin(const POINT &pt, MONITORINFO &mi) {
    RECT rc = {pt.x-1, pt.y-1, pt.x+1, pt.y+1};
    HMONITOR hMonitor = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
    mi.cbSize = sizeof(mi);
    GetMonitorInfo(hMonitor, &mi);
}

void positionOrigin(int action, POINT &pt) {
    if (action == 0) {
        updateCanvasSize(pt);

        MONITORINFO mi;
        GetWorkAreaByOrigin(pt, mi);
        if (mi.rcWork.left != desktopRect.left || mi.rcWork.top != desktopRect.top) {
            CopyMemory(&desktopRect, &mi.rcWork, sizeof(RECT));
            MoveWindow(hMainWnd, desktopRect.left, desktopRect.top, 1, 1, TRUE);
            createCanvas();
            prepareLabels();
        }
#ifdef _DEBUG
        std::stringstream line;
        line << "rcWork: {" << mi.rcWork.left << "," <<  mi.rcWork.top << "," <<  mi.rcWork.right << "," <<  mi.rcWork.bottom << "};\n";
        line << "desktopRect: {" << desktopRect.left << "," <<  desktopRect.top << "," <<  desktopRect.right << "," <<  desktopRect.bottom << "};\n";
        line << "canvasSize: {" << canvasSize.cx << "," <<  canvasSize.cy << "};\n";
        line << "canvasOrigin: {" << canvasOrigin.x << "," <<  canvasOrigin.y << "};\n";
        line << "labelCount: " << labelCount << "\n";
        log(line);
#endif
        WCHAR tmp[256];
        swprintf(tmp, 256, L"(%d,%d) ", pt.x, pt.y);
        showText(tmp, AppendToLastLabel);
    } else {
        positioning = FALSE;
        deskOrigin.x = pt.x;
        deskOrigin.y = pt.y;
        updateCanvasSize(pt);
        clearColor.SetValue(0x007f7f7f);
        gCanvas->Clear(clearColor);
    }
}

BOOL ColorDialog (HWND hWnd, COLORREF &clr) {
    DWORD dwCustClrs[16] = {
        RGB(0,0,0),
        RGB(0,0,255),
        RGB(0,255,0),
        RGB(128,255,255),
        RGB(255,0,0),
        RGB(255,0,255),
        RGB(255,255,0),
        RGB(192,192,192),
        RGB(127,127,127),
        RGB(0,0,128),
        RGB(0,128,0),
        RGB(0,255,255),
        RGB(128,0,0),
        RGB(255,0,128),
        RGB(128,128,64),
        RGB(255,255,255)
    };

    CHOOSECOLOR dlgColor;
    dlgColor.lStructSize = sizeof(CHOOSECOLOR);
    dlgColor.hwndOwner = hWnd;
    dlgColor.hInstance = NULL;
    dlgColor.lpTemplateName = NULL;
    dlgColor.rgbResult =  clr;
    dlgColor.lpCustColors =  dwCustClrs;
    dlgColor.Flags = CC_ANYCOLOR|CC_RGBINIT;
    dlgColor.lCustData = 0;
    dlgColor.lpfnHook = NULL;

    if (ChooseColor(&dlgColor)) {
        clr = dlgColor.rgbResult;
    }

    return TRUE;
}

HWND CreateToolTip(HWND hDlg, int toolID, LPWSTR pszText) {
    // Get the window of the tool.
    HWND hwndTool = GetDlgItem(hDlg, toolID);

    // Create the tooltip. g_hInst is the global instance handle.
    HWND hwndTip = CreateWindowEx(NULL, TOOLTIPS_CLASS, NULL,
            WS_POPUP |TTS_ALWAYSTIP | TTS_BALLOON,
            CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT,
            hDlg, NULL,
            hInstance, NULL);

    if (!hwndTool || !hwndTip) {
        return (HWND)NULL;
    }

    // Associate the tooltip with the tool.
    TOOLINFO toolInfo = { 0 };
    toolInfo.cbSize = sizeof(toolInfo);
    toolInfo.hwnd = hDlg;
    toolInfo.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    toolInfo.uId = (UINT_PTR)hwndTool;
    toolInfo.lpszText = pszText;
    SendMessage(hwndTip, TTM_ADDTOOL, 0, (LPARAM)&toolInfo);

    return hwndTip;
}

void writeSettingInt(LPCTSTR lpKeyName, DWORD dw) {
    WCHAR tmp[256];
    swprintf(tmp, 256, L"%d", dw);
    WritePrivateProfileString(L"KeyCastOW", lpKeyName, tmp, iniFile);
}

void saveSettings() {
    writeSettingInt(L"keyStrokeDelay", labelSettings.keyStrokeDelay);
    writeSettingInt(L"lingerTime", labelSettings.lingerTime);
    writeSettingInt(L"fadeDuration", labelSettings.fadeDuration);
    writeSettingInt(L"bgColor", labelSettings.bgColor);
    writeSettingInt(L"textColor", labelSettings.textColor);
    WritePrivateProfileStruct(L"KeyCastOW", L"labelFont", (LPVOID)&labelSettings.font, sizeof(labelSettings.font), iniFile);
    writeSettingInt(L"bgOpacity", labelSettings.bgOpacity);
    writeSettingInt(L"offsetX", deskOrigin.x);
    writeSettingInt(L"offsetY", deskOrigin.y);
    writeSettingInt(L"visibleShift", visibleShift);
    writeSettingInt(L"visibleModifier", visibleModifier);
    writeSettingInt(L"mouseCapturing", mouseCapturing);
    writeSettingInt(L"mouseCapturingMod", mouseCapturingMod);
    writeSettingInt(L"keyAutoRepeat", keyAutoRepeat);
    writeSettingInt(L"mergeMouseActions", mergeMouseActions);
    writeSettingInt(L"alignment", alignment);
    writeSettingInt(L"onlyCommandKeys", onlyCommandKeys);
    writeSettingInt(L"draggableLabel", draggableLabel);
    if (draggableLabel) {
        SetWindowLong(hMainWnd, GWL_EXSTYLE, GetWindowLong(hMainWnd, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
    } else {
        SetWindowLong(hMainWnd, GWL_EXSTYLE, GetWindowLong(hMainWnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
    }
    writeSettingInt(L"tcModifiers", tcModifiers);
    writeSettingInt(L"tcKey", tcKey);
}

void fixDeskOrigin() {
    if (deskOrigin.x > desktopRect.right || deskOrigin.x < desktopRect.left) {
        deskOrigin.x = desktopRect.right;
    }
    if (deskOrigin.y > desktopRect.bottom || deskOrigin.y < desktopRect.top) {
        deskOrigin.y = desktopRect.bottom;
    }
}

void loadSettings() {
    labelSettings.keyStrokeDelay = GetPrivateProfileInt(L"KeyCastOW", L"keyStrokeDelay", 3000, iniFile);
    labelSettings.lingerTime = GetPrivateProfileInt(L"KeyCastOW", L"lingerTime", 3000, iniFile);
    labelSettings.fadeDuration = GetPrivateProfileInt(L"KeyCastOW", L"fadeDuration", 100, iniFile);
    labelSettings.bgColor = GetPrivateProfileInt(L"KeyCastOW", L"bgColor", RGB(0, 0, 0), iniFile);
    labelSettings.textColor = GetPrivateProfileInt(L"KeyCastOW", L"textColor", RGB(255, 255, 255), iniFile);
    labelSettings.bgOpacity = GetPrivateProfileInt(L"KeyCastOW", L"bgOpacity", 80, iniFile);
    deskOrigin.x = GetPrivateProfileInt(L"KeyCastOW", L"offsetX", 1168, iniFile);
    deskOrigin.y = GetPrivateProfileInt(L"KeyCastOW", L"offsetY", 160, iniFile);
    MONITORINFO mi;
    GetWorkAreaByOrigin(deskOrigin, mi);
    CopyMemory(&desktopRect, &mi.rcWork, sizeof(RECT));
    MoveWindow(hMainWnd, desktopRect.left, desktopRect.top, 1, 1, TRUE);
    fixDeskOrigin();
    visibleShift = GetPrivateProfileInt(L"KeyCastOW", L"visibleShift", 0, iniFile);
    visibleModifier = GetPrivateProfileInt(L"KeyCastOW", L"visibleModifier", 0, iniFile);
    mouseCapturing = GetPrivateProfileInt(L"KeyCastOW", L"mouseCapturing", 0, iniFile);
    mouseCapturingMod = GetPrivateProfileInt(L"KeyCastOW", L"mouseCapturingMod", 0, iniFile);
    keyAutoRepeat = GetPrivateProfileInt(L"KeyCastOW", L"keyAutoRepeat", 1, iniFile);
    mergeMouseActions = GetPrivateProfileInt(L"KeyCastOW", L"mergeMouseActions", 0, iniFile);
    alignment = GetPrivateProfileInt(L"KeyCastOW", L"alignment", 1, iniFile);
    onlyCommandKeys = GetPrivateProfileInt(L"KeyCastOW", L"onlyCommandKeys", 0, iniFile);
    draggableLabel = GetPrivateProfileInt(L"KeyCastOW", L"draggableLabel", 0, iniFile);
    if (draggableLabel) {
        SetWindowLong(hMainWnd, GWL_EXSTYLE, GetWindowLong(hMainWnd, GWL_EXSTYLE) & ~WS_EX_TRANSPARENT);
    } else {
        SetWindowLong(hMainWnd, GWL_EXSTYLE, GetWindowLong(hMainWnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
    }
    // default toggle modifier: Win + Shift + K
    tcModifiers = GetPrivateProfileInt(L"KeyCastOW", L"tcModifiers", MOD_SHIFT | MOD_WIN, iniFile);
    tcKey = GetPrivateProfileInt(L"KeyCastOW", L"tcKey", 0x4b, iniFile);
    memset(&labelSettings.font, 0, sizeof(labelSettings.font));
    labelSettings.font.lfCharSet = DEFAULT_CHARSET;
    labelSettings.font.lfHeight = -37;
    labelSettings.font.lfPitchAndFamily = DEFAULT_PITCH;
    labelSettings.font.lfWeight  = FW_BLACK;
    labelSettings.font.lfOutPrecision = OUT_DEFAULT_PRECIS;
    labelSettings.font.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    labelSettings.font.lfQuality = ANTIALIASED_QUALITY;
    wcscpy_s(labelSettings.font.lfFaceName, LF_FACESIZE, TEXT("Arial Black"));
    GetPrivateProfileStruct(L"KeyCastOW", L"labelFont", &labelSettings.font, sizeof(labelSettings.font), iniFile);
}

void renderSettingsData(HWND hwndDlg) {
    WCHAR tmp[256];
    swprintf(tmp, 256, L"%d", previewLabelSettings.keyStrokeDelay);
    SetDlgItemText(hwndDlg, IDC_KEYSTROKEDELAY, tmp);
    swprintf(tmp, 256, L"%d", previewLabelSettings.lingerTime);
    SetDlgItemText(hwndDlg, IDC_LINGERTIME, tmp);
    swprintf(tmp, 256, L"%d", previewLabelSettings.fadeDuration);
    SetDlgItemText(hwndDlg, IDC_FADEDURATION, tmp);
    swprintf(tmp, 256, L"%d", previewLabelSettings.bgOpacity);
    SetDlgItemText(hwndDlg, IDC_BGOPACITY, tmp);

    CheckDlgButton(hwndDlg, IDC_VISIBLESHIFT, visibleShift ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_VISIBLEMODIFIER, visibleModifier ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_MOUSECAPTURING, mouseCapturing ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_MOUSECAPTURINGMOD, mouseCapturingMod ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_KEYAUTOREPEAT, keyAutoRepeat ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_MERGEMOUSEACTIONS, mergeMouseActions ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_ONLYCOMMANDKEYS, onlyCommandKeys ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_DRAGGABLELABEL, draggableLabel ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_MODCTRL, (tcModifiers & MOD_CONTROL) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_MODALT, (tcModifiers & MOD_ALT) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_MODSHIFT, (tcModifiers & MOD_SHIFT) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hwndDlg, IDC_MODWIN, (tcModifiers & MOD_WIN) ? BST_CHECKED : BST_UNCHECKED);
    swprintf(tmp, 256, L"%c", MapVirtualKey(tcKey, MAPVK_VK_TO_CHAR));
    SetDlgItemText(hwndDlg, IDC_TCKEY, tmp);
    ComboBox_SetCurSel(GetDlgItem(hwndDlg, IDC_ALIGNMENT), alignment);
}

void getLabelSettings(HWND hwndDlg, LabelSettings &lblSettings) {
    WCHAR tmp[256];
    GetDlgItemText(hwndDlg, IDC_KEYSTROKEDELAY, tmp, 256);
    lblSettings.keyStrokeDelay = _wtoi(tmp);
    GetDlgItemText(hwndDlg, IDC_LINGERTIME, tmp, 256);
    lblSettings.lingerTime = _wtoi(tmp);
    GetDlgItemText(hwndDlg, IDC_FADEDURATION, tmp, 256);
    lblSettings.fadeDuration = _wtoi(tmp);
    GetDlgItemText(hwndDlg, IDC_BGOPACITY, tmp, 256);
    lblSettings.bgOpacity = _wtoi(tmp);
    lblSettings.bgOpacity = min(lblSettings.bgOpacity, 255);
}

#define PREVIEWTIMER_INTERVAL 5
// TODO: better name for this? no longer displays preview label, only allows persistence of config on save
static void previewLabel() { 
    getLabelSettings(hDlgSettings, previewLabelSettings);
}

BOOL CALLBACK SettingsWndProc(HWND hwndDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    WCHAR tmp[256];
    switch (msg)
    {
        case WM_INITDIALOG:
            {
                renderSettingsData(hwndDlg);
                GetWindowRect(hwndDlg, &settingsDlgRect);
                SetWindowPos(hwndDlg, 0,
                        desktopRect.right - desktopRect.left - settingsDlgRect.right + settingsDlgRect.left,
                        desktopRect.bottom - desktopRect.top - settingsDlgRect.bottom + settingsDlgRect.top, 0, 0, SWP_NOSIZE);
                GetWindowRect(hwndDlg, &settingsDlgRect);
                HWND hCtrl = GetDlgItem(hwndDlg, IDC_ALIGNMENT);
                ComboBox_InsertString(hCtrl, 0, L"Left");
                ComboBox_InsertString(hCtrl, 1, L"Right");
            }
            return TRUE;
        case WM_NOTIFY:
            switch (((LPNMHDR)lParam)->code)
            {
                case NM_CLICK:          // Fall through to the next case.
                case NM_RETURN:
                    {
                        PNMLINK pNMLink = (PNMLINK)lParam;
                        LITEM   item    = pNMLink->item;
                        ShellExecute(NULL, L"open", item.szUrl, NULL, NULL, SW_SHOW);
                        break;
                    }
            }

            break;
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDC_TEXTFONT:
                    {
                        CHOOSEFONT cf ;
                        cf.lStructSize    = sizeof (CHOOSEFONT) ;
                        cf.hwndOwner      = hwndDlg ;
                        cf.hDC            = NULL ;
                        cf.lpLogFont      = &previewLabelSettings.font ;
                        cf.iPointSize     = 0 ;
                        cf.Flags          = CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS | CF_EFFECTS ;
                        cf.rgbColors      = 0 ;
                        cf.lCustData      = 0 ;
                        cf.lpfnHook       = NULL ;
                        cf.lpTemplateName = NULL ;
                        cf.hInstance      = NULL ;
                        cf.lpszStyle      = NULL ;
                        cf.nFontType      = 0 ;               // Returned from ChooseFont
                        cf.nSizeMin       = 0 ;
                        cf.nSizeMax       = 0 ;

                        if (ChooseFont (&cf)) {
                            prepareLabels();
                            saveSettings();
                        }
                    }
                    return TRUE;
                case IDC_TEXTCOLOR:
                    if (ColorDialog(hwndDlg, previewLabelSettings.textColor)) {
                        prepareLabels();
                        saveSettings();
                    }
                    return TRUE;
                case IDC_BGCOLOR:
                    if (ColorDialog(hwndDlg, previewLabelSettings.bgColor)) {
                        prepareLabels();
                        saveSettings();
                    }
                    return TRUE;
                case IDC_POSITION:
                    {
                        alignment = ComboBox_GetCurSel(GetDlgItem(hwndDlg, IDC_ALIGNMENT));
                        clearColor.SetValue(0x7f7f7f7f);
                        gCanvas->Clear(clearColor);
                        showText(L"+", AppendToLastLabel);
                        fadeLastLabel(FALSE);
                        positioning = TRUE;
                    }
                    return TRUE;
                case IDOK:
                    labelSettings = previewLabelSettings;
                    visibleShift = (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_VISIBLESHIFT));
                    visibleModifier = (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_VISIBLEMODIFIER));
                    mouseCapturing = (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_MOUSECAPTURING));
                    mouseCapturingMod = (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_MOUSECAPTURINGMOD));
                    keyAutoRepeat = (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_KEYAUTOREPEAT));
                    mergeMouseActions = (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_MERGEMOUSEACTIONS));
                    onlyCommandKeys = (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_ONLYCOMMANDKEYS));
                    draggableLabel = (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_DRAGGABLELABEL));
                    tcModifiers = 0;
                    if (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_MODCTRL)) {
                        tcModifiers |= MOD_CONTROL;
                    }
                    if (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_MODALT)) {
                        tcModifiers |= MOD_ALT;
                    }
                    if (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_MODSHIFT)) {
                        tcModifiers |= MOD_SHIFT;
                    }
                    if (BST_CHECKED == IsDlgButtonChecked(hwndDlg, IDC_MODWIN)) {
                        tcModifiers |= MOD_WIN;
                    }
                    GetDlgItemText(hwndDlg, IDC_TCKEY, tmp, 256);
                    alignment = ComboBox_GetCurSel(GetDlgItem(hwndDlg, IDC_ALIGNMENT));
                    if (tcModifiers != 0 && tmp[0] != '\0') {
                        tcKey = VkKeyScanEx(tmp[0], GetKeyboardLayout(0));
                        UnregisterHotKey(NULL, 1);
                        if (!RegisterHotKey(NULL, 1, tcModifiers | MOD_NOREPEAT, tcKey)) {
                            MessageBox(NULL, L"Unable to register hotkey, you probably need go to settings to redefine your hotkey for toggle capturing.", L"Warning", MB_OK|MB_ICONWARNING);
                        }
                    }
                    prepareLabels();
                    saveSettings();
                case IDCANCEL:
                    EndDialog(hwndDlg, wParam);
                    previewTimer.Stop();
                    return TRUE;
            }
    }
    return FALSE;
}

LRESULT CALLBACK DraggableWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static POINT s_last_mouse;
    switch(message)
    {
        // hold mouse to move
        case WM_LBUTTONDOWN:
            SetCapture(hWnd);
            GetCursorPos(&s_last_mouse);
            showTimer.Stop();
            break;
        case WM_MOUSEMOVE:
            if (GetCapture()==hWnd)
            {
                POINT p;
                GetCursorPos(&p);
                int dx= p.x - s_last_mouse.x;
                int dy= p.y - s_last_mouse.y;
                if (dx||dy)
                {
                    s_last_mouse=p;
                    RECT r;
                    GetWindowRect(hWnd,&r);
                    SetWindowPos(hWnd,HWND_TOPMOST,r.left+dx,r.top+dy,0,0,SWP_NOSIZE|SWP_NOACTIVATE);
                }
            }
            break;
        case WM_LBUTTONUP:
            ReleaseCapture();
            showTimer.Start(SHOWTIMER_INTERVAL);
            break;
        case WM_LBUTTONDBLCLK:
            SendMessage(hMainWnd, WM_COMMAND, MENU_CONFIG, 0);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK WindowFunc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static POINT s_last_mouse;
    static HMENU hPopMenu;
    static NOTIFYICONDATA nid;

    switch(message) {
        // trayicon
        case WM_CREATE:
            {
                memset(&nid, 0, sizeof(nid));

                nid.cbSize              = sizeof(nid);
                nid.hWnd                = hWnd;
                nid.uID                 = IDI_TRAY;
                nid.uFlags              = NIF_ICON | NIF_MESSAGE | NIF_TIP;
                nid.uCallbackMessage    = WM_TRAYMSG;
                nid.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));
                lstrcpy(nid.szTip, L"KeyCastOW");
                Shell_NotifyIcon(NIM_ADD, &nid);

                hPopMenu = CreatePopupMenu();
                AppendMenu(hPopMenu, MF_STRING, MENU_CONFIG,  L"&Settings...");
                AppendMenu(hPopMenu, MF_STRING, MENU_RESTORE,  L"&Restore default settings");
#ifdef _DEBUG
                AppendMenu(hPopMenu, MF_STRING, MENU_REPLAY,  L"Re&play");
#endif
                AppendMenu(hPopMenu, MF_STRING, MENU_EXIT,    L"E&xit");
                SetMenuDefaultItem(hPopMenu, MENU_CONFIG, FALSE);
            }
            break;
        case WM_TRAYMSG:
            {
                switch (lParam)
                {
                    case WM_RBUTTONUP:
                        {
                            POINT pnt;
                            GetCursorPos(&pnt);
                            SetForegroundWindow(hWnd); // needed to get keyboard focus
                            TrackPopupMenu(hPopMenu, TPM_LEFTALIGN, pnt.x, pnt.y, 0, hWnd, NULL);
                        }
                        break;
                    case WM_LBUTTONDBLCLK:
                        SendMessage(hWnd, WM_COMMAND, MENU_CONFIG, 0);
                        return 0;
                }
            }
            break;
        case WM_COMMAND:
            {
                switch (LOWORD(wParam))
                {
                    case MENU_CONFIG:
                        CopyMemory(&previewLabelSettings, &labelSettings, sizeof(previewLabelSettings));
                        renderSettingsData(hDlgSettings);
                        ShowWindow(hDlgSettings, SW_SHOW);
                        SetForegroundWindow(hDlgSettings);
                        previewTimer.Start(PREVIEWTIMER_INTERVAL);
                        break;
                    case MENU_RESTORE:
                        DeleteFile(iniFile);
                        loadSettings();
                        updateCanvasSize(deskOrigin);
                        createCanvas();
                        prepareLabels();
                        break;
#ifdef _DEBUG
                    case MENU_REPLAY:
                        {
                            if (replayStatus == 1) {
                                replayStatus = 2;
                                ModifyMenu(hPopMenu, MENU_REPLAY, MF_STRING, MENU_REPLAY, L"Re&play");
                            } else {
                                OPENFILENAME ofn;
                                ZeroMemory(&ofn, sizeof(OPENFILENAME));
                                ofn.lStructSize = sizeof(ofn);
                                ofn.hwndOwner   = NULL;
                                ofn.hInstance   = hInstance;
                                ofn.lpstrFile   = recordFN;
                                ofn.nMaxFile    = sizeof(recordFN);
                                ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
                                if (GetOpenFileName(&ofn)) {
                                    unsigned long id = 1;
                                    CreateThread(NULL,0,replay,recordFN,0,&id);
                                    ModifyMenu(hPopMenu, MENU_REPLAY, MF_STRING, MENU_REPLAY, L"Stop re&play");
                                }
                            }
                        }
                        break;
#endif
                    case MENU_EXIT:
                        Shell_NotifyIcon(NIM_DELETE, &nid);
                        ExitProcess(0);
                        break;
                    default:
                        break;
                }
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        case WM_DISPLAYCHANGE:
            {
                MONITORINFO mi;
                GetWorkAreaByOrigin(deskOrigin, mi);
                CopyMemory(&desktopRect, &mi.rcWork, sizeof(RECT));
                MoveWindow(hMainWnd, desktopRect.left, desktopRect.top, 1, 1, TRUE);
                fixDeskOrigin();
                updateCanvasSize(deskOrigin);
                createCanvas();
                prepareLabels();
            }
            break;
        // hold mouse to move
        case WM_LBUTTONDOWN:
            SetCapture(hWnd);
            GetCursorPos(&s_last_mouse);
            showTimer.Stop();
            break;
        case WM_MOUSEMOVE:
            if (GetCapture() == hWnd) {
                POINT p;
                GetCursorPos(&p);
                int dx = p.x - s_last_mouse.x;
                int dy = p.y - s_last_mouse.y;
                if (dx || dy) {
                    s_last_mouse=p;
                    positionOrigin(0, p);
                }
            }
            break;
        case WM_LBUTTONUP:
            ReleaseCapture();
            showTimer.Start(100);
            break;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

ATOM MyRegisterClassEx(HINSTANCE hInst, LPCWSTR className, WNDPROC wndProc) {
    WNDCLASSEX wcl;
    wcl.cbSize = sizeof(WNDCLASSEX);
    wcl.hInstance = hInst;
    wcl.lpszClassName = className;
    wcl.lpfnWndProc = wndProc;
    wcl.style = CS_DBLCLKS;
    wcl.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcl.hIconSm = LoadIcon(NULL, IDI_WINLOGO);
    wcl.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcl.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wcl.lpszMenuName = NULL;
    wcl.cbWndExtra = 0;
    wcl.cbClsExtra = 0;

    return RegisterClassEx(&wcl);
}

void CreateMiniDump(LPEXCEPTION_POINTERS lpExceptionInfo) {
    // Open a file
    HANDLE hFile = CreateFile(L"MiniDump.dmp", GENERIC_READ | GENERIC_WRITE,
        0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile != NULL &&  hFile != INVALID_HANDLE_VALUE) {

        // Create the minidump
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId          = GetCurrentThreadId();
        mdei.ExceptionPointers = lpExceptionInfo;
        mdei.ClientPointers    = FALSE;

        MINIDUMP_TYPE mdt      = MiniDumpNormal;
        BOOL retv = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
            hFile, mdt, (lpExceptionInfo != 0) ? &mdei : 0, 0, 0);

        if (!retv) {
            printf(("MiniDumpWriteDump failed. Error: %u \n"), GetLastError());
        } else {
            printf(("Minidump created.\n"));
        }

        // Close the file
        CloseHandle(hFile);

    } else {
        printf(("CreateFile failed. Error: %u \n"), GetLastError());
    }
}

LONG __stdcall MyUnhandledExceptionFilter(PEXCEPTION_POINTERS pExceptionInfo) {
    CreateMiniDump(pExceptionInfo);
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI WinMain(HINSTANCE hThisInst, HINSTANCE hPrevInst, LPSTR lpszArgs, int nWinMode) {
    MSG        msg;

    hInstance = hThisInst;

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LINK_CLASS|ICC_LISTVIEW_CLASSES|ICC_PAGESCROLLER_CLASS
        |ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES|ICC_TAB_CLASSES|ICC_TREEVIEW_CLASSES
        |ICC_UPDOWN_CLASS|ICC_USEREX_CLASSES|ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    GetModuleFileName(NULL, iniFile, MAX_PATH);
    iniFile[wcslen(iniFile)-4] = '\0';
    wcscat_s(iniFile, MAX_PATH, L".ini");
#ifdef _DEBUG
    wcscpy_s(capFile, MAX_PATH, iniFile);
    capFile[wcslen(capFile)-4] = '\0';
    wcscat_s(capFile, MAX_PATH, L".cap");

    wcscpy_s(logFile, MAX_PATH, iniFile);
    logFile[wcslen(logFile)-4] = '\0';
    wcscat_s(logFile, MAX_PATH, L".txt");
    errno_t err = _wfopen_s(&capStream, capFile, L"wb");
    err = _wfopen_s(&logStream, logFile, L"a");
#endif

    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR           gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    if (!MyRegisterClassEx(hThisInst, szWinName, WindowFunc)) {
        MessageBox(NULL, L"Could not register window class", L"Error", MB_OK);
        return 0;
    }

    hMainWnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        szWinName,
        szWinName,
        WS_POPUP,
        0, 0,            //X and Y position of window
        1, 1,            //Width and height of window
        NULL,
        NULL,
        hThisInst,
        NULL
    );

    if (!hMainWnd)    {
        MessageBox(NULL, L"Could not create window", L"Error", MB_OK);
        return 0;
    }

    loadSettings();
    updateCanvasSize(deskOrigin);
    hDlgSettings = CreateDialog(hThisInst, MAKEINTRESOURCE(IDD_DLGSETTINGS), NULL, (DLGPROC)SettingsWndProc);
    MyRegisterClassEx(hThisInst, L"STAMP", DraggableWndProc);
    hWndStamp = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_NOACTIVATE,
            L"STAMP", L"STAMP", WS_VISIBLE|WS_POPUP,
            0, 0, 1, 1,
            NULL, NULL, hThisInst, NULL);

    if (!RegisterHotKey(NULL, 1, tcModifiers | MOD_NOREPEAT, tcKey)) {
        MessageBox(NULL, L"Unable to register hotkey, you probably need go to settings to redefine your hotkey for toggle capturing.", L"Warning", MB_OK|MB_ICONWARNING);
    }
    UpdateWindow(hMainWnd);

    createCanvas();
    prepareLabels();
    ShowWindow(hMainWnd, SW_SHOW);
    HFONT hlabelFont = CreateFont(20,10,0,0,FW_BLACK,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_OUTLINE_PRECIS,
                CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY, VARIABLE_PITCH,TEXT("Arial"));
    HWND hlink = GetDlgItem(hDlgSettings, IDC_SYSLINK1);
    SendMessage(hlink, WM_SETFONT, (WPARAM)hlabelFont, TRUE);

    showTimer.OnTimedEvent = startFade;
    showTimer.Start(SHOWTIMER_INTERVAL);
    previewTimer.OnTimedEvent = previewLabel;

    kbdhook = SetWindowsHookEx(WH_KEYBOARD_LL, LLKeyboardProc, hThisInst, NULL);
    moshook = SetWindowsHookEx(WH_MOUSE_LL, LLMouseProc, hThisInst, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0,_WRITE_ABORT_MSG);
    SetUnhandledExceptionFilter(MyUnhandledExceptionFilter);

    while(GetMessage(&msg, NULL, 0, 0))    {
        if (msg.message == WM_HOTKEY) {
            if (kbdhook) {
                showText(L"\u2716", ReplaceLastLabel);     // disable key display
                UnhookWindowsHookEx(kbdhook);
                kbdhook = NULL;
                UnhookWindowsHookEx(moshook);
                moshook = NULL;
            } else {
                showText(L"\u2714", ReplaceLastLabel);     // enable key display
                kbdhook = SetWindowsHookEx(WH_KEYBOARD_LL, LLKeyboardProc, hInstance, NULL);
                moshook = SetWindowsHookEx(WH_MOUSE_LL, LLMouseProc, hThisInst, 0);
            }
        } else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    UnhookWindowsHookEx(kbdhook);
    UnhookWindowsHookEx(moshook);
    UnregisterHotKey(NULL, 1);
    delete gCanvas;
    delete fontPlus;
#ifdef _DEBUG
    fclose(capStream);
    fclose(logStream);
#endif

    GdiplusShutdown(gdiplusToken);
    return msg.wParam;
}
