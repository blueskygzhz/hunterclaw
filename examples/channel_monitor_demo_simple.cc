/**
 * HunterClaw Channel Monitor Demo (简化版 - C++11 兼容)
 * 
 * 展示 Channel Monitor 内部实现机制:
 * 1. 连接状态监控 (Connected/Disconnected/Reconnecting)
 * 2. 心跳检测机制 (Heartbeat/Timeout)
 * 3. 自动重连策略 (Exponential Backoff)
 * 4. 事件通知系统 (State Change/Error/Health)
 */

#include <iostream>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>
#include <ctime>
#include <cstdlib>
#include <iomanip>

using namespace std;

// ============================================================================
// 1. 连接状态定义
// ============================================================================

enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    RECONNECTING,
    FAILED
};

const char* state_to_string(ConnectionState state) {
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
    int heartbeat_sent;
    int heartbeat_received;
    int heartbeat_timeout;
    int reconnect_attempts;
    int messages_received;
    int error_count;
    
    chrono::steady_clock::time_point connected_since;
    
    HealthMetrics() 
        : heartbeat_sent(0), heartbeat_received(0), heartbeat_timeout(0),
          reconnect_attempts(0), messages_received(0), error_count(0) {}
    
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
// 3. Mock Channel (模拟通道)
// ============================================================================

class MockChannel {
private:
    string id_;
    atomic<bool> connected_;
    atomic<bool> network_issue_;
    
public:
    explicit MockChannel(const string& id) 
        : id_(id), connected_(false), network_issue_(false) {
        srand(time(NULL) + rand());
    }
    
    string get_id() const { return id_; }
    
    bool connect() {
        this_thread::sleep_for(chrono::milliseconds(100));
        
        if (network_issue_) {
            return false;
        }
        
        bool success = (rand() % 100) < 90;
        connected_ = success;
        return success;
    }
    
    void disconnect() {
        connected_ = false;
    }
    
    bool send_heartbeat() {
        if (!connected_ || network_issue_) {
            return false;
        }
        
        this_thread::sleep_for(chrono::milliseconds(50));
        return (rand() % 100) < 95;
    }
    
    bool is_connected() const {
        return connected_ && !network_issue_;
    }
    
    void simulate_network_issue() {
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
// 4. Channel Monitor 核心实现
// ============================================================================

class ChannelMonitor {
private:
    shared_ptr<MockChannel> channel_;
    string channel_id_;
    
    atomic<ConnectionState> state_;
    atomic<bool> running_;
    
    int heartbeat_interval_ms_;
    int max_reconnect_attempts_;
    int base_reconnect_delay_ms_;
    
    HealthMetrics metrics_;
    mutable mutex metrics_mutex_;
    
    shared_ptr<thread> monitor_thread_;
    mutex cv_mutex_;
    condition_variable cv_;
    
public:
    explicit ChannelMonitor(shared_ptr<MockChannel> channel)
        : channel_(channel), 
          channel_id_(channel->get_id()),
          state_(ConnectionState::DISCONNECTED),
          running_(false),
          heartbeat_interval_ms_(5000),
          max_reconnect_attempts_(5),
          base_reconnect_delay_ms_(1000) {}
    
    ~ChannelMonitor() {
        stop();
    }
    
    void start() {
        if (running_.exchange(true)) {
            return;
        }
        
        monitor_thread_ = shared_ptr<thread>(
            new thread(&ChannelMonitor::monitor_loop, this)
        );
        cout << "[MONITOR] 🚀 启动监控: " << channel_id_ << endl;
    }
    
    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        
        cv_.notify_all();
        if (monitor_thread_ && monitor_thread_->joinable()) {
            monitor_thread_->join();
        }
        
        channel_->disconnect();
        cout << "[MONITOR] 🛑 停止监控: " << channel_id_ << endl;
    }
    
    ConnectionState get_state() const {
        return state_.load();
    }
    
    HealthMetrics get_metrics() const {
        lock_guard<mutex> lock(metrics_mutex_);
        return metrics_;
    }
    
    void print_status() const {
        auto state = get_state();
        auto metrics = get_metrics();
        
        cout << "\n╔══════════════════════════════════════════════════════════╗\n";
        cout << "║ Channel: " << channel_id_ << string(45 - channel_id_.length(), ' ') << "║\n";
        cout << "╠══════════════════════════════════════════════════════════╣\n";
        cout << "║ State: " << state_to_string(state) 
             << string(48 - strlen(state_to_string(state)), ' ') << "║\n";
        
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
             << string(39, ' ') << "║\n";
        cout << "╠══════════════════════════════════════════════════════════╣\n";
        cout << "║ Reconnects: " << metrics.reconnect_attempts 
             << string(44 - to_string(metrics.reconnect_attempts).length(), ' ') << "║\n";
        cout << "║ Errors: " << metrics.error_count 
             << string(47 - to_string(metrics.error_count).length(), ' ') << "║\n";
        cout << "╚══════════════════════════════════════════════════════════╝\n";
    }
    
private:
    void monitor_loop() {
        while (running_) {
            auto state = state_.load();
            
            switch (state) {
                case ConnectionState::DISCONNECTED:
                    change_state(ConnectionState::CONNECTING);
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
                    running_ = false;
                    break;
            }
            
            this_thread::sleep_for(chrono::milliseconds(100));
        }
    }
    
    void handle_connecting() {
        cout << "[CONNECT] 🔌 正在连接: " << channel_id_ << "..." << endl;
        
        if (channel_->connect()) {
            {
                lock_guard<mutex> lock(metrics_mutex_);
                metrics_.connected_since = chrono::steady_clock::now();
                metrics_.reconnect_attempts = 0;
            }
            change_state(ConnectionState::CONNECTED);
            cout << "[CONNECT] ✅ 连接成功: " << channel_id_ << endl;
        } else {
            change_state(ConnectionState::RECONNECTING);
            cout << "[CONNECT] ❌ 连接失败: " << channel_id_ << endl;
        }
    }
    
    void handle_connected() {
        bool heartbeat_ok = send_heartbeat();
        
        if (!heartbeat_ok) {
            cout << "[HEARTBEAT] 💔 心跳超时: " << channel_id_ << endl;
            {
                lock_guard<mutex> lock(metrics_mutex_);
                metrics_.heartbeat_timeout++;
            }
            
            if (metrics_.heartbeat_timeout >= 3) {
                change_state(ConnectionState::RECONNECTING);
            }
        } else {
            lock_guard<mutex> lock(metrics_mutex_);
            metrics_.heartbeat_timeout = 0;
        }
        
        unique_lock<mutex> lock(cv_mutex_);
        cv_.wait_for(lock, chrono::milliseconds(heartbeat_interval_ms_), 
                     [this] { return !running_.load(); });
    }
    
    void handle_reconnecting() {
        {
            lock_guard<mutex> lock(metrics_mutex_);
            metrics_.reconnect_attempts++;
            
            if (metrics_.reconnect_attempts > max_reconnect_attempts_) {
                change_state(ConnectionState::FAILED);
                cout << "[RECONNECT] ❌ 重连失败,超过最大尝试次数: " << channel_id_ << endl;
                return;
            }
        }
        
        int delay_ms = base_reconnect_delay_ms_ * (1 << (metrics_.reconnect_attempts - 1));
        delay_ms = min(delay_ms, 30000);
        
        cout << "[RECONNECT] 🔄 尝试重连 #" << metrics_.reconnect_attempts 
             << " (延迟 " << delay_ms << "ms): " << channel_id_ << endl;
        
        unique_lock<mutex> lock(cv_mutex_);
        cv_.wait_for(lock, chrono::milliseconds(delay_ms), 
                     [this] { return !running_.load(); });
        
        if (running_) {
            change_state(ConnectionState::CONNECTING);
        }
    }
    
    bool send_heartbeat() {
        {
            lock_guard<mutex> lock(metrics_mutex_);
            metrics_.heartbeat_sent++;
        }
        
        cout << "[HEARTBEAT] 💓 发送心跳: " << channel_id_ 
             << " (#" << metrics_.heartbeat_sent << ")" << endl;
        
        bool success = channel_->send_heartbeat();
        
        if (success) {
            {
                lock_guard<mutex> lock(metrics_mutex_);
                metrics_.heartbeat_received++;
            }
            cout << "[HEARTBEAT] ✅ 心跳响应: " << channel_id_ 
                 << " (成功率: " << fixed << setprecision(1) 
                 << metrics_.get_heartbeat_success_rate() << "%)" << endl;
        }
        
        return success;
    }
    
    void change_state(ConnectionState new_state) {
        auto old_state = state_.exchange(new_state);
        if (old_state != new_state) {
            cout << "[STATE] 🔄 状态变化: " << channel_id_ 
                 << " (" << state_to_string(old_state) 
                 << " -> " << state_to_string(new_state) << ")" << endl;
        }
    }
};

// ============================================================================
// 5. Demo 主程序
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
║  • 健康指标统计 (Success Rate/Uptime/Errors)                 ║
║                                                              ║
╚══════════════════════════════════════════════════════════════╝
)" << endl;

    // 创建模拟通道
    auto slack = shared_ptr<MockChannel>(new MockChannel("Slack"));
    
    // 创建监控器
    ChannelMonitor monitor(slack);
    
    // 启动监控
    cout << "\n[DEMO] 🚀 启动 Slack 通道监控...\n" << endl;
    monitor.start();
    
    // 场景 1: 正常运行 15 秒
    cout << "\n[DEMO] ⏱️  场景 1: 正常运行 15 秒...\n" << endl;
    this_thread::sleep_for(chrono::seconds(15));
    monitor.print_status();
    
    // 场景 2: 模拟网络故障 20 秒
    cout << "\n[DEMO] 🔥 场景 2: 模拟网络故障 20 秒...\n" << endl;
    slack->simulate_network_issue();
    this_thread::sleep_for(chrono::seconds(20));
    monitor.print_status();
    
    // 场景 3: 恢复网络 15 秒
    cout << "\n[DEMO] ✅ 场景 3: 恢复网络 15 秒...\n" << endl;
    slack->restore_network();
    this_thread::sleep_for(chrono::seconds(15));
    monitor.print_status();
    
    // 停止监控
    cout << "\n[DEMO] 🛑 停止监控...\n" << endl;
    monitor.stop();
    
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
║                                                              ║
╚══════════════════════════════════════════════════════════════╝
)" << endl;

    return 0;
}
