#ifndef REPO_MYSQL_USER_REPOSITORY_H
#define REPO_MYSQL_USER_REPOSITORY_H

#include <mysql/mysql.h>
#include <map>
#include <string>

namespace repo_mysql
{
struct UserPasswordRecord
{
    std::string password;
    std::string salt;
};

bool insert_user(MYSQL *mysql, const std::string &username, const std::string &password_record);
bool fetch_user_password(MYSQL *mysql, const std::string &username, UserPasswordRecord &record);
bool update_user_password(MYSQL *mysql, const std::string &username, const std::string &password_record);
bool fetch_all_user_passwords(MYSQL *mysql, std::map<std::string, std::string> &users);
}

#endif
