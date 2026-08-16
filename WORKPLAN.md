# Work plan

Standing plan for the current run. Kept in the repo so it survives a restart.

## Environment, verified on this machine

| | |
| --- | --- |
| Qt | 6.11.0 MinGW 13.1, `C:/Qt/6.11.0/mingw_64` |
| Build | `cmake -S . -B build/cli -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/mingw_64 -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe` |
| Work drive | `P:\` mounted, holds `DZ`, `Mods`, `Core` |
| DayZ Tools | `D:/SteamLibrary/steamapps/common/DayZ Tools/Bin` |
| PBO extractor | `DayZ Tools/Bin/PboUtils/BankRev.exe`, plus `FileBank.exe` to pack |
| Diag client and server | `D:/SteamLibrary/steamapps/common/DayZ/DayZDiag_x64.exe`, `.../DayZServer` |
| Installed mods | 254 under `D:/SteamLibrary/steamapps/common/DayZ/!Workshop` |
| Frameworks on disk | CF and Dabs in `C:/Users/dilla/Downloads`, addon names in `resources/known-mods.json` |

The DayZ Tools registry key `HKCU\SOFTWARE\Bohemia Interactive\Dayz Tools` is NOT set here, so
nothing may depend on it alone.

## Rules that keep biting, do not relearn them

- A running `DAYZSUDONodeMod.exe` holds its own binary. The link fails, **Ninja aborts before
  relinking the test targets**, and every measurement after that reads a stale binary. Kill the app
  before building, and never trust numbers from a build that failed.
- A pin's `def` is emitted code. Deciding whether a pin gets an inline FIELD through
  `inlineEditorFor` therefore changes generated output. The field rule is `fieldEditorFor` in
  `src/nodeinputs.h`; both the painter and the click handler must use it.
- Quoting belongs where the user types, not where code is emitted: the lowering legitimately puts
  real identifiers into pins, so quoting in the generator turns working code into `Print("someVar")`.
- Test targets must live in the repo. A target pointing at the scratchpad breaks configure for
  everyone once that folder is cleaned.
- `QGuiApplication::instance()` resolves to the inherited `QCoreApplication::instance()`, so a
  console test thinks a GUI exists and `QFontMetrics` fail-fasts with 0xC0000602. Guard with
  `qobject_cast<QGuiApplication *>(...)`.

## Order of work

1. **Start page and project management** — recent projects, new, open, templates, saving.
2. **Interface polish** — the app grew feature by feature and needs a pass for coherence.
3. **PBO reading and mod import** — open any installed mod and read its scripts as graphs.
4. **Mod browser** — the 254 installed mods as a browsable library, each showing its node layout.

## Two ways to test a mod, and the app needs both

The user's point, and it is right: the template can launch **offline** for a quick look, but not
everything works there, so testing on a **dev server** has to be a first-class choice rather than
the only one or an afterthought.

Only `DayZDiag_x64.exe` is usable for either. Retail `DayZ_x64.exe` and `DayZServer_x64.exe` both
block past the loading screen once `-filePatching` is on.

Offline, one process, fast to iterate:

    DayZDiag_x64.exe -mission=<mission folder> -profiles=<client profiles> -mod=@A;@B -filePatching

Dev server, two processes, server first, client once the port is open:

    DayZDiag_x64.exe -server -config=<serverDZ.cfg> -profiles=<server profiles> \
        -mission=<mission> -mod=@A;@B -filePatching -port=2302
    DayZDiag_x64.exe -profiles=<client profiles> -mod=@A;@B -connect=127.0.0.1 -port=2302 \
        -filePatching

The template supplies both halves already: `Missions/<Mod>.ChernarusPlus` for the mission and
`Workbench/server.cfg` for the server, with `allowFilePatching = 1` set.

The UI must say what offline cannot show, because finding out by chasing a bug that only exists
offline is the expensive way to learn it. The list in `offlineLimits()` is the checked one and the
citations sit in `testrun.h`. Two corrections to what was guessed here first:

- **COT permissions do not fail closed offline, they fail open.** The guess was that the per-player
  files a server writes on first boot are missing. The real behaviour, read out of COT's own
  `scripts.pbo`: `JMPermissionManager::HasPermission` opens with `if ( IsMissionOffline() ) return
  true;`, and roles load only under `IsServer() && IsMultiplayer()`. So a permission-gated feature
  *always opens* offline, which is the more dangerous direction: it works on your machine and locks
  players out on the server.
- **`-serverMod=` was dropped, not confirmed.** It is not an offline-versus-dev-server difference the
  way this app is built, because neither mode passes a server-only chain.

## PBO notes

The format is a header of entries (filename, packing method, original size, reserved, timestamp,
data size), a zero-length terminating entry, then the data blobs back to back in the same order,
then a 20 byte SHA1 trailer.

The compressed mime is LZSS, but the two-byte token is a distance back into the **already-produced
output**, not an index into a 4096 byte ring buffer. Textbook Okumura LZSS decodes the first 20 or so
bytes of a DayZ script correctly and then turns to noise, which is the failure mode that reads as
success. Reading behind the start of the output yields `0x20`, and 3,985 entries in the installed
corpus depend on that. The 4 byte trailer after each stream is the sum of the decoded bytes and is
checked on every read, so a wrong decode is refused rather than returned. That check is what makes
the 100% figure below mean anything.

The corpus is genuinely hostile, measured not assumed: 22,956 entry names containing `..`, 578,059
that look absolute, 1,034,830 carrying control or reserved characters, one archive with a 150 MB
header claiming 1,298,973 entries, and a 2.1 GB archive. Bound the entry count by `fileSize / 21`
rather than a constant, or the three genuine million-entry mods get refused. Bound decompression by
`dataSize * 9 + 64`, the ceiling of what LZSS can expand, so a hostile size field cannot force an
allocation. A native reader is preferred
over shelling out to `BankRev.exe`: it works headless in tests, has no install dependency, and the
254 installed mods are the corpus to prove it against.

Measured, `pbotest --all`, 16 Aug: 1874 archives over 253 mods, 1874 opened, sizes reconciled on all
of them, 9,343,981 packed entries decoded and checksum-matched, 0 refused, 575.7 MB in 33.8 s.
Against BankRev as an outside oracle: 648 entries byte for byte, 0 differed. `BankRev.exe` is no
longer a fallback worth keeping, because it is the weaker reader of the two: it turns down all five
obfuscated archives the native reader opens, and it silently declines to write 373 entries it did
read. Its remaining use is as a second opinion in the test, not in the app.

`modlibrarytest`: 266 mods scanned, 1887 pbos, 0 refused. On a 1-in-6 sample of the 216 mods that
ship script, **23% of methods become node graphs** and the rest keep their text, well below the 64%
seen on the user's own code. Third-party mod code is the harder corpus and that number is the one to
move; it is per-method, so a mod at 0% is a mod whose graphs are all raw-body nodes.
