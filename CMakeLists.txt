cmake_minimum_required(VERSION 2.8)

project(hifi)

IF (WIN32)
    include_directories(SYSTEM "externals/winsdk")
    add_definitions( -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS )
    set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} "C:\\Program Files\\Microsoft SDKs\\Windows\\v7.1 ")
ENDIF(WIN32)

set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} $ENV{QT_CMAKE_PREFIX_PATH} )


# set our Base SDK to 10.8
set(CMAKE_OSX_SYSROOT /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.8.sdk)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.8)

# Find includes in corresponding build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)
# Instruct CMake to run moc automatically when needed.
set(CMAKE_AUTOMOC ON)

IF (APPLE)
   EXEC_PROGRAM(uname ARGS -v  OUTPUT_VARIABLE DARWIN_VERSION)
   STRING(REGEX MATCH "[0-9]+" DARWIN_VERSION ${DARWIN_VERSION})
   IF (DARWIN_VERSION GREATER 12)
     SET(CMAKE_CXX_FLAGS "-stdlib=libstdc++")
   ENDIF (DARWIN_VERSION GREATER 12)
ENDIF(APPLE)

# targets not supported on windows
if (NOT WIN32)
add_subdirectory(animation-server)
add_subdirectory(data-server)
endif (NOT WIN32)

# targets on all platforms
add_subdirectory(assignment-client)
add_subdirectory(domain-server)
add_subdirectory(interface)
add_subdirectory(pairing-server)
add_subdirectory(voxel-edit)