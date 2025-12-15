cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME quackfix)

# DuckDB's extension distribution supports vcpkg. As such, dependencies can be added in ./vcpkg.json and then
# used in cmake with find_package. Feel free to remove or replace with other dependencies.
# Note that it should also be removed from vcpkg.json to prevent needlessly installing it..
find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# Store the project root before project() changes context
set(QUACKFIX_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

project(${TARGET_NAME})
include_directories(src/include)

# Add tinyxml2 directory to includes
include_directories(third_party/tinyxml2)

set(EXTENSION_SOURCES 
    src/quackfix_extension.cpp
    src/dictionary/xml_loader.cpp
    third_party/tinyxml2/tinyxml2.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link OpenSSL in both the static library as the loadable extension
target_link_libraries(${EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)
target_link_libraries(${LOADABLE_EXTENSION_NAME} OpenSSL::SSL OpenSSL::Crypto)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
