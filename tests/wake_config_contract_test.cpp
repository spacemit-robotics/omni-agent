/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "daemon_config.hpp"
#include "wake_text_filter.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "ASSERTION FAILED: " << message << std::endl;
        std::exit(1);
    }
}

void requireFilter(
        const std::string& input,
        const std::vector<std::string>& phrases,
        const std::string& expected,
        bool expected_changed,
        bool expected_drop) {
    const auto result = omni_agent::filterWakeAsrText(input, phrases);
    require(result.text == expected, "unexpected filtered text for: " + input);
    require(result.changed == expected_changed, "unexpected changed flag for: " + input);
    require(result.drop == expected_drop, "unexpected drop flag for: " + input);
}

void writeVoiceConfig(const std::string& dir, const std::string& body) {
    std::ofstream output(dir + "/voice_chat.json", std::ios::trunc);
    require(output.good(), "failed to create voice_chat.json");
    output << body;
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected temporary config directory");
    const std::string config_dir = argv[1];

    const auto defaults = omni_agent::defaultWakePhrases();
    std::string validation_error;
    require(
        omni_agent::validateWakePhrases(defaults, &validation_error),
        "default wake phrases must be valid: " + validation_error);
    const auto phrases = omni_agent::flattenWakePhrases(defaults);

    requireFilter("小静，小静。", phrases, "", true, true);
    requireFilter(
        " 小静，小静，今天天气怎么样？ ",
        phrases,
        "今天天气怎么样？",
        true,
        false);
    requireFilter("小进 小进打开空调", phrases, "打开空调", true, false);
    requireFilter("小井小井，往前走十米。", phrases, "往前走十米。", true, false);
    requireFilter("小进小进小进小进", phrases, "", true, true);
    requireFilter(
        "小进小进，小进小进，往前走一米",
        phrases,
        "往前走一米",
        true,
        false);
    requireFilter("小静小静小进小进打开空调", phrases, "打开空调", true, false);
    requireFilter("小心，小心！", phrases, "", true, true);
    requireFilter("Tentu.", phrases, "", true, true);
    requireFilter("Bien.", phrases, "", true, true);
    requireFilter("嗯。", phrases, "", true, true);
    requireFilter("打开空调", phrases, "打开空调", false, false);
    requireFilter(
        "HELLO, ROBOT turn on the light",
        {"hello robot"},
        "turn on the light",
        true,
        false);
    require(
        omni_agent::shouldDropUnmatchedWakeAsr(true, 0),
        "wake-only first segment without post-HID speech must be dropped");
    require(
        omni_agent::shouldDropUnmatchedWakeAsr(true, 1),
        "single post-HID VAD frame is insufficient continuation evidence");
    require(
        !omni_agent::shouldDropUnmatchedWakeAsr(true, 2),
        "continuous post-HID speech must preserve an unmatched command");
    require(
        !omni_agent::shouldDropUnmatchedWakeAsr(false, 0),
        "a follow-up utterance must not use the first-segment guard");
    validation_error.clear();
    require(
        !omni_agent::validateWakePhrases(
            {{"duplicate", "小进小进", {"小进，小进"}}},
            &validation_error),
        "normalized duplicate aliases must fail validation");

    writeVoiceConfig(config_dir, R"({
        "wake": {
            "enabled": true,
            "strip_from_asr": true,
            "command_timeout_ms": 4321,
            "phrases": [{
                "id": "custom",
                "canonical": "你好小智",
                "asr_aliases": ["你好小志"]
            }]
        }
    })");
    auto config = omni_agent::LoadConfig(config_dir);
    require(config.voice_chat_status.error.empty(), "custom wake config must parse");
    require(config.wake.enabled, "custom wake config must enable wake");
    require(config.wake.strip_from_asr, "custom wake config must enable stripping");
    require(config.wake.command_timeout_ms == 4321, "custom timeout must parse");
    require(config.wake.phrases.size() == 1, "custom phrase must replace defaults");
    require(config.wake.phrases[0].canonical == "你好小智", "custom canonical must parse");

    writeVoiceConfig(config_dir, R"({
        "wake": {
            "enabled": true,
            "drop_wake_asr": false,
            "drop_audio_ms": 500,
            "post_ack_tail_ms": 0
        }
    })");
    config = omni_agent::LoadConfig(config_dir);
    require(config.voice_chat_status.error.empty(), "legacy wake config must parse");
    require(!config.wake.strip_from_asr, "legacy drop_wake_asr must remain compatible");
    require(
        config.wake.legacy_drop_wake_asr_configured,
        "legacy drop_wake_asr use must be observable for a deprecation warning");
    require(
        config.wake.legacy_audio_drop_configured,
        "legacy audio drop fields must be observable for a deprecation warning");

    writeVoiceConfig(config_dir, R"({
        "wake": {
            "enabled": true,
            "strip_from_asr": true,
            "phrases": []
        }
    })");
    config = omni_agent::LoadConfig(config_dir);
    require(
        config.voice_chat_status.error == "wake.phrases must not be empty",
        "empty configured phrase list must fail explicitly");

    writeVoiceConfig(config_dir, R"({
        "wake": {
            "enabled": true,
            "strip_from_asr": true,
            "phrases": [{
                "id": "duplicate",
                "canonical": "小进小进",
                "asr_aliases": ["小进，小进"]
            }]
        }
    })");
    config = omni_agent::LoadConfig(config_dir);
    require(
        config.voice_chat_status.error.find("duplicate wake phrase alias") !=
            std::string::npos,
        "duplicate configured aliases must fail explicitly");

    writeVoiceConfig(config_dir, R"({
        "wake": {
            "enabled": false,
            "strip_from_asr": true,
            "phrases": []
        }
    })");
    config = omni_agent::LoadConfig(config_dir);
    require(
        config.voice_chat_status.error.empty(),
        "phrase table is not validated while wake is disabled");
    require(!config.wake.enabled, "wake must stay disabled");

    writeVoiceConfig(config_dir, R"({
        "wake": {
            "enabled": true,
            "strip_from_asr": true,
            "phrases": null
        }
    })");
    config = omni_agent::LoadConfig(config_dir);
    require(config.voice_chat_status.error.empty(), "null phrases must parse");
    require(
        config.wake.phrases.size() == defaults.size(),
        "null phrases must keep the built-in defaults");

    require(config.wake.source == "hid", "wake source must default to HID so older configs keep working");
    require(config.wake.kws.threshold == 0.3f, "KWS threshold must default to 0.3");
    require(config.wake.kws.holdoff_ms == 1000, "KWS holdoff must default to 1000 ms");
    require(config.wake.kws.partial_threshold == 0.0f, "KWS partial match must default to off");
    require(!config.wake.kws.echo_null, "KWS echo null must default to off");
    require(config.wake.kws.echo_null_first_channel == 2, "KWS echo null must default to ch2-ch4");
    require(!config.debug.save_aec_dump, "AEC dump must default to off");

    writeVoiceConfig(config_dir, R"({
        "debug": {"save_aec_dump": true, "aec_dump_dir": "/data/aec_dumps"}
    })");
    config = omni_agent::LoadConfig(config_dir);
    require(config.voice_chat_status.error.empty(), "AEC dump config must parse");
    require(config.debug.save_aec_dump, "debug.save_aec_dump must parse");
    require(config.debug.aec_dump_dir == "/data/aec_dumps", "debug.aec_dump_dir must parse");

    writeVoiceConfig(config_dir, R"({
        "wake": {
            "enabled": true,
            "source": "kws",
            "kws": {"model_dir": "/opt/kws/xiaojin-ft05", "threshold": 0.45, "holdoff_ms": 0,
                    "partial_threshold": 0.9, "echo_null": true, "echo_null_first_channel": 3}
        }
    })");
    config = omni_agent::LoadConfig(config_dir);
    require(config.voice_chat_status.error.empty(), "KWS wake config must parse");
    require(config.wake.kws.model_dir == "/opt/kws/xiaojin-ft05", "KWS model_dir must parse");
    require(config.wake.kws.threshold == 0.45f, "KWS threshold must parse");
    require(config.wake.kws.holdoff_ms == 0, "KWS holdoff 0 must be allowed");
    require(config.wake.kws.partial_threshold == 0.9f, "KWS partial threshold must parse");
    require(config.wake.kws.echo_null, "KWS echo null must parse");
    require(config.wake.kws.echo_null_first_channel == 3, "KWS echo null first channel must parse");

    writeVoiceConfig(config_dir, R"({
        "wake": {"enabled": true, "source": "hid", "device": "/dev/hidraw1"}
    })");
    config = omni_agent::LoadConfig(config_dir);
    require(config.voice_chat_status.error.empty(), "HID wake config must parse");
    require(config.wake.source == "hid", "HID wake source must parse");
    require(config.wake.device == "/dev/hidraw1", "HID device must parse");

    writeVoiceConfig(config_dir, R"({
        "wake": {"enabled": true, "source": "button"}
    })");
    config = omni_agent::LoadConfig(config_dir);
    require(
        config.voice_chat_status.error.find("wake.source") != std::string::npos,
        "unknown wake source must fail explicitly");

    writeVoiceConfig(config_dir, R"({
        "wake": {"enabled": true, "kws": {"threshold": 0}}
    })");
    config = omni_agent::LoadConfig(config_dir);
    require(
        config.voice_chat_status.error.find("wake.kws.threshold") != std::string::npos,
        "out-of-range KWS threshold must fail explicitly");

    writeVoiceConfig(config_dir, R"({
        "wake": {"enabled": true, "kws": {"echo_null": true, "echo_null_first_channel": 0}}
    })");
    config = omni_agent::LoadConfig(config_dir);
    require(
        config.voice_chat_status.error.find("wake.kws.echo_null_first_channel") != std::string::npos,
        "a 0 echo null channel must fail explicitly");

    require(config.aec.reference_channel == 0, "AEC reference must default to the software loopback");
    require(config.aec.wake_echo_guard == "auto", "wake echo guard must default to auto");
    {
        std::ofstream aec(config_dir + "/aec.json", std::ios::trunc);
        aec << R"({"reference_channel": 1, "reference_gain_db": 9.5, "wake_echo_guard": "on"})";
    }
    config = omni_agent::LoadConfig(config_dir);
    require(config.aec_status.error.empty(), "AEC reference config must parse");
    require(config.aec.reference_channel == 1, "aec.reference_channel must parse");
    require(config.aec.reference_gain_db == 9.5f, "aec.reference_gain_db must parse");
    require(config.aec.wake_echo_guard == "on", "aec.wake_echo_guard must parse");
    {
        std::ofstream aec(config_dir + "/aec.json", std::ios::trunc);
        aec << R"({"wake_echo_guard": "sometimes"})";
    }
    config = omni_agent::LoadConfig(config_dir);
    require(
        config.aec_status.error.find("aec.wake_echo_guard") != std::string::npos,
        "an unknown wake echo guard mode must fail explicitly");
    {
        std::ofstream aec(config_dir + "/aec.json", std::ios::trunc);
        aec << R"({"reference_channel": -1})";
    }
    config = omni_agent::LoadConfig(config_dir);
    require(
        config.aec_status.error.find("aec.reference_channel") != std::string::npos,
        "a negative AEC reference channel must fail explicitly");
    std::remove((config_dir + "/aec.json").c_str());

    writeVoiceConfig(config_dir, "{ invalid json");
    config = omni_agent::LoadConfig(config_dir);
    require(
        !config.voice_chat_status.error.empty(),
        "malformed voice_chat.json must report a load error");

    std::cout << "PASS --wake-config-contract" << std::endl;
    return 0;
}
