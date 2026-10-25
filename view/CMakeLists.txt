cmake_minimum_required(VERSION 3.16)
project(MapsViewPlugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# Derive the module name from metadata.json -- single source of truth.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/metadata.json" METADATA_JSON)
string(JSON MODULE_NAME GET "${METADATA_JSON}" name)

# The view renders with Qt Quick inside its own process (RFC-1 ui-host --gui),
# so the plugin links Quick/Qml/OpenGL itself; ui-host links only Gui.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick OpenGL Network)

# The engine (libQMapLibre + libQMapLibreQuickPrivate, metadata.json
# nix.external_libraries) is staged by the builder under lib/ with its
# headers; EXTERNAL_LIBS links it and ships it beside the plugin.
#
# Everything under src/ except the backend is the view proper and is also
# compiled into the conformance tests (tests/CMakeLists.txt) and the dev
# launcher (devtools/), so keep it free of generated-code includes.
set(MAPS_VIEW_SOURCES
    src/view/camera_intents.h
    src/view/camera_intents.cpp
    src/view/capabilities.h
    src/view/capabilities.cpp
    src/view/core_client.h
    src/view/core_client.cpp
    src/view/engine_config.h
    src/view/engine_config.cpp
    src/view/map_item.h
    src/view/map_item.cpp
    src/view/map_change.h
    src/view/map_change.cpp
    src/view/scene_applier.h
    src/view/scene_applier.cpp
    src/view/style_loader.h
    src/view/style_loader.cpp
    src/view/view_controller.h
    src/view/view_controller.cpp
    src/view/view_window.h
    src/view/view_window.cpp
    src/providers/resource_provider.h
    src/providers/resource_provider.cpp
    src/providers/provider_registry.h
    src/providers/provider_registry.cpp
    src/providers/composite_file_source.h
    src/providers/composite_file_source.cpp
    src/providers/memory_file_source.h
    src/providers/memory_file_source.cpp
    src/providers/logos_file_source.h
    src/providers/logos_file_source.cpp
)

logos_module(
    NAME ${MODULE_NAME}
    REP_FILE src/maps_view.rep
    SOURCES
        src/maps_view_backend.h
        src/maps_view_backend.cpp
        src/sdk_core_client.h
        src/sdk_core_client.cpp
        ${MAPS_VIEW_SOURCES}
    EXTERNAL_LIBS
        QMapLibre
        QMapLibreQuickPrivate
    LINK_LIBRARIES
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::OpenGL
        Qt6::Network
    INCLUDE_DIRS
        src
)
