#include "ipc_negotiation.h"
#include "ipc_test_endpoints.h"
#include "voice_composition_pipe.h"
#include <Windows.h>
#include <winhttp.h>

#include <atomic>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

class Pipe {
public:
  explicit Pipe(const wchar_t *name) {
    const auto deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
      handle_ = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
      if (handle_ != INVALID_HANDLE_VALUE) {
        DWORD mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
        if (SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr))
          return;
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        throw std::runtime_error("Could not configure probe pipe");
      }
      Sleep(10);
    }
    throw std::runtime_error("Server did not open its pipe within 15 seconds");
  }
  ~Pipe() {
    if (handle_ != INVALID_HANDLE_VALUE)
      CloseHandle(handle_);
  }
  Pipe(const Pipe &) = delete;
  Pipe &operator=(const Pipe &) = delete;

  template <typename T> void Write(const T &packet) {
    DWORD written = 0;
    if (!WriteFile(handle_, &packet, sizeof(packet), &written, nullptr) ||
        written != sizeof(packet))
      throw std::runtime_error("Could not write a complete probe frame");
  }

  template <typename T> T Read() {
    const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
      T packet{};
      DWORD read = 0;
      if (ReadFile(handle_, &packet, sizeof(packet), &read, nullptr)) {
        if (read == sizeof(packet))
          return packet;
        if (read != 0)
          throw std::runtime_error("Truncated probe frame");
      } else if (GetLastError() != ERROR_NO_DATA)
        throw std::runtime_error(
            "Probe pipe disconnected before its expected reply");
      Sleep(5);
    }
    throw std::runtime_error(
        "Server did not acknowledge the probe within 5 seconds");
  }

  template <typename T> bool TryRead(T &packet) {
    DWORD read = 0;
    if (ReadFile(handle_, &packet, sizeof(packet), &read, nullptr))
      return read == sizeof(packet);
    if (GetLastError() != ERROR_NO_DATA)
      throw std::runtime_error("Worker pipe disconnected");
    return false;
  }

private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class WorkerQueue {
public:
  explicit WorkerQueue(Pipe &pipe)
      : reader_([this, &pipe] {
          while (!stop_) {
            FanyImeNamedpipeDataToTsfWorkerThread packet{};
            try {
              if (pipe.TryRead(packet)) {
                if (packet.msg_type !=
                        FanyImeWorkerReplyType::CommitTranslationBuffer &&
                    packet.msg_type !=
                        FanyImeWorkerReplyType::FocusSessionReady)
                  continue;
                std::lock_guard<std::mutex> lock(mutex_);
                packets_.push_back(packet);
              } else
                Sleep(2);
            } catch (...) {
              stop_ = true;
            }
          }
        }) {}
  ~WorkerQueue() {
    stop_ = true;
    reader_.join();
  }
  FanyImeNamedpipeDataToTsfWorkerThread Read() {
    const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!packets_.empty()) {
          auto packet = packets_.front();
          packets_.pop_front();
          return packet;
        }
      }
      Sleep(2);
    }
    throw std::runtime_error("Worker delivery timeout");
  }

private:
  std::atomic<bool> stop_{false};
  std::mutex mutex_;
  std::deque<FanyImeNamedpipeDataToTsfWorkerThread> packets_;
  std::thread reader_;
};

void Require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

int ApiCount() {
  wchar_t portText[20]{};
  GetEnvironmentVariableW(L"MSIME_PROBE_HTTP_PORT", portText, 20);
  HINTERNET session = WinHttpOpen(
      L"MSIME Buffer Test", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0);
  HINTERNET connection = WinHttpConnect(
      session, L"127.0.0.1", static_cast<INTERNET_PORT>(_wtoi(portText)), 0);
  HINTERNET request = WinHttpOpenRequest(connection, L"GET", L"/count", nullptr,
                                         nullptr, nullptr, 0);
  Require(request &&
              WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr),
          "Mock counter unavailable");
  char result[32]{};
  DWORD read = 0;
  Require(WinHttpReadData(request, result, 31, &read), "Counter read failed");
  WinHttpCloseHandle(request);
  WinHttpCloseHandle(connection);
  WinHttpCloseHandle(session);
  return std::stoi(result);
}
int main() {
  try {
    UseIsolatedTranslationProbeEndpoints();
    const uint64_t client =
        (static_cast<uint64_t>(GetCurrentProcessId()) << 32) | 100u;
    std::cout << "stage: connect" << std::endl;
    Pipe replies(FANY_IME_TO_TSF_NAMED_PIPE);
    FanyImePipeHello endpoint{client, FanyImePipeRole::ToTsf};
    replies.Write(endpoint);
    Require(replies.Read<FanyImeNamedpipeDataToTsf>().msg_type ==
                FanyImeReplyType::PipeReady,
            "Reply registration");
    Pipe workers(FANY_IME_TO_TSF_WORKER_THREAD_NAMED_PIPE);
    endpoint.pipe_role = FanyImePipeRole::ToTsfWorkerThread;
    workers.Write(endpoint);
    Require(workers.Read<FanyImeNamedpipeDataToTsfWorkerThread>().msg_type ==
                FanyImeWorkerReplyType::PipeReady,
            "Worker registration");
    WorkerQueue workerQueue(workers);
    Pipe main(FANY_IME_NAMED_PIPE);
    const auto hello = FanyImeProtocol::Hello(
        client, 1,
        FanyImeProtocol::Capabilities | FanyImeProtocol::TranslationBuffer |
            FanyImeProtocol::CompositionRestore |
            FanyImeProtocol::CaretStateIndicator);
    main.Write(hello);
    std::cout << "stage: negotiate" << std::endl;
    Require(FanyImeProtocol::AcceptReply(
                replies.Read<FanyImeNamedpipeDataToTsf>(), 1),
            "Main negotiation");
    FanyImeNamedpipeData packet{};
    packet.client_id = client;
    packet.event_type = FanyImePipeEventType::ClientActivated;
    packet.request_id = 2;
    main.Write(packet);
    for (int i = 0; i < 40; ++i)
      if (workerQueue.Read().msg_type ==
          FanyImeWorkerReplyType::FocusSessionReady)
        break;
    std::cout << "stage: activated" << std::endl;
    packet.event_type = FanyImePipeEventType::StatusSnapshot;
    packet.keycode = 1;
    main.Write(packet);
    wchar_t normalMode[2]{};
    const bool ordinaryMode =
        GetEnvironmentVariableW(L"MSIME_PROBE_NORMAL", normalMode, 2) != 0;
    uint64_t sequence = 10;
    std::wstring localSpelling;
    const auto key = [&](UINT code, WCHAR ch, UINT modifiers = 0) {
      FanyImeNamedpipeData k{};
      k.client_id = client;
      k.event_type = FanyImePipeEventType::KeyEvent;
      k.request_id = ++sequence;
      k.keycode = code;
      k.wch = ch;
      k.modifiers_down = modifiers;
      k.point[0] = 200;
      k.point[1] = 200;
      if (ordinaryMode) {
        if (code >= 'A' && code <= 'Z')
          localSpelling += ch;
        k.pinyin_length = static_cast<int>(localSpelling.size());
        wcscpy_s(k.pinyin_string, localSpelling.c_str());
      }
      main.Write(k);
      if (ordinaryMode && code >= 'A' && code <= 'Z') {
        Sleep(20);
        return;
      }

      const auto response = replies.Read<FanyImeNamedpipeDataToTsf>();
      Require(response.request_id == k.request_id,
              "Key reply request ID mismatch");
      if (ordinaryMode) {
        Require(response.msg_type != FanyImeReplyType::TranslationBufferState,
                "Ordinary Chinese was unexpectedly buffered");
        if (code == VK_SPACE)
          Require(response.msg_type == FanyImeReplyType::Normal &&
                      std::wstring(response.candidate_string) == L"你好",
                  "Ordinary Space did not immediately commit Chinese");
      } else
        Require(response.msg_type == FanyImeReplyType::TranslationBufferState,
                "Buffered key accidentally followed a commit/composition path");
    };
    const auto type = [&](const std::string &raw) {
      for (char c : raw)
        key(static_cast<UINT>(toupper(c)), static_cast<WCHAR>(c));
    };
    if (ordinaryMode) {
      type("nihao");
      FanyImeNamedpipeData show{};
      show.client_id = client;
      show.event_type = FanyImePipeEventType::ShowCandidateWnd;
      show.pinyin_length = static_cast<int>(localSpelling.size());
      wcscpy_s(show.pinyin_string, localSpelling.c_str());
      main.Write(show);
      Sleep(50);
      key(VK_SPACE, L' ');
      Sleep(1000);
      Require(ApiCount() == 0, "Ordinary Chinese called translation API");
      std::cout << "PASS: ordinary Chinese Space commits immediately, zero "
                   "translation API calls\n";
      return 0;
    }
    const auto sentence = [&] {
      for (const auto raw : {"wojuede", "wozhegeren", "haishitinghaode"}) {
        type(raw);
        key(VK_SPACE, L' ');
        Require(ApiCount() == 0, "Phrase selection called translation API");
      }
      key(VK_OEM_PERIOD, L'.');
    };
    const auto delivery = [&]() {
      std::wstring assembled;
      for (int i = 0; i < 10000; ++i) {
        const auto w = workerQueue.Read();
        if (w.msg_type != FanyImeWorkerReplyType::CommitTranslationBuffer)
          continue;
        const auto frame = FanyImeVoiceCompositionPipe::ParseFrame(w.data);
        Require(frame.valid, "Invalid framed translation delivery");
        if (frame.first)
          assembled.clear();
        assembled += frame.chunk;
        if (!frame.last)
          continue;
        std::wstring text = std::move(assembled);
        auto split = text.find(L'\t');
        Require(split != std::wstring::npos, "Malformed delivery");
        FanyImeNamedpipeData receipt{};
        receipt.event_type = FanyImePipeEventType::TranslationCommitAck;
        receipt.client_id = client;
        receipt.request_id = std::stoull(text.substr(0, split));
        receipt.wch = 1;
        main.Write(receipt);
        Sleep(30);
        return text.substr(split + 1);
      }
      throw std::runtime_error("No final delivery");
    };
    std::cout << "stage: segmented" << std::endl;
    sentence();
    Sleep(1000);
    Require(ApiCount() == 0, "Idle buffer called translation API");
    key(VK_RETURN, L'\r', 2);
    key(VK_RETURN, L'\r', 2);
    Require(delivery() == L"Translated sentence.",
            "Expected final translation only");
    Require(ApiCount() == 1, "Duplicate Ctrl+Enter made concurrent requests");
    // Identical segmented sentence is now served from SQLite.
    for (const auto raw : {"wojuede", "wozhegeren", "haishitinghaode"}) {
      type(raw);
      key(VK_SPACE, L' ');
    }
    key(VK_OEM_PERIOD, L'.');
    key(VK_RETURN, L'\r', 2);
    Require(delivery() == L"Translated sentence.", "Cache delivery mismatch");
    Require(ApiCount() == 1, "Cache hit called API");
    type("nihao");
    key('1', L'1');
    key(VK_OEM_COMMA, L',');
    key(VK_BACK, L'\b');
    key(VK_RETURN, L'\r');
    Require(delivery() == L"你好", "Enter did not commit edited original");
    Require(ApiCount() == 1, "Enter/number/backspace called API");
    type("nihao");
    key(VK_SPACE, L' ');
    key(VK_RETURN, L'\r', 2);
    Sleep(1200);
    Require(ApiCount() == 2, "Failure probe did not call API once");
    key(VK_RETURN, L'\r');
    Require(delivery() == L"你好", "API failure lost original");
    type("nihao");
    key(VK_SPACE, L' ');
    key(VK_RETURN, L'\r', 2);
    Require(delivery() == L"Translated sentence.", "Failure prevented retry");
    Require(ApiCount() == 3, "Failed translation was incorrectly cached");
    type("wo");
    key(VK_ESCAPE, 27);
    key(VK_ESCAPE, 27);
    type("nihao");
    key(VK_RETURN, L'\r');
    Require(delivery() == L"你好", "Esc leaked prior spelling");
    Require(ApiCount() == 3, "Esc or Enter called API");
    std::cout << "stage: long" << std::endl;
    for (int i = 0; i < 110; ++i) {
      type("nihao");
      key(VK_SPACE, L' ');
    }
    key(VK_RETURN, L'\r', 2);
    Require(delivery() == std::wstring(450, L'x'),
            "Long translated sentence was truncated");
    Require(ApiCount() == 4,
            "Long sentence did not make exactly one explicit request");
    std::cout << "stage: long" << std::endl;
    for (int i = 0; i < 110; ++i) {
      type("nihao");
      key(VK_SPACE, L' ');
    }
    key(VK_RETURN, L'\r');
    std::wstring longOriginal;
    for (int i = 0; i < 110; ++i)
      longOriginal += L"你好";
    Require(delivery() == longOriginal, "Long Chinese Enter was truncated");
    Require(ApiCount() == 4, "Long Enter called API");
    std::cout
        << "PASS: real Server segmented buffer, zero automatic requests, "
           "duplicate suppression, cache, Enter, digits, punctuation, "
           "backspace, Esc, API failure/retry and acknowledged delivery\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
