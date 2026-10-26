cmake_minimum_required(VERSION 3.15)
project(MapsEngineTests LANGUAGES CXX)

include(LogosTest)

# The engine under test. Built here exactly as the module builds it (one
# add_subdirectory of the same CMakeLists), so the unit tests exercise the
# archive the plugin links, Qt-free guard included.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/engine ${CMAKE_CURRENT_BINARY_DIR}/engine)

# Unit tests for maps_engine (docs/architecture.md §16.2 scene replay plus
# unit coverage of the catalogue, patcher, anchors and capabilities) and for
# MapsCoreImpl, constructed directly with test-only event forwarders
# (maps_core_events_test.cpp) in place of the generated event bodies.
logos_test(
    NAME maps_engine_tests
    MODULE_SOURCES
        ../src/maps_core_impl.cpp
    TEST_SOURCES
        main.cpp
        maps_core_events_test.cpp
        test_impl.cpp
        test_canon.cpp
        test_scene.cpp
        test_store.cpp
        test_replay.cpp
        test_anchors.cpp
        test_basemap.cpp
        test_stylepatch.cpp
        test_capability.cpp
        test_registry.cpp
        test_projection.cpp
        test_groups.cpp
    EXTRA_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
    EXTRA_LINK_LIBS
        maps_engine
)

# Fixtures are read at run time relative to this directory.
target_compile_definitions(maps_engine_tests PRIVATE
    MAPS_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
