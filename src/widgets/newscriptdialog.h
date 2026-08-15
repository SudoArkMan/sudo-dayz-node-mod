// New script: name a class, pick the shape it starts in, write the file.
//
// The explorer's "New file" leaves an empty page, and an empty page in a mod
// folder is still a class header, a base class and a super call away from doing
// anything. The starting content is picked from the script module the file
// lands in, because a script under 4_World and a script under 5_Mission reopen
// completely different classes.
#pragma once

#include <QDialog>
#include <QString>

class Catalog;
class QComboBox;
class QDialogButtonBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

enum class ScriptKind {
    NewClass,     // a class of the mod's own, with a constructor and destructor
    ModdedClass,  // modded class <base>, with one example override
    Empty,        // nothing at all
};

struct NewScriptOptions {
    QString className;   // names the file, and the class when it is a new one
    QString baseClass;   // the class a modded script reopens
    ScriptKind kind = ScriptKind::NewClass;
};

// The Enforce written for these options: tabs for indentation, newline at the
// end, and nothing at all for ScriptKind::Empty. `catalog` decides which method
// a modded class overrides as its example and may be null, in which case the
// well known bases still resolve and the rest get a body with no method in it.
QString scriptSkeleton(const NewScriptOptions &options, const Catalog *catalog = nullptr);

// The script module `path` sits in, spelled the way the mod template spells it:
// "3_Game", "4_World", "5_Mission", or empty outside all three.
QString scriptModuleOf(const QString &path);

// The class a modded script in this module usually reopens, or empty where the
// module has no usual answer.
QString defaultBaseFor(const QString &module);

// Whether `name` can name a class in Enforce. `reason` says why not.
bool isValidScriptName(const QString &name, QString *reason = nullptr);

class NewScriptDialog : public QDialog {
    Q_OBJECT
public:
    NewScriptDialog(QWidget *parent, const QString &folder, const Catalog *catalog);

    // Runs the dialog and writes the file. Returns the path written, or an
    // empty string when the user cancelled.
    static QString run(QWidget *parent, const QString &folder, const Catalog *catalog);

    NewScriptOptions options() const;

private slots:
    void validateInput();

private:
    QString targetPath() const;

    QString m_folder;
    const Catalog *m_catalog;
    QFormLayout *m_form;   // the base row is taken out of it for the other kinds
    QLineEdit *m_name;
    QComboBox *m_kind;
    QLineEdit *m_base;
    QPlainTextEdit *m_preview;
    QLabel *m_note;      // the file that will be written, and the problem if any
    QDialogButtonBox *m_buttons;
};
