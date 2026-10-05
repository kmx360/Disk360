cmake_policy(SET CMP0057 NEW)
get_filename_component(MINGW_BIN_DIR "${MINGW_BIN}" DIRECTORY)

file(GLOB DEPLOY_EXES "${DEPLOY_DIR}/*.exe" "${DEPLOY_DIR}/*.dll")

set(ALL_DLLS "")
set(TO_SCAN ${DEPLOY_EXES})

while(TO_SCAN)
    list(POP_FRONT TO_SCAN CURRENT)
    execute_process(
        COMMAND objdump -p "${CURRENT}"
        OUTPUT_VARIABLE OBJDUMP_OUT
        ERROR_QUIET
    )
    string(REGEX MATCHALL "DLL Name: [^\n]+" DLL_LINES "${OBJDUMP_OUT}")
    foreach(line ${DLL_LINES})
        string(REGEX REPLACE "DLL Name: " "" dll "${line}")
        string(STRIP "${dll}" dll)
        string(TOLOWER "${dll}" dll_lower)
        if(dll_lower MATCHES "^api-ms-" OR dll_lower MATCHES "^ext-ms-")
            continue()
        endif()
        if(EXISTS "${MINGW_BIN_DIR}/${dll}" AND NOT "${dll}" IN_LIST ALL_DLLS)
            list(APPEND ALL_DLLS "${dll}")
            file(COPY "${MINGW_BIN_DIR}/${dll}" DESTINATION "${DEPLOY_DIR}")
            list(APPEND TO_SCAN "${DEPLOY_DIR}/${dll}")
        endif()
    endforeach()
endwhile()
