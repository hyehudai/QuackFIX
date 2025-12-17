cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME quackfix)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# Store the project root before project() changes context
set(QUACKFIX_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

project(${TARGET_NAME})
include_directories(src/include)
include_directories(src)

# Add tinyxml2 directory to includes
include_directories(third_party/tinyxml2)

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
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
