DROP TRIGGER IF EXISTS trg_files_after_insert;
DROP TRIGGER IF EXISTS trg_files_after_delete;

CREATE TABLE users_v005 (
    id BIGINT NOT NULL AUTO_INCREMENT,
    username VARCHAR(50) NOT NULL,
    password_hash VARCHAR(255) NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    last_login_at TIMESTAMP NULL DEFAULT NULL,
    disabled_at TIMESTAMP NULL DEFAULT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uq_users_username (username),
    KEY idx_users_disabled_at (disabled_at)
) ENGINE=InnoDB;

CREATE TABLE user_sessions_v005 (
    token VARCHAR(128) NOT NULL,
    user_id BIGINT NOT NULL,
    expires_at TIMESTAMP NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (token),
    KEY idx_sessions_user_id (user_id),
    KEY idx_sessions_expires_at (expires_at)
) ENGINE=InnoDB;

CREATE TABLE folders_v005 (
    id BIGINT NOT NULL AUTO_INCREMENT,
    user_id BIGINT NOT NULL,
    parent_id BIGINT NULL DEFAULT NULL,
    parent_key BIGINT GENERATED ALWAYS AS (COALESCE(parent_id, 0)) STORED,
    name VARCHAR(128) NOT NULL,
    deleted_marker BIGINT NOT NULL DEFAULT 0,
    deleted_at TIMESTAMP NULL DEFAULT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uq_folders_active_name (user_id, parent_key, name, deleted_marker),
    KEY idx_folders_user_parent_deleted (user_id, parent_key, deleted_at, id),
    KEY idx_folders_user_parent_name_deleted (user_id, parent_key, name, deleted_at),
    KEY idx_folders_parent_id (parent_id)
) ENGINE=InnoDB;

CREATE TABLE physical_files_v005 (
    id BIGINT NOT NULL AUTO_INCREMENT,
    sha256 CHAR(64) NOT NULL,
    stored_name VARCHAR(128) NOT NULL,
    file_size BIGINT NOT NULL DEFAULT 0,
    ref_count BIGINT NOT NULL DEFAULT 0,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uq_physical_sha256 (sha256),
    UNIQUE KEY uq_physical_stored_name (stored_name),
    CONSTRAINT chk_physical_file_size CHECK (file_size >= 0),
    CONSTRAINT chk_physical_ref_count CHECK (ref_count >= 0)
) ENGINE=InnoDB;

CREATE TABLE files_v005 (
    id BIGINT NOT NULL AUTO_INCREMENT,
    user_id BIGINT NOT NULL,
    stored_name VARCHAR(128) NOT NULL,
    physical_id BIGINT NOT NULL,
    original_name VARCHAR(128) NOT NULL,
    content_type VARCHAR(128) NOT NULL DEFAULT 'application/octet-stream',
    folder_id BIGINT NULL DEFAULT NULL,
    folder_key BIGINT GENERATED ALWAYS AS (COALESCE(folder_id, 0)) STORED,
    file_size BIGINT NOT NULL DEFAULT 0,
    is_public TINYINT(1) NOT NULL DEFAULT 0,
    content_sha256 CHAR(64) NOT NULL DEFAULT '',
    deleted_marker BIGINT NOT NULL DEFAULT 0,
    deleted_at TIMESTAMP NULL DEFAULT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uq_files_active_name (user_id, folder_key, original_name, deleted_marker),
    KEY idx_physical_id (physical_id),
    KEY idx_user_deleted_id (user_id, deleted_at, id),
    KEY idx_user_folder_deleted_name (user_id, folder_key, deleted_at, original_name),
    KEY idx_public_deleted_id (is_public, deleted_at, id),
    KEY idx_user_name_deleted (user_id, original_name, deleted_at),
    KEY idx_files_folder_id (folder_id),
    CONSTRAINT chk_files_file_size CHECK (file_size >= 0),
    CONSTRAINT chk_files_is_public CHECK (is_public IN (0, 1))
) ENGINE=InnoDB;

CREATE TABLE file_shares_v005 (
    token VARCHAR(64) NOT NULL,
    file_id BIGINT NOT NULL,
    user_id BIGINT NOT NULL,
    access_code_hash CHAR(64) NOT NULL DEFAULT '',
    expires_at TIMESTAMP NULL DEFAULT NULL,
    max_downloads BIGINT NOT NULL DEFAULT 0,
    download_count BIGINT NOT NULL DEFAULT 0,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (token),
    KEY idx_file_shares_file_id (file_id),
    KEY idx_file_shares_user_created (user_id, created_at),
    CONSTRAINT chk_file_shares_limits CHECK (max_downloads >= 0 AND download_count >= 0)
) ENGINE=InnoDB;

CREATE TABLE operation_logs_v005 (
    id BIGINT NOT NULL AUTO_INCREMENT,
    user_id BIGINT NULL DEFAULT NULL,
    username_snapshot VARCHAR(50) NOT NULL DEFAULT '',
    action VARCHAR(64) NOT NULL,
    resource_type VARCHAR(64) NOT NULL,
    resource_id BIGINT NOT NULL DEFAULT 0,
    detail JSON NOT NULL,
    result VARCHAR(32) NOT NULL DEFAULT 'success',
    request_id VARCHAR(64) NOT NULL DEFAULT '',
    ip VARCHAR(45) NOT NULL DEFAULT '',
    user_agent VARCHAR(255) NOT NULL DEFAULT '',
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    KEY idx_operation_user_created (user_id, created_at),
    KEY idx_operation_username_created (username_snapshot, created_at),
    KEY idx_operation_created (created_at)
) ENGINE=InnoDB;

INSERT INTO users_v005(username, password_hash, created_at)
SELECT username, passwd, CURRENT_TIMESTAMP
FROM `user`;

INSERT INTO users_v005(username, password_hash)
SELECT 'name',
       'pbkdf2_sha256$210000$5f0b5c0e6f4d9a71c2e84f10aa7d3912$69b9ececeda544f749543a4b941fd186bb779d96d9c54ff0dbb09294a7fe4108'
WHERE NOT EXISTS (
    SELECT 1 FROM users_v005 WHERE username = 'name'
);

INSERT INTO user_sessions_v005(token, user_id, expires_at, created_at)
SELECT s.token, u.id, s.expires_at, s.created_at
FROM user_sessions s
JOIN users_v005 u ON u.username = s.username;

INSERT INTO folders_v005(id, user_id, parent_id, name, deleted_marker, deleted_at, created_at)
SELECT f.id,
       u.id,
       NULLIF(f.parent_id, 0),
       f.name,
       f.deleted_marker,
       f.deleted_at,
       f.created_at
FROM folders f
JOIN users_v005 u ON u.username = f.owner_username;

INSERT INTO physical_files_v005(id, sha256, stored_name, file_size, ref_count, created_at)
SELECT id, sha256, stored_name, GREATEST(file_size, 0), GREATEST(ref_count, 0), created_at
FROM physical_files;

INSERT INTO files_v005(id, user_id, stored_name, physical_id, original_name, content_type,
                       folder_id, file_size, is_public, content_sha256, deleted_marker,
                       deleted_at, created_at)
SELECT f.id,
       u.id,
       f.stored_name,
       f.physical_id,
       f.original_name,
       COALESCE(NULLIF(f.content_type, ''), 'application/octet-stream'),
       NULLIF(f.folder_id, 0),
       GREATEST(f.file_size, 0),
       IF(f.is_public <> 0, 1, 0),
       COALESCE(f.content_sha256, ''),
       IF(f.deleted_at IS NULL, 0, f.id),
       f.deleted_at,
       f.created_at
FROM files f
JOIN users_v005 u ON u.username = f.owner_username
JOIN physical_files_v005 p ON p.id = f.physical_id;

INSERT INTO file_shares_v005(token, file_id, user_id, access_code_hash,
                             expires_at, max_downloads, download_count, created_at)
SELECT s.token,
       s.file_id,
       u.id,
       COALESCE(s.access_code_hash, ''),
       s.expires_at,
       GREATEST(s.max_downloads, 0),
       GREATEST(s.download_count, 0),
       s.created_at
FROM file_shares s
JOIN users_v005 u ON u.username = s.owner_username
JOIN files_v005 f ON f.id = s.file_id;

INSERT INTO operation_logs_v005(id, user_id, username_snapshot, action, resource_type,
                                resource_id, detail, created_at)
SELECT o.id,
       u.id,
       o.username,
       o.action,
       o.resource_type,
       o.resource_id,
       JSON_OBJECT('message', COALESCE(o.detail, '')),
       o.created_at
FROM operation_logs o
LEFT JOIN users_v005 u ON u.username = o.username;

UPDATE physical_files_v005 p
LEFT JOIN (
    SELECT physical_id, COUNT(*) AS refs
    FROM files_v005
    GROUP BY physical_id
) f ON f.physical_id = p.id
SET p.ref_count = COALESCE(f.refs, 0);

ALTER TABLE user_sessions_v005
    ADD CONSTRAINT fk_sessions_user_v005
        FOREIGN KEY (user_id) REFERENCES users_v005(id)
        ON DELETE CASCADE;

ALTER TABLE folders_v005
    ADD CONSTRAINT fk_folders_user_v005
        FOREIGN KEY (user_id) REFERENCES users_v005(id)
        ON DELETE CASCADE,
    ADD CONSTRAINT fk_folders_parent_v005
        FOREIGN KEY (parent_id) REFERENCES folders_v005(id)
        ON DELETE RESTRICT,
    ADD CONSTRAINT chk_folders_parent_id_v005
        CHECK (parent_id IS NULL OR parent_id > 0);

ALTER TABLE files_v005
    ADD CONSTRAINT fk_files_user_v005
        FOREIGN KEY (user_id) REFERENCES users_v005(id)
        ON DELETE CASCADE,
    ADD CONSTRAINT fk_files_folder_v005
        FOREIGN KEY (folder_id) REFERENCES folders_v005(id)
        ON DELETE RESTRICT,
    ADD CONSTRAINT fk_files_physical_v005
        FOREIGN KEY (physical_id) REFERENCES physical_files_v005(id)
        ON DELETE RESTRICT;

ALTER TABLE file_shares_v005
    ADD CONSTRAINT fk_file_shares_file_v005
        FOREIGN KEY (file_id) REFERENCES files_v005(id)
        ON DELETE CASCADE,
    ADD CONSTRAINT fk_file_shares_user_v005
        FOREIGN KEY (user_id) REFERENCES users_v005(id)
        ON DELETE CASCADE;

ALTER TABLE operation_logs_v005
    ADD CONSTRAINT fk_operation_logs_user_v005
        FOREIGN KEY (user_id) REFERENCES users_v005(id)
        ON DELETE SET NULL;

RENAME TABLE
    `user` TO user_legacy_005,
    user_sessions TO user_sessions_legacy_005,
    folders TO folders_legacy_005,
    physical_files TO physical_files_legacy_005,
    files TO files_legacy_005,
    file_shares TO file_shares_legacy_005,
    operation_logs TO operation_logs_legacy_005,
    users_v005 TO users,
    user_sessions_v005 TO user_sessions,
    folders_v005 TO folders,
    physical_files_v005 TO physical_files,
    files_v005 TO files,
    file_shares_v005 TO file_shares,
    operation_logs_v005 TO operation_logs;

CREATE TRIGGER trg_files_after_insert
AFTER INSERT ON files
FOR EACH ROW
UPDATE physical_files
SET ref_count = ref_count + 1
WHERE id = NEW.physical_id;

CREATE TRIGGER trg_files_after_delete
AFTER DELETE ON files
FOR EACH ROW
UPDATE physical_files
SET ref_count = GREATEST(ref_count - 1, 0)
WHERE id = OLD.physical_id;
