cmake_minimum_required(VERSION 3.16)
project(kos3_accel_services VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(accel_common
    src/common.cpp
)
target_include_directories(accel_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(accel_common PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(accel_common PUBLIC pthread)

add_executable(accel_node_a src/node_a.cpp)
target_link_libraries(accel_node_a PRIVATE accel_common)
target_compile_options(accel_node_a PRIVATE -Wall -Wextra -Wpedantic)

add_executable(accel_server src/server.cpp)
target_link_libraries(accel_server PRIVATE accel_common)
target_compile_options(accel_server PRIVATE -Wall -Wextra -Wpedantic)

add_executable(accel_node_b src/node_b.cpp)
target_link_libraries(accel_node_b PRIVATE accel_common)
target_compile_options(accel_node_b PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS accel_node_a accel_server accel_node_b RUNTIME DESTINATION bin)
install(FILES systemd/accel-node-a.service systemd/accel-server.service systemd/accel-node-b.service DESTINATION lib/systemd/system)
