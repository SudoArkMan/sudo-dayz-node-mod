// Variable Manager dock: the script's member variables (name, type, default),
// plus the DayZ-specific sync/persist flags. Editable in place; the type
// colour chip matches the pin colour the variable's get/set nodes will use.
//
// Type is a dropdown over the primitives, the common containers and the whole
// catalogue, but still free text: a class from another mod is a legitimate
// member type and the catalogue has never heard of it.
//
// Rows are drag sources. Dropping one on the canvas places its get or set
// node, which is how a member gets used without anyone typing its name.
//
// The current row is announced so the Inspector can show the whole declaration
// for it: every property the model carries, not just the five columns that fit
// across a dock.
#pragma once

#include <QWidget>

class Document;
class QComboBox;
class QTableWidget;
class QTableWidgetItem;

// The Type control, so anything that edits a member type gets the same list
// rather than a second copy of it: primitives, containers, then every class and
// enum in the catalogue, each carrying the pin colour its get/set nodes use.
// Editable on purpose, since a modder's own classes are nowhere in the
// catalogue. The model behind it is built once for the whole application.
void configureVariableTypeCombo(QComboBox *combo, Document *doc);

class VariablesPanel : public QWidget {
    Q_OBJECT
public:
    explicit VariablesPanel(Document *doc, QWidget *parent = nullptr);

    // Puts the caret in a variable's name cell. A variable the user just
    // conjured from a pin has a derived name that almost always wants
    // changing, so promoting drops them straight into renaming it.
    void beginRename(const QString &variableId);

    // Makes a variable the current row, which is also what puts the Inspector
    // on it. Silently does nothing when the id is not in this graph.
    void selectVariable(const QString &variableId);

public slots:
    void refresh();

signals:
    // Drag/double-click a variable to place a get/set node.
    void variableNodeRequested(const QString &variableId, bool setter);

    // The current row, or an empty string when the table has no selection.
    // The Inspector listens and shows the variable's properties.
    void variableSelected(const QString &variableId);

private slots:
    void onCreateVariable();
    void onCellChanged(int row, int column);
    void onContextMenu(const QPoint &pos);
    void onSelectionChanged();

private:
    Document *m_doc;
    QTableWidget *m_table;
    bool m_loading = false;
    // What was last announced through variableSelected. Also what the selection
    // is put back to after a rebuild, since refilling the table drops it.
    QString m_selectedId;

    int rowOfVariable(const QString &variableId) const;
    QString currentVariableId() const;
    void setSelectedVariable(const QString &variableId);
};
