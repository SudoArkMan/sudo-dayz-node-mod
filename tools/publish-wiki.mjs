// Publishes docs/ to the GitHub wiki.
//
// The wiki is its own git repository with a flat page namespace: a file called
// Getting-Started.md is served at /wiki/Getting-Started. So the tree under
// docs/ is flattened here, and every link between documents is rewritten to
// match, because a relative .md link that works in the repository is a 404 on
// the wiki.
//
//   node tools/publish-wiki.mjs            check what would change
//   node tools/publish-wiki.mjs --push     write it and push
//
// The wiki is a clone under build/wiki, kept between runs so a publish is a
// diff rather than a rewrite of every page.

import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

const REPO = "https://github.com/SudoArkMan/sudo-dayz-node-mod.wiki.git";
const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname).replace(/^\/([A-Za-z]:)/, "$1"), "..");
const DOCS = path.join(ROOT, "docs");
const WORK = path.join(ROOT, "build", "wiki");
const push = process.argv.includes("--push");

// docs/getting-started.md becomes Getting-Started, which is both the file name
// and the URL. Notes keep a prefix so they sort together in the sidebar and do
// not collide with a top level page of the same name.
function pageName(rel) {
  const parts = rel.replace(/\.md$/, "").split(/[\\/]/);
  const titled = parts.map(p =>
    p.split("-").map(w => (w ? w[0].toUpperCase() + w.slice(1) : w)).join("-"));
  if (titled[0] === "Notes") return titled.slice(1).join("-");
  if (titled.length > 1 && titled[titled.length - 1] === "README")
    return titled.slice(0, -1).join("-");
  return titled.join("-");
}

function walk(dir, base = dir) {
  const out = [];
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) { out.push(...walk(p, base)); continue; }
    if (e.name.endsWith(".md")) out.push(path.relative(base, p));
  }
  return out;
}

const files = walk(DOCS).sort();
const pages = new Map(files.map(f => [f, pageName(f)]));

// A link is rewritten only when it names a document we are publishing. Anything
// else is left alone, because a link out to the repository is still correct.
function rewrite(text, fromRel) {
  return text.replace(/\]\(([^)\s]+?)(#[^)]*)?\)/g, (whole, target, anchor = "") => {
    if (/^(https?:|mailto:)/.test(target)) return whole;
    if (/\.(png|jpg|jpeg|gif|svg)$/i.test(target))
      return `](${path.posix.join("images", path.basename(target))}${anchor})`;
    const resolved = path.normalize(path.join(path.dirname(fromRel), target));
    for (const [rel, name] of pages)
      if (path.normalize(rel) === resolved) return `](${name}${anchor})`;
    // A document that lives in the repository rather than the wiki.
    if (target.endsWith(".md"))
      return `](https://github.com/SudoArkMan/sudo-dayz-node-mod/blob/master/${target.replace(/^(\.\.\/)+/, "")}${anchor})`;
    return whole;
  });
}

if (!fs.existsSync(WORK)) {
  fs.mkdirSync(path.dirname(WORK), { recursive: true });
  execFileSync("git", ["clone", REPO, WORK], { stdio: "inherit" });
} else {
  execFileSync("git", ["-C", WORK, "pull", "--quiet"], { stdio: "inherit" });
}

// Clear the pages we own, so a document deleted from docs/ leaves the wiki too.
// Home.md is written below rather than preserved, because a stub Home is what a
// new wiki starts with and leaving it means the front page never says anything.
for (const e of fs.readdirSync(WORK))
  if (e.endsWith(".md")) fs.rmSync(path.join(WORK, e));

// Every page is overwritten on the next publish, so an edit made here would be
// lost with nothing to say where it went. The notice is the only warning a
// person gets before typing into the wrong window.
const notice = rel =>
  `<!-- Generated from docs/${rel.replace(/\\/g, "/")}. Edits made in the wiki are ` +
  `overwritten on the next publish: change the file in the repository instead. -->\n\n`;

const written = [];
for (const [rel, name] of pages) {
  const body = rewrite(fs.readFileSync(path.join(DOCS, rel), "utf8"), rel);
  fs.writeFileSync(path.join(WORK, `${name}.md`), notice(rel) + body);
  written.push(name);
}

const imgSrc = path.join(DOCS, "images");
if (fs.existsSync(imgSrc)) {
  const imgDst = path.join(WORK, "images");
  fs.mkdirSync(imgDst, { recursive: true });
  for (const f of fs.readdirSync(imgSrc))
    fs.copyFileSync(path.join(imgSrc, f), path.join(imgDst, f));
}

// The sidebar is the wiki's only navigation, so it is generated rather than
// hand kept: a page added to docs/ that nobody links to is invisible otherwise.
const order = ["Getting-Started", "Node-Reference", "Examples", "Architecture"];
const rest = written.filter(n => !order.includes(n)).sort();
const inOrder = [...order.filter(n => written.includes(n)), ...rest];
fs.writeFileSync(path.join(WORK, "_Sidebar.md"),
  "### SUDO DayZ Node Mod\n\n" +
  inOrder.map(n => `- [[${n.replace(/-/g, " ")}|${n}]]`).join("\n") +
  "\n\n[Repository](https://github.com/SudoArkMan/sudo-dayz-node-mod)\n");

// The front page is the sidebar's contents with a line about the project, so a
// wiki with pages on it never opens on "Welcome to the wiki".
fs.writeFileSync(path.join(WORK, "Home.md"),
  "<!-- Generated by tools/publish-wiki.mjs. Edits here are overwritten. -->\n\n" +
  "# SUDO DayZ Node Mod\n\n" +
  "A visual scripting editor for DayZ Enforce Script. It opens a mod's scripts as node\n" +
  "graphs and writes them back byte for byte.\n\n" +
  inOrder.map(n => `- [[${n.replace(/-/g, " ")}|${n}]]`).join("\n") +
  "\n\nThe source, the releases and the issue tracker are in the\n" +
  "[repository](https://github.com/SudoArkMan/sudo-dayz-node-mod).\n");

const status = execFileSync("git", ["-C", WORK, "status", "--porcelain"], { encoding: "utf8" });
if (!status.trim()) { console.log("wiki is already up to date"); process.exit(0); }
console.log(status.trimEnd());
console.log(`\n${written.length} pages, ${inOrder.length} in the sidebar`);

if (!push) { console.log("\nnothing pushed. Re-run with --push"); process.exit(0); }
// A fresh clone inherits no identity here on Windows, and the commit then fails
// with nothing useful on stdout.
execFileSync("git", ["-C", WORK, "config", "user.name", "SudoArkMan"]);
execFileSync("git", ["-C", WORK, "config", "user.email", "sudoarkman@gmail.com"]);
execFileSync("git", ["-C", WORK, "add", "-A"], { stdio: "inherit" });
execFileSync("git", ["-C", WORK, "commit", "-m", "Publish documentation from docs/"], { stdio: "inherit" });
execFileSync("git", ["-C", WORK, "push", "--quiet"], { stdio: "inherit" });
console.log("published");
