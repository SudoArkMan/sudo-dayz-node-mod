# Engineering notes

Working notes kept because the measurements in them are the reason parts of this project are the
way they are. Not a manual. [../getting-started.md](../getting-started.md) is the manual, and
[../../CONTRIBUTING.md](../../CONTRIBUTING.md) is the build.

## The reference machine

What the numbers below were measured on. Nothing in the application depends on these paths; they
are here so a figure can be read in context.

| | |
| --- | --- |
| Qt | 6.11.0 MinGW 13.1, `C:/Qt/6.11.0/mingw_64` |
| Build | `cmake -S . -B build/cli -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/mingw_64 -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe` |
| Work drive | `P:\` mounted, holding `DZ`, `Mods` and `Core` |
| DayZ Tools | under the Steam library, `DayZ Tools/Bin` |
| PBO extractor | `DayZ Tools/Bin/PboUtils/BankRev.exe`, and `FileBank.exe` to pack |
| Diag client and server | `DayZDiag_x64.exe` from the DayZ install, and the DayZServer install beside it |
| Installed mods | 254 under the DayZ install's `!Workshop` |
| Frameworks on disk | Community Framework and Dabs Framework unpacked locally, addon names recorded in `resources/known-mods.json` |

The DayZ Tools registry key `HKCU\SOFTWARE\Bohemia Interactive\Dayz Tools` is not set on this
machine, so nothing in the application may depend on it alone. That is why the Test dock carries a
"Set DayZ Tools folder..." action.

## Traps in this codebase

Five things that have each cost real time here, written down so they cost it once.

- A running `DAYZSUDONodeMod.exe` holds its own binary. The link fails, **Ninja stops before
  relinking the test targets**, and every measurement after that reads a stale binary that still
  passes. Close the application before building, and do not trust numbers from a build that failed.
- A pin's `def` is emitted code. Deciding whether a pin gets an inline field through
  `inlineEditorFor` therefore changes generated output. The field rule is `fieldEditorFor` in
  `src/nodeinputs.h`, and both the painter and the click handler have to use it.
- Quoting belongs where the user types, not where code is emitted: the lowering legitimately puts
  real identifiers into pins, so quoting in the generator turns working code into `Print("someVar")`.
- Test targets must name sources inside the repository. A target pointing at a scratch folder
  breaks configure for everybody else the moment that folder is cleaned.
- `QGuiApplication::instance()` resolves to the inherited `QCoreApplication::instance()`, so a
  console test thinks a GUI exists and `QFontMetrics` fail-fasts with 0xC0000602. Guard with
  `qobject_cast<QGuiApplication *>(...)`.

## Two ways to test a mod, and the application needs both

The template can launch **offline** for a quick look, but not everything works there, so testing on
a **dev server** has to be a first-class choice rather than the only one or an afterthought.

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
- **`-serverMod=` is real, and it is a dev-server-only difference.** It was dropped once as
  unconfirmed. The evidence is on this machine and the citations are on `withServerModChain` in
  `testrun.h`: `DayZServer\server_manager\Server_manager.ps1:1014` hands it to `DayZServer_x64.exe`
  beside `-mod=`, and the same folder ships `example_mod_list.txt` and `example_server_mod_list.txt`
  as two disjoint lists. The app now passes it to the diag server and to nothing else, so a mod
  marked server only is on the server's command line, off the client's, and off the offline one,
  which `offlineLimits()` says in as many words.

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
read. Its remaining use is as a second opinion in the test, not in the application.

`modlibrarytest`: 266 mods scanned, 1887 pbos, 0 refused. On a 1-in-6 sample of the 216 mods that
ship script, **23% of methods become node graphs** and the rest keep their text, well below the 64%
seen on this project's own graphs. Third-party mod code is the harder corpus and that number is the
one to move; it is per-method, so a mod at 0% is a mod whose graphs are all raw-body nodes.
