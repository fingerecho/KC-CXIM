-- ============================================================================
-- migrate_add_friends_index.sql
-- 给「已存在」的 CXIMDB.friends 表补索引。
-- 新建库无需执行本文件：db/schema.sql 里已经包含该索引。
-- ----------------------------------------------------------------------------
-- 适用查询（step1_tcp_echo/db.cpp：Db::listFriends）：
--   SELECT u.id,u.username,u.nickname,u.avatar_url,f.remark,f.status
--     FROM friends f JOIN users u ON u.id=f.friend_id
--    WHERE f.user_id=? AND f.status=1
--    ORDER BY f.friend_id ASC;
--
-- 索引 (user_id,status,friend_id,remark)：
--   user_id 等值 + status 等值 -> friend_id 有序，直接满足过滤和排序；
--   把 remark 放进索引后，friends 侧无需回表，EXPLAIN 可出现 Using index。
--
-- 注意：若索引已存在，会报 1061 Duplicate key name，可忽略。
-- 执行前先确认：SHOW INDEX FROM friends;
-- ============================================================================

-- /*************补索引增强MariaDB查询性能********************/
ALTER TABLE friends
  ADD INDEX idx_friends_user_status (user_id, status, friend_id, remark);
-- /*************补索引增强MariaDB查询性能********************/

-- 刷新统计信息，让优化器立即感知新索引。
ANALYZE TABLE friends;
