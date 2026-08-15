// Syntax highlighting for Enforce Script, driven by the lexer.
//
// Knows about the catalogue as well as the grammar: an identifier that names a
// real vanilla class is coloured differently from one that does not, which is
// what turns "this compiles" into "this exists".
#pragma once

#include "lexer.h"

#include <QHash>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

class Catalog;

class EnforceHighlighter : public QSyntaxHighlighter {
    Q_OBJECT
public:
    explicit EnforceHighlighter(QTextDocument *doc, const Catalog *cat = nullptr);

    // Names declared by the surrounding graph (variables, functions, params).
    // They are known-good even though the catalogue has never heard of them.
    void setLocalSymbols(const QStringList &names);
    // Underline these ranges as problems; used for unbalanced braces and
    // identifiers nothing in scope can resolve.
    void setProblemLines(const QSet<int> &lines);

protected:
    void highlightBlock(const QString &text) override;

private:
    const Catalog *m_cat;
    QSet<QString> m_locals;
    QSet<int> m_problemLines;
    QHash<TokenKind, QTextCharFormat> m_formats;
    QTextCharFormat m_classFormat;
    QTextCharFormat m_localFormat;
    QTextCharFormat m_unknownFormat;
    QTextCharFormat m_problemFormat;
};
