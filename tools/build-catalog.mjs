#!/usr/bin/env node
/**
 * Turns the dayz-script-api index into resources/catalog.json.
 *
 *   node tools/build-catalog.mjs [apiIndex.json] [out.json]
 *   node tools/build-catalog.mjs --legacy-events   (the pre-fix classification)
 *
 * Default source is the dayz-script-api skill's index. Re-run after a DayZ
 * update (regenerate that index first) to refresh the node library.
 *
 * The output is packed, a string table plus flat tuples, because the verbose
 * shape of the source index is ~12 MB and most of it (docs, field lists, parse
 * metadata) is not needed to build nodes.
 *
 * This lived in the Electron project until the event classification had to be
 * fixed. It is here now because catalog.json is a file this repo ships and a
 * repo that cannot rebuild what it ships has no way to prove the file matches
 * the rule that is documented for it. `--legacy-events` restores the old rule
 * so the two outputs can be diffed against each other.
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = path.dirname(fileURLToPath(import.meta.url))
const args = process.argv.slice(2)
const LEGACY_EVENTS = args.includes('--legacy-events')
const positional = args.filter((a) => !a.startsWith('--'))
const SRC = positional[0]
  ?? path.join(process.env.USERPROFILE ?? process.env.HOME ?? '',
    '.claude', 'skills', 'dayz-script-api', 'reference', 'api-index.json')
const OUT = positional[1] ?? path.join(HERE, '..', 'resources', 'catalog.json')

if (!fs.existsSync(SRC)) {
  console.error(`API index not found: ${SRC}
Generate it first:
  node ~/.claude/skills/dayz-script-api/scripts/build_index.mjs P:\\scripts`)
  process.exit(2)
}

const idx = JSON.parse(fs.readFileSync(SRC, 'utf8'))

// --------------------------------------------------------------- string table
const strings = []
const strIds = new Map()
const S = (s) => {
  const key = s ?? ''
  let id = strIds.get(key)
  if (id === undefined) { id = strings.length; strings.push(key); strIds.set(key, id) }
  return id
}
// Reserve id 0 for the empty string: `0` doubles as the "no guards" sentinel,
// so whatever lands at index 0 would otherwise be read back as a real value.
S('')

// ------------------------------------------------------------------- flags
export const F = {
  STATIC: 1,
  PROTO: 2,
  NATIVE: 4,
  OVERRIDE: 8,
  PURE: 16,
  EVENT: 32,
  PROTECTED: 64,
  CTOR: 128,
}

/** Methods that read state and return it get pure (exec-less) nodes, matching
 *  Blueprint's convention. Anything that might mutate keeps its exec pins. */
const PURE_PREFIX = /^(Get|Is|Has|Can|To|Find|Compute|Calculate|Query|Count|Contains|Should)/

/**
 * `editor/` is Workbench tooling: plugins that run inside the editor, never
 * inside a mission. No class a mod writes descends from anything in there, so
 * an override declared under it is not evidence that anybody's base method is
 * a hook. Ten of the sixteen `Run` declarations in the tree live here, and they
 * are the whole reason `Timer::Run` was catalogued as an event.
 */
const isEditorPath = (p) => /^editor[\\/]/i.test(p)

/**
 * DayZ's naming convention for a method the engine calls: `EEInit`,
 * `EEItemAttached`, `EOnFrame`, `OnStoreSave`, `OnWorkStart`. It is needed
 * because inheritance evidence cannot see a leaf. Nothing in vanilla extends
 * `PlayerBase`, so nothing overrides `PlayerBase::OnReconnect`, and the most
 * modded class in the game would otherwise declare no hooks at all. It also
 * finds 919 hooks the old rule missed outright, `EntityAI::OnInventoryInit` and
 * `ComponentEnergyManager::OnWork` among them.
 *
 * The capital after the prefix is what keeps `Once` and `Online` out.
 */
const CALLBACK_NAME = /^(EE|EOn|On)[A-Z]/

/**
 * Doc comments are doxygen. Strip the markup that only makes sense rendered,
 * drop `@code` blocks (often dozens of lines), and cap the length: the node
 * inspector wants a paragraph, not a manual page.
 */
const DOC_CAP = 420
function cleanDoc(raw) {
  if (!raw) return ''
  let s = raw
    .replace(/@code[\s\S]*?@endcode/g, ' ')
    .replace(/\\code[\s\S]*?\\endcode/g, ' ')
    .replace(/[\\@](brief|details?|desc)\s*/gi, '')
    .replace(/[\\@]n\b/g, ' ')
    .replace(/[\\@](note|warning|remark|remarks)\s*/gi, (_, t) => ` [${t.toLowerCase()}] `)
    // A semicolon, not a middot. These land in the node inspector, and the
    // house rules this project ships under have no middot and no U+2026 in
    // anything drawn on screen. 1,394 vanilla docs carried one and 229 more
    // ended in an ellipsis character.
    .replace(/[\\@]param\s*(\[[^\]]*\])?\s*/gi, ' ; param ')
    .replace(/[\\@](return|returns)\s*/gi, ' ; returns ')
    .replace(/[\\@](defgroup|ingroup|addtogroup|\{|\})\s*\S*/g, ' ')
    .replace(/[*/]{2,}/g, ' ')
    .replace(/\s+/g, ' ')
    // A semicolon closes the word before it rather than floating between two.
    .replace(/ ;/g, ';')
    // The first note usually follows the end of a sentence, and ".;" is not
    // punctuation anybody writes. Later ones keep the semicolon, which is what
    // stops "param x Width param y Height" running together.
    .replace(/([.:;,!?])\s*;\s*/g, '$1 ')
    .replace(/^\s*;\s*/, '')
    .trim()
  // Three dots, and never hanging off the separator the cut landed on.
  if (s.length > DOC_CAP)
    s = s.slice(0, DOC_CAP - 3).replace(/\s+\S*$/, '').replace(/[;,\s]+$/, '') + '...'
  return s
}

/** `out`/`inout` params surface as extra output pins; Enforce leans on them
 *  heavily (Class.CastTo, ConfigGetChildName, ...). */
function paramDir(text) {
  if (/\binout\b/.test(text)) return 2
  if (/\bout\b/.test(text)) return 1
  return 0
}

function parseParam(raw) {
  const text = raw.trim()
  if (!text || text === 'void') return null
  const dir = paramDir(text)
  // strip modifiers, then split "type name = default"
  const stripped = text
    .replace(/\b(out|inout|notnull|ref|autoptr|const|owned|local|volatile)\b\s*/g, '')
    .trim()
  const eq = stripped.indexOf('=')
  const decl = (eq >= 0 ? stripped.slice(0, eq) : stripped).trim()
  const def = eq >= 0 ? stripped.slice(eq + 1).trim() : null
  // split on the last whitespace that is not inside <>
  let depth = 0, cut = -1
  for (let i = 0; i < decl.length; i++) {
    const c = decl[i]
    if (c === '<') depth++
    else if (c === '>') depth--
    else if (/\s/.test(c) && depth === 0) cut = i
  }
  const type = cut < 0 ? decl : decl.slice(0, cut).trim()
  const name = cut < 0 ? '' : decl.slice(cut + 1).trim().replace(/\[.*$/, '')
  return { type: type || 'auto', name: name || 'value', dir, def }
}

// ------------------------------------------------------- collect declarations
const classes = []
const classIdByName = new Map()
const files = []
const fileIds = new Map()
const FI = (p) => {
  let id = fileIds.get(p)
  if (id === undefined) { id = files.length; files.push(p); fileIds.set(p, id) }
  return id
}

// `typedef ItemBase InventoryItemSuper;` and 38 others like it. DayZ swaps what
// a whole family extends by aliasing one name, so `Magazine extends
// InventoryItemSuper` really extends ItemBase. All 58 `extends` edges in the
// tree that name a non-class name resolve through one of these.
const aliasOf = new Map()
for (const f of idx.files)
  for (const t of f.typedefs ?? [])
    if (!aliasOf.has(t.name)) aliasOf.set(t.name, t.aliasOf)

for (const f of idx.files) {
  for (const c of f.classes ?? []) {
    if (c.forward) continue
    if (classIdByName.has(c.name)) continue // first (retail) declaration wins
    classIdByName.set(c.name, classes.length)
    classes.push({
      n: c.name,
      x: c.extends ? c.extends.replace(/<.*$/, '').trim() : null,
      f: FI(f.path),
      l: c.line,
      m: S(f.module),
      g: c.guards?.length ? S(c.guards.join('&')) : 0,
      d: cleanDoc(c.doc),
    })
  }
}
// resolve extends to ids once every class is known
for (const c of classes) c.x = c.x && classIdByName.has(c.x) ? classIdByName.get(c.x) : -1

// ------------------------------------------------------------------- methods
const methods = []

/**
 * Which declarations are overridable hooks, i.e. Event nodes.
 *
 * Event-ness is a property of one declaration on one class, not of a method
 * name. Deciding it by name meant that `event void Run()` on `WorkbenchPlugin`
 * made `Timer::Run(float, Managed, string, ...)` an event too, and an event
 * node has no input pins at all, so `Timer::Run` could not be reached from a
 * Timer pin and a five second timer needed a raw text node.
 *
 * The rule: walk up from every class that declares `override N` and mark
 * `<ancestor>|N`. A declaration of N on C is a hook when some descendant of C
 * really overrides it. Evidence from `editor/` does not count.
 *
 * The `event` keyword is independent evidence and stronger than any of this:
 * the language itself says the engine calls it. 83 declarations carry it.
 */
// Base class by name, with typedef aliases resolved. Deliberately separate from
// the packed `classes[].x` the app reads: widening the shipped class graph moves
// `isA`, the access check, the palette's class filter and the lowering resolver
// all at once, which is its own change with its own evidence. This map exists
// only to decide event-ness.
const baseByName = new Map()
for (const f of idx.files) {
  for (const c of f.classes ?? []) {
    if (c.forward || baseByName.has(c.name)) continue
    let base = c.extends ? c.extends.replace(/<.*$/, '').trim() : null
    if (base && !classIdByName.has(base) && aliasOf.has(base)) base = aliasOf.get(base)
    baseByName.set(c.name, classIdByName.has(base) ? base : null)
  }
}

const hooked = new Set()          // "Class|Method" for the chain-walked rule
const overriddenByName = new Set() // the pre-fix rule, kept for --legacy-events
for (const f of idx.files) {
  for (const c of f.classes ?? []) {
    for (const m of c.methods ?? []) {
      if (!m.modifiers?.includes('override')) continue
      overriddenByName.add(m.name)
      if (isEditorPath(f.path)) continue
      // Start at the base: a class does not override its own declaration.
      // `seen` bounds a cycle, which a hand-edited index can contain.
      const seen = new Set()
      let base = baseByName.get(c.name) ?? null
      while (base && !seen.has(base)) {
        seen.add(base)
        hooked.add(`${base}|${m.name}`)
        base = baseByName.get(base) ?? null
      }
    }
  }
}

const seenPerClass = new Set()
for (const f of idx.files) {
  for (const c of f.classes ?? []) {
    if (c.forward) continue
    const owner = classIdByName.get(c.name)
    if (owner === undefined) continue
    for (const m of c.methods ?? []) {
      const mods = m.modifiers ?? []
      if (mods.includes('private')) continue          // not callable from script
      if (m.name.startsWith('~')) continue            // destructor
      const key = `${owner}|${m.name}|${m.params.length}`
      if (seenPerClass.has(key)) continue
      seenPerClass.add(key)

      const isCtor = m.name === c.name
      const params = m.params.map(parseParam).filter(Boolean)
      const ret = isCtor ? c.name : (m.returns || 'void')
      const hasOut = params.some((p) => p.dir !== 0)

      let flags = 0
      if (mods.includes('static')) flags |= F.STATIC
      if (mods.includes('proto')) flags |= F.PROTO
      if (mods.includes('native')) flags |= F.NATIVE
      if (mods.includes('override')) flags |= F.OVERRIDE
      if (mods.includes('protected')) flags |= F.PROTECTED
      if (isCtor) flags |= F.CTOR
      if (!isCtor && ret !== 'void' && !hasOut && PURE_PREFIX.test(m.name)) flags |= F.PURE
      // Getters are excluded even though they are technically overridable:
      // `GetQuantity` is something you call, and an Event node (exec-out, no
      // inputs) is the wrong shape for it. `proto native` is excluded because
      // the engine owns the body: an override of one does not compile, so an
      // Event node for it could only ever generate a broken file.
      const evidence = LEGACY_EVENTS
        ? overriddenByName.has(m.name)
        : hooked.has(`${c.name}|${m.name}`)
          || mods.includes('event')
          || CALLBACK_NAME.test(m.name)
      // The `event` keyword marks a declaration that is never itself an
      // override, so the !override test still holds for both rules.
      const nativeBody = !LEGACY_EVENTS && mods.includes('native')
      if (!isCtor && evidence && !nativeBody && !mods.includes('override')
          && !(flags & F.PURE)) {
        flags |= F.EVENT
      }

      methods.push([
        owner,
        S(m.name),
        S(ret),
        // 4th slot is the declared default, so codegen can omit trailing
        // optional arguments instead of inventing a value for them
        params.map((p) => [S(p.type), S(p.name), p.dir, p.def ? S(p.def) : 0]),
        flags,
        m.line,
        m.guards?.length ? S(m.guards.join('&')) : 0,
        m.doc ? S(cleanDoc(m.doc)) : 0,
      ])
    }
  }
}

// --------------------------------------------------------------------- enums
const enums = []
const seenEnum = new Set()
for (const f of idx.files) {
  for (const e of f.enums ?? []) {
    if (seenEnum.has(e.name)) continue
    seenEnum.add(e.name)
    enums.push([S(e.name), e.values.map((v) => S(v.name)), FI(f.path), e.line])
  }
}

// ------------------------------------------------------------ global functions
const globals = []
for (const f of idx.files) {
  for (const g of f.globalFuncs ?? []) {
    const params = g.params.map(parseParam).filter(Boolean)
    const hasOut = params.some((p) => p.dir !== 0)
    let flags = F.STATIC
    if (g.modifiers?.includes('proto')) flags |= F.PROTO
    if (g.modifiers?.includes('native')) flags |= F.NATIVE
    if (g.returns !== 'void' && !hasOut && PURE_PREFIX.test(g.name)) flags |= F.PURE
    globals.push([
      S(g.name),
      S(g.returns || 'void'),
      params.map((p) => [S(p.type), S(p.name), p.dir, p.def ? S(p.def) : 0]),
      flags,
      FI(f.path),
      g.line,
      g.guards?.length ? S(g.guards.join('&')) : 0,
      g.doc ? S(cleanDoc(g.doc)) : 0,
    ])
  }
}

// ------------------------------------------------------------------ constants
const consts = []
for (const f of idx.files) {
  for (const c of f.consts ?? []) consts.push([S(c.name), S(c.type), S(c.value ?? ''), FI(f.path), c.line])
}

const out = {
  v: 1,
  source: idx.root,
  strings,
  files,
  classes: classes.map((c) => [S(c.n), c.x, c.f, c.l, c.m, c.g, S(c.d)]),
  methods,
  enums,
  globals,
  consts,
  totals: {
    classes: classes.length,
    methods: methods.length,
    events: methods.filter((m) => m[4] & F.EVENT).length,
    pure: methods.filter((m) => m[4] & F.PURE).length,
    enums: enums.length,
    globals: globals.length,
    consts: consts.length,
  },
}

fs.mkdirSync(path.dirname(OUT), { recursive: true })
fs.writeFileSync(OUT, JSON.stringify(out))

const kb = (n) => `${(n / 1024).toFixed(0)} KB`
console.log(`[catalog] ${SRC}`)
if (LEGACY_EVENTS) console.log('[catalog] --legacy-events: name-global event rule')
console.log(`[catalog] classes ${out.totals.classes}  methods ${out.totals.methods}  ` +
  `(events ${out.totals.events}, pure ${out.totals.pure})`)
console.log(`[catalog] enums ${out.totals.enums}  globals ${out.totals.globals}  consts ${out.totals.consts}`)
console.log(`[catalog] strings ${strings.length}  ->  ${OUT}  ${kb(fs.statSync(OUT).size)}`)
