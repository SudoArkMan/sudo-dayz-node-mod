cmake_minimum_required(VERSION 3.16)

# The version, and the only place in the tree it is written down. A release
# bumps this line and nothing else. Semantic versioning, MAJOR.MINOR.PATCH,
# because that is what parseVersion in src/update.h orders and what the release
# tag vMAJOR.MINOR.PATCH carries.
project(DAYZSUDONodeMod VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# One version, everywhere
# ---------------------------------------------------------------------------
#
# project() gives three numbers. A pre-release needs a suffix that project()
# will not accept, so NODEMOD_VERSION_SUFFIX carries it and the two are joined
# here. -DNODEMOD_VERSION_SUFFIX=-rc.1 turns 0.2.0 into 0.2.0-rc.1 without
# touching a source file.
set(NODEMOD_VERSION_SUFFIX "" CACHE STRING
    "Appended to the project version, for example -rc.1. Empty for a release.")
set(NODEMOD_VERSION "${PROJECT_VERSION}${NODEMOD_VERSION_SUFFIX}")

set(NODEMOD_PRODUCT     "SUDO DayZ Node Mod")
set(NODEMOD_VENDOR      "SudoArkMan")
set(NODEMOD_DESCRIPTION "Visual scripting for DayZ Enforce Script")
set(NODEMOD_COPYRIGHT   "Copyright 2026 SudoArkMan")

# Where the update check looks when the settings file says nothing. Cache
# entries rather than constants in the middle of a URL, so a fork points itself
# somewhere else without editing code.
set(NODEMOD_UPDATE_OWNER "SudoArkMan" CACHE STRING
    "GitHub owner the update check reads releases from.")
set(NODEMOD_UPDATE_REPO "sudo-dayz-node-mod" CACHE STRING
    "GitHub repository the update check reads releases from.")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

# Directory scope on purpose. Every target here and in tests/ may include
# <version.h>, and putting it on each of the five target blocks instead would
# mean five edits every time one of them is touched.
include_directories("${CMAKE_CURRENT_BINARY_DIR}/generated")

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets Network)

# Precompiled headers. QString and the Qt containers are most of what any
# translation unit here parses and they are the same in all of them, so each
# tier precompiles the set its own Qt module allows. A tier gets no header from
# a module it does not link, or the include path for it would not be there.
set(NODEMOD_PCH_CORE
  "<QByteArray>" "<QDir>" "<QFile>" "<QFileInfo>" "<QHash>" "<QMap>"
  "<QRegularExpression>" "<QSet>" "<QString>" "<QStringList>" "<QVector>"
  "<QJsonArray>" "<QJsonDocument>" "<QJsonObject>"
)
set(NODEMOD_PCH_GUI ${NODEMOD_PCH_CORE} "<QColor>" "<QFont>")
set(NODEMOD_PCH_WIDGETS ${NODEMOD_PCH_GUI} "<QPainter>" "<QWidget>")

# Sources shared by the app and the tests, compiled once each rather than once
# per target that names them.
#
# The split into four is load bearing, not tidiness. Several console tests link
# pins.cpp and never theme.cpp, which is the whole reason branding.h is a
# header-only palette; one library holding everything would put Qt Widgets back
# into those tests and throw that away. So each file sits in the lowest tier
# that can satisfy it, and a tier links only the Qt modules its own sources
# need. The boundary between model and themed is a call into theme.cpp, so it
# does not follow the directory names: enforce/layout.cpp asks theme for font
# metrics and is therefore a Widgets file.
#
#   nodemod_core     Qt Core, nothing further
#   nodemod_model    adds Qt Gui, reaches no further than QColor
#   nodemod_themed   Qt Widgets, because it calls theme.cpp
#   nodemod_panels   the widgets and panels more than one target builds on
#
# The tiers do not link each other. A consumer names every tier it needs, so
# nothing depends on whether CMake carries object files across a chain of
# object libraries.

# What lexertest, parsertest, configtest, pbotest and templatetest link, and the
# set that keeps all five clear of Qt Gui.
add_library(nodemod_core OBJECT
  src/enforce/lexer.h        src/enforce/lexer.cpp
  src/enforce/ast.h          src/enforce/parser.cpp
  src/config/configtree.h    src/config/configtree.cpp
  src/pbo/pboreader.h        src/pbo/pboreader.cpp
  src/workdrive.h            src/workdrive.cpp
  src/modtemplate.h          src/modtemplate.cpp
)
target_include_directories(nodemod_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nodemod_core PUBLIC Qt${QT_VERSION_MAJOR}::Core)
target_precompile_headers(nodemod_core PRIVATE ${NODEMOD_PCH_CORE})

# Qt Gui and no further. pins.h carries a QColor so this tier cannot be Core,
# but nothing in it calls theme.cpp and nothing in it may start to: coretest,
# crashtest and eventstest link the whole tier and have to stay off Widgets.
add_library(nodemod_model OBJECT
  src/pins.h        src/pins.cpp
  src/graph.h       src/graph.cpp
  src/catalog.h     src/catalog.cpp
  src/project.h     src/project.cpp
  src/builtins.h    src/builtins.cpp
  src/scriptapi.h   src/scriptapi.cpp
  src/templates.h   src/templates.cpp
  src/codegen.h     src/codegen.cpp
  src/analysis.h    src/analysis.cpp
  src/events.h      src/events.cpp
  src/nodeindex.h   src/nodeindex.cpp
  src/document.h    src/document.cpp
  src/recentprojects.h src/recentprojects.cpp
)
target_include_directories(nodemod_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nodemod_model PUBLIC
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
)
target_precompile_headers(nodemod_model PRIVATE ${NODEMOD_PCH_GUI})

# The model layer that reaches theme.cpp, directly or one call down. lower.cpp
# and import.cpp are here because they run the layout pass, and modlibrary.cpp
# because it runs the importer, so none of the three can sit with the model tier
# without dragging Widgets in behind it.
add_library(nodemod_themed OBJECT
  src/theme.h       src/theme.cpp
  src/moddeps.h     src/moddeps.cpp
  src/modlibrary.h  src/modlibrary.cpp
  src/testrun.h     src/testrun.cpp
  src/enforce/lower.h       src/enforce/lower.cpp
  src/enforce/layout.h      src/enforce/layout.cpp
  src/enforce/import.h      src/enforce/import.cpp
  src/enforce/highlighter.h src/enforce/highlighter.cpp
)
target_include_directories(nodemod_themed PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nodemod_themed PUBLIC
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
  Qt${QT_VERSION_MAJOR}::Widgets
)
target_precompile_headers(nodemod_themed PRIVATE ${NODEMOD_PCH_WIDGETS})

# Widgets and panels a test links so the thing under test is the one the app
# ships rather than a copy that can drift.
#
# branding.cpp and widgets/startpage.cpp are deliberately absent. startpage.cpp
# is the only caller of branding.cpp, and branding.h says in as many words that
# theme.cpp is compiled into targets that link no branding.cpp. Putting either
# here would hand branding.cpp to nine targets that do without it today and
# quietly retire that rule. Both are compiled twice, which is what they cost
# before this change too.
add_library(nodemod_panels OBJECT
  src/widgets/valueeditor.h     src/widgets/valueeditor.cpp
  src/widgets/codeeditor.h      src/widgets/codeeditor.cpp
  src/widgets/configeditor.h    src/widgets/configeditor.cpp
  src/widgets/newscriptdialog.h src/widgets/newscriptdialog.cpp
  src/panels/modbrowser.h       src/panels/modbrowser.cpp
  src/panels/testpanel.h        src/panels/testpanel.cpp
)
target_include_directories(nodemod_panels PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nodemod_panels PUBLIC
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
  Qt${QT_VERSION_MAJOR}::Widgets
)
target_precompile_headers(nodemod_panels PRIVATE ${NODEMOD_PCH_WIDGETS})

# Version ordering, the changelog, and asking GitHub what has been released.
# A tier of its own because it is the only thing in the app that links Qt
# Network, and the seventeen test targets that do without it should carry on
# doing without it. Nothing here reaches Qt Gui, so the tier stops at Core.
add_library(nodemod_update OBJECT
  src/update.h      src/update.cpp
)
target_include_directories(nodemod_update PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nodemod_update PUBLIC
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Network
)
target_precompile_headers(nodemod_update PRIVATE ${NODEMOD_PCH_CORE})

# The .qrc carries the icon and artwork inside the binary; this carries the icon
# on the binary, which is a separate thing Windows reads without running it.
if(WIN32)
  enable_language(RC)
  set(APP_ICON_RESOURCE_WINDOWS
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/brand/appicon.rc")

  # The Properties tab in Explorer, and what an antivirus report calls the file.
  # An unsigned binary with no version block reads as anonymous, which is the
  # wrong first impression for something people download from a mod community.
  # Separate from appicon.rc so a version bump never touches the artwork.
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/packaging/version.rc.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc" @ONLY)
  set(APP_VERSION_RESOURCE_WINDOWS
      "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
endif()

# Only what nothing else links. Everything shared arrives through the four
# object libraries below.
add_executable(DAYZSUDONodeMod
  main.cpp
  resources/brand.qrc
  ${APP_ICON_RESOURCE_WINDOWS}
  ${APP_VERSION_RESOURCE_WINDOWS}
  src/branding.h    src/branding.cpp
  src/widgets/startpage.h    src/widgets/startpage.cpp
  src/widgets/splashscreen.h src/widgets/splashscreen.cpp
  src/nodeinputs.h  src/nodeinputs.cpp
  src/widgets/newmoddialog.h src/widgets/newmoddialog.cpp
  src/widgets/codedialog.h   src/widgets/codedialog.cpp
  src/widgets/filedialog.h   src/widgets/filedialog.cpp
  src/canvas/nodeitem.h      src/canvas/nodeitem.cpp
  src/canvas/wireitem.h      src/canvas/wireitem.cpp
  src/canvas/noteitem.h      src/canvas/noteitem.cpp
  src/canvas/nodescene.h     src/canvas/nodescene.cpp
  src/canvas/nodeview.h      src/canvas/nodeview.cpp
  src/canvas/minimapwidget.h src/canvas/minimapwidget.cpp
  src/panels/palettepanel.h    src/panels/palettepanel.cpp
  src/panels/eventspanel.h     src/panels/eventspanel.cpp
  src/panels/outlinerpanel.h   src/panels/outlinerpanel.cpp
  src/panels/variablespanel.h  src/panels/variablespanel.cpp
  src/panels/inspectorpanel.h  src/panels/inspectorpanel.cpp
  src/panels/codeviewpanel.h   src/panels/codeviewpanel.cpp
  src/panels/explorerpanel.h   src/panels/explorerpanel.cpp
  src/mainwindow.h  src/mainwindow.cpp
)

target_include_directories(DAYZSUDONodeMod PRIVATE src)
target_link_libraries(DAYZSUDONodeMod PRIVATE
  nodemod_core
  nodemod_model
  nodemod_themed
  nodemod_panels
  nodemod_update
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::Network
)
target_precompile_headers(DAYZSUDONodeMod PRIVATE ${NODEMOD_PCH_WIDGETS})

add_subdirectory(tests)

# ---------------------------------------------------------------------------
# The release artifact
# ---------------------------------------------------------------------------
#
# The install tree is what a stranger unzips, so it is flat rather than
# bin/lib/share: DAYZSUDONodeMod.exe at the top with the Qt DLLs beside it, and
# resources/ one level down where the app already looks for it. GNUInstallDirs
# would put the exe in bin/ and the app would still find resources/ by walking
# up, but the folder would then need explaining and this one does not.
#
# Nothing here is a step somebody has to remember. `cmake --install` runs
# windeployqt itself, so a build that skips it cannot happen; the failure it
# prevents is "Qt6Core.dll was not found", which is what a bare exe does when it
# leaves this machine.

set(NODEMOD_ARTIFACT_NAME "SUDO-DayZ-Node-Mod-${NODEMOD_VERSION}-win64")

install(TARGETS DAYZSUDONodeMod RUNTIME DESTINATION .)

# Read at runtime, by path, from beside the executable. catalog.json is the one
# the app refuses to start without. Showcase.sdzn is the sample the start page
# offers. SUDO_Link.sdzn is a test fixture and stays out, and so does the
# .autosave beside it: shipping a stray autosave would have the app offer to
# recover work the user never did.
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/catalog.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/known-mods.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/Showcase.sdzn"
    DESTINATION resources)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/resources/mod-template"
        DESTINATION resources)

# resources/brand is deliberately absent: brand.qrc compiles it into the binary,
# so a copy on disk would be the same artwork twice.

# The licences of what ships beside the app. Qt is LGPLv3 here and the LGPL
# wants its text carried with the binaries; the GCC runtime DLLs want the
# runtime library exception carried with theirs.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/packaging/licenses/"
        DESTINATION licenses)

# The per-user install and its uninstaller, which is what makes the ZIP an
# install rather than a folder somebody has to file themselves.
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/Install.cmd"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/install.ps1"
    DESTINATION .)

# The MinGW runtime, alongside what windeployqt --compiler-runtime copies.
# Naming them is not belt and braces for its own sake: --compiler-runtime finds
# the compiler through the environment, and the environment at install time is
# not the environment at build time. These three paths are resolved from the
# compiler that actually built the binary, so they cannot be the wrong ones.
get_filename_component(NODEMOD_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
set(NODEMOD_RUNTIME_DLLS)
foreach(dll libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
  if(EXISTS "${NODEMOD_MINGW_BIN}/${dll}")
    list(APPEND NODEMOD_RUNTIME_DLLS "${NODEMOD_MINGW_BIN}/${dll}")
  endif()
endforeach()
if(NODEMOD_RUNTIME_DLLS)
  install(FILES ${NODEMOD_RUNTIME_DLLS} DESTINATION .)
endif()

# The top level documents. Checked at install time rather than at configure
# time because CHANGELOG.md is what the What's new panel reads and it may be
# written after this file was configured; a configure-time if(EXISTS) would then
# ship an install with no changelog and say nothing about it.
install(CODE "
  foreach(doc LICENSE CHANGELOG.md README.md THIRD-PARTY-NOTICES.md)
    if(EXISTS \"${CMAKE_CURRENT_SOURCE_DIR}/\${doc}\")
      file(INSTALL \"${CMAKE_CURRENT_SOURCE_DIR}/\${doc}\"
           DESTINATION \"\${CMAKE_INSTALL_PREFIX}\")
    else()
      message(WARNING \"\${doc} is not in the tree, so it is not in the package.\")
    endif()
  endforeach()
")

# windeployqt, found next to the qmake of the Qt this build configured against,
# so a machine with several Qt installs cannot deploy the wrong one.
if(WIN32)
  get_target_property(NODEMOD_QMAKE Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
  get_filename_component(NODEMOD_QT_BIN "${NODEMOD_QMAKE}" DIRECTORY)
  find_program(NODEMOD_WINDEPLOYQT
               NAMES windeployqt6 windeployqt
               HINTS "${NODEMOD_QT_BIN}"
               DOC "windeployqt of the Qt this build uses.")
  if(NOT NODEMOD_WINDEPLOYQT)
    message(FATAL_ERROR
      "windeployqt was not found next to ${NODEMOD_QMAKE}. Without it the "
      "package would be an executable with no Qt beside it, which starts on "
      "this machine and nowhere else.")
  endif()

  # --compiler-runtime because a MinGW build needs libstdc++ and libgcc, and a
  # machine with no toolchain has neither.
  #
  # --no-translations because the app has no translations to go with them.
  #
  # The three exclusions are the only ones taken, and they are taken for what
  # they cost in licence terms rather than in megabytes. windeployqt copies
  # opengl32sw.dll (Mesa llvmpipe), D3Dcompiler_47.dll and dxcompiler.dll by
  # default, and each carries redistribution terms of its own that would then
  # have to travel with every release. This is a Qt Widgets program: no
  # QOpenGLWidget, no Qt Quick, no shader compilation, and QGraphicsView paints
  # through the raster engine. None of the three is loaded, so none of them is
  # worth the paperwork. The packaged application is started with Qt off PATH
  # at the end of tools/release.ps1, which is where that claim gets tested
  # rather than assumed.
  install(CODE "
    message(STATUS \"Deploying Qt into \${CMAKE_INSTALL_PREFIX}\")
    execute_process(
      COMMAND \"${NODEMOD_WINDEPLOYQT}\"
              --compiler-runtime
              --no-translations
              --no-opengl-sw
              --no-system-d3d-compiler
              --no-system-dxc-compiler
              --verbose 1
              \"\${CMAKE_INSTALL_PREFIX}/DAYZSUDONodeMod.exe\"
      RESULT_VARIABLE deploy_result)
    if(NOT deploy_result EQUAL 0)
      message(FATAL_ERROR \"windeployqt failed with \${deploy_result}.\")
    endif()
  ")
endif()

# A self-contained folder without going near cpack, for the run that just wants
# to look at it. Same install rules, so it is the same folder the ZIP holds.
set(NODEMOD_STAGE_DIR "${CMAKE_BINARY_DIR}/stage/${NODEMOD_ARTIFACT_NAME}")
add_custom_target(stage
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${NODEMOD_STAGE_DIR}"
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
          --prefix "${NODEMOD_STAGE_DIR}"
  DEPENDS DAYZSUDONodeMod
  COMMENT "Staging ${NODEMOD_STAGE_DIR}"
  VERBATIM USES_TERMINAL)

# ---------------------------------------------------------------------------
# CPack
# ---------------------------------------------------------------------------
#
# ZIP and only ZIP. The installer question is answered in packaging/README.md:
# Qt Installer Framework is one of Qt's tools, the tools are GPLv3, and the
# installer binarycreator produces carries the GPLv3 installerbase inside it, so
# shipping one would put a GPLv3 executable in the releases of an MIT project.
# The per-user install script does the same job with no licence attached.
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "SUDO-DayZ-Node-Mod")
set(CPACK_PACKAGE_VENDOR "${NODEMOD_VENDOR}")
set(CPACK_PACKAGE_VERSION "${NODEMOD_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${NODEMOD_DESCRIPTION}")
set(CPACK_PACKAGE_FILE_NAME "${NODEMOD_ARTIFACT_NAME}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "${NODEMOD_PRODUCT}")
set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts")
# One folder inside the archive rather than fifty loose files, so unzipping into
# Downloads does not scatter Qt across it.
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_SOURCE_GENERATOR "")
include(CPack)
