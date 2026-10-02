#!/usr/bin/env bash
set -euo pipefail

module_dir="application/native/omni_agent"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/omni-wake-test.XXXXXX")"

cleanup() {
  rm -rf "${build_dir}"
}
trap cleanup EXIT

json_flags=()
if pkg-config --exists nlohmann_json 2>/dev/null; then
  read -r -a json_flags <<< "$(pkg-config --cflags nlohmann_json)"
elif [[ -f /opt/homebrew/include/nlohmann/json.hpp ]]; then
  json_flags=(-I/opt/homebrew/include)
fi

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
  "${module_dir}/tests/wake_config_contract_test.cpp" \
  "${module_dir}/src/daemon_config.cpp" \
  "${module_dir}/src/wake_text_filter.cpp" \
  -I"${module_dir}/include" \
  -I"${module_dir}/src" \
  "${json_flags[@]}" \
  -o "${build_dir}/wake_config_contract_test"

mkdir -p "${build_dir}/config"
"${build_dir}/wake_config_contract_test" "${build_dir}/config"
