#pragma once

#include <string>

#include "mini_redis/config.hpp"

namespace mini_redis
{

    // 从 key=value 配置文件加载配置，支持 # 注释行与空行
    bool loadConfigFromFile(const std::string &path, ServerConfig &cfg, std::string &err);

} // namespace mini_redis
