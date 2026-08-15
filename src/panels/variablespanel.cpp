#include "variablespanel.h"

#include "canvas/nodeview.h"
#include "document.h"
#include "theme.h"

#include <QAction>
#include <QComboBox>
#include <QCompleter>
#include <QDrag>
#include <QHeaderView>
#include <QIcon>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Column { ColName = 0, ColType, ColDefault, ColSync, ColPersist, ColCount };

PinType pinTypeFor(const Document *doc, const QString &type)
{
    return pinTypeOf(type, [doc](const QString &name) {
        return doc && doc->catalog().isEnum(name);
    });
}

QPixmap chipPixmap(PinKind kind)
{
    QPixmap chip(12, 12);
    chip.fill(Qt::transparent);
    QPainter painter(&chip);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(theme::border(), 1));
    painter.setBrush(pinColor(kind));
    painter.drawRoundedRect(QRectF(1.5, 1.5, 9.0, 9.0), 2.0, 2.0);
    return chip;
}

// The chip is the same colour the variable's get/set pins will be, so a glance
// down the column tells you what will plug into what.
QPixmap typeChip(const Document *doc, const QString &type)
{
    return chipPixmap(pinTypeFor(doc, type).kind);
}

// What the Type dropdown offers before the catalogue: the primitives, then the
// containers spelled the way Enforce wants them. The container entries are
// starting points rather than finished choices, since the element type is what
// the modder came to change.
QStringList commonTypes()
{
    return {QStringLiteral("bool"),
            QStringLiteral("int"),
            QStringLiteral("float"),
            QStringLiteral("string"),
            QStringLiteral("vector"),
            QStringLiteral("typename"),
            QStringLiteral("array<string>"),
            QStringLiteral("array<int>"),
            QStringLiteral("array<float>"),
            QStringLiteral("array<ref EntityAI>"),
            QStringLiteral("set<string>"),
            QStringLiteral("map<string, string>")};
}

// The catalogue hands enums out one key at a time and keeps no list of them. A
// key past the last enum names nothing, which is where the walk stops.
QStringList enumTypes(const Catalog &catalog)
{
    QStringList out;
    for (int i = 0;; ++i) {
        const QString name = catalog.enumName(QStringLiteral("en%1").arg(i));
        if (name.isEmpty()) break;
        out.append(name);
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

// Every type the dropdown knows, chip included, in the order a member is most
// often declared: primitives, containers, then the whole catalogue.
QStandardItemModel *buildTypeModel(Document *doc, QObject *parent)
{
    auto *model = new QStandardItemModel(parent);

    // Ten pin kinds cover six thousand entries, so a chip is painted once per
    // kind and the rows share it.
    QHash<int, QIcon> chips;
    QSet<QString> seen;
    const auto add = [&](const QString &type) {
        if (type.isEmpty() || seen.contains(type)) return;
        seen.insert(type);
        const PinKind kind = pinTypeFor(doc, type).kind;
        QIcon &chip = chips[static_cast<int>(kind)];
        if (chip.isNull()) chip = QIcon(chipPixmap(kind));
        auto *item = new QStandardItem(chip, type);
        item->setEditable(false);
        model->appendRow(item);
    };

    for (const QString &type : commonTypes()) add(type);
    if (doc) {
        for (const QString &name : doc->catalog().classNames()) add(name);
        for (const QString &name : enumTypes(doc->catalog())) add(name);
    }
    return model;
}

// The Type column as a dropdown that still takes free text.
//
// 6,108 catalogue classes is well past what anyone spells from memory, and a
// type one letter out declares a member the generator cannot type. The list is
// not a whitelist though: a modder's own classes, and classes from other mods,
// are nowhere in the catalogue, so whatever is typed is kept.
//
// The control itself is built by configureVariableTypeCombo, which the
// Inspector's type box also calls, so the two can never offer different lists.
class TypeDelegate : public QStyledItemDelegate {
public:
    TypeDelegate(Document *doc, QObject *parent)
        : QStyledItemDelegate(parent), m_doc(doc)
    {
    }

protected:
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override
    {
        Q_UNUSED(option);
        Q_UNUSED(index);
        auto *combo = new QComboBox(parent);
        configureVariableTypeCombo(combo, m_doc);

        // Picking a row off the list is a finished answer, so it lands in the
        // cell there and then. Typed text is not: a half spelled class name
        // commits the ordinary way, on Enter or on leaving the cell.
        auto *self = const_cast<TypeDelegate *>(this);
        connect(combo, &QComboBox::activated, combo, [self, combo]() {
            emit self->commitData(combo);
            emit self->closeEditor(combo);
        });
        return combo;
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo) {
            QStyledItemDelegate::setEditorData(editor, index);
            return;
        }
        const QString current = index.data(Qt::EditRole).toString();
        // Move to the row first so the popup opens on the current type, then put
        // the text back: a type the list does not carry still has to survive
        // opening the editor.
        const int row = combo->findText(current);
        if (row >= 0) combo->setCurrentIndex(row);
        combo->setEditText(current);
        if (combo->lineEdit()) combo->lineEdit()->selectAll();
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override
    {
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo) {
            QStyledItemDelegate::setModelData(editor, model, index);
            return;
        }
        model->setData(index, combo->currentText().trimmed(), Qt::EditRole);
    }

    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override
    {
        Q_UNUSED(index);
        // The column is sized to whatever "bool" needs and the popup is never
        // narrower than the box it drops from, so the editor is widened to
        // something a class name fits in, stopping at the edge of the dock.
        QRect rect = option.rect;
        const QWidget *host = editor->parentWidget();
        const int room = host ? qMax(40, host->width() - rect.x()) : rect.width();
        rect.setWidth(qMin(qMax(rect.width(), 220), room));
        editor->setGeometry(rect);
    }

private:
    Document *m_doc;
};

QString uniqueName(const Graph &graph, const QString &base)
{
    const auto taken = [&graph](const QString &candidate) {
        for (const GraphVariable &v : graph.variables)
            if (v.name == candidate) return true;
        return false;
    };
    QString name = base;
    int suffix = 2;
    while (taken(name)) name = base + QString::number(suffix++);
    return name;
}

int indexOfVariable(const Graph &graph, const QString &id)
{
    for (int i = 0; i < graph.variables.size(); ++i)
        if (graph.variables.at(i).id == id) return i;
    return -1;
}

// Dragging a row onto the canvas is how a member becomes a node. The payload is
// the variable id and nothing else: whether it lands as a get or a set is the
// canvas's decision, taken from the modifiers held at the drop. startDrag is
// overridden rather than mimeData so the drop side never has to unpack Qt's own
// item-model mime format.
class VariableTable : public QTableWidget {
public:
    explicit VariableTable(QWidget *parent = nullptr) : QTableWidget(parent) {}

protected:
    void startDrag(Qt::DropActions supportedActions) override
    {
        Q_UNUSED(supportedActions);
        // The id lives on the name cell, so a drag started from the type or
        // default column still knows which variable it is carrying.
        const QTableWidgetItem *nameItem = item(currentRow(), ColName);
        if (!nameItem) return;
        const QString id = nameItem->data(Qt::UserRole).toString();
        if (id.isEmpty()) return;

        auto *mime = new QMimeData;
        mime->setData(variableDragMimeType(), id.toUtf8());
        mime->setText(nameItem->text());

        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->exec(Qt::CopyAction);
    }
};

} // namespace

void configureVariableTypeCombo(QComboBox *combo, Document *doc)
{
    if (!combo) return;

    // Six thousand rows are worth assembling once. The model is parented to the
    // document so it lives exactly as long as the catalogue behind it, and
    // every type control in the application points at this one instance rather
    // than at a copy that could fall out of step with it.
    static QPointer<QStandardItemModel> shared;
    static QPointer<Document> owner;
    if (!shared || owner != doc) {
        shared = buildTypeModel(doc, doc);
        owner = doc;
    }

    combo->setEditable(true);
    // NoInsert: a type the catalogue never heard of belongs in the field, not
    // appended to the list every other control reads from.
    combo->setInsertPolicy(QComboBox::NoInsert);
    combo->setModel(shared);
    combo->setIconSize(QSize(12, 12));
    combo->setMaxVisibleItems(14);
    // Without this the longest of six thousand class names decides how wide the
    // control has to be, and a dock is not that wide. The cell editor is given
    // its geometry by the delegate, and the Inspector's box stretches to the
    // panel, so neither needs the width the list would ask for.
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(12);

    // Per control, because a QCompleter serves one widget at a time.
    auto *completer = new QCompleter(shared, combo);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    // Contains rather than prefix: a class name is usually half remembered from
    // the middle, so "Item" has to reach ItemBase and InventoryItem alike.
    completer->setFilterMode(Qt::MatchContains);
    completer->setMaxVisibleItems(14);
    combo->setCompleter(completer);
}

VariablesPanel::VariablesPanel(Document *doc, QWidget *parent)
    : QWidget(parent), m_doc(doc), m_table(new VariableTable(this))
{
    // A Q_OBJECT subclass is not painted by the sheet's `QDockWidget > QWidget`
    // rule, so without this the dock body comes out at the window colour
    // instead of the panel colour.
    setAttribute(Qt::WA_StyledBackground, true);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    auto *create = new QPushButton(tr("Create variable"), this);
    create->setToolTip(tr("Declare a new member on this class"));
    layout->addWidget(create);

    m_table->setColumnCount(ColCount);
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Type"), tr("Default"),
                                        tr("Sync"), tr("Persist")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setIconSize(QSize(12, 12));
    m_table->setWordWrap(false);
    m_table->setShowGrid(false);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    // DragOnly: the panel is a source, never a target. Nothing that could be
    // dropped here would mean anything.
    m_table->setDragEnabled(true);
    m_table->setDragDropMode(QAbstractItemView::DragOnly);
    m_table->horizontalHeader()->setSectionResizeMode(ColName, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColType, QHeaderView::ResizeToContents);
    // Only Name stretches. Two stretching columns in a narrow dock leaves both
    // too small to read, and names are what you scan for.
    m_table->horizontalHeader()->setSectionResizeMode(ColDefault,
                                                      QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColSync, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColPersist, QHeaderView::ResizeToContents);
    // Only the Type column: the name is free text, the default is a literal, and
    // the two flags are check boxes.
    m_table->setItemDelegateForColumn(ColType, new TypeDelegate(m_doc, m_table));
    layout->addWidget(m_table, 1);

    auto *hint = new QLabel(
        tr("Drag a row onto the canvas to place it. Hold Ctrl for a set node, "
           "Alt for a get node."),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: %1").arg(theme::textDim().name()));
    layout->addWidget(hint);

    connect(create, &QPushButton::clicked, this, &VariablesPanel::onCreateVariable);
    connect(m_table, &QTableWidget::cellChanged, this, &VariablesPanel::onCellChanged);
    connect(m_table, &QWidget::customContextMenuRequested,
            this, &VariablesPanel::onContextMenu);
    // Both signals: the current cell moving is the ordinary case, and
    // itemSelectionChanged is what catches the selection being cleared without
    // the current cell going anywhere.
    connect(m_table, &QTableWidget::currentCellChanged,
            this, &VariablesPanel::onSelectionChanged);
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &VariablesPanel::onSelectionChanged);

    if (m_doc) {
        connect(m_doc, &Document::graphChanged, this, &VariablesPanel::refresh);
        connect(m_doc, &Document::activeScriptChanged, this, &VariablesPanel::refresh);
        connect(m_doc, &Document::projectChanged, this, &VariablesPanel::refresh);
    }

    refresh();
}

void VariablesPanel::refresh()
{
    // Filling the table writes to every cell. Without the guard the write-back
    // slot would fire once per cell and stamp the graph with its own contents.
    m_loading = true;
    m_table->clearContents();

    const Graph *graph = m_doc ? m_doc->activeGraph() : nullptr;
    const int count = graph ? graph->variables.size() : 0;
    m_table->setRowCount(count);

    const auto flagItem = [](bool on) {
        auto *item = new QTableWidgetItem;
        // Draggable like the rest of the row: a press that never moves still
        // toggles the box, so the whole row can be used as a drag handle.
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable
                       | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled);
        item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        item->setTextAlignment(Qt::AlignCenter);
        return item;
    };

    for (int row = 0; row < count; ++row) {
        const GraphVariable &var = graph->variables.at(row);

        auto *name = new QTableWidgetItem(var.name);
        name->setIcon(QIcon(typeChip(m_doc, var.type)));
        // Rows are matched back to variables by id, so a rename never loses the
        // row it came from.
        name->setData(Qt::UserRole, var.id);
        m_table->setItem(row, ColName, name);

        m_table->setItem(row, ColType, new QTableWidgetItem(var.type));
        m_table->setItem(row, ColDefault, new QTableWidgetItem(var.def));

        auto *sync = flagItem(var.sync);
        sync->setToolTip(tr("Registered with RegisterNetSyncVariable and pushed with SetSynchDirty."));
        m_table->setItem(row, ColSync, sync);

        auto *persist = flagItem(var.persist);
        persist->setToolTip(tr("Written in OnStoreSave and read back in OnStoreLoad."));
        m_table->setItem(row, ColPersist, persist);
    }

    m_loading = false;

    // Refilling the table drops the selection along with the old items, and the
    // Inspector reads the selection. Without this, committing any edit would
    // blank the panel the edit was made in.
    const int row = rowOfVariable(m_selectedId);
    if (row >= 0) m_table->setCurrentCell(row, ColName);
    else setSelectedVariable(QString());
}

int VariablesPanel::rowOfVariable(const QString &variableId) const
{
    if (variableId.isEmpty()) return -1;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *item = m_table->item(row, ColName);
        if (item && item->data(Qt::UserRole).toString() == variableId) return row;
    }
    return -1;
}

QString VariablesPanel::currentVariableId() const
{
    // A cleared selection has to read as no variable at all, and the current
    // cell survives clearing, so the selection model is asked first.
    const QItemSelectionModel *selection = m_table->selectionModel();
    if (!selection || !selection->hasSelection()) return QString();
    const QTableWidgetItem *item = m_table->item(m_table->currentRow(), ColName);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void VariablesPanel::setSelectedVariable(const QString &variableId)
{
    if (variableId == m_selectedId) return;
    m_selectedId = variableId;
    emit variableSelected(variableId);
}

void VariablesPanel::onSelectionChanged()
{
    // Mid-rebuild the table has no selection to speak of, and the one it loses
    // there is not one the user gave up.
    if (m_loading) return;
    setSelectedVariable(currentVariableId());
}

void VariablesPanel::selectVariable(const QString &variableId)
{
    const int row = rowOfVariable(variableId);
    if (row < 0) return;
    m_table->setCurrentCell(row, ColName);
}

void VariablesPanel::onCreateVariable()
{
    if (!m_doc || !m_doc->activeGraph()) return;

    m_doc->beginEdit(tr("Create variable"));
    Graph *graph = m_doc->activeGraph();
    GraphVariable var;
    var.id = nextId(QStringLiteral("v"));
    var.name = uniqueName(*graph, QStringLiteral("m_NewVar"));
    var.type = QStringLiteral("bool");
    graph->variables.append(var);
    m_doc->commitEdit();

    // The table has been rebuilt by the change signal; put the caret in the new
    // name so the variable can be named without a second click.
    beginRename(var.id);
}

void VariablesPanel::beginRename(const QString &variableId)
{
    const int row = rowOfVariable(variableId);
    if (row < 0) return;
    selectVariable(variableId);
    m_table->editItem(m_table->item(row, ColName));
}

void VariablesPanel::onCellChanged(int row, int column)
{
    if (m_loading || !m_doc || !m_doc->activeGraph()) return;

    QTableWidgetItem *nameItem = m_table->item(row, ColName);
    QTableWidgetItem *edited = m_table->item(row, column);
    if (!nameItem || !edited) return;

    // Read the cell before anything commits: committing rebuilds the table and
    // every item pointer with it.
    const QString id = nameItem->data(Qt::UserRole).toString();
    const QString text = edited->text().trimmed();
    const bool checked = edited->checkState() == Qt::Checked;

    Graph *graph = m_doc->activeGraph();
    const int index = indexOfVariable(*graph, id);
    if (index < 0) return;

    if (column == ColName) {
        // An empty or duplicated name produces a class that will not compile,
        // so the edit is refused and the cell put back as it was.
        bool clash = text.isEmpty();
        for (int i = 0; i < graph->variables.size() && !clash; ++i)
            clash = i != index && graph->variables.at(i).name == text;
        if (clash) {
            refresh();
            return;
        }
    }

    m_doc->beginEdit(tr("Edit variable"));
    GraphVariable &var = m_doc->activeGraph()->variables[index];
    switch (column) {
    case ColName: var.name = text; break;
    case ColType: var.type = text; break;
    case ColDefault: var.def = text; break;
    case ColSync: var.sync = checked; break;
    case ColPersist: var.persist = checked; break;
    default: break;
    }
    m_doc->commitEdit();
}

void VariablesPanel::onContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_table->indexAt(pos);
    QTableWidgetItem *nameItem = index.isValid() ? m_table->item(index.row(), ColName)
                                                 : nullptr;
    const QString id = nameItem ? nameItem->data(Qt::UserRole).toString() : QString();
    const bool onVariable = !id.isEmpty();

    QMenu menu(this);
    QAction *getter = menu.addAction(tr("Add get node"));
    QAction *setter = menu.addAction(tr("Add set node"));
    menu.addSeparator();
    QAction *remove = menu.addAction(tr("Delete variable"));
    menu.addSeparator();
    QAction *create = menu.addAction(tr("Create variable"));
    getter->setEnabled(onVariable);
    setter->setEnabled(onVariable);
    remove->setEnabled(onVariable);

    const QAction *chosen = menu.exec(m_table->viewport()->mapToGlobal(pos));
    if (!chosen) return;

    if (chosen == create) {
        onCreateVariable();
        return;
    }
    if (chosen == getter) {
        emit variableNodeRequested(id, false);
        return;
    }
    if (chosen == setter) {
        emit variableNodeRequested(id, true);
        return;
    }
    if (chosen != remove || !m_doc || !m_doc->activeGraph()) return;

    m_doc->beginEdit(tr("Delete variable"));
    Graph *graph = m_doc->activeGraph();
    // Get/set nodes name the variable by id, so they go with it. Left behind
    // they would generate a reference to something the class no longer has.
    // The id has to match exactly: a suffix test sweeps away the nodes of every
    // other variable whose id this one ends with ("count" inside "hitcount").
    QStringList doomed;
    for (const GraphNode &node : graph->nodes) {
        if (node.kind != NodeKind::VarGet && node.kind != NodeKind::VarSet) continue;
        const GraphVariable *owner = variableForRef(*graph, node.ref);
        if (owner && owner->id == id) doomed << node.id;
    }
    for (const QString &nodeId : doomed) removeNode(*graph, nodeId);

    const int victim = indexOfVariable(*graph, id);
    if (victim >= 0) graph->variables.removeAt(victim);
    m_doc->commitEdit();
}
