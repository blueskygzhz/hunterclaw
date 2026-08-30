#!/bin/bash
# ============================================================================
# run_remote_a2a_tests.sh
# 构建并运行 remote_a2a 全部测试套（离线，不发任何网络包）。
#
# 用法: cd HunterClaw && bash tests/run_remote_a2a_tests.sh
#
#   ASAN=1 bash tests/run_remote_a2a_tests.sh   # 带 AddressSanitizer 跑
#
# ★★ 为什么需要 ASAN 开关: 本子系统有 detached 后台线程 + 跨线程共享
#   PendingTaskStore。这类问题（use-after-free / 数据竞争）在普通构建下
#   【往往不崩】—— 测试全绿但内存已经错了。
#   ⚠ 改动 execution_engine / pending_tasks 的线程或生命周期相关代码后，
#     务必用 ASAN=1 跑一遍，否则等于没验证。
# ============================================================================
set -u

CXX="${CXX:-/usr/local/gcc-12.2.0/bin/g++}"
INC="-Iinclude -Iexternal/json/single_include -Ithird_party"
OUT="${OUT:-/tmp/hc_ra2a_tests}"
mkdir -p "$OUT"

# ── AddressSanitizer ────────────────────────────────────────────────────────
ASAN="${ASAN:-0}"
SAN_FLAGS=""
ASAN_PRELOAD=""
if [ "$ASAN" = "1" ]; then
  # ★ -O1 而非 -O0: ASan 在 -O0 下慢得多，且 -O1 仍保留完整栈帧
  SAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g -O1"
  # ⚠ 这套 gcc 装在非标准前缀（/usr/local/gcc-12.2.0），运行时找不到
  #   libasan.so —— 必须显式 LD_PRELOAD，否则每个套件都"BUILD 成功但跑不起来"。
  ASAN_PRELOAD="$($CXX -print-file-name=libasan.so 2>/dev/null)"
  if [ ! -f "$ASAN_PRELOAD" ]; then
    echo "!! 找不到 libasan.so，无法启用 ASan" >&2
    exit 1
  fi
  # ⚠ detect_leaks=0: 测试里有意保留的单例/detached 线程会报大量"泄漏"，
  #   那不是本脚本要查的问题（要查内存泄漏请单独跑 LSan）。
  export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:print_stacktrace=1"
  echo "[ASan] 已启用 (preload: $ASAN_PRELOAD)"
fi

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
src/tools/interrupt_coordinator.cc
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
  if ! $CXX -std=c++17 $SAN_FLAGS $INC "tests/$name.cc" $extra -lpthread -o "$OUT/$name" \
       > "$OUT/$name.build.log" 2>&1; then
    echo "BUILD FAILED (见 $OUT/$name.build.log)"
    FAILED_SUITES="$FAILED_SUITES $name(build)"
    return
  fi
  local out rc
  if [ -n "$ASAN_PRELOAD" ]; then
    out="$(LD_PRELOAD="$ASAN_PRELOAD" "$OUT/$name" 2>&1)"; rc=$?
  else
    out="$("$OUT/$name" 2>&1)"; rc=$?
  fi

  # ★★ 先查 sanitizer 报告，再看测试计数。
  #   ⚠ 顺序不能反: ASan 在报错后可能仍让进程跑完并打出"0 失败"，
  #     只看计数会把一个内存错误报成通过。
  if echo "$out" | grep -q "ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error:"; then
    echo "SANITIZER ERROR"
    FAILED_SUITES="$FAILED_SUITES $name(asan)"
    TOTAL_FAIL=$((TOTAL_FAIL + 1))
    echo "$out" | grep -A6 "ERROR: AddressSanitizer\|runtime error:" | head -14
    echo "$out" > "$OUT/$name.run.log"
    return
  fi

  local line
  line="$(echo "$out" | grep -oE '结果: [0-9]+ 通过, [0-9]+ 失败|PASS: [0-9]+  FAIL: [0-9]+' | tail -1)"

  # ⚠ 抓不到计数行 = 进程没跑到收尾（崩溃/信号中断），必须报失败。
  #   原先这里会算成 0 通过 0 失败，静默计入"全绿"。
  if [ -z "$line" ]; then
    echo "NO RESULT (退出码 $rc，见 $OUT/$name.run.log)"
    FAILED_SUITES="$FAILED_SUITES $name(crash)"
    TOTAL_FAIL=$((TOTAL_FAIL + 1))
    echo "$out" > "$OUT/$name.run.log"
    echo "$out" | tail -6
    return
  fi

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
