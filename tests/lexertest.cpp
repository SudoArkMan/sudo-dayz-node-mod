#include "enforce/lexer.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>
static int fails=0;
static void check(bool ok,const QString&w){QTextStream o(stdout);o<<(ok?"  ok   ":"  FAIL ")<<w<<Qt::endl;if(!ok)fails++;}
int main(int argc,char**argv){
  QCoreApplication a(argc,argv); QTextStream o(stdout);

  o << "tokenising" << Qt::endl;
  { LexState s=LexState::Normal;
    const auto t=EnforceLexer::tokenize("override void EEInit() { m_X = \"hi\"; }",s);
    QString kinds; for(const Token&x:t) if(x.kind!=TokenKind::Whitespace)
      kinds += QString::number(int(x.kind))+" ";
    check(t.first().kind==TokenKind::Keyword,"'override' is a keyword");
    bool sawType=false,sawStr=false;
    for(const Token&x:t){ if(x.text=="void")sawType=x.kind==TokenKind::Type;
                          if(x.text.startsWith('"'))sawStr=x.kind==TokenKind::String; }
    check(sawType,"'void' is a type"); check(sawStr,"string literal recognised"); }

  { LexState s=LexState::Normal;
    EnforceLexer::tokenize("/* start",s);
    check(s==LexState::InBlockComment,"block comment carries state across lines");
    EnforceLexer::tokenize("still comment */ int x;",s);
    check(s==LexState::Normal,"block comment closes"); }

  { LexState s=LexState::Normal;
    const auto t=EnforceLexer::tokenize("#ifdef SERVER",s);
    check(t.first().kind==TokenKind::Preprocessor,"#ifdef is preprocessor"); }

  { LexState s=LexState::Normal;
    const auto t=EnforceLexer::tokenize("float f = 1.5e-3; int h = 0xFF;",s);
    int nums=0; for(const Token&x:t) if(x.kind==TokenKind::Number) nums++;
    check(nums==2,QString("exponent and hex numbers (%1)").arg(nums)); }

  o << "scanning" << Qt::endl;
  { const EnforceScan sc = scanEnforce("m_RestApi = CreateRestApi();\nif (!m_RestApi) return;");
    check(sc.calls.contains("CreateRestApi"),"finds the call");
    check(sc.assignedTo.contains("m_RestApi"),"finds the assignment target");
    check(sc.braceBalance==0,"balanced braces");
    check(sc.statements>=2,"counts statements"); }
  { const EnforceScan sc = scanEnforce("if (x) {\n  y();");
    check(sc.braceBalance==1,"unbalanced brace detected"); }
  { const EnforceScan sc = scanEnforce("GetGame().GetPlayer().SetHealth(100);");
    check(sc.members.contains("GetPlayer")&&sc.members.contains("SetHealth"),"member chain"); }

  o << "summaries" << Qt::endl;
  check(enforceSummary("m_BaseUrl = baseUrl;")=="m_BaseUrl = baseUrl;","short code shown whole");
  { const QString s=enforceSummary("// explain\nDoThing();");
    check(s=="DoThing();",QString("leading comment skipped, got '%1'").arg(s)); }
  { const QString s=enforceSummary("A();\nB();\nC();");
    check(s.contains("+2"),QString("extra line count, got '%1'").arg(s)); }

  // The real test: run it over vanilla source and make sure nothing explodes
  // and identifiers actually come out.
  o << "vanilla sample" << Qt::endl;
  QDir dz("P:/scripts/4_World/Entities");
  const QStringList files = dz.entryList({"*.c"}, QDir::Files);
  int scanned=0; long long idents=0, toks=0;
  for(const QString &f : files.mid(0,25)){
    QFile file(dz.filePath(f));
    if(!file.open(QIODevice::ReadOnly)) continue;
    const QString text=QString::fromUtf8(file.readAll());
    const auto t=EnforceLexer::tokenizeAll(text);
    toks+=t.size();
    int unknown=0; for(const Token&x:t) if(x.kind==TokenKind::Unknown) unknown++;
    if(unknown>0){ o<<"    "<<f<<" has "<<unknown<<" unknown tokens"<<Qt::endl; fails++; }
    idents+=scanEnforce(text).identifiers.size();
    scanned++;
  }
  check(scanned>0,QString("scanned %1 vanilla files, %2 tokens, %3 distinct identifiers")
        .arg(scanned).arg(toks).arg(idents));
  o<<Qt::endl<<(fails==0?"LEXER OK":QString("%1 FAILURES").arg(fails))<<Qt::endl;
  return fails==0?0:1;
}
