cmake_minimum_required(VERSION 3.14)
project(MapsCorePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# Derive the module name from metadata.json -- single source of truth.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET "${METADATA_JSON}" name)

# Universal core module: the impl class is the only hand-written C++; the
# Qt plugin glue, the LIDL contract and the typed clients other modules use
# are generated from src/maps_core_impl.h.
logos_module(
    NAME ${MODULE_NAME}
    SOURCES
        src/maps_core_impl.h
        src/maps_core_impl.cpp
)
