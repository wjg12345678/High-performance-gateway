#include "config.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

volatile sig_atomic_t g_server_stop = 0;
volatile sig_atomic_t g_server_reload = 0;

namespace
{
volatile sig_atomic_t g_supervisor_stop = 0;
volatile sig_atomic_t g_worker_exit_signal = 0;
pid_t g_supervisor_worker_pid = -1;

const int DAEMON_RESTART_MAX_TIMES = 5;
const int DAEMON_RESTART_WINDOW_SECONDS = 60;
const int DAEMON_RESTART_DELAY_SECONDS = 2;
const int EXIT_CODE_RELOAD = 3;

string getenv_or_default(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value ? value : fallback;
}

int getenv_int_or_default(const char *name, int fallback)
{
    const char *value = getenv(name);
    return value ? atoi(value) : fallback;
}

bool process_exists(pid_t pid)
{
    if (pid <= 0)
    {
        return false;
    }
    if (kill(pid, 0) == 0)
    {
        return true;
    }
    return errno != ESRCH;
}

bool write_pid_file(const string &pid_file, pid_t pid)
{
    ofstream out(pid_file.c_str(), ios::out | ios::trunc);
    if (!out.is_open())
    {
        return false;
    }
    out << pid << '\n';
    return out.good();
}

pid_t read_pid_file(const string &pid_file)
{
    ifstream in(pid_file.c_str());
    if (!in.is_open())
    {
        return -1;
    }

    long pid = -1;
    in >> pid;
    if (!in.good() && !in.eof())
    {
        return -1;
    }
    return pid > 0 ? (pid_t)pid : -1;
}

void remove_pid_file_if_match(const string &pid_file, pid_t pid)
{
    pid_t current = read_pid_file(pid_file);
    if (current == pid)
    {
        unlink(pid_file.c_str());
    }
}

bool check_existing_supervisor(const Config &config)
{
    pid_t existing = read_pid_file(config.pid_file);
    if (existing <= 0)
    {
        return false;
    }

    if (process_exists(existing))
    {
        return true;
    }

    unlink(config.pid_file.c_str());
    return false;
}

bool daemonize_process()
{
    pid_t pid = fork();
    if (pid < 0)
    {
        return false;
    }
    if (pid > 0)
    {
        _exit(0);
    }

    if (setsid() < 0)
    {
        return false;
    }

    signal(SIGHUP, SIG_IGN);

    pid = fork();
    if (pid < 0)
    {
        return false;
    }
    if (pid > 0)
    {
        _exit(0);
    }

    umask(0);

    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd < 0)
    {
        return false;
    }

    if (dup2(null_fd, STDIN_FILENO) < 0 ||
        dup2(null_fd, STDOUT_FILENO) < 0 ||
        dup2(null_fd, STDERR_FILENO) < 0)
    {
        close(null_fd);
        return false;
    }

    if (null_fd > STDERR_FILENO)
    {
        close(null_fd);
    }

    return true;
}

void write_signal_message(int sig)
{
    switch (sig)
    {
    case SIGSEGV:
        write(STDERR_FILENO, "server received SIGSEGV\n", sizeof("server received SIGSEGV\n") - 1);
        break;
    case SIGABRT:
        write(STDERR_FILENO, "server received SIGABRT\n", sizeof("server received SIGABRT\n") - 1);
        break;
    case SIGBUS:
        write(STDERR_FILENO, "server received SIGBUS\n", sizeof("server received SIGBUS\n") - 1);
        break;
    case SIGFPE:
        write(STDERR_FILENO, "server received SIGFPE\n", sizeof("server received SIGFPE\n") - 1);
        break;
    case SIGILL:
        write(STDERR_FILENO, "server received SIGILL\n", sizeof("server received SIGILL\n") - 1);
        break;
    default:
        write(STDERR_FILENO, "server received fatal signal\n", sizeof("server received fatal signal\n") - 1);
        break;
    }
}

void supervisor_signal_handler(int sig)
{
    g_supervisor_stop = 1;
    if (g_supervisor_worker_pid > 0)
    {
        kill(g_supervisor_worker_pid, sig == SIGHUP ? SIGHUP : SIGTERM);
    }
}

void worker_term_handler(int sig)
{
    if (sig == SIGHUP)
    {
        g_server_reload = 1;
        return;
    }

    g_server_stop = 1;
    g_worker_exit_signal = sig;
}

void worker_fatal_handler(int sig)
{
    write_signal_message(sig);
    signal(sig, SIG_DFL);
    raise(sig);
}

void install_supervisor_signal_handlers()
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = supervisor_signal_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    signal(SIGPIPE, SIG_IGN);
}

void install_worker_signal_handlers()
{
    struct sigaction term_action;
    memset(&term_action, 0, sizeof(term_action));
    term_action.sa_handler = worker_term_handler;
    sigemptyset(&term_action.sa_mask);

    sigaction(SIGTERM, &term_action, NULL);
    sigaction(SIGINT, &term_action, NULL);
    sigaction(SIGHUP, &term_action, NULL);

    struct sigaction fatal_action;
    memset(&fatal_action, 0, sizeof(fatal_action));
    fatal_action.sa_handler = worker_fatal_handler;
    sigemptyset(&fatal_action.sa_mask);
    fatal_action.sa_flags = SA_RESETHAND;

    sigaction(SIGSEGV, &fatal_action, NULL);
    sigaction(SIGABRT, &fatal_action, NULL);
    sigaction(SIGBUS, &fatal_action, NULL);
    sigaction(SIGFPE, &fatal_action, NULL);
    sigaction(SIGILL, &fatal_action, NULL);

    signal(SIGPIPE, SIG_IGN);
}

int run_server_process(const Config &config)
{
    g_server_stop = 0;
    g_server_reload = 0;
    g_worker_exit_signal = 0;
    install_worker_signal_handlers();

    //数据库配置，环境变量优先覆盖配置文件
    string db_host = getenv_or_default("TWS_DB_HOST", config.db_host.c_str());
    int db_port = getenv_int_or_default("TWS_DB_PORT", config.db_port);
    string user = getenv_or_default("TWS_DB_USER", config.db_user.c_str());
    string passwd = getenv_or_default("TWS_DB_PASSWORD", config.db_password.c_str());
    string databasename = getenv_or_default("TWS_DB_NAME", config.db_name.c_str());

    //鉴权配置，环境变量优先覆盖配置文件
    string auth_token = getenv_or_default("TWS_AUTH_TOKEN", config.auth_token.c_str());

    WebServer server;

    //初始化
    server.init(config.PORT, user, passwd, databasename, db_host, db_port, config.LOGWrite,
                config.OPT_LINGER, config.TRIGMode, config.sql_num, config.thread_num,
                config.threadpool_max_threads, config.threadpool_idle_timeout,
                config.mysql_idle_timeout, config.conn_timeout,
                config.close_log, config.actor_model, config.log_level,
                config.log_split_lines, config.log_queue_size,
                config.https_enable, config.https_cert_file, config.https_key_file,
                auth_token);

    //日志
    server.log_write();
    if (0 == config.close_log)
    {
        Log::get_instance()->write_log(1, "server worker started, pid=%d, port=%d, daemon=%d", getpid(), config.PORT, config.daemon_mode);
        Log::get_instance()->flush();
    }

    if (!server.tls_init())
    {
        return 1;
    }

    //数据库
    server.sql_pool();

    //线程池
    server.thread_pool();

    //触发模式
    server.trig_mode();

    //监听
    server.eventListen();

    //运行
    server.eventLoop();

    if (0 == config.close_log)
    {
        if (g_server_reload)
        {
            Log::get_instance()->write_log(2, "server worker reload requested, pid=%d", getpid());
        }
        else if (g_worker_exit_signal != 0)
        {
            Log::get_instance()->write_log(2, "server worker stopping by signal %d, pid=%d", g_worker_exit_signal, getpid());
        }
        else
        {
            Log::get_instance()->write_log(1, "server worker stopped normally, pid=%d", getpid());
        }
        Log::get_instance()->flush();
    }

    return g_server_reload ? EXIT_CODE_RELOAD : 0;
}

int run_daemon_supervisor(const Config &config)
{
    if (check_existing_supervisor(config))
    {
        return 1;
    }

    if (!daemonize_process())
    {
        return 1;
    }

    if (!write_pid_file(config.pid_file, getpid()))
    {
        return 1;
    }

    install_supervisor_signal_handlers();

    int restart_count = 0;
    time_t window_start = 0;

    while (!g_supervisor_stop)
    {
        pid_t pid = fork();
        if (pid < 0)
        {
            sleep(1);
            continue;
        }

        if (pid == 0)
        {
            return run_server_process(config);
        }

        g_supervisor_worker_pid = pid;

        int status = 0;
        bool wait_ok = false;
        while (!wait_ok)
        {
            pid_t waited = waitpid(pid, &status, 0);
            if (waited == pid)
            {
                wait_ok = true;
                break;
            }

            if (waited < 0 && errno == EINTR)
            {
                if (g_supervisor_stop && g_supervisor_worker_pid > 0)
                {
                    kill(g_supervisor_worker_pid, SIGTERM);
                }
                continue;
            }

            return 1;
        }

        g_supervisor_worker_pid = -1;

        if (g_supervisor_stop)
        {
            remove_pid_file_if_match(config.pid_file, getpid());
            return 0;
        }

        if (WIFEXITED(status))
        {
            int exit_code = WEXITSTATUS(status);
            if (exit_code == 0)
            {
                return 0;
            }
            if (exit_code == EXIT_CODE_RELOAD)
            {
                restart_count = 0;
                window_start = 0;
                sleep(1);
                continue;
            }
        }

        time_t now = time(NULL);
        if (window_start == 0 || now - window_start > DAEMON_RESTART_WINDOW_SECONDS)
        {
            window_start = now;
            restart_count = 0;
        }

        ++restart_count;
        if (restart_count > DAEMON_RESTART_MAX_TIMES)
        {
            remove_pid_file_if_match(config.pid_file, getpid());
            return 1;
        }

        sleep(DAEMON_RESTART_DELAY_SECONDS);
    }

    remove_pid_file_if_match(config.pid_file, getpid());
    return 0;
}
}

int main(int argc, char *argv[])
{
    //命令行解析
    Config config;
    config.parse_arg(argc, argv);

    if (config.daemon_mode)
    {
        return run_daemon_supervisor(config);
    }

    return run_server_process(config);
}
