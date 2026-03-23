cmake_minimum_required(VERSION 3.10)
project(vulkan-test)

set(CMAKE_CXX_STANDARD 17)

find_package(Vulkan REQUIRED)

add_executable(vk_memstress vk_memstress.cpp)
target_link_libraries(vk_memstress Vulkan::Vulkan)

add_executable(vk_compute_min vk_compute_min.cpp)
target_link_libraries(vk_compute_min Vulkan::Vulkan)
