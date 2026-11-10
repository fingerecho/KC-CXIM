# kc-cxim-AdminBackend

《铖信》即时通讯管理后台的 C++ 后端骨架，基于 Drogon 1.9.x + nlohmann/json + MariaDB。

## 技术栈

| 项 | 选型 |
| --- | --- |
| 语言 | C++17 |
| HTTP 框架 | Drogon 1.9.x |
| JSON | nlohmann/json |
| 数据库 | MariaDB / MySQL，通过 Drogon `dbClient` |
| 加密 | OpenSSL（HS256、PBKDF2、随机数） |
| 构建 | CMake + vcpkg |
| 异步模型 | 统一回调式异步（`HttpCallback` + `execSqlAsync`） |

## 目录结构

```text
dev/Admin/AdminBackend/
├── CMakeLists.txt
├── vcpkg.json
├── config.json.example
├── db/schema_admin.sql
├── scripts/smoke_test.sh
└── src/
    ├── main.cc
    ├── common/
    │   ├── AppContext.h/.cc
    │   ├── AppError.h
    │   ├── ApiResponse.h
    │   ├── CryptoUtil.h/.cc
    │   ├── JwtCodec.h/.cc
    │   ├── Once.h
    │   ├── ParamUtil.h
    │   ├── PasswordHasher.h/.cc
    │   ├── Responder.h
    │   └── Result.h
    ├── config/
    │   └── AppConfig.h/.cc
    ├── models/
    │   ├── AdminUser.h
    │   ├── AuditLog.h
    │   ├── ModelJson.h
    │   ├── Stats.h
    │   └── UserSummary.h
    ├── controllers/
    │   ├── AuthController.h/.cc
    │   ├── HealthController.h/.cc
    │   ├── PlaceholderController.h/.cc
    │   ├── StatsController.h/.cc
    │   └── UserController.h/.cc
    ├── services/
    │   ├── AuditService.h/.cc
    │   ├── AuthService.h/.cc
    │   ├── StatsService.h/.cc
    │   └── UserService.h/.cc
    ├── repositories/
    │   ├── AdminRepository.h/.cc
    │   ├── AuditRepository.h/.cc
    │   ├── SessionRepository.h/.cc
    │   ├── StatsRepository.h/.cc
    │   └── UserRepository.h/.cc
    └── filters/
        ├── AccessLogAdvice.h/.cc
        ├── AuthFilter.h/.cc
        ├── CorsAdvice.h/.cc
        ├── ErrorAdvice.h/.cc
        ├── RbacFilter.h/.cc
        └── RequestIdAdvice.h/.cc
```

## 依赖

- C++17 编译器（MSVC 2022 / GCC）
- CMake ≥ 3.16
- vcpkg
- Drogon 1.9.x
- nlohmann-json
- OpenSSL
- MariaDB / MySQL

## 构建

Windows（Git Bash）：

```bash
cd dev/Admin/AdminBackend
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="<vcpkg>/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Debug -j
```

Linux：

```bash
cd dev/Admin/AdminBackend
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="<vcpkg>/scripts/buildsystems/vcpkg.cmake"
cmake --build build -j
```

## 配置

```bash
cp config.json.example config.json
```

修改 `config.json`：

- `db_clients[0]`：MariaDB 地址、端口、库名、账号、密码
- `custom_config.admin.jwt_secret`：至少 16 个字符，建议 32 字节以上随机值
- `custom_config.admin.jwt_issuer`：默认 `kc-cxim-admin`
- `custom_config.admin.cors_origin`：默认 `http://localhost:5173`
- `listeners[0].port`：默认 `8080`

生成 JWT 密钥示例：

```bash
python -c "import secrets; print(secrets.token_hex(32))"
```

`config.json` 已加入 `.gitignore`，不要提交真实密钥。

## 初始化数据库

```bash
mysql -u root -p < db/schema_admin.sql
```

如需为应用创建独立的数据库账号：

```sql
CREATE USER IF NOT EXISTS 'cx_admin'@'%' IDENTIFIED BY 'CHANGE_ME';
GRANT SELECT, INSERT, UPDATE, DELETE ON CXIMDB.* TO 'cx_admin'@'%';
FLUSH PRIVILEGES;
```

脚本会：

- 创建 / 使用 `CXIMDB`
- 创建 `admin_users`、`admin_sessions`、`audit_logs`
- 写入演示管理员

演示账号：

```text
用户名：admin
密码：Admin@123
```

演示密码使用 PBKDF2-HMAC-SHA256 存储。生产环境请立即修改密码哈希生成方式或替换为 Argon2/bcrypt。

## 启动

Windows：

```bash
./build/Debug/kc-cxim-admin-backend.exe ./config.json
```

Linux：

```bash
./build/kc-cxim-admin-backend ./config.json
```

启动后监听 `0.0.0.0:8080`。

## 前端联调

前端项目：`dev/Admin/AdminPanel`

Vite 开发代理已将 `/admin/api` 转发到 `http://127.0.0.1:8080`。启动后端后，前端直接访问：

```text
http://localhost:5173
```

> 前端登录页当前演示提示为 `admin / admin123`，本后端种子密码为 `admin / Admin@123`；联调时请统一两处，或修改前端提示文案。

登录接口：

```bash
curl -s -X POST http://127.0.0.1:8080/admin/api/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"admin","password":"Admin@123"}'
```

成功响应：

```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "token": "<access-token>",
    "refreshToken": "<refresh-token>",
    "admin": {
      "id": 1,
      "username": "admin",
      "name": "超级管理员",
      "role": "super_admin",
      "roleName": "超级管理员"
    }
  },
  "request_id": "..."
}
```

## API

| 方法 | 路径 | 认证 | 说明 |
| --- | --- | --- | --- |
| GET | `/healthz` | 否 | 健康检查 |
| POST | `/admin/api/auth/login` | 否 | 管理员登录 |
| POST | `/admin/api/auth/refresh` | 否 | 刷新 token |
| POST | `/admin/api/auth/logout` | 是 | 登出 |
| GET | `/admin/api/auth/me` | 是 | 当前管理员 |
| GET | `/admin/api/users` | 是 | 用户分页查询 |
| GET | `/admin/api/users/{id}` | 是 | 用户详情 |
| PATCH | `/admin/api/users/{id}/status` | 是 | 封禁 / 解禁 |
| POST | `/admin/api/users/{id}/reset-password` | 是 | 重置密码（骨架返回 501） |
| POST | `/admin/api/users/{id}/kick` | 是 | 强制下线（骨架返回 501） |
| GET | `/admin/api/stats/overview` | 是 | 核心指标 |
| GET | `/admin/api/stats/trend?days=7` | 是 | 趋势数据 |

以下路由当前返回 `501 NOT_IMPLEMENTED`：

`friends`、`groups`、`messages`、`moderation`、`push`、`files`、`configs`、`audit-logs`、`admins`、`roles`。

## 错误格式

成功：

```json
{"code":0,"message":"ok","data":{},"request_id":"..."}
```

错误（严格三个字段）：

```json
{"code":"INVALID_PARAM","field":"page","message":"页码必须大于 0"}
```

状态码：

- 400 参数错误
- 401 未认证 / token 过期
- 403 无权限
- 404 资源不存在
- 409 状态冲突
- 500 内部错误（不泄露 SQL、堆栈、异常原文）
- 501 未实现

## 分层与异步约定

- `controllers/`：路由、参数解析、调用 service、通过 `Responder` 返回
- `services/`：业务校验与编排、审计日志
- `repositories/`：唯一允许出现 SQL 的层，使用 `execSqlAsync` + `?` 参数绑定
- `filters/`：RequestId、CORS、访问日志、Auth、RBAC、500 兜底
- 所有 handler 与异步回调都有 `try/catch`
- `Responder` / `OnceCallback` 保证响应和回调恰好一次
- 禁止 `[&]`、`[this]`；service/repository 使用 `shared_from_this()`
- 禁止 `execSqlSync`、`sleep`、同步文件 IO、`newHttpJsonResponse`、`getJsonObject`

## 说明与后续

- `onlineUsers` 暂无共享数据源（IM 在线状态在进程内 `SessionManager`），返回 `0` 且 `onlineUsersAvailable=false`。
- `lastActive` 使用 `users.updated_at` 近似。
- `reset-password`、`kick` 需要后续接入用户端能力。
- 群组、审核、推送、文件、配置、审计查询等待后续迭代。
- 生产环境建议补充：TLS、独立数据库只读/写账号、登录限流、2FA、审计导出审批。

## 自检命令

```bash
grep -rn "execSqlSync" src/ || true
grep -rn "sleep\|Sleep\|usleep" src/ || true
grep -rn "\[&\]" src/ || true
grep -rn "\[this\]" src/ || true
grep -rn "newHttpJsonResponse\|getJsonObject\|Json::Value" src/ || true
grep -rniE "select |insert |update |delete " src/controllers/ src/services/ || true
```

## 冒烟测试

```bash
bash scripts/smoke_test.sh
```

可通过环境变量覆盖：

```bash
BASE_URL=http://127.0.0.1:8080 \
CX_ADMIN_USER=admin \
CX_ADMIN_PASS=Admin@123 \
bash scripts/smoke_test.sh
```
