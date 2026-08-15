// Node Palette dock: search across the whole catalogue plus builtins,
// grouped by category. Double-click or drag onto the canvas to place.
#pragma once

#include <QWidget>

class Document;
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

signals:
    // Emitted on double-click; the main window places it at the view centre.
    void nodeRequested(const QString &key);

private slots:
    void onSearchChanged(const QString &text);
    void onItemActivated(QTreeWidgetItem *item, int column);

private:
    Document *m_doc;
    QLineEdit *m_search;
    QTreeWidget *m_tree;
    QString m_classFilter;

    void populate(const QString &query);
    void addGroup(const QString &title, const QVector<QPair<QString, QString>> &rows);
};
