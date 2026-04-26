#define FUSE_USE_VERSION 35

#include <fuse.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "tgfs.hpp"

struct fuse_operations TgfsOps();

static void PrintUsage(const char* argv0) {
    std::cerr << "Usage: " << argv0
              << " <bot_token> <chat_id> <mountpoint> [fuse_options]\n";
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::string bot_token = argv[1];
    std::string chat_id = argv[2];
    std::string mountpoint = argv[3];

    std::vector<const char*> fuse_argv;
    fuse_argv.push_back(argv[0]);
    fuse_argv.push_back(mountpoint.c_str());
    for (int i = 4; i < argc; ++i) fuse_argv.push_back(argv[i]);
    fuse_argv.push_back("-s");

    auto ctx = std::make_unique<TgFsContext>();
    try {
        ctx->tg = std::make_shared<TelegramClient>(bot_token, chat_id);
    } catch (const std::exception& e) {
        std::cerr << "Failed to init Telegram client: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "[tgfs] mounting on " << mountpoint << "\n";
    std::cerr << "[tgfs] unmount: fusermount3 -u " << mountpoint << "\n";

    struct fuse_operations ops = TgfsOps();
    return fuse_main(
        static_cast<int>(fuse_argv.size()),
        const_cast<char**>(fuse_argv.data()),
        &ops,
        ctx.get());
}
