#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <vector>

class DownloadCache {
public:
    static constexpr size_t kMaxEntries = 32;

    const std::vector<uint8_t>* Get(int64_t message_id) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(message_id);
        if (it == map_.end()) return nullptr;
        return &it->second;
    }

    void Put(int64_t message_id, std::vector<uint8_t> data) {
        std::lock_guard<std::mutex> lk(mu_);
        if (map_.size() >= kMaxEntries) {
            map_.erase(order_.front());
            order_.pop_front();
        }
        map_[message_id] = std::move(data);
        order_.push_back(message_id);
    }

    void Evict(int64_t message_id) {
        std::lock_guard<std::mutex> lk(mu_);
        map_.erase(message_id);
        order_.remove(message_id);
    }

private:
    mutable std::mutex mu_;
    std::map<int64_t, std::vector<uint8_t>> map_;
    std::list<int64_t> order_;
};