cmake_minimum_required(VERSION 3.16)

project(Disk360)

add_subdirectory(src)

if(WIN32)
    set(DEPLOY_DIR "${CMAKE_BINARY_DIR}/deploy")

    add_custom_target(deploy
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${DEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:disk360-gui>
            $<TARGET_FILE:disk360-cli>
            "${DEPLOY_DIR}"
        COMMAND windeployqt6 "${DEPLOY_DIR}/$<TARGET_FILE_NAME:disk360-gui>"
        COMMAND ${CMAKE_COMMAND}
            -DDEPLOY_DIR=${DEPLOY_DIR}
            -DMINGW_BIN=${CMAKE_CXX_COMPILER}
            -P "${CMAKE_SOURCE_DIR}/cmake/copy_mingw_dlls.cmake"
        DEPENDS disk360-gui disk360-cli
        COMMENT "Packaging Disk360 into ${DEPLOY_DIR}"
    )
endif()
