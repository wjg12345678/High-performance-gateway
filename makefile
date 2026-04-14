CXX ?= g++

DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g
else
    CXXFLAGS += -O2

endif

server: main.cpp ./timer/lst_timer.cpp ./timer/heap_timer.cpp ./http/core/connection.cpp ./http/core/router.cpp ./http/files/file_helpers.cpp ./http/files/file_store.cpp ./http/files/file_service.cpp ./http/api/operation_service.cpp ./http/api/auth_state.cpp ./http/api/auth_session.cpp ./http/api/auth.cpp ./http/core/utils.cpp ./http/core/response.cpp ./http/core/io.cpp ./http/core/parser.cpp ./http/core/runtime.cpp ./log/log.cpp ./CGImysql/sql_connection_pool.cpp webserver.cpp webserver_sub_reactor.cpp config.cpp
	$(CXX) -o server  $^ $(CXXFLAGS) -lpthread -lmysqlclient -lssl -lcrypto

clean:
	rm  -r server
