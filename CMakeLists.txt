cmake_minimum_required(VERSION 3.25)

project(tilecopy VERSION 1.0.4 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "tilecopy is a Windows-only tool.")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "tilecopy must be built with the Visual Studio (MSVC) compiler.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tilecopy
  src/main.cpp
  src/util.cpp
  src/hash.cpp
  src/options.cpp
  src/chunkdb.cpp
  src/fsmeta.cpp
  src/reparse.cpp
  src/delta.cpp
  src/engine.cpp
)

target_compile_definitions(tilecopy PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  TILECOPY_VERSION=L"${PROJECT_VERSION}"
)

target_compile_options(tilecopy PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)

target_link_libraries(tilecopy PRIVATE bcrypt advapi32)
