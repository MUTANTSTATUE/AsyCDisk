#pragma once

#include <uv.h>

class EventLoop {
public:
    EventLoop();
    ~EventLoop();

    // Prevent copy and assignment
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void Run();
    void Stop();

    uv_loop_t* GetLoop() const;

private:
    uv_loop_t* loop_;
};
