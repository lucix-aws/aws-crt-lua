cmake_minimum_required(VERSION 3.9...3.31)
project(aws-crt-lua C)

option(BUILD_DEPS "Build CRT dependencies as part of build" ON)

if (BUILD_DEPS)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/crt/aws-c-common/cmake")
    include(AwsFindPackage)
    set(IN_SOURCE_BUILD ON)
    set(BUILD_TESTING OFF)
    set(BUILD_SHARED_LIBS OFF)

    if (UNIX AND NOT APPLE)
        set(SEARCH_LIBCRYPTO OFF CACHE BOOL "Let S2N use libcrypto from AWS-LC.")
        include(AwsPrebuildDependency)
        set(AWSLC_CMAKE_ARGUMENTS
            -DDISABLE_GO=ON
            -DDISABLE_PERL=ON
            -DBUILD_LIBSSL=OFF
            -DBUILD_TESTING=OFF
        )
        aws_prebuild_dependency(
            DEPENDENCY_NAME AWSLC
            SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/crt/aws-lc
            CMAKE_ARGUMENTS ${AWSLC_CMAKE_ARGUMENTS}
        )
        aws_prebuild_dependency(
            DEPENDENCY_NAME S2N
            SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/crt/s2n
            CMAKE_ARGUMENTS -DUNSAFE_TREAT_WARNINGS_AS_ERRORS=OFF -DBUILD_TESTING=OFF
        )
    endif()

    add_subdirectory(crt/aws-c-common)
    add_subdirectory(crt/aws-c-sdkutils)
    add_subdirectory(crt/aws-c-cal)
    add_subdirectory(crt/aws-c-io)
    add_subdirectory(crt/aws-c-compression)
    add_subdirectory(crt/aws-c-http)
    add_subdirectory(crt/aws-c-auth)
else()
    find_package(aws-c-common REQUIRED)
    include(AwsFindPackage)
    set(IN_SOURCE_BUILD OFF)
endif()

include(AwsCFlags)
include(AwsSharedLibSetup)
include(GNUInstallDirs)

# Build the shim as a shared library
add_library(aws-crt-lua SHARED src/crt_http.c)
aws_set_common_properties(aws-crt-lua)

target_include_directories(aws-crt-lua PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

aws_use_package(aws-c-common)
aws_use_package(aws-c-io)
aws_use_package(aws-c-http)
aws_use_package(aws-c-auth)

target_link_libraries(aws-crt-lua PRIVATE ${DEP_AWS_LIBS})
