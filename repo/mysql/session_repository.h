#ifndef REPO_MYSQL_SESSION_REPOSITORY_H
#define REPO_MYSQL_SESSION_REPOSITORY_H

#include <mysql/mysql.h>
#include <string>
#include <ctime>

namespace repo_mysql
{
struct SessionRecord
{
    std::string username;
    time_t expires_at;

    SessionRecord() : expires_at(0) {}
};

bool find_active_session(MYSQL *mysql, const std::string &token, SessionRecord &record);
bool cleanup_session_token_or_expired(MYSQL *mysql, const std::string &token);
bool upsert_session(MYSQL *mysql, const std::string &token, const std::string &username, int ttl_seconds);
bool refresh_session(MYSQL *mysql, const std::string &token, const std::string &username, int ttl_seconds, bool &updated);
bool delete_session(MYSQL *mysql, const std::string &token);
bool delete_user_sessions(MYSQL *mysql, const std::string &username, const std::string &except_token);
}

#endif
