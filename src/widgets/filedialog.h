// One file from the Mod Explorer, open in a window of its own.
//
// config.cpp, types.xml and the stringtable get edited beside the graph rather
// than instead of it, so these are ordinary top level windows: the canvas stays
// live while a file is open, and several files can be open at once. Opening a
// path that already has a window raises that window instead of loading the file
// twice, because two editors over one file means one of them loses its edits.
#pragma once

#include <QDialog>

class CodeEditor;
class Document;
class QDialogButtonBox;
class QLabel;

class FileDialog : public QDialog {
    Q_OBJECT
public:
    // Opens `path`, or raises the window already showing it. Returns nullptr
    // and fills `error` when the file cannot be read.
    static FileDialog *openFile(QWidget *parent, Document *doc, const QString &path,
                                QString *error = nullptr);

    FileDialog(QWidget *parent, Document *doc, const QString &path);
    ~FileDialog() override;

    QString path() const { return m_path; }

    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr);

public slots:
    // Routed through close() so the unsaved-changes check runs on Escape as
    // well as on the window button.
    void reject() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setDirty(bool dirty);
    void updateTitle();

    Document *m_doc;
    QString m_path;
    CodeEditor *m_editor;
    QLabel *m_status;
    QDialogButtonBox *m_buttons;
    bool m_dirty = false;
    bool m_loading = false;
    // Line endings are kept as they were found. Rewriting a whole config.cpp
    // from CRLF to LF over a one word edit turns the diff into the entire file.
    bool m_crlf = false;
    // Enforce completion and the brace balance check only make sense on script.
    // On an .xml or a .csv both are noise, so the status line carries the path.
    bool m_enforce = false;
};
