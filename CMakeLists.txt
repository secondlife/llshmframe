cmake_minimum_required(VERSION 3.16)
project(llshmframe VERSION 1.0.0 LANGUAGES CXX C) # C is only for the vendored glad.c loader

option(LLSHMFRAME_SHARED         "Build llshmframe as a shared library" OFF)
option(LLSHMFRAME_BUILD_EXAMPLES "Build example executables"            ON)
option(LLSHMFRAME_BUILD_TESTS    "Build tests"                          ON)
option(LLSHMFRAME_BUILD_GL_EXAMPLE "Build the GLFW/OpenGL multiview demo consumer (Windows only)" ${WIN32})

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Lets set_target_properties(... PROPERTIES FOLDER ...) below group targets
# into Solution Explorer folders on generators that support it (Visual
# Studio, Xcode); a silent no-op elsewhere.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# local subset of Boost headers - no libs needed
set(Boost_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third-party/boost_1_91_0/")

if(LLSHMFRAME_SHARED)
    add_library(llshmframe SHARED)
else()
    add_library(llshmframe STATIC)
endif()

add_library(llshmframe::llshmframe ALIAS llshmframe)

target_sources(llshmframe PRIVATE
    src/llsegment.cpp
    src/llpublisher.cpp
    src/llsubscriber.cpp
    src/llutil.cpp)

target_compile_features(llshmframe PUBLIC cxx_std_17)

target_include_directories(llshmframe
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

# PRIVATE: Boost stays out of the consumer's build.

target_include_directories(llshmframe PRIVATE ${Boost_INCLUDE_DIRS})

# Building the library defines LLSHMFRAME_BUILDING (dllexport); consumers
# never define it, so they pick up dllimport instead. LLSHMFRAME_SHARED is
# PUBLIC so a consumer building against the shared target automatically
# selects the dllimport branch in the header.
target_compile_definitions(llshmframe PRIVATE LLSHMFRAME_BUILDING)
if(LLSHMFRAME_SHARED)
    target_compile_definitions(llshmframe PUBLIC LLSHMFRAME_SHARED)
endif()

if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(llshmframe PUBLIC Threads::Threads)
    if(NOT APPLE)
        target_link_libraries(llshmframe PUBLIC rt)   # shm_open
    endif()
endif()

if(MSVC)
    target_compile_options(llshmframe PRIVATE /W4)
    target_compile_definitions(llshmframe PRIVATE _CRT_SECURE_NO_WARNINGS)

    # turn off warning: "structure was padded due to alignment specifier"
    target_compile_options(llshmframe PRIVATE /wd4324)

    # turn off warning: "needs to have dll-interface" -- the pimpl'd
    # unique_ptr members are never touched by client code, only by the
    # exported methods that manage them, so this is the standard benign
    # case for the idiom and only matters when LLSHMFRAME_SHARED is ON.
    target_compile_options(llshmframe PRIVATE /wd4251)
else()
    target_compile_options(llshmframe PRIVATE -Wall -Wextra)
endif()

set_target_properties(llshmframe PROPERTIES
    VERSION   ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    POSITION_INDEPENDENT_CODE ON)

if(LLSHMFRAME_BUILD_EXAMPLES)
    add_executable(llshmframe_sender   examples/sender.cpp)
    add_executable(llshmframe_receiver examples/receiver.cpp)
    add_executable(llshmframe_minimal  examples/minimal.cpp)
    target_link_libraries(llshmframe_sender   PRIVATE llshmframe::llshmframe)
    target_link_libraries(llshmframe_receiver PRIVATE llshmframe::llshmframe)
    target_link_libraries(llshmframe_minimal  PRIVATE llshmframe::llshmframe)
    target_include_directories(llshmframe_sender   PRIVATE examples)
    target_include_directories(llshmframe_receiver PRIVATE examples)
    target_include_directories(llshmframe_minimal  PRIVATE examples)

    # Solution Explorer folder, kept apart from the llshmframe library
    # target itself and from the more elaborate multiview demo below.
    set_target_properties(llshmframe_sender llshmframe_receiver llshmframe_minimal
        PROPERTIES FOLDER "Examples")

    # Console producer: plain portable C++, no GL/windowing dependency, so
    # it builds everywhere the library itself does.
    add_executable(llshmframe_multiview_producer examples/multiview_producer.cpp)
    target_link_libraries(llshmframe_multiview_producer PRIVATE llshmframe::llshmframe)
    target_include_directories(llshmframe_multiview_producer PRIVATE examples)
    set_target_properties(llshmframe_multiview_producer PROPERTIES FOLDER "Examples/Multiview")

    if(WIN32 AND LLSHMFRAME_BUILD_GL_EXAMPLE)
        set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/glad-0.1.36")
        set(GLFW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third-party/glfw-3.4.bin.WIN64")

        add_executable(llshmframe_multiview_consumer
            examples/multiview_consumer_gl/multiview_consumer.cpp
            examples/multiview_consumer_gl/multiview_consumer.h
            ${GLAD_DIR}/src/glad.c)
        target_include_directories(llshmframe_multiview_consumer PRIVATE
            examples
            ${GLAD_DIR}/include
            ${GLFW_DIR}/include)
        target_link_libraries(llshmframe_multiview_consumer PRIVATE
            llshmframe::llshmframe
            ${GLFW_DIR}/lib-vc2022/glfw3.lib
            opengl32)
        set_target_properties(llshmframe_multiview_consumer PROPERTIES FOLDER "Examples/Multiview")

        # Keeps the vendored GL loader out of the way of this demo's own two
        # files in Solution Explorer's per-project file list.
        source_group("Third-Party" FILES ${GLAD_DIR}/src/glad.c)
    endif()
endif()

if(LLSHMFRAME_BUILD_TESTS)
    enable_testing()
    add_executable(llshmframe_tests tests/test_llshmframe.cpp)
    target_link_libraries(llshmframe_tests PRIVATE llshmframe::llshmframe)
    add_test(NAME llshmframe_tests COMMAND llshmframe_tests)
endif()

# ------------------------------------------------------------------ install

install(TARGETS llshmframe
    EXPORT llshmframeTargets
    LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT llshmframeTargets
    FILE      llshmframeTargets.cmake
    NAMESPACE llshmframe::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/llshmframe)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/llshmframeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/llshmframeConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/llshmframe)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/llshmframeConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/llshmframeConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/llshmframeConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/llshmframe)

if(LLSHMFRAME_BUILD_EXAMPLES)
    set(CMAKE_CONFIGURATION_TYPES "Release")

    set_property(
      DIRECTORY
      PROPERTY
      VS_STARTUP_PROJECT
      llshmframe_multiview_producer
    )
endif()
