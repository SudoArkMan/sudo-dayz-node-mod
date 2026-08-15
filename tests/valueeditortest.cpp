// The typed value editor: which widget a type gets, and what literal comes back
// out of it.
//
// Every kind is driven twice, once from setValue and once from the widget, since
// those are the two directions a details panel uses and only one of them can be
// checked by reading the screen.
//
// The offscreen platform is forced here rather than left to the caller's
// environment: a test that only passes with a display is a test nobody runs.
//   ./tests/valueeditortest ../resources
#include "catalog.h"
#include "theme.h"
#include "widgets/valueeditor.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QStringList>
#include <QTextStream>

#include <limits>

static int failures = 0;

static void check(bool ok, const QString &what)
{
    QTextStream out(stdout);
    out << (ok ? "  ok   " : "  FAIL ") << what << Qt::endl;
    if (!ok) failures++;
}

static void equal(const QString &got, const QString &want, const QString &what)
{
    const bool ok = got == want;
    QTextStream out(stdout);
    out << (ok ? "  ok   " : "  FAIL ") << what;
    if (!ok) out << " (wanted [" << want << "], got [" << got << "])";
    out << Qt::endl;
    if (!ok) failures++;
}

// The literal goes in and the same literal comes back: what a panel needs before
// it can show a value without rewriting the file it came from.
static void roundTrip(ValueEditor &editor, const QString &literal, const QString &what)
{
    editor.setValue(literal);
    equal(editor.value(), literal, what);
}

int main(int argc, char *argv[])
{
    // Offscreen unless the caller has already chosen, so --shot can be taken on
    // a platform that has fonts.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTextStream out(stdout);

    const QString root = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("resources");

    Catalog cat;
    const bool loaded = cat.load(root + "/catalog.json");
    check(loaded, QStringLiteral("loads catalog.json (%1)").arg(cat.error()));
    if (!loaded) return 1;

    QString why;

    out << "bool" << Qt::endl;
    {
        ValueEditor editor(&cat);
        editor.setType(QStringLiteral("bool"));
        auto *box = editor.findChild<QCheckBox *>(QStringLiteral("boolValue"));
        check(box != nullptr, QStringLiteral("a bool gets a check box"));
        if (!box) return 1;

        roundTrip(editor, QStringLiteral("true"), QStringLiteral("true survives"));
        check(box->isChecked(), QStringLiteral("true ticks the box"));
        roundTrip(editor, QStringLiteral("false"), QStringLiteral("false survives"));
        check(!box->isChecked(), QStringLiteral("false clears the box"));

        editor.setValue(QStringLiteral("TRUE"));
        equal(editor.value(), QStringLiteral("true"),
              QStringLiteral("true is spelled the one way"));

        editor.setValue(QStringLiteral("banana"));
        equal(editor.value(), QStringLiteral("banana"),
              QStringLiteral("a value the box cannot hold is kept as it was"));
        check(!editor.isValid(&why), QStringLiteral("banana is reported"));
        check(!why.isEmpty(), QStringLiteral("and the report says something"));

        int seen = 0;
        QString last;
        QObject::connect(&editor, &ValueEditor::valueChanged,
                         [&seen, &last](const QString &v) { seen++; last = v; });

        editor.setValue(QStringLiteral("false"));
        check(seen == 0, QStringLiteral("setValue does not emit"));
        box->setChecked(true);
        check(seen == 1, QStringLiteral("ticking the box emits once"));
        equal(last, QStringLiteral("true"), QStringLiteral("and emits the literal"));
        box->setChecked(true);
        check(seen == 1, QStringLiteral("the same state again emits nothing"));
    }

    out << "int" << Qt::endl;
    {
        ValueEditor editor(&cat);
        editor.setType(QStringLiteral("int"));
        auto *spin = editor.findChild<QSpinBox *>(QStringLiteral("intValue"));
        check(spin != nullptr, QStringLiteral("an int gets a spin box"));
        if (!spin) return 1;
        check(spin->minimum() == std::numeric_limits<int>::min()
                  && spin->maximum() == std::numeric_limits<int>::max(),
              QStringLiteral("the box covers the whole int range"));

        roundTrip(editor, QStringLiteral("0"), QStringLiteral("0 survives"));
        roundTrip(editor, QStringLiteral("42"), QStringLiteral("42 survives"));
        roundTrip(editor, QStringLiteral("-7"), QStringLiteral("a negative survives"));
        roundTrip(editor, QStringLiteral("2147483647"),
                  QStringLiteral("the top of the range survives"));
        roundTrip(editor, QStringLiteral("-2147483648"),
                  QStringLiteral("the bottom of the range survives"));
        check(spin->value() == std::numeric_limits<int>::min(),
              QStringLiteral("and reaches the box"));

        editor.setValue(QStringLiteral("0x20"));
        equal(editor.value(), QStringLiteral("0x20"),
              QStringLiteral("a hex literal is left spelled as it was written"));
        check(spin->value() == 32, QStringLiteral("and reads as 32"));
        check(editor.isValid(), QStringLiteral("hex is a whole number"));

        editor.setValue(QStringLiteral("3.5"));
        equal(editor.value(), QStringLiteral("3.5"),
              QStringLiteral("a fraction in an int slot is kept"));
        check(!editor.isValid(&why), QStringLiteral("and reported"));

        spin->setValue(9);
        equal(editor.value(), QStringLiteral("9"),
              QStringLiteral("the box writes a plain number"));
    }

    out << "float" << Qt::endl;
    {
        ValueEditor editor(&cat);
        editor.setType(QStringLiteral("float"));
        auto *spin = editor.findChild<QDoubleSpinBox *>(QStringLiteral("floatValue"));
        check(spin != nullptr, QStringLiteral("a float gets a double spin box"));
        if (!spin) return 1;

        roundTrip(editor, QStringLiteral("0.0"), QStringLiteral("0.0 survives"));
        roundTrip(editor, QStringLiteral("1.5"), QStringLiteral("1.5 survives"));
        roundTrip(editor, QStringLiteral("-2.25"), QStringLiteral("a negative survives"));
        roundTrip(editor, QStringLiteral("0.001"),
                  QStringLiteral("small decimals are not rounded away"));
        roundTrip(editor, QStringLiteral("12345.6789"),
                  QStringLiteral("nine significant digits survive"));

        editor.setValue(QStringLiteral("1"));
        equal(editor.value(), QStringLiteral("1.0"),
              QStringLiteral("an int literal in a float slot gains its point"));
        editor.setValue(QStringLiteral("2.50"));
        equal(editor.value(), QStringLiteral("2.5"),
              QStringLiteral("a trailing zero goes"));

        spin->setValue(3.0);
        equal(editor.value(), QStringLiteral("3.0"),
              QStringLiteral("the box writes a float literal, not an int one"));

        editor.setValue(QStringLiteral("banana"));
        equal(editor.value(), QStringLiteral("banana"),
              QStringLiteral("text in a float slot is kept"));
        check(!editor.isValid(&why), QStringLiteral("and reported"));
        editor.setValue(QStringLiteral("nan"));
        equal(editor.value(), QStringLiteral("nan"),
              QStringLiteral("nan is kept as it was written"));
        check(!editor.isValid(&why), QStringLiteral("and reported"));
    }

    out << "vector" << Qt::endl;
    {
        ValueEditor editor(&cat);
        editor.setType(QStringLiteral("vector"));
        const auto boxes = editor.findChildren<QDoubleSpinBox *>();
        check(boxes.size() == 3, QStringLiteral("a vector gets three boxes"));
        auto *y = editor.findChild<QDoubleSpinBox *>(QStringLiteral("vectorY"));
        check(y != nullptr, QStringLiteral("and they are named x, y and z"));
        if (!y) return 1;

        roundTrip(editor, QStringLiteral("\"0 0 0\""),
                  QStringLiteral("the zero vector survives"));
        roundTrip(editor, QStringLiteral("\"1 2 3\""),
                  QStringLiteral("whole numbers stay whole"));
        roundTrip(editor, QStringLiteral("\"1.5 0 -2\""),
                  QStringLiteral("a mixed vector survives"));

        editor.setValue(QStringLiteral("1 2 3"));
        equal(editor.value(), QStringLiteral("\"1 2 3\""),
              QStringLiteral("an unquoted vector gains its quotes"));

        editor.setValue(QStringLiteral("\"1 2\""));
        equal(editor.value(), QStringLiteral("\"1 2\""),
              QStringLiteral("two numbers are not a vector and are kept"));
        check(!editor.isValid(&why), QStringLiteral("and reported"));

        editor.setValue(QStringLiteral("\"1 2 3\""));
        y->setValue(9.0);
        equal(editor.value(), QStringLiteral("\"1 9 3\""),
              QStringLiteral("a box writes back into the literal"));
    }

    out << "string" << Qt::endl;
    {
        ValueEditor editor(&cat);
        editor.setType(QStringLiteral("string"));
        auto *line = editor.findChild<QLineEdit *>(QStringLiteral("textValue"));
        check(line != nullptr, QStringLiteral("a string gets a line edit"));
        if (!line) return 1;

        roundTrip(editor, QStringLiteral("\"hello\""),
                  QStringLiteral("a quoted string survives"));
        equal(line->text(), QStringLiteral("hello"),
              QStringLiteral("the box holds the text without its quotes"));
        roundTrip(editor, QStringLiteral("\"\""),
                  QStringLiteral("the empty string survives"));

        line->setText(QStringLiteral("plain"));
        equal(editor.value(), QStringLiteral("\"plain\""),
              QStringLiteral("typing gets the quotes put on"));

        line->setText(QStringLiteral("he said \"hi\""));
        equal(editor.value(), QStringLiteral("\"he said \\\"hi\\\"\""),
              QStringLiteral("an embedded quote is escaped"));

        // "a\\b" in the file is a\b once the parser has read it.
        editor.setValue(QStringLiteral("\"a\\\\b\""));
        equal(line->text(), QStringLiteral("a\\b"),
              QStringLiteral("an escaped backslash reads as one backslash"));
        equal(editor.value(), QStringLiteral("\"a\\\\b\""),
              QStringLiteral("and is written back the same"));

        line->setText(QStringLiteral("C:\\DayZ"));
        equal(editor.value(), QStringLiteral("\"C:\\\\DayZ\""),
              QStringLiteral("a typed backslash is doubled"));

        editor.setValue(QStringLiteral("hello"));
        equal(editor.value(), QStringLiteral("\"hello\""),
              QStringLiteral("text arriving without quotes gets them"));
        check(editor.isValid(), QStringLiteral("a string slot reports nothing"));
    }

    out << "enum" << Qt::endl;
    {
        // Whatever the catalogue offers first: the test is about the widget, not
        // about one enum, and hard-coding a name ties it to a DayZ version.
        QString name;
        QStringList members;
        for (int i = 0; i < 20000; ++i) {
            const QString candidate = cat.enumName(QStringLiteral("en%1").arg(i));
            if (candidate.isEmpty()) break;
            const QStringList values = cat.enumValues(candidate);
            if (values.size() < 2 || values.first().isEmpty()) continue;
            name = candidate;
            members = values;
            break;
        }
        check(!name.isEmpty(), QStringLiteral("the catalogue carries an enum to test"));
        if (name.isEmpty()) return 1;
        out << "       using " << name << " (" << members.size() << " members)" << Qt::endl;

        ValueEditor editor(&cat);
        editor.setType(name);
        check(editor.pinType().kind == PinKind::Enum,
              QStringLiteral("an enum name reads as an enum"));
        auto *combo = editor.findChild<QComboBox *>(QStringLiteral("choiceValue"));
        check(combo != nullptr, QStringLiteral("an enum gets a combo"));
        if (!combo) return 1;
        bool allThere = true;
        for (const QString &member : members)
            allThere = allThere && combo->findText(member) >= 0;
        check(allThere, QStringLiteral("every member is on the list"));
        check(!combo->isEditable(), QStringLiteral("a known enum is a closed list"));
        // A variable with no default is on no member, and a combo sitting on the
        // first one would say otherwise.
        check(combo->count() == members.size() + 1 && combo->currentText().isEmpty(),
              QStringLiteral("with a blank row for a variable that has no default"));

        const QString qualified = name + QLatin1Char('.') + members.first();
        roundTrip(editor, qualified, QStringLiteral("a qualified member survives"));

        editor.setValue(members.at(1));
        equal(editor.value(), name + QLatin1Char('.') + members.at(1),
              QStringLiteral("a bare member is read through its enum"));
        equal(combo->currentText(), members.at(1),
              QStringLiteral("and the combo lands on it"));

        const QString absent = name + QStringLiteral(".NotAMember");
        editor.setValue(absent);
        equal(editor.value(), absent,
              QStringLiteral("a member the enum has not got is kept"));
        check(!editor.isValid(&why) && why.contains(name),
              QStringLiteral("and reported against the enum"));
        equal(combo->currentText(), absent,
              QStringLiteral("and still shows on screen"));

        editor.setValue(QStringLiteral("0"));
        equal(editor.value(), QStringLiteral("0"),
              QStringLiteral("a number in an enum slot is not qualified"));
        check(editor.isValid(), QStringLiteral("an enum is an int, so 0 compiles"));

        combo->setCurrentIndex(combo->findText(members.first()));
        equal(editor.value(), qualified,
              QStringLiteral("picking a member writes the qualified literal"));
        check(combo->count() == members.size(),
              QStringLiteral("and the row that carried 0 is gone"));
    }

    out << "enum and class the catalogue has never heard of" << Qt::endl;
    {
        // No catalogue: a mod's own enum and a mod's own class look exactly like
        // this, and neither may be locked out of its own default.
        ValueEditor editor(nullptr);
        editor.setType(QStringLiteral("EMyModState"));
        auto *line = editor.findChild<QLineEdit *>(QStringLiteral("textValue"));
        check(line != nullptr && line->isEnabled(),
              QStringLiteral("an unknown type stays editable"));
        if (!line) return 1;
        roundTrip(editor, QStringLiteral("EMyModState.IDLE"),
                  QStringLiteral("a mod enum member survives"));
        check(editor.isValid(),
              QStringLiteral("nothing is reported with no catalogue to report against"));
        check(!editor.note().isEmpty(),
              QStringLiteral("a note says the value is taken as typed"));

        auto *label = editor.findChild<QLabel *>(QStringLiteral("valueNote"));
        check(label != nullptr && label->text() == editor.note(),
              QStringLiteral("and the note is on screen"));

        line->setText(QStringLiteral("EMyModState.BUSY"));
        equal(editor.value(), QStringLiteral("EMyModState.BUSY"),
              QStringLiteral("typing goes through unchanged"));
    }

    out << "typename" << Qt::endl;
    {
        ValueEditor editor(&cat);
        editor.setType(QStringLiteral("typename"));
        auto *combo = editor.findChild<QComboBox *>(QStringLiteral("choiceValue"));
        check(combo != nullptr, QStringLiteral("a typename gets a combo"));
        if (!combo) return 1;
        check(combo->isEditable(), QStringLiteral("and it takes typing"));
        check(combo->count() > 5000, QStringLiteral("with the classes on the list"));

        roundTrip(editor, QStringLiteral("PlayerBase"),
                  QStringLiteral("a class name survives"));
        check(editor.isValid(), QStringLiteral("PlayerBase is a class"));
        roundTrip(editor, QStringLiteral("string"),
                  QStringLiteral("a primitive is a typename too"));
        check(editor.isValid(), QStringLiteral("and is not reported"));

        editor.setValue(QStringLiteral("PlayerBse"));
        equal(editor.value(), QStringLiteral("PlayerBse"),
              QStringLiteral("a misspelled class is kept"));
        check(!editor.isValid(&why), QStringLiteral("and reported"));
        check(why.contains(QStringLiteral("PlayerBse")),
              QStringLiteral("naming what it could not find"));
    }

    out << "object" << Qt::endl;
    {
        ValueEditor editor(&cat);
        editor.setType(QStringLiteral("PlayerBase"));
        check(editor.pinType().kind == PinKind::Object,
              QStringLiteral("a class name reads as an object"));
        auto *line = editor.findChild<QLineEdit *>(QStringLiteral("textValue"));
        check(line != nullptr, QStringLiteral("an object gets a field"));
        if (!line) return 1;
        check(!line->isEnabled(), QStringLiteral("that cannot be typed into"));
        equal(line->text(), QStringLiteral("null"), QStringLiteral("showing null"));
        check(!editor.note().isEmpty(), QStringLiteral("with a note saying why"));
        equal(editor.value(), QString(),
              QStringLiteral("and no default at all, which is not the same as null"));
        roundTrip(editor, QStringLiteral("null"),
                  QStringLiteral("an explicit null survives"));
        check(editor.isValid(), QStringLiteral("an object slot reports nothing"));
    }

    out << "array and map" << Qt::endl;
    {
        ValueEditor editor(&cat);
        editor.setType(QStringLiteral("array<string>"));
        check(editor.pinType().isArray, QStringLiteral("array<string> reads as an array"));
        auto *line = editor.findChild<QLineEdit *>(QStringLiteral("textValue"));
        check(line != nullptr && !line->isEnabled(),
              QStringLiteral("an array is not typed into"));
        check(!editor.note().isEmpty(), QStringLiteral("with a note saying why"));
        roundTrip(editor, QStringLiteral("null"), QStringLiteral("null survives"));
        check(editor.isValid(), QStringLiteral("an array slot reports nothing"));

        editor.setType(QStringLiteral("map<string, int>"));
        check(editor.pinType().isArray, QStringLiteral("a map reads as an array too"));
        editor.setType(QStringLiteral("TStringArray"));
        check(editor.pinType().isArray, QStringLiteral("and so does TStringArray"));
    }

    out << "retyping" << Qt::endl;
    {
        ValueEditor editor(&cat);
        editor.setType(QStringLiteral("int"));
        editor.setValue(QStringLiteral("5"));
        editor.setType(QStringLiteral("float"));
        equal(editor.value(), QStringLiteral("5.0"),
              QStringLiteral("an int default retyped as a float gains its point"));
        editor.setType(QStringLiteral("string"));
        equal(editor.value(), QStringLiteral("\"5.0\""),
              QStringLiteral("and as a string it gains quotes"));
        editor.setType(QStringLiteral("bool"));
        equal(editor.value(), QStringLiteral("\"5.0\""),
              QStringLiteral("a default a bool cannot hold is not thrown away"));
        check(!editor.isValid(&why), QStringLiteral("and is reported instead"));
    }

    out << "junk" << Qt::endl;
    {
        ValueEditor editor(&cat);
        const QStringList types = {
            QStringLiteral("array<"),      QStringLiteral("<<<"),
            QStringLiteral("map<int,>"),   QStringLiteral("ref"),
            QStringLiteral("array<array<int>>"), QStringLiteral("\""),
            QStringLiteral("9lives"),      QStringLiteral("bool"),
        };
        const QStringList values = {
            QStringLiteral("\"unterminated"), QStringLiteral(""),
            QStringLiteral("   "),            QStringLiteral("1e999"),
            QStringLiteral("null"),           QString(400, QLatin1Char('x')),
            QStringLiteral("\\"),             QStringLiteral("{1, 2, 3}"),
        };
        for (const QString &type : types) {
            editor.setType(type);
            for (const QString &value : values) {
                editor.setValue(value);
                QString ignored;
                editor.isValid(&ignored);
                // Nothing is invented: a value it cannot read comes back as it
                // went in, trimmed and no more.
                const QString trimmed = value.trimmed();
                if (editor.value() != trimmed && editor.pinType().kind == PinKind::Any)
                    check(false, QStringLiteral("an Any slot passed [%1] through as [%2]")
                                     .arg(value, editor.value()));
            }
        }
        check(true, QStringLiteral("odd types and odd values do not take it down"));
    }

    // A shape nobody has looked at is a shape nobody has checked. `--shot <png>`
    // lays one editor of every kind out in the app's own theme and writes the
    // picture, so the widths and the notes can be judged rather than assumed.
    const int shotAt = QCoreApplication::arguments().indexOf(QStringLiteral("--shot"));
    if (shotAt > 0 && shotAt + 1 < QCoreApplication::arguments().size()) {
        const QString path = QCoreApplication::arguments().at(shotAt + 1);
        theme::apply(app);

        QWidget sheet;
        sheet.setAutoFillBackground(true);
        auto *form = new QFormLayout(&sheet);
        form->setContentsMargins(10, 10, 10, 10);
        form->setSpacing(6);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

        struct Row { const char *type; const char *value; };
        const Row rows[] = {
            {"bool", "true"},
            {"int", "12"},
            {"float", "1.5"},
            {"string", "\"Hello there\""},
            {"vector", "\"0 1.5 -2\""},
            {"typename", "PlayerBase"},
            {"PlayerBase", ""},
            {"array<string>", ""},
            {"EMyModEnum", "EMyModEnum.IDLE"},
        };
        for (const Row &row : rows) {
            auto *editor = new ValueEditor(&cat, &sheet);
            editor->setType(QString::fromLatin1(row.type));
            editor->setValue(QString::fromLatin1(row.value));
            form->addRow(QString::fromLatin1(row.type), editor);
        }
        // One real enum on the end, so the closed list is in the picture too.
        for (int i = 0; i < 20000; ++i) {
            const QString candidate = cat.enumName(QStringLiteral("en%1").arg(i));
            if (candidate.isEmpty()) break;
            const QStringList values = cat.enumValues(candidate);
            if (values.size() < 3) continue;
            auto *editor = new ValueEditor(&cat, &sheet);
            editor->setType(candidate);
            editor->setValue(candidate + QLatin1Char('.') + values.at(1));
            form->addRow(candidate, editor);
            break;
        }

        sheet.resize(360, sheet.sizeHint().height());
        const bool saved = sheet.grab().save(path);
        out << (saved ? "wrote " : "could not write ") << path << Qt::endl;
    }

    out << Qt::endl
        << (failures == 0 ? QStringLiteral("ALL VALUE EDITOR TESTS PASSED")
                          : QStringLiteral("%1 FAILURES").arg(failures))
        << Qt::endl;
    return failures == 0 ? 0 : 1;
}
