What is in this folder, and why it ships.

SUDO DayZ Node Mod itself is under the MIT licence. Its text is in LICENSE, one
level up from here. Nothing in this folder covers the application; these are the
libraries that sit beside it in the same install folder.

Qt-LICENSE.txt
    The Qt libraries beside the executable (Qt6Core.dll, Qt6Gui.dll,
    Qt6Widgets.dll, Qt6Network.dll, Qt6Svg.dll and Qt6Pdf.dll where present)
    and every Qt plugin in the subfolders next to them: platforms\, styles\,
    iconengines\, imageformats\, generic\, tls\ and networkinformation\. Used
    under the GNU Lesser General Public License version 3, whose text this file
    contains along with the GPL version 3 it supplements.

    Qt is linked dynamically and its DLLs are ordinary files in the install
    folder. Replacing them with your own build of the same Qt version is the
    relinking right the LGPL reserves for you, and it needs no cooperation from
    this application. Qt's own sources are published at https://download.qt.io.

GPLv3.txt
    The GPL version 3, as the licence the following exception modifies.

GCC-Runtime-Library-Exception.txt
    libgcc_s_seh-1.dll and libstdc++-6.dll, the GCC runtime libraries a MinGW
    build needs on a machine with no compiler installed. Distributed under the
    GCC runtime library exception, which is what allows them to travel with a
    program that is not itself under the GPL.

MinGW-w64-runtime-COPYING.txt
    The mingw-w64 runtime, which is what the build links against for the C
    library. Permissive.

winpthreads-COPYING.txt
    libwinpthread-1.dll. Permissive, from the same project.

Three files this folder deliberately has no text for, because they are not in
the install:

    opengl32sw.dll        Mesa 3D llvmpipe, the software OpenGL fallback The
                          Qt Company ships with Qt.
    D3Dcompiler_47.dll    Microsoft, from the Windows SDK redistributables.
    dxcompiler.dll        Microsoft DirectX Shader Compiler.

    windeployqt copies all three by default and the deploy line turns all three
    off. None of them is Qt code, each carries redistribution terms of its own,
    and none of them is loaded by this application: it is a Qt Widgets program
    with no QOpenGLWidget, no Qt Quick and no shader compilation, and its canvas
    paints through the raster engine. Leaving them out removes about eighteen
    megabytes and three sets of conditions that were never owed. The packaged
    application is started with Qt taken out of PATH at the end of every release
    run, which is what turns that from an argument into a test.

No part of the Qt Installer Framework is here, and none of it ships. The reason
is written down in packaging/README.md in the source tree.
