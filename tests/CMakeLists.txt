# Headless model-layer test. Runs without a display:
#   cmake --build . --target coretest && ./tests/coretest ../resources
add_executable(coretest
  coretest.cpp
  ../src/pins.cpp
  ../src/graph.cpp
  ../src/catalog.cpp
  ../src/project.cpp
  ../src/builtins.cpp
  ../src/scriptapi.cpp
  ../src/templates.cpp
  ../src/codegen.cpp
  ../src/analysis.cpp
  ../src/enforce/lexer.cpp
)
target_include_directories(coretest PRIVATE ../src)
target_link_libraries(coretest PRIVATE
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
)

# The four crash repros from the audit: cycles, nesting depth and Sequence
# fan-out all used to take the process down.
add_executable(crashtest
  crashtest.cpp
  ../src/pins.cpp
  ../src/graph.cpp
  ../src/catalog.cpp
  ../src/project.cpp
  ../src/builtins.cpp
  ../src/codegen.cpp
  ../src/analysis.cpp
  ../src/scriptapi.cpp
  ../src/templates.cpp
  ../src/enforce/lexer.cpp
)
target_include_directories(crashtest PRIVATE ../src)
target_link_libraries(crashtest PRIVATE
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
)

# Event ranking. Proves ItemBase's 227 events all survive the reorder, that the
# curated names still exist in the catalogue, and that debug and deprecated
# entries sink instead of disappearing:
#   ./tests/eventstest ../resources
add_executable(eventstest
  eventstest.cpp
  ../src/pins.cpp
  ../src/graph.cpp
  ../src/catalog.cpp
  ../src/events.cpp
)
target_include_directories(eventstest PRIVATE ../src)
target_link_libraries(eventstest PRIVATE
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
)

# Mod scaffolding. Copies the bundled template into a QTemporaryDir and proves
# the ModTemplate token is gone from every name and every body.
add_executable(templatetest
  templatetest.cpp
  ../src/modtemplate.cpp
)
target_include_directories(templatetest PRIVATE ../src)
target_link_libraries(templatetest PRIVATE Qt${QT_VERSION_MAJOR}::Core)

# Enforce tokeniser, checked against real vanilla source when P: is mounted.
add_executable(lexertest
  lexertest.cpp
  ../src/enforce/lexer.cpp
)
target_include_directories(lexertest PRIVATE ../src)
target_link_libraries(lexertest PRIVATE Qt${QT_VERSION_MAJOR}::Core)

# Enforce parser. Prints the share of real statements it has to keep as raw,
# over vanilla source and over the user's own project:
#   ./tests/parsertest ../resources
add_executable(parsertest
  parsertest.cpp
  ../src/enforce/parser.cpp
  ../src/enforce/lexer.cpp
)
target_include_directories(parsertest PRIVATE ../src)
target_link_libraries(parsertest PRIVATE Qt${QT_VERSION_MAJOR}::Core)

# Enforce file to graph and back. Prints the share of real files that survive
# the round trip, and what the rest lost:
#   ./tests/importtest ../resources [-v]
add_executable(importtest
  importtest.cpp
  ../src/pins.cpp
  ../src/graph.cpp
  ../src/catalog.cpp
  ../src/project.cpp
  ../src/builtins.cpp
  ../src/scriptapi.cpp
  ../src/templates.cpp
  ../src/codegen.cpp
  ../src/theme.cpp
  ../src/enforce/lexer.cpp
  ../src/enforce/parser.cpp
  ../src/enforce/lower.cpp
  ../src/enforce/layout.cpp
  ../src/enforce/import.cpp
  ../src/enforce/highlighter.cpp
  ../src/widgets/newscriptdialog.cpp
)
target_include_directories(importtest PRIVATE ../src)
# The New script dialog is linked in so the skeletons under test are the ones
# the app actually writes rather than a copy that can drift. Nothing here builds
# a QApplication: only scriptSkeleton() is called, and it touches no widget.
target_link_libraries(importtest PRIVATE
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
  Qt${QT_VERSION_MAJOR}::Widgets
)

# Enforce code to nodes and back. Prints how much of the user's project stops
# being a text box:
#   ./tests/lowertest ../resources
add_executable(lowertest
  lowertest.cpp
  ../src/pins.cpp
  ../src/graph.cpp
  ../src/catalog.cpp
  ../src/project.cpp
  ../src/builtins.cpp
  ../src/scriptapi.cpp
  ../src/templates.cpp
  ../src/codegen.cpp
  ../src/enforce/lexer.cpp
  ../src/enforce/parser.cpp
  ../src/enforce/lower.cpp
  ../src/enforce/layout.cpp
  ../src/theme.cpp
)
target_include_directories(lowertest PRIVATE ../src)
# Widgets comes in through theme.cpp, which the layout pass links against. The
# test never builds a QApplication, and layoutNodes falls back to constants when
# there is no GUI, so this still runs headless.
target_link_libraries(lowertest PRIVATE
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
  Qt${QT_VERSION_MAJOR}::Widgets
)

# Arithmetic has to be reachable from the palette and survive into the file.
add_executable(mathtest
  mathtest.cpp
  ../src/pins.cpp ../src/graph.cpp ../src/catalog.cpp ../src/project.cpp
  ../src/builtins.cpp ../src/scriptapi.cpp ../src/templates.cpp
  ../src/enforce/lexer.cpp ../src/enforce/parser.cpp ../src/enforce/lower.cpp
  ../src/enforce/layout.cpp ../src/enforce/import.cpp
  ../src/codegen.cpp ../src/theme.cpp
)
target_include_directories(mathtest PRIVATE ../src)
target_link_libraries(mathtest PRIVATE
  Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets
)

# Depending on another mod: the COT and CF presets, reading addon names out of a
# config.cpp, dependencies through a .sdzn round trip, and indexing a mod folder
# into nodes. Builds its own mod folders in a QTemporaryDir, so it needs no mod
# installed and runs headless:
#   ./tests/depstest ../resources
add_executable(depstest
  depstest.cpp
  ../src/moddeps.cpp
  ../src/pins.cpp ../src/graph.cpp ../src/catalog.cpp ../src/project.cpp
  ../src/builtins.cpp ../src/scriptapi.cpp ../src/templates.cpp
  ../src/config/configtree.cpp
  ../src/enforce/lexer.cpp ../src/enforce/parser.cpp ../src/enforce/lower.cpp
  ../src/enforce/layout.cpp ../src/enforce/import.cpp
  ../src/codegen.cpp ../src/theme.cpp
)
target_include_directories(depstest PRIVATE ../src)
# Widgets arrives through theme.cpp, which the layout pass inside the importer
# links against. No QApplication is built here.
target_link_libraries(depstest PRIVATE
  Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets
)

# config.cpp as a class tree. Prints the share of real configs that survive
# parse and write byte for byte, over the repo and over every vanilla config on
# the work drive when P: is mounted:
#   ./tests/configtest ../resources [-v]
add_executable(configtest
  configtest.cpp
  ../src/config/configtree.cpp
  ../src/enforce/lexer.cpp
)
target_include_directories(configtest PRIVATE ../src)
target_link_libraries(configtest PRIVATE Qt${QT_VERSION_MAJOR}::Core)

# The typed value editor: one widget per Enforce kind, and the literal it hands
# back. Builds a QApplication on the offscreen platform, which the test forces
# itself, so it runs with no display:
#   ./tests/valueeditortest ../resources
add_executable(valueeditortest
  valueeditortest.cpp
  ../src/widgets/valueeditor.cpp
  ../src/pins.cpp ../src/graph.cpp ../src/catalog.cpp ../src/theme.cpp
)
target_include_directories(valueeditortest PRIVATE ../src)
target_link_libraries(valueeditortest PRIVATE
  Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets
)

# The config editor's rules, and what a save writes. Builds a QApplication on
# the offscreen platform, which the test forces itself, so it runs with no
# display. `--shot <png>` writes a picture of the window on the bundled template:
#   ./tests/configeditortest ../resources [--shot config.png]
add_executable(configeditortest
  configeditortest.cpp
  ../src/config/configtree.cpp
  ../src/widgets/configeditor.cpp
  ../src/widgets/codeeditor.cpp
  ../src/widgets/valueeditor.cpp
  ../src/enforce/highlighter.cpp
  ../src/enforce/lexer.cpp ../src/enforce/parser.cpp ../src/enforce/lower.cpp
  ../src/enforce/layout.cpp ../src/enforce/import.cpp
  ../src/pins.cpp ../src/graph.cpp ../src/catalog.cpp ../src/project.cpp
  ../src/moddeps.cpp
  ../src/builtins.cpp ../src/scriptapi.cpp ../src/templates.cpp
  ../src/codegen.cpp ../src/analysis.cpp ../src/document.cpp ../src/theme.cpp
)
target_include_directories(configeditortest PRIVATE ../src)
target_link_libraries(configeditortest PRIVATE
  Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets
)
