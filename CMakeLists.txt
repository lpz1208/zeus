cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(zeus VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ZEUS_BUILD_TESTS "Build Zeus tests" ON)

find_package(GDAL CONFIG REQUIRED)
find_package(Boost REQUIRED)

add_library(zeus_map_engine
    cpp/map-engine/src/geojson_exporter.cc
    cpp/map-engine/src/map_builder.cc
    cpp/map-engine/src/map_runtime.cc
    cpp/map-engine/src/map_serializer.cc
    cpp/map-engine/src/map_validator.cc
    cpp/map-engine/src/osm_road_preprocessor.cc
    cpp/map-engine/src/shapefile_importer.cc
    cpp/map-engine/src/types.cc
)

target_include_directories(zeus_map_engine
    PUBLIC
        cpp/map-engine/include
        ${Boost_INCLUDE_DIRS}
)

target_link_libraries(zeus_map_engine PUBLIC GDAL::GDAL)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(zeus_map_engine PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(zeus_routing_core
    cpp/routing-core/src/route_exporter.cc
    cpp/routing-core/src/route_planner.cc
    cpp/routing-core/src/route_types.cc
    cpp/routing-core/src/search.cc
)

target_include_directories(zeus_routing_core
    PUBLIC
        cpp/routing-core/include
)

target_link_libraries(zeus_routing_core PUBLIC zeus_map_engine)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(zeus_routing_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(zeus_simulation_core
    cpp/simulation-core/src/playback_exporter.cc
    cpp/simulation-core/src/simulation_engine.cc
    cpp/simulation-core/src/simulation_session.cc
)

target_include_directories(zeus_simulation_core
    PUBLIC
        cpp/simulation-core/include
)

target_link_libraries(zeus_simulation_core PUBLIC zeus_routing_core)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(zeus_simulation_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(zeus-map
    tools/zeus-map/main.cc
    tools/zeus-map/session_worker.cc
    tools/zeus-map/simulate_io.cc)
target_link_libraries(zeus-map PRIVATE zeus_map_engine zeus_routing_core zeus_simulation_core)

if(ZEUS_BUILD_TESTS)
    enable_testing()
    add_executable(zeus-map-engine-tests tests/map_engine_tests.cc)
    target_link_libraries(zeus-map-engine-tests PRIVATE zeus_map_engine)
    add_test(NAME zeus-map-engine-tests COMMAND zeus-map-engine-tests)
    add_executable(zeus-routing-tests tests/routing_tests.cc)
    target_link_libraries(zeus-routing-tests PRIVATE zeus_routing_core)
    add_test(NAME zeus-routing-tests COMMAND zeus-routing-tests)
    add_executable(zeus-simulation-tests tests/simulation_tests.cc)
    target_link_libraries(zeus-simulation-tests PRIVATE zeus_simulation_core)
    add_test(NAME zeus-simulation-tests COMMAND zeus-simulation-tests)
endif()
