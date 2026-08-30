#!/bin/bash
# ============================================================================
# run_remote_a2a_tests.sh
# 构建并运行 remote_a2a 全部测试套（离线，不发任何网络包）。
#
# 用法: cd HunterClaw && bash tests/run_remote_a2a_tests.sh
# ============================================================================
set -u

CXX="${CXX:-/usr/local/gcc-12.2.0/bin/g++}"
INC="-Iinclude -Iexternal/json/single_include -Ithird_party"
OUT="${OUT:-/tmp/hc_ra2a_tests}"
mkdir -p "$OUT"

# remote_a2a 源码分两层：
#   LOW  —— 不依赖 tools::（Tool / ToolParams / tool_result）
#   HIGH —— 依赖 tools::，需要连带 stub
# ★ 这样分层不只是为了链接方便：它同时验证了「核心不依赖 tools 层」
#   这条边界 —— 若哪天 LOW 层误引了 tools::，rpc/exec/callback 三套会立刻链接失败。
LOW_SRCS="
src/remote_a2a/types.cc
src/remote_a2a/task_tracking.cc
src/remote_a2a/probe_policy.cc
src/remote_a2a/url_safety.cc
src/remote_a2a/agent_card.cc
src/remote_a2a/card_sanitizer.cc
src/remote_a2a/card_validator.cc
src/remote_a2a/auth.cc
src/remote_a2a/credential_resolver.cc
src/remote_a2a/json_rpc.cc
src/remote_a2a/card_resolver.cc
src/remote_a2a/remote_agent_client.cc
src/remote_a2a/pending_tasks.cc
src/remote_a2a/execution_engine.cc
src/remote_a2a/callback_endpoint.cc
"
# ⚠ callback_route.cc 不在任何一层：它依赖 httplib，留给集成测试。
HIGH_SRCS="
src/remote_a2a/agent_tool.cc
src/remote_a2a/config.cc
src/remote_a2a/agent_registry.cc
src/remote_a2a/runtime.cc
src/tools/before_tool_call_policy.cc
src/tools/tool_approval_runtime.cc
"

exists() {
  local out=""
  for f in $1; do
    [ -f "$f" ] && out="$out $f"
  done
  echo "$out"
}
LOW="$(exists "$LOW_SRCS")"
HIGH="$(exists "$HIGH_SRCS")"

INFRA="src/infra/http_client.cc src/infra/system_events.cc"
# ⚠ test_stub_approval_deps.cc 提供 core::get_logger / core::utils /
#   security::is_gateway_owner_only_core_tool / ToolHookManager 的占位实现 ——
#   before_tool_call_policy 依赖它们, 而链真实实现会拖入
#   llm/sessions/memory 一大串。
STUBS="tests/test_stub_tool_result.cc tests/test_stub_tool_manager.cc tests/test_stub_approval_deps.cc"

TOTAL_PASS=0
TOTAL_FAIL=0
FAILED_SUITES=""

run_suite() {
  local name="$1"; shift
  local extra="$*"
  printf "%-34s " "$name"
  if ! $CXX -std=c++17 $INC "tests/$name.cc" $extra -lpthread -o "$OUT/$name" \
       > "$OUT/$name.build.log" 2>&1; then
    echo "BUILD FAILED (见 $OUT/$name.build.log)"
    FAILED_SUITES="$FAILED_SUITES $name(build)"
    return
  fi
  local out
  out="$("$OUT/$name" 2>&1)"
  local line
  line="$(echo "$out" | grep -oE '结果: [0-9]+ 通过, [0-9]+ 失败|PASS: [0-9]+  FAIL: [0-9]+' | tail -1)"
  local p f
  p="$(echo "$line" | grep -oE '[0-9]+' | head -1)"
  f="$(echo "$line" | grep -oE '[0-9]+' | tail -1)"
  TOTAL_PASS=$((TOTAL_PASS + ${p:-0}))
  TOTAL_FAIL=$((TOTAL_FAIL + ${f:-0}))
  echo "$line"
  if [ "${f:-1}" != "0" ]; then
    FAILED_SUITES="$FAILED_SUITES $name"
    echo "$out" | grep "FAIL:" | head -8
  fi
}

echo "===== remote_a2a 测试套 ====="

run_suite test_remote_a2a_core \
  src/remote_a2a/types.cc src/remote_a2a/task_tracking.cc

run_suite test_remote_a2a_security \
  src/remote_a2a/url_safety.cc src/remote_a2a/agent_card.cc \
  src/remote_a2a/card_sanitizer.cc src/remote_a2a/card_validator.cc

run_suite test_remote_a2a_auth \
  src/remote_a2a/auth.cc src/remote_a2a/credential_resolver.cc

run_suite test_remote_a2a_rpc          $LOW $INFRA
run_suite test_remote_a2a_exec         $LOW $HIGH $INFRA $STUBS
run_suite test_remote_a2a_callback     $LOW $INFRA
run_suite test_pending_task_resume     $LOW $INFRA
run_suite test_remote_a2a_tools        $LOW $HIGH $INFRA $STUBS
run_suite test_remote_a2a_registry     $LOW $HIGH $INFRA $STUBS
run_suite test_remote_a2a_runtime      $LOW $HIGH $INFRA $STUBS

echo
echo "===== 合计: $TOTAL_PASS 通过, $TOTAL_FAIL 失败 ====="

# ── 项目级约束自查 ────────────────────────────────────────────────────────
echo
echo "--- 禁用并发原语自查（应为空）---"
grep -rn "std::future\|std::promise\|std::async\|packaged_task" \
     src/remote_a2a/ include/hunterclaw/remote_a2a/ \
  | grep -vE ":[0-9]+:[[:space:]]*(//|\*|/\*)" || echo "(通过)"

echo
echo "--- callback_route 之外不得 include httplib（应为空）---"
# ⚠ 只匹配真实的 #include 行 —— 注释里提到 httplib 是允许的
#   （runtime.h 就在注释里解释了「为什么不 include httplib」）。
grep -rn "^[[:space:]]*#[[:space:]]*include.*httplib" \
     src/remote_a2a/ include/hunterclaw/remote_a2a/ \
  | grep -v "callback_route" || echo "(通过)"

echo
echo "--- 核心头文件不得 include gateway（应为空）---"
# ★ runtime.h 通过 std::function 注入挂载器来避开 gateway 依赖 ——
#   若哪天有人图省事直接 include gateway/http_gateway.h，这里会报出来。
grep -rn "^[[:space:]]*#[[:space:]]*include.*gateway/" \
     src/remote_a2a/ include/hunterclaw/remote_a2a/ \
  | grep -v "callback_route" || echo "(通过)"

if [ -n "$FAILED_SUITES" ]; then
  echo
  echo "失败套件:$FAILED_SUITES"
  exit 1
fi
exit 0
