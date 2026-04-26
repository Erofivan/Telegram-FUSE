#include "telegram_client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>

static size_t WriteCallback(char* ptr, size_t size, size_t nmemb,
                            void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

static size_t WriteBytesCallback(char* ptr, size_t size, size_t nmemb,
                                 void* userdata) {
    auto* buf = static_cast<std::vector<uint8_t>*>(userdata);
    buf->insert(buf->end(), ptr, ptr + size * nmemb);
    return size * nmemb;
}

static std::string JsonStr(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string val;
    for (size_t i = pos; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            char esc = json[++i];
            switch (esc) {
                case 'n': val += '\n'; break;
                case 'r': val += '\r'; break;
                case 't': val += '\t'; break;
                case '\\': val += '\\'; break;
                case '"': val += '"'; break;
                case '/': val += '/'; break;
                default: val += esc; break;
            }
        } else if (json[i] == '"') {
            return val;
        } else {
            val += json[i];
        }
    }
    return val;
}

static std::string JsonNum(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size() || json[pos] == '"') return {};
    auto end = pos;
    while (end < json.size() && (std::isdigit(json[end]) || json[end] == '-')) ++end;
    return json.substr(pos, end - pos);
}

static bool JsonBool(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    return json.substr(pos, 4) == "true";
}

static std::string FindCaption(const std::string& json) {
    return JsonStr(json, "caption");
}

std::string TelegramClient::MakeCaption(const std::string& tag) {
    if (tag.empty()) return {};
    std::string t = tag;
    std::replace(t.begin(), t.end(), ' ', '_');
    return "#" + t;
}

std::string TelegramClient::BuildCaption(const std::string& tag,
                                         const std::string& name) {
    if (tag.empty()) return "TGFS_FILE:" + name;
    return MakeCaption(tag) + "\nTGFS_FILE:" + name;
}

std::string TelegramClient::ExtractTag(const std::string& caption) {
    if (caption.empty() || caption[0] != '#') return {};
    auto newline = caption.find('\n');
    std::string hashtag = (newline == std::string::npos)
                              ? caption.substr(1)
                              : caption.substr(1, newline - 1);
    std::replace(hashtag.begin(), hashtag.end(), '_', ' ');
    return hashtag;
}

std::string TelegramClient::ExtractFilename(const std::string& caption) {
    auto pos = caption.find("TGFS_FILE:");
    if (pos == std::string::npos) return {};
    std::string name = caption.substr(pos + strlen("TGFS_FILE:"));
    auto nl = name.find('\n');
    if (nl != std::string::npos) name = name.substr(0, nl);
    return name;
}

TelegramClient::TelegramClient(std::string bot_token, std::string chat_id)
    : token_(std::move(bot_token)), chat_id_(std::move(chat_id)) {
    curl_global_init(CURL_GLOBAL_ALL);
}

std::string TelegramClient::ApiGet(
    const std::string& method,
    const std::map<std::string, std::string>& params) {
    std::string url =
        "https://api.telegram.org/bot" + token_ + "/" + method + "?";
    for (auto& [k, v] : params) {
        url += k + "=" + v + "&";
    }
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("curl error: ") +
                                 curl_easy_strerror(res));
    }
    return response;
}

std::string TelegramClient::ApiUpload(
    const std::string& method,
    const std::map<std::string, std::string>& fields,
    const std::string& file_field, const std::string& filename,
    const std::vector<uint8_t>& data) {
    std::string url = "https://api.telegram.org/bot" + token_ + "/" + method;
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl_easy_init failed");
    curl_mime* form = curl_mime_init(curl);
    for (auto& [k, v] : fields) {
        curl_mimepart* part = curl_mime_addpart(form);
        curl_mime_name(part, k.c_str());
        curl_mime_data(part, v.c_str(), CURL_ZERO_TERMINATED);
    }
    curl_mimepart* fp = curl_mime_addpart(form);
    curl_mime_name(fp, file_field.c_str());
    curl_mime_filename(fp, filename.c_str());
    curl_mime_data(fp, reinterpret_cast<const char*>(data.data()), data.size());
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode res = curl_easy_perform(curl);
    curl_mime_free(form);
    curl_easy_cleanup(curl);
    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("curl upload error: ") +
                                 curl_easy_strerror(res));
    }
    return response;
}

std::optional<TgFile> TelegramClient::ParseMessage(const std::string& msg) {
    auto doc_pos = msg.find("\"document\":");
    if (doc_pos == std::string::npos) return std::nullopt;
    auto obj_start = msg.find('{', doc_pos);
    if (obj_start == std::string::npos) return std::nullopt;
    int depth = 0;
    size_t obj_end = obj_start;
    for (size_t i = obj_start; i < msg.size(); ++i) {
        if (msg[i] == '{') ++depth;
        else if (msg[i] == '}') {
            --depth;
            if (depth == 0) {
                obj_end = i;
                break;
            }
        }
    }
    std::string doc = msg.substr(obj_start, obj_end - obj_start + 1);
    TgFile f;
    f.file_id = JsonStr(doc, "file_id");
    f.unique_id = JsonStr(doc, "file_unique_id");
    auto sz = JsonNum(doc, "file_size");
    f.size = sz.empty() ? 0 : std::stoll(sz);
    std::string tg_filename = JsonStr(doc, "file_name");
    std::string caption = FindCaption(msg);
    std::string cap_name = TelegramClient::ExtractFilename(caption);
    f.name = cap_name.empty() ? tg_filename : cap_name;
    if (f.name.empty()) f.name = f.unique_id;
    f.tag = TelegramClient::ExtractTag(caption);
    auto mid_s = JsonNum(msg, "message_id");
    f.message_id = mid_s.empty() ? 0 : std::stoll(mid_s);
    auto ds = JsonNum(msg, "date");
    f.date = ds.empty() ? 0 : static_cast<time_t>(std::stoll(ds));
    if (f.file_id.empty()) return std::nullopt;
    return f;
}

static std::string MetaPath(const std::string& chat_id) {
    std::string s = chat_id;
    for (char& c : s) {
        if (c == '-') c = '_';
    }
    return "/tmp/tgfs_meta_" + s + ".json";
}

static void SaveMeta(const std::vector<TgFile>& files,
                     const std::string& chat_id) {
    std::ofstream f(MetaPath(chat_id), std::ios::trunc);
    for (auto& tf : files) {
        f << "{\"message_id\":" << tf.message_id
          << ",\"name\":\"" << tf.name << "\""
          << ",\"tag\":\"" << tf.tag << "\""
          << ",\"file_id\":\"" << tf.file_id << "\""
          << ",\"unique_id\":\"" << tf.unique_id << "\""
          << ",\"size\":" << tf.size
          << ",\"date\":" << tf.date << "}\n";
    }
}

static std::vector<TgFile> LoadMeta(const std::string& chat_id) {
    std::vector<TgFile> result;
    std::ifstream f(MetaPath(chat_id));
    if (!f) return result;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        TgFile tf;
        auto mid = JsonNum(line, "message_id");
        tf.message_id = mid.empty() ? 0 : std::stoll(mid);
        tf.name = JsonStr(line, "name");
        tf.tag = JsonStr(line, "tag");
        tf.file_id = JsonStr(line, "file_id");
        tf.unique_id = JsonStr(line, "unique_id");
        auto sz = JsonNum(line, "size");
        tf.size = sz.empty() ? 0 : std::stoll(sz);
        auto ds = JsonNum(line, "date");
        tf.date = ds.empty() ? 0 : static_cast<time_t>(std::stoll(ds));
        if (!tf.file_id.empty()) result.push_back(tf);
    }
    return result;
}

void TelegramClient::FetchAllMessages() {
    // Read file list ONLY from local metadata.
    // Telegram Bot API does not allow reading channel/chat history,
    // so we maintain the index ourselves: every UploadFile / DeleteMessage
    // updates the meta file. getUpdates is unreliable for this purpose
    // (channel posts are not delivered there for bots reliably and updates
    // are consumed once read).
    cache_ = LoadMeta(chat_id_);
}

void TelegramClient::RefreshCache() {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!cache_valid_) {
        try {
            FetchAllMessages();
        } catch (const std::exception& e) {
            std::cerr << "[tgfs] FetchAllMessages failed: " << e.what()
                      << "\n";
            cache_.clear();
        } catch (...) {
            std::cerr << "[tgfs] FetchAllMessages failed: unknown\n";
            cache_.clear();
        }
        cache_valid_ = true;
    }
}

void TelegramClient::InvalidateCache() {
    std::lock_guard<std::mutex> lk(mutex_);
    cache_valid_ = false;
}

std::vector<TgFile> TelegramClient::ListFiles(const std::string& tag) {
    RefreshCache();
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<TgFile> result;
    for (auto& tf : cache_) {
        if (tf.tag == tag) result.push_back(tf);
    }
    return result;
}

std::vector<std::string> TelegramClient::ListDirs() {
    RefreshCache();
    std::lock_guard<std::mutex> lk(mutex_);
    std::set<std::string> tags;
    for (auto& tf : cache_) {
        if (!tf.tag.empty()) tags.insert(tf.tag);
    }
    return {tags.begin(), tags.end()};
}

std::optional<TgFile> TelegramClient::FindFile(const std::string& name,
                                               const std::string& tag) {
    RefreshCache();
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& tf : cache_) {
        if (tf.name == name && tf.tag == tag) return tf;
    }
    return std::nullopt;
}

std::vector<uint8_t> TelegramClient::DownloadFile(const std::string& file_id) {
    std::string resp;
    try {
        resp = ApiGet("getFile", {{"file_id", file_id}});
    } catch (const std::exception& e) {
        std::cerr << "[tgfs] getFile failed: " << e.what() << "\n";
        return {};
    }
    if (!JsonBool(resp, "ok")) return {};
    std::string file_path = JsonStr(resp, "file_path");
    if (file_path.empty()) return {};
    std::string url = "https://api.telegram.org/file/bot" + token_ + "/" +
                      file_path;
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    std::vector<uint8_t> data;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBytesCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return data;
}

std::optional<TgFile> TelegramClient::UploadFile(
    const std::string& name, const std::vector<uint8_t>& data,
    const std::string& tag) {
    std::map<std::string, std::string> fields;
    fields["chat_id"] = chat_id_;
    fields["caption"] = BuildCaption(tag, name);
    std::string resp;
    try {
        resp = ApiUpload("sendDocument", fields, "document", name, data);
    } catch (const std::exception& e) {
        std::cerr << "[tgfs] sendDocument failed: " << e.what() << "\n";
        return std::nullopt;
    }
    if (!JsonBool(resp, "ok")) {
        std::cerr << "[tgfs] upload failed: " << resp << "\n";
        return std::nullopt;
    }
    auto res_pos = resp.find("\"result\":");
    if (res_pos == std::string::npos) return std::nullopt;
    auto ms = resp.find('{', res_pos);
    if (ms == std::string::npos) return std::nullopt;
    int d = 0;
    size_t me = ms;
    for (size_t i = ms; i < resp.size(); ++i) {
        if (resp[i] == '{') ++d;
        else if (resp[i] == '}') {
            --d;
            if (!d) {
                me = i;
                break;
            }
        }
    }
    std::string msg_obj = resp.substr(ms, me - ms + 1);
    auto tf = ParseMessage(msg_obj);
    if (!tf) return std::nullopt;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // make sure cache is loaded before append
        if (!cache_valid_) {
            try { FetchAllMessages(); } catch (...) {}
            cache_valid_ = true;
        }
        cache_.push_back(*tf);
        SaveMeta(cache_, chat_id_);
    }
    return tf;
}

bool TelegramClient::DeleteMessage(int64_t message_id) {
    std::string resp;
    try {
        resp = ApiGet("deleteMessage",
                      {{"chat_id", chat_id_},
                       {"message_id", std::to_string(message_id)}});
    } catch (const std::exception& e) {
        std::cerr << "[tgfs] deleteMessage failed: " << e.what() << "\n";
        return false;
    }
    bool ok = JsonBool(resp, "ok");
    // Even if Telegram refuses to delete (e.g. older than 48h), drop it from
    // local index so the user does not see a ghost file.
    {
        std::lock_guard<std::mutex> lk(mutex_);
        cache_.erase(
            std::remove_if(cache_.begin(), cache_.end(),
                           [message_id](const TgFile& f) {
                               return f.message_id == message_id;
                           }),
            cache_.end());
        SaveMeta(cache_, chat_id_);
    }
    return ok;
}
