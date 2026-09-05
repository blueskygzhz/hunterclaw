    // =======================================================================
    // manager.turn-timeout.ts → acp_turn_timeout
    // C++ 与真源结构不同（同步回调 vs AsyncIterable），因此这些边界必须实测，
    // 不能靠"照着翻译"来保证。
    // =======================================================================
    {
        std::cout << "\n[turn-timeout] 对齐 OC manager.turn-timeout.ts\n";

        // ── isCancellationStopReason ─────────────────────────────────────
        CHECK(acp::is_cancellation_stop_reason(std::string("cancel")), "stopReason=cancel");
        CHECK(acp::is_cancellation_stop_reason(std::string("cancelled")), "stopReason=cancelled");
        CHECK(acp::is_cancellation_stop_reason(std::string("manual-cancel")),
              "stopReason=manual-cancel");
        CHECK(!acp::is_cancellation_stop_reason(std::string("end_turn")),
              "普通结束不算取消");
        CHECK(!acp::is_cancellation_stop_reason(std::nullopt), "缺失 stopReason 不算取消");

        // ── resolveAcpTurnTerminalStatus ─────────────────────────────────
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

            // ⚠ HunterClaw 特有风险：status 字段在 ToolCall 事件里复用为工具状态
            done.status = std::string("in_progress");
            done.stop_reason = std::string("cancelled");
            CHECK(acp::resolve_acp_turn_terminal_status(done) == "cancelled",
                  "**关键**：非法 status 值被忽略，回落 stopReason（防工具状态污染终态）");
        }

        // ── resolveTurnTimeoutMs ─────────────────────────────────────────
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

            opts.timeout_seconds = -5;
            CHECK(acp::resolve_turn_timeout_ms(opts) ==
                      acp::ACP_DEFAULT_AGENT_TIMEOUT_SECONDS * 1000,
                  "负值回落默认值");
        }

        // ── makeTurnTimeoutError ─────────────────────────────────────────
        {
            const auto err = acp::make_turn_timeout_error(30000);
            CHECK(std::string(err.what()) == "ACP turn timed out after 30s.",
                  "超时文案与真源逐字一致");
            CHECK(err.detail_code.has_value() &&
                      *err.detail_code == acp::ACP_TURN_TIMEOUT_DETAIL_CODE,
                  "**关键**：带 TURN_TIMEOUT detailCode（background task 靠它判 timed_out）");
            CHECK(err.code == acp::AcpErrorCode::ACP_TURN_FAILED, "错误码为 ACP_TURN_FAILED");

            const auto sub_second = acp::make_turn_timeout_error(400);
            CHECK(std::string(sub_second.what()) == "ACP turn timed out after 1s.",
                  "不足 1s 也显示 1s（对齐 OC Math.max(1, ...)）");
        }

        // ── awaitTurnWithTimeout ─────────────────────────────────────────
        {
            // 正常完成
            auto ok = acp::await_turn_with_timeout("s1", 5000, 5000, []() {
                acp::AcpTurnStreamOutcome o;
                o.saw_output = true;
                o.terminal_status = std::string("completed");
                return o;
            });
            CHECK(ok.kind == acp::AwaitTurnResult::Kind::Completed, "正常完成返回 Completed");
            CHECK(ok.outcome.saw_output, "sawOutput 透传");
            CHECK(ok.outcome.terminal_status.has_value() &&
                      *ok.outcome.terminal_status == "completed",
                  "terminalStatus 透传");

            // 抛错
            auto failed = acp::await_turn_with_timeout("s2", 5000, 5000,
                []() -> acp::AcpTurnStreamOutcome {
                    throw acp::AcpRuntimeError(acp::AcpErrorCode::ACP_TURN_FAILED, "boom");
                });
            CHECK(failed.kind == acp::AwaitTurnResult::Kind::Failed, "抛错返回 Failed");
            CHECK(failed.error.has_value() && std::string(failed.error->what()) == "boom",
                  "原始错误被保留");

            // 超时：turn 体睡 600ms，超时设 150ms
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
            // 让迟到线程跑完，避免污染后续用例
            std::this_thread::sleep_for(std::chrono::milliseconds(700));

            // timeout_ms <= 0 → 无限等待
            auto unbounded = acp::await_turn_with_timeout("s4", 0, 0, []() {
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
                acp::AcpTurnStreamOutcome o;
                o.terminal_status = std::string("completed");
                return o;
            });
            CHECK(unbounded.kind == acp::AwaitTurnResult::Kind::Completed,
                  "timeoutMs<=0 退化为无限等待（对齐 OC）");
        }

        // ── eventGate ────────────────────────────────────────────────────
        {
            auto gate = std::make_shared<acp::AcpTurnEventGate>();
            CHECK(gate->open.load(), "闸门默认开启");
            gate->open.store(false);
            CHECK(!gate->open.load(), "闸门可关闭（超时后抑制迟到事件）");
        }
    }

