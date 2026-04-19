#include "EventLoop.h"
#include "Logger.h"

EventLoop::EventLoop() {
    loop_ = new uv_loop_t();
    uv_loop_init(loop_);
    LOG_INFO("EventLoop initialized.");
}

EventLoop::~EventLoop() {
    if (loop_) {
        // Warning: uv_loop_close will return UV_EBUSY if there are still active handles
        int res = uv_loop_close(loop_);
        if (res == UV_EBUSY) {
            LOG_WARN("EventLoop destroyed with active handles! Memory leak possible.");
        }
        delete loop_;
        loop_ = nullptr;
        LOG_INFO("EventLoop destroyed.");
    }
}

void EventLoop::Run() {
    LOG_INFO("EventLoop running...");
    uv_run(loop_, UV_RUN_DEFAULT);
    LOG_INFO("EventLoop stopped.");
}

void EventLoop::Stop() {
    LOG_INFO("EventLoop stop requested.");
    uv_stop(loop_);
}

uv_loop_t* EventLoop::GetLoop() const {
    return loop_;
}
