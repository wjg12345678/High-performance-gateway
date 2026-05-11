USE qgydb;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version VARCHAR(128) NOT NULL,
    applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (version)
) ENGINE=InnoDB;

CREATE TABLE users (
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

CREATE TABLE user_sessions (
    token VARCHAR(128) NOT NULL,
    user_id BIGINT NOT NULL,
    expires_at TIMESTAMP NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (token),
    KEY idx_sessions_user_id (user_id),
    KEY idx_sessions_expires_at (expires_at),
    CONSTRAINT fk_sessions_user
        FOREIGN KEY (user_id) REFERENCES users(id)
        ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE folders (
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
    KEY idx_folders_parent_id (parent_id),
    CONSTRAINT fk_folders_user
        FOREIGN KEY (user_id) REFERENCES users(id)
        ON DELETE CASCADE,
    CONSTRAINT fk_folders_parent
        FOREIGN KEY (parent_id) REFERENCES folders(id)
        ON DELETE RESTRICT,
    CONSTRAINT chk_folders_parent_id
        CHECK (parent_id IS NULL OR parent_id > 0)
) ENGINE=InnoDB;

CREATE TABLE physical_files (
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

CREATE TABLE files (
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
    CONSTRAINT fk_files_user
        FOREIGN KEY (user_id) REFERENCES users(id)
        ON DELETE CASCADE,
    CONSTRAINT fk_files_folder
        FOREIGN KEY (folder_id) REFERENCES folders(id)
        ON DELETE RESTRICT,
    CONSTRAINT fk_files_physical
        FOREIGN KEY (physical_id) REFERENCES physical_files(id)
        ON DELETE RESTRICT,
    CONSTRAINT chk_files_file_size CHECK (file_size >= 0),
    CONSTRAINT chk_files_is_public CHECK (is_public IN (0, 1))
) ENGINE=InnoDB;

CREATE TABLE file_shares (
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
    CONSTRAINT fk_file_shares_file
        FOREIGN KEY (file_id) REFERENCES files(id)
        ON DELETE CASCADE,
    CONSTRAINT fk_file_shares_user
        FOREIGN KEY (user_id) REFERENCES users(id)
        ON DELETE CASCADE,
    CONSTRAINT chk_file_shares_limits CHECK (max_downloads >= 0 AND download_count >= 0)
) ENGINE=InnoDB;

CREATE TABLE operation_logs (
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
    KEY idx_operation_created (created_at),
    CONSTRAINT fk_operation_logs_user
        FOREIGN KEY (user_id) REFERENCES users(id)
        ON DELETE SET NULL
) ENGINE=InnoDB;

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

INSERT INTO users(username, password_hash)
VALUES (
    'name',
    'pbkdf2_sha256$210000$5f0b5c0e6f4d9a71c2e84f10aa7d3912$69b9ececeda544f749543a4b941fd186bb779d96d9c54ff0dbb09294a7fe4108'
);

INSERT IGNORE INTO schema_migrations(version)
VALUES
    ('001_init_schema'),
    ('002_upgrade_existing_drive_dedup'),
    ('003_recycle_bin_ref_counts'),
    ('004_drop_passwd_salt'),
    ('005_normalize_schema'),
    ('006_ref_count_triggers');
