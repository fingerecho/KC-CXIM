#include "repositories/UserRepository.h"

#include <drogon/drogon.h>
#include <drogon/orm/Row.h>

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>
#include "common/Once.h"

namespace
{
constexpr const char *kUserSelect =
    "SELECT u.id, u.username, u.nickname, u.status, u.created_at, "
    "u.updated_at, "
    "(SELECT COUNT(*) FROM friends f "
    " WHERE f.user_id = u.id AND f.status = 1) AS friends_count, "
    "(SELECT COUNT(*) FROM conversation_members cm "
    " WHERE cm.user_id = u.id) AS conversations_count, "
    "(SELECT COUNT(*) FROM messages m "
    " WHERE m.sender_id = u.id) AS messages_count "
    "FROM users u";

constexpr const char *kUserFilter =
    " WHERE (? = '' OR u.username LIKE CONCAT('%', ?, '%') "
    "OR u.nickname LIKE CONCAT('%', ?, '%') "
    "OR CAST(u.id AS CHAR) = ?) "
    "AND (? = 'all' OR (? = 'active' AND u.status = 1) "
    "OR (? = 'banned' AND u.status = 0))";

std::string formatDateTime(const drogon::orm::Field &field)
{
    if (field.isNull())
    {
        return {};
    }
    auto value = field.as<std::string>();
    if (value.size() >= 16)
    {
        value.resize(16);
    }
    return value;
}

UserSummary mapUser(const drogon::orm::Row &row)
{
    UserSummary user;
    user.id = row["id"].as<unsigned long long>();
    user.username = row["username"].as<std::string>();
    user.nickname = row["nickname"].as<std::string>();
    user.status = row["status"].as<int>() == 1 ? "active" : "banned";
    user.friends = row["friends_count"].as<unsigned long long>();
    user.conversations =
        row["conversations_count"].as<unsigned long long>();
    user.messages = row["messages_count"].as<unsigned long long>();
    user.createdAt = formatDateTime(row["created_at"]);
    user.lastActive = formatDateTime(row["updated_at"]);
    return user;
}
}  // namespace

UserRepository::UserRepository(drogon::orm::DbClientPtr db)
    : db_(std::move(db))
{
}

void UserRepository::countAndList(
    int page,
    int pageSize,
    const std::string &keyword,
    const std::string &status,
    std::function<void(Result<UserPage>)> cb)
{
    struct ListState
    {
        std::atomic_int remaining{2};
        std::atomic_bool done{false};
        std::mutex mutex;
        std::uint64_t total{0};
        std::vector<UserSummary> rows;
        int page{1};
        int pageSize{20};
        std::function<void(Result<UserPage>)> cb;
    };

    auto state = std::make_shared<ListState>();
    state->page = page;
    state->pageSize = pageSize;
    state->cb = std::move(cb);

    auto finish = [state]() {
        if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) != 1)
        {
            return;
        }
        if (state->done.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }

        UserPage result;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            result.total = state->total;
            result.list = std::move(state->rows);
            result.page = state->page;
            result.pageSize = state->pageSize;
        }
        state->cb(Result<UserPage>(std::move(result)));
    };

    auto onError = [state](const drogon::orm::DrogonDbException &e) {
        if (state->done.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        LOG_ERROR << "query users failed: " << e.base().what();
        state->cb(AppError::internal());
    };

    const std::string countSql = std::string("SELECT COUNT(*) AS total FROM users u") +
                                 kUserFilter;
    const std::string listSql = std::string(kUserSelect) + kUserFilter +
                                " ORDER BY u.id DESC LIMIT ? OFFSET ?";

    const int offset = (page - 1) * pageSize;

    db_->execSqlAsync(
        countSql,
        [state, finish](const drogon::orm::Result &result) {
            try
            {
                std::uint64_t total = 0;
                if (!result.empty())
                {
                    total = result[0]["total"].as<unsigned long long>();
                }
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->total = total;
                }
                finish();
            }
            catch (const std::exception &e)
            {
                if (state->done.exchange(true, std::memory_order_acq_rel))
                {
                    return;
                }
                LOG_ERROR << "map user count failed: " << e.what();
                state->cb(AppError::internal());
            }
            catch (...)
            {
                if (state->done.exchange(true, std::memory_order_acq_rel))
                {
                    return;
                }
                state->cb(AppError::internal());
            }
        },
        onError,
        keyword,
        keyword,
        keyword,
        keyword,
        status,
        status,
        status);

    db_->execSqlAsync(
        listSql,
        [state, finish](const drogon::orm::Result &result) {
            try
            {
                std::vector<UserSummary> rows;
                rows.reserve(static_cast<std::size_t>(result.size()));
                for (const auto &row : result)
                {
                    rows.push_back(mapUser(row));
                }
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->rows = std::move(rows);
                }
                finish();
            }
            catch (const std::exception &e)
            {
                if (state->done.exchange(true, std::memory_order_acq_rel))
                {
                    return;
                }
                LOG_ERROR << "map user list failed: " << e.what();
                state->cb(AppError::internal());
            }
            catch (...)
            {
                if (state->done.exchange(true, std::memory_order_acq_rel))
                {
                    return;
                }
                state->cb(AppError::internal());
            }
        },
        onError,
        keyword,
        keyword,
        keyword,
        keyword,
        status,
        status,
        status,
        pageSize,
        offset);
}

void UserRepository::findById(
    std::uint64_t id,
    std::function<void(Result<UserSummary>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<UserSummary>>>(
        std::move(cb));

    const std::string sql =
        std::string(kUserSelect) + " WHERE u.id = ? LIMIT 1";

    db_->execSqlAsync(
        sql,
        [once](const drogon::orm::Result &result) {
            try
            {
                if (result.empty())
                {
                    (*once)(AppError::notFound("id", "用户不存在"));
                    return;
                }
                (*once)(Result<UserSummary>(mapUser(result[0])));
            }
            catch (const std::exception &e)
            {
                LOG_ERROR << "map user failed: " << e.what();
                (*once)(AppError::internal());
            }
            catch (...)
            {
                (*once)(AppError::internal());
            }
        },
        [once](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "find user failed: " << e.base().what();
            (*once)(AppError::internal());
        },
        static_cast<long long>(id));
}

void UserRepository::updateStatus(
    std::uint64_t id,
    int status,
    std::function<void(Result<bool>)> cb)
{
    auto once = std::make_shared<OnceCallback<Result<bool>>>(std::move(cb));

    db_->execSqlAsync(
        "UPDATE users SET status = ? WHERE id = ?",
        [once](const drogon::orm::Result &result) {
            (*once)(Result<bool>(result.affectedRows() > 0));
        },
        [once](const drogon::orm::DrogonDbException &e) {
            LOG_ERROR << "update user status failed: " << e.base().what();
            (*once)(AppError::internal());
        },
        status,
        static_cast<long long>(id));
}
