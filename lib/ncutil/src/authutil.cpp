// authutil.cpp
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#include "authutil.h"

#include <algorithm>
#include <iostream>

#include "qrutil.h"
#include "strutil.h"

bool AuthUtil::Input(std::shared_ptr<AuthHandler> p_AuthHandler, const AuthPrompt& p_Prompt, std::string& p_Value)
{
  if (p_AuthHandler)
  {
    return p_AuthHandler->Input(p_Prompt, p_Value);
  }

  std::cout << p_Prompt.message;
  if (!p_Prompt.data.empty() && !p_Prompt.isSecret)
  {
    std::cout << " (" << p_Prompt.data << ")";
  }

  std::cout << ": ";
  std::cout.flush();
  if (p_Prompt.isSecret)
  {
    p_Value = StrUtil::GetPass();
  }
  else
  {
    std::getline(std::cin, p_Value);
  }

  if (!std::cin.good()) return false;

  StrUtil::Trim(p_Value);
  if (p_Value.empty() && !p_Prompt.isSecret)
  {
    p_Value = p_Prompt.data;
  }

  return true;
}

void AuthUtil::Show(std::shared_ptr<AuthHandler> p_AuthHandler, const AuthPrompt& p_Prompt)
{
  if (p_AuthHandler)
  {
    p_AuthHandler->Show(p_Prompt);
    return;
  }

  switch (p_Prompt.step)
  {
    case AuthStepQr:
      std::cout << "\n" << p_Prompt.message << "\n\n" << QrUtil::ToTerminalString(p_Prompt.data) << "\n";
      break;

    case AuthStepInfo:
      std::cout << "\n" << p_Prompt.message << "\n";
      if (!p_Prompt.data.empty())
      {
        std::cout << "\n    " << p_Prompt.data << "\n\n";
      }
      break;

    case AuthStepError:
      std::cout << "Error: " << p_Prompt.message << "\n";
      break;

    default:
      break;
  }

  std::cout.flush();
}

bool AuthUtil::Input(std::shared_ptr<AuthHandler> p_AuthHandler, const std::string& p_Field,
                     const std::string& p_Message, std::string& p_Value, bool p_IsSecret /*= false*/,
                     bool p_IsOptional /*= false*/, const std::string& p_Default /*= std::string()*/)
{
  AuthPrompt prompt;
  prompt.step = AuthStepInput;
  prompt.field = p_Field;
  prompt.message = p_Message;
  prompt.data = p_Default;
  prompt.isSecret = p_IsSecret;
  prompt.isOptional = p_IsOptional;
  return Input(p_AuthHandler, prompt, p_Value);
}

void AuthUtil::ShowQr(std::shared_ptr<AuthHandler> p_AuthHandler, const std::string& p_Message,
                      const std::string& p_Data)
{
  AuthPrompt prompt;
  prompt.step = AuthStepQr;
  prompt.message = p_Message;
  prompt.data = p_Data;
  Show(p_AuthHandler, prompt);
}

void AuthUtil::ShowInfo(std::shared_ptr<AuthHandler> p_AuthHandler, const std::string& p_Message,
                        const std::string& p_Data /*= std::string()*/)
{
  AuthPrompt prompt;
  prompt.step = AuthStepInfo;
  prompt.message = p_Message;
  prompt.data = p_Data;
  Show(p_AuthHandler, prompt);
}

void AuthUtil::ShowError(std::shared_ptr<AuthHandler> p_AuthHandler, const std::string& p_Message)
{
  AuthPrompt prompt;
  prompt.step = AuthStepError;
  prompt.message = p_Message;
  Show(p_AuthHandler, prompt);
}

bool AuthUtil::InputPhoneNumber(std::shared_ptr<AuthHandler> p_AuthHandler, std::string& p_PhoneNumber)
{
  std::string str;
  if (!Input(p_AuthHandler, s_AuthFieldPhone, "Phone number with country code (ex. +6511111111)", str))
  {
    return false;
  }

  str.erase(std::remove_if(str.begin(), str.end(),
                           [](const char& ch) { return !std::isdigit(static_cast<unsigned char>(ch)) && (ch != '+'); }),
            str.end());
  p_PhoneNumber = str;
  return !p_PhoneNumber.empty();
}

void AuthUtil::SecureClear(std::string& p_Str)
{
  volatile char* ptr = &p_Str[0];
  for (size_t i = 0; i < p_Str.size(); ++i)
  {
    ptr[i] = 0;
  }

  p_Str.clear();
}
