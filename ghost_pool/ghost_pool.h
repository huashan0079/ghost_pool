#pragma once
#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <stdexcept>

template<typename T, size_t N>
class GhostPool;

template<typename T, size_t N>
struct shared_ghost_ptr {
    std::shared_ptr<T> sp;
    GhostPool<T, N>* pool_;
    size_t idx;
    uint32_t generation;

    shared_ghost_ptr() = default;
    shared_ghost_ptr(std::shared_ptr<T> p, GhostPool<T, N>* pool, size_t i, uint32_t g)
        : sp(std::move(p)), pool_(pool), idx(i), generation(g) {
    }
    //weak_ghost_ptr<T, N> make_weak() const noexcept;
    // 透明地访问 shared_ptr
    T* operator->() const { return sp.get(); }
    T& operator*() const { return *sp; }
    explicit operator bool() const { return (bool)sp; }

    // 自动转换回 shared_ptr（用户友好）
    operator std::shared_ptr<T>() const { return sp; }
};

template<typename T, size_t N>
class weak_ghost_ptr {
    GhostPool<T, N>* pool_;
    size_t idx_;
    uint32_t expected_gen_;

public:
    weak_ghost_ptr() : pool_(nullptr), idx_(0), expected_gen_(0) {}

    // 只能从 shared_ghost_ptr 创建
    weak_ghost_ptr(const shared_ghost_ptr<T, N>& sp)
        : pool_(sp.pool_), idx_(sp.idx), expected_gen_(sp.generation) {
    }

    template<typename Func>
    bool access(Func&& f);

    bool expired() const;
};

// 幽灵池：环形数组 + 代际管理
template<typename T, size_t N>
class GhostPool {
private:
    // 堆上分配连续内存
    T* pool_;
    std::atomic<uint32_t>* generation_;
    std::atomic<bool>* is_alive_;
    std::atomic<int>* pin_count_;
    std::atomic<size_t> next_{ 0 };

public:
    GhostPool() {
        // 所有元数据在堆上，连续分配
        pool_ = static_cast<T*>(::operator new(sizeof(T) * N));
        generation_ = new std::atomic<uint32_t>[N];
        is_alive_ = new std::atomic<bool>[N];
        pin_count_ = new std::atomic<int>[N];

        for (size_t i = 0; i < N; ++i) {
            generation_[i].store(0);
            is_alive_[i].store(false);
            pin_count_[i].store(0);
        }
    }

    ~GhostPool() {
        // 析构存活对象
        for (size_t i = 0; i < N; ++i) {
            if (is_alive_[i].load()) {
                pool_[i].~T();
            }
        }
        ::operator delete(pool_);
        delete[] generation_;
        delete[] is_alive_;
        delete[] pin_count_;
    }

    // 禁止拷贝
    GhostPool(const GhostPool&) = delete;
    GhostPool& operator=(const GhostPool&) = delete;

    template<typename... Args>
    shared_ghost_ptr<T, N> allocate(Args&&... args) {
        size_t start = next_.fetch_add(1, std::memory_order_relaxed);
        size_t idx = start%N;

        while (true) {
            // 尝试抢占该槽位
            bool expected = false;
            if (is_alive_[idx].compare_exchange_weak(expected, true)) {
                break;  // 成功找到空闲槽位
            }
            // 当前槽位被占用，移到下一个
            idx = (idx + 1) % N;
            if (idx == start) {
                // 绕了一圈都没找到，池满了
                throw std::runtime_error("pool exhausted");
            }
        }
        if (idx >= N) {
            std::cerr << "ERROR: idx " << idx << " >= " << N << "\n";
            throw std::runtime_error("idx out of range");
        }
        // 自定义删除器：不释放内存，只标记
        auto deleter = [this, idx](T* ptr) {
            // 标记死亡（让弱指针知道）
            
            generation_[idx].fetch_add(1);
            // 等待所有 pin 释放（自旋）
            while (pin_count_[idx].load(std::memory_order_acquire) != 0) {
                _mm_pause();  // 或 std::this_thread::yield()
            }
            // 析构对象
            ptr->~T();
            is_alive_[idx].store(false);
            };

        // placement new 构造
        new (&pool_[idx]) T(std::forward<Args>(args)...);

        // 使用自定义删除器创建 shared_ptr
        std::shared_ptr<T> sp(&pool_[idx], deleter);
        return shared_ghost_ptr<T, N>(std::move(sp), this, idx, generation_[idx].load());
    }

    weak_ghost_ptr<T, N> observe(const shared_ghost_ptr<T, N>& sp) {
        return weak_ghost_ptr<T, N>(sp);
    }

    bool try_pin(size_t idx, uint32_t expected_gen) {
        // 第一次检查
        if (generation_[idx].load(std::memory_order_acquire) != expected_gen) {
            return false;
        }
        if (!is_alive_[idx].load(std::memory_order_acquire)) {
            return false;
        }

        // 增加 pin 计数
        pin_count_[idx].fetch_add(1, std::memory_order_acq_rel);

        // 二次验证：在增加计数后，检查状态是否变化
        if (generation_[idx].load(std::memory_order_acquire) != expected_gen ||
            !is_alive_[idx].load(std::memory_order_acquire)) {
            pin_count_[idx].fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        return true;
    }

    // 解除固定
    void unpin(size_t idx) {
        pin_count_[idx].fetch_sub(1, std::memory_order_release);
    }

    T* get_ptr(size_t idx) const {
        return &pool_[idx];
    }

    bool is_alive(size_t idx, uint32_t expected_gen) const {
        return generation_[idx].load(std::memory_order_acquire) == expected_gen &&
            is_alive_[idx].load(std::memory_order_acquire);
    }
};



// weak_ghost_ptr 成员函数定义（必须在头文件中）
template<typename T, size_t N>
template<typename Func>
bool weak_ghost_ptr<T, N>::access(Func&& f) {
    if (!pool_) return false;

    if (pool_->try_pin(idx_, expected_gen_)) {
        T* ptr = pool_->get_ptr(idx_);
        f(ptr);
        pool_->unpin(idx_);
        return true;
    }
    return false;
}

template<typename T, size_t N>
bool weak_ghost_ptr<T, N>::expired() const {
    return !pool_ || !pool_->is_alive(idx_, expected_gen_);
}




