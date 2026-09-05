    // =======================================================================
    // manager.background-task.ts → acp_background_task
    // ACP 子会话不进 SubagentRegistry，父侧只能靠 detached task 镜像感知结果，
    // 因此这些边界值/推断规则必须与真源逐条一致。
    // =======================================================================
    {
        std::cout << "\n[background-task] 对齐 OC manager.background-task.ts\n";

        // ── summarizeBackgroundTaskText ──────────────────────────────────
        CHECK(acp::summarize_background_task_text("  ") == "ACP background task",
              "空白文本回落固定占位串（对齐 OC ?? \"ACP background task\"）");
        CHECK(acp::summarize_background_task_text("  refactor foo  ") == "refactor foo",
              "normalizeText 先 trim");
        {
            const std::string long_text(300, 'x');
            const std::string out = acp::summarize_background_task_text(long_text);
            // OC: truncateUtf16Safe(s, MAX-1) + "…"，故字节数 = 159 + 3(U+2026)
            CHECK(out.size() == acp::ACP_BACKGROUND_TASK_TEXT_MAX_LENGTH - 1 + 3,
                  "超长按 MAX-1 截断再补省略号");
            CHECK(out.substr(out.size() - 3) == "\xE2\x80\xA6",
                  "省略号是 U+2026 而非三个点");
        }
        {
            // ⚠ 关键：截断点落在多字节字符中间时不得产生非法 UTF-8
            const std::string cjk_text = std::string(158, 'a') + "中文尾巴";
            const std::string out = acp::summarize_background_task_text(cjk_text);
            CHECK(out.size() <= acp::ACP_BACKGROUND_TASK_TEXT_MAX_LENGTH - 1 + 3,
                  "CJK 截断不超过上限");
            bool valid = true;
            for (size_t i = 0; i < out.size();) {
                const unsigned char c = static_cast<unsigned char>(out[i]);
                size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3
                           : (c >> 3) == 0x1E ? 4 : 0;
                if (len == 0 || i + len > out.size()) { valid = false; break; }
                for (size_t k = 1; k < len; ++k) {
                    if ((static_cast<unsigned char>(out[i + k]) & 0xC0) != 0x80) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) break;
                i += len;
            }
            CHECK(valid, "**关键**：截断不切断 UTF-8 多字节序列（否则日志/DB 出现乱码）");
        }

        // ── appendBackgroundTaskProgressSummary ──────────────────────────
        CHECK(acp::append_background_task_progress_summary("abc", "   ") == "abc",
              "纯空白 chunk 不改变已有摘要");
        CHECK(acp::append_background_task_progress_summary("", "  hello  ") == "hello",
              "首段 trimStart 且折叠尾部空白");
        CHECK(acp::append_background_task_progress_summary("hello", " world") == "hello world",
              "续接段保留前导空格以保住词边界");
        CHECK(acp::append_background_task_progress_summary("a", "b\n\nc") == "a b c",
              "换行折叠为单空格（保持单行摘要）");
        {
            const std::string base(238, 'y');
            const std::string out = acp::append_background_task_progress_summary(base, "zzzzz");
            CHECK(out.size() == acp::ACP_BACKGROUND_TASK_PROGRESS_MAX_LENGTH - 1 + 3,
                  "进度摘要按 240-1 截断再补省略号");
        }

        // ── resolveBackgroundTaskFailureStatus ───────────────────────────
        {
            acp::AcpRuntimeError timeout_err(
                acp::AcpErrorCode::ACP_TURN_FAILED, "turn timed out",
                std::string(acp::ACP_TURN_TIMEOUT_DETAIL_CODE));
            CHECK(acp::resolve_background_task_failure_status(timeout_err) ==
                      tasks::TaskStatus::TIMED_OUT,
                  "detailCode==TURN_TIMEOUT → timed_out");

            acp::AcpRuntimeError other_err(acp::AcpErrorCode::ACP_TURN_FAILED, "boom");
            CHECK(acp::resolve_background_task_failure_status(other_err) ==
                      tasks::TaskStatus::FAILED,
                  "无 detailCode → failed（而非 timed_out）");

            acp::AcpRuntimeError resume_err(
                acp::AcpErrorCode::ACP_SESSION_INIT_FAILED, "need resume",
                std::string("SESSION_RESUME_REQUIRED"));
            CHECK(acp::resolve_background_task_failure_status(resume_err) ==
                      tasks::TaskStatus::FAILED,
                  "**关键**：其他 detailCode 不得误判成超时");
        }

        // ── resolveBackgroundTaskTerminalResult ──────────────────────────
        {
            auto r = acp::resolve_background_task_terminal_result("");
            CHECK(r.terminal_outcome.has_value() &&
                      *r.terminal_outcome == tasks::TaskTerminalOutcome::BLOCKED,
                  "空进度摘要 → blocked（没产出可交付物）");

            r = acp::resolve_background_task_terminal_result("I'll check the logs");
            CHECK(r.terminal_outcome.has_value() &&
                      *r.terminal_outcome == tasks::TaskTerminalOutcome::BLOCKED,
                  "progress-only 文本 → blocked（只报意图不算完成）");

            r = acp::resolve_background_task_terminal_result(
                "Refactored 7 call sites and added 3 unit tests.");
            CHECK(!r.terminal_outcome.has_value(), "真实交付物描述 → 不标 blocked");

            r = acp::resolve_background_task_terminal_result(
                "write failed: permission denied for /etc/hosts.");
            CHECK(r.terminal_outcome.has_value() &&
                      *r.terminal_outcome == tasks::TaskTerminalOutcome::BLOCKED,
                  "permission denied → blocked");
            CHECK(r.terminal_summary.has_value() &&
                      *r.terminal_summary == "Permission denied for /etc/hosts.",
                  "**关键**：提取路径且剥掉尾部标点（便于父侧直接展示）");

            r = acp::resolve_background_task_terminal_result("Permission denied");
            CHECK(r.terminal_summary.has_value() && *r.terminal_summary == "Permission denied.",
                  "无路径时用通用文案");

            const char* writable_probes[] = {"I need a writable session to continue",
                                             "filesystem authorization missing",
                                             "please use `apply_patch` instead"};
            for (const char* probe : writable_probes) {
                r = acp::resolve_background_task_terminal_result(probe);
                CHECK(r.terminal_outcome.has_value() &&
                          *r.terminal_outcome == tasks::TaskTerminalOutcome::BLOCKED,
                      std::string("writable-session 判据命中: ") + probe);
                CHECK(r.terminal_summary.has_value() &&
                          *r.terminal_summary ==
                              "Writable session or apply_patch authorization required.",
                      "统一 writable-session 文案");
            }
        }

        // ── task-completion-contract（真源依赖） ─────────────────────────
        {
            CHECK(tasks::is_progress_only_completion_text("Let me run the tests"),
                  "\"Let me run\" 属 progress-only");
            CHECK(tasks::is_progress_only_completion_text("Checking the config"),
                  "裸动名词开头（Checking）属 progress-only");
            CHECK(tasks::is_progress_only_completion_text("Next, I'll review the diff"),
                  "剥掉 follow-up 前缀后仍是 progress-only");
            CHECK(!tasks::is_progress_only_completion_text(
                      "I'll check the logs. Found the root cause in parser.cc line 42."),
                  "**关键**：后接实质结论的句子不算 progress-only（否则成功也被判 blocked）");
            CHECK(!tasks::is_progress_only_completion_text("Fixed the parser bug"),
                  "完成态描述不属 progress-only");
            CHECK(tasks::normalize_completion_text("  a \n\t b  ") == "a b", "空白折叠 + trim");
            CHECK(tasks::truncate_utf8_safe("中文", 4) == "中",
                  "**关键**：UTF-8 安全截断退回字符边界");
        }
    }

