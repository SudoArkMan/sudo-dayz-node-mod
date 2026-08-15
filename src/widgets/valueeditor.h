// One value, edited through the right widget for its Enforce type.
//
// The details panel and the variable table have the same job: take a type and a
// literal as it appears in the .c, put something usable in front of it, and hand
// back a literal the generator can paste after an `=`. That decision lives here
// once, so the two cannot disagree about what a float or a vector looks like.
//
// The widget is picked through pinTypeOf, the same type reading the pins use, so
// a slot always gets the editor its wire colour promises.
//
// Two rules run through the whole class:
//
//   A literal it cannot represent is kept exactly as it arrived and reported
//   through isValid. A member declared in a file we did not write is not garbage
//   to be normalised away, and nothing here rewrites a value behind the author.
//
//   setType and setValue never emit. Only a change made in the widget does, so a
//   panel can show a variable without writing to it.
#pragma once

#include "pins.h"

#include <QString>
#include <QWidget>

class Catalog;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QSpinBox;

class ValueEditor : public QWidget {
    Q_OBJECT
public:
    // The catalogue resolves enum members and class names. It may be null, in
    // which case every name is taken on trust and nothing is reported.
    explicit ValueEditor(const Catalog *catalog, QWidget *parent = nullptr);

    // Rebuilds the widget for this type ("bool", "vector", "PlayerBase",
    // "array<string>"). The current value is carried across and re-read through
    // the new type, so retyping a variable keeps a default that still fits.
    void setType(const QString &enforceType);
    QString type() const { return m_type; }
    PinType pinType() const { return m_pin; }

    // The literal as it appears in the .c, quotes included for strings and
    // vectors. Empty is a legal state: it means the declaration has no `=` at
    // all, which is not the same as a zero.
    void setValue(const QString &literal);
    QString value() const { return m_value; }

    // False when the literal would not compile in a slot of this type. `reason`
    // is a sentence for the panel to show. Typing is never blocked by it.
    bool isValid(QString *reason = nullptr) const;

    // Why this type has no editable literal, or empty when it has one. Shown
    // under the field, and readable by a panel that lays out its own hints.
    QString note() const { return m_note; }

signals:
    void valueChanged(const QString &literal);

private:
    void rebuild();
    void buildText(bool editable);
    void applyValue(const QString &literal);
    void onEdited();
    QString literalFromWidgets() const;
    QString enumLiteral(const QString &member) const;

    const Catalog *m_catalog = nullptr;
    QString m_type;
    PinType m_pin;
    QString m_value;
    QString m_note;
    bool m_built = false;
    bool m_loading = false;  // widget writes coming from us, not from the author
    bool m_extraRow = false; // the enum combo carries a value the enum does not

    QHBoxLayout *m_row = nullptr;
    QLabel *m_noteLabel = nullptr;
    QCheckBox *m_check = nullptr;
    QSpinBox *m_int = nullptr;
    QDoubleSpinBox *m_float = nullptr;
    QDoubleSpinBox *m_vector[3] = {nullptr, nullptr, nullptr};
    QLineEdit *m_text = nullptr;
    QComboBox *m_choice = nullptr;
};
