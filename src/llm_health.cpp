/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "llm_health.hpp"

#include <exception>
#include <mutex>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace omni_agent {

namespace {

constexpr size_t kMaxResponseBytes = 1024 * 1024;  // 防御上限：1 MB

// libcurl 全局只需初始化一次。probe 在 LLMService 构造前运行，不能假设
// curl_global_init 已被别处调用，用 once_flag 自保。进程退出交给 OS 回收，
// 不配对 curl_global_cleanup（避免与其他 curl 使用者的 cleanup 互相干扰）。
void EnsureCurlGlobalInit() {
    static std::once_flag once;
    std::call_once(once, []() {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

size_t WriteToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    const size_t n = size * nmemb;
    if (out->size() + n > kMaxResponseBytes) {
        return 0;  // 超上限，返回短计数让 curl 以写错误中止
    }
    out->append(ptr, n);
    return n;
}

// base_url 末尾去掉多余 '/' 后拼 "/models"，与 LLMService 用的 base_url 对齐。
std::string JoinModelsUrl(const std::string& base_url) {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url + "/models";
}

// 防御解析 OpenAI /v1/models 响应：返回 data[].id 列表。
// 若 body 不是合法 JSON 或没有 data 数组，返回空 vector 并把原因写入 *error。
std::vector<std::string> ExtractModelIds(const std::string& body, std::string* error) {
    std::vector<std::string> ids;
    try {
        const auto j = nlohmann::json::parse(body);
        if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) {
            if (error) {
                *error = "response has no data[] array";
            }
            return ids;
        }
        for (const auto& item : j["data"]) {
            if (item.is_object() && item.contains("id") && item["id"].is_string()) {
                ids.push_back(item["id"].get<std::string>());
            }
        }
    } catch (const std::exception& e) {
        if (error) {
            *error = std::string("json parse: ") + e.what();
        }
        ids.clear();
    }
    return ids;
}

std::string ExtractErrorMessage(const std::string& body) {
    try {
        const auto j = nlohmann::json::parse(body);
        if (j.is_object() && j.contains("error") && j["error"].is_object()
                && j["error"].contains("message")
                && j["error"]["message"].is_string()) {
            return j["error"]["message"].get<std::string>();
        }
    } catch (const std::exception&) {
        // ignore
    }
    return "";
}

}  // namespace

LlmHealthResult ProbeLlmModels(const std::string& base_url,
        const std::string& api_key, int32_t timeout_sec) {
    LlmHealthResult result;
    if (timeout_sec <= 0) {
        timeout_sec = 5;
    }

    EnsureCurlGlobalInit();
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        result.status = LlmHealthStatus::UNREACHABLE;
        result.detail = "curl_easy_init failed";
        return result;
    }

    const std::string url = JoinModelsUrl(base_url);
    std::string body;
    char errbuf[CURL_ERROR_SIZE] = {0};

    // 用临时指针接收 curl_slist_append 的返回值：append 失败时返回 NULL，
    // 直接覆盖 headers 会丢失之前已分配的节点（无法被 curl_slist_free_all 释放）。
    struct curl_slist* headers = nullptr;
    if (auto* h = curl_slist_append(headers, "Accept: application/json")) {
        headers = h;
    }
    std::string auth;
    if (!api_key.empty()) {
        auth = "Authorization: Bearer " + api_key;
        if (auto* h = curl_slist_append(headers, auth.c_str())) {
            headers = h;
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<int64_t>(timeout_sec));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<int64_t>(timeout_sec));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "omni_agent/voice_chat");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    const CURLcode rc = curl_easy_perform(curl);

    int64_t http_code = 0;  // LP64: long == int64_t，getinfo 写入安全
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    result.http_code = static_cast<int32_t>(http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        // 连接/超时类错误归 UNREACHABLE（可重试），其余归 BAD_RESPONSE。
        switch (rc) {
            case CURLE_COULDNT_CONNECT:
            case CURLE_COULDNT_RESOLVE_HOST:
            case CURLE_COULDNT_RESOLVE_PROXY:
            case CURLE_OPERATION_TIMEDOUT:
            case CURLE_SEND_ERROR:
            case CURLE_RECV_ERROR:
            case CURLE_GOT_NOTHING:
                result.status = LlmHealthStatus::UNREACHABLE;
                break;
            default:
                result.status = LlmHealthStatus::BAD_RESPONSE;
                break;
        }
        const char* msg = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        result.detail = std::string("curl: ") + msg + " (url=" + url + ")";
        return result;
    }

    if (http_code == 401 || http_code == 403) {
        result.status = LlmHealthStatus::BAD_RESPONSE;
        result.detail = "HTTP " + std::to_string(http_code) + " 鉴权失败";
        const std::string em = ExtractErrorMessage(body);
        if (!em.empty()) {
            result.detail += ": " + em;
        }
        return result;
    }
    if (http_code < 200 || http_code >= 300) {
        result.status = LlmHealthStatus::BAD_RESPONSE;
        result.detail = "HTTP " + std::to_string(http_code);
        const std::string em = ExtractErrorMessage(body);
        if (!em.empty()) {
            result.detail += ": " + em;
        }
        return result;
    }
    if (body.empty()) {
        result.status = LlmHealthStatus::BAD_RESPONSE;
        result.detail = "HTTP " + std::to_string(http_code) + ": empty body";
        return result;
    }

    std::string parse_err;
    result.model_ids = ExtractModelIds(body, &parse_err);
    if (!parse_err.empty()) {
        result.status = LlmHealthStatus::BAD_RESPONSE;
        result.detail = parse_err;
        return result;
    }
    if (result.model_ids.empty()) {
        result.status = LlmHealthStatus::NO_MODEL;
        result.detail = "/models returned data[] empty";
        return result;
    }

    result.status = LlmHealthStatus::HEALTHY;
    result.detail = "ok";
    return result;
}

}  // namespace omni_agent
