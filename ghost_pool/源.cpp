#include <iostream>
#include <chrono>
#include <random>
#include <unordered_map>
#include <list>
#include "ghost_pool.h"
#include <thread>
#include <vector>
#include <string> 
#include <atomic>
using namespace std::chrono;

/*struct TestNode {
    int key;
    int value;

    TestNode(int k, int v) : key(k), value(v) {}
};
// ==================== 标准 new/delete 版本 ====================
class LRU_New_Shared {
    int capacity_;
    std::list<int> lru_list_;
    std::unordered_map<int, std::pair<std::shared_ptr<TestNode>, decltype(lru_list_)::iterator>> map_;

public:
    LRU_New_Shared(int cap) : capacity_(cap) {}

    void put(int key, int value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second.first->value = value;
            lru_list_.erase(it->second.second);
            lru_list_.push_front(key);
            it->second.second = lru_list_.begin();
            return;
        }

        if (map_.size() >= (size_t)capacity_) {
            int last_key = lru_list_.back();
            map_.erase(last_key);
            lru_list_.pop_back();
        }

        lru_list_.push_front(key);
        map_[key] = { std::make_shared<TestNode>(key, value), lru_list_.begin() };
    }

    int get(int key) {
        auto it = map_.find(key);
        if (it == map_.end()) return -1;
        lru_list_.erase(it->second.second);
        lru_list_.push_front(key);
        it->second.second = lru_list_.begin();
        return it->second.first->value;
    }
};

// ==================== 幽灵池版本 ====================
template<size_t MAX_NODES>
class LRU_Pool {
    int capacity_;
    std::list<shared_ghost_ptr<TestNode, MAX_NODES>> lru_list_;
    using ListIter = typename std::list<shared_ghost_ptr<TestNode, MAX_NODES>>::iterator;
    std::unordered_map<int, ListIter> map_;  // key -> 链表中的迭代器
    GhostPool<TestNode, MAX_NODES>& pool_;

public:
    LRU_Pool(int cap, GhostPool<TestNode, MAX_NODES>& pool)
        : capacity_(cap), pool_(pool) {
    }

    void put(int key, int value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            // 更新值
            (*it->second)->value = value;
            // 移动到头部
            lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
            return;
        }

        // 淘汰
        if (map_.size() >= (size_t)capacity_&&!lru_list_.empty()) {
            if (!lru_list_.empty()) {
                auto last = lru_list_.back();
                map_.erase(last->key);
                lru_list_.pop_back();
            }
        }

        // 分配新节点
        auto node = pool_.allocate(key, value);
        lru_list_.push_front(node);
        map_[key] = lru_list_.begin();
    }

    int get(int key) {
        auto it = map_.find(key);
        if (it == map_.end()) return -1;
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return (*it->second)->value;
    }
};
// ==================== 测试 ====================


int main() {
    const int CAPACITY = 100000;
    const int OPS = 2000000;
    const int READ_RATIO = 20;

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> key_dist(0, 50000);
    std::uniform_int_distribution<> ratio_dist(0, 99);

    std::cout << "=== LRU 缓存性能测试 ===\n";
    std::cout << "容量: " << CAPACITY << "\n";
    std::cout << "操作数: " << OPS << "\n";
    std::cout << "读比例: " << READ_RATIO << "%\n\n";

    // 标准共享指针版本
    {
        LRU_New_Shared lru(CAPACITY);
        auto start = high_resolution_clock::now();
        for (int i = 0; i < OPS; ++i) {
            int key = key_dist(gen);
            if (ratio_dist(gen) < READ_RATIO) {
                lru.get(key);
            }
            else {
                lru.put(key, i);
            }
        }
        auto end = high_resolution_clock::now();
        auto time_shared = duration_cast<milliseconds>(end - start).count();
        std::cout << "shared_ptr 版本: " << time_shared << " ms\n";
    }

    // 幽灵池版本
    {
        GhostPool<TestNode, 200000> pool;
        LRU_Pool<200000> lru(CAPACITY, pool);
        auto start = high_resolution_clock::now();
        for (int i = 0; i < OPS; ++i) {
            int key = key_dist(gen);
            if (ratio_dist(gen) < READ_RATIO) {
                lru.get(key);
            }
            else {
                lru.put(key, i);
            }
        }
        auto end = high_resolution_clock::now();
        auto time_pool = duration_cast<milliseconds>(end - start).count();
        std::cout << "幽灵池版本:     " << time_pool << " ms\n";
        //std::cout << "\n提升: " << (double)time_shared / time_pool << "x\n";
    }

    return 0;
}
*/

#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include "ghost_pool.h"

using namespace std::chrono;

struct Data {
    int id;
    Data(int i) : id(i) {}
    void print() const {
        std::cout << "  Data id: " << id << "\n";
    }
};

int main() {
    constexpr size_t POOL_SIZE = 10;
    GhostPool<Data, POOL_SIZE> pool;

    std::cout << "=== 幽灵池：同时访问与淘汰 ===\n\n";

    std::atomic<bool> running(true);
    std::atomic<int> success_count(0);

    // 分配对象
    auto sp = std::make_shared<shared_ghost_ptr<Data, POOL_SIZE>>(pool.allocate(100));
    auto wp = pool.observe(*sp);

    // 线程1：不断访问
    std::thread reader([&]() {
        while (running) {
            bool b=wp.access([&](Data* ptr) {
                if (ptr) success_count++;
                std::this_thread::sleep_for(milliseconds(3000));
                });
            if (!b) {
                std::cout << "Reader: wp.expired()\n";
            }
        }
        std::cout << "Reader 退出，成功访问: " << success_count << "\n";
        });

    // 线程2：延迟后释放 sp
    std::thread writer([&]() {
        std::this_thread::sleep_for(milliseconds(1000));
        std::cout << "Writer: 释放 sp\n";
        sp.reset();  // 释放 shared_ptr，对象被淘汰
        running = false;
        });

    writer.join();
    reader.join();

    std::cout << "\n=== 测试完成 ===\n";
    return 0;
}