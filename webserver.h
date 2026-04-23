#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <string>
#include <memory>
#include <vector>
#include <deque>
#include <signal.h>
#include <openssl/ssl.h>

#include "./threadpool/threadpool.h"
#include "./http/core/connection.h"
#include "./timer/lst_timer.h"
#include "./timer/heap_timer.h"

const int MAX_FD = 65536;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //最小超时单位

extern volatile sig_atomic_t g_server_stop;
extern volatile sig_atomic_t g_server_reload;

class WebServer
{
public:
    WebServer();
    ~WebServer();

    void init(int port , string user, string passWord, string databaseName,
              string dbHost, int dbPort,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int threadpool_max_threads, int threadpool_idle_timeout,
              int mysql_idle_timeout, int conn_timeout,
              int close_log, int actor_model, int log_level, int log_split_lines, int log_queue_size,
              const string &threadpool_queue_mode,
              int https_enable, const string &https_cert_file, const string &https_key_file,
              const string &auth_token);

    void thread_pool();
    void sql_pool();
    void log_write();
    bool tls_init();
    void trig_mode();
    void init_sub_reactors();
    void eventListen();
    void eventLoop();
    bool dealclientdata();
    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);

    class SubReactor
    {
    public:
        SubReactor();
        bool init(WebServer *server, int index);
        bool start();
        void stop();
        void wait();
        bool dispatch(int connfd);

    private:
        static void *worker(void *arg);
        void run();
        void register_connection(int connfd);
        void handle_notify();
        void scan_timeout();
        void refresh_timer(int sockfd);
        int next_wait_timeout_ms();
        void remove_connection(int sockfd);

    private:
        WebServer *m_server;
        int m_index;
        int m_epollfd;
        int m_notifyfd;
        pthread_t m_tid;
        bool m_stop;
        locker m_pending_lock;
        std::deque<int> m_pending_connections;
        HeapTimer m_timer_heap;
    };

public:
    //基础
    int m_port;
    std::string m_root;
    int m_log_write;
    int m_log_level;
    int m_log_split_lines;
    int m_log_queue_size;
    int m_close_log;
    int m_actormodel;
    int m_https_enable;
    string m_https_cert_file;
    string m_https_key_file;
    string m_auth_token;
    SSL_CTX *m_ssl_ctx;

    int m_epollfd;
    std::vector<HttpConnection> users;
    std::vector<sockaddr_in> m_pending_addresses;

    //数据库相关
    connection_pool *m_connPool;
    string m_user;         //登陆数据库用户名
    string m_passWord;     //登陆数据库密码
    string m_databaseName; //使用数据库名
    string m_dbHost;       //数据库主机
    int m_dbPort;          //数据库端口
    int m_sql_num;
    int m_mysql_idle_timeout;

    //线程池相关
    std::unique_ptr<threadpool<HttpConnection>> m_pool;
    int m_thread_num;
    int m_threadpool_max_threads;
    int m_threadpool_idle_timeout;
    string m_threadpool_queue_mode;

    //epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];

    int m_listenfd;
    int m_OPT_LINGER;
    int m_TRIGMode;
    int m_LISTENTrigmode;
    int m_CONNTrigmode;
    int m_conn_timeout;

    int m_sub_reactor_num;
    int m_next_sub_reactor;
    std::vector<SubReactor> m_sub_reactors;

    Utils utils;
};
#endif
