// authbridge.cpp
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#include "authbridge.h"

#include "authutil.h"
#include "log.h"

AuthBridge::AuthBridge(const StepHandler& p_StepHandler)
  : m_StepHandler(p_StepHandler)
{
}

bool AuthBridge::Input(const AuthPrompt& p_Prompt, std::string& p_Value)
{
  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    if (m_Cancelled) return false;

    // a protocol asks for one input at a time; a new request replaces a stale one
    m_Pending = true;
    m_HasValue = false;
    m_Field = p_Prompt.field;
    AuthUtil::SecureClear(m_Value);
  }

  LOG_DEBUG("auth input requested %s", p_Prompt.field.c_str());
  m_StepHandler(p_Prompt);

  std::unique_lock<std::mutex> lock(m_Mutex);
  m_CondVar.wait(lock, [this]() { return m_HasValue || m_Cancelled; });
  m_Pending = false;
  if (m_Cancelled)
  {
    LOG_DEBUG("auth input cancelled %s", p_Prompt.field.c_str());
    return false;
  }

  p_Value = m_Value;
  AuthUtil::SecureClear(m_Value);
  m_HasValue = false;
  return true;
}

void AuthBridge::Show(const AuthPrompt& p_Prompt)
{
  m_StepHandler(p_Prompt);
}

bool AuthBridge::ProvideInput(const std::string& p_Field, const std::string& p_Value)
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  if (!m_Pending || m_HasValue || (!p_Field.empty() && (p_Field != m_Field))) return false;

  m_Value = p_Value;
  m_HasValue = true;
  m_CondVar.notify_all();
  return true;
}

void AuthBridge::Cancel()
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  m_Cancelled = true;
  m_CondVar.notify_all();
}

void AuthBridge::Reset()
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  m_Cancelled = false;
}

bool AuthBridge::IsCancelled() const
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  return m_Cancelled;
}

bool AuthBridge::IsInputPending() const
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  return m_Pending && !m_HasValue;
}

std::string AuthBridge::GetPendingField() const
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  return (m_Pending && !m_HasValue) ? m_Field : std::string();
}
