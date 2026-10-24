cmake_minimum_required(VERSION 3.14)
project(MapsUiPlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# Derive the module name from metadata.json -- single source of truth.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET ${METADATA_JSON} name)

# Universal UI module: the .rep is the view<->backend contract, the *Backend
# class is the only hand-written C++. repc and the generated *Plugin glue in
# generated_code/ are driven by logos_module().
logos_module(
    NAME ${MODULE_NAME}
    REP_FILE src/maps_ui.rep
    SOURCES
        src/maps_ui_backend.h
        src/maps_ui_backend.cpp
    INCLUDE_DIRS
        src
)
