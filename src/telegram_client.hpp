#pragma once

#include <ctime>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct TgFile {
    int64_t message_id;
    std::string name;
    std::string file_id;
    std::string unique_id;
    int64_t size;
    time_t date;
    std::string tag;
};

class TelegramClient {
public:
    TelegramClient(std::string bot_token, std::string chat_id);

    std::vector<TgFile> ListFiles(const std::string& tag);
    std::vector<std::string> ListDirs();
    std::vector<uint8_t> DownloadFile(const std::string& file_id);
    std::optional<TgFile> UploadFile(const std::string& name,
                                     const std::vector<uint8_t>& data,
                                     const std::string& tag);
    bool DeleteMessage(int64_t message_id);
    std::optional<TgFile> FindFile(const std::string& name,
                                   const std::string& tag);
    void InvalidateCache();

private:
    std::string token_;
    std::string chat_id_;

    mutable std::mutex mutex_;
    bool cache_valid_ = false;
    std::vector<TgFile> cache_;

    void FetchAllMessages();
    void RefreshCache();

    std::string ApiGet(const std::string& method,
                       const std::map<std::string, std::string>& params = {});
    std::string ApiUpload(const std::string& method,
                          const std::map<std::string, std::string>& fields,
                          const std::string& file_field,
                          const std::string& filename,
                          const std::vector<uint8_t>& data);

    static std::optional<TgFile> ParseMessage(const std::string& json_fragment);
    static std::string MakeCaption(const std::string& tag);
    static std::string ExtractTag(const std::string& caption);
    static std::string ExtractFilename(const std::string& caption);
    static std::string BuildCaption(const std::string& tag,
                                    const std::string& name);
};