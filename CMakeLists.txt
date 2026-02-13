cmake_minimum_required(VERSION 3.16)
project(spms_broadcast_ring_buffer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(publisher publisher.cpp)
target_link_libraries(publisher pthread)

add_executable(subscriber subscriber.cpp)
target_link_libraries(subscriber pthread)
