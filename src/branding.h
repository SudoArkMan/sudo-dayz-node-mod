// The brand pack in resources/brand, as code.
//
// resources/brand/README.txt is the specification: the palette below is its
// colour table and the accessors below are the only way into the artwork, so
// nothing else in the app carries a brand hex or a path under resources/brand.
#pragma once

#include <QColor>

class QIcon;
class QPixmap;

namespace branding {

// The pack's colour table. Header only, and it has to stay that way: theme.cpp
// reads these and is compiled into seven test targets that link no branding.cpp,
// so a call into one would break every one of them.
inline QColor ground()    { return QColor(0x16, 0x1C, 0x24); }
inline QColor panel()     { return QColor(0x23, 0x2B, 0x36); }
inline QColor accent()    { return QColor(0x5F, 0x92, 0xC9); }
inline QColor text()      { return QColor(0xE6, 0xEC, 0xF3); }
inline QColor mutedText() { return QColor(0x7D, 0x88, 0x99); }
inline QColor rule()      { return QColor(0x46, 0x53, 0x64); }

// Window, task bar and alt-tab icon, carrying every size the pack ships so
// Windows picks a drawn bitmap rather than stretching one it liked the look of.
QIcon appIcon();

// The splash art at the variant the screen wants, with the ratio stamped on it
// so Qt sizes the window in logical pixels either way.
QPixmap splashArt(qreal dpr);

// The toolbar corner mark, scaled to `logicalHeight` with its 320x50 aspect
// kept. Null when the mark cannot be loaded, which callers treat as "no mark".
QPixmap cornerMark(int logicalHeight, qreal dpr);

} // namespace branding
