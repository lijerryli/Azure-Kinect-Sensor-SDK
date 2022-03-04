# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT License.

add_executable(k4arawtodepth main.cpp ${CMAKE_CURRENT_BINARY_DIR}/version.rc)

target_include_directories(k4arawtodepth PUBLIC
    ${K4A_PRIV_INCLUDE_DIR})

target_link_libraries(k4arawtodepth PRIVATE
    k4a::k4a
    k4a::k4arecord
    k4ainternal::deloader
    )

# Include ${CMAKE_CURRENT_BINARY_DIR}/version.rc in the target's sources
# to embed version information
set(K4A_FILEDESCRIPTION "Azure Kinect Raw to Depth Tool")
set(K4A_ORIGINALFILENAME "k4arawtodepth.exe")
configure_file(
    ${K4A_VERSION_RC}
    ${CMAKE_CURRENT_BINARY_DIR}/version.rc
    @ONLY
    )

# Setup install
include(GNUInstallDirs)

install(
    TARGETS
    k4arawtodepth
    RUNTIME DESTINATION
        ${CMAKE_INSTALL_BINDIR}
    COMPONENT
        tools
)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
    install(
        FILES
            $<TARGET_PDB_FILE:k4arawtodepth>
        DESTINATION
            ${CMAKE_INSTALL_BINDIR}
        COMPONENT
            tools
        OPTIONAL
    )
endif()