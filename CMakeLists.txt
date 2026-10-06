cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(level-solver VERSION 1.4.5)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

setup_geode_mod(${PROJECT_NAME})

# Custom post-build deployment to local Geometry Dash mods directory (local builds only)
if (WIN32 AND NOT DEFINED ENV{CI} AND NOT DEFINED ENV{GITHUB_ACTIONS})
    add_custom_command(TARGET ${PROJECT_NAME}_PACKAGE POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$ENV{LOCALAPPDATA}/GeometryDash/geode/mods"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_BINARY_DIR}/matthew.level-solver.geode"
            "$ENV{LOCALAPPDATA}/GeometryDash/geode/mods/matthew.level-solver.geode"
        COMMENT "Deploying matthew.level-solver.geode to %LOCALAPPDATA%/GeometryDash/geode/mods/"
    )
endif()
