cmake_minimum_required(VERSION 3.16)

project(DAYZSUDONodeMod VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets)

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

# The .qrc carries the icon and artwork inside the binary; this carries the icon
# on the binary, which is a separate thing Windows reads without running it.
if(WIN32)
  enable_language(RC)
  set(APP_ICON_RESOURCE_WINDOWS
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/brand/appicon.rc")
endif()

# Only what nothing else links. Everything shared arrives through the four
# object libraries below.
add_executable(DAYZSUDONodeMod
  main.cpp
  resources/brand.qrc
  ${APP_ICON_RESOURCE_WINDOWS}
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
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
  Qt${QT_VERSION_MAJOR}::Widgets
)
target_precompile_headers(DAYZSUDONodeMod PRIVATE ${NODEMOD_PCH_WIDGETS})

add_subdirectory(tests)

include(GNUInstallDirs)
install(TARGETS DAYZSUDONodeMod
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
