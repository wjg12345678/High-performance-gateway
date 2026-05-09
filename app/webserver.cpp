#include "webserver.h"

#include <openssl/err.h>

#include "../infra/db/sql_connection_pool.h"

namespace
{
bool execute_optional_migration(MYSQL *mysql, const char *sql)
{
    if (mysql == nullptr || sql == nullptr)
    {
        return false;
    }

    if (mysql_query(mysql, sql) == 0)
    {
        return true;
    }

    const unsigned int err = mysql_errno(mysql);
    return err == 1060 || err == 1061 || err == 1091;
}
}

WebServer::WebServer()
    : m_https_enable(0), m_ssl_ctx(nullptr), m_epollfd(-1), m_connPool(nullptr),
      m_listenfd(-1), m_sub_reactor_num(1), m_next_sub_reactor(0), m_upload_max_bytes(100 * 1024 * 1024),
      m_user_storage_quota_bytes(1024L * 1024L * 1024L)
{
    users.resize(MAX_FD);
    m_pending_addresses.resize(MAX_FD);

    char server_path[200];
    getcwd(server_path, 200);
    m_root = std::string(server_path) + "/webroot";
}

WebServer::~WebServer()
{
    for (size_t i = 0; i < m_sub_reactors.size(); ++i)
    {
        m_sub_reactors[i].stop();
    }
    for (size_t i = 0; i < m_sub_reactors.size(); ++i)
    {
        m_sub_reactors[i].wait();
    }
    if (m_epollfd != -1)
    {
        close(m_epollfd);
    }
    if (m_listenfd != -1)
    {
        close(m_listenfd);
    }
    if (m_ssl_ctx != nullptr)
    {
        SSL_CTX_free(m_ssl_ctx);
        m_ssl_ctx = nullptr;
    }
}

void WebServer::init(int port, string user, string passWord, string databaseName, string dbHost, int dbPort, int log_write,
                     int opt_linger, int trigmode, int sql_num, int thread_num, int threadpool_max_threads,
                     int threadpool_idle_timeout, int mysql_idle_timeout, int upload_max_bytes,
                     long user_storage_quota_bytes, int conn_timeout,
                     int close_log, int actor_model, int log_level, int log_split_lines, int log_queue_size,
                     const string &threadpool_queue_mode,
                     int https_enable, const string &https_cert_file, const string &https_key_file,
                     int legacy_compat)
{
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_dbHost = dbHost;
    m_dbPort = dbPort;
    m_sql_num = sql_num;
    m_mysql_idle_timeout = mysql_idle_timeout;
    m_upload_max_bytes = upload_max_bytes > 0 ? upload_max_bytes : 100 * 1024 * 1024;
    m_user_storage_quota_bytes = user_storage_quota_bytes >= 0 ? user_storage_quota_bytes : 1024L * 1024L * 1024L;
    m_thread_num = thread_num;
    m_threadpool_max_threads = threadpool_max_threads;
    m_threadpool_idle_timeout = threadpool_idle_timeout;
    m_threadpool_queue_mode = threadpool_queue_mode;
    m_log_write = log_write;
    m_log_level = log_level;
    m_log_split_lines = log_split_lines;
    m_log_queue_size = log_queue_size;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;
    m_https_enable = https_enable;
    m_https_cert_file = https_cert_file;
    m_https_key_file = https_key_file;
    m_conn_timeout = conn_timeout > 0 ? conn_timeout : 15;
    m_sub_reactor_num = thread_num > 0 ? thread_num : 1;
    m_next_sub_reactor = 0;
    HttpConnection::set_legacy_compat(legacy_compat != 0);
    HttpConnection::configure_uploads(static_cast<size_t>(m_upload_max_bytes),
                                      static_cast<size_t>(m_user_storage_quota_bytes));
}

bool WebServer::tls_init()
{
    if (!m_https_enable)
    {
        HttpConnection::configure_tls(nullptr, false);
        return true;
    }

    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    const SSL_METHOD *method = TLS_server_method();
    m_ssl_ctx = SSL_CTX_new(method);
    if (m_ssl_ctx == nullptr)
    {
        return false;
    }

    SSL_CTX_set_min_proto_version(m_ssl_ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(m_ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);

    if (SSL_CTX_use_certificate_file(m_ssl_ctx, m_https_cert_file.c_str(), SSL_FILETYPE_PEM) <= 0)
    {
        return false;
    }

    if (SSL_CTX_use_PrivateKey_file(m_ssl_ctx, m_https_key_file.c_str(), SSL_FILETYPE_PEM) <= 0)
    {
        return false;
    }

    if (SSL_CTX_check_private_key(m_ssl_ctx) != 1)
    {
        return false;
    }

    HttpConnection::configure_tls(m_ssl_ctx, true);
    return true;
}

void WebServer::trig_mode()
{
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, m_log_split_lines, m_log_queue_size, m_log_level);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, m_log_split_lines, 0, m_log_level);
    }
}

void WebServer::sql_pool()
{
    m_connPool = connection_pool::GetInstance();
    m_connPool->init(m_dbHost, m_user, m_passWord, m_databaseName, m_dbPort, m_sql_num, m_close_log, m_mysql_idle_timeout);
    MYSQL *mysql = nullptr;
    connectionRAII mysqlcon(&mysql, m_connPool);
    if (mysql != nullptr)
    {
        execute_optional_migration(mysql, "CREATE TABLE IF NOT EXISTS folders (id BIGINT NOT NULL AUTO_INCREMENT, owner_username VARCHAR(50) NOT NULL, parent_id BIGINT NOT NULL DEFAULT 0, name VARCHAR(128) NOT NULL, deleted_marker BIGINT NOT NULL DEFAULT 0, deleted_at TIMESTAMP NULL DEFAULT NULL, created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY (id), UNIQUE KEY uq_owner_parent_name_active (owner_username, parent_id, name, deleted_marker), KEY idx_owner_parent_deleted (owner_username, parent_id, deleted_at, id), KEY idx_owner_parent_name_deleted (owner_username, parent_id, name, deleted_at)) ENGINE=InnoDB");
        execute_optional_migration(mysql, "ALTER TABLE folders ADD COLUMN deleted_marker BIGINT NOT NULL DEFAULT 0");
        execute_optional_migration(mysql, "ALTER TABLE folders ADD UNIQUE KEY uq_owner_parent_name_active (owner_username, parent_id, name, deleted_marker)");
        execute_optional_migration(mysql, "ALTER TABLE folders ADD KEY idx_owner_parent_deleted (owner_username, parent_id, deleted_at, id)");
        execute_optional_migration(mysql, "ALTER TABLE folders ADD KEY idx_owner_parent_name_deleted (owner_username, parent_id, name, deleted_at)");
        execute_optional_migration(mysql, "CREATE TABLE IF NOT EXISTS physical_files (id BIGINT NOT NULL AUTO_INCREMENT, sha256 CHAR(64) NOT NULL, stored_name VARCHAR(128) NOT NULL, file_size BIGINT NOT NULL DEFAULT 0, ref_count BIGINT NOT NULL DEFAULT 0, created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY (id), UNIQUE KEY uq_physical_sha256 (sha256), UNIQUE KEY uq_physical_stored_name (stored_name)) ENGINE=InnoDB");
        execute_optional_migration(mysql, "ALTER TABLE files ADD COLUMN folder_id BIGINT NOT NULL DEFAULT 0");
        execute_optional_migration(mysql, "ALTER TABLE files ADD COLUMN physical_id BIGINT NOT NULL DEFAULT 0");
        execute_optional_migration(mysql, "ALTER TABLE files ADD COLUMN is_public TINYINT(1) NOT NULL DEFAULT 0");
        execute_optional_migration(mysql, "ALTER TABLE files ADD COLUMN content_sha256 CHAR(64) NOT NULL DEFAULT ''");
        execute_optional_migration(mysql, "ALTER TABLE files ADD COLUMN deleted_at TIMESTAMP NULL DEFAULT NULL");
        execute_optional_migration(mysql, "ALTER TABLE files ADD KEY idx_owner_deleted_id (owner_username, deleted_at, id)");
        execute_optional_migration(mysql, "ALTER TABLE files ADD KEY idx_owner_folder_deleted_name (owner_username, folder_id, deleted_at, original_name)");
        execute_optional_migration(mysql, "ALTER TABLE files ADD KEY idx_physical_id (physical_id)");
        execute_optional_migration(mysql, "ALTER TABLE files ADD KEY idx_public_deleted_id (is_public, deleted_at, id)");
        execute_optional_migration(mysql, "ALTER TABLE files ADD KEY idx_owner_name_deleted (owner_username, original_name, deleted_at)");
        execute_optional_migration(mysql, "ALTER TABLE files DROP INDEX idx_owner_username");
        execute_optional_migration(mysql, "INSERT IGNORE INTO physical_files(sha256, stored_name, file_size, ref_count) SELECT content_sha256, MIN(stored_name), MAX(file_size), SUM(CASE WHEN deleted_at IS NULL THEN 1 ELSE 0 END) FROM files WHERE content_sha256<>'' GROUP BY content_sha256");
        execute_optional_migration(mysql, "UPDATE files f JOIN physical_files p ON f.content_sha256=p.sha256 SET f.physical_id=p.id, f.stored_name=p.stored_name WHERE f.physical_id=0 AND f.content_sha256<>''");
    }
    users[0].initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    threadpool<HttpConnection>::queue_mode mode = threadpool<HttpConnection>::queue_mode_mutex;
    if (m_threadpool_queue_mode == "lockfree")
    {
        mode = threadpool<HttpConnection>::queue_mode_lockfree;
    }

    m_pool = std::make_unique<threadpool<HttpConnection>>(0, m_connPool, m_thread_num, 10000,
                                                          m_threadpool_max_threads, m_threadpool_idle_timeout,
                                                          mode);
}

void WebServer::init_sub_reactors()
{
    m_sub_reactors.resize(m_sub_reactor_num);
    for (int i = 0; i < m_sub_reactor_num; ++i)
    {
        bool ok = m_sub_reactors[i].init(this, i) && m_sub_reactors[i].start();
        assert(ok);
    }
}

void WebServer::eventListen()
{
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    int ret = 0;
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);

    utils.init(m_conn_timeout);

    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    init_sub_reactors();
}

bool WebServer::dealclientdata()
{
    bool accepted = false;

    while (true)
    {
        struct sockaddr_in client_address;
        socklen_t client_addrlength = sizeof(client_address);
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            if ((errno == EAGAIN || errno == EWOULDBLOCK) && 1 == m_LISTENTrigmode)
            {
                break;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
            }
            return accepted;
        }

        if (HttpConnection::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
        }
        else
        {
            SubReactor &reactor = m_sub_reactors[m_next_sub_reactor];
            m_next_sub_reactor = (m_next_sub_reactor + 1) % m_sub_reactor_num;
            m_pending_addresses[connfd] = client_address;
            if (!reactor.dispatch(connfd))
            {
                utils.show_error(connfd, "Dispatch connection failed");
                LOG_ERROR("%s", "dispatch connection failed");
            }
            else
            {
                accepted = true;
            }
        }

        if (0 == m_LISTENTrigmode)
        {
            break;
        }
    }
    return accepted;
}

void WebServer::dealwithread(int sockfd)
{
    if (users[sockfd].needs_tls_handshake())
    {
        int handshake_result = users[sockfd].do_tls_handshake();
        if (handshake_result < 0)
        {
            users[sockfd].close_conn();
        }
        return;
    }

    if (1 == m_actormodel)
    {
        if (!m_pool->append(&users[sockfd], 0))
        {
            users[sockfd].close_conn();
        }
        return;
    }

    users[sockfd].lock_request();
    const bool read_ok = users[sockfd].read_once();
    users[sockfd].unlock_request();

    if (read_ok)
    {
        LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
        if (!m_pool->append_p(&users[sockfd]))
        {
            users[sockfd].close_conn();
        }
        return;
    }

    users[sockfd].close_conn();
}

void WebServer::dealwithwrite(int sockfd)
{
    if (users[sockfd].needs_tls_handshake())
    {
        int handshake_result = users[sockfd].do_tls_handshake();
        if (handshake_result < 0)
        {
            users[sockfd].close_conn();
        }
        return;
    }

    if (1 == m_actormodel)
    {
        if (!m_pool->append(&users[sockfd], 1))
        {
            users[sockfd].close_conn();
        }
        return;
    }

    users[sockfd].lock_request();
    const bool write_ok = users[sockfd].write();
    users[sockfd].unlock_request();

    if (write_ok)
    {
        LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
        return;
    }

    users[sockfd].close_conn();
}

void WebServer::eventLoop()
{
    while (!g_server_stop && !g_server_reload)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0)
        {
            if (errno == EINTR)
            {
                if (g_server_stop || g_server_reload)
                {
                    break;
                }
                continue;
            }
            LOG_ERROR("%s", "main reactor epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == m_listenfd)
            {
                dealclientdata();
            }
        }
    }
}
