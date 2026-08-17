# Packaging

What a release is made of, how it is made, and the one licence question that
decided its shape.

## The artifact

One ZIP, `SUDO-DayZ-Node-Mod-<version>-win64.zip`. It unpacks to a single folder
holding the executable, the Qt DLLs and plugins, the MinGW runtime, `resources/`,
the licence texts and an install script. Nothing in it needs Qt on the machine
and nothing in it needs a compiler.

```
SUDO-DayZ-Node-Mod-0.1.0-win64/
    DAYZSUDONodeMod.exe
    Install.cmd
    install.ps1
    LICENSE
    CHANGELOG.md
    README.md
    THIRD-PARTY-NOTICES.md
    Qt6Core.dll  Qt6Gui.dll  Qt6Widgets.dll  Qt6Network.dll  Qt6Svg.dll  Qt6Pdf.dll
    libgcc_s_seh-1.dll  libstdc++-6.dll  libwinpthread-1.dll
    platforms/  styles/  iconengines/  imageformats/  generic/
    tls/  networkinformation/
    licenses/
    resources/
        catalog.json  known-mods.json  Showcase.sdzn  mod-template/
```

157 files, 60 MB unpacked and 22.3 MB in the ZIP. What is not in it is
deliberate: `opengl32sw.dll`,
`D3Dcompiler_47.dll` and `dxcompiler.dll` are copied by `windeployqt` unless
told otherwise, are not Qt code, carry redistribution terms of their own, and
are never loaded by a Qt Widgets program with no OpenGL and no shader
compilation. Leaving them out costs nothing and saves eighteen megabytes and
three sets of conditions.

Running the exe out of that folder is a complete install. `Install.cmd` is for
people who want a Start menu entry and an Add or remove programs entry instead
of a folder in Downloads.

## Why there is no setup.exe

Qt Installer Framework 4.11 is installed on the build machine and would produce
one. It is not used, and the reason is a licence question rather than a
technical one.

**The evidence, from files on this machine.**

`C:/Qt/Licenses/LICENSE`, the licence text the Qt installation ships, says in
the opening section:

> most of the functionality is available under LGPLv3. It should be noted that
> the tools as well as some add-on components are available only under GPLv3.

Qt Installer Framework is one of those tools. It lives under `C:/Qt/Tools/`
beside Qt Creator, CMake, Ninja and the MinGW toolchain.

The Installer Framework package itself carries no licence text at all. Its
installed file list, `C:/Qt/installerResources/qt.tools.ifw.411/4.11.0-0-202603231357ifw-win-x64.txt`,
records 966 files: 576 under `examples/`, 383 under `doc/`, 6 in `bin/` and the
`README`. Twenty-eight of them have a licence-like name and every one of those
sits under `doc/` or `examples/`, so not one is a licence, a COPYING or a notice
for the tool itself. So there is no grant shipped with the tool that would
soften the statement above.

That file is UTF-16 big endian, not text a byte-wise search reads. Searching it
as ASCII finds nothing at all and looks like the same answer, which is the way
this particular check goes wrong. The counts above were taken by decoding it
first. The installed folder agrees with them: `C:/Qt/Tools/QtInstallerFramework/4.11/`
holds `README`, `bin/`, `doc/` and `examples/` and nothing else.

The nearest thing to one is what Qt puts on its other tools. Qt Creator ships
`C:/Qt/Tools/QtCreator/share/qtcreator/LICENSE.GPL3-EXCEPT`, which is GPLv3
annotated with The Qt Company GPL Exception 1.0. Its first exception reads:

> As a special exception you may create a larger work which contains the output
> of this application and distribute that work under terms of your choice, so
> long as the work is not otherwise derived from or based on this application

That exception is what lets a program written in Qt Creator be shipped under any
licence, because Qt Creator's output is your source code and your source code is
not derived from Qt Creator. It does not reach an installer, because
`binarycreator.exe` does not merely emit a file: it produces the installer by
embedding `installerbase.exe`, the framework's own installer runtime, into it.
Both are in `C:/Qt/Tools/QtInstallerFramework/4.11/bin/`, and `installerbase.exe`
is 38 MB of statically linked framework and Qt. An installer built that way is
derived from the application that built it, which is the case the exception
excludes.

**The conclusion.** Shipping an Installer Framework installer would mean putting
a GPLv3 executable into the releases of an MIT project, and taking on GPLv3's
obligation to make the corresponding source of that executable available to
everyone who receives it. The Installer Framework source is not on this machine,
only the binaries. That is a commitment worth making deliberately or not at all,
and it is not worth making for a Start menu shortcut.

So the ZIP ships alone, and the install job is done by `install.ps1`, which is
part of this repository and under the same MIT licence as the rest of it.

Two things follow that are worth writing down rather than rediscovering:

- **Qt itself is fine.** The application links Qt dynamically and ships the
  DLLs as ordinary files beside the executable, which is what LGPLv3 asks for.
  It must stay that way: a static Qt build would turn the same libraries into a
  licensing problem the moment the binary is distributed. `Qt6Core.dll`,
  `Qt6Gui.dll`, `Qt6Widgets.dll` and `Qt6Network.dll` are imports of
  `DAYZSUDONodeMod.exe` and none of them may become anything else.
- **If a real installer is wanted later**, Inno Setup is the clean route. It is
  under a permissive licence that puts no conditions on the installers it
  produces. It is not installed on this machine and nothing here downloads it.
  The install tree that CPack packs is exactly what an Inno `[Files]` section
  would take, so adding it later is a script and not a rebuild.

## How it is built

Everything is driven from `CMakeLists.txt`, so there is no step anybody has to
remember.

`cmake --install` on a configured build tree produces the folder above. It runs
`windeployqt` itself as an install rule, so the DLLs cannot be forgotten. That
is the failure this arrangement exists to prevent: an executable copied on its
own starts on the build machine, where Qt is on `PATH`, and dies with
`Qt6Core.dll was not found` on the machine it was sent to.

| Command | Result |
| --- | --- |
| `cmake --build build/cli --target stage` | the folder, at `build/cli/stage/SUDO-DayZ-Node-Mod-<version>-win64` |
| `cpack -G ZIP --config build/cli/CPackConfig.cmake` | the ZIP, at `build/cli/artifacts/` |
| `pwsh -File tools/release.ps1` | a clean build, the tests, both artifacts, and a verification run |

`tools/release.ps1` is the one to use. It configures a build tree of its own,
builds it, runs every test suite in `tests/` and refuses to package if any one
of them fails, packs the ZIP, then unpacks that ZIP into a temporary folder and
starts the executable there with Qt taken out of `PATH`. A package that only
runs on a machine that already has Qt is the failure that looks like success,
and that last step is the one that catches it.

The suites are found by looking in the build tree rather than by being listed
here, so a new test target is part of the gate the moment it builds and nobody
has to remember to add it.

`-VerifyInstall` adds the other half: it installs from the packaged ZIP into a
scratch folder, checks the shortcut and the Add or remove programs entry, then
uninstalls and checks that all of it is gone. It refuses to run when a real
install is already on the machine, because that install belongs to the user and
is not something a check may take apart.

## The version

The `project(DAYZSUDONodeMod VERSION x.y.z)` call at the top of `CMakeLists.txt`
is the only place a version number is written. A release bumps that line.

From there CMake configures `src/version.h.in` into `<build>/generated/version.h`,
which carries `NODEMOD_VERSION` and `nodemod::kVersion`, and
`packaging/version.rc.in` into the Windows version resource that Explorer's
Properties tab reads. `main.cpp` hands `NODEMOD_VERSION` to
`QCoreApplication::setApplicationVersion`, and the window title, the About box,
the start page and the update check read it back from there. `install.ps1` reads
it off the executable's version resource, so it needs no copy of its own.

Semantic versioning, because that is what `parseVersion` in `src/update.h`
orders and what the release tag `v<version>` carries. A pre-release adds a
suffix without touching the source tree:

    cmake -B build/rc -DNODEMOD_VERSION_SUFFIX=-rc.1 ...

which produces `0.2.0-rc.1`, and the update check ranks it below `0.2.0` the way
semver says it should.

## The release checklist

Four steps, and only the last two leave this machine.

1. **Bump the version.** One line in `CMakeLists.txt`. Nothing else.
2. **Write the changelog section.** `## [x.y.z] - YYYY-MM-DD` in `CHANGELOG.md`,
   in the shape `parseChangelog` in `src/update.h` reads. The release run refuses
   to package when the version being built has no section, because the What is
   new panel on the start page reads that file out of the install folder and a
   package without one ships a blank panel. This is also what the update check
   shows people who have an older version.
3. **Run the release.** From a clean tree:

       pwsh -File tools/release.ps1 -VerifyInstall

   It builds, gates on the tests, packs, and proves the package runs with Qt off
   `PATH`. It writes `build/release/artifacts/SUDO-DayZ-Node-Mod-<version>-win64.zip`
   and publishes nothing.
4. **Tag and publish, by hand.** `git tag v<version>`, push it, create the
   release and attach that one ZIP. The tag has to match the version, because
   the update check compares the tag it reads from GitHub against the version
   the running build carries, and a mismatch is either an update that never
   appears or one that appears forever.

The update check reads `https://api.github.com/repos/SudoArkMan/sudo-dayz-node-mod/releases`
by default, which is the repository this version resource names. Both halves are
CMake cache entries, `NODEMOD_UPDATE_OWNER` and `NODEMOD_UPDATE_REPO`, so a fork
repoints itself at configure time.
