#pragma once

#include "util/FFmpegUtils.h"
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
    // Blocks if queue is full. Returns false if aborted.
    bool Push(AVPacket* pkt) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condPush.wait(lock, [&] { return m_queue.size() < static_cast<size_t>(m_maxSize) || m_abort; });
        if (m_abort) return false;

        AVPacket* copy = av_packet_alloc();
        av_packet_move_ref(copy, pkt);
        m_queue.push(copy);
        m_condPop.notify_one();
        return true;
    }

    // Pop a packet. Blocks if queue is empty. Returns false if aborted.
    bool Pop(AVPacket* pkt) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condPop.wait(lock, [&] { return !m_queue.empty() || m_abort; });
        if (m_abort && m_queue.empty()) return false;

        AVPacket* front = m_queue.front();
        m_queue.pop();
        av_packet_move_ref(pkt, front);
        av_packet_free(&front);
        m_condPush.notify_one();
        return true;
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
};
