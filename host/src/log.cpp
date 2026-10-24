/*
 * log.cpp — 日志
 *
 * 回调可能在接收线程里被调用，调用方必须自己保证线程安全。
 */

#include "internal.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace espgpu {

namespace {
std::atomic<int> g_level{LOG_INFO};
std::mutex g_cb_mu;
espgpu_log_fn g_cb = nullptr;
void *g_cb_user = nullptr;
} // namespace

void log_msg(int level, const char *fmt, ...)
{
    if (level > g_level.load()) {
        return;
    }

    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    static const char *kTag[] = {"", "E", "W", "I", "D"};
    const int t = (level >= 0 && level <= LOG_DEBUG) ? level : 0;
    fprintf(stderr, "[espgpu %s] %s\n", kTag[t], buf);

    espgpu_log_fn cb;
    void *user;
    {
        std::lock_guard<std::mutex> lk(g_cb_mu);
        cb = g_cb;
        user = g_cb_user;
    }
    if (cb) {
        cb(level, buf, user);
    }
}

void log_set_level(int level)
{
    g_level = level;
}

void log_set_callback(espgpu_log_fn cb, void *user)
{
    std::lock_guard<std::mutex> lk(g_cb_mu);
    g_cb = cb;
    g_cb_user = user;
}

} // namespace espgpu
