cmake_minimum_required(VERSION 3.16)

project(DAYZSUDONodeMod VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui Widgets)

# The .qrc carries the icon and artwork inside the binary; this carries the icon
# on the binary, which is a separate thing Windows reads without running it.
if(WIN32)
  enable_language(RC)
  set(APP_ICON_RESOURCE_WINDOWS
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/brand/appicon.rc")
endif()

add_executable(DAYZSUDONodeMod
  main.cpp
  resources/brand.qrc
  ${APP_ICON_RESOURCE_WINDOWS}
  src/branding.h    src/branding.cpp
  src/widgets/splashscreen.h src/widgets/splashscreen.cpp
  src/pins.h        src/pins.cpp
  src/nodeinputs.h  src/nodeinputs.cpp
  src/catalog.h     src/catalog.cpp
  src/builtins.h    src/builtins.cpp
  src/graph.h       src/graph.cpp
  src/project.h     src/project.cpp
  src/recentprojects.h src/recentprojects.cpp
  src/moddeps.h     src/moddeps.cpp
  src/modlibrary.h  src/modlibrary.cpp
  src/scriptapi.h   src/scriptapi.cpp
  src/events.h      src/events.cpp
  src/templates.h   src/templates.cpp
  src/document.h    src/document.cpp
  src/modtemplate.h src/modtemplate.cpp
  src/testrun.h     src/testrun.cpp
  src/widgets/newmoddialog.h src/widgets/newmoddialog.cpp
  src/codegen.h     src/codegen.cpp
  src/analysis.h    src/analysis.cpp
  src/theme.h       src/theme.cpp
  src/enforce/lexer.h        src/enforce/lexer.cpp
  src/enforce/ast.h          src/enforce/parser.cpp
  src/enforce/lower.h        src/enforce/lower.cpp
  src/enforce/import.h       src/enforce/import.cpp
  src/enforce/layout.h       src/enforce/layout.cpp
  src/enforce/highlighter.h  src/enforce/highlighter.cpp
  src/config/configtree.h    src/config/configtree.cpp
  src/pbo/pboreader.h        src/pbo/pboreader.cpp
  src/widgets/configeditor.h src/widgets/configeditor.cpp
  src/widgets/valueeditor.h  src/widgets/valueeditor.cpp
  src/widgets/codeeditor.h   src/widgets/codeeditor.cpp
  src/widgets/codedialog.h   src/widgets/codedialog.cpp
  src/widgets/filedialog.h   src/widgets/filedialog.cpp
  src/widgets/newscriptdialog.h src/widgets/newscriptdialog.cpp
  src/widgets/startpage.h    src/widgets/startpage.cpp
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
  src/panels/modbrowser.h      src/panels/modbrowser.cpp
  src/panels/testpanel.h       src/panels/testpanel.cpp
  src/mainwindow.h  src/mainwindow.cpp
)

target_include_directories(DAYZSUDONodeMod PRIVATE src)
target_link_libraries(DAYZSUDONodeMod PRIVATE
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
  Qt${QT_VERSION_MAJOR}::Widgets
)

add_subdirectory(tests)

include(GNUInstallDirs)
install(TARGETS DAYZSUDONodeMod
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
