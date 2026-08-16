# Every target here links tiers out of the root CMakeLists rather than naming
# shared sources by relative path, so a shared file is compiled once instead of
# once per target. The tier a target stops at is the interesting part and is
# still per target: nodemod_core is Qt Core, nodemod_model adds Qt Gui, and
# nodemod_themed is the first tier that carries Qt Widgets. A console test that
# names nodemod_model and stops there stays off Widgets, which is the property
# branding.h depends on.
#
# Run one:   cmake --build ../build/cli --target coretest && ./tests/coretest ../resources
# Run all:   cmake --build ../build/cli && for f in tests/*.exe; do "$f" ../resources; done

# Headless model-layer test. Runs without a display.
add_executable(coretest coretest.cpp)
target_link_libraries(coretest PRIVATE nodemod_core nodemod_model)

# The four crash repros from the audit: cycles, nesting depth and Sequence
# fan-out all used to take the process down.
add_executable(crashtest crashtest.cpp)
target_link_libraries(crashtest PRIVATE nodemod_core nodemod_model)

# Event ranking. Proves ItemBase's 227 events all survive the reorder, that the
# curated names still exist in the catalogue, and that debug and deprecated
# entries sink instead of disappearing:
#   ./tests/eventstest ../resources
add_executable(eventstest eventstest.cpp)
target_link_libraries(eventstest PRIVATE nodemod_core nodemod_model)

# Mod scaffolding. Copies the bundled template into a QTemporaryDir and proves
# the ModTemplate token is gone from every name and every body.
add_executable(templatetest templatetest.cpp)
target_link_libraries(templatetest PRIVATE nodemod_core)

# Enforce tokeniser, checked against real vanilla source when P: is mounted.
add_executable(lexertest lexertest.cpp)
target_link_libraries(lexertest PRIVATE nodemod_core)

# Enforce parser. Prints the share of real statements it has to keep as raw,
# over vanilla source and over the user's own project:
#   ./tests/parsertest ../resources
add_executable(parsertest parsertest.cpp)
target_link_libraries(parsertest PRIVATE nodemod_core)

# Enforce file to graph and back. Prints the share of real files that survive
# the round trip, and what the rest lost:
#   ./tests/importtest ../resources [-v]
#
# The New script dialog comes in with nodemod_panels so the skeletons under test
# are the ones the app actually writes rather than a copy that can drift.
# Nothing here builds a QApplication: only scriptSkeleton() is called, and it
# touches no widget.
add_executable(importtest importtest.cpp)
target_link_libraries(importtest PRIVATE
  nodemod_core nodemod_model nodemod_themed nodemod_panels)

# Enforce code to nodes and back. Prints how much of the user's project stops
# being a text box:
#   ./tests/lowertest ../resources
#
# Widgets arrives with nodemod_themed, which is where theme.cpp and the layout
# pass live. The test never builds a QApplication, and layoutNodes falls back to
# constants when there is no GUI, so this still runs headless.
add_executable(lowertest lowertest.cpp)
target_link_libraries(lowertest PRIVATE
  nodemod_core nodemod_model nodemod_themed)

# Arithmetic has to be reachable from the palette and survive into the file.
add_executable(mathtest mathtest.cpp)
target_link_libraries(mathtest PRIVATE
  nodemod_core nodemod_model nodemod_themed)

# Depending on another mod: the COT and CF presets, reading addon names out of a
# config.cpp, dependencies through a .sdzn round trip, and indexing a mod folder
# into nodes. Builds its own mod folders in a QTemporaryDir, so it needs no mod
# installed and runs headless:
#   ./tests/depstest ../resources
add_executable(depstest depstest.cpp)
target_link_libraries(depstest PRIVATE
  nodemod_core nodemod_model nodemod_themed)

# config.cpp as a class tree. Prints the share of real configs that survive
# parse and write byte for byte, over the repo and over every vanilla config on
# the work drive when P: is mounted:
#   ./tests/configtest ../resources [-v]
add_executable(configtest configtest.cpp)
target_link_libraries(configtest PRIVATE nodemod_core)

# The typed value editor: one widget per Enforce kind, and the literal it hands
# back. Builds a QApplication on the offscreen platform, which the test forces
# itself, so it runs with no display:
#   ./tests/valueeditortest ../resources
add_executable(valueeditortest valueeditortest.cpp)
target_link_libraries(valueeditortest PRIVATE
  nodemod_core nodemod_model nodemod_themed nodemod_panels)

# The config editor's rules, and what a save writes. Builds a QApplication on
# the offscreen platform, which the test forces itself, so it runs with no
# display. `--shot <png>` writes a picture of the window on the bundled template:
#   ./tests/configeditortest ../resources [--shot config.png]
add_executable(configeditortest configeditortest.cpp)
target_link_libraries(configeditortest PRIVATE
  nodemod_core nodemod_model nodemod_themed nodemod_panels)

# The dependency rules DZ314/315/316 were proven in a scratch harness; keeping
# them covered means keeping the harness.
add_executable(deprulestest deprulestest.cpp)
target_link_libraries(deprulestest PRIVATE
  nodemod_core nodemod_model nodemod_themed)

# The installed mods as a library. Builds a mod of its own in a QTemporaryDir to
# check the read path and the read only promise, then walks whatever is really
# installed and prints how much of it the importer can model. Forces the
# offscreen platform itself, so it runs with no display:
#   ./tests/modlibrarytest ../resources [sample size] [--shot browser.png]
add_executable(modlibrarytest modlibrarytest.cpp)
target_link_libraries(modlibrarytest PRIVATE
  nodemod_core nodemod_model nodemod_themed nodemod_panels)

# PBO archives read without DayZ Tools. The synthetic half builds its own
# archives in a QTemporaryDir; the corpus half walks every pbo under the
# installed mods and prints the rate, skipping itself when there are none:
#   ./tests/pbotest ../resources [--all] [--limit N] [--workshop <dir>]
add_executable(pbotest pbotest.cpp)
target_link_libraries(pbotest PRIVATE nodemod_core)

# The recent projects list, and the page built on it. The list half runs on
# files the test writes into a QTemporaryDir and its own store file, so it never
# touches the user's real history. `--shot` and `--gallery` write pictures of
# the start page and of the templates column. Forces the offscreen platform
# itself, so it runs with no display:
#   ./tests/recenttest ../resources [--shot start.png] [--gallery templates.png]
#
# The start page and branding are named here rather than pulled from a tier.
# They are the pair that has to stay out of nodemod_panels so branding.cpp
# reaches no target that does without it, and this is the one target that wants
# them. The .qrc is what the page draws the brand lockup out of.
add_executable(recenttest
  recenttest.cpp
  ../src/widgets/startpage.cpp
  ../src/branding.cpp
  ../resources/brand.qrc
)
target_link_libraries(recenttest PRIVATE
  nodemod_core nodemod_model nodemod_themed nodemod_panels)

# Running the mod: prerequisite detection against the real machine, a junction
# made and removed under a temporary name, the project.cfg rewrite line by line,
# and the build, launch and offline command lines printed rather than executed.
# Starts no game and packs no PBO. The dock is built either way, so the mode
# selector is driven the way a user drives it; `--shot` and `--shot-offline`
# write pictures of it in both modes:
#   ./tests/testruntest ../resources [--shot dock.png] [--shot-offline off.png]
#
# The panel arrives with nodemod_panels so the picture is of the dock the app
# really ships. The offscreen platform is forced by the test itself, so this
# still runs with no display.
add_executable(testruntest testruntest.cpp)
target_link_libraries(testruntest PRIVATE
  nodemod_core nodemod_model nodemod_themed nodemod_panels)
