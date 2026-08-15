// The four crash repros from the audit, run for real.
#include "analysis.h"
#include "builtins.h"
#include "catalog.h"
#include "codegen.h"
#include "project.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTextStream>

static Catalog cat; static Builtins builtins; static Project proj;
static int fails = 0;
static void run(const QString &name, Graph &g, int expectMaxMs = 8000) {
    QTextStream o(stdout);
    QElapsedTimer t; t.start();
    const GenResult r = generateEnforce(g, cat, builtins, proj);
    const qint64 ms = t.elapsed();
    int depth = 0; for (const QChar c : r.code) { if (c=='{') depth++; else if (c=='}') depth--; }
    const bool ok = ms < expectMaxMs && depth == 0;
    if (!ok) fails++;
    o << (ok ? "  ok   " : "  FAIL ") << name << "  (" << ms << " ms, "
      << r.code.size()/1024 << " KB, brace depth " << depth << ", "
      << r.warnings.size() << " warnings)" << Qt::endl;
}
static GraphNode N(const QString &id, NodeKind k, const QString &ref, double x=0,double y=0){
    GraphNode n; n.id=id; n.kind=k; n.ref=ref; n.x=x; n.y=y; return n; }
static void E(Graph &g, const QString &fn, const QString &fp, const QString &tn, const QString &tp){
    g.edges.append({nextId("e"), {fn,fp}, {tn,tp}}); }

int main(int argc, char **argv){
    QCoreApplication a(argc, argv);
    QTextStream o(stdout);
    cat.load("resources/catalog.json");
    o << "crash repros from the audit" << Qt::endl;

    { // 1. data-pin cycle: two Not nodes feeding each other
      Graph g; g.className="C1"; g.baseClass="ItemBase";
      g.nodes << N("b",NodeKind::Builtin,"bi.begin") << N("p",NodeKind::Builtin,"bi.print")
              << N("n1",NodeKind::Builtin,"bi.not") << N("n2",NodeKind::Builtin,"bi.not");
      E(g,"b","exec","p","exec"); E(g,"n1","ret","p","value");
      E(g,"n2","ret","n1","a");   E(g,"n1","ret","n2","a");
      run("data-pin cycle (two Not nodes cross-wired)", g); }

    { // 2. exec cycle back into the chain
      Graph g; g.className="C2"; g.baseClass="ItemBase";
      g.nodes << N("b",NodeKind::Builtin,"bi.begin") << N("br",NodeKind::Builtin,"bi.branch")
              << N("p",NodeKind::Builtin,"bi.print");
      E(g,"b","exec","br","exec"); E(g,"br","true","p","exec"); E(g,"p","exec","br","exec");
      run("exec cycle (Print loops back into Branch)", g); }

    { // 3. 500 nested branches, no cycle
      Graph g; g.className="C3"; g.baseClass="ItemBase";
      g.nodes << N("b",NodeKind::Builtin,"bi.begin");
      for(int i=0;i<500;i++){ GraphNode n=N(QString("br%1").arg(i),NodeKind::Builtin,"bi.branch");
        n.inputs["cond"]="true"; g.nodes<<n; }
      E(g,"b","exec","br0","exec");
      for(int i=0;i<499;i++) E(g,QString("br%1").arg(i),"true",QString("br%1").arg(i+1),"exec");
      run("500 nested Branch nodes (depth only)", g); }

    { // 4. sequence fan-out, the 2.5 GB case
      Graph g; g.className="C4"; g.baseClass="ItemBase";
      g.nodes << N("b",NodeKind::Builtin,"bi.begin");
      const int N_=24;
      for(int i=0;i<N_;i++) g.nodes << N(QString("s%1").arg(i),NodeKind::Builtin,"bi.sequence");
      g.nodes << N("p",NodeKind::Builtin,"bi.print");
      E(g,"b","exec","s0","exec");
      for(int i=0;i<N_;i++){ const QString nxt = i+1<N_ ? QString("s%1").arg(i+1) : QString("p");
        for(const char *pin : {"then0","then1","then2"}) E(g,QString("s%1").arg(i),pin,nxt,"exec"); }
      run("24 chained Sequence nodes (3^n fan-out)", g); }

    o << Qt::endl << (fails==0 ? "ALL CRASH REPROS SURVIVED" : QString("%1 FAILED").arg(fails)) << Qt::endl;
    return fails==0?0:1;
}
