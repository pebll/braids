// main.cpp
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

// braidsd - braids core daemon.
//
// The core (accounts, login flows, chat model) with a JSON-RPC 2.0 API on a
// Unix socket and/or stdin/stdout, see doc/ARCHITECTURE.md and doc/API.md.

#include <atomic>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

#include "api.h"
#include "apputil.h"
#include "core.h"
#include "fileutil.h"
#include "log.h"
#include "protocolfactory.h"
#include "server.h"

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
    "Serves the JSON-RPC API on a Unix socket, by default\n"
    "$XDG_RUNTIME_DIR/braids/braidsd.sock (<confdir>/braidsd.sock with --confdir\n"
    "or without XDG_RUNTIME_DIR).\n"
    "\n"
    "    -c, --confdir <DIR>    use a different directory than ~/.config/braids\n"
    "    -d, --verbose          enable verbose logging (-dd for trace logging)\n"
    "    -l, --list-protocols   load and list the enabled protocols\n"
    "    -s, --stdio            serve the API on stdin/stdout (no socket unless\n"
    "                           --socket is given), exit at end of input\n"
    "    -S, --socket <PATH>    serve the API on the Unix socket at PATH\n"
    "        --no-socket        do not serve on a socket\n"
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

static int Run(bool p_IsStdio, const std::string& p_SocketPath)
{
  Core core;
  ApiServer server(core);
  std::string error;
  if (p_IsStdio && !server.AddStdio(error))
  {
    std::cerr << "error: " << error << "\n";
    return 1;
  }

  core.SetEventSink([&server](const std::string& p_Method, const json& p_Params)
  {
    server.Broadcast(p_Method, p_Params);
  });

  if (!core.Init(error))
  {
    std::cerr << "error: " << error << "\n";
    return 1;
  }

  // the config dir lock is held now, so a stale socket can be replaced
  if (!p_SocketPath.empty() && !server.Listen(p_SocketPath, error))
  {
    std::cerr << "error: " << error << "\n";
    core.SetEventSink(nullptr);
    core.Cleanup();
    return 1;
  }

  if (p_IsStdio)
  {
    server.Broadcast("ready", json{ { "apiVersion", Core::GetApiVersion() } });
  }

  std::atomic<bool> terminate(false);
  std::thread signalWatcher([&]()
  {
    while (!terminate)
    {
      if (s_Terminate || AppUtil::IsTerminateRequested())
      {
        server.Stop();
        return;
      }

      usleep(100 * 1000);
    }
  });

  server.Run(terminate);
  terminate = true;
  signalWatcher.join();

  core.SetEventSink(nullptr);
  core.Cleanup();
  server.Close();
  return 0;
}

int main(int argc, char* argv[])
{
  // profile data and caches are owner-only
  umask(077);

  std::string confDir;
  bool isStdio = false;
  bool isNoSocket = false;
  std::string socketPath;
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
    else if (((arg == "-S") || (arg == "--socket")) && ((i + 1) < argc))
    {
      socketPath = argv[++i];
    }
    else if (arg == "--no-socket")
    {
      isNoSocket = true;
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

  if (!confDir.empty() && (confDir.at(0) != '/'))
  {
    confDir = FileUtil::GetCurrentWorkingDir() + "/" + confDir;
  }

  FileUtil::SetApplicationDir(confDir.empty() ? FileUtil::GetDefaultApplicationDir() : confDir);
  if (isNoSocket || (isStdio && socketPath.empty()))
  {
    // stdio alone, unless a socket is requested explicitly
    socketPath.clear();
  }
  else if (socketPath.empty())
  {
    socketPath = ApiServer::GetDefaultSocketPath(FileUtil::GetApplicationDir(), !confDir.empty());
  }

  if (!isStdio && socketPath.empty())
  {
    std::cerr << "error: neither --stdio nor a socket\n";
    return 1;
  }

  Log::SetVerboseLevel(verboseLevel);

  AppUtil::InitTerminateSignalHandler();
  signal(SIGINT, TerminateHandler);
  signal(SIGPIPE, SIG_IGN);

  return Run(isStdio, socketPath);
}
