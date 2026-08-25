#pragma once

#include "core/ConvertedFrame.h"
#include "util/FFmpegUtils.h"
#include "util/Profiler.h"
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <queue>

// Queue of display-ready converted frames from ConvertThread to the main
// thread (which picks the due frame and uploads it).
class FrameQueue {
public:
    explicit FrameQueue(int maxSize = 4) : m_maxSize(maxSize) {}

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    // Push a frame. Blocks if queue is full. Returns false if aborted or
    // interrupted — the frame is not enqueued in that case, and queue
    // contents are preserved on interrupt.
    bool Push(ConvertedFramePtr frame) {
        PROFILE_SCOPE_N("FrameQueue::Push");
        std::unique_lock<std::mutex> lock(m_mutex);
        {
            // Blocking here = queue-full backpressure.
            PROFILE_WAIT_SCOPE_N("WaitQueueFull");
            m_condPush.wait(lock, [&] { return m_queue.size() < static_cast<size_t>(m_maxSize) || m_abort || m_interrupt; });
        }
        if (m_abort || m_interrupt) return false;

        m_queue.push(std::move(frame));
        m_condPop.notify_one();
        return true;
    }

    // Try to pop without blocking. Returns nullptr if empty.
    ConvertedFramePtr TryPop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return nullptr;

        ConvertedFramePtr front = std::move(m_queue.front());
        m_queue.pop();
        m_condPush.notify_one();
        return front;
    }

    // Peek at the front frame's PTS without removing it. Returns AV_NOPTS_VALUE if empty.
    int64_t PeekPts() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return AV_NOPTS_VALUE;
        return m_queue.front()->pts;
    }

    void Flush() {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty()) m_queue.pop();
        m_condPush.notify_all();
    }

    void Abort() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_abort = true;
        m_condPush.notify_all();
        m_condPop.notify_all();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_abort = false;
        m_interrupt = false;
    }

    // Wake any waiting Push and make it return false without erasing queue
    // contents. See PacketQueue::Interrupt for usage.
    void Interrupt() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_interrupt = true;
        m_condPush.notify_all();
        m_condPop.notify_all();
    }

    void ClearInterrupt() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_interrupt = false;
    }

    // Wait until queue has at least one frame, an abort/interrupt occurs,
    // or `timeoutMs` elapses. Used by StepFrame to tick the pipeline once.
    // Returns true if a frame is now available.
    bool WaitForOne(int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condPop.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [&] { return !m_queue.empty() || m_abort || m_interrupt; })
            && !m_queue.empty();
    }

    int Size() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_queue.size());
    }

private:
    std::queue<ConvertedFramePtr> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_condPush;
    std::condition_variable m_condPop;
    int m_maxSize;
    bool m_abort = false;
    bool m_interrupt = false;
};
