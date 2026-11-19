# Mini-Redis：C++17 内存键值存储

用 C++17 与 Linux epoll 从零实现的内存键值数据库，兼容 RESP 协议。
支持 String / Hash / ZSet 三种数据结构、键过期、AOF 与 RDB 两种持久化、以及主从复制，
可以直接用 `redis-cli` 连接操作（无第三方依赖，只用标准库与 POSIX 接口）。

## 特性

- **单线程 Reactor**：`epoll` 边缘触发（`EPOLLET`），一个事件循环处理所有连接；
  超时驱动的过期扫描用 `timerfd` 挂进同一个循环，不额外开线程。
- **手写 RESP 协议解析**：状态机逐字节解析，支持粘包与半包（一次 read 里有多条命令、或半条命令），
  入站/出站缓冲都是动态字节数组。
- **三种数据结构**：String、Hash、ZSet。ZSet 在小集合时用有序 `vector`，
  成员数超过 128 时自动切换为跳表，兼顾小集合的缓存友好与大集合的插入性能。
- **键过期**：惰性删除（访问时判断）+ 主动扫描（每 200ms 触发，每次采样 64 个键）。
- **AOF 持久化**：`no` / `everysec` / `always` 三种刷盘策略，批量聚合、预分配、
  `writev` 合并写、`sync_file_range` 后台回写、`BGREWRITEAOF` 重写。
- **RDB 快照**：`MRDB2` 文本格式，`SAVE` / `BGSAVE` 落盘，启动时自动加载。
- **主从复制**：`SYNC` 全量同步，`PSYNC` 基于 4MB 复制积压缓冲区（backlog）增量续传。

## 架构

```
        ┌─────────────────────────────────────────────────────┐
        │  主线程：epoll_wait (EPOLLET)                        │
        │    ├─ listen_fd ─► accept ─► 每连接读缓冲             │
        │    ├─ conn_fd   ─► RESP 解析 ─► 命令分发 ─► 响应入缓冲  │
        │    └─ timer_fd  ─► 每 200ms 主动过期扫描（64 键/次）    │
        └───────────────┬─────────────────────────────────────┘
                        │ 写命令
        ┌───────────────▼──────────────┐   ┌──────────────────┐
        │   KeyValueStore              │   │  AOF writer 线程  │
        │   map<string, Record>        │──►│  批量 writev      │
        │   expire_index_              │   │  everysec/always │
        │   String / Hash / ZSet(跳表)  │   ├──────────────────┤
        └───────────────┬──────────────┘   │ AOF rewriter 线程 │
                        │                  └──────────────────┘
                        ├─► RDB 快照（MRDB2 文本，SAVE / BGSAVE）
                        └─► backlog（4MB）─► 从库（SYNC / PSYNC）
```

## 支持的命令

| 分组 | 命令 |
| :--- | :--- |
| 连接 | `PING`、`ECHO` |
| String / 通用 | `SET`、`GET`、`DEL`、`EXISTS`、`KEYS`、`FLUSHALL` |
| 过期 | `EXPIRE`、`TTL` |
| Hash | `HSET`、`HGET`、`HDEL`、`HEXISTS`、`HGETALL`、`HLEN` |
| ZSet | `ZADD`、`ZREM`、`ZRANGE`、`ZSCORE` |
| 持久化 | `SAVE`、`BGSAVE`、`BGREWRITEAOF` |
| 运维 | `INFO`、`CONFIG GET` |
| 复制 | `SYNC`、`PSYNC` |

共 27 条。未实现的命令返回错误串，不会因此断开连接。

## 目录结构

```
.
├── include/mini_redis/       # 头文件
│   ├── server.hpp            # 事件循环、连接管理、命令分发
│   ├── kv.hpp                # KeyValueStore：String / Hash / ZSet 与过期索引
│   ├── resp.hpp              # RESP 解析与响应编码
│   ├── aof.hpp               # AOF 写入、重写、回放
│   ├── rdb.hpp               # RDB 快照（MRDB2）
│   ├── replica_client.hpp    # 从库向主库同步
│   ├── config.hpp            # 配置结构体
│   └── ...
├── src/                      # 与上面一一对应的实现
├── tools/
│   ├── test_smoke.sh         # 冒烟测试（需本机有 redis-cli）
│   └── bench.sh              # 压测脚本（需 redis-benchmark）
├── always.conf everysec.conf none.conf aof.conf rdb.conf   # 配置样例
└── CMakeLists.txt
```

## 构建与运行

```bash
cmake -B build
cmake --build build -j          # 产物 build/mini_redis
```

```bash
./build/mini_redis                       # 默认 0.0.0.0:6379
./build/mini_redis --port 7000            # 指定端口
./build/mini_redis --bind 127.0.0.1       # 只监听本机
./build/mini_redis --config aof.conf      # 读取配置文件
./build/mini_redis --help
```

CMake 默认 `Release`（`-O3 -DNDEBUG`），并开启 `-Wall -Wextra -Wpedantic -Wconversion`。

## 配置

配置文件是 `key=value` 形式（`true` / `false`、`1` / `0`、`yes` 都可）。命令行参数优先级高于配置文件。

| 配置项 | 说明 | 样例值 |
| :--- | :--- | :--- |
| `port` | 监听端口 | `6379` |
| `bind_address` | 监听地址 | `0.0.0.0` |
| `aof.enabled` | 是否开启 AOF | `true` |
| `aof.mode` | 刷盘策略：`no` / `everysec` / `always` | `everysec` |
| `aof.dir`、`aof.filename` | AOF 文件位置 | `./data`、`appendonly.aof` |
| `aof.batch_bytes` | 攒够多少字节就落一次 `writev` | `262144` |
| `aof.batch_wait_us` | 攒不够字节时的最长等待（微秒） | `2000` |
| `aof.prealloc_bytes` | 预分配大小（见「已知问题」第 1 条） | `67108864` |
| `aof.sync_interval_ms` | `everysec` 模式的实际同步周期 | `250` |
| `aof.use_sync_file_range`、`aof.sfr_min_bytes` | 后台回写开关与触发阈值 | `true`、`1048576` |
| `aof.fadvise_dontneed_after_sync` | 落盘后对已写范围做 `DONTNEED` | `false` |
| `rdb.enabled`、`rdb.dir`、`rdb.filename` | RDB 开关与路径 | `true`、`./data`、`dump.rdb` |
| `replica.enabled` | 作为从库启动 | `true` |
| `replica.master_host`、`replica.master_port` | 主库地址 | `127.0.0.1`、`6379` |

## 实测结果

环境：WSL2 Ubuntu / g++ 15.2 / `-O2`。

### 功能

| 项目 | 结果 |
| :--- | :--- |
| 编译（`-Wall -Wextra -Wpedantic -Wconversion`） | 0 警告、0 错误 |
| `PING` / `SET` / `GET` / `EXISTS` / `TTL` / `DEL` | 返回值与 Redis 语义一致 |
| `HSET` / `HGET`、`ZADD` / `ZSCORE` | 正常 |
| `INFO` / `CONFIG GET` | 返回内容，`redis-cli` 可正常握手 |
| RDB：`SAVE` → `kill -9` → 重启 | 数据与 TTL 均恢复（`dump.rdb` 93 B，格式 `MRDB2`） |
| 主从：主库 `SET` / `HSET` → 从库读 | 从库能读到增量数据 |
| AOF（`aof.prealloc_bytes=0`）：`SET` / `EXPIRE` 回放 | 通过 |
