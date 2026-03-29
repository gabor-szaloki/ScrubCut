#pragma once

#include "util/FFmpegUtils.h"
#include <mutex>
#include <condition_variable>
#include <queue>

class FrameQueue {
public:
    explicit FrameQueue(int maxSize = 4) : m_maxSize(maxSize) {}
    ~FrameQueue() { Flush(); }

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    // Push a frame (takes ownership via av_frame_move_ref).
    // Blocks if queue is full. Returns false if aborted.
    bool Push(AVFrame* frame) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condPush.wait(lock, [&] { return m_queue.size() < static_cast<size_t>(m_maxSize) || m_abort; });
        if (m_abort) return false;

        AVFrame* copy = av_frame_alloc();
        av_frame_move_ref(copy, frame);
        m_queue.push(copy);
        m_condPop.notify_one();
        return true;
    }

    // Pop a frame. Blocks if queue is empty. Returns false if aborted.
    bool Pop(AVFrame* frame) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condPop.wait(lock, [&] { return !m_queue.empty() || m_abort; });
        if (m_abort && m_queue.empty()) return false;

        AVFrame* front = m_queue.front();
        m_queue.pop();
        av_frame_move_ref(frame, front);
        av_frame_free(&front);
        m_condPush.notify_one();
        return true;
    }

    // Try to pop without blocking. Returns false if empty.
    bool TryPop(AVFrame* frame) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return false;

        AVFrame* front = m_queue.front();
        m_queue.pop();
        av_frame_move_ref(frame, front);
        av_frame_free(&front);
        m_condPush.notify_one();
        return true;
    }

    // Peek at the front frame's PTS without removing it. Returns AV_NOPTS_VALUE if empty.
    int64_t PeekPts() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) return AV_NOPTS_VALUE;
        return m_queue.front()->pts;
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
};
