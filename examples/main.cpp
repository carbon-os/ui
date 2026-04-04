#include <ui/webview.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

static std::filesystem::path local_app_data()
{
    PWSTR raw = nullptr;
    SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw);
    std::filesystem::path p(raw);
    CoTaskMemFree(raw);
    return p;
}
#endif

static void usage(const char* argv0)
{
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  --load-file <path>         HTML file to load\n"
#ifdef _WIN32
        "  --webview-runtime <path>   Path to the WebView2 fixed-version runtime directory\n"
#endif
        "  --help                     Show this message\n",
        argv0);
}

int main(int argc, char* argv[])
{
    std::string load_file;
    std::string runtime_path;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--load-file") == 0) {
            if (i + 1 >= argc) {
                std::fputs("error: --load-file requires a path argument\n", stderr);
                return 1;
            }
            load_file = argv[++i];
            continue;
        }
#ifdef _WIN32
        if (std::strcmp(argv[i], "--webview-runtime") == 0) {
            if (i + 1 >= argc) {
                std::fputs("error: --webview-runtime requires a path argument\n", stderr);
                return 1;
            }
            runtime_path = argv[++i];
            continue;
        }
#endif
        if (std::strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        std::fprintf(stderr, "error: unknown argument: %s\n", argv[i]);
        usage(argv[0]);
        return 1;
    }

    if (load_file.empty()) {
        std::fputs("error: --load-file is required\n", stderr);
        usage(argv[0]);
        return 1;
    }

#ifdef _WIN32
    if (runtime_path.empty()) {
        std::fputs("error: --webview-runtime is required on Windows\n", stderr);
        usage(argv[0]);
        return 1;
    }

    // Validate runtime path exists and looks correct
    std::filesystem::path rt(runtime_path);
    if (!std::filesystem::exists(rt)) {
        std::fprintf(stderr, "error: runtime path does not exist: %s\n", runtime_path.c_str());
        return 1;
    }
    if (!std::filesystem::exists(rt / "msedgewebview2.exe")) {
        std::fprintf(stderr, "error: msedgewebview2.exe not found in: %s\n", runtime_path.c_str());
        std::fputs("  contents:\n", stderr);
        for (auto& entry : std::filesystem::directory_iterator(rt))
            std::fprintf(stderr, "    %s\n", entry.path().filename().string().c_str());
        return 1;
    }

    // Ensure user data dir exists — WebView2 sometimes refuses to create it itself
    auto user_data = local_app_data() / "ipc_example";
    std::filesystem::create_directories(user_data);
    std::string user_data_str = user_data.string();
#endif

    std::string html = std::filesystem::absolute(load_file).lexically_normal().string();
    std::fprintf(stdout, "loading: %s\n", html.c_str());

    ui::WebView wv(ui::WebViewConfig{
        .title   = "IPC Example",
        .width   = 900,
        .height  = 640,
        .debug   = true,
        .logging = true,
#ifdef _WIN32
        .runtime_path  = runtime_path,
        .user_data_dir = user_data_str,
        .browser_args  =
            "--user-agent=\"IpcExample/1.0 "
            "(Windows NT; WebView2/146) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/146.0.0.0 Safari/537.36\"",
#endif
    });

    wv.on_message("ping", [&wv](ui::Message msg) {
        if (!msg.is_text()) return;
        std::string received(msg.text());
        std::printf("[C++] ping: %s\n", received.c_str());
        wv.post_message("pong", "C++ received: " + received);
    });

    wv.on_message("binary-in", [&wv](ui::Message msg) {
        if (!msg.is_binary()) return;
        const auto& bytes = msg.data();
        std::printf("[C++] binary-in: %zu bytes\n", bytes.size());
        std::vector<uint8_t> reply(bytes.size());
        for (std::size_t i = 0; i < bytes.size(); ++i)
            reply[i] = bytes[i] ^ 0xFF;
        wv.post_message("binary-out", reply);
    });

    wv.on_ready([&wv, &html]() {
        wv.load_file(html);
    });

    wv.on_close([&wv]() -> bool {
        std::puts("[C++] window closed – shutting down");
        wv.terminate();
        return true;
    });

    wv.run();
}