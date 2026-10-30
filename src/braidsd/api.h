// api.h
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#pragma once

#include <functional>
#include <stdexcept>
#include <string>

#include "nlohmann/json.hpp"

using json = nlohmann::json;

// API error codes (JSON-RPC 2.0 reserved codes, plus braids specific ones)
enum ApiErrorCode
{
  ApiErrorParseError = -32700,
  ApiErrorInvalidRequest = -32600,
  ApiErrorMethodNotFound = -32601,
  ApiErrorInvalidParams = -32602,
  ApiErrorInternal = -32603,
  ApiErrorUnknownAccount = 1,
  ApiErrorUnknownChat = 2,
  ApiErrorUnknownMessage = 3,
  ApiErrorUnsupported = 4,
  ApiErrorFailed = 5,
};

class ApiException : public std::runtime_error
{
public:
  ApiException(int p_Code, const std::string& p_Message)
    : std::runtime_error(p_Message)
    , m_Code(p_Code)
  {
  }

  int GetCode() const { return m_Code; }

private:
  int m_Code;
};

// Reply to an API call; may be invoked later (asynchronously), exactly once.
class Reply
{
public:
  using Func = std::function<void (const json& p_Result, int p_ErrorCode, const std::string& p_ErrorMessage)>;

  Reply() { }
  explicit Reply(const Func& p_Func)
    : m_Func(p_Func) { }

  void Ok(const json& p_Result) const
  {
    if (m_Func) m_Func(p_Result, 0, std::string());
  }

  void Error(int p_Code, const std::string& p_Message) const
  {
    if (m_Func) m_Func(json(), p_Code, p_Message);
  }

private:
  Func m_Func;
};

// Emits an API event (JSON-RPC notification): method and params.
using EventSink = std::function<void (const std::string& p_Method, const json& p_Params)>;

// Parameter helpers, throwing ApiErrorInvalidParams
inline std::string ParamStr(const json& p_Params, const char* p_Name)
{
  if (!p_Params.is_object() || !p_Params.contains(p_Name) || !p_Params[p_Name].is_string())
  {
    throw ApiException(ApiErrorInvalidParams, std::string("missing string param '") + p_Name + "'");
  }

  return p_Params[p_Name].get<std::string>();
}

inline std::string ParamStrOpt(const json& p_Params, const char* p_Name, const std::string& p_Default = std::string())
{
  if (!p_Params.is_object() || !p_Params.contains(p_Name) || p_Params[p_Name].is_null()) return p_Default;

  if (!p_Params[p_Name].is_string())
  {
    throw ApiException(ApiErrorInvalidParams, std::string("param '") + p_Name + "' must be a string");
  }

  return p_Params[p_Name].get<std::string>();
}

inline bool ParamBool(const json& p_Params, const char* p_Name, bool p_Default)
{
  if (!p_Params.is_object() || !p_Params.contains(p_Name) || p_Params[p_Name].is_null()) return p_Default;

  if (!p_Params[p_Name].is_boolean())
  {
    throw ApiException(ApiErrorInvalidParams, std::string("param '") + p_Name + "' must be a boolean");
  }

  return p_Params[p_Name].get<bool>();
}

inline int64_t ParamInt(const json& p_Params, const char* p_Name, int64_t p_Default)
{
  if (!p_Params.is_object() || !p_Params.contains(p_Name) || p_Params[p_Name].is_null()) return p_Default;

  if (!p_Params[p_Name].is_number_integer())
  {
    throw ApiException(ApiErrorInvalidParams, std::string("param '") + p_Name + "' must be an integer");
  }

  return p_Params[p_Name].get<int64_t>();
}
