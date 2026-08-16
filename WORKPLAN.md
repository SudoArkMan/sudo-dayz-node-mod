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
offline is the expensive way to learn it: a server-only mod chain (`-serverMod=`) is not applied,
anything that depends on a second client cannot happen, and COT's permissions need the per-player
files a server writes on first boot. Verify that list against the tooling before printing it as
fact rather than repeating it from here.

## PBO notes

The format is a header of entries (filename, packing method, original size, reserved, timestamp,
data size), a zero-length terminating entry, then the data blobs back to back in the same order,
then a 20 byte SHA1 trailer. DayZ ships them uncompressed in practice. A native reader is preferred
over shelling out to `BankRev.exe`: it works headless in tests, has no install dependency, and the
254 installed mods are the corpus to prove it against. Keep `BankRev.exe` as a fallback for anything
the reader refuses.
