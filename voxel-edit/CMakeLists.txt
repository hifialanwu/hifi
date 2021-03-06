cmake_minimum_required(VERSION 2.8)

set(TARGET_NAME voxel-edit)

set(ROOT_DIR ..)
set(MACRO_DIR ${ROOT_DIR}/cmake/macros)

# setup for find modules
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/modules/")

# set up the external glm library
include(${MACRO_DIR}/IncludeGLM.cmake)
include_glm(${TARGET_NAME} ${ROOT_DIR})

include(${MACRO_DIR}/SetupHifiProject.cmake)

setup_hifi_project(${TARGET_NAME} TRUE)

# link in the shared library
include(${MACRO_DIR}/LinkHifiLibrary.cmake)
link_hifi_library(shared ${TARGET_NAME} ${ROOT_DIR})

# link in the hifi octree library
link_hifi_library(octree ${TARGET_NAME} ${ROOT_DIR})

# link in the hifi voxels library
link_hifi_library(voxels ${TARGET_NAME} ${ROOT_DIR})

IF (WIN32)
	target_link_libraries(${TARGET_NAME} Winmm Ws2_32)
ENDIF(WIN32)
