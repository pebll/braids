// eventloop.cpp
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#include "eventloop.h"

#include <chrono>
#include <future>

#include "log.h"

EventLoop::~EventLoop()
{
  Stop();
}

void EventLoop::Start(const std::function<void()>& p_Tick)
{
  m_Tick = p_Tick;
  m_Running = true;
  m_Thread = std::thread(&EventLoop::Process, this);
}

void EventLoop::Stop()
{
  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_Running = false;
    m_CondVar.notify_all();
  }

  if (m_Thread.joinable())
  {
    m_Thread.join();
  }
}

void EventLoop::Post(const std::function<void()>& p_Task)
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  m_Queue.push_back(p_Task);
  m_CondVar.notify_one();
}

bool EventLoop::IsLoopThread() const
{
  return std::this_thread::get_id() == m_Thread.get_id();
}

void EventLoop::Invoke(const std::function<void()>& p_Task)
{
  std::promise<void> done;
  Post([&]()
  {
    p_Task();
    done.set_value();
  });
  done.get_future().wait();
}

void EventLoop::Process()
{
  while (true)
  {
    std::deque<std::function<void()>> tasks;
    {
      std::unique_lock<std::mutex> lock(m_Mutex);
      if (m_Queue.empty() && m_Running)
      {
        m_CondVar.wait_for(lock, std::chrono::milliseconds(100));
      }

      if (!m_Running && m_Queue.empty()) break;

      tasks.swap(m_Queue);
    }

    for (auto& task : tasks)
    {
      try
      {
        task();
      }
      catch (const std::exception& e)
      {
        LOG_ERROR("event loop task failed: %s", e.what());
      }
    }

    if (m_Tick)
    {
      m_Tick();
    }
  }
}
