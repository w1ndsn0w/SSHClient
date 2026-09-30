cmake_minimum_required(VERSION 3.22)
project(SSHClient LANGUAGES C CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "This first version uses the Windows desktop API.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LIBSSH2_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(CRYPTO_BACKEND WinCNG CACHE STRING "" FORCE)
set(ENABLE_ECDSA_WINCNG ON CACHE BOOL "" FORCE)
FetchContent_Declare(libssh2
    GIT_REPOSITORY https://github.com/libssh2/libssh2.git
    GIT_TAG libssh2-1.11.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libssh2)

add_executable(SSHClient WIN32 main.cpp sftp_client.cpp server_profiles.cpp app.rc)
target_include_directories(SSHClient PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_source_files_properties(app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/app.ico")
target_compile_definitions(SSHClient PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(SSHClient PRIVATE libssh2_static ws2_32 bcrypt comctl32 comdlg32 shell32 ole32 uuid)
if(MINGW)
    target_link_options(SSHClient PRIVATE -municode)
    get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(runtime_dll libwinpthread-1.dll libgcc_s_seh-1.dll libstdc++-6.dll)
        if(EXISTS "${MINGW_BIN_DIR}/${runtime_dll}")
            add_custom_command(TARGET SSHClient POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MINGW_BIN_DIR}/${runtime_dll}" "$<TARGET_FILE_DIR:SSHClient>")
        endif()
    endforeach()
endif()
