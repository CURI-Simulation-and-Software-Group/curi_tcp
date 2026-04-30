cmake_minimum_required(VERSION 3.2.0)

# Only call project() if this is the root project
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(curi_tcp LANGUAGES C CXX)
endif()

###################################Add library###########################################
add_library(CURI_TCP_LIB
    ${CMAKE_CURRENT_SOURCE_DIR}/src/curi_tcp.c
)
target_include_directories(CURI_TCP_LIB PUBLIC 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

###################################Generate Executable###################################
#Generate Executable if this CMakeLists isn't a subdirectory 
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    link_libraries(CURI_TCP_LIB)
    add_executable(client
        ${CMAKE_CURRENT_SOURCE_DIR}/example/client.c
    )
    add_executable(client1
        ${CMAKE_CURRENT_SOURCE_DIR}/example/client1.c
    )
    add_executable(server_nonstop
        ${CMAKE_CURRENT_SOURCE_DIR}/example/server_nonstop.c
    )
    add_executable(server
        ${CMAKE_CURRENT_SOURCE_DIR}/example/server.c
    )
endif()