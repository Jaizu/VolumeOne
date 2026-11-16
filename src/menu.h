#pragma once

#include "common.h"

// The tray's right-click menu, drawn by us so the current devices can be
// highlighted across the whole row (a themed Win32 menu can't, and turns
// classic as soon as one item is owner-drawn). Looks like a Windows 11
// context menu: acrylic, rounded, follows the app theme.
struct MenuItem {
    UINT id = 0;
    std::wstring text;
    HBITMAP icon = nullptr;  // premultiplied 32-bit top-down DIB; not owned
    bool current = false;    // highlighted row, semibold
    bool separator = false;
    std::vector<MenuItem> sub;  // non-empty: opens a submenu
};

// Like TrackPopupMenu with TPM_RETURNCMD | TPM_BOTTOMALIGN: blocks until an
// item is picked (returns its id) or the menu is dismissed (returns 0).
// Called again while open, it closes the open menu.
UINT TrackMenu(const std::vector<MenuItem>& items, int x, int y, bool rightAlign, bool light);
