# reaper_aaf: AAF import/export for REAPER, plus aaf2rpp, rpp2aaf and aafdump.
cmake_minimum_required(VERSION 3.20)
# macOS: one universal binary for Apple silicon and Intel Macs
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
project(reaper_aaf VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  # static runtime: the DLL works without a Visual C++ redistributable
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Single-configuration generators (Makefiles, Ninja: macOS) also put the binaries in
# build/Release, like Visual Studio does.
get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi_config)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
  endif()
  if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}")
  endif()
endif()

# Conversion code used by the extension and the command-line tools
add_library(aaf_core STATIC
  src/aaf_export.cpp
  src/aaf_file.cpp
  src/aaf_import.cpp
  src/cfb.cpp
  src/converter.cpp
  src/media_info.cpp
  src/rpp_reader.cpp
  src/rpp_sink.cpp
  src/tempo_map.cpp
  src/util.cpp
  third_party/pugixml/pugixml.cpp
)
target_include_directories(aaf_core PUBLIC src third_party/pugixml)

# REAPER loads every reaper_*.dll/.dylib/.so from its UserPlugins folder
set(_suffix "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_suffix "-x64")
endif()
add_library(reaper_aaf SHARED src/reaper_main.cpp src/reaper_ext.cpp)
target_include_directories(reaper_aaf PRIVATE third_party/reaper-sdk)
target_link_libraries(reaper_aaf PRIVATE aaf_core)
set_target_properties(reaper_aaf PROPERTIES PREFIX "" OUTPUT_NAME "reaper_aaf${_suffix}")
if(APPLE)
  # REAPER provides the Win32-like API (SWELL) on macOS; swell-modstub.mm looks it up at load time
  enable_language(OBJCXX)
  target_sources(reaper_aaf PRIVATE third_party/WDL/swell/swell-modstub.mm)
  target_compile_definitions(reaper_aaf PRIVATE SWELL_PROVIDED_BY_APP)
  target_link_libraries(reaper_aaf PRIVATE "-framework AppKit")
  set_target_properties(reaper_aaf PROPERTIES SUFFIX ".dylib")
endif()

foreach(_tool aaf2rpp rpp2aaf aafdump)
  add_executable(${_tool} cli/${_tool}.cpp)
  target_link_libraries(${_tool} PRIVATE aaf_core)
endforeach()

if(WIN32)
  set(_userplugins "$ENV{APPDATA}/REAPER/UserPlugins")
elseif(APPLE)
  set(_userplugins "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
else()
  set(_userplugins "$ENV{HOME}/.config/REAPER/UserPlugins")
endif()
set(REAPER_USERPLUGINS_DIR "${_userplugins}" CACHE PATH "REAPER UserPlugins folder used by 'cmake --install'")
install(TARGETS reaper_aaf RUNTIME DESTINATION "${REAPER_USERPLUGINS_DIR}" LIBRARY DESTINATION "${REAPER_USERPLUGINS_DIR}")
