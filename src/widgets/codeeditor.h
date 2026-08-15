// Enforce Script editor.
//
// A plain text box is not enough for the Raw nodes: they are where the graph
// gives up and the user writes real code, so that is exactly where a mistake
// is least visible. This gives them line numbers, highlighting, bracket
// matching, and completion drawn from the 29k-method catalogue.
#pragma once

#include <QPlainTextEdit>

class Catalog;
class Document;
class EnforceHighlighter;
class QCompleter;
class QAbstractItemModel;

class CodeEditor : public QPlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(QWidget *parent = nullptr);

    // Wiring the document turns on catalogue-backed completion and marks the
    // graph's own variables and functions as known symbols.
    void setDocumentContext(Document *doc);
    // Extra names to treat as declared: event parameters, loop variables.
    void setLocalSymbols(const QStringList &names);

    // Re-scans and marks problems. Returns the scan so a caller can show the
    // same findings elsewhere.
    struct Status { int braceBalance = 0; int parenBalance = 0; QStringList problems; };
    Status validate();

    void lineNumberAreaPaintEvent(QPaintEvent *event);
    int lineNumberAreaWidth() const;

signals:
    void statusChanged(const Status &status);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void highlightCurrentLine();
    void updateLineNumberArea(const QRect &rect, int dy);
    void insertCompletion(const QString &completion);

private:
    QString wordUnderCursor() const;
    void refreshCompletionModel(const QString &prefix);
    void matchBrackets(QList<QTextEdit::ExtraSelection> &selections);

    QWidget *m_lineNumberArea;
    EnforceHighlighter *m_highlighter;
    QCompleter *m_completer;
    QAbstractItemModel *m_completionModel;
    Document *m_doc = nullptr;
    QStringList m_locals;
};
