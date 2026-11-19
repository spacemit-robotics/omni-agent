/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "daemon_config.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "ASSERTION FAILED: " << message << std::endl;
        std::exit(1);
    }
}

void writeConfig(const std::string& dir, const std::string& name, const std::string& body) {
    std::ofstream output(dir + "/" + name, std::ios::trunc);
    require(output.good(), "failed to create " + name);
    output << body;
}

nlohmann::json mergedConfig(const omni_agent::DaemonConfig& config) {
    return nlohmann::json::parse(omni_agent::DumpMergedConfig(config));
}

void testDefaultTemplates(const std::string& dir) {
    omni_agent::WriteDefaultConfigs(dir, true);
    const auto config = omni_agent::LoadConfig(dir);
    require(config.voice_chat_status.loaded, "default voice_chat.json loads: " + config.voice_chat_status.error);
    require(config.asr.provider.empty(), "default asr.provider keeps the engine preset");
}

void testAsrProvider(const std::string& dir) {
    for (const std::string provider : {"cpu", "spacemit"}) {
        writeConfig(dir, "voice_chat.json", R"({"asr": {"engine": "sensevoice", "provider": ")" + provider + R"("}})");
        const auto config = omni_agent::LoadConfig(dir);
        require(config.voice_chat_status.loaded, "asr.provider " + provider + " loads: " + config.voice_chat_status.error);
        require(config.asr.provider == provider, "asr.provider " + provider + " parsed");
        require(mergedConfig(config)["voice_chat"]["asr"]["provider"] == provider,
            "merged config shows asr.provider " + provider);
    }

    writeConfig(dir, "voice_chat.json", R"({"asr": {"engine": "sensevoice"}})");
    auto config = omni_agent::LoadConfig(dir);
    require(config.voice_chat_status.loaded && config.asr.provider.empty(), "absent asr.provider stays empty");

    writeConfig(dir, "voice_chat.json", R"({"asr": {"provider": "gpu"}})");
    config = omni_agent::LoadConfig(dir);
    require(!config.voice_chat_status.loaded, "unknown asr.provider is rejected");
    writeConfig(dir, "voice_chat.json", R"({"asr": {"provider": "CPU"}})");
    config = omni_agent::LoadConfig(dir);
    require(!config.voice_chat_status.loaded, "asr.provider is case sensitive");
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected temporary config directory");
    testDefaultTemplates(argv[1]);
    testAsrProvider(argv[1]);
    std::cout << "daemon_config_test: all passed" << std::endl;
    return 0;
}
