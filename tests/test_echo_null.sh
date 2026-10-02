#!/usr/bin/env bash
set -euo pipefail

module_dir="application/native/omni_agent"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/omni-echo-null-test.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror \
  "${module_dir}/tests/echo_null_test.cpp" \
  "${module_dir}/src/echo_null_beamformer.cpp" \
  -I"${module_dir}/include" \
  -o "${build_dir}/echo_null_test"

"${build_dir}/echo_null_test"
