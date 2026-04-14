USE qgydb;

CREATE TABLE IF NOT EXISTS user (
    username VARCHAR(50) NOT NULL,
    passwd VARCHAR(128) NOT NULL,
    passwd_salt VARCHAR(64) NOT NULL DEFAULT '',
    UNIQUE KEY uq_username (username)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS user_sessions (
    token VARCHAR(128) NOT NULL,
    username VARCHAR(50) NOT NULL,
    expires_at TIMESTAMP NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (token),
    KEY idx_username (username),
    KEY idx_expires_at (expires_at)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS files (
    id BIGINT NOT NULL AUTO_INCREMENT,
    owner_username VARCHAR(50) NOT NULL,
    stored_name VARCHAR(128) NOT NULL,
    original_name VARCHAR(128) NOT NULL,
    content_type VARCHAR(128) NOT NULL DEFAULT 'text/plain',
    file_size BIGINT NOT NULL DEFAULT 0,
    is_public TINYINT(1) NOT NULL DEFAULT 0,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_owner_username (owner_username)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS operation_logs (
    id BIGINT NOT NULL AUTO_INCREMENT,
    username VARCHAR(50) NOT NULL,
    action VARCHAR(64) NOT NULL,
    resource_type VARCHAR(64) NOT NULL,
    resource_id BIGINT NOT NULL DEFAULT 0,
    detail TEXT NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_username (username)
) ENGINE=InnoDB;

INSERT INTO user(username, passwd, passwd_salt)
SELECT 'name',
       SHA2(CONCAT('seed-salt', 'passwd'), 256),
       'seed-salt'
WHERE NOT EXISTS (
    SELECT 1 FROM user WHERE username = 'name'
);
