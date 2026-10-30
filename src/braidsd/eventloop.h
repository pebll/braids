// eventloop.h
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

// Single worker thread executing posted tasks in order. All core state
// (accounts, chat model) is only touched from this thread, so it needs no
// locking. A tick function is called at least every 100 ms for timers.
class EventLoop
{
public:
  ~EventLoop();

  void Start(const std::function<void()>& p_Tick);
  void Stop();
  void Post(const std::function<void()>& p_Task);
  bool IsLoopThread() const;

  // Post and wait for completion (must not be called from the loop thread).
  void Invoke(const std::function<void()>& p_Task);

private:
  void Process();

private:
  std::function<void()> m_Tick;
  std::thread m_Thread;
  std::mutex m_Mutex;
  std::condition_variable m_CondVar;
  std::deque<std::function<void()>> m_Queue;
  bool m_Running = false;
};
