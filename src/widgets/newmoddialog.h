// New mod: name it, pick where it goes, choose whether it ships a mission.
//
// Scaffolding is the whole point, so the dialog shows exactly what will be
// written before it writes anything.
#pragma once

#include "modtemplate.h"

#include <QDialog>

class QLineEdit;
class QCheckBox;
class QLabel;
class QDialogButtonBox;
class QListWidget;

class NewModDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewModDialog(QWidget *parent = nullptr);

    // Runs the dialog and the scaffold. Returns the result; check `ok`.
    static ModTemplateResult run(QWidget *parent);

    ModTemplateOptions options() const;
    QString parentDirectory() const;

private slots:
    void browse();
    void validateInput();

private:
    QLineEdit *m_prefix;
    QLineEdit *m_displayName;
    QLineEdit *m_author;
    QLineEdit *m_location;
    QCheckBox *m_includeMissions;
    QListWidget *m_maps;
    QLabel *m_preview;   // the folder that will be created, and the problem if any
    QDialogButtonBox *m_buttons;
};
