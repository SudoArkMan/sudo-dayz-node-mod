// config.cpp, edited as the class tree it is.
//
// A node graph would be the wrong shape for this file. Nodes and wires draw
// execution and data flow, and a config has neither: it is a nested tree of
// classes with properties and arrays hanging off it, so drawn as nodes it would
// only be a worse tree. This is a tree with a property panel, which buys the
// thing the braces were costing anyway. No hand counted nesting, and no silent
// typo in a path.
//
// The raw text sits under both, editable, because somebody shipping a mod has to
// be able to see what is actually being written to the file.
//
// The rules are the reason the window exists. A files[] path that is not on disk
// means that script module never loads and nothing anywhere says so. A CfgPatches
// class left at the template's name means two mods from that template collide the
// first time both are subscribed. Neither shows up until something is broken in
// game, and both are on screen here the moment the file opens.
#pragma once

#include "config/configtree.h"

#include <QDialog>
#include <QString>
#include <QStringList>
#include <QVector>

class CodeEditor;
class Document;
class QDialogButtonBox;
class QLabel;
class QListWidget;
class QScrollArea;
class QSplitter;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;
class QVBoxLayout;
class QWidget;
struct Project;

// What the rules need that is not in the file: where the mod sits on disk and
// what its folder is called. Both are needed to answer "is this path real",
// which is the check worth having.
struct ConfigContext {
    QString modRoot;  // the folder files[] paths are relative to
    QString prefix;   // the child of modRoot holding Scripts, "SudoTest3"

    // False when the pair above does not describe a mod folder, which happens
    // for a config opened from a download folder or a scratch copy. The rules
    // that touch the disk are skipped rather than reporting every path in the
    // file as missing.
    bool onDisk() const;
};

// Taken from the project when the config sits inside its mod folder, and from
// the file's own location otherwise, so a config opened from anywhere still gets
// checked.
ConfigContext configContextFor(const QString &configPath, const Project &project);

struct ConfigFinding {
    enum class Level { Warning, Error };
    // The one fix that can be applied without asking anything further. Renaming
    // the patch class is mechanical: there is one right answer and the folder
    // the file sits in already says what it is.
    enum class Fix { None, RenamePatchClass };

    Level level = Level::Warning;
    // Slash separated, the same paths findClass takes:
    // "CfgMods/SudoTest3/defs/gameScriptModule". Selects the row when the
    // finding is clicked.
    QString path;
    // The property it is about, empty when it is about the class itself.
    QString property;
    // The sentence, already naming the class it is about.
    QString text;
    Fix fix = Fix::None;
    QString fixLabel;
    QString fixValue;
};

// Every rule, over a parsed config. A free function with no widget in it, so the
// rules can be run against a real mod folder from a test.
QVector<ConfigFinding> validateConfig(const ConfigFile &file, const ConfigContext &context);

class ConfigEditor : public QDialog {
    Q_OBJECT
public:
    // Opens `path`, or raises the window already on it. Returns nullptr and
    // fills `error` when the file cannot be read, so the caller can fall back to
    // the text editor: a config this cannot read is the one its author most
    // needs in front of them.
    static ConfigEditor *openFile(QWidget *parent, Document *doc, const QString &path,
                                  QString *error = nullptr);

    ConfigEditor(QWidget *parent, Document *doc, const QString &path);
    ~ConfigEditor() override;

    QString path() const { return m_path; }

    bool load(QString *error = nullptr);
    bool save(QString *error = nullptr);

    // What the rules found, for a test that would rather not read the screen.
    QVector<ConfigFinding> findings() const { return m_findings; }

    // Puts the tree and the property panel on one class, or on one property of
    // it. Used by the findings rows, and by anything that wants to open this
    // window pointing at a particular class. A path that is no longer there
    // lands on the class above it rather than on nothing.
    void selectPath(const QString &path, const QString &property = QString());

public slots:
    void reject() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    // Findings rows are widgets and carry the height they were measured at, so
    // a window that changes width has to measure them again.
    void resizeEvent(QResizeEvent *event) override;
    // Clicking a findings row reveals the class it is about. The row is a
    // widget, so the press never reaches the list on its own.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildTree();
    void buildProperties();
    void buildFindings();
    QTreeWidgetItem *itemForPath(const QString &path, const QString &property) const;

    void onTreeSelectionChanged();
    void onTreeContextMenu(const QPoint &pos);
    void reparseFromText();

    // A property's value changed and nothing moved. The row's summary, the file
    // text and the findings are refreshed; the tree and the property panel are
    // left alone, or the widget being typed into would be rebuilt under the
    // cursor.
    void applyValueEdit(const QString &path, const QString &property);
    // A class or property was added, renamed or deleted, so everything is
    // rebuilt and the selection is put back where it was.
    void applyStructureEdit();
    void refreshText();
    void refreshFindings();
    void setDirty(bool dirty);
    void updateTitle();

    // The class the panel is editing, or nullptr when nothing is selected.
    ConfigClass *selectedClass();
    QString selectedPath() const;

    Document *m_doc;
    QString m_path;
    ConfigFile m_file;
    ConfigContext m_context;
    QVector<ConfigFinding> m_findings;

    QTreeWidget *m_tree;
    QScrollArea *m_propertyArea;
    QLabel *m_propertyTitle;
    QListWidget *m_findingList;
    QLabel *m_findingSummary;
    QToolButton *m_textToggle;
    CodeEditor *m_text;
    QSplitter *m_split;
    QDialogButtonBox *m_buttons;
    // Text pane to model, and model back to text pane. Both are debounced: a
    // reparse on every keystroke would rebuild the tree under the typist, and
    // re-rendering the file on every keystroke would stat every path in it.
    QTimer *m_textSettle;
    QTimer *m_editSettle;

    bool m_dirty = false;
    // Text arriving from us rather than from the author. The text pane and the
    // property panel both write to the model, and without this each would answer
    // the other's edit with one of its own.
    bool m_loading = false;
    // Line endings are kept as they were found. Rewriting a config from CRLF to
    // LF over a one word edit turns the diff into the whole file.
    bool m_crlf = false;
};
