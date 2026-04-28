#pragma once

#include "util/FFmpegUtils.h"
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <queue>

class PacketQueue {
public:
    explicit PacketQueue(int maxSize = 64) : m_maxSize(maxSize) {}
    ~PacketQueue() { Flush(); }

    PacketQueue(const PacketQueue&) = delete;
    PacketQueue& operator=(const PacketQueue&) = delete;

    // Push a packet (takes ownership via av_packet_move_ref).
    // Blocks if queue is full. Returns false if aborted or interrupted —
    // contents preserved on interrupt, caller should re-check external state
    // (e.g. pipeline-active flag) and retry or park.
    bool Push(AVPacket* pkt) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condPush.wait(lock, [&] { return m_queue.size() < static_cast<size_t>(m_maxSize) || m_abort || m_interrupt; });
        if (m_abort || m_interrupt) return false;

        AVPacket* copy = av_packet_alloc();
        av_packet_move_ref(copy, pkt);
        m_queue.push(copy);
        m_condPop.notify_one();
        return true;
    }

    // Pop a packet. Blocks if queue is empty. Returns false if aborted or
    // interrupted (contents preserved on interrupt).
    bool Pop(AVPacket* pkt) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condPop.wait(lock, [&] { return !m_queue.empty() || m_abort || m_interrupt; });
        if (m_interrupt) return false;
        if (m_abort && m_queue.empty()) return false;

        AVPacket* front = m_queue.front();
        m_queue.pop();
        av_packet_move_ref(pkt, front);
        av_packet_free(&front);
        m_condPush.notify_one();
        return true;
    }

    // Pop with timeout. Returns false on timeout/abort/interrupt.
    bool PopWithTimeout(AVPacket* pkt, int timeoutMs) {
        std::unique_lock<std::mutex> lock(m_mutex);
        bool ready = m_condPop.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [&] { return !m_queue.empty() || m_abort || m_interrupt; });
        if (!ready) return false;
        if (m_interrupt) return false;
        if (m_abort && m_queue.empty()) return false;

        AVPacket* front = m_queue.front();
        m_queue.pop();
        av_packet_move_ref(pkt, front);
        av_packet_free(&front);
        m_condPush.notify_one();
        return true;
    }

    bool Empty() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_queue.empty();
    }

    void Flush() {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty()) {
            AVPacket* pkt = m_queue.front();
            m_queue.pop();
            av_packet_free(&pkt);
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
    // queue contents. Use to nudge threads to a safe park point during
    // pause/seek; pair with ClearInterrupt() before resuming.
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
    std::queue<AVPacket*> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_condPush;
    std::condition_variable m_condPop;
    int m_maxSize;
    bool m_abort = false;
    bool m_interrupt = false;
};
