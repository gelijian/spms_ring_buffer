cmake_minimum_required(VERSION 3.16)
project(spms_broadcast_ring_buffer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(publisher publisher.cc)
target_link_libraries(publisher pthread)

add_executable(subscriber subscriber.cc)
target_link_libraries(subscriber pthread)

add_executable(profiler profiler.cc)
target_link_libraries(profiler pthread)
