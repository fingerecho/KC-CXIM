-- ============================================================================
-- KC-CXIM 即时通讯后端数据库 schema
-- ----------------------------------------------------------------------------
-- 适用：MariaDB 10.x / MySQL 8.x（InnoDB + utf8mb4）
-- 目标：用尽可能少的表，支撑「用户 / 好友 / 会话 / 消息历史 / 离线消息 /
--       投递与已读回执 / 消息去重」这些功能。
--
-- 表一览（共 6 张）：
--   1) users                 用户账号
--   2) friends               好友关系（双向各一行）
--   3) conversations         会话（单聊 / 群聊），并保存 seq 生成器
--   4) conversation_members  会话成员 + 投递/已读游标（回执）
--   5) messages              消息历史（服务端 msg_id / seq / ts 都在这里生成）
--   6) offline_messages      离线消息队列（上线后补推，推送完删除）
--
-- 重要约定：
--   * 本文件以 DBConfig.ini 里的 dbname=CXIMDB 为例，下面直接
--     CREATE DATABASE / USE CXIMDB，保证单独执行本文件也能“建库 + 建表”。
--   * 如果以后把 DBConfig.ini 的 dbname 改成别的名字，二选一：
--       1) 把本文件中所有 CXIMDB 替换成新库名；
--       2) 注释掉下面 CREATE DATABASE / USE 两行，交给
--          prepare_env_V0.sh 以 DBConfig.ini 的库作为默认库来导入。
--   * 表结构刻意保持简单，方便初学者阅读和修改；生产规模上来后，
--     可以再按注释里的提示做分区、拆表、去外键等优化。
-- ============================================================================

-- 如果数据库不存在则创建；显式指定 utf8mb4，避免中文和 emoji 乱码。
CREATE DATABASE IF NOT EXISTS CXIMDB
    DEFAULT CHARACTER SET utf8mb4
    DEFAULT COLLATE utf8mb4_unicode_ci;

USE CXIMDB;

-- 让本次会话的字符集也使用 utf8mb4（防止导入过程中出现乱码）。
SET NAMES utf8mb4;


-- ============================================================================
-- 1) users：用户账号
-- ----------------------------------------------------------------------------
-- 设计要点：
--   * id 就是协议里的 userId，使用 BIGINT UNSIGNED，容量足够且不会很快用尽。
--   * password_hash 只存哈希（bcrypt / argon2），绝不能在库里存明文密码。
--   * status 用于软禁用，不物理删除用户，避免历史消息外键断裂。
--   * created_at / updated_at 由数据库自动填，业务层不用手写。
-- ============================================================================
CREATE TABLE IF NOT EXISTS users (
    id            BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT       COMMENT '用户ID，即协议中的 userId',
    username      VARCHAR(64)      NOT NULL                      COMMENT '登录名，全局唯一',
    password_hash VARCHAR(255)     NOT NULL                      COMMENT '口令哈希（bcrypt/argon2），禁止存明文',
    nickname      VARCHAR(64)      NOT NULL DEFAULT ''           COMMENT '昵称',
    avatar_url    VARCHAR(255)     NOT NULL DEFAULT ''           COMMENT '头像 URL（图片放 COS，不存二进制）',
    status        TINYINT UNSIGNED NOT NULL DEFAULT 1            COMMENT '账号状态：1=正常，0=禁用',
    created_at    DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3)                 COMMENT '注册时间（毫秒精度）',
    updated_at    DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                   ON UPDATE CURRENT_TIMESTAMP(3)                        COMMENT '最后更新时间',
    PRIMARY KEY (id),
    UNIQUE KEY uk_users_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='用户账号';


-- ============================================================================
-- 2) friends：好友关系
-- ----------------------------------------------------------------------------
-- 设计要点：
--   * 采用「双向各存一行」的简单做法：
--       建立好友时插入 (A,B) 和 (B,A) 两行；
--       查询“我的好友”时只需 WHERE user_id=?，非常简单直观。
--   * 缺点是要保证两行状态一致（应用层放在同一个事务里更新）。
--     若想只存一行，可改成 user_id < friend_id，但查询要用 OR，复杂度更高。
--   * status：0=申请中，1=已成为好友，2=已拉黑。
-- ============================================================================
CREATE TABLE IF NOT EXISTS friends (
    user_id    BIGINT UNSIGNED  NOT NULL                    COMMENT '关系拥有者 userId',
    friend_id  BIGINT UNSIGNED  NOT NULL                    COMMENT '对方 userId',
    status     TINYINT UNSIGNED NOT NULL DEFAULT 0          COMMENT '0=申请中，1=好友，2=拉黑',
    remark     VARCHAR(64)      NOT NULL DEFAULT ''         COMMENT '好友备注名',
    created_at DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (user_id, friend_id),
    KEY idx_friends_friend (friend_id),
    CONSTRAINT fk_friends_user   FOREIGN KEY (user_id)   REFERENCES users(id) ON DELETE CASCADE,
    CONSTRAINT fk_friends_friend FOREIGN KEY (friend_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='好友关系（双向各一行）';


-- ============================================================================
-- 3) conversations：会话（单聊 / 群聊）
-- ----------------------------------------------------------------------------
-- 设计要点：
--   * 单聊和群聊共用一张表，用 type 区分：1=单聊，2=群聊。
--   * dedup_key 用来避免“同一对用户被重复创建多个单聊会话”：
--       单聊：dedup_key = 'u:<较小的userId>:<较大的userId>'（唯一）
--       群聊：dedup_key = NULL（NULL 在唯一索引里可重复，互不影响）
--     建单聊时先 INSERT，若撞唯一键则说明已存在，直接查出来复用。
--   * last_seq 是“会话内消息序号生成器”：
--       每发一条消息执行
--         UPDATE conversations SET last_seq = LAST_INSERT_ID(last_seq + 1) WHERE id = ?;
--         SELECT LAST_INSERT_ID();   -- 得到本条消息的 seq
--       MySQL 的 LAST_INSERT_ID(expr) 会把 expr 记住并在下一次 SELECT 返回，
--       这样可以在并发下安全地为同一会话分配单调递增、不重复的 seq。
-- ============================================================================
CREATE TABLE IF NOT EXISTS conversations (
    id         BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT       COMMENT '会话ID',
    type       TINYINT UNSIGNED NOT NULL DEFAULT 1            COMMENT '1=单聊，2=群聊',
    title      VARCHAR(128)     NOT NULL DEFAULT ''           COMMENT '会话/群名称',
    owner_id   BIGINT UNSIGNED  NOT NULL DEFAULT 0            COMMENT '群主 userId；单聊为 0',
    dedup_key  VARCHAR(64)      NULL                          COMMENT '单聊去重键：u:<小id>:<大id>；群聊为 NULL',
    last_seq   BIGINT UNSIGNED  NOT NULL DEFAULT 0            COMMENT '该会话已分配的最大消息序号（seq 生成器）',
    created_at DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (id),
    UNIQUE KEY uk_conversations_dedup (dedup_key),
    KEY idx_conversations_owner (owner_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='会话（单聊/群聊）';


-- ============================================================================
-- 4) conversation_members：会话成员 + 投递/已读游标
-- ----------------------------------------------------------------------------
-- 设计要点：
--   * 一个用户在一个会话里一行，记录角色与两种“游标”：
--       last_delivered_seq：已成功投递到该用户客户端的最大 seq（投递回执）
--       last_read_seq     ：用户已读到（已打开）的最大 seq（已读回执）
--   * 用“游标”代替“每条消息每个用户一行回执表”，表更小、写更少，
--     对单聊/群聊都适用，是 IM 里最常见的简洁做法。
--   * 更新时用 GREATEST()，保证游标只增不减，即使客户端乱序回报也不会倒退：
--       UPDATE conversation_members
--          SET last_read_seq = GREATEST(last_read_seq, ?)
--        WHERE conversation_id = ? AND user_id = ?;
--   * 未读数 = 会话当前 last_seq - 我的 last_read_seq（再排除自己发的消息）。
-- ============================================================================
CREATE TABLE IF NOT EXISTS conversation_members (
    conversation_id    BIGINT UNSIGNED  NOT NULL,
    user_id            BIGINT UNSIGNED  NOT NULL,
    role               TINYINT UNSIGNED NOT NULL DEFAULT 0    COMMENT '0=成员，1=管理员，2=群主',
    last_delivered_seq BIGINT UNSIGNED  NOT NULL DEFAULT 0    COMMENT '已投递到的会话 seq（投递回执）',
    last_read_seq      BIGINT UNSIGNED  NOT NULL DEFAULT 0    COMMENT '已读到的会话 seq（已读回执）',
    muted              TINYINT UNSIGNED NOT NULL DEFAULT 0    COMMENT '是否免打扰：0=否，1=是',
    joined_at          DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3) COMMENT '加入时间',
    PRIMARY KEY (conversation_id, user_id),
    KEY idx_members_user (user_id),
    CONSTRAINT fk_members_conv FOREIGN KEY (conversation_id)
        REFERENCES conversations(id) ON DELETE CASCADE,
    CONSTRAINT fk_members_user FOREIGN KEY (user_id)
        REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='会话成员与投递/已读游标';


-- ============================================================================
-- 5) messages：消息历史
-- ----------------------------------------------------------------------------
-- 设计要点（对应需求里的“服务端分配 msg_id/ts、单调 seq、去重”）：
--   * id：全局自增，就是服务端分配给这条消息的 msg_id；客户端传来的 msg_id
--         不再被信任，业务层 INSERT 后把 id 回给发送方。
--   * seq：会话内单调递增序号，由 conversations.last_seq 原子分配；
--         用于排序、分页、已读/投递游标、断线补拉。
--   * created_at：服务端时间戳，替代客户端传来的 ts，防止伪造；
--         协议里的 ts（毫秒）可由应用层用 UNIX_TIMESTAMP(created_at)*1000 得到。
--   * client_msg_id：客户端生成的幂等 ID，用来做“发送去重”：
--         唯一键 (conversation_id, sender_id, client_msg_id) 冲突时，说明是重复发送，
--         直接返回已存在的那条消息即可，不会插入两条。
--         client_msg_id 允许为 NULL；MySQL 唯一索引允许多个 NULL。
--   * type 与协议 MsgType 对齐（1=文本，2=图片……）；心跳不入库。
--   * content 用 TEXT；图片/文件请存 URL，二进制放 COS，不要塞进数据库。
--
-- 常用查询：
--   历史分页（keyset，性能好，避免 OFFSET 越翻越慢）：
--     SELECT id, seq, sender_id, type, content, created_at
--       FROM messages
--      WHERE conversation_id = ? AND seq < ?
--      ORDER BY seq DESC
--      LIMIT 20;
-- ============================================================================
CREATE TABLE IF NOT EXISTS messages (
    id              BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT      COMMENT '全局消息ID（服务端生成的 msg_id）',
    conversation_id BIGINT UNSIGNED   NOT NULL                     COMMENT '所属会话',
    seq             BIGINT UNSIGNED   NOT NULL                     COMMENT '会话内单调递增序号，用于排序/去重/回执',
    sender_id       BIGINT UNSIGNED   NOT NULL                     COMMENT '发送者 userId',
    type            SMALLINT UNSIGNED NOT NULL DEFAULT 1           COMMENT '消息类型：1=文本，2=图片…（与协议对齐）',
    content         TEXT              NOT NULL                     COMMENT '文本正文或图片/文件 URL',
    client_msg_id   VARCHAR(64)       NULL                         COMMENT '客户端幂等ID，用于发送去重',
    created_at      DATETIME(3)       NOT NULL DEFAULT CURRENT_TIMESTAMP(3) COMMENT '服务端消息时间（毫秒精度）',
    PRIMARY KEY (id),
    UNIQUE KEY uk_messages_conv_seq (conversation_id, seq),
    UNIQUE KEY uk_messages_dedup (conversation_id, sender_id, client_msg_id),
    KEY idx_messages_sender (sender_id),
    KEY idx_messages_conv_time (conversation_id, created_at),
    KEY idx_messages_created (created_at),
    CONSTRAINT fk_messages_conv FOREIGN KEY (conversation_id)
        REFERENCES conversations(id) ON DELETE CASCADE,
    CONSTRAINT fk_messages_sender FOREIGN KEY (sender_id)
        REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='消息历史';


-- ============================================================================
-- 6) offline_messages：离线消息队列
-- ----------------------------------------------------------------------------
-- 设计要点（对应“离线用户不再丢弃消息，先落库、上线补推”）：
--   * 发送消息时，对每个“当前不在线”的接收者插入一行；
--   * 接收者登录后按 user_id 拉取，推送成功并收到 ACK 后删除；
--   * 主键 (user_id, message_id) 天然防止同一条消息重复入队；
--   * seq 是冗余字段，方便“按会话 + 序号”顺序补推。
--
-- 发送事务示例（伪代码，建议放在同一个事务里）：
--   UPDATE conversations SET last_seq = LAST_INSERT_ID(last_seq + 1) WHERE id = ?;
--   SET @seq = LAST_INSERT_ID();
--   INSERT INTO messages(conversation_id, seq, sender_id, type, content, client_msg_id)
--        VALUES (?, @seq, ?, ?, ?, ?);
--   SET @msg_id = LAST_INSERT_ID();
--   -- 对每个离线接收者：
--   INSERT IGNORE INTO offline_messages(user_id, message_id, conversation_id, seq)
--        VALUES (?, @msg_id, ?, @seq);
--   COMMIT;
--
-- 上线补推示例：
--   SELECT om.message_id, m.seq, m.conversation_id, m.sender_id, m.type, m.content, m.created_at
--     FROM offline_messages om
--     JOIN messages m ON m.id = om.message_id
--    WHERE om.user_id = ?
--    ORDER BY om.conversation_id, om.seq
--    LIMIT 200;
--   -- 推送成功、收到客户端 ACK 后删除（分批删，避免长事务）：
--   DELETE FROM offline_messages WHERE user_id = ? AND message_id IN (...);
--
-- 规模提示：如果群成员很多，逐人插 row 会膨胀；此时可改成“会话游标方案”：
--   不建 offline_messages，直接靠 conversation_members.last_delivered_seq
--   与 messages.seq 计算未投递区间。当前 MVP 先用显式队列，简单直观。
-- ============================================================================
CREATE TABLE IF NOT EXISTS offline_messages (
    user_id         BIGINT UNSIGNED NOT NULL                    COMMENT '离线接收者 userId',
    message_id      BIGINT UNSIGNED NOT NULL                    COMMENT 'messages.id',
    conversation_id BIGINT UNSIGNED NOT NULL                    COMMENT '冗余的会话ID，便于按会话补推',
    seq             BIGINT UNSIGNED NOT NULL                    COMMENT '冗余的会话内序号',
    created_at      DATETIME(3)     NOT NULL DEFAULT CURRENT_TIMESTAMP(3) COMMENT '入队时间',
    PRIMARY KEY (user_id, message_id),
    KEY idx_offline_user_conv_seq (user_id, conversation_id, seq),
    KEY idx_offline_msg (message_id),
    KEY idx_offline_created (created_at),
    CONSTRAINT fk_offline_user FOREIGN KEY (user_id)
        REFERENCES users(id) ON DELETE CASCADE,
    CONSTRAINT fk_offline_msg FOREIGN KEY (message_id)
        REFERENCES messages(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='离线消息队列（上线补推）';


-- ============================================================================
-- 附：数据保留 / 清理 / 备份与恢复（说明性内容，不自动执行）
-- ============================================================================
-- 1) 消息保留策略
--    * 先和产品确认“消息保留多久”（例如 180 天 / 永久）。
--    * 定期分批删除，避免一次性大 DELETE 长时间锁表：
--        DELETE FROM messages
--         WHERE created_at < NOW() - INTERVAL 180 DAY
--         LIMIT 5000;
--      在应用或定时任务里循环执行，直到影响行数为 0。
--    * messages 上已有 idx_messages_created(created_at)，可加速按时间的删除。
--    * 数据量很大时，可改用“按月分区 / 归档到冷存储”，但分区要求分区键
--      出现在每个唯一键里，需要同步调整主键与唯一索引，届时再评估。
--
-- 2) 离线消息清理
--    * 正常情况下补推成功就删除；对长期不上线的用户，可清理过期积压：
--        DELETE FROM offline_messages
--         WHERE created_at < NOW() - INTERVAL 30 DAY
--         LIMIT 5000;
--
-- 3) 备份（逻辑备份，最简单）
--    * 每日全量（--single-transaction 保证 InnoDB 一致性，不锁表）：
--        mysqldump --single-transaction --quick --routines --triggers --events \
--          -u root -p CXIMDB | gzip > /backup/CXIMDB_$(date +%F).sql.gz
--    * 恢复：
--        gunzip -c /backup/CXIMDB_2026-11-08.sql.gz | mysql -u root -p CXIMDB
--    * 建议把备份文件再上传到腾讯云 COS，避免和数据库同盘丢失。
--
-- 4) 增量 / 时间点恢复（PITR）
--    * prepare_env_V0.sh 已开启 binlog（ROW 格式，保留 7 天）：
--        log_bin = /var/log/mysql/mariadb-bin
--    * 恢复思路：先还原最近一次全量备份，再用 binlog 重放到故障前一刻。
--    * 建议定期做“备份可恢复演练”，只备份不验证等于没备份。
--
-- 5) 恢复后的数据一致性
--    * 恢复后检查 conversations.last_seq 与 messages 中该会话的 MAX(seq) 是否一致：
--        SELECT c.id, c.last_seq, COALESCE(MAX(m.seq), 0) AS max_seq
--          FROM conversations c
--          LEFT JOIN messages m ON m.conversation_id = c.id
--         GROUP BY c.id, c.last_seq
--        HAVING c.last_seq < max_seq;   -- 有结果说明游标落后，需要修复
-- ============================================================================
