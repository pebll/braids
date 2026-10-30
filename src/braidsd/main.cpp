// main.cpp
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// braidsd - braids core daemon.
//
// Milestone M1: the core (accounts, login flows, chat model) with a JSON-RPC
// 2.0 interface over stdin/stdout (--stdio), used as test harness. The Unix
// socket transport follows in M2, see doc/ARCHITECTURE.md and doc/API.md.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include "api.h"
#include "apputil.h"
#include "core.h"
#include "fileutil.h"
#include "log.h"
#include "protocolfactory.h"

static std::atomic<bool> s_Terminate(false);

static void TerminateHandler(int /*p_Signal*/)
{
  s_Terminate = true;
}

static void ShowHelp()
{
  std::cout <<
    "braidsd is the core daemon of braids, a multi-protocol messenger.\n"
    "\n"
    "Usage: braidsd [OPTION]\n"
    "\n"
    "    -c, --confdir <DIR>    use a different directory than ~/.config/braids\n"
    "    -d, --verbose          enable verbose logging (-dd for trace logging)\n"
    "    -l, --list-protocols   load and list the enabled protocols\n"
    "    -s, --stdio            serve the JSON-RPC API on stdin/stdout\n"
    "    -h, --help             display this help and exit\n"
    "    -v, --version          output version information and exit\n"
    "\n"
    "The API is documented in doc/API.md.\n";
}

static int ListProtocols()
{
  int failed = 0;
  for (const auto& factory : ProtocolFactories::GetAll())
  {
    std::shared_ptr<Protocol> protocol = factory->Create();
    std::cout << factory->GetName() << ": " << (protocol ? "ok" : "failed to load") << "\n";
    failed += protocol ? 0 : 1;
  }

  return (failed == 0) ? 0 : 1;
}

// Writes one JSON object per line to the API output.
class LineWriter
{
public:
  explicit LineWriter(FILE* p_File)
    : m_File(p_File) { }

  void Write(const json& p_Json)
  {
    // replace invalid utf-8 instead of throwing, protocol data is not always valid
    const std::string line = p_Json.dump(-1, ' ', false, json::error_handler_t::replace);
    std::unique_lock<std::mutex> lock(m_Mutex);
    fprintf(m_File, "%s\n", line.c_str());
    fflush(m_File);
  }

private:
  FILE* m_File;
  std::mutex m_Mutex;
};

static void HandleLine(Core& p_Core, std::shared_ptr<LineWriter> p_Writer, const std::string& p_Line)
{
  json request = json::parse(p_Line, nullptr, false);
  if (request.is_discarded() || !request.is_object())
  {
    p_Writer->Write(json{ { "jsonrpc", "2.0" }, { "id", nullptr },
                      { "error", { { "code", ApiErrorParseError }, { "message", "parse error" } } } });
    return;
  }

  const json id = request.contains("id") ? request["id"] : json();
  if (!request.contains("method") || !request["method"].is_string())
  {
    p_Writer->Write(json{ { "jsonrpc", "2.0" }, { "id", id },
                      { "error", { { "code", ApiErrorInvalidRequest }, { "message", "invalid request" } } } });
    return;
  }

  const std::string method = request["method"].get<std::string>();
  const json params = request.contains("params") ? request["params"] : json::object();
  const bool isNotification = !request.contains("id");
  p_Core.Call(method, params, Reply([p_Writer, id, isNotification](const json& p_Result, int p_ErrorCode,
                                                                   const std::string& p_ErrorMessage)
  {
    if (isNotification) return;

    if (p_ErrorCode == 0)
    {
      p_Writer->Write(json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", p_Result } });
    }
    else
    {
      p_Writer->Write(json{ { "jsonrpc", "2.0" }, { "id", id },
                        { "error", { { "code", p_ErrorCode }, { "message", p_ErrorMessage } } } });
    }
  }));
}

static int RunStdio()
{
  // keep the original stdout for the api, and send anything else written to
  // stdout (e.g. by protocol libraries) to stderr, so it cannot corrupt the stream
  const int apiFd = dup(STDOUT_FILENO);
  dup2(STDERR_FILENO, STDOUT_FILENO);
  FILE* apiFile = fdopen(apiFd, "w");
  if (apiFile == nullptr)
  {
    std::cerr << "error: unable to open api output\n";
    return 1;
  }

  std::shared_ptr<LineWriter> writer = std::make_shared<LineWriter>(apiFile);
  Core core;
  core.SetEventSink([writer](const std::string& p_Method, const json& p_Params)
  {
    writer->Write(json{ { "jsonrpc", "2.0" }, { "method", p_Method }, { "params", p_Params } });
  });

  std::string error;
  if (!core.Init(error))
  {
    std::cerr << "error: " << error << "\n";
    return 1;
  }

  writer->Write(json{ { "jsonrpc", "2.0" }, { "method", "ready" },
                  { "params", { { "apiVersion", Core::GetApiVersion() } } } });

  std::string buffer;
  while (!s_Terminate && !AppUtil::IsTerminateRequested())
  {
    struct pollfd pfd;
    pfd.fd = STDIN_FILENO;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int rv = poll(&pfd, 1, 100);
    if (rv <= 0) continue;

    char chunk[4096];
    const ssize_t len = read(STDIN_FILENO, chunk, sizeof(chunk));
    if (len <= 0) break; // eof

    buffer.append(chunk, len);
    size_t pos = 0;
    while ((pos = buffer.find('\n')) != std::string::npos)
    {
      const std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      if (line.find_first_not_of(" \t\r") == std::string::npos) continue;

      HandleLine(core, writer, line);
    }
  }

  core.SetEventSink(nullptr);
  core.Cleanup();
  fclose(apiFile);
  return 0;
}

int main(int argc, char* argv[])
{
  // profile data and caches are owner-only
  umask(077);

  std::string confDir;
  bool isStdio = false;
  bool isListProtocols = false;
  int verboseLevel = Log::INFO_LEVEL;
  for (int i = 1; i < argc; ++i)
  {
    const std::string arg(argv[i]);
    if ((arg == "-h") || (arg == "--help"))
    {
      ShowHelp();
      return 0;
    }
    else if ((arg == "-v") || (arg == "--version"))
    {
      std::cout << "braidsd " << AppUtil::GetAppVersion() << "\n";
      return 0;
    }
    else if ((arg == "-l") || (arg == "--list-protocols"))
    {
      isListProtocols = true;
    }
    else if ((arg == "-s") || (arg == "--stdio"))
    {
      isStdio = true;
    }
    else if ((arg == "-d") || (arg == "--verbose"))
    {
      verboseLevel = Log::DEBUG_LEVEL;
    }
    else if ((arg == "-dd") || (arg == "--extra-verbose"))
    {
      verboseLevel = Log::TRACE_LEVEL;
    }
    else if (((arg == "-c") || (arg == "--confdir")) && ((i + 1) < argc))
    {
      confDir = argv[++i];
    }
    else
    {
      ShowHelp();
      return 1;
    }
  }

  if (isListProtocols)
  {
    return ListProtocols();
  }

  if (!isStdio)
  {
    ShowHelp();
    return 1;
  }

  if (!confDir.empty() && (confDir.at(0) != '/'))
  {
    confDir = FileUtil::GetCurrentWorkingDir() + "/" + confDir;
  }

  FileUtil::SetApplicationDir(confDir.empty() ? FileUtil::GetDefaultApplicationDir() : confDir);
  Log::SetVerboseLevel(verboseLevel);

  AppUtil::InitTerminateSignalHandler();
  signal(SIGINT, TerminateHandler);
  signal(SIGPIPE, SIG_IGN);

  return RunStdio();
}
