cmake_minimum_required(VERSION 2.8)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/modules/")

project(hifi)

set(GLM_ROOT_DIR ${CMAKE_SOURCE_DIR}/thirdparty)
set(PORTAUDIO_ROOT_DIR ${CMAKE_SOURCE_DIR}/thirdparty/Portaudio)
set(LODEPNG_ROOT_DIR ${CMAKE_SOURCE_DIR}/thirdparty/LodePNG)

add_subdirectory(interface)