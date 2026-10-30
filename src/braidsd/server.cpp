// server.cpp
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#include "server.h"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "core.h"
#include "fileutil.h"
#include "log.h"

// a client that does not read its events is dropped beyond this
static const size_t s_MaxOutBuf = 64 * 1024 * 1024;

static void SetNonBlocking(int p_Fd)
{
  const int flags = fcntl(p_Fd, F_GETFL, 0);
  fcntl(p_Fd, F_SETFL, flags | O_NONBLOCK);
}


ApiServer::ApiServer(Core& p_Core)
  : m_Core(p_Core)
  , m_Stop(false)
{
  if (pipe(m_WakeupPipe) == 0)
  {
    SetNonBlocking(m_WakeupPipe[0]);
    SetNonBlocking(m_WakeupPipe[1]);
  }
}

ApiServer::~ApiServer()
{
  Close();
  if (m_WakeupPipe[0] != -1) close(m_WakeupPipe[0]);
  if (m_WakeupPipe[1] != -1) close(m_WakeupPipe[1]);
}

std::string ApiServer::GetDefaultSocketPath(const std::string& p_ConfDir, bool p_IsCustomConfDir)
{
  // a custom config dir gets its own socket, so instances do not collide
  if (!p_IsCustomConfDir)
  {
    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    if ((runtimeDir != nullptr) && (runtimeDir[0] == '/'))
    {
      return std::string(runtimeDir) + "/braids/braidsd.sock";
    }
  }

  return p_ConfDir + "/braidsd.sock";
}

bool ApiServer::AddStdio(std::string& p_Error)
{
  // keep the original stdout for the api, and send anything else written to
  // stdout (e.g. by protocol libraries) to stderr, so it cannot corrupt the stream
  const int apiFd = dup(STDOUT_FILENO);
  if (apiFd == -1)
  {
    p_Error = "unable to open api output";
    return false;
  }

  dup2(STDERR_FILENO, STDOUT_FILENO);
  SetNonBlocking(apiFd);

  std::shared_ptr<Connection> connection = std::make_shared<Connection>();
  connection->inFd = STDIN_FILENO;
  connection->outFd = apiFd;
  connection->isStdio = true;
  std::unique_lock<std::mutex> lock(m_Mutex);
  m_Connections.push_back(connection);
  return true;
}

bool ApiServer::Listen(const std::string& p_Path, std::string& p_Error)
{
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  if (p_Path.size() >= sizeof(addr.sun_path))
  {
    p_Error = "socket path too long: " + p_Path;
    return false;
  }

  strncpy(addr.sun_path, p_Path.c_str(), sizeof(addr.sun_path) - 1);
  FileUtil::MkDir(FileUtil::DirName(p_Path));

  // another daemon listening there (e.g. with another config dir)?
  int probeFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if ((probeFd != -1) && (connect(probeFd, (struct sockaddr*)&addr, sizeof(addr)) == 0))
  {
    close(probeFd);
    p_Error = "another braidsd is listening on " + p_Path;
    return false;
  }

  if (probeFd != -1) close(probeFd);

  // stale socket file from a daemon that did not exit cleanly
  unlink(p_Path.c_str());

  m_ListenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (m_ListenFd == -1)
  {
    p_Error = std::string("socket: ") + strerror(errno);
    return false;
  }

  const mode_t oldMask = umask(077);
  const int rv = bind(m_ListenFd, (struct sockaddr*)&addr, sizeof(addr));
  umask(oldMask);
  if ((rv != 0) || (listen(m_ListenFd, 16) != 0))
  {
    p_Error = "unable to listen on " + p_Path + ": " + strerror(errno);
    close(m_ListenFd);
    m_ListenFd = -1;
    return false;
  }

  chmod(p_Path.c_str(), 0600);
  SetNonBlocking(m_ListenFd);
  m_SocketPath = p_Path;
  LOG_INFO("listening on %s", p_Path.c_str());
  return true;
}

void ApiServer::Broadcast(const std::string& p_Method, const json& p_Params)
{
  const json event = json{ { "jsonrpc", "2.0" }, { "method", p_Method }, { "params", p_Params } };
  std::vector<std::shared_ptr<Connection>> connections;
  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    connections = m_Connections;
  }

  if (connections.empty()) return;

  // serialize once for all connections
  const std::string line = event.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
  for (auto& connection : connections)
  {
    std::unique_lock<std::mutex> lock(connection->outMutex);
    if (connection->isClosed) continue;

    connection->outBuf += line;
  }

  Wakeup();
}

void ApiServer::Write(std::shared_ptr<Connection> p_Connection, const json& p_Json)
{
  // replace invalid utf-8 instead of throwing, protocol data is not always valid
  const std::string line = p_Json.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
  {
    std::unique_lock<std::mutex> lock(p_Connection->outMutex);
    if (p_Connection->isClosed) return;

    p_Connection->outBuf += line;
  }

  Wakeup();
}

void ApiServer::Wakeup()
{
  if (m_WakeupPipe[1] == -1) return;

  const char c = 0;
  ssize_t rv = write(m_WakeupPipe[1], &c, 1);
  (void)rv; // full pipe means a wakeup is pending anyway
}

void ApiServer::Stop()
{
  m_Stop = true;
  Wakeup();
}

void ApiServer::Run(const std::atomic<bool>& p_Terminate)
{
  while (!m_Stop && !p_Terminate)
  {
    std::vector<std::shared_ptr<Connection>> connections;
    {
      std::unique_lock<std::mutex> lock(m_Mutex);
      connections = m_Connections;
    }

    std::vector<struct pollfd> pfds;
    pfds.push_back({ m_WakeupPipe[0], POLLIN, 0 });
    pfds.push_back({ m_ListenFd, POLLIN, 0 }); // fd -1 is ignored by poll
    for (auto& connection : connections)
    {
      bool hasOutput = false;
      bool isOverflow = false;
      {
        std::unique_lock<std::mutex> lock(connection->outMutex);
        hasOutput = !connection->outBuf.empty();
        isOverflow = !connection->isStdio && (connection->outBuf.size() > s_MaxOutBuf);
      }

      if (isOverflow)
      {
        LOG_WARNING("client not reading, disconnecting");
        CloseConnection(connection);
        pfds.push_back({ -1, 0, 0 });
        pfds.push_back({ -1, 0, 0 });
        continue;
      }

      pfds.push_back({ connection->inFd, POLLIN, 0 });
      if (connection->outFd == connection->inFd)
      {
        if (hasOutput) pfds.back().events |= POLLOUT;
        pfds.push_back({ -1, 0, 0 });
      }
      else
      {
        pfds.push_back({ hasOutput ? connection->outFd : -1, POLLOUT, 0 });
      }
    }

    const int rv = poll(pfds.data(), pfds.size(), 100);
    if (rv <= 0) continue;

    if (pfds[0].revents & POLLIN)
    {
      char buf[256];
      while (read(m_WakeupPipe[0], buf, sizeof(buf)) > 0)
      {
      }
    }

    if (pfds[1].revents & POLLIN)
    {
      Accept();
    }

    for (size_t i = 0; i < connections.size(); ++i)
    {
      std::shared_ptr<Connection> connection = connections[i];
      const struct pollfd& inPfd = pfds[2 + (i * 2)];
      const struct pollfd& outPfd = pfds[2 + (i * 2) + 1];
      if ((inPfd.revents & POLLOUT) || (outPfd.revents & (POLLOUT | POLLERR | POLLHUP)))
      {
        FlushOutput(connection);
      }

      if (inPfd.revents & (POLLIN | POLLHUP | POLLERR))
      {
        if (!ReadInput(connection))
        {
          if (connection->isStdio)
          {
            // end of input stops the daemon in stdio mode
            m_Stop = true;
          }
          else
          {
            CloseConnection(connection);
          }
        }
      }
    }
  }
}

void ApiServer::Accept()
{
  while (true)
  {
    const int fd = accept4(m_ListenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd == -1) return;

    std::shared_ptr<Connection> connection = std::make_shared<Connection>();
    connection->inFd = fd;
    connection->outFd = fd;
    // ready is the first line, before any event
    connection->outBuf = json{ { "jsonrpc", "2.0" }, { "method", "ready" },
      { "params", { { "apiVersion", Core::GetApiVersion() } } } }.dump() + "\n";
    {
      std::unique_lock<std::mutex> lock(m_Mutex);
      m_Connections.push_back(connection);
    }

    LOG_DEBUG("client connected");
  }
}

bool ApiServer::ReadInput(std::shared_ptr<Connection> p_Connection)
{
  char chunk[16384];
  const ssize_t len = read(p_Connection->inFd, chunk, sizeof(chunk));
  if (len == 0) return false; // eof

  if (len < 0) return (errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR);

  p_Connection->inBuf.append(chunk, len);
  size_t pos = 0;
  while ((pos = p_Connection->inBuf.find('\n')) != std::string::npos)
  {
    const std::string line = p_Connection->inBuf.substr(0, pos);
    p_Connection->inBuf.erase(0, pos + 1);
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;

    HandleLine(p_Connection, line);
  }

  return true;
}

void ApiServer::FlushOutput(std::shared_ptr<Connection> p_Connection)
{
  std::unique_lock<std::mutex> lock(p_Connection->outMutex);
  while (!p_Connection->outBuf.empty())
  {
    const ssize_t len = p_Connection->isStdio
      ? write(p_Connection->outFd, p_Connection->outBuf.data(), p_Connection->outBuf.size())
      : send(p_Connection->outFd, p_Connection->outBuf.data(), p_Connection->outBuf.size(), MSG_NOSIGNAL);
    if (len > 0)
    {
      p_Connection->outBuf.erase(0, len);
    }
    else if ((len < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK) || (errno == EINTR)))
    {
      return;
    }
    else
    {
      // peer gone, the read side reports eof
      p_Connection->outBuf.clear();
      return;
    }
  }
}

void ApiServer::HandleLine(std::shared_ptr<Connection> p_Connection, const std::string& p_Line)
{
  json request = json::parse(p_Line, nullptr, false);
  if (request.is_discarded() || !request.is_object())
  {
    Write(p_Connection, json{ { "jsonrpc", "2.0" }, { "id", nullptr },
            { "error", { { "code", ApiErrorParseError }, { "message", "parse error" } } } });
    return;
  }

  const json id = request.contains("id") ? request["id"] : json();
  if (!request.contains("method") || !request["method"].is_string())
  {
    Write(p_Connection, json{ { "jsonrpc", "2.0" }, { "id", id },
            { "error", { { "code", ApiErrorInvalidRequest }, { "message", "invalid request" } } } });
    return;
  }

  const std::string method = request["method"].get<std::string>();
  const json params = request.contains("params") ? request["params"] : json::object();
  const bool isNotification = !request.contains("id");
  std::weak_ptr<Connection> weakConnection = p_Connection;
  Reply reply([this, weakConnection, id, isNotification](const json& p_Result, int p_ErrorCode,
                                                         const std::string& p_ErrorMessage)
  {
    if (isNotification) return;

    std::shared_ptr<Connection> connection = weakConnection.lock();
    if (!connection) return;

    if (p_ErrorCode == 0)
    {
      Write(connection, json{ { "jsonrpc", "2.0" }, { "id", id }, { "result", p_Result } });
    }
    else
    {
      Write(connection, json{ { "jsonrpc", "2.0" }, { "id", id },
              { "error", { { "code", p_ErrorCode }, { "message", p_ErrorMessage } } } });
    }
  });

  if (method == "daemon.shutdown")
  {
    LOG_INFO("shutdown requested");
    reply.Ok(json::object());
    Stop();
    return;
  }

  m_Core.Call(method, params, reply);
}

void ApiServer::CloseConnection(std::shared_ptr<Connection> p_Connection)
{
  {
    std::unique_lock<std::mutex> lock(p_Connection->outMutex);
    if (p_Connection->isClosed) return;

    p_Connection->isClosed = true;
    p_Connection->outBuf.clear();
  }

  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_Connections.erase(std::remove(m_Connections.begin(), m_Connections.end(), p_Connection),
                        m_Connections.end());
  }

  if (!p_Connection->isStdio)
  {
    close(p_Connection->inFd);
    LOG_DEBUG("client disconnected");
  }
}

void ApiServer::Close()
{
  std::vector<std::shared_ptr<Connection>> connections;
  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    connections = m_Connections;
  }

  for (auto& connection : connections)
  {
    // best effort: deliver what is pending (e.g. the shutdown reply), within 2 s
    for (int i = 0; i < 20; ++i)
    {
      FlushOutput(connection);
      {
        std::unique_lock<std::mutex> lock(connection->outMutex);
        if (connection->outBuf.empty()) break;
      }

      struct pollfd pfd = { connection->outFd, POLLOUT, 0 };
      poll(&pfd, 1, 100);
    }

    if (connection->isStdio)
    {
      std::unique_lock<std::mutex> lock(connection->outMutex);
      connection->isClosed = true;
      close(connection->outFd);
    }

    CloseConnection(connection);
  }

  if (m_ListenFd != -1)
  {
    close(m_ListenFd);
    m_ListenFd = -1;
    unlink(m_SocketPath.c_str());
  }
}
