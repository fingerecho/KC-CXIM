-- ============================================================================
-- KC-CXIM 即时通讯后端数据库 schema
-- ----------------------------------------------------------------------------
-- 适用：MariaDB 10.x / MySQL 8.x（InnoDB + utf8mb4）
-- 目标：用尽可能少的表，支撑「用户 / 好友 / 会话 / 消息历史 / 离线消息 /
--       投递与已读回执 / 消息去重」这些功能。
--
-- 表一览（共 10 张）：
--   1)   users                 用户账号
--   2)   friends               好友关系（双向各一行）
--   2.1) friend_requests       好友申请（单向，带待处理/通过/拒绝状态机）
--   3)   conversations         会话（单聊 / 群聊），并保存 seq 生成器
--   4)   conversation_members  会话成员 + 投递/已读游标（回执）
--   5)   messages              消息历史（服务端 msg_id / seq / ts 都在这里生成）
--   6)   offline_messages      离线消息队列（上线后补推，推送完删除）
--   7)   groups                群资料（群名/公告/群主/状态），关联一个 type=2 会话
--   8)   group_members         群成员与角色（群主/管理员/普通成员）
--   9)   group_requests        入群申请（带待处理/通过/拒绝状态机）
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
--   * status：0=保留（申请状态改由 friend_requests 表保存），1=已成为好友，2=已拉黑。
--     说明：引入 friend_requests 后，friends 表只表达“最终关系”：
--       - 申请过程的待处理/拒绝/取消都在 friend_requests 里，避免两行关系表
--         因为“谁先申请”而出现状态歧义；
--       - 拉黑只写一行 (A,B,status=2)，同时删除 B->A 的好友行，
--         这样 B 的“我的好友”查询里不会再看到 A。
-- ============================================================================
CREATE TABLE IF NOT EXISTS friends (
    user_id    BIGINT UNSIGNED  NOT NULL                    COMMENT '关系拥有者 userId',
    friend_id  BIGINT UNSIGNED  NOT NULL                    COMMENT '对方 userId',
    status     TINYINT UNSIGNED NOT NULL DEFAULT 0          COMMENT '0=保留，1=好友，2=拉黑',
    remark     VARCHAR(64)      NOT NULL DEFAULT ''         COMMENT '好友备注名',
    created_at DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    updated_at DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                ON UPDATE CURRENT_TIMESTAMP(3),
    PRIMARY KEY (user_id, friend_id),
    KEY idx_friends_friend (friend_id),
    -- /*************补索引增强MariaDB查询性能********************/
    -- 覆盖「我的好友列表」查询：WHERE user_id=? AND status=1 ORDER BY friend_id。
    -- remark 也放进索引，避免回表取备注，EXPLAIN 可出现 Using index。
    KEY idx_friends_user_status (user_id, status, friend_id, remark),
    -- /*************补索引增强MariaDB查询性能********************/
    CONSTRAINT fk_friends_user   FOREIGN KEY (user_id)   REFERENCES users(id) ON DELETE CASCADE,
    CONSTRAINT fk_friends_friend FOREIGN KEY (friend_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='好友关系（双向各一行）';


-- ============================================================================
-- 2.1) friend_requests：好友申请（单向、带状态机）
-- ----------------------------------------------------------------------------
-- 为什么需要独立一张表？
--   friends 表用“双向各一行”表达最终关系，但一个申请天然是单向的：
--   A 申请 B，只应有一条 A->B 的申请记录，且需要记录“谁申请谁、附言、
--   待处理/通过/拒绝/取消”的状态变化。如果把申请状态塞进 friends.status，
--   两行关系会在“A 申请 B、B 又申请 A”时产生歧义，因此拆表更清晰。
--
-- 状态机：
--   0 pending  ：待被申请人处理
--   1 accepted ：已通过（friends 表已写入双向好友行）
--   2 rejected ：已拒绝
--   3 canceled ：申请人或系统取消（保留字段，当前接口未开放取消）
--
-- 唯一键 (requester_id,target_id)：
--   * 同一个方向只保留一条申请记录，避免刷屏；
--   * 被拒绝后允许重新申请：应用层把旧记录重置为 pending，而不是插入新行。
--   * 反向申请 (B,A) 是另一条记录，应用层会提示“对方已向你发送申请”。
--
-- result_notified：
--   申请结果需要实时推给申请人。如果处理申请时申请人不在线，
--   该列保持 0；申请人下次 TCP 登录时服务端补推结果并置 1，
--   用最小成本实现“离线事件补偿”，避免再建一张事件队列表。
-- ============================================================================
CREATE TABLE IF NOT EXISTS friend_requests (
    id              BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT       COMMENT '申请ID',
    requester_id    BIGINT UNSIGNED  NOT NULL                     COMMENT '申请人 userId',
    target_id       BIGINT UNSIGNED  NOT NULL                     COMMENT '被申请人 userId',
    message         VARCHAR(255)     NOT NULL DEFAULT ''          COMMENT '申请附言',
    status          TINYINT UNSIGNED NOT NULL DEFAULT 0           COMMENT '0=待处理，1=已通过，2=已拒绝，3=已取消',
    result_notified TINYINT UNSIGNED NOT NULL DEFAULT 0           COMMENT '结果是否已推送给申请人：0=否，1=是',
    created_at      DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3) COMMENT '申请时间（毫秒精度）',
    updated_at      DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                    ON UPDATE CURRENT_TIMESTAMP(3)          COMMENT '状态最后变更时间',
    PRIMARY KEY (id),
    UNIQUE KEY uk_friend_requests_pair (requester_id, target_id),
    KEY idx_friend_requests_target (target_id, status),
    KEY idx_friend_requests_requester (requester_id, status, result_notified),
    CONSTRAINT fk_friend_requests_requester FOREIGN KEY (requester_id)
        REFERENCES users(id) ON DELETE CASCADE,
    CONSTRAINT fk_friend_requests_target FOREIGN KEY (target_id)
        REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='好友申请（单向，带状态机）';


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
-- 7) groups：群资料（BE-GROUP-01）
-- ----------------------------------------------------------------------------
-- 设计要点：
--   * 一个群对应一个 conversations 里的 type=2 会话；conversation_id 就是
--     群消息读写、seq 分配、成员游标所在的会话。建群时在同一个事务里
--     先 INSERT conversations(type=2,dedup_key=NULL) 再回填到这里。
--   * 为什么群要单独一张表、而不是只用 conversations？
--       conversations 是“会话”这种通用结构，只放 seq 生成器和 owner；
--       群特有的公告/简介/头像/状态/人数上限属于“群业务”，放进
--       conversations 会让单聊行也拖着一堆空字段，拆表更清晰。
--   * owner_id 冗余存一份群主：转让/解散/权限校验高频用到群主，避免每次
--     都 JOIN group_members 按 role=1 反查。真正的成员角色仍以 group_members 为准。
--   * status 用软删除：0=正常，1=已解散。解散不物理删行，保留历史消息与审计，
--     解散后禁止再收发该群消息（应用层校验）。
--   * member_limit：成员上限，邀请/申请入群时校验，防刷群。
-- ============================================================================
CREATE TABLE IF NOT EXISTS groups (
    id              BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT       COMMENT '群ID（group_id，对外协议中的 to 即填此值）',
    name            VARCHAR(128)     NOT NULL DEFAULT ''           COMMENT '群名称',
    announcement    VARCHAR(512)     NOT NULL DEFAULT ''           COMMENT '群公告',
    intro           VARCHAR(255)     NOT NULL DEFAULT ''           COMMENT '群简介',
    avatar_url      VARCHAR(255)     NOT NULL DEFAULT ''           COMMENT '群头像 URL',
    owner_id        BIGINT UNSIGNED  NOT NULL DEFAULT 0            COMMENT '群主 userId（转让时更新）',
    conversation_id BIGINT UNSIGNED  NOT NULL DEFAULT 0            COMMENT '对应的会话ID（conversations.id，type=2）',
    status          TINYINT UNSIGNED NOT NULL DEFAULT 0            COMMENT '0=正常，1=已解散',
    member_limit    INT UNSIGNED     NOT NULL DEFAULT 200          COMMENT '成员数量上限，邀请/申请时校验',
    created_at      DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3) COMMENT '建群时间（毫秒精度）',
    updated_at      DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                     ON UPDATE CURRENT_TIMESTAMP(3)         COMMENT '资料最后变更时间',
    PRIMARY KEY (id),
    KEY idx_groups_conversation (conversation_id),
    KEY idx_groups_owner (owner_id),
    CONSTRAINT fk_groups_owner FOREIGN KEY (owner_id) REFERENCES users(id),
    CONSTRAINT fk_groups_conversation FOREIGN KEY (conversation_id) REFERENCES conversations(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='群资料（BE-GROUP-01）';


-- ============================================================================
-- 8) group_members：群成员与角色（BE-GROUP-04）
-- ----------------------------------------------------------------------------
-- 设计要点：
--   * (group_id,user_id) 主键天然保证“一人一群只有一行”，重复邀请幂等。
--   * role 表达群内权限：1=群主，2=管理员，3=普通成员。权限矩阵在应用层
--     （db.cpp 群 DAO）集中校验：
--       群主   ：全部操作（解散/转让/任命管理员/邀请/移除/改资料/发言）
--       管理员 ：邀请、移除普通成员、改资料、发言
--       普通成员：发言、退群
--   * 群主的“唯一性”由应用层保证：转让时在同一事务里把旧群主降为管理员/成员、
--     新群主升为 role=1，避免出现两个群主。
--   * group_nickname 是每个成员在本群的群内昵称（BE-GROUP-05），只影响本行。
--   * 退群/被移出/解散：应用层删除对应 group_members 行（解散可整群删除），
--     即时生效——发群消息、拉群历史都会先 JOIN 本表校验成员身份。
--   * 同时会在 conversation_members 维护该成员在群会话里的投递/已读游标；
--     group_members 管“是不是成员/什么角色”，conversation_members 管“读到哪”。
-- ============================================================================
CREATE TABLE IF NOT EXISTS group_members (
    group_id       BIGINT UNSIGNED  NOT NULL                    COMMENT '群ID',
    user_id        BIGINT UNSIGNED  NOT NULL                    COMMENT '成员 userId',
    role           TINYINT UNSIGNED NOT NULL DEFAULT 3          COMMENT '1=群主，2=管理员，3=普通成员',
    group_nickname VARCHAR(64)      NOT NULL DEFAULT ''         COMMENT '本群群内昵称',
    joined_at      DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3) COMMENT '入群时间',
    PRIMARY KEY (group_id, user_id),
    KEY idx_group_members_user (user_id),
    KEY idx_group_members_group_role (group_id, role),
    CONSTRAINT fk_gm_group FOREIGN KEY (group_id) REFERENCES groups(id) ON DELETE CASCADE,
    CONSTRAINT fk_gm_user  FOREIGN KEY (user_id)  REFERENCES users(id)  ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='群成员与角色（BE-GROUP-04）';


-- ============================================================================
-- 9) group_requests：入群申请（BE-GROUP-03）
-- ----------------------------------------------------------------------------
-- 设计要点：
--   * 参照 friend_requests 的状态机与 result_notified 离线补偿思路：
--       0=待处理，1=已通过，2=已拒绝，3=已取消；
--       通过时在同一事务里 INSERT group_members + conversation_members；
--       result_notified 用于“申请人离线时，登录后补推处理结果”。
--   * handler_id：审批人（群主/管理员）userId，便于审计与事件展示。
--   * 同一个 (group_id,requester_id) 允许多条历史记录，不做唯一键：
--       被拒后允许再次申请（与好友申请“复用同一行”不同，这里直接插新行，
--       群申请量小、保留完整申请历史更直观）。待处理重复由应用层查
--       status=0 拦截。
-- ============================================================================
CREATE TABLE IF NOT EXISTS group_requests (
    id              BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT     COMMENT '入群申请ID',
    group_id        BIGINT UNSIGNED  NOT NULL                    COMMENT '目标群ID',
    requester_id    BIGINT UNSIGNED  NOT NULL                    COMMENT '申请人 userId',
    handler_id      BIGINT UNSIGNED  NOT NULL DEFAULT 0          COMMENT '审批人 userId（0=未处理）',
    message         VARCHAR(255)     NOT NULL DEFAULT ''         COMMENT '申请附言',
    status          TINYINT UNSIGNED NOT NULL DEFAULT 0          COMMENT '0=待处理，1=已通过，2=已拒绝，3=已取消',
    result_notified TINYINT UNSIGNED NOT NULL DEFAULT 0          COMMENT '结果是否已推送给申请人：0=否，1=是',
    created_at      DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3) COMMENT '申请时间',
    updated_at      DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3)
                                     ON UPDATE CURRENT_TIMESTAMP(3)          COMMENT '状态最后变更时间',
    PRIMARY KEY (id),
    KEY idx_group_requests_group_status (group_id, status),
    KEY idx_group_requests_requester (requester_id, status, result_notified),
    CONSTRAINT fk_gr_group     FOREIGN KEY (group_id)     REFERENCES groups(id)        ON DELETE CASCADE,
    CONSTRAINT fk_gr_requester FOREIGN KEY (requester_id) REFERENCES users(id)          ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='入群申请（BE-GROUP-03）';


-- ============================================================================
-- 10) files：图片/文件对象登记表（图片文件功能·七牛云直传）
-- ----------------------------------------------------------------------------
-- 设计原理（与规划一致，逐条对应实现）：
--   * 这张表只登记"对象元数据 + 权属"，**二进制永远在七牛**——messages 表
--     注释早就写明"图片/文件存 URL，不塞数据库"，这里把另一半补齐：
--     URL 指向的对象归七牛管，我们只管"谁传的、多大、叫什么、还在不在"。
--   * uuid 唯一键是对外标识（自增 id 只做主键，防止"枚举 id 摸文件"）；
--     object_key 由服务端生成（im/<yyyy-mm>/<uuid>.<ext>）并锁进上传凭证
--     policy——客户端拿着凭证也只能写这一个 key。
--   * 签发上传凭证时**不写这张表**（无状态票据替代，见 object_store.cpp）：
--     行只出现在 commit 之后，"没 commit 的文件不存在"因此是数据库层面
--     的事实，而不是靠应用代码记着的约定。
--   * size/qetag 登记的是七牛 stat 的权威返回：服务器不中继字节，
--     客户端自报的大小只能用于配额与 policy 限额，落库一律以 stat 为准。
--   * qetag 是七牛自定义校验和（与标准 sha256 **算法不同**，不可互相验证）。
--     强度边界如实记录：服务器不碰字节，所以无法核验客户端自报的 sha256；
--     完整性核对链路是"上传→stat 登记 qetag→下载后本地重算比对登记值"。
--   * status 软删除：1=正常，2=已标记删除。DELETE 接口先标 2 再调七牛物理删
--     （删除失败只留孤儿对象+日志，不会让业务"看得见已删文件"）。
--   * 保留/清理（已知边界的落地口径，规划要求写进注释不假装）：
--       - 直传成功但从未 commit 的对象 = 本表不可见的孤儿：签发不落库，
--         所以这类孤儿**只能靠七牛控制台/生命周期规则兜底**，应用侧无表可扫；
--         真实模式强烈建议给 bucket 配置"未命名片段 7 天自动清理"+定期对账。
--       - status=1 超过 FILE_RETENTION_DAYS=30 天仅打告警日志，是否强删
--         等产品拍板（七牛存储按量计费，"先存着"不是无代价的）。
-- ============================================================================
CREATE TABLE IF NOT EXISTS files (
    id          BIGINT UNSIGNED  NOT NULL AUTO_INCREMENT      COMMENT '内部主键（不对外）',
    uuid        CHAR(36)         NOT NULL                     COMMENT '对外文件标识：服务端生成的随机 v4，出现在 object_key 与换取 URL',
    uploader_id BIGINT UNSIGNED  NOT NULL                     COMMENT '上传者 userId（发送校验的权属依据：只能发自己上传的文件）',
    name        VARCHAR(255)     NOT NULL DEFAULT ''          COMMENT '原始文件名（仅展示；消毒后存储：去控制字符、UTF-8 边界安全截断）',
    size        BIGINT UNSIGNED  NOT NULL DEFAULT 0           COMMENT '字节数：以七牛 stat 返回为准登记，防客户端谎报',
    mime        VARCHAR(127)     NOT NULL DEFAULT ''          COMMENT 'MIME（按扩展名白名单归一化后的值）',
    qetag       CHAR(32)         NOT NULL DEFAULT ''          COMMENT '七牛对象校验和（stat 返回；与标准 sha256 算法不同，不可互换核验——强度边界见本节注释）',
    object_key  VARCHAR(255)     NOT NULL                     COMMENT '七牛对象名 im/<yyyy-mm>/<uuid>.<ext>；服务端生成并锁进上传凭证 policy',
    bucket      VARCHAR(63)      NOT NULL DEFAULT ''          COMMENT '所在 bucket（换空间/迁移后旧行可追溯）',
    zone        TINYINT UNSIGNED NOT NULL DEFAULT 0           COMMENT '七牛区域：0=z0华东,1=z1华北,2=z2华南,3=na0北美,4=as0东南亚',
    status      TINYINT UNSIGNED NOT NULL DEFAULT 1           COMMENT '1=正常，2=已删除（软删：先标库再删七牛对象）',
    created_at  DATETIME(3)      NOT NULL DEFAULT CURRENT_TIMESTAMP(3) COMMENT 'commit 登记时间（毫秒精度）',
    PRIMARY KEY (id),
    UNIQUE KEY uk_files_uuid (uuid),
    KEY idx_files_uploader_created (uploader_id, created_at),   -- 日配额 SUM(size) 按 (uploader,当天) 扫
    KEY idx_files_status_created (status, created_at),          -- 超期告警/清理扫描
    KEY idx_files_object_key (object_key),                      -- 对账（按 key 反查登记）
    CONSTRAINT fk_files_uploader FOREIGN KEY (uploader_id) REFERENCES users(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci
  COMMENT='图片/文件对象登记（七牛云直传，二进制不在本库）';


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
