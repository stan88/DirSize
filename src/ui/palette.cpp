#include "ui/palette.h"

namespace dirsize::ui {

bool IsColorLight(BYTE r, BYTE g, BYTE b) { return ((5 * g) + (2 * r) + b) > (8 * 128); }

Palette MakePalette(bool dark, bool highContrast, COLORREF accent, const SystemColors& system) {
    Palette p;
    p.highContrast = highContrast;
    if (highContrast) {
        // One foreground/background pair from the system, no own colors.
        p.dark = !IsColorLight(GetRValue(system.window), GetGValue(system.window), GetBValue(system.window));
        p.background = system.window;
        p.text = system.windowText;
        p.secondaryText = system.windowText;
        p.headerBackground = system.window;
        p.line = system.windowText;
        p.selection = system.highlight;
        p.selectionText = system.highlightText;
        p.bar = system.highlight;
        p.barTrack = system.window;
        p.warning = system.windowText;
        p.scrollThumb = system.windowText;
        p.scrollThumbActive = system.highlight;
        return p;
    }
    p.dark = dark;
    if (dark) {
        p.background = RGB(0x20, 0x20, 0x20);
        p.text = RGB(0xF2, 0xF2, 0xF2);
        p.secondaryText = RGB(0xA8, 0xA8, 0xA8);
        p.headerBackground = RGB(0x2B, 0x2B, 0x2B);
        p.line = RGB(0x3C, 0x3C, 0x3C);
        p.selection = RGB(0x3A, 0x4D, 0x63);
        p.selectionText = RGB(0xFF, 0xFF, 0xFF);
        p.barTrack = RGB(0x38, 0x38, 0x38);
        p.warning = RGB(0xFF, 0xB0, 0x5C);
        p.scrollThumb = RGB(0x5C, 0x5C, 0x5C);
        p.scrollThumbActive = RGB(0x80, 0x80, 0x80);
    } else {
        p.background = RGB(0xFF, 0xFF, 0xFF);
        p.text = RGB(0x1A, 0x1A, 0x1A);
        p.secondaryText = RGB(0x5F, 0x5F, 0x5F);
        p.headerBackground = RGB(0xF3, 0xF3, 0xF3);
        p.line = RGB(0xE3, 0xE3, 0xE3);
        p.selection = RGB(0xCC, 0xE4, 0xF7);
        p.selectionText = RGB(0x00, 0x00, 0x00);
        p.barTrack = RGB(0xE8, 0xE8, 0xE8);
        p.warning = RGB(0xA3, 0x4A, 0x00);
        p.scrollThumb = RGB(0xC2, 0xC2, 0xC2);
        p.scrollThumbActive = RGB(0x8A, 0x8A, 0x8A);
    }
    p.bar = accent;
    return p;
}

}  // namespace dirsize::ui
