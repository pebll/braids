// authbridge.h
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

#include "protocol.h"

// AuthHandler that turns protocol auth steps into API events (account.auth) and
// waits for the answer from the API (accounts.authInput / accounts.authCancel).
// Input() is called on protocol threads and blocks; the other methods are
// thread-safe.
class AuthBridge : public AuthHandler
{
public:
  // Called for each step, with the step as event params (without the scope id).
  using StepHandler = std::function<void (const AuthPrompt& p_Prompt)>;

  explicit AuthBridge(const StepHandler& p_StepHandler);

  bool Input(const AuthPrompt& p_Prompt, std::string& p_Value) override;
  void Show(const AuthPrompt& p_Prompt) override;

  // Provide the value for the pending input. Returns false if no input with
  // this field is pending.
  bool ProvideInput(const std::string& p_Field, const std::string& p_Value);

  // Cancel the pending and all future inputs (until Reset()).
  void Cancel();
  void Reset();

  bool IsCancelled() const override;
  bool IsInputPending() const;
  std::string GetPendingField() const;

private:
  StepHandler m_StepHandler;
  mutable std::mutex m_Mutex;
  std::condition_variable m_CondVar;
  bool m_Pending = false;
  bool m_HasValue = false;
  bool m_Cancelled = false;
  std::string m_Field;
  std::string m_Value;
};
