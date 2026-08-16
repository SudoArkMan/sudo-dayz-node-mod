# The documentation generator, configured on its own rather than as a target of
# the root project.
#
# It is a console tool that wants Qt Core and Qt Gui and nothing else, it is run
# by whoever is editing the docs rather than by every build, and keeping it out
# of the root CMakeLists means a docs change can never break the app's build.
# The cost is naming the handful of sources it links, which is the set
# nodeindex.cpp and codegen.cpp reach and no more.
#
#   cmake -S tools/nodedoc -B build/nodedoc -G Ninja \
#     -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/mingw_64 \
#     -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe
#   cmake --build build/nodedoc
#   build/nodedoc/nodedoc --reference docs/node-reference.md
cmake_minimum_required(VERSION 3.16)

project(nodedoc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Gui)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Gui)

set(NODEMOD_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

add_executable(nodedoc
  nodedoc.cpp
  ${NODEMOD_SRC}/pins.cpp
  ${NODEMOD_SRC}/graph.cpp
  ${NODEMOD_SRC}/catalog.cpp
  ${NODEMOD_SRC}/builtins.cpp
  ${NODEMOD_SRC}/nodeindex.cpp
  ${NODEMOD_SRC}/templates.cpp
  ${NODEMOD_SRC}/scriptapi.cpp
  ${NODEMOD_SRC}/codegen.cpp
  ${NODEMOD_SRC}/project.cpp
)

target_include_directories(nodedoc PRIVATE ${NODEMOD_SRC})
target_link_libraries(nodedoc PRIVATE
  Qt${QT_VERSION_MAJOR}::Core
  Qt${QT_VERSION_MAJOR}::Gui
)
