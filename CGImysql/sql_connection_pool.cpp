#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include <unistd.h>
#include "sql_connection_pool.h"

using namespace std;

namespace
{
const int kPoolInitRetryTimes = 10;
const unsigned int kPoolInitRetryDelaySeconds = 1;
}

connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
	m_MaxConn = 0;
	m_idleTimeout = 60;
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

MYSQL *connection_pool::create_connection()
{
	MYSQL *con = mysql_init(NULL);
	if (con == NULL)
	{
		return NULL;
	}

	bool reconnect = true;
	mysql_options(con, MYSQL_OPT_RECONNECT, &reconnect);

	MYSQL *connected = mysql_real_connect(con, m_url.c_str(), m_User.c_str(), m_PassWord.c_str(),
											  m_DatabaseName.c_str(), atoi(m_Port.c_str()), NULL, 0);
	if (connected == NULL)
	{
		const char *error_message = mysql_error(con);
		LOG_ERROR("MySQL connect failed host=%s port=%s db=%s user=%s error=%s",
				  m_url.c_str(), m_Port.c_str(), m_DatabaseName.c_str(), m_User.c_str(),
				  error_message && error_message[0] ? error_message : "unknown");
		mysql_close(con);
		return NULL;
	}
	return connected;
}

bool connection_pool::reconnect_connection(MYSQL *&conn)
{
	if (conn != NULL)
	{
		lock.lock();
		m_connLastActive.erase(conn);
		lock.unlock();
		mysql_close(conn);
		conn = NULL;
	}

	conn = create_connection();
	if (conn == NULL)
	{
		return false;
	}

	lock.lock();
	m_connLastActive[conn] = time(NULL);
	lock.unlock();
	return true;
}

bool connection_pool::check_connection(MYSQL *conn)
{
	if (conn == NULL)
	{
		return false;
	}

	time_t now = time(NULL);
	lock.lock();
	map<MYSQL *, time_t>::iterator it = m_connLastActive.find(conn);
	if (it == m_connLastActive.end())
	{
		m_connLastActive[conn] = now;
		lock.unlock();
		return true;
	}

	if (now - it->second < m_idleTimeout)
	{
		lock.unlock();
		return true;
	}
	lock.unlock();

	if (mysql_ping(conn) != 0)
	{
		return false;
	}

	lock.lock();
	it->second = now;
	lock.unlock();
	return true;
}

//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log, int idle_timeout)
{
	static bool mysql_library_ready = false;
	if (!mysql_library_ready)
	{
		if (mysql_library_init(0, NULL, NULL) != 0)
		{
			LOG_ERROR("MySQL client library init failed");
			exit(1);
		}
		mysql_library_ready = true;
	}

	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;
	m_idleTimeout = idle_timeout > 0 ? idle_timeout : 60;

	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
		for (int attempt = 1; attempt <= kPoolInitRetryTimes; ++attempt)
		{
			con = create_connection();
			if (con != NULL)
			{
				break;
			}

			LOG_ERROR("MySQL pool init retry %d/%d for connection slot %d", attempt, kPoolInitRetryTimes, i + 1);
			sleep(kPoolInitRetryDelaySeconds);
		}

		if (con == NULL)
		{
			LOG_ERROR("MySQL pool init failed after retries, slot=%d/%d", i + 1, MaxConn);
			exit(1);
		}
		connList.push_back(con);
		m_connLastActive[con] = time(NULL);
		++m_FreeConn;
	}

	reserve.reset(m_FreeConn);

	m_MaxConn = m_FreeConn;
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	reserve.wait();
	
	lock.lock();
	if (connList.empty())
	{
		lock.unlock();
		return NULL;
	}

	con = connList.front();
	connList.pop_front();

	--m_FreeConn;
	++m_CurConn;

	lock.unlock();

	if (!check_connection(con) && !reconnect_connection(con))
	{
		lock.lock();
		--m_CurConn;
		--m_MaxConn;
		lock.unlock();
		return NULL;
	}

	lock.lock();
	m_connLastActive[con] = time(NULL);
	lock.unlock();
	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	bool healthy = check_connection(con) || reconnect_connection(con);
	lock.lock();
	if (!healthy)
	{
		--m_CurConn;
		--m_MaxConn;
		lock.unlock();
		return false;
	}

	connList.push_back(con);
	m_connLastActive[con] = time(NULL);
	++m_FreeConn;
	--m_CurConn;

	lock.unlock();

	reserve.post();
	return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			m_connLastActive.erase(con);
			mysql_close(con);
		}
		m_CurConn = 0;
		m_FreeConn = 0;
		m_MaxConn = 0;
		connList.clear();
	}

	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
	connRefRAII = SQL;
}

connectionRAII::~connectionRAII(){
	if (conRAII != NULL)
	{
		poolRAII->ReleaseConnection(conRAII);
		if (connRefRAII != NULL)
		{
			*connRefRAII = NULL;
		}
		conRAII = NULL;
	}
}
