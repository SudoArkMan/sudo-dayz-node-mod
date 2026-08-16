// Inspector dock: the details of whatever is selected.
//
// A node gives what it is, what it does, where it lives in the vanilla source
// and any cautions (engine-only, guarded, protected), plus its per-node
// options: Begin mode, cast target, the call-super toggle on events.
//
// A variable picked in the Variable Manager gives its whole declaration
// instead: default value first, then name, type, the sync and persist flags,
// the access modifiers, the ref decision, and a live preview of the line the
// generator will write. That is the way to set a member's value without wiring
// a Set node for it.
//
// Last selection wins. Picking a node on the canvas takes the panel off the
// variable it was showing, and picking a variable takes it off the node, so the
// two selections never fight over the dock.
#pragma once

#include <QWidget>

#include <functional>

class Document;
class ValueEditor;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTextBrowser;
class QTimer;

struct GraphVariable;

class InspectorPanel : public QWidget {
    Q_OBJECT
public:
    explicit InspectorPanel(Document *doc, QWidget *parent = nullptr);

public slots:
    void refresh();
    // The Variable Manager's current row. An empty id hands the panel back to
    // the canvas selection.
    void showVariable(const QString &variableId);

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onBeginModeChanged(int index);
    void onSuperToggled(bool checked);
    void onClassCommitted();
    void onNodeSelectionChanged();
    void onNameCommitted();
    void onTypeCommitted();
    void onDefaultCommitted();
    void onFlagToggled();
    void onRefModeChanged(int index);

private:
    Document *m_doc;
    QLabel *m_title;
    QLabel *m_kind;
    QTextBrowser *m_body;
    QComboBox *m_beginMode;
    QCheckBox *m_callSuper;
    // Cast To and New Object carry their target class in opts. Until this
    // existed nothing in the app wrote that key, so a node placed by hand
    // stayed on `new Class()` for ever while its own help said to set it here.
    QComboBox *m_classPick;
    bool m_classesLoaded = false;
    // Which node the class box is currently showing. editingFinished fires on
    // focus leaving, and clicking straight from one node to another can deliver
    // that after the selection has already moved, which would write the old
    // node's class onto the new one.
    QString m_classNodeId;
    QString m_nodeId;

    // Variable details. Empty id means the panel is on a node, or on nothing.
    QString m_variableId;
    QWidget *m_varPane;
    QLineEdit *m_varName;
    QLabel *m_varNameError;
    QComboBox *m_varType;
    ValueEditor *m_varValue;
    QLabel *m_varDefaultWarning;
    // The value editor reports every keystroke. This is what turns a burst of
    // them into one undo step; see the note on onDefaultCommitted.
    QTimer *m_defaultCommit;
    QPlainTextEdit *m_varPreview;
    QCheckBox *m_varSync;
    QCheckBox *m_varPersist;
    QCheckBox *m_varStatic;
    QCheckBox *m_varConst;
    QCheckBox *m_varPrivate;
    QCheckBox *m_varProtected;
    QComboBox *m_varRef;
    QLabel *m_varRefNote;
    QLabel *m_varFindings;
    // Set while the form is being filled from the graph, so writing a field
    // back does not read as the user having edited it.
    bool m_varLoading = false;

    void showEmpty();
    void buildVariableForm();
    void fillVariable(const GraphVariable &var);
    void setVariableMode(bool on);
    void updatePreview();
    void bindVariablesPanel();
    // Writes a default that is still inside the debounce, so leaving the
    // variable never loses what was typed into it.
    void flushPendingDefault();
    // The variable as the form currently reads, uncommitted text included, so
    // the preview can show the line before the edit lands on the undo stack.
    bool pendingVariable(GraphVariable *out) const;
    QString currentDefaultText() const;
    // Runs `edit` on the live variable inside one undo step. False when the
    // variable is gone, which is the case after the graph changed under us.
    bool editVariable(const QString &label,
                      const std::function<void(GraphVariable &)> &edit);
};
