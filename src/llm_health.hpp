/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LLM_HEALTH_HPP
#define LLM_HEALTH_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace omni_agent {

enum class LlmHealthStatus {
    HEALTHY,       // /v1/models 返回 200 且 data[] 非空
    UNREACHABLE,   // DNS / connect / read 超时 / 连接被拒
    NO_MODEL,      // HTTP 2xx 但 data[] 为空
    BAD_RESPONSE,  // 非 2xx / 报文格式错 / JSON 解析失败 / 鉴权失败
};

struct LlmHealthResult {
    LlmHealthStatus status = LlmHealthStatus::UNREACHABLE;
    int32_t http_code = 0;                // 0 表示没成功完成 HTTP 交换
    std::string detail;                   // 给人看的失败原因；HEALTHY 时也可有简短描述
    std::vector<std::string> model_ids;   // /v1/models 返回的 data[].id 列表
};

// 探测 LLM 端点 /models 接口。
// base_url 形如 "http://127.0.0.1:9191/v1" 或 "http://api.example.com/v1"，
// 末尾可带可不带 '/'。仅支持 http://（daemon 场景；不引入 TLS 依赖）。
// api_key 为空表示不发 Authorization 头。
// timeout_sec 同时作为 connect/send/recv 三段的上限（粗略）。
LlmHealthResult ProbeLlmModels(const std::string& base_url,
                                const std::string& api_key,
                                int32_t timeout_sec);

}  // namespace omni_agent

#endif  // LLM_HEALTH_HPP
