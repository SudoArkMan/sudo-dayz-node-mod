// Graph Outliner dock: every node in the active graph, filterable by name.
// Selecting a row selects and frames the node on the canvas.
#pragma once

#include <QWidget>

class Document;
class QLineEdit;
class QListWidget;

class OutlinerPanel : public QWidget {
    Q_OBJECT
public:
    explicit OutlinerPanel(Document *doc, QWidget *parent = nullptr);

public slots:
    void refresh();

signals:
    void nodeActivated(const QString &nodeId);

private:
    Document *m_doc;
    QLineEdit *m_filter;
    QListWidget *m_list;
};
