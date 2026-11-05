#pragma once
// Colors of the front end for light, dark and high contrast. Pure: the
// caller reads the system state (theme.h) and passes it in.

#include <windows.h>

namespace dirsize::ui {

struct Palette {
    bool dark = false;
    bool highContrast = false;
    COLORREF background = 0;
    COLORREF text = 0;
    COLORREF secondaryText = 0;  // header text, counts, status line
    COLORREF headerBackground = 0;
    COLORREF line = 0;           // separators
    COLORREF selection = 0;
    COLORREF selectionText = 0;
    COLORREF bar = 0;            // share bar
    COLORREF barTrack = 0;       // share bar background
    COLORREF warning = 0;        // "no access"
};

// System colors used in high contrast (GetSysColor), passed in for testing.
struct SystemColors {
    COLORREF window = 0;         // COLOR_WINDOW
    COLORREF windowText = 0;     // COLOR_WINDOWTEXT
    COLORREF highlight = 0;      // COLOR_HIGHLIGHT
    COLORREF highlightText = 0;  // COLOR_HIGHLIGHTTEXT
    COLORREF grayText = 0;       // COLOR_GRAYTEXT
};

// The "is this color light" test from the Microsoft page "Support Dark and
// Light themes in Win32 apps": ((5 * G) + (2 * R) + B) > (8 * 128).
bool IsColorLight(BYTE r, BYTE g, BYTE b);

// High contrast wins: then only the system colors are used (as the High
// Contrast documentation asks), and the accent color is ignored.
Palette MakePalette(bool dark, bool highContrast, COLORREF accent, const SystemColors& system);

}  // namespace dirsize::ui
