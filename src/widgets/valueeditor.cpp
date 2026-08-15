#include "valueeditor.h"

#include "catalog.h"
#include "theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QSpinBox>
#include <QStringList>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

namespace {

// A 32-bit float needs nine significant decimal digits to read back as the same
// number, so nine is where the boxes stop.
constexpr int kFloatDecimals = 9;

// The shortest text that reads back as the same number. `keepPoint` holds a
// decimal point on it: a float slot handed `1` gets an int token, and Enforce
// counts that as a different type.
QString numberText(double v, bool keepPoint)
{
    for (int digits = keepPoint ? 1 : 0; digits <= kFloatDecimals; ++digits) {
        const QString text = QString::number(v, 'f', digits);
        if (text.toDouble() == v) return text;
    }
    return QString::number(v, 'f', kFloatDecimals);
}

// Hand-rolled rather than a regex, because a closing quote that is itself
// escaped does not close the literal.
bool isQuoted(const QString &s)
{
    if (s.size() < 2 || !s.startsWith(QLatin1Char('"')) || !s.endsWith(QLatin1Char('"')))
        return false;
    int slashes = 0;
    for (int i = s.size() - 2; i >= 1 && s.at(i) == QLatin1Char('\\'); --i) slashes++;
    return slashes % 2 == 0;
}

QString innerOf(const QString &quoted) { return quoted.mid(1, quoted.size() - 2); }

// A raw quote ends the literal early and a raw backslash eats the character
// after it, so both are escaped on the way out. The backslash goes first, or the
// escapes added here get escaped in turn.
QString quoteString(const QString &text)
{
    QString escaped = text;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + escaped + QLatin1Char('"');
}

// The inverse, and only for the two escapes quoteString writes. A "\n" from a
// file we did not write is left spelled out rather than turned into a real
// newline, which a line edit cannot hold and would not survive the way back.
QString unescapeString(const QString &inner)
{
    QString out;
    out.reserve(inner.size());
    for (int i = 0; i < inner.size(); ++i) {
        const QChar c = inner.at(i);
        if (c != QLatin1Char('\\') || i + 1 >= inner.size()) {
            out += c;
            continue;
        }
        const QChar next = inner.at(i + 1);
        if (next == QLatin1Char('\\') || next == QLatin1Char('"')) {
            out += next;
            ++i;
            continue;
        }
        out += c;
    }
    return out;
}

// An Enforce vector literal is three numbers in a quoted string. The quotes are
// optional here so a value typed without them still lands somewhere useful.
bool parseVector(const QString &literal, double out[3])
{
    QString body = literal.trimmed();
    if (isQuoted(body)) body = innerOf(body);
    const QStringList parts = body.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() != 3) return false;
    for (int i = 0; i < 3; ++i) {
        bool ok = false;
        const double v = parts.at(i).toDouble(&ok);
        if (!ok || !std::isfinite(v)) return false;
        out[i] = v;
    }
    return true;
}

QString vectorLiteral(const double xyz[3])
{
    return QStringLiteral("\"%1 %2 %3\"")
        .arg(numberText(xyz[0], false), numberText(xyz[1], false),
             numberText(xyz[2], false));
}

// Reads a whole number. `plain` comes back false for a literal written in
// another base: 0xFF is left as it was written, because a bitmask read as 255 is
// a bitmask nobody can check.
bool parseInt(const QString &literal, int *value, bool *plain)
{
    bool ok = false;
    const int n = literal.toInt(&ok, 10);
    if (ok) {
        if (value) *value = n;
        if (plain) *plain = true;
        return true;
    }
    const qlonglong wide = literal.toLongLong(&ok, 0);
    if (!ok || wide < std::numeric_limits<int>::min()
        || wide > std::numeric_limits<int>::max())
        return false;
    if (value) *value = int(wide);
    if (plain) *plain = false;
    return true;
}

bool parseFloat(const QString &literal, double *value)
{
    bool ok = false;
    const double d = literal.toDouble(&ok);
    if (!ok || !std::isfinite(d)) return false;
    if (value) *value = d;
    return true;
}

// Types with no class behind them. A typename slot takes these as readily as it
// takes a class name.
const QStringList &primitiveTypes()
{
    static const QStringList names = {
        QStringLiteral("bool"),   QStringLiteral("int"),
        QStringLiteral("float"),  QStringLiteral("string"),
        QStringLiteral("vector"), QStringLiteral("typename"),
        QStringLiteral("void"),   QStringLiteral("Class"),
        QStringLiteral("auto"),   QStringLiteral("Managed")};
    return names;
}

bool isKnownTypeName(const QString &name, const Catalog *cat)
{
    if (primitiveTypes().contains(name)) return true;
    // Without a catalogue nothing can be told apart, and a warning nobody can
    // act on is worse than no warning.
    if (!cat) return true;
    return cat->classId(name) >= 0 || cat->isEnum(name);
}

// A spin box that shows the number the way it will be written. The stock box
// pads to `decimals` ("1.500000"), which is not what lands in the file and reads
// as noise in a dock this narrow.
class FloatSpin : public QDoubleSpinBox {
public:
    FloatSpin(bool keepPoint, QWidget *parent)
        : QDoubleSpinBox(parent), m_keepPoint(keepPoint)
    {
        // The C locale, because textFromValue writes a dot and the box has to
        // accept back what it just showed.
        setLocale(QLocale::c());
        // The theme paints the step buttons as a bare block with no arrow on it,
        // and a button that shows nothing is worse than the keyboard and the
        // wheel, which still step the value.
        setButtonSymbols(QAbstractSpinBox::NoButtons);
        setDecimals(kFloatDecimals);
        const double limit = double(std::numeric_limits<float>::max());
        setRange(-limit, limit);
        // One signal per finished number rather than one per keystroke, so a
        // half typed "1.5" never arrives as 1.
        setKeyboardTracking(false);
    }

protected:
    QString textFromValue(double v) const override { return numberText(v, m_keepPoint); }

    // The stock hint sizes for the widest number in range, and the range here is
    // the whole float domain. Nothing that long is typed as a default.
    QSize sizeHint() const override { return capped(QDoubleSpinBox::sizeHint()); }
    QSize minimumSizeHint() const override
    {
        return capped(QDoubleSpinBox::minimumSizeHint());
    }

private:
    QSize capped(QSize hint) const
    {
        const int room = fontMetrics().horizontalAdvance(QStringLiteral("-000000.000"))
                         + 30 + fontMetrics().horizontalAdvance(prefix());
        hint.setWidth(qMin(hint.width(), room));
        return hint;
    }

    bool m_keepPoint;
};

// Why this literal would not compile in a slot of this type, or empty when it
// would. Kept apart from the widgets so the answer is the same whether the value
// was typed or loaded.
QString problemWith(const QString &literal, const PinType &pin, const Catalog *cat)
{
    const QString raw = literal.trimmed();
    // No default at all is a legal declaration, and an array or an object has
    // nothing here to check.
    if (raw.isEmpty() || pin.isArray) return {};

    switch (pin.kind) {
    case PinKind::Bool: {
        const QString low = raw.toLower();
        if (low == QLatin1String("true") || low == QLatin1String("false")
            || raw == QLatin1String("1") || raw == QLatin1String("0"))
            return {};
        return ValueEditor::tr("%1 is not true or false.").arg(raw);
    }
    case PinKind::Int:
        if (parseInt(raw, nullptr, nullptr)) return {};
        return ValueEditor::tr("%1 is not a whole number.").arg(raw);
    case PinKind::Float:
        if (parseFloat(raw, nullptr)) return {};
        return ValueEditor::tr("%1 is not a number.").arg(raw);
    case PinKind::Vector: {
        double xyz[3];
        if (parseVector(raw, xyz)) return {};
        return ValueEditor::tr(
            "A vector default is three numbers in quotes, like \"0 0 0\".");
    }
    case PinKind::Enum: {
        const QStringList members = cat ? cat->enumValues(pin.cls) : QStringList();
        if (members.isEmpty()) return {};
        const QString member = raw.section(QLatin1Char('.'), -1);
        const QString owner = raw.contains(QLatin1Char('.'))
                                  ? raw.section(QLatin1Char('.'), 0, -2)
                                  : pin.cls;
        if (owner == pin.cls && members.contains(member)) return {};
        // An Enforce enum is an int underneath, so a number in the slot is a
        // legal if joyless default.
        bool number = false;
        raw.toInt(&number);
        if (number) return {};
        return ValueEditor::tr("%1 is not a member of %2.").arg(raw, pin.cls);
    }
    case PinKind::Typename:
        if (isKnownTypeName(raw, cat)) return {};
        return ValueEditor::tr(
                   "The catalogue has no type called %1. Leave it if it is your own class.")
            .arg(raw);
    default:
        return {};
    }
}

} // namespace

ValueEditor::ValueEditor(const Catalog *catalog, QWidget *parent)
    : QWidget(parent), m_catalog(catalog)
{
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(2);

    m_row = new QHBoxLayout;
    m_row->setContentsMargins(0, 0, 0, 0);
    m_row->setSpacing(4);
    column->addLayout(m_row);

    m_noteLabel = new QLabel(this);
    m_noteLabel->setObjectName(QStringLiteral("valueNote"));
    m_noteLabel->setWordWrap(true);
    m_noteLabel->setStyleSheet(QStringLiteral("color: %1").arg(theme::textDim().name()));
    m_noteLabel->hide();
    column->addWidget(m_noteLabel);

    // An editor with no type set is still an editor: an empty type reads as Any,
    // which is a plain line edit.
    setType(QString());
}

void ValueEditor::setType(const QString &enforceType)
{
    const QString next = enforceType.trimmed();
    if (m_built && next == m_type) return;
    m_type = next;
    m_pin = pinTypeOf(m_type, [this](const QString &name) {
        return m_catalog && m_catalog->isEnum(name);
    });
    m_built = true;
    rebuild();
    applyValue(m_value);
}

void ValueEditor::setValue(const QString &literal)
{
    applyValue(literal);
}

bool ValueEditor::isValid(QString *reason) const
{
    const QString problem = problemWith(m_value, m_pin, m_catalog);
    if (reason) *reason = problem;
    return problem.isEmpty();
}

void ValueEditor::buildText(bool editable)
{
    m_text = new QLineEdit(this);
    m_text->setObjectName(QStringLiteral("textValue"));
    m_text->setEnabled(editable);
    m_row->addWidget(m_text, 1);
    if (editable)
        connect(m_text, &QLineEdit::textChanged, this, &ValueEditor::onEdited);
}

void ValueEditor::rebuild()
{
    const bool wasLoading = m_loading;
    m_loading = true;

    m_check = nullptr;
    m_int = nullptr;
    m_float = nullptr;
    m_vector[0] = m_vector[1] = m_vector[2] = nullptr;
    m_text = nullptr;
    m_choice = nullptr;
    m_extraRow = false;
    m_note.clear();

    // Disowned before it goes, not just taken out of the layout. The delete is
    // deferred so a type change made from a valueChanged handler cannot free the
    // widget that is still emitting, and until it lands a child of the old shape
    // would answer findChild ahead of the new one.
    while (QLayoutItem *item = m_row->takeAt(0)) {
        if (QWidget *old = item->widget()) {
            old->hide();
            old->setParent(nullptr);
            old->deleteLater();
        }
        delete item;
    }

    if (m_pin.isArray) {
        buildText(false);
        m_note = tr("An array or map default is a list of items, which this editor does "
                    "not write. Fill it in the constructor.");
    } else {
        switch (m_pin.kind) {
        case PinKind::Bool:
            m_check = new QCheckBox(this);
            m_check->setObjectName(QStringLiteral("boolValue"));
            m_row->addWidget(m_check);
            m_row->addStretch(1);
            connect(m_check, &QCheckBox::toggled, this, &ValueEditor::onEdited);
            break;

        case PinKind::Int:
            m_int = new QSpinBox(this);
            m_int->setObjectName(QStringLiteral("intValue"));
            m_int->setLocale(QLocale::c());
            m_int->setButtonSymbols(QAbstractSpinBox::NoButtons);
            m_int->setRange(std::numeric_limits<int>::min(),
                            std::numeric_limits<int>::max());
            m_int->setKeyboardTracking(false);
            m_row->addWidget(m_int, 1);
            connect(m_int, &QSpinBox::valueChanged, this, &ValueEditor::onEdited);
            break;

        case PinKind::Float:
            m_float = new FloatSpin(true, this);
            m_float->setObjectName(QStringLiteral("floatValue"));
            m_row->addWidget(m_float, 1);
            connect(m_float, &QDoubleSpinBox::valueChanged, this, &ValueEditor::onEdited);
            break;

        case PinKind::Vector: {
            static const char *const axes[3] = {"x ", "y ", "z "};
            static const char *const names[3] = {"vectorX", "vectorY", "vectorZ"};
            for (int i = 0; i < 3; ++i) {
                // The axis sits inside the box. Three labels and three boxes do
                // not fit across a dock this narrow.
                m_vector[i] = new FloatSpin(false, this);
                m_vector[i]->setObjectName(QString::fromLatin1(names[i]));
                m_vector[i]->setPrefix(QString::fromLatin1(axes[i]));
                m_row->addWidget(m_vector[i], 1);
                connect(m_vector[i], &QDoubleSpinBox::valueChanged,
                        this, &ValueEditor::onEdited);
            }
            break;
        }

        case PinKind::String:
            buildText(true);
            m_text->setPlaceholderText(tr("Text, written with the quotes added"));
            break;

        case PinKind::Enum: {
            const QStringList members = m_catalog ? m_catalog->enumValues(m_pin.cls)
                                                  : QStringList();
            m_choice = new QComboBox(this);
            m_choice->setObjectName(QStringLiteral("choiceValue"));
            m_choice->setMaxVisibleItems(14);
            // A combo sizes itself to its widest row by default. One long member
            // name would then set the width of the dock this sits in.
            m_choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            m_choice->setMinimumContentsLength(12);
            // A known enum is a closed list. An enum the catalogue carries no
            // members for is not, so there the box takes typing.
            m_choice->setEditable(members.isEmpty());
            m_choice->setInsertPolicy(QComboBox::NoInsert);
            if (!members.isEmpty()) m_choice->addItems(members);
            m_row->addWidget(m_choice, 1);
            connect(m_choice, &QComboBox::currentTextChanged,
                    this, &ValueEditor::onEdited);
            break;
        }

        case PinKind::Typename: {
            m_choice = new QComboBox(this);
            m_choice->setObjectName(QStringLiteral("choiceValue"));
            m_choice->setEditable(true);
            m_choice->setInsertPolicy(QComboBox::NoInsert);
            m_choice->setMaxVisibleItems(14);
            // Six thousand class names include some very long ones, and the
            // widest of them is not the width this field should take.
            m_choice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            m_choice->setMinimumContentsLength(12);
            QStringList names = primitiveTypes();
            if (m_catalog) {
                QStringList classes = m_catalog->classNames();
                classes.sort(Qt::CaseInsensitive);
                names += classes;
            }
            m_choice->addItems(names);
            auto *completer = new QCompleter(names, m_choice);
            completer->setCaseSensitivity(Qt::CaseInsensitive);
            // Contains rather than prefix: a class name is usually half
            // remembered from the middle, so "Item" has to reach ItemBase and
            // InventoryItem alike.
            completer->setFilterMode(Qt::MatchContains);
            completer->setMaxVisibleItems(14);
            m_choice->setCompleter(completer);
            m_row->addWidget(m_choice, 1);
            connect(m_choice, &QComboBox::currentTextChanged,
                    this, &ValueEditor::onEdited);
            break;
        }

        case PinKind::Object:
            if (m_catalog && m_catalog->classId(m_pin.cls) >= 0) {
                buildText(false);
                m_note = tr("An object member has no literal default. Build it in the "
                            "constructor and this stays null.");
            } else {
                // The catalogue knows every vanilla class, so a name missing from
                // it belongs to the mod: its own class, or its own enum. Neither
                // can be checked, and neither should be locked.
                buildText(true);
                m_note = tr("The catalogue does not know %1, so the value is written "
                            "exactly as it is typed.").arg(m_pin.cls);
            }
            break;

        default:
            buildText(true);
            break;
        }
    }

    m_noteLabel->setText(m_note);
    m_noteLabel->setVisible(!m_note.isEmpty());
    m_loading = wasLoading;
}

void ValueEditor::applyValue(const QString &literal)
{
    const bool wasLoading = m_loading;
    m_loading = true;

    const QString raw = literal.trimmed();
    m_value = raw;

    if (m_check) {
        const QString low = raw.toLower();
        m_check->setChecked(low == QLatin1String("true") || raw == QLatin1String("1"));
        // Spelling is settled here, the value is not: TRUE and true are the same
        // default, 1 is the same default written another way and is left alone.
        if (low == QLatin1String("true") || low == QLatin1String("false")) m_value = low;
    } else if (m_int) {
        int n = 0;
        bool plain = true;
        if (parseInt(raw, &n, &plain)) {
            m_int->setValue(n);
            if (plain) m_value = QString::number(n);
        } else {
            m_int->setValue(0);
        }
    } else if (m_float) {
        double d = 0.0;
        const bool ok = parseFloat(raw, &d);
        m_float->setValue(ok ? d : 0.0);
        // The box holds nine decimals. Where it would move the number, the
        // literal stays exactly as it was written.
        if (ok && m_float->value() == d) m_value = numberText(d, true);
    } else if (m_vector[0]) {
        double xyz[3] = {0.0, 0.0, 0.0};
        bool exact = parseVector(raw, xyz);
        for (int i = 0; i < 3; ++i) {
            m_vector[i]->setValue(xyz[i]);
            exact = exact && m_vector[i]->value() == xyz[i];
        }
        if (exact) m_value = vectorLiteral(xyz);
    } else if (m_choice && m_pin.kind == PinKind::Enum) {
        if (m_choice->isEditable()) {
            m_choice->setEditText(raw);
        } else {
            if (m_extraRow) {
                m_choice->removeItem(0);
                m_extraRow = false;
            }
            const QString member = raw.section(QLatin1Char('.'), -1);
            const QString owner = raw.contains(QLatin1Char('.'))
                                      ? raw.section(QLatin1Char('.'), 0, -2)
                                      : m_pin.cls;
            const int row = owner == m_pin.cls ? m_choice->findText(member) : -1;
            if (row >= 0) {
                m_choice->setCurrentIndex(row);
                // A bare member name does not compile on its own; Enforce reads
                // a member through its enum.
                m_value = enumLiteral(member);
            } else {
                // A value the enum has no member for still belongs on screen, so
                // it goes into the list as its own row. An empty one says the
                // declaration has no default, which no member can say.
                m_choice->insertItem(0, raw);
                m_extraRow = true;
                m_choice->setCurrentIndex(0);
            }
        }
    } else if (m_choice) {
        m_choice->setEditText(raw);
    } else if (m_text) {
        // The inert field first: array<string> is a string kind with isArray on,
        // and a list of strings is not one string to be quoted.
        if (!m_text->isEnabled()) {
            m_text->setText(raw.isEmpty() ? QStringLiteral("null") : raw);
        } else if (m_pin.kind == PinKind::String) {
            if (isQuoted(raw)) {
                m_text->setText(unescapeString(innerOf(raw)));
            } else if (raw.isEmpty()) {
                m_text->clear();
            } else {
                // Unquoted text in a string slot does not compile, and the
                // likeliest reading is that the quotes were left off.
                m_text->setText(raw);
                m_value = quoteString(raw);
            }
        } else {
            m_text->setText(raw);
        }
    }

    m_loading = wasLoading;
}

QString ValueEditor::literalFromWidgets() const
{
    if (m_check)
        return m_check->isChecked() ? QStringLiteral("true") : QStringLiteral("false");
    if (m_int) return QString::number(m_int->value());
    if (m_float) return numberText(m_float->value(), true);
    if (m_vector[0]) {
        double xyz[3];
        for (int i = 0; i < 3; ++i) xyz[i] = m_vector[i]->value();
        return vectorLiteral(xyz);
    }
    if (m_choice) {
        const QString text = m_choice->currentText().trimmed();
        return m_pin.kind == PinKind::Enum ? enumLiteral(text) : text;
    }
    if (m_text && m_text->isEnabled()) {
        const QString text = m_text->text();
        return m_pin.kind == PinKind::String && !m_pin.isArray ? quoteString(text)
                                                               : text.trimmed();
    }
    return m_value;
}

QString ValueEditor::enumLiteral(const QString &member) const
{
    if (member.isEmpty() || member.contains(QLatin1Char('.')) || m_pin.cls.isEmpty())
        return member;
    // A number in an enum slot is the raw int, not a member, and qualifying it
    // would spell a member that does not exist.
    bool number = false;
    member.toInt(&number);
    if (number) return member;
    return m_pin.cls + QLatin1Char('.') + member;
}

void ValueEditor::onEdited()
{
    if (m_loading) return;

    // Picking a real member retires the row that carried the value the enum did
    // not have. Nothing on screen changes, so this raises no further signal.
    if (m_extraRow && m_choice && m_choice->currentIndex() != 0) {
        m_loading = true;
        m_choice->removeItem(0);
        m_extraRow = false;
        m_loading = false;
    }

    const QString next = literalFromWidgets();
    if (next == m_value) return;
    m_value = next;
    emit valueChanged(m_value);
}
