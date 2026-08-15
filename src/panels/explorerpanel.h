// Mod Explorer: the files on disk behind the project.
//
// The graph is only part of a mod. config.cpp, the stringtable, types.xml and
// the Workbench project all have to be edited too, and alt-tabbing to a file
// manager to find them breaks the thread of what you were doing.
#pragma once

#include <QWidget>

class Document;
class QFileSystemModel;
class QTreeView;
class QLineEdit;
class QSortFilterProxyModel;

class ExplorerPanel : public QWidget {
    Q_OBJECT
public:
    explicit ExplorerPanel(Document *doc, QWidget *parent = nullptr);

    // Root of the tree. Empty hides the tree and shows the reason instead.
    void setModRoot(const QString &path);
    QString modRoot() const { return m_root; }

signals:
    // A text file the app can edit itself (.cpp, .xml, .json, .csv, .cfg).
    void fileActivated(const QString &path);
    // A .c, which is Enforce Script. This is a visual editor, so the main
    // window imports it and opens the graph; the text editor is the fallback
    // and sits on the context menu as "Edit as text".
    void scriptActivated(const QString &path);
    // A .sdzn project, which the main window opens as a project rather than text.
    void projectActivated(const QString &path);

private slots:
    void onActivated(const QModelIndex &index);
    void onContextMenu(const QPoint &pos);
    void onFilterChanged(const QString &text);

private:
    void revealInFileManager(const QString &path);
    QString selectedPath() const;

    Document *m_doc;
    QFileSystemModel *m_model;
    QSortFilterProxyModel *m_proxy;
    QTreeView *m_tree;
    QLineEdit *m_filter;
    QString m_root;
};
