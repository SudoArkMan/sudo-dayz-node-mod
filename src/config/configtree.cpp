#include "config/configtree.h"

#include "enforce/lexer.h"

#include <algorithm>

namespace {

const QChar kUnitSep(0x1f);
const QChar kRecordSep(0x1e);

bool isTrivia(const Token &t)
{
    return t.kind == TokenKind::Whitespace || t.kind == TokenKind::Comment;
}

bool isPunct(const Token &t, const char *text)
{
    return t.kind == TokenKind::Punctuation && t.text == QLatin1String(text);
}

// A property name or a class name. Types are in here because `class Managed`
// and a key called `map` both tokenise as Type, and ClassName is what the
// highlighter marks when the catalogue knows the word.
bool isNameToken(const Token &t)
{
    switch (t.kind) {
    case TokenKind::Identifier:
    case TokenKind::Type:
    case TokenKind::ClassName:
        return true;
    case TokenKind::Keyword:
        return t.text != QLatin1String("class");
    default:
        return false;
    }
}

// How many characters of an operator token are the assignment, 0 if it is not
// one. The tokeniser is greedy about operator characters, so `x=-1;` arrives as
// a single "=-" and the '-' belongs to the value. Vanilla is full of those.
int assignLength(const Token &t)
{
    if (t.kind != TokenKind::Operator || t.text.isEmpty()) return 0;
    if (t.text.startsWith(QLatin1String("+=")) || t.text.startsWith(QLatin1String("-=")))
        return 2;
    if (!t.text.startsWith(QLatin1Char('='))) return 0;
    if (t.text.size() > 1 && t.text.at(1) == QLatin1Char('=')) return 0;  // a comparison
    return 1;
}

QString leadingSpace(const QString &s)
{
    int i = 0;
    while (i < s.size() && s.at(i).isSpace()) i++;
    return s.left(i);
}

QString trailingSpace(const QString &s)
{
    int i = s.size();
    while (i > 0 && s.at(i - 1).isSpace()) i--;
    return s.mid(i);
}

// Indentation of the current line, taken from the trivia that led up to a
// construct. Everything after the last newline is what the construct sits on.
QString indentFrom(const QString &lead)
{
    const int nl = lead.lastIndexOf(QLatin1Char('\n'));
    if (nl < 0) return QString();
    return lead.mid(nl + 1);
}

QString valueHeadSig(const ConfigValue &v)
{
    return v.name + kUnitSep + (v.isArray ? QStringLiteral("[]") : QString()) + kUnitSep + v.op;
}

QString valueBodySig(const ConfigValue &v)
{
    if (v.isArray && (v.fmt.braced || !v.items.isEmpty() || v.scalar.isEmpty()))
        return QStringLiteral("A") + kUnitSep + v.items.join(kRecordSep);
    return QStringLiteral("S") + kUnitSep + v.scalar;
}

QString classHeadSig(const ConfigClass &c)
{
    return c.name + kUnitSep + c.base + kUnitSep + (c.external ? QLatin1Char('1') : QLatin1Char('0'));
}

bool valueWasParsed(const ConfigValue &v) { return !v.fmt.head.isEmpty(); }
bool classWasParsed(const ConfigClass &c) { return !c.fmt.headPrefix.isEmpty(); }

// ---------------------------------------------------------------- parsing

class ConfigParser {
public:
    explicit ConfigParser(const QString &src)
        : m_src(src), m_toks(EnforceLexer::tokenizeAll(src))
    {
        m_lineStart.append(0);
        for (int i = 0; i < src.size(); ++i)
            if (src.at(i) == QLatin1Char('\n')) m_lineStart.append(i + 1);
    }

    ConfigFile run()
    {
        ConfigFile file;
        file.newline = m_src.contains(QStringLiteral("\r\n"))
                           ? QStringLiteral("\r\n") : QStringLiteral("\n");

        readPreamble(file);
        QString tail;
        parseBody(file.values, file.classes, tail);
        file.trailer = tail;

        // Anything the body loop refused to enter, such as a stray '}' at the
        // top level, is still ahead of the cursor. Keep it.
        if (m_i < m_toks.size()) {
            const int from = offset();
            error(from, QStringLiteral("stray '%1' at the top level, kept as is")
                            .arg(m_toks.at(m_i).text));
            file.trailer += m_src.mid(from);
            m_i = m_toks.size();
        }
        file.errors = m_errors;
        return file;
    }

private:
    const QString &m_src;
    QVector<Token> m_toks;
    QVector<int> m_lineStart;
    QStringList m_errors;
    int m_i = 0;

    int tokenEnd(int k) const { return m_toks.at(k).start + m_toks.at(k).length; }

    // One name, cursor left just past it. Config names may start with a digit,
    // as `class 1kHz_mono_1s_SoundShader` in the vanilla sound configs does, and
    // the tokeniser splits that into a number and an identifier. Tokens with no
    // gap between them are one name.
    bool readName(QString &name, int &nameEnd)
    {
        const Token *t = peek();
        if (!t || (!isNameToken(*t) && t->kind != TokenKind::Number)) return false;
        name = t->text;
        nameEnd = tokenEnd(m_i);
        m_i++;
        while (m_i < m_toks.size()) {
            const Token &x = m_toks.at(m_i);
            if (x.start != nameEnd) break;
            if (!isNameToken(x) && x.kind != TokenKind::Number) break;
            name += x.text;
            nameEnd = tokenEnd(m_i);
            m_i++;
        }
        return true;
    }
    int offset() const { return m_i < m_toks.size() ? m_toks.at(m_i).start : m_src.size(); }
    const Token *peek() const { return m_i < m_toks.size() ? &m_toks.at(m_i) : nullptr; }

    void skipTrivia()
    {
        while (m_i < m_toks.size() && isTrivia(m_toks.at(m_i))) m_i++;
    }

    void advanceTo(int charOffset)
    {
        while (m_i < m_toks.size() && m_toks.at(m_i).start < charOffset) m_i++;
    }

    int lineOf(int charOffset) const
    {
        const auto it = std::upper_bound(m_lineStart.begin(), m_lineStart.end(), charOffset);
        return int(it - m_lineStart.begin());
    }

    void error(int charOffset, const QString &what)
    {
        m_errors << QStringLiteral("line %1: %2").arg(lineOf(charOffset)).arg(what);
    }

    // A preprocessor directive runs to the end of its line, and keeps running
    // while the line ends in a backslash.
    int endOfDirective(int from) const
    {
        int at = from;
        for (;;) {
            int nl = m_src.indexOf(QLatin1Char('\n'), at);
            if (nl < 0) return m_src.size();
            int back = nl;
            while (back > at && m_src.at(back - 1).isSpace()) back--;
            if (back > at && m_src.at(back - 1) == QLatin1Char('\\')) { at = nl + 1; continue; }
            return nl + 1;
        }
    }

    // Comments, blank lines and #include up to the first class or property.
    void readPreamble(ConfigFile &file)
    {
        for (;;) {
            skipTrivia();
            const Token *t = peek();
            if (!t) break;
            if (t->kind != TokenKind::Preprocessor) break;
            advanceTo(endOfDirective(t->start));
        }
        file.preamble = m_src.left(offset());
    }

    void parseBody(QVector<ConfigValue> &values, QVector<ConfigClass> &classes,
                   QString &bodyTail)
    {
        int seq = 0;
        int leadStart = offset();
        for (;;) {
            skipTrivia();
            const Token *t = peek();
            if (!t) break;
            if (isPunct(*t, "}")) break;

            const int itemStart = t->start;
            const QString lead = m_src.mid(leadStart, itemStart - leadStart);
            const int save = m_i;

            if (t->kind == TokenKind::Keyword && t->text == QLatin1String("class")) {
                ConfigClass c;
                if (parseClass(c, lead)) {
                    c.seq = seq++;
                    classes.append(c);
                    leadStart = offset();
                    continue;
                }
                m_i = save;
            } else if (isNameToken(*t) || t->kind == TokenKind::Number) {
                ConfigValue v;
                if (parseValue(v, lead)) {
                    v.seq = seq++;
                    values.append(v);
                    leadStart = offset();
                    continue;
                }
                m_i = save;
            }

            // Not understood. Leave the cursor past it without starting a new
            // member, so the text lands in the next member's lead and comes
            // back out of writeConfig untouched. A preprocessor line is not an
            // error, it just has no place in the tree.
            if (t->kind != TokenKind::Preprocessor)
                error(itemStart, QStringLiteral("kept verbatim, not understood: %1")
                                     .arg(snippet(itemStart)));
            consumeRaw();
            if (m_i == save) m_i++;  // never stall
        }
        bodyTail = m_src.mid(leadStart, offset() - leadStart);
    }

    QString snippet(int from) const
    {
        int nl = m_src.indexOf(QLatin1Char('\n'), from);
        if (nl < 0) nl = m_src.size();
        QString s = m_src.mid(from, qMin(nl - from, 48)).trimmed();
        return s;
    }

    // Swallow one unreadable statement: to the ';' that closes it, or up to the
    // '}' that closes the block it sits in, whichever comes first.
    void consumeRaw()
    {
        const Token *t = peek();
        if (t && t->kind == TokenKind::Preprocessor) {
            advanceTo(endOfDirective(t->start));
            return;
        }
        int depth = 0;
        while (m_i < m_toks.size()) {
            const Token &x = m_toks.at(m_i);
            if (x.kind == TokenKind::Punctuation) {
                if (x.text == QLatin1String("{") || x.text == QLatin1String("(")
                    || x.text == QLatin1String("[")) {
                    depth++;
                } else if (x.text == QLatin1String("}") || x.text == QLatin1String(")")
                           || x.text == QLatin1String("]")) {
                    if (depth == 0) return;
                    depth--;
                } else if (x.text == QLatin1String(";") && depth == 0) {
                    m_i++;
                    return;
                }
            }
            m_i++;
        }
    }

    bool parseClass(ConfigClass &c, const QString &lead)
    {
        const int classStart = offset();
        c.line = lineOf(classStart);
        c.fmt.lead = lead;
        c.fmt.indent = indentFrom(lead);
        m_i++;  // past 'class'

        skipTrivia();
        int nameEnd = 0;
        if (!readName(c.name, nameEnd)) return false;

        skipTrivia();
        const Token *t = peek();
        if (t && t->kind == TokenKind::Operator && t->text == QLatin1String(":")) {
            m_i++;
            skipTrivia();
            if (!readName(c.base, nameEnd)) return false;
            skipTrivia();
            t = peek();
        }

        if (!t) return false;

        if (isPunct(*t, ";")) {
            c.external = true;
            c.fmt.headPrefix = m_src.mid(classStart, nameEnd - classStart);
            c.fmt.headSuffix = m_src.mid(nameEnd, tokenEnd(m_i) - nameEnd);
            c.fmt.sigHead = classHeadSig(c);
            m_i++;
            return true;
        }

        if (!isPunct(*t, "{")) return false;
        const int braceEnd = tokenEnd(m_i);
        c.fmt.headPrefix = m_src.mid(classStart, nameEnd - classStart);
        c.fmt.headSuffix = m_src.mid(nameEnd, braceEnd - nameEnd);
        c.fmt.sigHead = classHeadSig(c);
        m_i++;  // past '{'

        parseBody(c.values, c.classes, c.fmt.bodyTail);
        c.fmt.childIndent = childIndentOf(c);

        skipTrivia();
        if (m_i >= m_toks.size()) {
            error(classStart, QStringLiteral("class %1 is never closed").arg(c.name));
            return true;  // bodyTail already holds the rest of the file
        }
        const int closeStart = m_toks.at(m_i).start;
        int footerEnd = tokenEnd(m_i);
        m_i++;  // past '}'

        const int afterBrace = m_i;
        skipTrivia();
        const Token *semi = peek();
        if (semi && isPunct(*semi, ";")) {
            footerEnd = tokenEnd(m_i);
            m_i++;
        } else {
            m_i = afterBrace;
        }
        c.fmt.footer = m_src.mid(closeStart, footerEnd - closeStart);
        return true;
    }

    QString childIndentOf(const ConfigClass &c) const
    {
        QString first;
        int best = INT_MAX;
        for (const ConfigValue &v : c.values)
            if (v.seq < best) { best = v.seq; first = v.fmt.lead; }
        for (const ConfigClass &k : c.classes)
            if (k.seq < best) { best = k.seq; first = k.fmt.lead; }
        if (best != INT_MAX && first.contains(QLatin1Char('\n')))
            return indentFrom(first);
        return c.fmt.indent + QLatin1Char('\t');
    }

    bool parseValue(ConfigValue &v, const QString &lead)
    {
        const int stmtStart = offset();
        v.line = lineOf(stmtStart);
        v.fmt.lead = lead;
        int nameEnd = 0;
        if (!readName(v.name, nameEnd)) return false;

        skipTrivia();
        const Token *t = peek();
        if (t && isPunct(*t, "[")) {
            m_i++;
            skipTrivia();
            t = peek();
            if (!t || !isPunct(*t, "]")) return false;
            v.isArray = true;
            m_i++;
            skipTrivia();
            t = peek();
        }

        const int opLen = t ? assignLength(*t) : 0;
        if (opLen == 0) return false;
        v.op = t->text.left(opLen);
        const int opEnd = t->start + opLen;
        m_i++;

        // The ';' that ends the statement, ignoring the ones nested in a list.
        int depth = 0, semiTok = -1;
        for (int k = m_i; k < m_toks.size(); ++k) {
            const Token &x = m_toks.at(k);
            if (x.kind != TokenKind::Punctuation) continue;
            if (x.text == QLatin1String("{") || x.text == QLatin1String("(")
                || x.text == QLatin1String("[")) {
                depth++;
            } else if (x.text == QLatin1String("}") || x.text == QLatin1String(")")
                       || x.text == QLatin1String("]")) {
                if (depth == 0) break;
                depth--;
            } else if (x.text == QLatin1String(";") && depth == 0) {
                semiTok = k;
                break;
            }
        }
        if (semiTok < 0) return false;

        const int bodyStart = opEnd;
        const int bodyEnd = m_toks.at(semiTok).start;
        v.fmt.head = m_src.mid(stmtStart, opEnd - stmtStart);
        v.fmt.body = m_src.mid(bodyStart, bodyEnd - bodyStart);
        v.fmt.tail = m_src.mid(bodyEnd, tokenEnd(semiTok) - bodyEnd);
        v.fmt.pre = leadingSpace(v.fmt.body);
        v.fmt.post = trailingSpace(v.fmt.body.mid(v.fmt.pre.size()));

        readList(v, m_i, semiTok, bodyStart, bodyEnd);
        m_i = semiTok + 1;
        v.fmt.sigHead = valueHeadSig(v);
        v.fmt.sigBody = valueBodySig(v);
        return true;
    }

    // Splits `{ a, b }` into entries, keeping the padding and the separator so
    // a one entry change stays a one line change. Anything that is not a plain
    // brace list is left as a scalar, which is what preserves it.
    void readList(ConfigValue &v, int fromTok, int toTok, int bodyStart, int bodyEnd)
    {
        v.scalar = m_src.mid(bodyStart, bodyEnd - bodyStart).trimmed();
        v.fmt.sep = QStringLiteral(", ");
        if (!v.isArray) return;

        int openTok = -1, closeTok = -1, depth = 0;
        for (int k = fromTok; k < toTok; ++k) {
            const Token &x = m_toks.at(k);
            if (isTrivia(x)) continue;
            if (openTok < 0) {
                if (isPunct(x, "{")) { openTok = k; depth = 1; continue; }
                return;  // not a brace list, the scalar keeps it
            }
            if (x.kind != TokenKind::Punctuation) continue;
            if (x.text == QLatin1String("{") || x.text == QLatin1String("(")
                || x.text == QLatin1String("[")) {
                depth++;
            } else if (x.text == QLatin1String("}") || x.text == QLatin1String(")")
                       || x.text == QLatin1String("]")) {
                depth--;
                if (depth == 0) { closeTok = k; break; }
            }
        }
        if (openTok < 0 || closeTok < 0) return;
        for (int k = closeTok + 1; k < toTok; ++k)
            if (!isTrivia(m_toks.at(k))) return;  // trailing junk after the list

        const int innerStart = tokenEnd(openTok);
        const int innerEnd = m_toks.at(closeTok).start;
        const QString inner = m_src.mid(innerStart, innerEnd - innerStart);

        QVector<QPair<int, int>> segs;
        int segStart = innerStart;
        depth = 0;
        for (int k = openTok + 1; k < closeTok; ++k) {
            const Token &x = m_toks.at(k);
            if (x.kind != TokenKind::Punctuation) continue;
            if (x.text == QLatin1String("{") || x.text == QLatin1String("(")
                || x.text == QLatin1String("[")) {
                depth++;
            } else if (x.text == QLatin1String("}") || x.text == QLatin1String(")")
                       || x.text == QLatin1String("]")) {
                depth--;
            } else if (x.text == QLatin1String(",") && depth == 0) {
                segs.append({segStart, x.start});
                segStart = x.start + 1;
            }
        }
        segs.append({segStart, innerEnd});

        QVector<QPair<int, int>> kept;
        for (const auto &seg : segs) {
            int a = seg.first, b = seg.second;
            while (a < b && m_src.at(a).isSpace()) a++;
            while (b > a && m_src.at(b - 1).isSpace()) b--;
            if (a >= b) continue;  // an empty slot, such as a trailing comma
            kept.append({a, b});
            v.items << m_src.mid(a, b - a);
        }

        v.fmt.braced = true;
        v.scalar.clear();
        if (kept.isEmpty()) {
            v.fmt.padOpen = inner;
            v.fmt.padClose.clear();
        } else {
            v.fmt.padOpen = leadingSpace(inner);
            v.fmt.padClose = trailingSpace(inner);
        }
        if (kept.size() >= 2)
            v.fmt.sep = m_src.mid(kept.at(0).second, kept.at(1).first - kept.at(0).second);
    }
};

// ---------------------------------------------------------------- writing

QString writeValue(const ConfigValue &v)
{
    const ValueFormat &f = v.fmt;
    const bool fresh = !valueWasParsed(v);

    QString out;
    if (fresh || f.sigHead != valueHeadSig(v))
        out += v.name + (v.isArray ? QStringLiteral("[]") : QString())
               + QLatin1Char(' ') + v.op;
    else
        out += f.head;

    if (fresh || f.sigBody != valueBodySig(v)) {
        const QString pre = (fresh && f.pre.isEmpty()) ? QStringLiteral(" ") : f.pre;
        const QString sep = f.sep.isEmpty() ? QStringLiteral(", ") : f.sep;
        if (v.isArray && (f.braced || !v.items.isEmpty() || v.scalar.isEmpty())) {
            out += pre + QLatin1Char('{') + f.padOpen + v.items.join(sep)
                   + f.padClose + QLatin1Char('}') + f.post;
        } else {
            out += pre + v.scalar + f.post;
        }
    } else {
        out += f.body;
    }

    out += f.tail.isEmpty() ? QStringLiteral(";") : f.tail;
    return out;
}

QString writeClass(const ConfigClass &c, const QString &newline);

// One pass over a container's members in source order. New members carry no
// sequence, so they sort to the end and get an indent made up on the spot.
QString writeMembers(const QVector<ConfigValue> &values, const QVector<ConfigClass> &classes,
                     const QString &childIndent, const QString &newline)
{
    struct Member {
        int seq;
        int tie;
        const ConfigValue *value;
        const ConfigClass *cls;
    };
    QVector<Member> members;
    members.reserve(values.size() + classes.size());
    int tie = 0;
    for (const ConfigValue &v : values) members.append({v.seq, tie++, &v, nullptr});
    for (const ConfigClass &k : classes) members.append({k.seq, tie++, nullptr, &k});
    std::stable_sort(members.begin(), members.end(), [](const Member &a, const Member &b) {
        if (a.seq != b.seq) return a.seq < b.seq;
        return a.tie < b.tie;
    });

    QString out;
    for (const Member &m : members) {
        if (m.value) {
            out += valueWasParsed(*m.value) ? m.value->fmt.lead : newline + childIndent;
            out += writeValue(*m.value);
        } else {
            out += classWasParsed(*m.cls) ? m.cls->fmt.lead : newline + childIndent;
            out += writeClass(*m.cls, newline);
        }
    }
    return out;
}

QString writeClass(const ConfigClass &c, const QString &newline)
{
    const ClassFormat &f = c.fmt;
    const bool fresh = !classWasParsed(c);

    QString head;
    if (fresh || f.sigHead != classHeadSig(c)) {
        head = QStringLiteral("class ") + c.name;
        if (!c.base.isEmpty()) head += QStringLiteral(" : ") + c.base;
    } else {
        head = f.headPrefix;
    }

    if (c.external) {
        const bool keep = !fresh && f.headSuffix.contains(QLatin1Char(';'));
        return head + (keep ? f.headSuffix : QStringLiteral(";"));
    }

    const QString indent = f.indent;
    const QString childIndent = f.childIndent.isEmpty()
                                    ? indent + QLatin1Char('\t') : f.childIndent;
    const bool keepSuffix = !fresh && f.headSuffix.contains(QLatin1Char('{'));
    head += keepSuffix ? f.headSuffix : newline + indent + QLatin1Char('{');

    QString out = head;
    out += writeMembers(c.values, c.classes, childIndent, newline);
    // An empty footer on a parsed class means the source ran out before the
    // '}' did. Inventing one here would change a file the user has not fixed
    // yet, so the damage is reported by parseConfig and left alone.
    out += fresh ? newline + indent + QStringLiteral("};") : f.bodyTail + f.footer;
    return out;
}

int nextSeq(const ConfigClass &parent)
{
    int top = -1;
    for (const ConfigValue &v : parent.values)
        if (v.seq != INT_MAX) top = qMax(top, v.seq);
    for (const ConfigClass &c : parent.classes)
        if (c.seq != INT_MAX) top = qMax(top, c.seq);
    return top + 1;
}

QStringList splitPath(const QString &path)
{
    return path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

ConfigClass *childClass(QVector<ConfigClass> &classes, const QString &name)
{
    for (ConfigClass &c : classes)
        if (c.name == name) return &c;
    return nullptr;
}

} // namespace

ConfigFile parseConfig(const QString &text)
{
    ConfigParser parser(text);
    return parser.run();
}

QString writeConfig(const ConfigFile &file)
{
    QString out = file.preamble;
    out += writeMembers(file.values, file.classes, QString(), file.newline);
    out += file.trailer;
    return out;
}

ConfigClass *findClass(ConfigFile &file, const QString &path)
{
    const QStringList parts = splitPath(path);
    if (parts.isEmpty()) return nullptr;
    ConfigClass *cur = childClass(file.classes, parts.first());
    for (int i = 1; cur && i < parts.size(); ++i)
        cur = childClass(cur->classes, parts.at(i));
    return cur;
}

const ConfigClass *findClass(const ConfigFile &file, const QString &path)
{
    return findClass(const_cast<ConfigFile &>(file), path);
}

ConfigClass *findClass(ConfigClass &parent, const QString &path)
{
    const QStringList parts = splitPath(path);
    ConfigClass *cur = &parent;
    for (const QString &part : parts) {
        cur = childClass(cur->classes, part);
        if (!cur) return nullptr;
    }
    return cur;
}

ConfigValue *findValue(ConfigClass &parent, const QString &name)
{
    for (ConfigValue &v : parent.values)
        if (v.name == name) return &v;
    return nullptr;
}

ConfigValue *findValue(ConfigFile &file, const QString &path)
{
    QStringList parts = splitPath(path);
    if (parts.isEmpty()) return nullptr;
    const QString leaf = parts.takeLast();
    if (parts.isEmpty()) {
        for (ConfigValue &v : file.values)
            if (v.name == leaf) return &v;
        return nullptr;
    }
    ConfigClass *owner = findClass(file, parts.join(QLatin1Char('/')));
    return owner ? findValue(*owner, leaf) : nullptr;
}

ConfigValue *addValue(ConfigClass &parent, const QString &name, bool isArray)
{
    ConfigValue v;
    v.name = name;
    v.isArray = isArray;
    v.seq = nextSeq(parent);
    parent.values.append(v);
    return &parent.values.last();
}

ConfigClass *addClass(ConfigClass &parent, const QString &name, const QString &base)
{
    ConfigClass c;
    c.name = name;
    c.base = base;
    c.seq = nextSeq(parent);
    c.fmt.indent = parent.fmt.childIndent.isEmpty()
                       ? parent.fmt.indent + QLatin1Char('\t') : parent.fmt.childIndent;
    c.fmt.childIndent = c.fmt.indent + QLatin1Char('\t');
    parent.classes.append(c);
    return &parent.classes.last();
}

ConfigClass *addClass(ConfigFile &file, const QString &name, const QString &base)
{
    ConfigClass c;
    c.name = name;
    c.base = base;
    int top = -1;
    for (const ConfigClass &k : file.classes)
        if (k.seq != INT_MAX) top = qMax(top, k.seq);
    for (const ConfigValue &v : file.values)
        if (v.seq != INT_MAX) top = qMax(top, v.seq);
    c.seq = top + 1;
    c.fmt.childIndent = QStringLiteral("\t");
    file.classes.append(c);
    return &file.classes.last();
}

bool removeValue(ConfigClass &parent, const QString &name)
{
    for (int i = 0; i < parent.values.size(); ++i) {
        if (parent.values.at(i).name != name) continue;
        parent.values.removeAt(i);
        return true;
    }
    return false;
}

bool removeClass(ConfigClass &parent, const QString &name)
{
    for (int i = 0; i < parent.classes.size(); ++i) {
        if (parent.classes.at(i).name != name) continue;
        parent.classes.removeAt(i);
        return true;
    }
    return false;
}

QString configLiteral(const QString &text)
{
    // Same treatment the mod scaffolder gives a display name, so a value typed
    // into the tree and one written by New mod come out identical.
    QString body = text.simplified();
    body.replace(QLatin1Char('"'), QLatin1Char('\''));
    return QLatin1Char('"') + body + QLatin1Char('"');
}

QString configUnquote(const QString &literal)
{
    const QString s = literal.trimmed();
    if (s.size() >= 2 && s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"')))
        return s.mid(1, s.size() - 2);
    return s;
}
