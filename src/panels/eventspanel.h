// Events dock: the hooks the active script's class can override.
//
// The Node Palette answers "find the node called X", which is no use when you
// do not know that EEItemAttached exists. This is the other question: given
// this class, what can I hook? The list comes from eventsForClass(), so the
// handful a mod starts from sit at the top and the debug and deprecated
// entries sit at the bottom, collapsed.
//
// Double-click or Enter places the event. The panel does not place it itself:
// where a node lands, and what to do when the graph already has that override,
// are the window's decisions.
#pragma once

#include "events.h"

#include <QSet>
#include <QWidget>

class Document;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

class EventsPanel : public QWidget {
    Q_OBJECT
public:
    explicit EventsPanel(Document *doc, QWidget *parent = nullptr);

    // Puts the caret in the search box with the old query selected. setFocus()
    // on the panel lands here too, via the focus proxy.
    void focusSearch();

signals:
    void eventRequested(const QString &key);
    // The "New custom event..." row. The name is asked for by the window, which
    // is also what declares the function and drops its entry node.
    void customEventRequested();
    void statusMessage(const QString &text);

public slots:
    // Rebuilds when the class the events belong to has changed, or when the
    // graph gained or lost one of them. Cheap to call on every graph signal.
    void refresh();

private slots:
    void onItemActivated(QTreeWidgetItem *item, int column);
    void onCurrentChanged();

private:
    Document *m_doc;
    QLabel *m_class;
    QLineEdit *m_search;
    QTreeWidget *m_tree;
    QLabel *m_footer;
    // The class's events, read once per class rather than per keystroke: the
    // lookup walks every search row in a 29k-entry catalogue.
    QVector<EventInfo> m_events;
    // What the tree was last built from. Rebuilding on every graphChanged would
    // throw away the selection and the scroll position several times a second.
    QString m_shownClass;
    QSet<QString> m_shownPlaced;
    bool m_loaded = false;

    void populate();
    // The class whose events are listed: a modded script overrides methods on
    // the class it mods, a new one on the class it extends.
    QString targetClass() const;
};

// The same list as a popup, at the cursor. Unreal puts Add Event on the canvas
// and so does this: a dock you have to find first does not answer "what can I
// hook here" at the moment the question is being asked.
class EventPopup : public QWidget {
    Q_OBJECT
public:
    explicit EventPopup(Document *doc, QWidget *parent = nullptr);

    void popupAt(const QPoint &globalPos);

signals:
    void eventPicked(const QString &key);
    void customEventPicked();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    Document *m_doc;
    QLineEdit *m_search;
    QTreeWidget *m_tree;
    QLabel *m_footer;
    // A click and an activation can both land on one gesture; the first one
    // through wins, so a double-click cannot place the event twice.
    bool m_picked = false;

    void populate();
    void pick(QTreeWidgetItem *item);
    void updateFooter();
};
