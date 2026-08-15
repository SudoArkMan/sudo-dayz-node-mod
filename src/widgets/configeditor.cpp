#include "configeditor.h"

#include "document.h"
#include "project.h"
#include "theme.h"
#include "widgets/codeeditor.h"
#include "widgets/valueeditor.h"

#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

// Which class path a row is under, and which property it is. A class row has an
// empty property; the file's own top level properties have an empty path.
constexpr int kPathRole = Qt::UserRole;
constexpr int kPropRole = Qt::UserRole + 1;

// Above this many rows the tree opens one level instead of all of them. A mod
// config is small enough to read whole, and a vanilla config is not.
constexpr int kExpandAllLimit = 400;

QString joinPath(const QString &parent, const QString &name)
{
    return parent.isEmpty() ? name : parent + QLatin1Char('/') + name;
}

bool sameName(const QString &a, const QString &b)
{
    return a.compare(b, Qt::CaseInsensitive) == 0;
}

// findValue is non-const, and the rules run over a config nobody is allowed to
// edit, so lookups there go through this instead of a const_cast.
const ConfigValue *valueOf(const ConfigClass &node, const QString &name)
{
    for (const ConfigValue &v : node.values)
        if (sameName(v.name, name)) return &v;
    return nullptr;
}

// The property a row names, ready to be written to. An empty path means the
// file's own top level, which is where a vanilla config keeps half its values
// and a mod config keeps none.
ConfigValue *mutableValue(ConfigFile &file, const QString &path, const QString &name)
{
    if (!path.isEmpty()) {
        ConfigClass *owner = findClass(file, path);
        return owner ? findValue(*owner, name) : nullptr;
    }
    for (ConfigValue &value : file.values)
        if (sameName(value.name, name)) return &value;
    return nullptr;
}

bool isQuoted(const QString &literal)
{
    const QString s = literal.trimmed();
    return s.size() >= 2 && s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"'));
}

bool looksNumeric(const QString &text)
{
    bool ok = false;
    text.trimmed().toDouble(&ok);
    return ok;
}

// What the value editor should be built as. config.cpp has no declared types, so
// the literal is all there is to go on: a quoted value is text, a bare number is
// a number, and anything else is a macro or a define that has to survive being
// looked at. Those get a plain field, because a string editor would put quotes
// round a define and stop the config loading.
QString typeForLiteral(const QString &literal)
{
    const QString s = literal.trimmed();
    if (s.isEmpty() || isQuoted(s)) return QStringLiteral("string");
    if (!looksNumeric(s)) return QString();
    bool integral = false;
    s.toInt(&integral);
    return integral ? QStringLiteral("int") : QStringLiteral("float");
}

// The literal to write for text typed into a row. A value that arrived quoted
// goes back quoted; one that did not is left exactly as the author had it, which
// is what keeps a define from being turned into a string.
QString relit(const QString &typed, const QString &original)
{
    const QString text = typed.trimmed();
    if (original.isEmpty()) return looksNumeric(text) ? text : configLiteral(text);
    if (isQuoted(original)) return configLiteral(text);
    return text;
}

// A path out of the config, resolved against the mod folder. Backslashes and a
// leading separator are both things people type, and neither is what QDir wants.
QString resolveUnderRoot(const ConfigContext &context, const QString &literal)
{
    QString rel = configUnquote(literal).trimmed();
    rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
    if (rel.isEmpty() || context.modRoot.isEmpty()) return {};
    return QDir::cleanPath(context.modRoot + QLatin1Char('/') + rel);
}

// The four script modules, plus anything else a mod adds with the same suffix.
// These are folders; the engine walks them for .c files.
bool isScriptModule(const QString &className)
{
    return className.endsWith(QLatin1String("ScriptModule"), Qt::CaseInsensitive);
}

// imageSets and widgetStyles list files rather than folders, and both are
// registered the same silent way.
bool listsFiles(const QString &className)
{
    return sameName(className, QStringLiteral("imageSets"))
           || sameName(className, QStringLiteral("widgetStyles"));
}

// Every class in the file with the path that reaches it.
void collectClasses(const ConfigClass &node, const QString &path,
                    QVector<QPair<QString, const ConfigClass *>> &out)
{
    out.append({ path, &node });
    for (const ConfigClass &child : node.classes)
        collectClasses(child, joinPath(path, child.name), out);
}

QVector<QPair<QString, const ConfigClass *>> allClasses(const ConfigFile &file)
{
    QVector<QPair<QString, const ConfigClass *>> out;
    for (const ConfigClass &c : file.classes) collectClasses(c, c.name, out);
    return out;
}

// One row per entry, edited in place, with Add and Remove beside it.
//
// This is the widget the window is really for. `files[] = { "A/Scripts/3_Game" }`
// written as one comma separated string is where the mistakes live: a missing
// quote, a lost comma, a path that is one letter off and never loads. A row that
// does not resolve on disk is coloured, so the mistake is visible while it is
// being made rather than the next time the mod is launched.
class ArrayEditor : public QWidget {
public:
    ArrayEditor(const QStringList &items, std::function<bool(const QString &)> check,
                QWidget *parent)
        : QWidget(parent), m_check(std::move(check))
    {
        // As tall as its rows and no taller. Left to itself in a form with one
        // property, it would take the whole panel and leave the list floating in
        // the middle of it.
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(4);
        row->setAlignment(Qt::AlignTop);

        m_list = new QListWidget(this);
        m_list->setAlternatingRowColors(false);
        m_list->setSelectionMode(QAbstractItemView::SingleSelection);
        m_list->setEditTriggers(QAbstractItemView::DoubleClicked
                                | QAbstractItemView::SelectedClicked
                                | QAbstractItemView::EditKeyPressed);
        m_list->setUniformItemSizes(true);
        row->addWidget(m_list, 1);

        auto *side = new QVBoxLayout();
        side->setContentsMargins(0, 0, 0, 0);
        side->setSpacing(4);
        auto *add = new QPushButton(tr("Add"), this);
        auto *remove = new QPushButton(tr("Remove"), this);
        side->addWidget(add);
        side->addWidget(remove);
        side->addStretch(1);
        row->addLayout(side);

        m_loading = true;
        for (const QString &literal : items) appendRow(literal);
        m_loading = false;
        refreshMarks();
        resizeToRows();

        connect(m_list, &QListWidget::itemChanged, this, [this]() {
            if (m_loading) return;
            refreshMarks();
            emitChanged();
        });
        connect(add, &QPushButton::clicked, this, [this]() {
            m_loading = true;
            QListWidgetItem *item = appendRow(QString());
            m_loading = false;
            resizeToRows();
            m_list->setCurrentItem(item);
            // Straight into editing: an empty row nobody can see the point of is
            // worse than no row at all.
            m_list->editItem(item);
        });
        connect(remove, &QPushButton::clicked, this, [this]() {
            const int row = m_list->currentRow();
            if (row < 0) return;
            delete m_list->takeItem(row);
            resizeToRows();
            emitChanged();
        });
    }

    // The literals as they should be written, quotes included.
    QStringList literals() const
    {
        QStringList out;
        for (int i = 0; i < m_list->count(); ++i) {
            const QListWidgetItem *item = m_list->item(i);
            const QString text = item->text().trimmed();
            if (text.isEmpty()) continue;
            out << relit(text, item->data(Qt::UserRole).toString());
        }
        return out;
    }

    std::function<void(const QStringList &)> onChanged;

private:
    QListWidgetItem *appendRow(const QString &literal)
    {
        auto *item = new QListWidgetItem(configUnquote(literal), m_list);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setData(Qt::UserRole, literal);
        return item;
    }

    void emitChanged()
    {
        if (onChanged) onChanged(literals());
    }

    // A path that is not there is the whole point of the panel, so it is marked
    // where it is typed and not only in the findings list.
    void refreshMarks()
    {
        if (!m_check) return;
        // Colouring a row is a change to the row, and the list says so. Without
        // this the mark set here comes straight back in as an edit, and the
        // value is written to the model twice for every character typed.
        const bool outer = m_loading;
        m_loading = true;
        for (int i = 0; i < m_list->count(); ++i) {
            QListWidgetItem *item = m_list->item(i);
            const QString text = item->text().trimmed();
            const bool ok = text.isEmpty() || m_check(text);
            item->setForeground(ok ? theme::text() : theme::errorColor());
            item->setToolTip(ok ? QString()
                                : tr("Nothing at this path under the mod folder."));
        }
        m_loading = outer;
    }

    // A files[] with two entries in a box sized for ten is dead space in a panel
    // that has none to spare.
    void resizeToRows()
    {
        const int rows = qBound(2, m_list->count() + 1, 6);
        const int step = m_list->sizeHintForRow(0) > 0 ? m_list->sizeHintForRow(0) : 18;
        m_list->setFixedHeight(rows * step + 6);
    }

    QListWidget *m_list;
    std::function<bool(const QString &)> m_check;
    bool m_loading = false;
};

// Every open window, keyed by the file it is on, so activating a config twice
// raises the first window instead of opening a second editor over the same
// bytes. Cleared from the destructor.
QHash<QString, ConfigEditor *> &openWindows()
{
    static QHash<QString, ConfigEditor *> windows;
    return windows;
}

QString windowKey(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

} // namespace

bool ConfigContext::onDisk() const
{
    if (modRoot.isEmpty() || prefix.isEmpty()) return false;
    // A folder with a Scripts in it, which is the same test the rest of the app
    // uses to decide what a mod folder is. Without it every config opened out of
    // a downloads folder would have its own folder read as a mod prefix, and
    // every path in the file would come back missing.
    return QFileInfo(modRoot + QLatin1Char('/') + prefix + QStringLiteral("/Scripts"))
        .isDir();
}

ConfigContext configContextFor(const QString &configPath, const Project &project)
{
    ConfigContext context;
    const QString clean = QDir::cleanPath(QFileInfo(configPath).absoluteFilePath());

    // The shape the template lays down is <root>/<prefix>/Scripts/config.cpp,
    // and a mod that keeps its config one folder higher is still <root>/<prefix>.
    QDir dir = QFileInfo(clean).absoluteDir();
    if (sameName(dir.dirName(), QStringLiteral("Scripts"))) dir.cdUp();
    context.prefix = dir.dirName();
    QDir root = dir;
    if (root.cdUp()) context.modRoot = QDir::cleanPath(root.absolutePath());

    // The project knows better when the config is inside the mod folder it was
    // scaffolded with: a prefix set there by hand beats one read off the path.
    if (!project.modRoot.isEmpty()) {
        const QString projectRoot =
            QDir::cleanPath(QFileInfo(project.modRoot).absoluteFilePath());
        const QString rel = QDir(projectRoot).relativeFilePath(clean);
        const bool inside = !rel.startsWith(QLatin1String("../"))
                            && !QDir::isAbsolutePath(rel) && rel.contains(QLatin1Char('/'));
        if (inside) {
            context.modRoot = projectRoot;
            context.prefix = project.modPrefix.isEmpty()
                                 ? rel.section(QLatin1Char('/'), 0, 0)
                                 : project.modPrefix;
        }
    }
    return context;
}

QVector<ConfigFinding> validateConfig(const ConfigFile &file, const ConfigContext &context)
{
    QVector<ConfigFinding> found;

    auto add = [&found](ConfigFinding::Level level, const QString &path,
                        const QString &property, const QString &text) -> ConfigFinding & {
        ConfigFinding finding;
        finding.level = level;
        finding.path = path;
        finding.property = property;
        finding.text = text;
        found.append(finding);
        return found.last();
    };

    // What this mod is called. The folder on disk is the answer when the file is
    // sitting in one, and the config's own dir is the answer when it is not, so
    // a config opened from anywhere still gets the check below.
    QString modName = context.onDisk() ? context.prefix : QString();
    if (modName.isEmpty()) {
        if (const ConfigClass *mods = findClass(file, QStringLiteral("CfgMods"))) {
            if (!mods->classes.isEmpty()) {
                const ConfigClass &first = mods->classes.first();
                const ConfigValue *dir = valueOf(first, QStringLiteral("dir"));
                modName = dir ? configUnquote(dir->scalar).trimmed() : QString();
                if (modName.isEmpty()) modName = first.name;
            }
        }
    }
    if (modName.isEmpty()) modName = context.prefix;

    // 1. The patch class still named after the template. Every mod scaffolded
    // from it declares MT_Scripts, and two addons cannot declare the same patch
    // class: the second one to load is the one that breaks.
    if (const ConfigClass *patches = findClass(file, QStringLiteral("CfgPatches"))) {
        for (const ConfigClass &patch : patches->classes) {
            if (!sameName(patch.name, QStringLiteral("MT_Scripts"))) continue;
            if (modName.isEmpty() || sameName(modName, QStringLiteral("MT"))) continue;
            const QString wanted = modName + QStringLiteral("_Scripts");
            ConfigFinding &f = add(
                ConfigFinding::Level::Error,
                joinPath(QStringLiteral("CfgPatches"), patch.name), QString(),
                QStringLiteral("MT_Scripts is the mod template's own patch class. Every "
                               "mod built from that template declares it, so any two of "
                               "them collide when both are loaded. This one should be %1.")
                    .arg(wanted));
            f.fix = ConfigFinding::Fix::RenamePatchClass;
            f.fixValue = wanted;
            f.fixLabel = QStringLiteral("Rename to %1").arg(wanted);
        }
    }

    // 2. A files[] entry that is not on disk. The engine registers the module and
    // finds nothing in it, the mod's script never runs, and no log line says so.
    if (context.onDisk()) {
        for (const auto &entry : allClasses(file)) {
            const ConfigClass &node = *entry.second;
            const bool modules = isScriptModule(node.name);
            if (!modules && !listsFiles(node.name)) continue;

            const ConfigValue *files = valueOf(node, QStringLiteral("files"));
            if (!files || !files->isArray) continue;

            for (const QString &item : files->items) {
                const QString resolved = resolveUnderRoot(context, item);
                if (resolved.isEmpty()) continue;
                const QFileInfo info(resolved);
                if (modules ? info.isDir() : info.exists()) continue;
                add(ConfigFinding::Level::Error, entry.first, QStringLiteral("files"),
                    modules
                        ? QStringLiteral("%1 lists %2, which is not a folder under the "
                                         "mod. The module loads nothing and the game "
                                         "does not say why.")
                              .arg(node.name, configUnquote(item))
                        : QStringLiteral("%1 lists %2, which is not there.")
                              .arg(node.name, configUnquote(item)));
            }
        }
    }

    // 3 to 6, over the mod entries themselves.
    static const QVector<ConfigClass> noMods;
    const ConfigClass *mods = findClass(file, QStringLiteral("CfgMods"));
    for (const ConfigClass &mod : mods ? mods->classes : noMods) {
        const QString path = joinPath(QStringLiteral("CfgMods"), mod.name);
        const ConfigValue *dirValue = valueOf(mod, QStringLiteral("dir"));
        const QString dirText =
            dirValue ? configUnquote(dirValue->scalar).trimmed() : QString();

        // 3. dir names the folder the engine goes looking for. When it does not
        // match the folder that is actually there, nothing under it is found.
        bool dirWrong = false;
        if (context.onDisk() && !dirText.isEmpty() && !sameName(dirText, context.prefix)) {
            dirWrong = true;
            add(ConfigFinding::Level::Error, path, QStringLiteral("dir"),
                QStringLiteral("%1 has dir \"%2\", but the mod folder on disk is called "
                               "%3. The engine looks for the folder dir names.")
                    .arg(mod.name, dirText, context.prefix));
        }

        // 5. Class against dir. Skipped when dir has already been reported: one
        // wrong word should not read as two problems.
        if (!dirWrong && !dirText.isEmpty() && !sameName(mod.name, dirText)) {
            add(ConfigFinding::Level::Error, path, QStringLiteral("dir"),
                QStringLiteral("%1 has dir \"%2\". The class name and dir are the mod's "
                               "one name written twice, so one of the two is wrong.")
                    .arg(mod.name, dirText));
        }

        // 4. inputs points at the file that registers the mod's key bindings.
        if (const ConfigValue *inputs = valueOf(mod, QStringLiteral("inputs"))) {
            const QString text = configUnquote(inputs->scalar).trimmed();
            if (context.onDisk() && !text.isEmpty()) {
                const QString resolved = resolveUnderRoot(context, inputs->scalar);
                if (!resolved.isEmpty() && !QFileInfo(resolved).isFile())
                    add(ConfigFinding::Level::Warning, path, QStringLiteral("inputs"),
                        QStringLiteral("%1 has inputs \"%2\", which is not there. The "
                                       "mod's own key bindings are not registered.")
                            .arg(mod.name, text));
            }
        }

        // 6. What the launcher shows. Both ship empty in the template.
        for (const char *field : { "name", "author" }) {
            const QString name = QString::fromLatin1(field);
            const ConfigValue *value = valueOf(mod, name);
            const QString text = value ? configUnquote(value->scalar).trimmed() : QString();
            if (!text.isEmpty()) continue;
            add(ConfigFinding::Level::Warning, path, name,
                QStringLiteral("%1 has no %2. This is what the launcher lists the mod by.")
                    .arg(mod.name, name));
        }
    }

    // Errors first, each group in the order the rules ran, so the one that stops
    // the mod loading is never below the one about a blank launcher row.
    std::stable_sort(found.begin(), found.end(),
                     [](const ConfigFinding &a, const ConfigFinding &b) {
                         return a.level == ConfigFinding::Level::Error
                                && b.level != ConfigFinding::Level::Error;
                     });
    return found;
}

ConfigEditor::ConfigEditor(QWidget *parent, Document *doc, const QString &path)
    : QDialog(parent), m_doc(doc),
      m_path(QDir::cleanPath(QFileInfo(path).absoluteFilePath())),
      m_tree(new QTreeWidget(this)), m_propertyArea(new QScrollArea(this)),
      m_propertyTitle(new QLabel(this)), m_findingList(new QListWidget(this)),
      m_findingSummary(new QLabel(this)), m_textToggle(new QToolButton(this)),
      m_text(new CodeEditor(this)), m_split(new QSplitter(Qt::Vertical, this)),
      m_buttons(new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close,
                                     this)),
      m_textSettle(new QTimer(this)), m_editSettle(new QTimer(this))
{
    // Not modal and not blocking the canvas, so it gets a real window frame: a
    // dialog frame has no minimise button, and this window stays open as long as
    // the config is being worked on.
    setWindowFlags(Qt::Window);
    setAttribute(Qt::WA_DeleteOnClose, true);

    m_context = configContextFor(m_path, m_doc ? m_doc->project() : Project());

    auto *top = new QSplitter(Qt::Horizontal, this);

    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({ tr("Class"), tr("Value") });
    m_tree->header()->setStretchLastSection(true);
    m_tree->header()->resizeSection(0, 220);
    m_tree->setUniformRowHeights(true);
    m_tree->setAnimated(false);
    m_tree->setTextElideMode(Qt::ElideMiddle);
    m_tree->setIndentation(14);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    top->addWidget(m_tree);

    auto *right = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(8, 0, 0, 0);
    rightLayout->setSpacing(4);
    m_propertyTitle->setFont(theme::uiFont(8, true));
    rightLayout->addWidget(m_propertyTitle);
    m_propertyArea->setWidgetResizable(true);
    m_propertyArea->setFrameShape(QFrame::NoFrame);
    rightLayout->addWidget(m_propertyArea, 1);
    top->addWidget(right);
    top->setStretchFactor(0, 3);
    top->setStretchFactor(1, 4);

    auto *problems = new QWidget(this);
    auto *problemLayout = new QVBoxLayout(problems);
    problemLayout->setContentsMargins(0, 4, 0, 0);
    problemLayout->setSpacing(3);
    m_findingSummary->setFont(theme::uiFont(8, true));
    problemLayout->addWidget(m_findingSummary);
    m_findingList->setSelectionMode(QAbstractItemView::NoSelection);
    m_findingList->setFocusPolicy(Qt::NoFocus);
    problemLayout->addWidget(m_findingList, 1);

    auto *textPane = new QWidget(this);
    auto *textLayout = new QVBoxLayout(textPane);
    textLayout->setContentsMargins(0, 4, 0, 0);
    textLayout->setSpacing(3);
    m_textToggle->setCheckable(true);
    m_textToggle->setChecked(true);
    m_textToggle->setText(tr("File text"));
    m_textToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_textToggle->setArrowType(Qt::DownArrow);
    m_textToggle->setAutoRaise(true);
    textLayout->addWidget(m_textToggle, 0, Qt::AlignLeft);
    m_text->setFont(theme::monoFont(8));
    // No document context: completion here would offer Enforce classes, and this
    // file has none. The highlighting is worth keeping, since a config's strings,
    // numbers and comments read the same way a script's do.
    textLayout->addWidget(m_text, 1);

    m_split->addWidget(top);
    m_split->addWidget(problems);
    m_split->addWidget(textPane);
    m_split->setStretchFactor(0, 5);
    m_split->setStretchFactor(1, 1);
    m_split->setStretchFactor(2, 2);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);
    layout->addWidget(m_split, 1);
    layout->addWidget(m_buttons);

    m_textSettle->setSingleShot(true);
    m_textSettle->setInterval(500);
    connect(m_textSettle, &QTimer::timeout, this, &ConfigEditor::reparseFromText);

    m_editSettle->setSingleShot(true);
    m_editSettle->setInterval(200);
    connect(m_editSettle, &QTimer::timeout, this, [this]() {
        refreshText();
        refreshFindings();
    });

    connect(m_tree, &QTreeWidget::itemSelectionChanged, this,
            &ConfigEditor::onTreeSelectionChanged);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this,
            &ConfigEditor::onTreeContextMenu);
    connect(m_text, &QPlainTextEdit::textChanged, this, [this]() {
        if (m_loading) return;
        setDirty(true);
        m_textSettle->start();
    });
    connect(m_textToggle, &QToolButton::toggled, this, [this](bool open) {
        m_textToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        m_text->setVisible(open);
    });

    connect(m_buttons, &QDialogButtonBox::accepted, this, [this]() {
        QString error;
        if (!save(&error)) QMessageBox::warning(this, tr("Save config"), error);
    });
    connect(m_buttons, &QDialogButtonBox::rejected, this, &ConfigEditor::reject);

    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, [this]() {
        if (QPushButton *button = m_buttons->button(QDialogButtonBox::Save))
            button->click();
    });

    QSettings settings;
    const QSize remembered = settings.value(QStringLiteral("configEditor/size")).toSize();
    resize(remembered.isValid() ? remembered : QSize(1000, 760));

    // Stretch factors alone give the findings a single row, and a list of
    // problems you have to scroll to read is a list nobody reads. The splitter
    // keeps whatever the user drags it to afterwards.
    const int tall = height();
    m_split->setSizes({ tall * 55 / 100, tall * 20 / 100, tall * 25 / 100 });

    if (QPushButton *button = m_buttons->button(QDialogButtonBox::Save))
        button->setEnabled(false);
    updateTitle();
}

ConfigEditor::~ConfigEditor()
{
    QSettings settings;
    settings.setValue(QStringLiteral("configEditor/size"), size());

    auto &windows = openWindows();
    for (auto it = windows.begin(); it != windows.end(); ++it) {
        if (it.value() != this) continue;
        windows.erase(it);
        break;
    }
}

ConfigEditor *ConfigEditor::openFile(QWidget *parent, Document *doc, const QString &path,
                                     QString *error)
{
    const QString key = windowKey(path);
    if (ConfigEditor *existing = openWindows().value(key)) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return existing;
    }

    auto *window = new ConfigEditor(parent, doc, path);
    if (!window->load(error)) {
        delete window;
        return nullptr;
    }
    openWindows().insert(key, window);
    window->show();
    return window;
}

bool ConfigEditor::load(QString *error)
{
    // A config is a few kilobytes. Anything of this size with a .cpp on the end
    // is something else wearing the extension, and reading it in to find that
    // out costs a minute of somebody's afternoon.
    const QFileInfo info(m_path);
    if (info.size() > 8 * 1024 * 1024) {
        if (error)
            *error = tr("%1 is %2 MB, which is not a config.")
                         .arg(info.fileName())
                         .arg(info.size() / (1024.0 * 1024.0), 0, 'f', 1);
        return false;
    }

    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = tr("Cannot read %1.").arg(QDir::toNativeSeparators(m_path));
        return false;
    }
    const QByteArray bytes = file.readAll();

    m_crlf = bytes.contains("\r\n");
    QString text = QString::fromUtf8(bytes);
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));

    m_file = parseConfig(text);
    if (m_file.classes.isEmpty() && m_file.values.isEmpty()) {
        if (error)
            *error = m_file.errors.isEmpty()
                         ? tr("There is no class in it to show.")
                         : m_file.errors.first();
        return false;
    }

    m_loading = true;
    m_text->setPlainText(text);
    m_loading = false;

    buildTree();
    // The mod's own CfgMods entry is what the file gets opened for: the name in
    // the launcher, the folder, the inputs, the four module lists under it. The
    // first class in the file is the fallback, since a panel that opens on
    // nothing reads as a panel that does not work.
    const ConfigClass *mods = findClass(m_file, QStringLiteral("CfgMods"));
    if (mods && !mods->classes.isEmpty())
        selectPath(joinPath(QStringLiteral("CfgMods"), mods->classes.first().name),
                   QString());
    else if (QTreeWidgetItem *first = m_tree->topLevelItem(0))
        m_tree->setCurrentItem(first);
    // Selecting scrolls, and a file that opens part way down hides the two
    // classes at the top of every config. The selection is inside the first
    // screenful of anything mod sized anyway.
    m_tree->scrollToTop();
    buildProperties();
    refreshFindings();
    setDirty(false);
    return true;
}

bool ConfigEditor::save(QString *error)
{
    // Text typed into the pane and not yet settled is still the author's edit,
    // so it goes into the model before anything is written. The other direction
    // matters as much: a value typed in the panel a moment ago has not reached
    // the pane yet, and the pane is what the check below reads.
    if (m_textSettle->isActive()) {
        m_textSettle->stop();
        reparseFromText();
    }
    if (m_editSettle->isActive()) {
        m_editSettle->stop();
        refreshText();
        refreshFindings();
    }

    const ConfigFile check = parseConfig(m_text->toPlainText());
    if (!check.errors.isEmpty()) {
        if (error)
            *error = tr("The file text has a problem, so nothing was written:\n\n%1")
                         .arg(check.errors.first());
        return false;
    }

    QString text = writeConfig(m_file);
    if (m_crlf) text.replace(QLatin1String("\n"), QLatin1String("\r\n"));

    QSaveFile out(m_path);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error) *error = tr("Cannot write %1.").arg(QDir::toNativeSeparators(m_path));
        return false;
    }
    out.write(text.toUtf8());
    if (!out.commit()) {
        if (error) *error = tr("Failed to save %1.").arg(QDir::toNativeSeparators(m_path));
        return false;
    }

    setDirty(false);
    return true;
}

void ConfigEditor::buildTree()
{
    const QSignalBlocker block(m_tree);
    m_tree->clear();

    int rows = 0;
    std::function<void(QTreeWidgetItem *, const ConfigClass &, const QString &)> addClassRow =
        [&](QTreeWidgetItem *parent, const ConfigClass &node, const QString &parentPath) {
            const QString path = joinPath(parentPath, node.name);
            auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
            rows++;
            item->setText(0, node.name);
            item->setFont(0, theme::uiFont(8, true));
            item->setData(0, kPathRole, path);
            item->setData(0, kPropRole, QString());
            if (!node.base.isEmpty())
                item->setText(1, tr("inherits %1").arg(node.base));
            else if (node.external)
                item->setText(1, tr("declared elsewhere"));
            if (!node.base.isEmpty() || node.external)
                item->setForeground(1, theme::textDim());

            for (const ConfigValue &value : node.values) {
                auto *row = new QTreeWidgetItem(item);
                rows++;
                row->setText(0, value.isArray ? value.name + QStringLiteral("[]")
                                              : value.name);
                row->setText(1, value.isArray ? value.items.join(QStringLiteral(", "))
                                              : value.scalar);
                row->setForeground(0, theme::textDim());
                row->setData(0, kPathRole, path);
                row->setData(0, kPropRole, value.name);
            }
            for (const ConfigClass &child : node.classes) addClassRow(item, child, path);
        };

    // A config with properties at the top level is unusual in a mod and normal
    // in vanilla, so they are shown rather than quietly dropped.
    for (const ConfigValue &value : m_file.values) {
        auto *row = new QTreeWidgetItem(m_tree);
        rows++;
        row->setText(0, value.isArray ? value.name + QStringLiteral("[]") : value.name);
        row->setText(1, value.isArray ? value.items.join(QStringLiteral(", "))
                                      : value.scalar);
        row->setForeground(0, theme::textDim());
        row->setData(0, kPathRole, QString());
        row->setData(0, kPropRole, value.name);
    }
    for (const ConfigClass &node : m_file.classes) addClassRow(nullptr, node, QString());

    if (rows <= kExpandAllLimit)
        m_tree->expandAll();
    else
        m_tree->expandToDepth(0);
}

void ConfigEditor::buildProperties()
{
    ConfigClass *node = selectedClass();
    const QString path = selectedPath();
    QVector<ConfigValue> *values = node ? &node->values : &m_file.values;

    auto *host = new QWidget(m_propertyArea);
    // The form sits in a column with a stretch under it. On its own it would
    // share the panel's height out between however many properties the class
    // happens to have, which puts two fields at opposite ends of the window.
    auto *column = new QVBoxLayout(host);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    auto *form = new QFormLayout();
    column->addLayout(form);
    column->addStretch(1);
    form->setContentsMargins(0, 0, 6, 0);
    form->setSpacing(6);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_propertyTitle->setText(node ? path : tr("File, outside any class"));

    if (values->isEmpty()) {
        auto *note = new QLabel(
            node ? tr("%1 holds classes and no properties of its own. Right-click the "
                      "tree to add one.").arg(node->name)
                 : tr("Nothing outside the classes. Right-click the tree to add a class."),
            host);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:%1;").arg(theme::textDim().name()));
        form->addRow(note);
    }

    // Paths only mean something when there is a mod folder to measure them
    // against, and only the module lists hold paths.
    const bool checkPaths = m_context.onDisk() && node
                            && (isScriptModule(node->name) || listsFiles(node->name));
    const ConfigContext context = m_context;
    const bool wantsFolder = node && isScriptModule(node->name);

    for (const ConfigValue &value : *values) {
        const QString name = value.name;
        if (value.isArray) {
            std::function<bool(const QString &)> check;
            if (checkPaths && sameName(name, QStringLiteral("files"))) {
                check = [context, wantsFolder](const QString &typed) {
                    const QString resolved = resolveUnderRoot(context, typed);
                    if (resolved.isEmpty()) return true;
                    const QFileInfo info(resolved);
                    return wantsFolder ? info.isDir() : info.exists();
                };
            }
            auto *editor = new ArrayEditor(value.items, check, host);
            editor->onChanged = [this, path, name](const QStringList &items) {
                ConfigValue *target = mutableValue(m_file, path, name);
                if (!target || target->items == items) return;
                target->items = items;
                applyValueEdit(path, name);
            };
            form->addRow(name + QStringLiteral("[]"), editor);
            continue;
        }

        const QString type = typeForLiteral(value.scalar);
        if (type.isEmpty()) {
            // A define or a macro. Shown exactly as written, and written back
            // exactly as typed, because nothing here knows what it means.
            auto *line = new QLineEdit(value.scalar, host);
            connect(line, &QLineEdit::textEdited, this,
                    [this, path, name](const QString &typed) {
                        ConfigValue *target = mutableValue(m_file, path, name);
                        if (!target || target->scalar == typed) return;
                        target->scalar = typed;
                        applyValueEdit(path, name);
                    });
            form->addRow(name, line);
            continue;
        }

        auto *editor = new ValueEditor(m_doc ? &m_doc->catalog() : nullptr, host);
        editor->setType(type);
        editor->setValue(value.scalar);
        connect(editor, &ValueEditor::valueChanged, this,
                [this, path, name](const QString &literal) {
                    ConfigValue *target = mutableValue(m_file, path, name);
                    if (!target || target->scalar == literal) return;
                    target->scalar = literal;
                    applyValueEdit(path, name);
                });
        form->addRow(name, editor);
    }

    m_propertyArea->setWidget(host);
}

void ConfigEditor::buildFindings()
{
    m_findingList->clear();

    int errors = 0;
    for (const ConfigFinding &finding : m_findings)
        if (finding.level == ConfigFinding::Level::Error) errors++;
    const int warnings = m_findings.size() - errors;

    if (m_findings.isEmpty()) {
        m_findingSummary->setText(tr("Nothing to flag."));
        m_findingSummary->setStyleSheet(
            QStringLiteral("color:%1;").arg(theme::textDim().name()));
    } else {
        m_findingSummary->setText(tr("%1 errors, %2 warnings").arg(errors).arg(warnings));
        m_findingSummary->setStyleSheet(
            QStringLiteral("color:%1;")
                .arg((errors > 0 ? theme::errorColor() : theme::warningColor()).name()));
    }

    for (int i = 0; i < m_findings.size(); ++i) {
        const ConfigFinding &finding = m_findings.at(i);
        const bool error = finding.level == ConfigFinding::Level::Error;

        auto *row = new QWidget(m_findingList);
        row->setProperty("finding", i);
        row->installEventFilter(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setSpacing(6);

        auto *label = new QLabel(finding.text, row);
        label->setWordWrap(true);
        label->setStyleSheet(
            QStringLiteral("color:%1;")
                .arg((error ? theme::errorColor() : theme::warningColor()).name()));
        layout->addWidget(label, 1);

        QPushButton *fixButton = nullptr;
        if (finding.fix != ConfigFinding::Fix::None && !finding.fixLabel.isEmpty()) {
            auto *fix = new QPushButton(finding.fixLabel, row);
            fixButton = fix;
            fix->setCursor(Qt::PointingHandCursor);
            const QString path = finding.path;
            const QString wanted = finding.fixValue;
            connect(fix, &QPushButton::clicked, this, [this, path, wanted]() {
                // Applying the fix rebuilds this list, and the button being
                // clicked lives in it. Deleting a button while its own click is
                // still on the stack takes the process with it, so the work
                // waits for the next turn of the event loop.
                QTimer::singleShot(0, this, [this, path, wanted]() {
                    ConfigClass *node = findClass(m_file, path);
                    if (!node) return;
                    node->name = wanted;
                    applyStructureEdit();
                    selectPath(joinPath(path.section(QLatin1Char('/'), 0, -2), wanted),
                               QString());
                });
            });
            layout->addWidget(fix, 0);
        }

        auto *item = new QListWidgetItem(m_findingList);
        // The row is a widget, so the list cannot work its own height out, and a
        // word wrapped label asked for its own size hint answers with something
        // nearly square. Measured for the width the text will really wrap at
        // instead: the sentences are long on purpose, since a finding that does
        // not say what breaks is one people learn to scroll past.
        const int taken = fixButton ? fixButton->sizeHint().width() + 40 : 40;
        // Before the first layout the list reports a placeholder width, and a
        // row measured against that comes out several times too tall. The window
        // is the better guess until the list has a real one.
        const int across = m_findingList->viewport()->width() > 200
                               ? m_findingList->viewport()->width()
                               : width() - 24;
        const int wrap = qMax(240, across - taken);
        const int text = label->fontMetrics()
                             .boundingRect(QRect(0, 0, wrap, 0), Qt::TextWordWrap,
                                           finding.text)
                             .height();
        int height = text + 10;
        if (fixButton) height = qMax(height, fixButton->sizeHint().height() + 8);
        item->setSizeHint(QSize(0, height));
        m_findingList->setItemWidget(item, row);
    }
}

void ConfigEditor::refreshFindings()
{
    m_findings.clear();
    // What the parser could not read comes first. The tree only shows what was
    // understood, so a line it could not place has to be said out loud or the
    // window quietly claims the file holds less than it does.
    for (const QString &problem : m_file.errors) {
        ConfigFinding finding;
        finding.level = ConfigFinding::Level::Error;
        finding.text = problem;
        m_findings.append(finding);
    }
    m_findings += validateConfig(m_file, m_context);
    buildFindings();
}

void ConfigEditor::refreshText()
{
    const QString text = writeConfig(m_file);
    if (m_text->toPlainText() == text) return;

    m_loading = true;
    const int scroll = m_text->verticalScrollBar()->value();
    m_text->setPlainText(text);
    m_text->verticalScrollBar()->setValue(scroll);
    m_loading = false;
}

QTreeWidgetItem *ConfigEditor::itemForPath(const QString &path, const QString &property) const
{
    for (QTreeWidgetItemIterator it(m_tree); *it; ++it) {
        if ((*it)->data(0, kPathRole).toString() != path) continue;
        if ((*it)->data(0, kPropRole).toString() != property) continue;
        return *it;
    }
    return nullptr;
}

void ConfigEditor::selectPath(const QString &path, const QString &property)
{
    QTreeWidgetItem *item = itemForPath(path, property);
    // A renamed or deleted class takes its row with it, so the class it sat in
    // is the next best place to land.
    if (!item && !property.isEmpty()) item = itemForPath(path, QString());
    if (!item && !path.isEmpty())
        item = itemForPath(path.section(QLatin1Char('/'), 0, -2), QString());
    if (!item) item = m_tree->topLevelItem(0);
    if (!item) return;

    m_tree->setCurrentItem(item);
    m_tree->scrollToItem(item);
}

ConfigClass *ConfigEditor::selectedClass()
{
    const QString path = selectedPath();
    return path.isEmpty() ? nullptr : findClass(m_file, path);
}

QString ConfigEditor::selectedPath() const
{
    const QTreeWidgetItem *item = m_tree->currentItem();
    return item ? item->data(0, kPathRole).toString() : QString();
}

void ConfigEditor::onTreeSelectionChanged()
{
    buildProperties();
}

void ConfigEditor::onTreeContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (item) m_tree->setCurrentItem(item);

    const QString path = item ? item->data(0, kPathRole).toString() : QString();
    const QString property = item ? item->data(0, kPropRole).toString() : QString();
    ConfigClass *node = path.isEmpty() ? nullptr : findClass(m_file, path);
    // A property row acts on the class it hangs under, which is the row's own
    // path: only the value it names is different.
    const bool onProperty = !property.isEmpty();

    QMenu menu(this);

    menu.addAction(tr("Add class..."), this, [this, node]() {
        bool accepted = false;
        const QString name = QInputDialog::getText(this, tr("Add class"), tr("Class name"),
                                                   QLineEdit::Normal, QString(), &accepted)
                                 .trimmed();
        if (!accepted || name.isEmpty()) return;
        if (node) addClass(*node, name);
        else addClass(m_file, name);
        applyStructureEdit();
        selectPath(joinPath(node ? node->name : QString(), name), QString());
    });

    if (node) {
        menu.addAction(tr("Add property..."), this, [this, node, path]() {
            bool accepted = false;
            const QString typed =
                QInputDialog::getText(this, tr("Add property"),
                                      tr("Name, ending in [] for a list"),
                                      QLineEdit::Normal, QString(), &accepted)
                    .trimmed();
            if (!accepted || typed.isEmpty()) return;
            const bool array = typed.endsWith(QLatin1String("[]"));
            const QString name = array ? typed.left(typed.size() - 2).trimmed() : typed;
            if (name.isEmpty()) return;
            if (ConfigValue *added = addValue(*node, name, array)) {
                // An empty pair of quotes rather than nothing: a scalar with no
                // value at all is not something config.cpp can express.
                if (!array) added->scalar = QStringLiteral("\"\"");
            }
            applyStructureEdit();
            selectPath(path, name);
        });
    }

    if (onProperty && node) {
        if (const ConfigValue *value = valueOf(*node, property)) {
            if (value->isArray) {
                menu.addAction(tr("Add item"), this, [this, node, path, property]() {
                    ConfigValue *target = findValue(*node, property);
                    if (!target) return;
                    target->items.append(QStringLiteral("\"\""));
                    applyStructureEdit();
                    selectPath(path, property);
                });
            }
        }
    }

    if (item) {
        menu.addSeparator();
        menu.addAction(tr("Rename..."), this, [this, node, path, property, onProperty]() {
            const QString before = onProperty ? property : (node ? node->name : QString());
            bool accepted = false;
            const QString name =
                QInputDialog::getText(this, tr("Rename"), tr("New name"),
                                      QLineEdit::Normal, before, &accepted)
                    .trimmed();
            if (!accepted || name.isEmpty() || name == before) return;

            if (onProperty) {
                ConfigValue *target = node ? findValue(*node, property)
                                           : findValue(m_file, property);
                if (!target) return;
                target->name = name;
                applyStructureEdit();
                selectPath(path, name);
                return;
            }
            if (!node) return;
            node->name = name;
            applyStructureEdit();
            selectPath(joinPath(path.section(QLatin1Char('/'), 0, -2), name), QString());
        });

        menu.addAction(tr("Delete"), this, [this, node, path, property, onProperty]() {
            const QString what = onProperty ? property : (node ? node->name : QString());
            if (what.isEmpty()) return;
            // A class takes everything under it, which is worth being asked about
            // once: this window has no undo of its own.
            const bool heavy = !onProperty && node
                               && (!node->classes.isEmpty() || !node->values.isEmpty());
            const QString question =
                heavy ? tr("Delete class %1 and everything in it?").arg(what)
                      : tr("Delete %1?").arg(what);
            if (QMessageBox::question(this, tr("Delete"), question,
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                != QMessageBox::Yes)
                return;

            const QString parentPath = path.section(QLatin1Char('/'), 0, -2);
            ConfigClass *parent = parentPath.isEmpty() ? nullptr
                                                       : findClass(m_file, parentPath);
            if (onProperty) {
                if (node) removeValue(*node, property);
            } else if (parent) {
                removeClass(*parent, what);
            } else {
                for (int i = 0; i < m_file.classes.size(); ++i) {
                    if (!sameName(m_file.classes.at(i).name, what)) continue;
                    m_file.classes.remove(i);
                    break;
                }
            }
            applyStructureEdit();
            selectPath(onProperty ? path : parentPath, QString());
        });
    }

    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void ConfigEditor::reparseFromText()
{
    ConfigFile parsed = parseConfig(m_text->toPlainText());
    if (!parsed.errors.isEmpty()) {
        // Half a brace is not a reason to throw the tree away. The errors go up
        // where the findings are, and the tree stays on the last text that read.
        m_findings.clear();
        for (const QString &error : parsed.errors) {
            ConfigFinding finding;
            finding.level = ConfigFinding::Level::Error;
            finding.text = error;
            m_findings.append(finding);
        }
        buildFindings();
        return;
    }

    const QString path = selectedPath();
    const QString property =
        m_tree->currentItem() ? m_tree->currentItem()->data(0, kPropRole).toString()
                              : QString();

    m_file = parsed;
    buildTree();
    selectPath(path, property);
    buildProperties();
    refreshFindings();
}

void ConfigEditor::applyValueEdit(const QString &path, const QString &property)
{
    setDirty(true);

    // The row under the cursor is cheap to correct now; the file text and the
    // rules are not, so they settle once the typing stops.
    if (QTreeWidgetItem *row = itemForPath(path, property)) {
        const ConfigValue *value = mutableValue(m_file, path, property);
        if (value)
            row->setText(1, value->isArray ? value->items.join(QStringLiteral(", "))
                                           : value->scalar);
    }
    m_editSettle->start();
}

void ConfigEditor::applyStructureEdit()
{
    setDirty(true);
    const QString path = selectedPath();
    const QString property =
        m_tree->currentItem() ? m_tree->currentItem()->data(0, kPropRole).toString()
                              : QString();

    buildTree();
    selectPath(path, property);
    buildProperties();
    refreshText();
    refreshFindings();
}

bool ConfigEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress) {
        const QVariant index = watched->property("finding");
        if (index.isValid()) {
            const int at = index.toInt();
            if (at >= 0 && at < m_findings.size()) {
                const ConfigFinding &finding = m_findings.at(at);
                if (!finding.path.isEmpty()) selectPath(finding.path, finding.property);
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void ConfigEditor::setDirty(bool dirty)
{
    if (m_dirty == dirty) return;
    m_dirty = dirty;
    if (QPushButton *button = m_buttons->button(QDialogButtonBox::Save))
        button->setEnabled(dirty);
    updateTitle();
}

void ConfigEditor::updateTitle()
{
    const QFileInfo info(m_path);
    setWindowTitle(QStringLiteral("%1%2 - %3")
                       .arg(info.fileName(),
                            m_dirty ? QStringLiteral(" *") : QString(),
                            QDir::toNativeSeparators(info.absolutePath())));
}

void ConfigEditor::reject()
{
    // Not QDialog::reject, which hides the window without a close event and
    // would step straight past the unsaved-changes check.
    close();
}

void ConfigEditor::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (event->oldSize().width() == event->size().width()) return;
    if (m_findingList->count() > 0) buildFindings();
}

void ConfigEditor::closeEvent(QCloseEvent *event)
{
    if (!m_dirty) {
        event->accept();
        return;
    }

    const QMessageBox::StandardButton answer = QMessageBox::warning(
        this, tr("Unsaved changes"),
        tr("%1 has changes that are not written to disk.").arg(QFileInfo(m_path).fileName()),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);

    if (answer == QMessageBox::Cancel) {
        event->ignore();
        return;
    }
    if (answer == QMessageBox::Save) {
        QString error;
        if (!save(&error)) {
            QMessageBox::warning(this, tr("Save config"), error);
            event->ignore();
            return;
        }
    }
    event->accept();
}
