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
    content_sha256 CHAR(64) NOT NULL DEFAULT '',
    deleted_at TIMESTAMP NULL DEFAULT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_owner_deleted_id (owner_username, deleted_at, id),
    KEY idx_public_deleted_id (is_public, deleted_at, id),
    KEY idx_owner_name_deleted (owner_username, original_name, deleted_at)
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
       'pbkdf2_sha256$210000$5f0b5c0e6f4d9a71c2e84f10aa7d3912$69b9ececeda544f749543a4b941fd186bb779d96d9c54ff0dbb09294a7fe4108',
       ''
WHERE NOT EXISTS (
    SELECT 1 FROM user WHERE username = 'name'
);
