cmake_minimum_required(VERSION 3.19)
project(MapsViewTests LANGUAGES CXX)

# Standalone build of the view's own sources (no builder, no generated glue,
# no IPC) for the section 16.1 / 16.3 conformance suites and the dev
# launcher. Driven by nix/view-tests.nix; usable by hand from a dev shell:
#
#   cmake -S view/tests -B /tmp/vt -DMAPS_VIEW_PMTILES_INCLUDE=<engine-src>/vendor/maplibre-native/vendor/PMTiles/cpp
#   cmake --build /tmp/vt && ctest --test-dir /tmp/vt --output-on-failure

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick OpenGL Network Test)
find_package(QMapLibre REQUIRED COMPONENTS Core QuickPrivate)
find_package(SQLite3 REQUIRED)

set(MAPS_VIEW_PMTILES_INCLUDE "" CACHE PATH "Directory holding pmtiles.hpp (engine source vendor/maplibre-native/vendor/PMTiles/cpp)")
option(MAPS_VIEW_BUILD_DEV "Build the maps_view_dev launcher" ON)

set(VIEW_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# Everything under src/ except the backend and SDK client (the plugin glue).
add_library(maps_view_lib STATIC
    ${VIEW_SRC}/view/camera_intents.cpp
    ${VIEW_SRC}/view/capabilities.cpp
    ${VIEW_SRC}/view/core_client.h
    ${VIEW_SRC}/view/core_client.cpp
    ${VIEW_SRC}/view/engine_config.cpp
    ${VIEW_SRC}/view/map_item.h
    ${VIEW_SRC}/view/map_item.cpp
    ${VIEW_SRC}/view/map_change.cpp
    ${VIEW_SRC}/view/scene_applier.h
    ${VIEW_SRC}/view/scene_applier.cpp
    ${VIEW_SRC}/view/style_loader.h
    ${VIEW_SRC}/view/style_loader.cpp
    ${VIEW_SRC}/view/view_controller.h
    ${VIEW_SRC}/view/view_controller.cpp
    ${VIEW_SRC}/view/view_window.h
    ${VIEW_SRC}/view/view_window.cpp
    ${VIEW_SRC}/providers/resource_provider.cpp
    ${VIEW_SRC}/providers/provider_registry.h
    ${VIEW_SRC}/providers/provider_registry.cpp
    ${VIEW_SRC}/providers/composite_file_source.cpp
    ${VIEW_SRC}/providers/memory_file_source.cpp
    ${VIEW_SRC}/providers/logos_file_source.cpp
    ${VIEW_SRC}/input/gesture_controller.h
    ${VIEW_SRC}/input/gesture_controller.cpp
    ${VIEW_SRC}/input/picker.h
    ${VIEW_SRC}/input/picker.cpp
    ${VIEW_SRC}/view/builtins.h
    ${VIEW_SRC}/view/builtins.cpp
    ${VIEW_SRC}/view/selection_overlay.h
    ${VIEW_SRC}/view/selection_overlay.cpp
)
target_include_directories(maps_view_lib PUBLIC ${VIEW_SRC})
target_link_libraries(maps_view_lib PUBLIC
    QMapLibre::Core QMapLibre::QuickPrivate
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::OpenGL Qt6::Network
    ${CMAKE_DL_LIBS}
)

enable_testing()

add_executable(test_engine_contract test_engine_contract.cpp fake_core_client.h fake_core_client.cpp)
target_link_libraries(test_engine_contract PRIVATE maps_view_lib)
add_test(NAME engine_contract COMMAND test_engine_contract)
set_tests_properties(engine_contract PROPERTIES
    ENVIRONMENT "MAPS_VIEW_QML_DIR=${VIEW_SRC}/qml;QT_QPA_PLATFORM=offscreen"
    TIMEOUT 300)

add_executable(test_picking test_picking.cpp fake_core_client.h fake_core_client.cpp)
target_link_libraries(test_picking PRIVATE maps_view_lib)
add_test(NAME picking COMMAND test_picking)
set_tests_properties(picking PROPERTIES
    ENVIRONMENT "MAPS_VIEW_QML_DIR=${VIEW_SRC}/qml;QT_QPA_PLATFORM=offscreen"
    TIMEOUT 300)

add_executable(test_gestures test_gestures.cpp)
target_link_libraries(test_gestures PRIVATE maps_view_lib Qt6::Test)
add_test(NAME gestures COMMAND test_gestures)
set_tests_properties(gestures PROPERTIES
    ENVIRONMENT "MAPS_VIEW_QML_DIR=${VIEW_SRC}/qml;QT_QPA_PLATFORM=offscreen"
    TIMEOUT 300)

add_executable(test_providers test_providers.cpp)
target_link_libraries(test_providers PRIVATE maps_view_lib SQLite::SQLite3)
if(MAPS_VIEW_PMTILES_INCLUDE)
    target_include_directories(test_providers PRIVATE ${MAPS_VIEW_PMTILES_INCLUDE})
endif()
add_test(NAME providers COMMAND test_providers)
set_tests_properties(providers PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)

if(MAPS_VIEW_BUILD_DEV)
    add_executable(maps_view_dev
        ${CMAKE_CURRENT_SOURCE_DIR}/../devtools/maps_view_dev/main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../devtools/maps_view_dev/logosctl_core_client.h
        ${CMAKE_CURRENT_SOURCE_DIR}/../devtools/maps_view_dev/logosctl_core_client.cpp)
    target_link_libraries(maps_view_dev PRIVATE maps_view_lib)
    install(TARGETS maps_view_dev RUNTIME DESTINATION bin)
    # The launcher looks for qml/MapWindow.qml beside its own binary's
    # directory (ViewWindow::pluginDirectory); ship the QML next to it.
    install(DIRECTORY ${VIEW_SRC}/qml DESTINATION bin)
endif()
install(TARGETS test_engine_contract test_providers test_gestures test_picking RUNTIME DESTINATION libexec/maps-view-tests)
install(DIRECTORY ${VIEW_SRC}/qml DESTINATION libexec/maps-view-tests)
