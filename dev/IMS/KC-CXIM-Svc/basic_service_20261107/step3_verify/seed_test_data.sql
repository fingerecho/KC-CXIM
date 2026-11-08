-- ============================================================================
-- 自动化验收用测试数据
-- ----------------------------------------------------------------------------
-- 警告：本脚本会清空消息/会话/离线/好友等业务表，并重建 1001/1002/1003 三个
--       测试账号。只用于测试数据库，切勿在生产库执行！
--
-- 登录凭据约定（简化版）：
--   本次实现直接把 users.password_hash 与登录 content 做字符串比较，
--   所以测试账号的 password_hash 就是 tokenA/tokenB/tokenC。
--   正式环境应改为 bcrypt/argon2 哈希校验。
-- ============================================================================

SET FOREIGN_KEY_CHECKS = 0;

TRUNCATE TABLE offline_messages;
TRUNCATE TABLE messages;
TRUNCATE TABLE conversation_members;
TRUNCATE TABLE conversations;
TRUNCATE TABLE friends;

DELETE FROM users
 WHERE id IN (1001, 1002, 1003)
    OR username IN ('user1001', 'user1002', 'user1003');

INSERT INTO users (id, username, password_hash, nickname) VALUES
    (1001, 'user1001', 'tokenA', 'A'),
    (1002, 'user1002', 'tokenB', 'B'),
    (1003, 'user1003', 'tokenC', 'C');

SET FOREIGN_KEY_CHECKS = 1;
