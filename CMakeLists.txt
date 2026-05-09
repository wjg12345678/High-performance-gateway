cmake_minimum_required(VERSION 3.16)

project(AtlasWebServer VERSION 1.0.0 LANGUAGES CXX)

option(ATLAS_ENABLE_SANITIZERS "Build targets with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ATLAS_ENABLE_COVERAGE "Build parser tests with gcov coverage instrumentation" OFF)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
    pkg_check_modules(MYSQLCLIENT mysqlclient)
endif()

if(NOT MYSQLCLIENT_FOUND)
    find_path(MYSQLCLIENT_INCLUDE_DIR mysql/mysql.h)
    find_library(MYSQLCLIENT_LIBRARY mysqlclient)
    if(NOT MYSQLCLIENT_INCLUDE_DIR OR NOT MYSQLCLIENT_LIBRARY)
        message(FATAL_ERROR "mysqlclient development files not found")
    endif()
    set(MYSQLCLIENT_INCLUDE_DIRS "${MYSQLCLIENT_INCLUDE_DIR}")
    set(MYSQLCLIENT_LIBRARIES "${MYSQLCLIENT_LIBRARY}")
endif()

set(ATLAS_SERVER_SOURCES
    app/main.cpp
    app/webserver.cpp
    app/webserver_sub_reactor.cpp
    app/config.cpp
    infra/timer/lst_timer.cpp
    infra/timer/heap_timer.cpp
    infra/log/log.cpp
    infra/db/sql_connection_pool.cpp
    infra/storage/storage.cpp
    http/core/connection.cpp
    http/controllers/auth_controller.cpp
    http/controllers/file_controller.cpp
    http/controllers/operation_controller.cpp
    http/router/router.cpp
    http/files/file_helpers.cpp
    http/files/file_store.cpp
    http/files/multipart_parser.cpp
    http/files/file_service.cpp
    http/api/operation_service.cpp
    http/api/auth_session.cpp
    http/api/auth.cpp
    http/core/utils.cpp
    http/core/response.cpp
    http/core/io.cpp
    http/core/parser.cpp
    http/core/runtime.cpp
    service/files/file_service.cpp
    service/auth/auth_service.cpp
    repo/mysql/mysql_utils.cpp
    repo/mysql/user_repository.cpp
    repo/mysql/session_repository.cpp
    repo/mysql/operation_repository.cpp
    repo/mysql/file_repository.cpp
)

add_executable(server ${ATLAS_SERVER_SOURCES})
target_include_directories(server PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
target_link_libraries(server PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto ${MYSQLCLIENT_LIBRARIES})

add_executable(parser-chunked-test tests/parser_chunked_test.cpp)
target_include_directories(parser-chunked-test PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
target_link_libraries(parser-chunked-test PRIVATE Threads::Threads)

if(ATLAS_ENABLE_SANITIZERS)
    target_compile_options(server PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(server PRIVATE -fsanitize=address,undefined)
    target_compile_options(parser-chunked-test PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(parser-chunked-test PRIVATE -fsanitize=address,undefined)
endif()

if(ATLAS_ENABLE_COVERAGE)
    target_compile_options(parser-chunked-test PRIVATE -O0 -g --coverage)
    target_link_options(parser-chunked-test PRIVATE --coverage)
endif()

enable_testing()
add_test(NAME parser_chunked COMMAND parser-chunked-test)
