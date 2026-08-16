# Third party notices

SUDO DayZ Node Mod is published under the MIT License, in `LICENSE`. That
covers the code written for this project. It does not cover the libraries the
application links against, the runtime files a Windows release ships beside the
executable, or the third party content bundled under `resources/`. Those are
listed here, each with the licence it carries and what that licence asks of a
release.

Every claim below was checked against files on the build machine rather than
recalled. Where something could not be checked from disk, this file says so
instead of guessing.

## Qt

| | |
| --- | --- |
| Version | Qt 6.11.0, MinGW 13.1 build, `C:/Qt/6.11.0/mingw_64` |
| Copyright | The Qt Company Ltd and contributors |
| Licence used here | GNU Lesser General Public License, version 3 |
| Licence text | `C:/Qt/Licenses/LICENSE` in a Qt install, and `LGPL-3.0-only.txt` plus `GPL-3.0-only.txt` under `Src/qtbase/LICENSES/` |
| Home | https://www.qt.io |

Qt's own licence file states that most Qt functionality is available under
LGPLv3, and that the tools and some add-on components are available only under
GPLv3. This project uses no GPL-only component. What it links and what a
release ships is listed below.

### Modules the application links

`CMakeLists.txt` names four Qt modules and nothing else:

    find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

All four are LGPLv3. `objdump -p` on the release build names all four in the
import table and nothing else from Qt:

    Qt6Core.dll  Qt6Gui.dll  Qt6Network.dll  Qt6Widgets.dll

`Qt6Network.dll` is there because `src/update.cpp` uses `QNetworkAccessManager`
to ask the GitHub releases API whether a newer version has been published.
Re-run `objdump -p` on the release build and confirm the import table still
names only these four before publishing.

### Modules a Windows release also ships

`windeployqt` copies more than the import table, because it also resolves the
Qt plugins the application could load at runtime. The deploy line in
`CMakeLists.txt` is what a release runs, and against the release build in this
repository it produces 22 files totalling 49.6 MB. The table below is that
list.

Left alone, `windeployqt` would copy 25 files and 93.6 MB. The difference is
three binaries that are not Qt code and that this application never loads:
`opengl32sw.dll` (Mesa 3D llvmpipe, 19.7 MB), `D3Dcompiler_47.dll` (4.0 MB) and
`dxcompiler.dll` (20.4 MB). `--no-opengl-sw`, `--no-system-d3d-compiler` and
`--no-system-dxc-compiler` turn them off, which is 44 MB and three sets of
third party terms that are then not owed at all.

Without `--no-translations` it also copies `translations/qt_*.qm`, 32 Qt
message catalogues under the same LGPLv3 as the libraries they belong to. The
deploy line in `RELEASING.md` excludes them because the application ships no
translations of its own, so they would localise Qt's standard dialogs inside an
otherwise English interface. If that line ever changes, they are redistributed
Qt material and the licence text already covers them.

Re-measure at release time. A Qt point release moves these sizes, and a new
`#include` can pull in a module nobody intended to ship.

| File | Size | Licence |
| --- | --- | --- |
| `Qt6Core.dll` | 10.8 MB | LGPLv3 |
| `Qt6Gui.dll` | 10.9 MB | LGPLv3 |
| `Qt6Widgets.dll` | 6.8 MB | LGPLv3 |
| `Qt6Svg.dll` | 0.6 MB | LGPLv3 |
| `Qt6Network.dll` | 1.9 MB | LGPLv3 |
| `Qt6Pdf.dll` | 8.0 MB | see the note below |
| `platforms/qwindows.dll` | | LGPLv3 |
| `styles/qmodernwindowsstyle.dll` | | LGPLv3 |
| `iconengines/qsvgicon.dll` | | LGPLv3 |
| `imageformats/qgif.dll`, `qico.dll`, `qjpeg.dll`, `qsvg.dll`, `qpdf.dll` | | LGPLv3, except `qpdf.dll` |
| `generic/qtuiotouchplugin.dll` | | LGPLv3 |
| `networkinformation/qnetworklistmanager.dll` | | LGPLv3 |
| `tls/qcertonlybackend.dll`, `tls/qschannelbackend.dll` | | LGPLv3 |
| `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll` | 2.3 MB | see "MinGW runtime" below |

Qt plugins are separate DLLs loaded at runtime, so they are dynamically linked
in the sense LGPLv3 cares about, the same as the core libraries.

Note on `Qt6Pdf.dll`: Qt PDF comes from the `qtwebengine` repository, which is
not installed on this machine, so its licence could not be read from disk the
way the others were. It is reached only through `imageformats/qpdf.dll`, and
this application contains no PDF code: a search of `src/` and `main.cpp` for
`QtPdf` and `qpdf` returns nothing. The same search finds no `QSvg` use, and
`resources/brand.qrc` carries PNG artwork only.

**`Qt6Pdf.dll` is in the release as the deploy line stands.** The line drops
the three non-Qt binaries but does not pass `--skip-plugin-types imageformats`,
so `imageformats/qpdf.dll` is still deployed and still pulls `Qt6Pdf.dll` in
behind it. Adding that exclusion removes both and closes the one licence
question this file cannot answer from disk. It is decision 3 in `RELEASING.md`,
and until it is settled a release ships one component whose licence this file
does not vouch for.

The `tls` and `networkinformation` plugin types have to stay. The update check
speaks HTTPS to `api.github.com`, and without `tls/qschannelbackend.dll` that
request fails at the handshake rather than loudly.

### What LGPLv3 asks of a release

Shipping the Qt DLLs is redistribution of Qt, so these apply to any release
that includes them.

1. **Keep Qt dynamically linked.** A user must be able to replace the Qt
   libraries with their own build and still run the application. That is true
   today: the DLLs sit beside the executable and are resolved at load time.
   Do not switch to a static Qt build, and do not link Qt with
   `-static-libgcc`-style whole-archive tricks. A static link would put this
   project's own source under the LGPLv3 relink obligation, which MIT alone
   does not satisfy.

2. **Say that the application uses Qt, and under which licence.** The release
   notes and the shipped documentation both need to state that this application
   uses the Qt framework under LGPLv3, name the Qt version, and point at the
   licence text.

3. **Ship the licence text.** Include a copy of the LGPLv3 (and the GPLv3 it
   supplements, since LGPLv3 is written as a set of additional permissions on
   top of it) in the release archive. Copy `LICENSE`, `LGPL-3.0-only.txt` and
   `GPL-3.0-only.txt` out of the Qt install rather than fetching a copy from
   elsewhere, so the text matches the version being shipped.

4. **Say where Qt came from.** Name the exact Qt version and give the user a
   way to obtain the corresponding source, which for an unmodified upstream Qt
   is a pointer to https://download.qt.io.

5. **If Qt is ever modified, ship those modifications.** No Qt source is
   modified here. If a patched Qt is ever used for a release, the patch has to
   ship with it under LGPLv3.

6. **Use no GPL-only Qt component.** Checked: the three linked modules and
   every module `windeployqt` pulled in are LGPLv3, with `Qt6Pdf.dll` the one
   left open above and excluded by the recommended deploy line.

Using Qt Creator, `windeployqt`, `moc`, `uic` and `rcc` as build tools does not
affect the licence of what they produce. Those tools are not shipped. Shipping
the Qt runtime DLLs is what makes a release a redistribution of Qt.

## MinGW runtime

`windeployqt` copies three runtime DLLs from the MinGW toolchain. The
executable's import table names two of them directly (`libgcc_s_seh-1.dll`,
`libstdc++-6.dll`); the third arrives as a dependency of the other two.

| File | Project | Licence |
| --- | --- | --- |
| `libgcc_s_seh-1.dll` | GCC runtime | GPLv3 with the GCC Runtime Library Exception |
| `libstdc++-6.dll` | GCC libstdc++ | GPLv3 with the GCC Runtime Library Exception |
| `libwinpthread-1.dll` | mingw-w64 winpthreads | MIT-style permissive licence, plus code under the zope and ANSI C headers licences used by mingw-w64 |

The GCC Runtime Library Exception is what allows a program compiled with GCC to
ship the GCC runtime without becoming GPL. It applies here: this project is
compiled with an unmodified GCC through an unmodified toolchain, which is
exactly the case the exception covers. Include the GCC Runtime Library
Exception text and the mingw-w64 licence in the release archive alongside the
Qt licences.

## Mesa and Microsoft runtime files

These three arrive through `windeployqt` and are not Qt code.

| File | Origin | Licence |
| --- | --- | --- |
| `opengl32sw.dll` | Mesa 3D `llvmpipe` software rasteriser, shipped by The Qt Company as an OpenGL fallback | MIT |
| `D3Dcompiler_47.dll` | Microsoft, part of the Windows SDK / DirectX redistributable set | Microsoft's redistribution terms for the Windows SDK |
| `dxcompiler.dll` | Microsoft DirectX Shader Compiler | Microsoft's terms; the upstream open source project is under the University of Illinois / NCSA Open Source License |

All three are optional for this application. It is a Qt Widgets program with no
`QOpenGLWidget`, no Qt Quick and no RHI shader compilation, so nothing here
drives the D3D12 backend that `dxcompiler.dll` serves.

`opengl32sw.dll` is the one to think about before removing: it is the fallback
for a machine with no working OpenGL driver. `RELEASING.md` covers the
trade-off and asks for the release build to be started once on a clean machine
after any exclusion.

## Bundled DayZ mod template

`resources/mod-template/` is a copy of the DayZ Mod Template by Andrew Fong
("InclementDab"), https://github.com/InclementDab/DayZ-Mod-Template. The
application copies it into a new mod folder when scaffolding a project, so it is
both redistributed with the application and redistributed again by every user
who scaffolds a mod.

It carries `resources/mod-template/LICENSE.md`, which is the MIT License with
this copyright line:

    Copyright (c) 2013 Mark Otto.
    Copyright (c) 2017 Andrew Fong.

That file is kept in place, unmodified, and the whole tree is committed with
`-text` in `.gitattributes` so its bytes do not move. Honouring it means:
keeping `LICENSE.md` inside the template so it travels with every copy the
application writes, and keeping the copyright notice above in this file.

`src/modtemplate.cpp` copies the template at byte level, and `templatetest`
checks that the copy is exact apart from the `ModTemplate` name substitution.
That is what keeps `LICENSE.md` in every scaffolded mod.

The template's mission folders (`Missions/ModTemplate.ChernarusPlus`,
`.Enoch`, `.sakhal`) contain DayZ mission configuration derived from Bohemia
Interactive's own mission files. They are data for the game, redistributed here
as part of the upstream template.

## Brand artwork

`resources/brand/` is original artwork for SUDO Servers, produced for this
project. Copyright 2026 Dillan Stephenson. It is not covered by the MIT
License in `LICENSE`: the code is MIT, the marks are not. Forks may use the
code freely and should replace the artwork rather than ship a build that
presents itself as SUDO Servers.

`resources/brand/README.txt` records one exception in the author's own words:
the DayZ artwork inside the splash image is a supplied asset and is Bohemia
Interactive's mark, not part of this identity.

## Node catalogue

`resources/catalog.json` (2.9 MB) is the generated node catalogue: 6,108
classes, 29,024 methods, 403 enums, 362 globals and 578 constants. It is built
by `tools/build-catalog.mjs` from an index of the DayZ script tree at
`P:/scripts`, which is Bohemia Interactive's Enforce Script source, unpacked
from an installed copy of DayZ. The generator reproduces the shipped file byte
for byte.

Alongside names and signatures, the catalogue carries documentation comments
lifted from that source.

**This is an unresolved redistribution question and it is deliberately left
open here.** Committing `catalog.json` redistributes derived content from
Bohemia Interactive's script source under this repository's MIT License, which
is not something this project is in a position to grant.
`RELEASING.md` states the options and the facts for each. Whichever way it is
settled, this section has to be rewritten to match before a release goes out.

## Sample projects

`resources/SUDO_Link.sdzn` and `resources/Showcase.sdzn` are the author's own
node graphs, used as test fixtures. Copyright 2026 Dillan Stephenson, MIT with
the rest of the project. `SUDO_Link.sdzn` is the 25-script project that
`coretest` and `importtest` measure against.

## Known mod metadata

`resources/known-mods.json` records addon names for Community Framework,
Community Online Tools and Dabs Framework, read out of each framework's own
`config.cpp`. These are factual identifiers, not code from those projects.
The frameworks themselves are neither bundled nor linked; the application only
names them so a mod can declare a dependency.

## DayZ and Enfusion

DayZ, Enfusion and Bohemia Interactive are trademarks of Bohemia Interactive
a.s. This project is not affiliated with, endorsed by, or supported by Bohemia
Interactive. It is a third party tool for people writing DayZ mods, and it
reads files that a DayZ or DayZ Tools installation puts on the user's own
machine. No Bohemia Interactive game content is redistributed with the
application, with the catalogue question above the one open item.
