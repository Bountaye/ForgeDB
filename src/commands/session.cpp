#include "forgedb/engine.hpp"

namespace forgedb {
void Session::reset() { multi_ = false; dirty_ = false; bytes_ = 0; queue_.clear(); }
Reply Session::execute(Engine& engine, const Command& c) {
    ++engine.stats.commands;
    if (auto error = validate_command(c)) {
        if (multi_) dirty_ = true;
        return Reply::error(*error);
    }
    const auto op = uppercase(c[0]);
    if (op == "MULTI") {
        if (multi_) { dirty_ = true; return Reply::error("MULTI cannot be nested"); }
        multi_ = true; return Reply::ok();
    }
    if (op == "DISCARD") {
        if (!multi_) return Reply::error("DISCARD without MULTI");
        reset(); return Reply::ok();
    }
    if (op == "EXEC") {
        if (!multi_) return Reply::error("EXEC without MULTI");
        if (dirty_) { reset(); return Reply::error("EXEC aborted because of queue errors"); }
        auto commands = std::move(queue_); reset(); return engine.transaction(commands);
    }
    if (multi_) {
        if (op == "COMPACT" || op == "INFO") { dirty_ = true; return Reply::error("administrative command cannot be queued"); }
        for (const auto& arg : c) bytes_ += arg.size() + 16;
        if (queue_.size() >= 1024 || bytes_ > 1024 * 1024) {
            dirty_ = true; return Reply::error("transaction queue limit exceeded");
        }
        queue_.push_back(c); return Reply::ok("QUEUED");
    }
    return engine.execute(c);
}
}
