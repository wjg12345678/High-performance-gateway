SET @drop_passwd_salt = (
    SELECT IF(
        COUNT(*) > 0,
        'ALTER TABLE `user` DROP COLUMN `passwd_salt`',
        'SELECT 1'
    )
    FROM INFORMATION_SCHEMA.COLUMNS
    WHERE TABLE_SCHEMA = DATABASE()
      AND TABLE_NAME = 'user'
      AND COLUMN_NAME = 'passwd_salt'
);

PREPARE drop_passwd_salt_stmt FROM @drop_passwd_salt;
EXECUTE drop_passwd_salt_stmt;
DEALLOCATE PREPARE drop_passwd_salt_stmt;
