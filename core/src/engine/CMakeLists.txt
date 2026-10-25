# maps_engine — the Qt-free scene engine behind maps_core
# (docs/architecture.md §6, §9, §11, §12.5).
#
# This library holds the retained scene, SceneBatch validation + coalescing,
# the identity gate and revision log, anchor resolution, the basemap
# catalogue, the style patcher, capability aggregation and the view/provider
# registries. It depends on the C++17 standard library, nlohmann::json and
# OpenSSL's libcrypto (SHA-256), and on nothing else: no Qt, no module SDK, no
# IPC. The Qt plugin glue and the module API live one directory up and link it.
#
# Both core/CMakeLists.txt (the module) and core/tests/CMakeLists.txt (the unit
# tests) add_subdirectory() this file, so it must stay self-contained.

cmake_minimum_required(VERSION 3.15)

find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

set(MAPS_ENGINE_SOURCES
    canon.cpp
    scene.cpp
    store.cpp
    anchors.cpp
    basemap.cpp
    stylepatch.cpp
    capability.cpp
    registry.cpp
    projection.cpp
    builtin_basemaps.cpp
)

add_library(maps_engine STATIC ${MAPS_ENGINE_SOURCES})

target_compile_features(maps_engine PUBLIC cxx_std_17)
set_target_properties(maps_engine PROPERTIES
    POSITION_INDEPENDENT_CODE ON   # linked into the module's shared plugin
    CXX_EXTENSIONS OFF
    AUTOMOC OFF                    # the builder turns CMAKE_AUTOMOC on globally;
    AUTOUIC OFF                    # there is no Qt here to moc
    AUTORCC OFF
)

target_include_directories(maps_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..  # #include "engine/<header>.h"
)

target_link_libraries(maps_engine
    PUBLIC  nlohmann_json::nlohmann_json
    PRIVATE OpenSSL::Crypto
)

# Qt-free guard (same as quests_engine): the archive must not reference any
# Qt symbol. A mangled Qt name starts with _ZN<len>Q<Upper>... or
# _ZNK<len>Q<Upper>...; fail the build if `nm -u` lists one.
if(UNIX AND CMAKE_NM)
    add_custom_command(TARGET maps_engine POST_BUILD
        COMMAND sh -c "if ${CMAKE_NM} -u $<TARGET_FILE:maps_engine> | grep -Eq '_ZNK?[0-9]+Q[A-Z]'; then echo 'maps_engine references Qt symbols:'; ${CMAKE_NM} -u -C $<TARGET_FILE:maps_engine> | grep -E 'Q[A-Z]' | head; exit 1; fi"
        VERBATIM
        COMMENT "maps_engine: checking the archive is Qt-free"
    )
endif()
