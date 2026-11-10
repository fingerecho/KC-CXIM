CREATE DATABASE IF NOT EXISTS CXIMDB
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_unicode_ci;

USE CXIMDB;

SET NAMES utf8mb4;

CREATE TABLE IF NOT EXISTS admin_users (
    id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    username      VARCHAR(64)     NOT NULL,
    password_hash VARCHAR(255)    NOT NULL,
    nickname      VARCHAR(64)     NOT NULL DEFAULT '',
    role          VARCHAR(32)     NOT NULL DEFAULT 'super_admin',
    status        TINYINT UNSIGNED NOT NULL DEFAULT 1,
    last_login_at DATETIME(3)     NULL,
    created_at    DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at    DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                  ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (id),
    UNIQUE KEY uk_admin_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS admin_sessions (
    id                 BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    admin_id           BIGINT UNSIGNED NOT NULL,
    refresh_token_hash CHAR(64)        NOT NULL,
    ip                 VARCHAR(64)     NOT NULL DEFAULT '',
    user_agent         VARCHAR(255)    NOT NULL DEFAULT '',
    expires_at         DATETIME(3)     NOT NULL,
    revoked            TINYINT UNSIGNED NOT NULL DEFAULT 0,
    created_at         DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (id),
    UNIQUE KEY uk_admin_session_token (refresh_token_hash),
    KEY idx_admin_session_admin (admin_id),
    CONSTRAINT fk_admin_session_admin FOREIGN KEY (admin_id)
        REFERENCES admin_users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS audit_logs (
    id          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    admin_id    BIGINT UNSIGNED NOT NULL,
    admin_name  VARCHAR(64)     NOT NULL DEFAULT '',
    action      VARCHAR(64)     NOT NULL,
    module      VARCHAR(64)     NOT NULL,
    target_type VARCHAR(64)     NOT NULL DEFAULT '',
    target_id   VARCHAR(64)     NOT NULL DEFAULT '',
    before_json TEXT            NULL,
    after_json  TEXT            NULL,
    ip          VARCHAR(64)     NOT NULL DEFAULT '',
    user_agent  VARCHAR(255)    NOT NULL DEFAULT '',
    request_id  VARCHAR(64)     NOT NULL DEFAULT '',
    result      VARCHAR(32)     NOT NULL DEFAULT 'success',
    created_at  DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (id),
    KEY idx_audit_admin (admin_id),
    KEY idx_audit_created (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT IGNORE INTO admin_users
    (username, password_hash, nickname, role, status)
VALUES
    (
        'admin',
        'pbkdf2-sha256$120000$Y3gtZGVtby1zYWx0LTIwMjY=$NjFRaVIyQpvdbPzbCC0NtOkPqAThy/i6TlE2ICKppUw=',
        '超级管理员',
        'super_admin',
        1
    );
