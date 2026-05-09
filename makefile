CXX ?= g++
LDLIBS += -lpthread -lmysqlclient -lssl -lcrypto
SANITIZER_FLAGS := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
ASAN_PRELOAD ?= $(shell $(CXX) -print-file-name=libasan.so)

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2
endif

SERVER_SRCS := \
    app/main.cpp \
    app/webserver.cpp \
    app/webserver_sub_reactor.cpp \
    app/config.cpp \
    infra/timer/lst_timer.cpp \
    infra/timer/heap_timer.cpp \
    infra/log/log.cpp \
    infra/db/sql_connection_pool.cpp \
    infra/storage/storage.cpp \
    http/core/connection.cpp \
    http/router/router.cpp \
    http/files/file_helpers.cpp \
    http/files/file_store.cpp \
    http/files/multipart_parser.cpp \
    http/files/file_service.cpp \
    http/api/operation_service.cpp \
    http/api/auth_session.cpp \
    http/api/auth.cpp \
    http/core/utils.cpp \
    http/core/response.cpp \
    http/core/io.cpp \
    http/core/parser.cpp \
    http/core/runtime.cpp \
    service/files/file_service.cpp \
    service/auth/auth_service.cpp \
    repo/mysql/mysql_utils.cpp \
    repo/mysql/user_repository.cpp \
    repo/mysql/session_repository.cpp \
    repo/mysql/operation_repository.cpp \
    repo/mysql/file_repository.cpp

server: $(SERVER_SRCS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LDFLAGS) $(LDLIBS)

server-sanitize: $(SERVER_SRCS)
	$(CXX) -o $@ $^ $(CXXFLAGS) $(SANITIZER_FLAGS) $(LDFLAGS) $(SANITIZER_FLAGS) $(LDLIBS)

parser-chunked-test: tests/parser_chunked_test.cpp http/core/parser.cpp http/core/connection.h
	$(CXX) -o $@ tests/parser_chunked_test.cpp $(CXXFLAGS) $(LDFLAGS) -lpthread

test-parser: parser-chunked-test
	./parser-chunked-test

parser-chunked-test-sanitize: tests/parser_chunked_test.cpp http/core/parser.cpp http/core/connection.h
	$(CXX) $(SANITIZER_FLAGS) -o $@ tests/parser_chunked_test.cpp $(CXXFLAGS) $(LDFLAGS) $(SANITIZER_FLAGS) -lpthread

test-parser-sanitize: parser-chunked-test-sanitize
	LD_PRELOAD="$(ASAN_PRELOAD)$${LD_PRELOAD:+:$${LD_PRELOAD}}" ASAN_OPTIONS=$${ASAN_OPTIONS:-detect_leaks=0:halt_on_error=1} UBSAN_OPTIONS=$${UBSAN_OPTIONS:-halt_on_error=1} ./parser-chunked-test-sanitize

clean:
	rm -f server server-sanitize parser-chunked-test parser-chunked-test-sanitize
