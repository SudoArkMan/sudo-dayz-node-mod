// Dark theme shared by every widget and the canvas.
//
// One source of truth for colours and metrics: the canvas paints with these
// directly, the docks get them through the stylesheet. Matches the reference
// look: near-black chrome, subtle panel separation, no pure black or white.
#pragma once

#include <QColor>
#include <QFont>
#include <QString>

class QApplication;

namespace theme {

// Chrome
QColor windowBg();      // outermost frame
QColor panelBg();       // dock contents
QColor headerBg();      // dock title bars, table headers
QColor canvasBg();      // graph background
QColor gridMinor();
QColor gridMajor();
QColor border();
QColor text();
QColor textDim();
QColor accent();        // selection / focus
QColor selection();     // node selection outline
QColor errorColor();
QColor warningColor();

// Node metrics, in scene units.
namespace node {
constexpr double width = 168.0;
constexpr double headerHeight = 20.0;
constexpr double pinRow = 13.0;
constexpr double pinRadius = 3.4;
constexpr double padding = 6.0;
constexpr double radius = 3.0;
} // namespace node

QColor nodeBody();
QColor nodeBodyAlt();   // second row shade for inline editors
QColor nodeOutline();

// Code views: syntax colours and the editor's own furniture.
//
// Desaturated on purpose. A stock VS Code palette on this near-black chrome
// reads as a second theme bolted on, so every hue here is pulled back until it
// sits with the docks.
namespace syntax {
QColor keyword();       // if, class, proto, override
QColor type();          // int, float, string, vector
QColor className();     // identifier the catalogue knows as a real class
QColor localName();     // graph variable, function or parameter
QColor unknownName();   // identifier nothing in scope resolves; opt-in only
QColor stringLit();
QColor number();
QColor comment();
QColor preprocessor();
QColor currentLine();   // caret row wash
QColor bracketMatch();  // background behind a matched bracket pair
} // namespace syntax

QFont uiFont(int pointSize = 8, bool bold = false);
QFont monoFont(int pointSize = 8);

// Applies palette + stylesheet to the whole application.
void apply(QApplication &app);

} // namespace theme
