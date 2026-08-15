// Enforce Script syntax tree.
//
// The point of this is to stop hand-written code from being a black box. A
// parsed statement can be lowered into real nodes, so `m_RestApi =
// CreateRestApi();` becomes a Set node fed by a call node instead of a text
// box the tool cannot reason about.
//
// Owned by unique_ptr, so the containers here are std::vector: QVector copies its
// elements in places and will not compile against a move-only type.
// Immutable once built. Anything the parser cannot
// represent is kept verbatim in a Raw node so nothing is ever lost.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <vector>

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

enum class ExprKind {
    Literal,     // 12, 1.5, "text", true, null
    Name,        // m_RestApi, baseUrl, PlayerBase
    Member,      // <object>.<name>
    Call,        // <callee>(args...)
    Index,       // <object>[<index>]
    Unary,       // !x, -x, ~x
    Binary,      // a + b, a && b, a == b
    Ternary,     // cond ? a : b
    Assign,      // a = b, a += b
    New,         // new Class(args...)
    Cast,        // Class.Cast(x) and the CastTo pattern
    Paren,       // kept so round-tripping to text does not change meaning
};

enum class LiteralType { Int, Float, String, Bool, Null, Vector };

struct Expr {
    ExprKind kind = ExprKind::Name;

    // Literal
    LiteralType literalType = LiteralType::Int;
    QString text;          // literal text as written, or the name for Name

    // Member / Call / Index / Unary / Binary / Ternary / Assign / New / Cast
    ExprPtr target;        // object of a member, callee of a call, lhs
    ExprPtr second;        // rhs, index, ternary "then"
    ExprPtr third;         // ternary "else"
    std::vector<ExprPtr> args; // call and constructor arguments
    QString op;            // operator text, "=" / "+=" for Assign
    QString typeName;      // New and Cast target class

    int line = 0;          // 1-based, for error reporting
};

enum class StmtKind {
    Expression,  // a call or an assignment used as a statement
    VarDecl,     // Type name = init;  (several declarators allowed)
    If,
    For,
    ForEach,
    While,
    Return,
    Break,
    Continue,
    Block,
    Switch,
    Delete,
    Raw,         // anything the parser declined; `text` holds it verbatim
};

struct Declarator {
    QString name;
    ExprPtr init;
};

struct SwitchCase {
    ExprPtr value;         // null for `default`
    std::vector<StmtPtr> body;
};

struct Stmt {
    StmtKind kind = StmtKind::Raw;

    ExprPtr expr;          // Expression, If/While condition, Return value, Delete
    QString typeName;      // VarDecl declared type, ForEach element type
    std::vector<Declarator> decls;

    std::vector<StmtPtr> body;      // If-then, loop body, Block
    std::vector<StmtPtr> elseBody;  // If-else

    // For: init and step live here so the loop keeps its exact shape.
    StmtPtr forInit;
    ExprPtr forCond;
    ExprPtr forStep;

    // ForEach: `foreach (int i, Item x : list)` keeps both names. The first
    // variable is not always an int: iterating a map binds the key, so
    // `foreach (string name, int score : m_Scores)` has to keep `string` or
    // regenerating the loop changes what it does.
    QString eachIndexType;
    QString eachIndexName;
    QString eachValueName;
    ExprPtr eachCollection;

    std::vector<SwitchCase> cases;

    QString text;          // Raw statements, verbatim
    int line = 0;
};

// Result of parsing a block of statements.
struct ParseResult {
    std::vector<StmtPtr> statements;
    // One entry per statement the parser had to keep as Raw, with the reason.
    // A non-empty list is not a failure: it is the honest measure of how much
    // of a body this tool can model.
    QStringList notes;
    // Hard syntax problems (unbalanced braces and the like). When non-empty the
    // caller should leave the original text alone.
    QStringList errors;
    int statementCount = 0;
    int rawCount = 0;
};

// Parses a statement body, the inside of a method. Never throws: anything it
// cannot handle comes back as a Raw statement.
ParseResult parseEnforceBody(const QString &code);

// Renders an expression back to Enforce text. Used to keep the parts that
// could not be lowered, and to check a round trip in tests.
QString exprToText(const Expr &e);
QString stmtToText(const Stmt &s, int indent = 0);
