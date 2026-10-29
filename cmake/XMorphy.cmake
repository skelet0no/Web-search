include(FetchContent)
set(XMORPHY_COMMIT "88eb5b84e28f0e00672f07e4fe92e88262242163")
set(XMORPHY_SOURCE_DIR "" CACHE PATH "Local pinned XMorphy checkout")
set(BOOST_HEADERS_DIR "" CACHE PATH "Directory containing boost/")
if(NOT XMORPHY_SOURCE_DIR)
    FetchContent_Declare(xmorphy_source
        URL https://codeload.github.com/alesapin/XMorphy/tar.gz/${XMORPHY_COMMIT}
        URL_HASH SHA256=2e43ce9aed8bcd846506e5d3bc491271c2565751cfd2e7da2f4ceaaeb95964a9)
    FetchContent_GetProperties(xmorphy_source)
    if(NOT xmorphy_source_POPULATED)
        FetchContent_Populate(xmorphy_source)
    endif()
    set(XMORPHY_SOURCE_DIR "${xmorphy_source_SOURCE_DIR}")
endif()
if(NOT BOOST_HEADERS_DIR)
    find_path(system_boost_headers boost/bimap.hpp)
    if(system_boost_headers)
        set(BOOST_HEADERS_DIR "${system_boost_headers}")
    endif()
endif()
if(NOT BOOST_HEADERS_DIR)
    FetchContent_Declare(boost_headers
        URL https://archives.boost.io/release/1.85.0/source/boost_1_85_0.tar.bz2
        URL_HASH SHA256=7009fe1faa1697476bdc7027703a2badb84e849b7b0baad5086b087b971f8617)
    FetchContent_GetProperties(boost_headers)
    if(NOT boost_headers_POPULATED)
        FetchContent_Populate(boost_headers)
    endif()
    set(BOOST_HEADERS_DIR "${boost_headers_SOURCE_DIR}")
endif()
# Upstream's top-level build unconditionally enables TensorFlow and Python.
# Compile only dictionary sources, replacing the unused embedded-data constructor.
set(xroot "${XMORPHY_SOURCE_DIR}/src/xmorphy")
file(READ "${xroot}/morph/DictMorphAnalyzer.cpp" dict_source)
string(REPLACE "#include <incbin.h>" "" dict_source "${dict_source}")
string(REPLACE "namespace\n{\n    INCBIN(affixdict, \"dicts/udaffixdict.bin\");\n    INCBIN(maindict, \"dicts/udmaindict.bin\");\n}" "" dict_source "${dict_source}")
set(default_ctor "DictMorphAnalyzer::DictMorphAnalyzer()\n{\n    std::istringstream mainIs(std::string{reinterpret_cast<const char *>(gmaindictData), gmaindictSize});\n    std::istringstream affixIs(std::string{reinterpret_cast<const char *>(gaffixdictData), gaffixdictSize});\n    dict = MorphDict::loadFromFiles(mainIs, affixIs);\n}")
string(FIND "${dict_source}" "${default_ctor}" ctor_at)
if(ctor_at LESS 0)
    message(FATAL_ERROR "Unsupported XMorphy source: use ${XMORPHY_COMMIT}")
endif()
string(REPLACE "${default_ctor}" "DictMorphAnalyzer::DictMorphAnalyzer() { throw std::logic_error(\"Use the stream constructor\"); }" dict_source "${dict_source}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/DictMorphAnalyzer.cpp" CONTENT "${dict_source}" @ONLY)
add_library(xmorphy_dictionary STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/generated/DictMorphAnalyzer.cpp"
    "${xroot}/build/MorphDict.cpp" "${xroot}/build/BuildDefs.cpp"
    "${xroot}/build/ParadigmBuilder.cpp" "${xroot}/build/RawDict.cpp"
    "${xroot}/utils/UniString.cpp" "${xroot}/utils/UniCharFuncs.cpp"
    "${xroot}/tag/ITag.cpp" "${xroot}/tag/UniSPTag.cpp"
    "${xroot}/tag/UniMorphTag.cpp" "${xroot}/tag/PhemTag.cpp" "${xroot}/tag/AnalyzerTag.cpp")
target_include_directories(xmorphy_dictionary SYSTEM PUBLIC "${XMORPHY_SOURCE_DIR}/src" "${BOOST_HEADERS_DIR}")
target_link_libraries(xmorphy_dictionary PUBLIC ICU::uc)
target_compile_options(xmorphy_dictionary PRIVATE "SHELL:-include array" "SHELL:-include cstdint")
set(LEMMATIZER_DICTIONARY_DIR "${XMORPHY_SOURCE_DIR}/data/dicts" CACHE PATH "Pinned dictionary directory")
install(FILES "${LEMMATIZER_DICTIONARY_DIR}/udmaindict.bin" "${LEMMATIZER_DICTIONARY_DIR}/udaffixdict.bin" DESTINATION share/web-search/dictionary)
install(FILES "${XMORPHY_SOURCE_DIR}/LICENSE" DESTINATION share/web-search/licenses RENAME XMorphy-LICENSE)
install(FILES "${PROJECT_SOURCE_DIR}/lemmatizer/THIRD_PARTY_NOTICES.md" DESTINATION share/web-search/licenses)
