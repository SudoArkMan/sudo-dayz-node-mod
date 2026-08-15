// The editor for one Raw node, and the viewer for generated script.
//
// Opens on the node it belongs to, so the title says which chain the code sits
// in and the symbol list is that graph's own variables and functions.
#pragma once

#include <QDialog>

class CodeEditor;
class Document;
class QLabel;
class QDialogButtonBox;

class CodeDialog : public QDialog {
    Q_OBJECT
public:
    // Editable: returns the edited text through accepted().
    static bool editNodeCode(QWidget *parent, Document *doc, const QString &nodeId);
    // Read-only viewer with the same highlighting, for Tools > Generate.
    static void showGenerated(QWidget *parent, Document *doc, const QString &title,
                              const QString &code, const QStringList &warnings);

    CodeDialog(QWidget *parent, Document *doc, bool readOnly);

    void setCode(const QString &code);
    QString code() const;
    void setWarnings(const QStringList &warnings);

private slots:
    void onStatusChanged();

private:
    CodeEditor *m_editor;
    QLabel *m_status;
    QLabel *m_warnings;
    QDialogButtonBox *m_buttons;
    bool m_readOnly;
};
