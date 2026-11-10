#include "filters/ErrorAdvice.h"

#include <drogon/drogon.h>

#include <string>
#include "common/ApiResponse.h"
#include "common/AppError.h"

void registerErrorAdvice()
{
    drogon::app().registerPreSendingAdvice(
        [](const drogon::HttpRequestPtr &,
           const drogon::HttpResponsePtr &resp) {
            if (!resp ||
                resp->statusCode() != drogon::k500InternalServerError)
            {
                return;
            }

            const auto body = resp->body();
            if (body.rfind("{\"code\"", 0) == 0)
            {
                return;
            }

            const auto replacement = makeError(AppError::internal());
            resp->setContentTypeCode(replacement->contentType());
            resp->setBody(std::string(replacement->body()));
        });
}
