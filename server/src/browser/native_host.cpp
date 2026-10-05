#include "reading_transport.h"
#include "engine/contracts/browser/reading_extension.h"
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <nlohmann/json.hpp>
int main(int argc, char **argv)
{
    if (argc < 2 || std::string(argv[1]) != BrowserReadingContract::ExtensionOrigin)
        return 1;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    uint32_t size = 0;
    if (fread(&size, 1, sizeof(size), stdin) != sizeof(size) || !size || size > BrowserReadingContract::MaxMessageBytes)
        return 1;
    std::string request(size, 0);
    if (fread(request.data(), 1, size, stdin) != size)
        return 1;
    std::string reply = nlohmann::json({{"ok", false}, {"error", "MSIME 后台未运行，请启动输入法后重试。"}}).dump();
    const auto name = BrowserReadingTransport::PipeName();
    if (!name.empty())
    {
        WaitNamedPipeW(name.c_str(), 1000);
        HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
        {
            if (BrowserReadingTransport::WriteMessage(pipe, std::move(request), [] { return false; }))
            {
                std::string result;
                if (BrowserReadingTransport::ReadMessage(pipe, result, 100000, [] { return false; }))
                    reply = std::move(result);
            }
            CloseHandle(pipe);
        }
    }
    size = static_cast<uint32_t>(reply.size());
    if (fwrite(&size, 1, sizeof(size), stdout) != sizeof(size) || fwrite(reply.data(), 1, size, stdout) != size)
        return 1;
    fflush(stdout);
    return 0;
}
