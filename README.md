# Mini-Redis：C++17 内存键值存储

一个兼容 RESP 协议的内存键值数据库，用 C++17 与 Linux epoll 从零实现。
支持 String / Hash / ZSet 三种数据类型、键过期、AOF 与 RDB 两种持久化，以及主从复制，
可以直接用 `redis-cli` 连接操作。

## 特性

- **网络层**：单线程 Reactor，`epoll` 边缘触发（`EPOLLET`），手写 RESP 协议解析状态机，
  支持流式解析与粘包/半包。
- **存储**：`unordered_map` 哈希索引 + 统一过期索引；ZSet 在小集合时用有序 `vector`，
  成员数超过 128 时自动切换为跳表。
- **过期**：惰性删除（访问时判断）+ 主动扫描（`timerfd` 每 200ms 触发，每次采样 64 个键）。
- **持久化**：AOF（`no` / `everysec` / `always` 三种刷盘策略，支持 `BGREWRITEAOF` 重写）
  与 RDB 快照（`MRDB2` 文本格式）。
- **复制**：`SYNC` 全量同步与 `PSYNC` 增量续传，基于 4MB 复制积压缓冲区（backlog）。
- **零第三方依赖**：只用 C++17 标准库与 POSIX 接口。

## 架构

```
            ┌──────────────────────────────────────────────┐
  redis-cli │  epoll_wait (EPOLLET)                        │
 ──────────►│    ├─ listen_fd  ──► accept ──► 每连接读缓冲   │
            │    ├─ conn_fd    ──► RESP 解析 ──► 命令分发    │
            │    └─ timer_fd   ──► 每 200ms 主动过期扫描     │
            └───────────────────┬──────────────────────────┘
                                │
                    ┌───────────▼────────────┐
                    │      KeyValueStore     │
                    │  map<key, Record>      │
                    │  expire_index_         │
                    │  String / Hash / ZSet  │
                    └───────────┬────────────┘
                                │ 写命令先追加
                    ┌───────────▼────────────┐
                    │   AOF（批量聚合 + fsync）│
                    │   RDB（MRDB2 快照）      │
                    │   backlog（供 PSYNC）    │
                    └────────────────────────┘
```

## 支持的命令

| 类型     | 命令                                              |
|:------ |:----------------------------------------------- |
| 连接     | `PING`、`ECHO`                                   |
| String | `SET`、`GET`、`DEL`、`EXISTS`、`KEYS`、`FLUSHALL`    |
| 过期     | `EXPIRE`、`TTL`                                  |
| Hash   | `HSET`、`HGET`、`HDEL`、`HEXISTS`、`HGETALL`、`HLEN` |
| ZSet   | `ZADD`、`ZREM`、`ZRANGE`、`ZSCORE`                 |
| 持久化    | `SAVE`、`BGSAVE`、`BGREWRITEAOF`                  |
| 运维     | `INFO`、`CONFIG GET`                             |
| 复制     | `SYNC`、`PSYNC`                                  |

## 持久化

### AOF

写命令先追加到 AOF。三种模式由 `aof.mode` 控制：

| 模式         | 行为                           |
|:---------- |:---------------------------- |
| `no`       | 只写用户态缓冲，由内核决定何时落盘            |
| `everysec` | 每秒一次 `fdatasync`，兼顾性能与安全（默认） |
| `always`   | 每条命令都 `fdatasync`，最安全但最慢     |

实现上的几个要点：

- **批量聚合**：攒够 `aof.batch_bytes`（默认 256KB）或等待超过 `aof.batch_wait_us`
  才真正调用 `writev`，把大量小写入合并成一次系统调用。
- **预分配**：按 `aof.prealloc_bytes`（默认 64MB）预扩展文件，减少文件尺寸反复增长。
- **部分写处理**：`writev` 返回值小于待写长度时，从偏移处继续写。
- **`sync_file_range`**：`aof.use_sync_file_range` 打开后，写入完成即触发后台回写，
  达到 `aof.sfr_min_bytes` 才调用，避免过于频繁。
- **`BGREWRITEAOF`**：后台线程重写 AOF，重写期间的新命令继续写旧文件并增量缓存，
  重写结束后切到新文件，避免阻塞主线程。

### RDB

`SAVE` / `BGSAVE` 把全量数据写成 `MRDB2` 格式的快照文件。格式为文本：
`STR` / `HASH` / `ZSET` 三段，每段先写数量，再逐行写记录，加载时按段解析。
进程启动时若存在 `dump.rdb` 会自动加载。

## 主从复制

- `SYNC`：从库连上后获得全量 RDB 快照，之后接收增量命令流。
- `PSYNC`：从库断线重连时带上自己的 offset，如果该 offset 仍落在主库的 4MB backlog 内，
  只补发缺失部分，否则退化为全量同步。

## 目录结构

```
.
├── include/mini_redis/     # 头文件（server / kv / resp / aof / rdb / replica / config）
├── src/                    # 实现
├── tools/
│   ├── test_smoke.sh       # 功能冒烟测试（依赖 redis-cli）
│   └── bench.sh            # 压测脚本（依赖 redis-benchmark）
├── always.conf             # 配置样例：always 刷盘
├── everysec.conf           # 配置样例：everysec 刷盘
├── none.conf               # 配置样例：不主动 fsync
├── aof.conf                # 配置样例：AOF + always
├── rdb.conf                # 配置样例：只用 RDB
└── CMakeLists.txt
```

## 构建

```bash
cmake -B build
cmake --build build -j
```

产物为 `build/mini_redis`，也可以用 `cmake --install build` 安装到 `bin/`。

## 运行

```bash
./build/mini_redis                     # 默认 0.0.0.0:6379
./build/mini_redis --port 7000          # 指定端口
./build/mini_redis --bind 127.0.0.1     # 只监听本机
./build/mini_redis --config aof.conf    # 读取配置文件
```

配置文件是 `key=value` 形式，可用项：

| 配置项                                                           | 说明                                       |
|:------------------------------------------------------------- |:---------------------------------------- |
| `port`、`bind_address`                                         | 监听端口与地址                                  |
| `aof.enabled`、`aof.mode`                                      | 是否开启 AOF 及刷盘模式（`no`/`everysec`/`always`） |
| `aof.dir`、`aof.filename`                                      | AOF 文件目录与文件名                             |
| `aof.batch_bytes`、`aof.batch_wait_us`                         | 批量聚合的字节阈值与等待上限                           |
| `aof.prealloc_bytes`                                          | AOF 预分配大小                                |
| `aof.sync_interval_ms`                                        | `everysec` 模式的实际同步周期                     |
| `aof.use_sync_file_range`、`aof.sfr_min_bytes`                 | 后台回写开关与触发阈值                              |
| `aof.fadvise_dontneed_after_sync`                             | 同步后对已落盘范围做 `DONTNEED`                    |
| `rdb.enabled`、`rdb.dir`、`rdb.filename`                        | RDB 开关与路径                                |
| `replica.enabled`、`replica.master_host`、`replica.master_port` | 作为从库连接主库                                 |

## 验证

```bash
# 功能冒烟（需要一个运行中的实例，且本机有 redis-cli）
./build/mini_redis --config aof.conf &
bash tools/test_smoke.sh

# 压测（需要 redis-benchmark）
DUR=10 bash tools/bench.sh

# 手动验证
redis-cli -p 6379 set foo bar
redis-cli -p 6379 get foo
redis-cli -p 6379 zadd rank 100 alice
redis-cli -p 6379 zrange rank 0 -1
```

## 说明

- 仅支持 Linux（依赖 `epoll`、`timerfd`、`writev`、`sync_file_range` 等接口）。
- 命令处理是单线程的：所有回调都在事件循环线程里执行，持久化与 AOF 重写在后台线程完成。
- `ZSCORE` 返回浮点格式（例如 `10.000000`），Redis 返回整数化的 `10`；语义一致，`redis-cli` 可正常解析。
- 未实现的命令会返回错误串，`CONFIG GET`、`INFO` 只提供最小可用集，便于
  `redis-cli` 等工具正常握手。
