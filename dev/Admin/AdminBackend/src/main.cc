#include <drogon/drogon.h>

#include <memory>
#include <string>

#include "common/ApiResponse.h"
#include "common/AppContext.h"
#include "common/AppError.h"
#include "config/AppConfig.h"
#include "filters/AccessLogAdvice.h"
#include "filters/AuthFilter.h"
#include "filters/CorsAdvice.h"
#include "filters/ErrorAdvice.h"
#include "filters/RbacFilter.h"
#include "filters/RequestIdAdvice.h"

int main(int argc, char **argv)
{
    const std::string configPath =
        argc > 1 ? argv[1] : "./config.json";

    try
    {
        drogon::app().loadConfigFile(configPath);
        const AppConfig config = AppConfig::fromDrogon();

        drogon::app().registerFilter(
            std::make_shared<AuthFilter>(config.jwtSecret,
                                         config.jwtIssuer));
        drogon::app().registerFilter(std::make_shared<RbacFilter>());

        registerRequestIdAdvice();
        registerCorsAdvice(config.corsOrigin);
        registerAccessLogAdvice();
        registerErrorAdvice();

        drogon::app().setCustom404Page(
            makeError(AppError::notFound("", "请求的资源不存在")));

        drogon::app().registerBeginningAdvice([config]() {
            try
            {
                auto db = drogon::app().getDbClient("default");
                if (!db)
                {
                    LOG_FATAL << "db client 'default' is not configured";
                    drogon::app().quit();
                    return;
                }
                AppContext::instance().init(config, db);
                LOG_INFO << "[admin] db client ready";
            }
            catch (const std::exception &e)
            {
                LOG_FATAL << "initialize app context failed: " << e.what();
                drogon::app().quit();
            }
            catch (...)
            {
                LOG_FATAL << "initialize app context failed";
                drogon::app().quit();
            }
        });

        LOG_INFO << "[admin] config loaded";
        LOG_INFO << "[admin] filters registered";

        drogon::app().run();
    }
    catch (const std::exception &e)
    {
        LOG_FATAL << "startup failed: " << e.what();
        return 1;
    }
    catch (...)
    {
        LOG_FATAL << "startup failed with unknown exception";
        return 1;
    }

    return 0;
}
