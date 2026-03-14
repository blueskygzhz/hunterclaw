/**
 * HunterClaw Channel Monitor Demo
 * 
 * 展示 Channel Monitor 内部实现机制:
 * 1. 连接状态监控 (Connected/Disconnected/Reconnecting)
 * 2. 心跳检测机制 (Heartbeat/Timeout)
 * 3. 自动重连策略 (Exponential Backoff)
 * 4. 事件通知系统 (State Change/Error/Health)
 * 5. 多通道并发监控 (Discord, Slack, Telegram)
 */

#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <random>
#include <iomanip>
#include <string>

using namespace std;

// ============================================================================
// 1. 连接状态定义
// ============================================================================

enum class ConnectionState {
    DISCONNECTED,    // 断开连接
    CONNECTING,      // 正在连接
    CONNECTED,       // 已连接
    RECONNECTING,    // 正在重连
    FAILED           // 连接失败
};

string state_to_string(ConnectionState state) {
    switch (state) {
        case ConnectionState::DISCONNECTED: return "DISCONNECTED";
        case ConnectionState::CONNECTING: return "CONNECTING";
        case ConnectionState::CONNECTED: return "CONNECTED";
        case ConnectionState::RECONNECTING: return "RECONNECTING";
        case ConnectionState::FAILED: return "FAILED";
        default: return "UNKNOWN";
    }
}

// ============================================================================
// 2. 健康状态定义
// ============================================================================

struct HealthMetrics {
    int heartbeat_sent = 0;           // 发送心跳次数
    int heartbeat_received = 0;       // 收到心跳响应次数
    int heartbeat_timeout = 0;        // 心跳超时次数
    int reconnect_attempts = 0;       // 重连尝试次数
    int messages_sent = 0;            // 发送消息数
    int messages_received = 0;        // 接收消息数
    int error_count = 0;              // 错误计数
    
    chrono::steady_clock::time_point last_heartbeat_time;
    chrono::steady_clock::time_point last_message_time;
    chrono::steady_clock::time_point connected_since;
    
    double get_heartbeat_success_rate() const {
        if (heartbeat_sent == 0) return 0.0;
        return (double)heartbeat_received / heartbeat_sent * 100.0;
    }
    
    int get_uptime_seconds() const {
        auto now = chrono::steady_clock::now();
        return chrono::duration_cast<chrono::seconds>(now - connected_since).count();
    }
};

// ============================================================================
// 3. 监控事件定义
// ============================================================================

enum class MonitorEvent {
    STATE_CHANGED,      // 状态改变
    HEARTBEAT_SENT,     // 心跳发送
    HEARTBEAT_RECEIVED, // 心跳响应
    HEARTBEAT_TIMEOUT,  // 心跳超时
    RECONNECTING,       // 开始重连
    RECONNECT_SUCCESS,  // 重连成功
    RECONNECT_FAILED,   // 重连失败
    ERROR_OCCURRED,     // 错误发生
    MESSAGE_RECEIVED    // 消息接收
};

struct MonitorEventData {
    MonitorEvent event;
    string channel_id;
    ConnectionState old_state;
    ConnectionState new_state;
    string message;
    chrono::steady_clock::time_point timestamp;
};

// ============================================================================
// 4. Channel 接口 (模拟真实 Channel)
// ============================================================================

class IChannel {
public:
    virtual ~IChannel() = default;
    virtual string get_id() const = 0;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool send_heartbeat() = 0;
    virtual bool is_connected() const = 0;
    virtual void simulate_network_issue() = 0;  // 模拟网络问题
};

// ============================================================================
// 5. Mock Channel 实现 (用于演示)
// ============================================================================

class MockChannel : public IChannel {
private:
    string id_;
    atomic<bool> connected_{false};
    atomic<bool> network_issue_{false};
    minstd_rand rng_;
    
public:
    explicit MockChannel(const string& id) 
        : id_(id), rng_(random_device{}()) {}
    
    string get_id() const override { return id_; }
    
    bool connect() override {
        this_thread::sleep_for(chrono::milliseconds(100)); // 模拟连接延迟
        
        if (network_issue_) {
            return false;  // 网络问题导致连接失败
        }
        
        // 90% 成功率
        bool success = (rng_() % 100) < 90;
        connected_ = success;
        return success;
    }
    
    void disconnect() override {
        connected_ = false;
    }
    
    bool send_heartbeat() override {
        if (!connected_ || network_issue_) {
            return false;
        }
        
        this_thread::sleep_for(chrono::milliseconds(50)); // 模拟网络延迟
        
        // 95% 成功率
        return (rng_() % 100) < 95;
    }
    
    bool is_connected() const override {
        return connected_ && !network_issue_;
    }
    
    void simulate_network_issue() override {
        network_issue_ = true;
        connected_ = false;
        cout << "[NETWORK] 🔥 模拟网络故障: " << id_ << endl;
    }
    
    void restore_network() {
        network_issue_ = false;
        cout << "[NETWORK] ✅ 恢复网络连接: " << id_ << endl;
    }
};

// ============================================================================
// 6. Channel Monitor 核心实现
// ============================================================================

class ChannelMonitor {
private:
    shared_ptr<IChannel> channel_;
    string channel_id_;
    
    // 状态
    atomic<ConnectionState> state_{ConnectionState::DISCONNECTED};
    atomic<bool> running_{false};
    
    // 配置
    int heartbeat_interval_ms_ = 5000;      // 心跳间隔 5秒
    int heartbeat_timeout_ms_ = 10000;      // 心跳超时 10秒
    int max_reconnect_attempts_ = 5;        // 最大重连次数
    int base_reconnect_delay_ms_ = 1000;    // 基础重连延迟 1秒
    
    // 健康指标
    HealthMetrics metrics_;
    
    // 线程管理
    unique_ptr<thread> monitor_thread_;
    mutex mutex_;
    condition_variable cv_;
    
    // 事件回调
    function<void(const MonitorEventData&)> event_callback_;
    
public:
    explicit ChannelMonitor(shared_ptr<IChannel> channel)
        : channel_(move(channel)), channel_id_(channel_->get_id()) {}
    
    ~ChannelMonitor() {
        stop();
    }
    
    // 设置事件回调
    void set_event_callback(function<void(const MonitorEventData&)> callback) {
        event_callback_ = move(callback);
    }
    
    // 启动监控
    void start() {
        if (running_.exchange(true)) {
            return;  // 已经在运行
        }
        
        monitor_thread_ = make_unique<thread>(&ChannelMonitor::monitor_loop, this);
        cout << "[MONITOR] 🚀 启动监控: " << channel_id_ << endl;
    }
    
    // 停止监控
    void stop() {
        if (!running_.exchange(false)) {
            return;  // 已经停止
        }
        
        cv_.notify_all();
        if (monitor_thread_ && monitor_thread_->joinable()) {
            monitor_thread_->join();
        }
        
        channel_->disconnect();
        cout << "[MONITOR] 🛑 停止监控: " << channel_id_ << endl;
    }
    
    // 获取当前状态
    ConnectionState get_state() const {
        return state_.load();
    }
    
    // 获取健康指标
    HealthMetrics get_metrics() const {
        lock_guard<mutex> lock(mutex_);
        return metrics_;
    }
    
    // 打印状态
    void print_status() const {
        auto state = get_state();
        auto metrics = get_metrics();
        
        cout << "\n╔══════════════════════════════════════════════════════════╗\n";
        cout << "║ Channel: " << channel_id_ << string(45 - channel_id_.length(), ' ') << "║\n";
        cout << "╠══════════════════════════════════════════════════════════╣\n";
        cout << "║ State: " << state_to_string(state) << string(48 - state_to_string(state).length(), ' ') << "║\n";
        
        if (state == ConnectionState::CONNECTED) {
            cout << "║ Uptime: " << metrics.get_uptime_seconds() << "s" 
                 << string(48 - to_string(metrics.get_uptime_seconds()).length(), ' ') << "║\n";
        }
        
        cout << "╠══════════════════════════════════════════════════════════╣\n";
        cout << "║ Heartbeat Metrics:                                       ║\n";
        cout << "║   Sent: " << metrics.heartbeat_sent 
             << string(49 - to_string(metrics.heartbeat_sent).length(), ' ') << "║\n";
        cout << "║   Received: " << metrics.heartbeat_received 
             << string(45 - to_string(metrics.heartbeat_received).length(), ' ') << "║\n";
        cout << "║   Timeout: " << metrics.heartbeat_timeout 
             << string(46 - to_string(metrics.heartbeat_timeout).length(), ' ') << "║\n";
        cout << "║   Success Rate: " 
             << fixed << setprecision(1) << metrics.get_heartbeat_success_rate() << "%" 
             << string(38 - to_string((int)metrics.get_heartbeat_success_rate()).length(), ' ') << "║\n";
        cout << "╠══════════════════════════════════════════════════════════╣\n";
        cout << "║ Messages: " << metrics.messages_received << " received" 
             << string(41 - to_string(metrics.messages_received).length(), ' ') << "║\n";
        cout << "║ Reconnects: " << metrics.reconnect_attempts 
             << string(44 - to_string(metrics.reconnect_attempts).length(), ' ') << "║\n";
        cout << "║ Errors: " << metrics.error_count 
             << string(47 - to_string(metrics.error_count).length(), ' ') << "║\n";
        cout << "╚══════════════════════════════════════════════════════════╝\n\n";
    }
    
private:
    // 核心监控循环
    void monitor_loop() {
        while (running_) {
            auto state = state_.load();
            
            switch (state) {
                case ConnectionState::DISCONNECTED:
                    handle_disconnected();
                    break;
                    
                case ConnectionState::CONNECTING:
                    handle_connecting();
                    break;
                    
                case ConnectionState::CONNECTED:
                    handle_connected();
                    break;
                    
                case ConnectionState::RECONNECTING:
                    handle_reconnecting();
                    break;
                    
                case ConnectionState::FAILED:
                    handle_failed();
                    break;
            }
            
            // 小延迟避免 CPU 占用过高
            this_thread::sleep_for(chrono::milliseconds(100));
        }
    }
    
    // 处理断开状态
    void handle_disconnected() {
        change_state(ConnectionState::CONNECTING);
    }
    
    // 处理连接中状态
    void handle_connecting() {
        cout << "[CONNECT] 🔌 正在连接: " << channel_id_ << "..." << endl;
        
        if (channel_->connect()) {
            {
                lock_guard<mutex> lock(mutex_);
                metrics_.connected_since = chrono::steady_clock::now();
                metrics_.reconnect_attempts = 0;
            }
            change_state(ConnectionState::CONNECTED);
            emit_event(MonitorEvent::RECONNECT_SUCCESS, "连接成功");
            cout << "[CONNECT] ✅ 连接成功: " << channel_id_ << endl;
        } else {
            change_state(ConnectionState::RECONNECTING);
            emit_event(MonitorEvent::RECONNECT_FAILED, "连接失败");
            cout << "[CONNECT] ❌ 连接失败: " << channel_id_ << endl;
        }
    }
    
    // 处理已连接状态 (核心心跳逻辑)
    void handle_connected() {
        // 发送心跳
        bool heartbeat_ok = send_heartbeat();
        
        if (!heartbeat_ok) {
            cout << "[HEARTBEAT] 💔 心跳超时: " << channel_id_ << endl;
            {
                lock_guard<mutex> lock(mutex_);
                metrics_.heartbeat_timeout++;
            }
            emit_event(MonitorEvent::HEARTBEAT_TIMEOUT, "心跳超时");
            
            // 连续超时 -> 进入重连
            if (metrics_.heartbeat_timeout >= 3) {
                change_state(ConnectionState::RECONNECTING);
                emit_event(MonitorEvent::RECONNECTING, "心跳超时,开始重连");
            }
        } else {
            // 心跳正常,重置超时计数
            lock_guard<mutex> lock(mutex_);
            metrics_.heartbeat_timeout = 0;
        }
        
        // 等待下一次心跳
        unique_lock<mutex> lock(mutex_);
        cv_.wait_for(lock, chrono::milliseconds(heartbeat_interval_ms_), 
                     [this] { return !running_; });
    }
    
    // 处理重连状态 (指数退避策略)
    void handle_reconnecting() {
        lock_guard<mutex> lock(mutex_);
        metrics_.reconnect_attempts++;
        
        if (metrics_.reconnect_attempts > max_reconnect_attempts_) {
            change_state(ConnectionState::FAILED);
            emit_event(MonitorEvent::ERROR_OCCURRED, "重连次数超限");
            cout << "[RECONNECT] ❌ 重连失败,超过最大尝试次数: " << channel_id_ << endl;
            return;
        }
        
        // 指数退避: 1s, 2s, 4s, 8s, 16s
        int delay_ms = base_reconnect_delay_ms_ * (1 << (metrics_.reconnect_attempts - 1));
        delay_ms = min(delay_ms, 30000);  // 最大 30 秒
        
        cout << "[RECONNECT] 🔄 尝试重连 #" << metrics_.reconnect_attempts 
             << " (延迟 " << delay_ms << "ms): " << channel_id_ << endl;
        
        emit_event(MonitorEvent::RECONNECTING, 
                   "重连尝试 #" + to_string(metrics_.reconnect_attempts));
        
        // 等待后重连
        unique_lock<mutex> ulock(mutex_);
        cv_.wait_for(ulock, chrono::milliseconds(delay_ms), 
                     [this] { return !running_; });
        
        if (running_) {
            change_state(ConnectionState::CONNECTING);
        }
    }
    
    // 处理失败状态
    void handle_failed() {
        cout << "[FAILED] 💀 通道失败,停止监控: " << channel_id_ << endl;
        running_ = false;
    }
    
    // 发送心跳
    bool send_heartbeat() {
        {
            lock_guard<mutex> lock(mutex_);
            metrics_.heartbeat_sent++;
            metrics_.last_heartbeat_time = chrono::steady_clock::now();
        }
        
        cout << "[HEARTBEAT] 💓 发送心跳: " << channel_id_ 
             << " (#" << metrics_.heartbeat_sent << ")" << endl;
        emit_event(MonitorEvent::HEARTBEAT_SENT, "发送心跳");
        
        bool success = channel_->send_heartbeat();
        
        if (success) {
            {
                lock_guard<mutex> lock(mutex_);
                metrics_.heartbeat_received++;
            }
            cout << "[HEARTBEAT] ✅ 心跳响应: " << channel_id_ 
                 << " (成功率: " << fixed << setprecision(1) 
                 << metrics_.get_heartbeat_success_rate() << "%)" << endl;
            emit_event(MonitorEvent::HEARTBEAT_RECEIVED, "心跳响应正常");
        }
        
        return success;
    }
    
    // 改变状态
    void change_state(ConnectionState new_state) {
        auto old_state = state_.exchange(new_state);
        if (old_state != new_state) {
            cout << "[STATE] 🔄 状态变化: " << channel_id_ 
                 << " (" << state_to_string(old_state) 
                 << " -> " << state_to_string(new_state) << ")" << endl;
            emit_event(MonitorEvent::STATE_CHANGED, 
                       state_to_string(old_state) + " -> " + state_to_string(new_state),
                       old_state, new_state);
        }
    }
    
    // 发射事件
    void emit_event(MonitorEvent event, const string& message,
                   ConnectionState old_state = ConnectionState::DISCONNECTED,
                   ConnectionState new_state = ConnectionState::DISCONNECTED) {
        if (event_callback_) {
            MonitorEventData data{
                event, channel_id_, old_state, new_state, 
                message, chrono::steady_clock::now()
            };
            event_callback_(data);
        }
    }
};

// ============================================================================
// 7. 多通道监控管理器
// ============================================================================

class MultiChannelMonitor {
private:
    map<string, unique_ptr<ChannelMonitor>> monitors_;
    mutex mutex_;
    
public:
    void add_channel(shared_ptr<IChannel> channel) {
        lock_guard<mutex> lock(mutex_);
        auto monitor = make_unique<ChannelMonitor>(channel);
        
        // 设置事件回调
        monitor->set_event_callback([this](const MonitorEventData& event) {
            handle_event(event);
        });
        
        string id = channel->get_id();
        monitors_[id] = move(monitor);
    }
    
    void start_all() {
        lock_guard<mutex> lock(mutex_);
        for (auto& [id, monitor] : monitors_) {
            monitor->start();
        }
    }
    
    void stop_all() {
        lock_guard<mutex> lock(mutex_);
        for (auto& [id, monitor] : monitors_) {
            monitor->stop();
        }
    }
    
    void print_all_status() const {
        lock_guard<mutex> lock(mutex_);
        cout << "\n";
        cout << "╔════════════════════════════════════════════════════════════╗\n";
        cout << "║         📊 Multi-Channel Monitor Status Report           ║\n";
        cout << "╚════════════════════════════════════════════════════════════╝\n";
        
        for (const auto& [id, monitor] : monitors_) {
            monitor->print_status();
        }
    }
    
private:
    void handle_event(const MonitorEventData& event) {
        // 可以在这里添加全局事件处理逻辑
        // 例如:告警、日志聚合、指标上报等
    }
};

// ============================================================================
// 8. Demo 主程序
// ============================================================================

int main() {
    cout << R"(
╔══════════════════════════════════════════════════════════════╗
║                                                              ║
║         🦅 HunterClaw Channel Monitor Demo 🦅                ║
║                                                              ║
║  展示内部实现机制:                                            ║
║  • 连接状态监控 (Connected/Disconnected/Reconnecting)        ║
║  • 心跳检测机制 (Heartbeat/Timeout)                          ║
║  • 自动重连策略 (Exponential Backoff)                        ║
║  • 事件通知系统 (State Change/Error/Health)                  ║
║  • 多通道并发监控 (Discord, Slack, Telegram)                 ║
║                                                              ║
╚══════════════════════════════════════════════════════════════╝
)" << endl;

    // 创建多通道监控器
    MultiChannelMonitor multi_monitor;
    
    // 添加 3 个模拟通道
    auto discord = make_shared<MockChannel>("Discord");
    auto slack = make_shared<MockChannel>("Slack");
    auto telegram = make_shared<MockChannel>("Telegram");
    
    multi_monitor.add_channel(discord);
    multi_monitor.add_channel(slack);
    multi_monitor.add_channel(telegram);
    
    // 启动所有监控
    cout << "\n[DEMO] 🚀 启动所有通道监控...\n" << endl;
    multi_monitor.start_all();
    
    // 场景 1: 正常运行 15 秒
    cout << "\n[DEMO] ⏱️  场景 1: 正常运行 15 秒...\n" << endl;
    this_thread::sleep_for(chrono::seconds(15));
    multi_monitor.print_all_status();
    
    // 场景 2: 模拟 Slack 网络故障
    cout << "\n[DEMO] 🔥 场景 2: 模拟 Slack 网络故障...\n" << endl;
    slack->simulate_network_issue();
    this_thread::sleep_for(chrono::seconds(20));
    multi_monitor.print_all_status();
    
    // 场景 3: 恢复 Slack 网络
    cout << "\n[DEMO] ✅ 场景 3: 恢复 Slack 网络...\n" << endl;
    dynamic_cast<MockChannel*>(slack.get())->restore_network();
    this_thread::sleep_for(chrono::seconds(15));
    multi_monitor.print_all_status();
    
    // 停止所有监控
    cout << "\n[DEMO] 🛑 停止所有监控...\n" << endl;
    multi_monitor.stop_all();
    
    cout << R"(
╔══════════════════════════════════════════════════════════════╗
║                                                              ║
║              ✅ Channel Monitor Demo 完成! ✅                 ║
║                                                              ║
║  核心机制演示:                                                ║
║  ✓ 连接状态自动机 (5 种状态切换)                             ║
║  ✓ 心跳检测 (5秒间隔,3次超时触发重连)                        ║
║  ✓ 指数退避重连 (1s, 2s, 4s, 8s, 16s)                       ║
║  ✓ 健康指标统计 (心跳成功率,运行时长,错误计数)                ║
║  ✓ 多通道并发监控 (独立线程,互不干扰)                        ║
║                                                              ║
╚══════════════════════════════════════════════════════════════╝
)" << endl;

    return 0;
}
