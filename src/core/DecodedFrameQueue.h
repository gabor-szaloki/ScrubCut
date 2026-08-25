#pragma once

#include "util/FFmpegUtils.h"
#include "util/Profiler.h"
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <queue>

// Queue of decoded (still unconverted) AVFrames from VideoDecodeThread to
// ConvertThread — the buffer between the decode and convert pipeline stages.
class DecodedFrameQueue {
public:
    explicit DecodedFrameQueue(int maxSize = 4) : m_maxSize(maxSize) {}
    ~DecodedFrameQueue() { Flush(); }

    DecodedFrameQueue(const DecodedFrameQueue&) = delete;
    DecodedFrameQueue& operator=(const DecodedFrameQueue&) = delete;

    // Push a frame (takes ownership via av_frame_move_ref). Blocks if the
    // queue is full. Returns false if aborted or interrupted — the frame is
    // left intact in that case, and queue contents are preserved on interrupt.
    bool Push(AVFrame* frame) {
        PROFILE_SCOPE_N("DecodedFrameQueue::Push");
        std::unique_lock<std::mutex> lock(m_mutex);
        {
            // Blocking here = queue-full backpressure.
            PROFILE_WAIT_SCOPE_N("WaitQueueFull");
            m_condPush.wait(lock, [&] { return m_queue.size() < static_cast<size_t>(m_maxSize) || m_abort || m_interrupt; });
        }
        if (m_abort || m_interrupt) return false;

        AVFrame* copy = av_frame_alloc();
        av_frame_move_ref(copy, frame);
        m_queue.push(copy);
        m_condPop.notify_one();
        return true;
    }

    // Pop with timeout. Returns false on timeout/abort/interrupt.
    bool PopWithTimeout(AVFrame* frame, int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        bool ready = m_condPop.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [&] { return !m_queue.empty() || m_abort || m_interrupt; });
        if (!ready) return false;
        if (m_interrupt) return false;
        if (m_abort && m_queue.empty()) return false;

        AVFrame* front = m_queue.front();
        m_queue.pop();
        av_frame_move_ref(frame, front);
        av_frame_free(&front);
        m_condPush.notify_one();
        return true;
    }

    void Flush() {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty()) {
            AVFrame* f = m_queue.front();
            m_queue.pop();
            av_frame_free(&f);
        }
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

    // Wake any waiting Push/Pop and make them return false without erasing
    // queue contents. See PacketQueue::Interrupt for usage.
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

    int Size() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_queue.size());
    }

private:
    std::queue<AVFrame*> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_condPush;
    std::condition_variable m_condPop;
    int m_maxSize;
    bool m_abort = false;
    bool m_interrupt = false;
};
