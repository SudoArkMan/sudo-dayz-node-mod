#include "theme.h"

#include "branding.h"

#include <QApplication>
#include <QPair>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QVector>

namespace theme {

// The brand pack's table, from resources/brand/README.txt. Anything that needs
// a colour asks here; nothing hard-codes a hex outside this file except the
// node accents in catalog.cpp and the pin colours in pins.cpp.
//
// Four of the six come straight from the pack. The rest are derived, and every
// derived value keeps the step it had against the value it was derived from,
// so the chrome reads the same as before while agreeing with the artwork.
QColor windowBg()  { return branding::ground(); }
QColor panelBg()   { return branding::panel(); }
QColor headerBg()  { return QColor("#2b3441"); } // panel, lifted the way it was
QColor canvasBg()  { return QColor("#151b23"); } // a shade under the ground
QColor gridMinor() { return QColor("#1d242e"); }
QColor gridMajor() { return QColor("#252d38"); }
QColor border()    { return branding::rule(); }
QColor text()      { return branding::text(); }
QColor textDim()   { return branding::mutedText(); }
QColor accent()    { return branding::accent(); }

// DESIGN.md gives the selected-node outline the accent hex. Kept as its own
// accessor so canvas selection can be retuned without moving focus rings.
QColor selection() { return branding::accent(); }

QColor errorColor()   { return QColor("#d9534f"); }
QColor warningColor() { return QColor("#e0a53f"); }

// Node bodies keep their old distance from the canvas, so a node stands off its
// background by exactly what it used to.
QColor nodeBody()    { return QColor("#212934"); }
QColor nodeBodyAlt() { return QColor("#262f3b"); }

// Nodes take the same rim as the docks so the canvas and the chrome read as
// one surface language rather than two themes stitched together.
QColor nodeOutline() { return border(); }

namespace syntax {

QColor keyword()      { return QColor("#b48ead"); }
QColor type()         { return QColor("#6fb8a8"); }

// Gold, not another teal. This is the one colour in the editor that answers
// "does this class actually exist in vanilla", so it must not read as a
// shade of the built-in types beside it.
QColor className()    { return QColor("#c8a96a"); }

QColor localName()    { return QColor("#7aa9d4"); } // echoes the accent blue
QColor unknownName()  { return warningColor(); }
QColor stringLit()    { return QColor("#c08a68"); }
QColor number()       { return QColor("#a3bf83"); }
QColor comment()      { return QColor("#5f7f5c"); }
QColor preprocessor() { return QColor("#97906a"); }

// One step off the editor background. Anything stronger fights the selection.
QColor currentLine()  { return QColor("#1d242f"); }
QColor bracketMatch() { return QColor("#2a4a69"); }

} // namespace syntax

} // namespace theme

namespace {

// Shades only the stylesheet needs. Private on purpose: the canvas must not
// start painting with values that are not in the brand table.
QColor rowAlt()       { return QColor("#1c232c"); } // alternating item rows over Base
QColor hoverBg()      { return QColor("#333d4b"); } // one step above headerBg
QColor accentHi()     { return QColor("#6da5dd"); } // hover / link
// Pressed and checked fills, and the selection band of a panel that does not
// have focus. Seated far enough that @text@ on it holds the 4.9:1 the old
// accent gave, which is the pairing an unfocused list is read through.
QColor accentLo()     { return QColor("#446990"); }

// The brand accent is light enough that a near-white label on a band of it
// lands at 3.0:1, worse than the 3.9:1 the old accent gave. Filled selections
// take it seated a fifth, which reads as the same blue and puts the label back
// at 4.0:1. Rules, focus rings and the node outline keep the accent itself.
QColor accentFill()   { return QColor("#507caa"); }

QColor scrollHandle() { return QColor("#465364"); }
QColor scrollHover()  { return QColor("#586779"); }
QColor textBright()   { return QColor("#f2f6fb"); } // selected text, never pure white
QColor disabledText() { return QColor("#7b8491"); } // 3.8:1 on panelBg, as before
QColor shadow()       { return QColor("#0b1017"); }

// Tokens rather than QString::arg: two dozen numbered placeholders in a sheet
// this long do not survive an edit. Delimited both sides so "@text@" cannot
// eat the front of "@textDim@".
QString expand(QString sheet)
{
    const QVector<QPair<QString, QColor>> tokens = {
        {QStringLiteral("@window@"), theme::windowBg()},
        {QStringLiteral("@panel@"), theme::panelBg()},
        {QStringLiteral("@header@"), theme::headerBg()},
        {QStringLiteral("@canvas@"), theme::canvasBg()},
        {QStringLiteral("@border@"), theme::border()},
        {QStringLiteral("@text@"), theme::text()},
        {QStringLiteral("@textBright@"), textBright()},
        {QStringLiteral("@textDim@"), theme::textDim()},
        {QStringLiteral("@textOff@"), disabledText()},
        {QStringLiteral("@accent@"), theme::accent()},
        {QStringLiteral("@accentHi@"), accentHi()},
        {QStringLiteral("@accentLo@"), accentLo()},
        {QStringLiteral("@accentFill@"), accentFill()},
        {QStringLiteral("@hover@"), hoverBg()},
        {QStringLiteral("@rowAlt@"), rowAlt()},
        {QStringLiteral("@scroll@"), scrollHandle()},
        {QStringLiteral("@scrollHi@"), scrollHover()},
        {QStringLiteral("@error@"), theme::errorColor()},
        {QStringLiteral("@warning@"), theme::warningColor()},
    };
    for (const auto &t : tokens)
        sheet.replace(t.first, t.second.name(QColor::HexRgb));
    return sheet;
}

// No font-family anywhere in here on purpose: a stylesheet font beats
// QWidget::setFont, so declaring one would clobber every monoFont() the code
// views set on themselves.
QString styleSheet()
{
    return expand(QStringLiteral(R"QSS(
/* ---- frame ---------------------------------------------------------- */
QMainWindow, QDialog {
    background: @window@;
}
QMainWindow::separator {
    background: @window@;
    width: 4px;
    height: 4px;
}
QMainWindow::separator:hover {
    background: @accent@;
}
QWidget:disabled {
    color: @textOff@;
}
QLabel {
    background: transparent;
    color: @text@;
}
QLabel:disabled {
    color: @textOff@;
}
QToolTip {
    background: @header@;
    color: @text@;
    border: 1px solid @border@;
    padding: 3px 6px;
}

/* ---- docks ---------------------------------------------------------- */
/* No titlebar-*-icon here: overriding them without a resource icon leaves the
   float and close buttons blank, so the base style keeps drawing them. */
QDockWidget {
    background: @panel@;
    color: @text@;
    font-weight: bold;
    border: none;
}
QDockWidget::title {
    background: @header@;
    color: @text@;
    text-align: left;
    padding: 4px 6px 4px 8px;
    border-bottom: 1px solid @window@;
}
/* The dock's content widget. Panels that do not paint themselves would
   otherwise show the main window's frame colour through. */
QDockWidget > QWidget {
    background: @panel@;
}
/* A child selector outranks a plain type selector, so a view used as the
   whole panel needs this or it comes out panel-coloured, not sunken. */
QDockWidget > QAbstractItemView, QDockWidget > QTextBrowser,
QDockWidget > QTextEdit, QDockWidget > QPlainTextEdit {
    background: @window@;
}
QDockWidget > QAbstractButton {
    background: transparent;
    border: 1px solid transparent;
    padding: 0px;
}
QDockWidget > QAbstractButton:hover {
    background: @hover@;
    border-color: @border@;
}

/* ---- menu bar and menus --------------------------------------------- */
QMenuBar {
    background: @window@;
    color: @text@;
    border-bottom: 1px solid @border@;
    padding: 1px 4px;
}
QMenuBar::item {
    background: transparent;
    padding: 4px 9px;
    border-radius: 2px;
}
QMenuBar::item:selected {
    background: @header@;
}
QMenuBar::item:pressed {
    background: @accentLo@;
    color: @textBright@;
}
QMenu {
    background: @panel@;
    color: @text@;
    border: 1px solid @border@;
    padding: 4px;
}
QMenu::item {
    padding: 4px 26px 4px 22px;
    border-radius: 2px;
}
QMenu::item:selected {
    background: @accentFill@;
    color: @textBright@;
}
QMenu::item:disabled {
    color: @textOff@;
}
QMenu::separator {
    height: 1px;
    background: @border@;
    margin: 4px 8px;
}
QMenu::indicator {
    width: 12px;
    height: 12px;
    margin-left: 5px;
}

/* ---- toolbar -------------------------------------------------------- */
QToolBar {
    background: @window@;
    border: none;
    border-bottom: 1px solid @border@;
    padding: 2px 4px;
    spacing: 2px;
}
QToolBar::separator {
    background: @border@;
    width: 1px;
    height: 1px;
    margin: 4px 5px;
}
QToolButton {
    background: transparent;
    color: @text@;
    border: 1px solid transparent;
    border-radius: 2px;
    padding: 3px 7px;
}
QToolButton:hover {
    background: @header@;
    border-color: @border@;
}
QToolButton:pressed, QToolButton:checked {
    background: @accentLo@;
    border-color: @accent@;
    color: @textBright@;
}
QToolButton:disabled {
    color: @textOff@;
}

/* ---- script tabs ---------------------------------------------------- */
QTabWidget::pane {
    background: @panel@;
    border: none;
    border-top: 1px solid @border@;
}
QTabBar {
    background: @window@;
    qproperty-drawBase: 0;
}
QTabBar::tab {
    background: @window@;
    color: @textDim@;
    border: none;
    border-top: 2px solid transparent;
    border-right: 1px solid @window@;
    border-radius: 0px;
    padding: 5px 14px;
    min-width: 64px;
}
QTabBar::tab:hover {
    background: @header@;
    color: @text@;
}
QTabBar::tab:selected {
    background: @panel@;
    color: @text@;
    border-top: 2px solid @accent@;
}
QTabBar::tab:disabled {
    color: @textOff@;
}
/* A project can carry more scripts than the bar is wide. Without these the
   scroll buttons inherit QToolButton's 7px side padding, which leaves their
   arrows no room to draw and strands every tab past the clip. */
QTabBar::scroller {
    width: 36px;
}
QTabBar QToolButton {
    background: @header@;
    color: @text@;
    border: none;
    border-left: 1px solid @window@;
    border-radius: 0px;
    padding: 0px;
    margin: 0px;
    width: 17px;
}
QTabBar QToolButton:hover {
    background: @hover@;
}
QTabBar QToolButton:pressed {
    background: @accentLo@;
}
QTabBar QToolButton:disabled {
    background: @window@;
    color: @textOff@;
}

/* ---- inputs --------------------------------------------------------- */
QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit, QTextBrowser {
    background: @window@;
    color: @text@;
    border: 1px solid @border@;
    border-radius: 2px;
    padding: 3px 6px;
    selection-background-color: @accentFill@;
    selection-color: @textBright@;
}
QTextBrowser, QTextEdit, QPlainTextEdit {
    padding: 4px;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus,
QPlainTextEdit:focus, QTextEdit:focus {
    border-color: @accent@;
}
QLineEdit:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
    background: @panel@;
    color: @textOff@;
}
QSpinBox::up-button, QDoubleSpinBox::up-button,
QSpinBox::down-button, QDoubleSpinBox::down-button {
    background: @header@;
    border: none;
    width: 13px;
}
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
    background: @hover@;
}
QComboBox {
    background: @window@;
    color: @text@;
    border: 1px solid @border@;
    border-radius: 2px;
    padding: 3px 6px;
    min-height: 16px;
}
QComboBox:hover {
    border-color: @accentLo@;
}
QComboBox:focus, QComboBox:on {
    border-color: @accent@;
}
QComboBox:disabled {
    background: @panel@;
    color: @textOff@;
}
QComboBox::drop-down {
    subcontrol-origin: padding;
    subcontrol-position: center right;
    width: 15px;
    border: none;
    border-left: 1px solid @border@;
    background: transparent;
}
QComboBox QAbstractItemView {
    background: @panel@;
    color: @text@;
    border: 1px solid @border@;
    outline: none;
    selection-background-color: @accentFill@;
    selection-color: @textBright@;
}
QCheckBox, QRadioButton {
    background: transparent;
    color: @text@;
    spacing: 6px;
}
QCheckBox:disabled, QRadioButton:disabled {
    color: @textOff@;
}
QCheckBox::indicator, QRadioButton::indicator {
    width: 12px;
    height: 12px;
    background: @window@;
    border: 1px solid @border@;
    border-radius: 2px;
}
QRadioButton::indicator {
    border-radius: 7px;
}
QCheckBox::indicator:hover, QRadioButton::indicator:hover {
    border-color: @accent@;
}
QCheckBox::indicator:checked, QRadioButton::indicator:checked {
    background: @accentFill@;
    border-color: @accentHi@;
}
QCheckBox::indicator:indeterminate {
    background: @accentLo@;
    border-color: @accentLo@;
}
QCheckBox::indicator:disabled, QRadioButton::indicator:disabled {
    background: @panel@;
    border-color: @textOff@;
}
QPushButton {
    background: @header@;
    color: @text@;
    border: 1px solid @border@;
    border-radius: 2px;
    padding: 4px 12px;
    min-height: 16px;
}
QPushButton:hover {
    background: @hover@;
    border-color: @accentLo@;
}
QPushButton:pressed, QPushButton:checked {
    background: @accentLo@;
    border-color: @accent@;
    color: @textBright@;
}
QPushButton:default {
    border-color: @accent@;
}
QPushButton:disabled {
    background: @panel@;
    color: @textOff@;
    border-color: @border@;
}
QGroupBox {
    background: transparent;
    border: 1px solid @border@;
    border-radius: 2px;
    margin-top: 9px;
    padding-top: 7px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 8px;
    padding: 0px 4px;
    color: @textDim@;
}

/* ---- lists, trees, tables ------------------------------------------- */
QTreeView, QTreeWidget, QListView, QListWidget, QTableView, QTableWidget {
    background: @window@;
    alternate-background-color: @rowAlt@;
    color: @text@;
    border: 1px solid @border@;
    border-radius: 2px;
    outline: none;
    selection-background-color: @accentFill@;
    selection-color: @textBright@;
}
QTreeView::item, QListView::item, QTableView::item {
    border: none;
    padding: 2px 4px;
}
QTreeView::item:hover, QListView::item:hover, QTableView::item:hover {
    background: @header@;
}
QTreeView::item:selected, QListView::item:selected, QTableView::item:selected {
    background: @accentFill@;
    color: @textBright@;
}
QTreeView::item:selected:!active, QListView::item:selected:!active,
QTableView::item:selected:!active {
    background: @accentLo@;
    color: @text@;
}
/* Without this the indent column stays unpainted and selection stops short
   of the left edge. */
QTreeView::branch {
    background: transparent;
}
QTreeView::branch:hover {
    background: @header@;
}
QTreeView::branch:selected {
    background: @accentFill@;
}
QHeaderView {
    background: @header@;
    border: none;
}
QHeaderView::section {
    background: @header@;
    color: @textDim@;
    border: none;
    border-right: 1px solid @window@;
    border-bottom: 1px solid @border@;
    padding: 3px 6px;
}
QHeaderView::section:hover {
    background: @hover@;
    color: @text@;
}
QTableView QTableCornerButton::section {
    background: @header@;
    border: none;
    border-bottom: 1px solid @border@;
}
QTableView {
    gridline-color: @panel@;
}

/* ---- scroll bars ---------------------------------------------------- */
QAbstractScrollArea::corner {
    background: transparent;
    border: none;
}
QScrollArea {
    background: @panel@;
    border: none;
}
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 0px;
}
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 0px;
}
QScrollBar::handle:vertical {
    background: @scroll@;
    border-radius: 3px;
    margin: 2px;
    min-height: 26px;
}
QScrollBar::handle:horizontal {
    background: @scroll@;
    border-radius: 3px;
    margin: 2px;
    min-width: 26px;
}
QScrollBar::handle:hover {
    background: @scrollHi@;
}
QScrollBar::handle:pressed {
    background: @accent@;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
    width: 0px;
    height: 0px;
    background: transparent;
    border: none;
}
QScrollBar::up-arrow, QScrollBar::down-arrow,
QScrollBar::left-arrow, QScrollBar::right-arrow {
    background: transparent;
    width: 0px;
    height: 0px;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: transparent;
}

/* ---- canvas and splitters ------------------------------------------- */
QGraphicsView {
    background: @canvas@;
    border: none;
}
QSplitter {
    background: @window@;
}
QSplitter::handle {
    background: @window@;
    image: none;
}
QSplitter::handle:horizontal {
    width: 4px;
}
QSplitter::handle:vertical {
    height: 4px;
}
QSplitter::handle:hover, QSplitter::handle:pressed {
    background: @accent@;
}

/* ---- status bar ----------------------------------------------------- */
QStatusBar {
    background: @window@;
    color: @textDim@;
    border-top: 1px solid @border@;
}
QStatusBar::item {
    border: none;
}
QStatusBar QLabel {
    background: transparent;
    color: @textDim@;
    padding: 0px 6px;
}
QStatusBar QSizeGrip {
    background: transparent;
    width: 12px;
    height: 12px;
}

/* ---- diagnostics ---------------------------------------------------- */
/* Set with setProperty("severity", ...) plus unpolish/polish. A palette colour
   cannot do this job: every rule above that names a colour for QLabel beats
   QWidget::setPalette, whatever the widget asks for. */
QLabel[severity="error"], QStatusBar QLabel[severity="error"] {
    color: @error@;
}
QLabel[severity="warning"], QStatusBar QLabel[severity="warning"] {
    color: @warning@;
}
QLabel[severity="note"], QStatusBar QLabel[severity="note"] {
    color: @text@;
}
QProgressBar {
    background: @window@;
    color: @text@;
    border: 1px solid @border@;
    border-radius: 2px;
    text-align: center;
}
QProgressBar::chunk {
    background: @accentFill@;
}
)QSS"));
}

} // namespace

QFont theme::uiFont(int pointSize, bool bold)
{
    QFont f;
#ifdef Q_OS_WIN
    f.setFamilies({QStringLiteral("Segoe UI"), QStringLiteral("Tahoma")});
#else
    f.setFamilies({QStringLiteral("Inter"), QStringLiteral("DejaVu Sans"),
                   QStringLiteral("Sans Serif")});
#endif
    f.setStyleHint(QFont::SansSerif);
    f.setPointSize(pointSize);
    f.setBold(bold);
    return f;
}

QFont theme::monoFont(int pointSize)
{
    QFont f;
    f.setFamilies({QStringLiteral("Consolas"), QStringLiteral("Cascadia Mono"),
                   QStringLiteral("DejaVu Sans Mono"), QStringLiteral("Monospace")});
    // Both are needed: styleHint picks the fallback when none of the families
    // exist, fixedPitch stops Qt substituting a proportional face for them.
    f.setStyleHint(QFont::Monospace);
    f.setFixedPitch(true);
    f.setPointSize(pointSize);
    return f;
}

void theme::apply(QApplication &app)
{
    // Fusion is the only style on Windows that honours a custom palette; the
    // native style paints its own chrome and would leave grey title bars and
    // white item views behind the stylesheet.
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        app.setStyle(fusion);

    QPalette p;
    p.setColor(QPalette::Window, windowBg());
    p.setColor(QPalette::WindowText, text());
    p.setColor(QPalette::Base, windowBg());
    p.setColor(QPalette::AlternateBase, rowAlt());
    p.setColor(QPalette::Text, text());
    p.setColor(QPalette::Button, headerBg());
    p.setColor(QPalette::ButtonText, text());
    p.setColor(QPalette::BrightText, textBright());
    p.setColor(QPalette::Highlight, accentFill());
    p.setColor(QPalette::HighlightedText, textBright());
    p.setColor(QPalette::ToolTipBase, headerBg());
    p.setColor(QPalette::ToolTipText, text());
    p.setColor(QPalette::PlaceholderText, textDim());
    p.setColor(QPalette::Link, accentHi());
    p.setColor(QPalette::LinkVisited, accentLo());

    // Fusion draws every frame and bevel from these four, so leaving them at
    // the light defaults is what makes a half-themed app look grey.
    p.setColor(QPalette::Light, border());
    p.setColor(QPalette::Midlight, hoverBg());
    p.setColor(QPalette::Mid, panelBg());
    p.setColor(QPalette::Dark, canvasBg());
    p.setColor(QPalette::Shadow, shadow());

    // Fusion derives disabled text by blending Text towards Window, which on a
    // near-black palette lands close to invisible. Pin every role instead.
    p.setColor(QPalette::Disabled, QPalette::WindowText, disabledText());
    p.setColor(QPalette::Disabled, QPalette::Text, disabledText());
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText());
    p.setColor(QPalette::Disabled, QPalette::ToolTipText, disabledText());
    p.setColor(QPalette::Disabled, QPalette::Window, windowBg());
    p.setColor(QPalette::Disabled, QPalette::Base, panelBg());
    p.setColor(QPalette::Disabled, QPalette::AlternateBase, panelBg());
    p.setColor(QPalette::Disabled, QPalette::Button, panelBg());
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor("#343f4d"));
    // Not textDim: the brand's muted grey is darker than the one this band was
    // tuned against, and a selected row in a disabled list would read at 3.0:1
    // instead of the 3.8:1 it had. Lifted just far enough to get that back.
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor("#919cad"));
    p.setColor(QPalette::Disabled, QPalette::PlaceholderText, QColor("#5f6772"));
    p.setColor(QPalette::Disabled, QPalette::Light, panelBg());

    // Inactive matches Active everywhere except selection, which the sheet
    // dims via :!active, so an unfocused panel does not go flat grey.
    p.setColor(QPalette::Inactive, QPalette::Highlight, accentLo());
    p.setColor(QPalette::Inactive, QPalette::HighlightedText, text());

    app.setPalette(p);
    app.setFont(uiFont());
    app.setStyleSheet(styleSheet());
}
