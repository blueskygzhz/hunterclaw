    // =======================================================================
    // manager.turn-timeout.ts → acp_turn_timeout
    // C++ 与真源结构不同（同步回调 vs AsyncIterable），这些边界必须实测。
    // =======================================================================
    {
        std::cout << "\n[turn-timeout] 对齐 OC manager.turn-timeout.ts\n";

        CHECK(acp::is_cancellation_stop_reason(std::string("cancel")), "stopReason=cancel");
        CHECK(acp::is_cancellation_stop_reason(std::string("cancelled")), "stopReason=cancelled");
        CHECK(acp::is_cancellation_stop_reason(std::string("manual-cancel")),
              "stopReason=manual-cancel");
        CHECK(!acp::is_cancellation_stop_reason(std::string("end_turn")), "普通结束不算取消");
        CHECK(!acp::is_cancellation_stop_reason(std::nullopt), "缺失 stopReason 不算取消");

        {
            acp::AcpRuntimeEvent done;
            done.type = acp::AcpRuntimeEvent::Type::Done;
            CHECK(acp::resolve_acp_turn_terminal_status(done) == "completed",
                  "无 status 无 stopReason → completed");

            done.stop_reason = std::string("cancelled");
            CHECK(acp::resolve_acp_turn_terminal_status(done) == "cancelled",
                  "旧适配器只给 stopReason 也能判出 cancelled");

            done.status = std::string("completed");
            CHECK(acp::resolve_acp_turn_terminal_status(done) == "completed",
                  "显式 status 优先于 stopReason");

            // HunterClaw 特有风险：status 字段在 ToolCall 事件里复用为工具状态
            done.status = std::string("in_progress");
            done.stop_reason = std::string("cancelled");
            CHECK(acp::resolve_acp_turn_terminal_status(done) == "cancelled",
                  "**关键**：非法 status 被忽略并回落 stopReason（防工具状态污染终态）");
        }

        {
            acp::AcpSessionRuntimeOptions opts;
            CHECK(acp::resolve_turn_timeout_ms(opts) ==
                      acp::ACP_DEFAULT_AGENT_TIMEOUT_SECONDS * 1000,
                  "未设置 timeoutSeconds → agent 默认值");
            opts.timeout_seconds = 30;
            CHECK(acp::resolve_turn_timeout_ms(opts) == 30000, "显式 timeoutSeconds 生效");
            opts.timeout_seconds = 0;
            CHECK(acp::resolve_turn_timeout_ms(opts) ==
                      acp::ACP_DEFAULT_AGENT_TIMEOUT_SECONDS * 1000,
                  "timeoutSeconds=0 视为未设置（对齐 OC 的 > 0 判据）");
        }

        {
            const auto err = acp::make_turn_timeout_error(30000);
            CHECK(std::string(err.what()) == "ACP turn timed out after 30s.",
                  "超时文案与真源逐字一致");
            CHECK(err.detail_code.has_value() &&
                      *err.detail_code == acp::ACP_TURN_TIMEOUT_DETAIL_CODE,
                  "**关键**：带 TURN_TIMEOUT detailCode（background task 靠它判 timed_out）");
            const auto sub = acp::make_turn_timeout_error(400);
            CHECK(std::string(sub.what()) == "ACP turn timed out after 1s.",
                  "不足 1s 也显示 1s（对齐 OC Math.max(1, ...)）");
        }

        {
            auto ok = acp::await_turn_with_timeout("s1", 5000, 5000, []() {
                acp::AcpTurnStreamOutcome o;
                o.saw_output = true;
                o.terminal_status = std::string("completed");
                return o;
            });
            CHECK(ok.kind == acp::AwaitTurnResult::Kind::Completed, "正常完成返回 Completed");
            CHECK(ok.outcome.saw_output, "sawOutput 透传");

            auto failed = acp::await_turn_with_timeout("s2", 5000, 5000,
                []() -> acp::AcpTurnStreamOutcome {
                    throw acp::AcpRuntimeError(acp::AcpErrorCode::ACP_TURN_FAILED, "boom");
                });
            CHECK(failed.kind == acp::AwaitTurnResult::Kind::Failed, "抛错返回 Failed");
            CHECK(failed.error.has_value() && std::string(failed.error->what()) == "boom",
                  "原始错误被保留");

            const auto t0 = std::chrono::steady_clock::now();
            auto timed = acp::await_turn_with_timeout("s3", 150, 150, []() {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                acp::AcpTurnStreamOutcome o;
                o.terminal_status = std::string("completed");
                return o;
            });
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - t0).count();
            CHECK(timed.kind == acp::AwaitTurnResult::Kind::TimedOut, "超时返回 TimedOut");
            CHECK(elapsed < 500,
                  "**关键**：超时后立即返回，不等 turn 体跑完（否则超时形同虚设）");
            std::this_thread::sleep_for(std::chrono::milliseconds(700));

            auto unbounded = acp::await_turn_with_timeout("s4", 0, 0, []() {
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                acp::AcpTurnStreamOutcome o;
                o.terminal_status = std::string("completed");
                return o;
            });
            CHECK(unbounded.kind == acp::AwaitTurnResult::Kind::Completed,
                  "timeoutMs<=0 退化为无限等待（对齐 OC）");
        }

        {
            auto gate = std::make_shared<acp::AcpTurnEventGate>();
            CHECK(gate->open.load(), "闸门默认开启");
            gate->open.store(false);
            CHECK(!gate->open.load(), "闸门可关闭（超时后抑制迟到事件）");
        }
    }

    // =======================================================================
    // manager.backend-failover.ts → acp_backend_failover
    // =======================================================================
    {
        std::cout << "\n[backend-failover] 对齐 OC manager.backend-failover.ts\n";

        {
            // ⚠ 首项即使为空串也必须保留：空串＝让 registry 自选默认 backend。
            //   若按常规习惯过滤掉，未配 backend 的会话会一个候选都不剩。
            auto plan = acp::resolve_backend_candidate_plan("", "", {});
            CHECK(plan.candidate_backends.size() == 1 && plan.candidate_backends[0].empty(),
                  "**关键**：全空时保留一个空串候选（＝registry 自选默认）");
            CHECK(plan.describe_backend_candidate("") == "<auto>",
                  "空 backend 的展示名为 <auto>");

            plan = acp::resolve_backend_candidate_plan("acpx", "resolved-x", {"a", "b"});
            CHECK(plan.candidate_backends.size() == 3, "configured + 2 fallbacks");
            CHECK(plan.candidate_backends[0] == "acpx", "configured 优先于 resolved");

            plan = acp::resolve_backend_candidate_plan("", "resolved-x", {"a"});
            CHECK(plan.candidate_backends[0] == "resolved-x",
                  "configured 缺省时回落 resolved");

            plan = acp::resolve_backend_candidate_plan("acpx", "", {"acpx", "b", "b", "  "});
            CHECK(plan.candidate_backends.size() == 2,
                  "去重且过滤空白 fallback（acpx 重复、b 重复、空串被丢）");
            CHECK(plan.candidate_backends[1] == "b", "保序");

            plan = acp::resolve_backend_candidate_plan("  acpx  ", "", {});
            CHECK(plan.candidate_backends[0] == "acpx", "configured 先 trim");
        }

        {
            acp::BackendAttempt attempt;
            attempt.code = acp::AcpErrorCode::ACP_TURN_FAILED;
            attempt.error = "backend unavailable";
            attempt.saw_output = false;
            CHECK(acp::is_failover_worthy_backend_error(attempt), "unavailable 属瞬时故障");

            attempt.saw_output = true;
            CHECK(!acp::is_failover_worthy_backend_error(attempt),
                  "**关键**：已产出可见输出禁止切换（否则用户看到同一问题被答两遍）");

            attempt.saw_output = false;
            for (const char* probe : {"rate limited", "rate-limit", "ratelimiting",
                                      "quota exceeded", "tokens exhausted",
                                      "temporarily down", "temporary failure",
                                      "server overloaded"}) {
                attempt.error = probe;
                CHECK(acp::is_failover_worthy_backend_error(attempt),
                      std::string("瞬时故障词表命中: ") + probe);
            }

            attempt.error = "syntax error in request";
            CHECK(!acp::is_failover_worthy_backend_error(attempt),
                  "非瞬时错误不切换（换 backend 也一样失败）");

            attempt.error = "backend unavailable";
            attempt.code = acp::AcpErrorCode::ACP_SESSION_INIT_FAILED;
            CHECK(acp::is_failover_worthy_backend_error(attempt), "INIT_FAILED 可切换");
            attempt.code = acp::AcpErrorCode::ACP_BACKEND_UNAVAILABLE;
            CHECK(acp::is_failover_worthy_backend_error(attempt), "BACKEND_UNAVAILABLE 可切换");
            attempt.code = acp::AcpErrorCode::ACP_NOT_ENABLED;
            CHECK(!acp::is_failover_worthy_backend_error(attempt),
                  "其他错误码不切换（配置问题换 backend 无用）");
        }

        {
            // ⚠ size_t 无符号：真源 `idx < length - 1` 直译在空数组时会下溢
            CHECK(!acp::should_attempt_backend_failover(0, {}),
                  "**关键**：空候选不下溢（真源 length-1 直译会得天文数字）");
            CHECK(!acp::should_attempt_backend_failover(0, {"a"}), "单候选无处可切");
            CHECK(acp::should_attempt_backend_failover(0, {"a", "b"}), "首个候选后还有备选");
            CHECK(!acp::should_attempt_backend_failover(1, {"a", "b"}), "末个候选已无备选");
        }
    }

    // =======================================================================
    // active-turns.ts + manager.runtime-resume-state.ts → acp_active_turns
    // =======================================================================
    {
        std::cout << "\n[active-turns/resume] 对齐 OC active-turns.ts + runtime-resume-state.ts\n";

        {
            const std::string key = "agent:x:acp:AT-1";
            CHECK(!acp::is_acp_turn_active(key), "初始未活跃");
            acp::mark_acp_turn_active(key);
            CHECK(acp::is_acp_turn_active(key), "标记后活跃");
            // 归一化：actor key 用 lowercase
            CHECK(acp::is_acp_turn_active("AGENT:X:ACP:at-1"),
                  "大小写不敏感（与 actor queue 同一归一化规则）");
            acp::clear_acp_turn_active(key);
            CHECK(!acp::is_acp_turn_active(key), "清除后不活跃");
            CHECK(!acp::is_acp_turn_active(""), "空 key 恒不活跃");
        }

        {
            CHECK(acp::is_recoverable_manager_acpx_exit_error("acpx exited with code 1"),
                  "acpx code 退出可恢复");
            CHECK(acp::is_recoverable_manager_acpx_exit_error("  acpx exited with signal sigkill"),
                  "acpx signal 退出可恢复（先 trim）");
            CHECK(!acp::is_recoverable_manager_acpx_exit_error("acpx exited"),
                  "缺 code/signal 不算可恢复");
            CHECK(!acp::is_recoverable_manager_acpx_exit_error("some other failure"),
                  "无关错误不可恢复");
        }

        {
            // ⚠ 真源按 detailCode 判定而非 message 文本：Claude 报 "Resource not found"、
            //   Kiro 报 "Internal error"(-32603)，文本匹配漏掉 Kiro 会让线程永久卡死。
            acp::AcpRuntimeError resume_err(
                acp::AcpErrorCode::ACP_TURN_FAILED, "Internal error",
                std::string(acp::ACP_SESSION_RESUME_REQUIRED_DETAIL_CODE));
            CHECK(acp::is_recoverable_missing_persistent_session_error(resume_err),
                  "**关键**：按 detailCode 判定（文本可能是 Internal error）");

            acp::AcpRuntimeError plain_err(acp::AcpErrorCode::ACP_TURN_FAILED,
                                           "Resource not found");
            CHECK(!acp::is_recoverable_missing_persistent_session_error(plain_err),
                  "无 detailCode 不判为 resume 失效（不靠文本猜）");
        }

        {
            acp::AcpRuntimeError exit_err(acp::AcpErrorCode::ACP_TURN_FAILED,
                                          "acpx exited with code 1");
            auto d = acp::prepare_fresh_runtime_handle_retry(0, exit_err, false, true, true);
            CHECK(d.should_retry && !d.needs_clear_resume_state,
                  "acpx 早退：重试但无需清 resume 状态");

            d = acp::prepare_fresh_runtime_handle_retry(1, exit_err, false, true, true);
            CHECK(!d.should_retry,
                  "**关键**：attempt>0 不再重试（一次性，防无限循环）");

            d = acp::prepare_fresh_runtime_handle_retry(0, exit_err, true, true, true);
            CHECK(!d.should_retry,
                  "**关键**：已有可见输出不重试（否则重复输出）");

            acp::AcpRuntimeError resume_err(
                acp::AcpErrorCode::ACP_TURN_FAILED, "Internal error",
                std::string(acp::ACP_SESSION_RESUME_REQUIRED_DETAIL_CODE));
            d = acp::prepare_fresh_runtime_handle_retry(0, resume_err, false, true, true);
            CHECK(d.should_retry && d.needs_clear_resume_state,
                  "resume 失效：重试且需清 identity");

            d = acp::prepare_fresh_runtime_handle_retry(0, resume_err, false, true, false);
            CHECK(!d.should_retry, "非 persistent 模式不走 resume 重建");

            d = acp::prepare_fresh_runtime_handle_retry(0, resume_err, false, false, true);
            CHECK(!d.should_retry, "无 runtime 时不重试");

            acp::AcpRuntimeError other(acp::AcpErrorCode::ACP_TURN_FAILED, "random failure");
            d = acp::prepare_fresh_runtime_handle_retry(0, other, false, true, true);
            CHECK(!d.should_retry, "无关错误不触发 fresh-handle 重试");
        }
    }

