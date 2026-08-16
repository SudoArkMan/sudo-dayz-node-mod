// Node Palette dock.
//
// Two states, and they answer different questions. With nothing typed it shows
// the task index from nodeindex.h: groups named after what you are trying to
// do, ranked by what real mods reach for, so a node can be found without
// already knowing its name. With a query it searches the whole catalogue, index
// rows first.
//
// A footer says what the selected row will do before it is placed. Double-click
// or drag onto the canvas to place it.
#pragma once

#include "nodeindex.h"

#include <QWidget>

class Document;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

class PalettePanel : public QWidget {
    Q_OBJECT
public:
    explicit PalettePanel(Document *doc, QWidget *parent = nullptr);

    // Restricts results to members of a class (and its ancestors).
    void setClassFilter(const QString &className);
    // Puts the caret in the search box with the old query selected, so typing
    // replaces it. setFocus() on the panel lands here too, via the focus proxy.
    void focusSearch();
    // Puts a query in the box as if it had been typed, so another surface can
    // hand the palette a question. Also how the headless UI check photographs
    // the search results and the empty state.
    void search(const QString &query);

signals:
    // Emitted on double-click; the main window places it at the view centre.
    void nodeRequested(const QString &key);
    // The row that hands over to the Events dock. The panel does not raise the
    // dock itself: which docks exist and whether one is open is the window's.
    void eventsRequested();

private slots:
    void onSearchChanged(const QString &text);
    void onItemActivated(QTreeWidgetItem *item, int column);
    void onCurrentChanged();

protected:
    // The footer is one elided line, so it has to be recut whenever the dock
    // changes width.
    void resizeEvent(QResizeEvent *event) override;

private:
    Document *m_doc;
    QLineEdit *m_search;
    QTreeWidget *m_tree;
    QLabel *m_footer;
    QString m_classFilter;
    // What the footer is saying, before it was cut to fit.
    QString m_footerText;
    // The index, and the class it was built for. Building it resolves two dozen
    // method names through a search over 29k rows apiece, which is not a thing
    // to redo on every keystroke; the only input that changes is the class.
    QVector<IndexGroup> m_index;
    QString m_indexClass;
    bool m_indexLoaded = false;

    const QVector<IndexGroup> &indexFor(const QString &selfClass);
    void elideFooter();
    void populate(const QString &query);
    // Adds one group with its rows. `doc` becomes the heading's tooltip, so the
    // evidence behind the ordering is reachable without spending a row on it.
    void addGroup(const IndexGroup &group, bool collapsed);
    // The rows shown when nothing matched: what is true, and what to do next.
    void addEmptyState(const QString &query);
};
