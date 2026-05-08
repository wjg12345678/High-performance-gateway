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

server: main.cpp ./timer/lst_timer.cpp ./timer/heap_timer.cpp ./http/core/connection.cpp ./http/core/router.cpp ./http/files/file_helpers.cpp ./http/files/file_store.cpp ./http/files/file_service.cpp ./http/api/operation_service.cpp ./http/api/auth_state.cpp ./http/api/auth_session.cpp ./http/api/auth.cpp ./http/core/utils.cpp ./http/core/response.cpp ./http/core/io.cpp ./http/core/parser.cpp ./http/core/runtime.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp webserver.cpp webserver_sub_reactor.cpp config.cpp
	$(CXX) -o $@ $^ $(CXXFLAGS) $(LDFLAGS) $(LDLIBS)

server-sanitize: main.cpp ./timer/lst_timer.cpp ./timer/heap_timer.cpp ./http/core/connection.cpp ./http/core/router.cpp ./http/files/file_helpers.cpp ./http/files/file_store.cpp ./http/files/file_service.cpp ./http/api/operation_service.cpp ./http/api/auth_state.cpp ./http/api/auth_session.cpp ./http/api/auth.cpp ./http/core/utils.cpp ./http/core/response.cpp ./http/core/io.cpp ./http/core/parser.cpp ./http/core/runtime.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp webserver.cpp webserver_sub_reactor.cpp config.cpp
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
