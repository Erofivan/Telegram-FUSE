#pragma once

#define FUSE_USE_VERSION 35

#include <fuse.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "cache.hpp"
#include "telegram_client.hpp"

struct FileHandle {
    int64_t message_id;
    std::string name;
    std::string tag;
    std::vector<uint8_t> buf;
    bool dirty;
    bool is_new;

    FileHandle() : message_id(0), dirty(false), is_new(false) {}
};

struct TgFsContext {
    std::shared_ptr<TelegramClient> tg;
    DownloadCache cache;
    std::mutex fh_mutex;
    std::map<uint64_t, FileHandle> open_files;
    std::map<std::string, std::vector<uint8_t>> pending_files;
    uint64_t next_fh = 1;

    static std::string PendingKey(const std::string& name,
                                  const std::string& tag) {
        return tag + "\n" + name;
    }

    uint64_t AllocFh(FileHandle fh) {
        std::lock_guard<std::mutex> lk(fh_mutex);
        uint64_t id = next_fh++;
        open_files[id] = std::move(fh);
        return id;
    }

    FileHandle* GetFh(uint64_t id) {
        std::lock_guard<std::mutex> lk(fh_mutex);
        auto it = open_files.find(id);
        return (it == open_files.end()) ? nullptr : &it->second;
    }

    void ReleaseFh(uint64_t id) {
        std::lock_guard<std::mutex> lk(fh_mutex);
        open_files.erase(id);
    }

    void PutPendingFile(const std::string& name,
                        const std::string& tag,
                        const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lk(fh_mutex);
        pending_files[PendingKey(name, tag)] = data;
    }

    bool HasPendingFile(const std::string& name,
                        const std::string& tag,
                        size_t* size = nullptr) {
        std::lock_guard<std::mutex> lk(fh_mutex);
        auto it = pending_files.find(PendingKey(name, tag));
        if (it == pending_files.end()) return false;
        if (size) *size = it->second.size();
        return true;
    }

    std::optional<std::vector<uint8_t>> GetPendingFile(const std::string& name,
                                                       const std::string& tag) {
        std::lock_guard<std::mutex> lk(fh_mutex);
        auto it = pending_files.find(PendingKey(name, tag));
        if (it == pending_files.end()) return std::nullopt;
        return it->second;
    }

    void RemovePendingFile(const std::string& name, const std::string& tag) {
        std::lock_guard<std::mutex> lk(fh_mutex);
        pending_files.erase(PendingKey(name, tag));
    }

    std::vector<std::pair<std::string, size_t>> ListPendingByTag(
        const std::string& tag) {
        std::lock_guard<std::mutex> lk(fh_mutex);
        std::vector<std::pair<std::string, size_t>> out;
        std::string prefix = tag + "\n";
        for (const auto& kv : pending_files) {
            if (kv.first.rfind(prefix, 0) == 0) {
                out.push_back({kv.first.substr(prefix.size()), kv.second.size()});
            }
        }
        return out;
    }
};

struct ParsedPath {
    std::string tag;
    std::string name;
    bool is_root_dir;

    static ParsedPath Parse(const char* path);
};