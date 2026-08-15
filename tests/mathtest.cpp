// Can a user do basic maths from the palette, and does each operator survive
// into the generated code?
#include "builtins.h"
#include "catalog.h"
#include "codegen.h"
#include "project.h"
#include <QCoreApplication>
#include <QTextStream>
static int fails=0;
int main(int argc,char**argv){
    QCoreApplication a(argc,argv); QTextStream o(stdout);
    Catalog cat; cat.load("resources/catalog.json");
    Builtins builtins;

    o << "palette entries" << Qt::endl;
    int found=0;
    for (const NodeDef &d : builtins.all())
        if (d.key.startsWith("bi.op.")) found++;
    o << (found==13?"  ok   ":"  FAIL ") << found << " per-operator entries" << Qt::endl;
    if (found!=13) fails++;

    // Searching a word should reach the operator.
    for (const char *word : {"multiply","subtract","divide","add"}) {
        bool hit=false;
        for (const NodeDef &d : builtins.all())
            if (d.subtitle.contains(QString::fromLatin1(word))) { hit=true; break; }
        o << (hit?"  ok   ":"  FAIL ") << "searching '" << word << "' finds an operator" << Qt::endl;
        if(!hit) fails++;
    }

    o << Qt::endl << "generated code" << Qt::endl;
    for (const char *sym : {"+","-","*","/","%",">=","&&"}) {
        Graph g; g.className="T"; g.baseClass="ItemBase";
        GraphNode begin; begin.id="b"; begin.kind=NodeKind::Builtin; begin.ref=bi::Begin;
        begin.opts.insert("noSuper","1");
        GraphNode pr; pr.id="p"; pr.kind=NodeKind::Builtin; pr.ref=bi::Print;
        GraphNode op; op.id="o"; op.kind=NodeKind::Builtin;
        op.ref = QString("bi.op.") + QString::fromLatin1(sym);   // placed from the palette
        op.inputs.insert("a","2"); op.inputs.insert("b","3");
        g.nodes << begin << pr << op;
        g.edges.append({"e1",{"b","exec"},{"p","exec"},{}});
        g.edges.append({"e2",{"o","ret"},{"p","value"},{}});
        Project proj;
        const GenResult gen = generateEnforce(g, cat, builtins, proj);
        QString line;
        for (const QString &l : gen.code.split('\n'))
            if (l.contains("Print(")) line = l.trimmed();
        const bool ok = line.contains(QString::fromLatin1(sym));
        o << (ok?"  ok   ":"  FAIL ") << QString("%1  %2").arg(QString::fromLatin1(sym),-4).arg(line) << Qt::endl;
        if(!ok) fails++;
    }
    o << Qt::endl << (fails==0?"MATH OK":QString("%1 FAILURES").arg(fails)) << Qt::endl;
    return fails==0?0:1;
}
