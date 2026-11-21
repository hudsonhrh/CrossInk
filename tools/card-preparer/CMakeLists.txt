cmake_minimum_required(VERSION 3.21)
project(CrossInkCardPreparer LANGUAGES C CXX)

if(NOT APPLE OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "Build the Mac card preparer with GCC 15 (CC=gcc-15 CXX=g++-15)")
endif()

include(FetchContent)
set(repo "${CMAKE_CURRENT_LIST_DIR}/../..")
set(CROSSINK_DEPENDENCIES "" CACHE PATH
    "Optional directory containing the firmware's simulator and ArduinoJson dependencies")
if(CROSSINK_DEPENDENCIES AND EXISTS "${CROSSINK_DEPENDENCIES}/simulator/src/HalStorage.cpp")
  set(simulator_SOURCE_DIR "${CROSSINK_DEPENDENCIES}/simulator")
else()
  FetchContent_Declare(simulator
    GIT_REPOSITORY https://github.com/uxjulia/crossink-simulator
    GIT_TAG e9879b6d9c5f98f9db57c8c4c2dfee573f1585e8
    SOURCE_SUBDIR unused)
  FetchContent_MakeAvailable(simulator)
endif()
if(CROSSINK_DEPENDENCIES AND EXISTS "${CROSSINK_DEPENDENCIES}/ArduinoJson/src/ArduinoJson.h")
  set(arduinojson_SOURCE_DIR "${CROSSINK_DEPENDENCIES}/ArduinoJson")
else()
  FetchContent_Declare(arduinojson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson
    GIT_TAG v7.4.2
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR unused)
  FetchContent_MakeAvailable(arduinojson)
endif()
set(sim "${simulator_SOURCE_DIR}/src")
set(lib "${repo}/lib")
set(sdk "${repo}/freeink-sdk/libs")

add_executable(crossink-prepare-engine engine.cpp
  "${sim}/HalStorage.cpp"
  "${sim}/SimulatorStackCheck.cpp"
  "${lib}/Epub/Epub.cpp"
  "${lib}/Epub/Epub/BookMetadataCache.cpp"
  "${lib}/Epub/Epub/css/CssParser.cpp"
  "${lib}/Epub/Epub/parsers/ContainerParser.cpp"
  "${lib}/Epub/Epub/parsers/ContentOpfParser.cpp"
  "${lib}/Epub/Epub/parsers/TocNavParser.cpp"
  "${lib}/Epub/Epub/parsers/TocNcxParser.cpp"
  "${lib}/Xtc/Xtc.cpp"
  "${lib}/Xtc/Xtc/XtcParser.cpp"
  "${lib}/ZipFile/ZipFile.cpp"
  "${lib}/miniz/src/InflateStream.cpp"
  "${lib}/miniz/src/miniz_impl.c"
  "${lib}/expat/xmlparse.c"
  "${lib}/expat/xmlrole.c"
  "${lib}/expat/xmltok.c"
  "${lib}/uzlib/src/uzlib_checksums.c"
  "${lib}/FsHelpers/FsHelpers.cpp"
  "${lib}/GfxRenderer/Bitmap.cpp"
  "${lib}/GfxRenderer/BitmapHelpers.cpp"
  "${lib}/Serialization/AtomicFile.cpp"
  "${lib}/Memory/BuildScratch.cpp"
  "${lib}/Utf8/Utf8.cpp")
target_compile_features(crossink-prepare-engine PRIVATE cxx_std_20)
target_compile_definitions(crossink-prepare-engine PRIVATE
  SIMULATOR SIMULATOR_DEVICE_X4_PRO ENABLE_SERIAL_LOG LOG_LEVEL=1 XML_GE=0 XML_CONTEXT_BYTES=0)
target_compile_options(crossink-prepare-engine PRIVATE -ffunction-sections -fdata-sections)
target_link_options(crossink-prepare-engine PRIVATE -Wl,-dead_strip -static-libstdc++ -static-libgcc)
target_include_directories(crossink-prepare-engine PRIVATE
  "${sim}" "${arduinojson_SOURCE_DIR}/src" "${repo}/src" "${repo}/include"
  "${sdk}/book/ContentProtection/include"
  "${sdk}/ui/FreeInkUI/include" "${sdk}/assets/ImageToneMapper/include"
  "${sdk}/assets/Icons/include"
  "${lib}/Epub" "${lib}/Xtc" "${lib}/expat" "${lib}/miniz/src" "${lib}/uzlib/src"
  "${lib}/ZipFile" "${lib}/FsHelpers" "${lib}/GfxRenderer"
  "${lib}/Logging" "${lib}/Memory" "${lib}/MemoryBudget"
  "${lib}/Serialization" "${lib}/Utf8" "${lib}/EpdFont" "${lib}/XmlParserUtils"
  "${lib}/InflateReader" "${lib}/GifToBmpConverter" "${lib}/GifSupport" "${lib}/JpegToBmpConverter" "${lib}/PngToBmpConverter")

install(TARGETS crossink-prepare-engine RUNTIME DESTINATION bin)
install(PROGRAMS prepare.py DESTINATION bin RENAME crossink-prepare)
install(FILES "${repo}/docs/card-preparer.md" "${repo}/LICENSE" DESTINATION .)
