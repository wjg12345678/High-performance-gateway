CREATE TABLE IF NOT EXISTS folders (
    id BIGINT NOT NULL AUTO_INCREMENT,
    owner_username VARCHAR(50) NOT NULL,
    parent_id BIGINT NOT NULL DEFAULT 0,
    name VARCHAR(128) NOT NULL,
    deleted_marker BIGINT NOT NULL DEFAULT 0,
    deleted_at TIMESTAMP NULL DEFAULT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uq_owner_parent_name_active (owner_username, parent_id, name, deleted_marker),
    KEY idx_owner_parent_deleted (owner_username, parent_id, deleted_at, id),
    KEY idx_owner_parent_name_deleted (owner_username, parent_id, name, deleted_at)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS physical_files (
    id BIGINT NOT NULL AUTO_INCREMENT,
    sha256 CHAR(64) NOT NULL,
    stored_name VARCHAR(128) NOT NULL,
    file_size BIGINT NOT NULL DEFAULT 0,
    ref_count BIGINT NOT NULL DEFAULT 0,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    UNIQUE KEY uq_physical_sha256 (sha256),
    UNIQUE KEY uq_physical_stored_name (stored_name)
) ENGINE=InnoDB;

DELIMITER //
DROP PROCEDURE IF EXISTS add_column_if_missing//
CREATE PROCEDURE add_column_if_missing(IN table_name_in VARCHAR(64), IN column_name_in VARCHAR(64), IN ddl_in TEXT)
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.COLUMNS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME = table_name_in
          AND COLUMN_NAME = column_name_in
    ) THEN
        SET @migration_ddl = ddl_in;
        PREPARE migration_stmt FROM @migration_ddl;
        EXECUTE migration_stmt;
        DEALLOCATE PREPARE migration_stmt;
    END IF;
END//

DROP PROCEDURE IF EXISTS add_index_if_missing//
CREATE PROCEDURE add_index_if_missing(IN table_name_in VARCHAR(64), IN index_name_in VARCHAR(128), IN ddl_in TEXT)
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM information_schema.STATISTICS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME = table_name_in
          AND INDEX_NAME = index_name_in
    ) THEN
        SET @migration_ddl = ddl_in;
        PREPARE migration_stmt FROM @migration_ddl;
        EXECUTE migration_stmt;
        DEALLOCATE PREPARE migration_stmt;
    END IF;
END//

DROP PROCEDURE IF EXISTS drop_index_if_exists//
CREATE PROCEDURE drop_index_if_exists(IN table_name_in VARCHAR(64), IN index_name_in VARCHAR(128), IN ddl_in TEXT)
BEGIN
    IF EXISTS (
        SELECT 1 FROM information_schema.STATISTICS
        WHERE TABLE_SCHEMA = DATABASE()
          AND TABLE_NAME = table_name_in
          AND INDEX_NAME = index_name_in
    ) THEN
        SET @migration_ddl = ddl_in;
        PREPARE migration_stmt FROM @migration_ddl;
        EXECUTE migration_stmt;
        DEALLOCATE PREPARE migration_stmt;
    END IF;
END//
DELIMITER ;

CALL add_column_if_missing('folders', 'deleted_marker', 'ALTER TABLE folders ADD COLUMN deleted_marker BIGINT NOT NULL DEFAULT 0');
CALL add_index_if_missing('folders', 'uq_owner_parent_name_active', 'ALTER TABLE folders ADD UNIQUE KEY uq_owner_parent_name_active (owner_username, parent_id, name, deleted_marker)');
CALL add_index_if_missing('folders', 'idx_owner_parent_deleted', 'ALTER TABLE folders ADD KEY idx_owner_parent_deleted (owner_username, parent_id, deleted_at, id)');
CALL add_index_if_missing('folders', 'idx_owner_parent_name_deleted', 'ALTER TABLE folders ADD KEY idx_owner_parent_name_deleted (owner_username, parent_id, name, deleted_at)');

CALL add_column_if_missing('files', 'folder_id', 'ALTER TABLE files ADD COLUMN folder_id BIGINT NOT NULL DEFAULT 0');
CALL add_column_if_missing('files', 'physical_id', 'ALTER TABLE files ADD COLUMN physical_id BIGINT NOT NULL DEFAULT 0');
CALL add_column_if_missing('files', 'is_public', 'ALTER TABLE files ADD COLUMN is_public TINYINT(1) NOT NULL DEFAULT 0');
CALL add_column_if_missing('files', 'content_sha256', 'ALTER TABLE files ADD COLUMN content_sha256 CHAR(64) NOT NULL DEFAULT ''''');
CALL add_column_if_missing('files', 'deleted_at', 'ALTER TABLE files ADD COLUMN deleted_at TIMESTAMP NULL DEFAULT NULL');

CALL add_index_if_missing('files', 'idx_owner_deleted_id', 'ALTER TABLE files ADD KEY idx_owner_deleted_id (owner_username, deleted_at, id)');
CALL add_index_if_missing('files', 'idx_owner_folder_deleted_name', 'ALTER TABLE files ADD KEY idx_owner_folder_deleted_name (owner_username, folder_id, deleted_at, original_name)');
CALL add_index_if_missing('files', 'idx_physical_id', 'ALTER TABLE files ADD KEY idx_physical_id (physical_id)');
CALL add_index_if_missing('files', 'idx_public_deleted_id', 'ALTER TABLE files ADD KEY idx_public_deleted_id (is_public, deleted_at, id)');
CALL add_index_if_missing('files', 'idx_owner_name_deleted', 'ALTER TABLE files ADD KEY idx_owner_name_deleted (owner_username, original_name, deleted_at)');
CALL drop_index_if_exists('files', 'idx_owner_username', 'ALTER TABLE files DROP INDEX idx_owner_username');

INSERT IGNORE INTO physical_files(sha256, stored_name, file_size, ref_count)
SELECT content_sha256, MIN(stored_name), MAX(file_size), SUM(CASE WHEN deleted_at IS NULL THEN 1 ELSE 0 END)
FROM files
WHERE content_sha256 <> ''
GROUP BY content_sha256;

UPDATE files f
JOIN physical_files p ON f.content_sha256 = p.sha256
SET f.physical_id = p.id,
    f.stored_name = p.stored_name
WHERE f.physical_id = 0
  AND f.content_sha256 <> '';

DROP PROCEDURE IF EXISTS add_column_if_missing;
DROP PROCEDURE IF EXISTS add_index_if_missing;
DROP PROCEDURE IF EXISTS drop_index_if_exists;
