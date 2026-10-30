// core.h
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "api.h"
#include "authbridge.h"
#include "chatmodel.h"
#include "eventloop.h"
#include "protocol.h"

class ScopedDirLock;

// braidsd core: account manager (load/add/remove accounts, login flows) and
// the API dispatcher in front of the ChatModel. All state lives on the event
// loop thread; Call() and the event sink may be used from any thread.
class Core
{
public:
  Core();
  ~Core();

  // Returns false (with p_Error set) if the config dir cannot be used, e.g.
  // because another braidsd instance holds its lock.
  bool Init(std::string& p_Error);
  void Cleanup();

  void SetEventSink(const EventSink& p_EventSink);

  // Execute an API method; the reply may be called later, on the loop thread.
  void Call(const std::string& p_Method, const json& p_Params, const Reply& p_Reply);

  static std::string GetApiVersion();

private:
  struct Account
  {
    std::string id;
    std::string protocolName;
    std::shared_ptr<Protocol> protocol;
    std::shared_ptr<AuthBridge> authBridge;
    std::unique_ptr<ScopedDirLock> dirLock;
    bool isReady = false; // false while the account is being set up
    std::string state = "offline";
    std::string error;
  };

  struct Setup
  {
    std::string id;
    std::string protocolName;
    std::shared_ptr<Protocol> protocol;
    std::shared_ptr<AuthBridge> authBridge;
    std::thread thread;
  };

private:
  void Dispatch(const std::string& p_Method, const json& p_Params, const Reply& p_Reply);
  void Tick();

  void LoadAccounts();
  void AddAccount(const std::string& p_ProtocolName, const Reply& p_Reply);
  void OnSetupDone(const std::string& p_SetupId, bool p_Success, const std::string& p_ProfileId);
  void BindSetup(const std::string& p_SetupId, const std::string& p_ProfileId);
  void AuthInput(const json& p_Params);
  void AuthCancel(const json& p_Params);
  void RemoveAccount(const std::string& p_AccountId);

  void OnProtocolMessage(std::shared_ptr<ServiceMessage> p_ServiceMessage);
  void OnStatus(const std::string& p_AccountId, uint32_t p_Flags);
  void SetAccountState(Account& p_Account, const std::string& p_State, const std::string& p_Error);

  std::shared_ptr<Protocol> GetProtocol(const std::string& p_AccountId);
  std::shared_ptr<AuthBridge> MakeAuthBridge(const std::string& p_ScopeKey, const std::string& p_ScopeId);
  std::function<void(std::shared_ptr<ServiceMessage>)> MakeMessageHandler(const std::string& p_SetupId);
  json AccountToJson(const Account& p_Account);

  void Emit(const std::string& p_Method, const json& p_Params);
  void EmitDirect(const std::string& p_Method, const json& p_Params);
  void RunBackground(const std::function<void()>& p_Task);
  void JoinBackground();

private:
  EventLoop m_Loop;
  std::unique_ptr<ChatModel> m_ChatModel;
  std::unique_ptr<ScopedDirLock> m_AppDirLock;
  std::string m_ProfilesDir;
  std::map<std::string, Account> m_Accounts;
  std::map<std::string, Setup> m_Setups;
  int m_NextSetupId = 1;

  std::mutex m_SinkMutex;
  EventSink m_EventSink;

  std::mutex m_ThreadsMutex;
  std::vector<std::thread> m_Threads;
  bool m_Initialized = false;
};
