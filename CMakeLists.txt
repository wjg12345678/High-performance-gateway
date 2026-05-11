cmake_minimum_required(VERSION 3.16)

project(AtlasWebServer VERSION 1.0.0 LANGUAGES CXX)

option(ATLAS_ENABLE_SANITIZERS "Build targets with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ATLAS_ENABLE_COVERAGE "Build parser tests with gcov coverage instrumentation" OFF)

set(CMAKE_CXX_STANDARD 17)
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

set(ATLAS_WITH_REDIS_LIMITER 0)
set(ATLAS_RATE_LIMITER_SOURCES
    service/rate_limit/auth_rate_limiter.cpp
)
set(ATLAS_RATE_LIMITER_INCLUDE_DIRS "")
set(ATLAS_RATE_LIMITER_LIBRARIES "")

find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIBRARY hiredis)

if(HIREDIS_INCLUDE_DIR AND HIREDIS_LIBRARY)
    set(ATLAS_WITH_REDIS_LIMITER 1)
    list(APPEND ATLAS_RATE_LIMITER_SOURCES
        third_party/redis_limiter/src/redis_pool.cpp
        third_party/redis_limiter/src/sliding_window_limiter.cpp
    )
    list(APPEND ATLAS_RATE_LIMITER_INCLUDE_DIRS
        third_party/redis_limiter/include
        ${HIREDIS_INCLUDE_DIR}
    )
    list(APPEND ATLAS_RATE_LIMITER_LIBRARIES
        ${HIREDIS_LIBRARY}
    )
else()
    message(WARNING "hiredis not found; auth rate limiter will use local fallback only")
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
    http/core/http_message.cpp
    http/controllers/auth_controller.cpp
    http/controllers/file_controller.cpp
    http/controllers/operation_controller.cpp
    http/router/router.cpp
    http/files/file_helpers.cpp
    http/files/file_store.cpp
    http/files/multipart_parser.cpp
    http/files/file_service.cpp
    http/core/utils.cpp
    http/core/response.cpp
    http/core/io.cpp
    http/core/parser.cpp
    http/core/runtime.cpp
    service/files/file_service.cpp
    service/files/share_service.cpp
    service/files/upload_service.cpp
    service/auth/auth_service.cpp
    ${ATLAS_RATE_LIMITER_SOURCES}
    repo/mysql/mysql_utils.cpp
    repo/mysql/user_repository.cpp
    repo/mysql/session_repository.cpp
    repo/mysql/operation_repository.cpp
    repo/mysql/file_repository.cpp
)

add_executable(server ${ATLAS_SERVER_SOURCES})
target_include_directories(server PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS} ${ATLAS_RATE_LIMITER_INCLUDE_DIRS})
target_compile_definitions(server PRIVATE ATLAS_WITH_REDIS_LIMITER=${ATLAS_WITH_REDIS_LIMITER})
target_link_libraries(server PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto ${MYSQLCLIENT_LIBRARIES} ${ATLAS_RATE_LIMITER_LIBRARIES})

add_executable(parser-chunked-test tests/parser_chunked_test.cpp)
target_include_directories(parser-chunked-test PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
target_link_libraries(parser-chunked-test PRIVATE Threads::Threads)

add_executable(http-message-test
    tests/http_message_test.cpp
    http/core/http_message.cpp
)
target_link_libraries(http-message-test PRIVATE OpenSSL::Crypto)

add_executable(file-helpers-test
    tests/file_helpers_test.cpp
    http/files/file_helpers.cpp
)

add_executable(log-smoke-test
    tests/log_smoke_test.cpp
    infra/log/log.cpp
)
target_link_libraries(log-smoke-test PRIVATE Threads::Threads)

add_executable(multipart-parser-test
    tests/multipart_parser_test.cpp
    http/files/multipart_parser.cpp
    http/files/file_helpers.cpp
    infra/storage/storage.cpp
)
target_link_libraries(multipart-parser-test PRIVATE OpenSSL::Crypto)

add_executable(upload-service-test
    tests/upload_service_test.cpp
    service/files/upload_service.cpp
    service/files/file_service.cpp
    http/files/file_helpers.cpp
    http/files/file_store.cpp
    infra/storage/storage.cpp
    repo/mysql/mysql_utils.cpp
    repo/mysql/file_repository.cpp
)
target_include_directories(upload-service-test PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
target_link_libraries(upload-service-test PRIVATE OpenSSL::Crypto ${MYSQLCLIENT_LIBRARIES})

add_executable(file-service-test
    tests/file_service_test.cpp
    service/files/file_service.cpp
    http/files/file_helpers.cpp
    http/files/file_store.cpp
    infra/storage/storage.cpp
    repo/mysql/mysql_utils.cpp
    repo/mysql/file_repository.cpp
)
target_include_directories(file-service-test PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
target_link_libraries(file-service-test PRIVATE OpenSSL::Crypto ${MYSQLCLIENT_LIBRARIES})

add_executable(share-service-test
    tests/share_service_test.cpp
    service/files/share_service.cpp
    service/files/file_service.cpp
    http/files/file_helpers.cpp
    http/files/file_store.cpp
    infra/storage/storage.cpp
    repo/mysql/mysql_utils.cpp
    repo/mysql/file_repository.cpp
)
target_include_directories(share-service-test PRIVATE ${MYSQLCLIENT_INCLUDE_DIRS})
target_link_libraries(share-service-test PRIVATE OpenSSL::Crypto ${MYSQLCLIENT_LIBRARIES})

add_custom_target(atlas-unit-tests
    DEPENDS
        parser-chunked-test
        http-message-test
        file-helpers-test
        log-smoke-test
        multipart-parser-test
        upload-service-test
        file-service-test
        share-service-test
)

if(ATLAS_ENABLE_SANITIZERS)
    target_compile_options(server PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(server PRIVATE -fsanitize=address,undefined)
    target_compile_options(parser-chunked-test PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(parser-chunked-test PRIVATE -fsanitize=address,undefined)
    target_compile_options(http-message-test PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(http-message-test PRIVATE -fsanitize=address,undefined)
    target_compile_options(file-helpers-test PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(file-helpers-test PRIVATE -fsanitize=address,undefined)
    target_compile_options(log-smoke-test PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(log-smoke-test PRIVATE -fsanitize=address,undefined)
    target_compile_options(multipart-parser-test PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(multipart-parser-test PRIVATE -fsanitize=address,undefined)
    target_compile_options(upload-service-test PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(upload-service-test PRIVATE -fsanitize=address,undefined)
    target_compile_options(file-service-test PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(file-service-test PRIVATE -fsanitize=address,undefined)
    target_compile_options(share-service-test PRIVATE -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(share-service-test PRIVATE -fsanitize=address,undefined)
endif()

if(ATLAS_ENABLE_COVERAGE)
    target_compile_options(parser-chunked-test PRIVATE -O0 -g --coverage)
    target_link_options(parser-chunked-test PRIVATE --coverage)
    target_compile_options(http-message-test PRIVATE -O0 -g --coverage)
    target_link_options(http-message-test PRIVATE --coverage)
    target_compile_options(file-helpers-test PRIVATE -O0 -g --coverage)
    target_link_options(file-helpers-test PRIVATE --coverage)
    target_compile_options(log-smoke-test PRIVATE -O0 -g --coverage)
    target_link_options(log-smoke-test PRIVATE --coverage)
    target_compile_options(multipart-parser-test PRIVATE -O0 -g --coverage)
    target_link_options(multipart-parser-test PRIVATE --coverage)
    target_compile_options(upload-service-test PRIVATE -O0 -g --coverage)
    target_link_options(upload-service-test PRIVATE --coverage)
    target_compile_options(file-service-test PRIVATE -O0 -g --coverage)
    target_link_options(file-service-test PRIVATE --coverage)
    target_compile_options(share-service-test PRIVATE -O0 -g --coverage)
    target_link_options(share-service-test PRIVATE --coverage)
endif()

enable_testing()
add_test(NAME parser_chunked COMMAND parser-chunked-test)
add_test(NAME http_message COMMAND http-message-test)
add_test(NAME file_helpers COMMAND file-helpers-test)
add_test(NAME log_smoke COMMAND log-smoke-test)
add_test(NAME multipart_parser COMMAND multipart-parser-test)
add_test(NAME upload_service COMMAND upload-service-test)
add_test(NAME file_service COMMAND file-service-test)
add_test(NAME share_service COMMAND share-service-test)
