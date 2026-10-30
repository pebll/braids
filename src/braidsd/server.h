// server.h
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "api.h"

class Core;

// JSON-RPC 2.0 transport for the Core: one JSON object per line, over
// stdin/stdout (one connection) and/or a Unix socket (any number of
// connections). Replies go to the requesting connection, events to all.
// Output is buffered per connection, so a slow client never blocks the core.
class ApiServer
{
public:
  explicit ApiServer(Core& p_Core);
  ~ApiServer();

  // Serve on stdin/stdout; the server stops at end of input.
  bool AddStdio(std::string& p_Error);

  // Serve on a Unix socket at p_Path (created 0600). Fails if another
  // daemon is listening there.
  bool Listen(const std::string& p_Path, std::string& p_Error);

  // Send an event to all connections; thread safe.
  void Broadcast(const std::string& p_Method, const json& p_Params);

  // Process connections until Stop() or the terminate flag is set.
  void Run(const std::atomic<bool>& p_Terminate);
  void Stop();

  // Blocking flush of pending output, then close all connections.
  void Close();

  static std::string GetDefaultSocketPath(const std::string& p_ConfDir, bool p_IsCustomConfDir);

private:
  struct Connection
  {
    int inFd = -1;
    int outFd = -1;
    bool isStdio = false;
    std::string inBuf;
    std::mutex outMutex;
    std::string outBuf;
    bool isClosed = false;
  };

  void Accept();
  bool ReadInput(std::shared_ptr<Connection> p_Connection);
  void FlushOutput(std::shared_ptr<Connection> p_Connection);
  void HandleLine(std::shared_ptr<Connection> p_Connection, const std::string& p_Line);
  void Write(std::shared_ptr<Connection> p_Connection, const json& p_Json);
  void CloseConnection(std::shared_ptr<Connection> p_Connection);
  void Wakeup();

private:
  Core& m_Core;
  int m_ListenFd = -1;
  std::string m_SocketPath;
  int m_WakeupPipe[2] = { -1, -1 };
  std::atomic<bool> m_Stop;
  std::mutex m_Mutex;
  std::vector<std::shared_ptr<Connection>> m_Connections;
};
