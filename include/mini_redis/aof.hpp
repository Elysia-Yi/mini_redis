#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <chrono>

#include "mini_redis/config.hpp"

namespace mini_redis
{

  class KeyValueStore;

  class AofLogger
  {
  public:
    AofLogger();
    ~AofLogger();

    bool init(const AofOptions &opts, std::string &err);
    void shutdown();

    // 回放已有 AOF 到 store
    bool load(KeyValueStore &store, std::string &err);

    // 追加一条命令，如 {"SET","k","v"}
    bool appendCommand(const std::vector<std::string> &parts);

    // 直接追加收到的原始 RESP 字节，避免重新序列化
    bool appendRaw(const std::string &raw_resp);

    bool isEnabled() const { return opts_.enabled; }
    AofMode mode() const { return opts_.mode; }
    std::string path() const;

    // 后台重写 AOF（BGREWRITEAOF）
    bool bgRewrite(KeyValueStore &store, std::string &err);

  private:
    int fd_ = -1;
    AofOptions opts_;
    std::atomic<bool> running_{false};
    int timer_fd_ = -1; // everysec 定时 fsync

    struct AofItem
    {
      std::string data;
      int64_t seq;
    };
    std::thread writer_thread_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::condition_variable cv_commit_;
    std::deque<AofItem> queue_;
    std::atomic<bool> stop_{false};
    size_t pending_bytes_ = 0;
    std::chrono::steady_clock::time_point last_sync_tp_{std::chrono::steady_clock::now()};
    std::atomic<int64_t> seq_gen_{0};
    int64_t last_synced_seq_ = 0;

    std::atomic<bool> rewriting_{false};
    std::thread rewriter_thread_;
    std::mutex incr_mtx_;
    std::vector<std::string> incr_cmds_;

    // 重写时暂停 writer，便于安全切换 fd
    std::atomic<bool> pause_writer_{false};
    std::mutex pause_mtx_;
    std::condition_variable cv_pause_;
    bool writer_is_paused_ = false;

    void writerLoop();
    void rewriterLoop(KeyValueStore *store);
  };

  std::string toRespArray(const std::vector<std::string> &parts);

} // namespace mini_redis
