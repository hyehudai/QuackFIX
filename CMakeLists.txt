cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME quackfix)




set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# Store the project root before project() changes context
set(QUACKFIX_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

project(${TARGET_NAME})

# Require C++17 standard for structured bindings and other modern features
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Explicitly set C++17 for MSVC (Windows builds)
if(MSVC)
    add_compile_options(/std:c++17)
endif()

include_directories(src/include)
include_directories(src)

# Add tinyxml2 directory to includes
include_directories(third_party/tinyxml2)

# Generate embedded FIX dictionary at build time
set(EMBEDDED_DICT_INPUT "${CMAKE_CURRENT_SOURCE_DIR}/data/fix44_dictionary.xml")
set(EMBEDDED_DICT_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/embedded_fix44_dictionary.cpp")
set(EMBEDDED_DICT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_embedded_dictionary.py")

add_custom_command(
    OUTPUT ${EMBEDDED_DICT_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E echo "Generating embedded FIX dictionary..."
    COMMAND python3 ${EMBEDDED_DICT_SCRIPT} ${EMBEDDED_DICT_INPUT} ${EMBEDDED_DICT_OUTPUT}
    DEPENDS ${EMBEDDED_DICT_INPUT} ${EMBEDDED_DICT_SCRIPT}
    COMMENT "Generating embedded FIX 4.4 dictionary from XML"
    VERBATIM
)

set(EXTENSION_SOURCES 
    src/quackfix_extension.cpp
    src/dictionary/xml_loader.cpp
    src/parser/fix_tokenizer.cpp
    src/parser/fix_type_conversions.cpp
    src/parser/fix_group_parser.cpp
    src/parser/fix_file_reader.cpp
    src/table_function/read_fix_function.cpp
    src/table_function/dictionary_functions.cpp
    third_party/tinyxml2/tinyxml2.cpp
    ${EMBEDDED_DICT_OUTPUT}
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
