-- 1. 创建数据库
CREATE DATABASE IF NOT EXISTS `tlss_db`
    DEFAULT CHARACTER SET utf8mb4
    COLLATE utf8mb4_0900_ai_ci;

-- 2. 切换数据库
USE `tlss_db`;

-- 3. 创建用户表
CREATE TABLE IF NOT EXISTS `users` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `username` VARCHAR(64) NOT NULL,
    `salt` VARCHAR(64) NOT NULL,
    `password_hash` VARCHAR(255) NOT NULL,
    `is_deleted` TINYINT NOT NULL DEFAULT 0,
    `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
        ON UPDATE CURRENT_TIMESTAMP,

    PRIMARY KEY (`id`),
    UNIQUE KEY `uk_username` (`username`),
    CONSTRAINT `chk_is_deleted`
        CHECK (`is_deleted` IN (0, 1))
) ENGINE=InnoDB
  DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_0900_ai_ci;

-- 4.创建虚拟文件表
CREATE TABLE virtual_files (
    id BIGINT NOT NULL AUTO_INCREMENT,
    file_name VARCHAR(255) NOT NULL,
    username VARCHAR(64) NOT NULL,
    parent_id BIGINT NOT NULL DEFAULT -1,
    path VARCHAR(1024) NOT NULL,
    file_type ENUM('file', 'directory') NOT NULL,

    PRIMARY KEY (id),

    UNIQUE KEY uk_virtual_files_sibling
        (username, parent_id, file_name),

    KEY idx_virtual_files_user_parent
        (username, parent_id),

    CONSTRAINT fk_virtual_files_user
        FOREIGN KEY (username)
        REFERENCES users(username)
        ON UPDATE CASCADE
        ON DELETE RESTRICT,

    CONSTRAINT chk_virtual_files_root
        CHECK (
            (
                parent_id = -1
                AND file_name = '/'
                AND path = '/'
                AND file_type = 'directory'
            )
            OR (
                parent_id > 0
                AND path <> '/'
            )
        )
) ENGINE=InnoDB
  DEFAULT CHARSET=utf8mb4
  COLLATE=utf8mb4_0900_ai_ci;
