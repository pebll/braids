// authutil.h
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#pragma once

#include <memory>
#include <string>

#include "protocol.h"

// Helpers for protocol authentication steps. Forwards to the given AuthHandler,
// or falls back to the terminal (stdin/stdout) when it is null, which keeps the
// protocol libraries usable without braidsd (e.g. in standalone tests).
class AuthUtil
{
public:
  static bool Input(std::shared_ptr<AuthHandler> p_AuthHandler, const AuthPrompt& p_Prompt, std::string& p_Value);
  static void Show(std::shared_ptr<AuthHandler> p_AuthHandler, const AuthPrompt& p_Prompt);

  // Convenience wrappers
  static bool Input(std::shared_ptr<AuthHandler> p_AuthHandler, const std::string& p_Field,
                    const std::string& p_Message, std::string& p_Value, bool p_IsSecret = false,
                    bool p_IsOptional = false, const std::string& p_Default = std::string());
  static void ShowQr(std::shared_ptr<AuthHandler> p_AuthHandler, const std::string& p_Message,
                     const std::string& p_Data);
  static void ShowInfo(std::shared_ptr<AuthHandler> p_AuthHandler, const std::string& p_Message,
                       const std::string& p_Data = std::string());
  static void ShowError(std::shared_ptr<AuthHandler> p_AuthHandler, const std::string& p_Message);

  // True if the authentication was cancelled through the handler.
  static bool IsCancelled(std::shared_ptr<AuthHandler> p_AuthHandler);

  // Asks for a phone number and returns it with only digits and a leading '+' kept.
  static bool InputPhoneNumber(std::shared_ptr<AuthHandler> p_AuthHandler, std::string& p_PhoneNumber);

  // Overwrites the content of a string holding a secret before it is released.
  static void SecureClear(std::string& p_Str);
};
