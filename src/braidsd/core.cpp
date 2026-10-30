// core.cpp
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#include "core.h"

#include <algorithm>
#include <sstream>

#include <dirent.h>

#include "appconfig.h"
#include "apputil.h"
#include "authutil.h"
#include "fileutil.h"
#include "log.h"
#include "messagecache.h"
#include "profiles.h"
#include "protocolfactory.h"
#include "scopeddirlock.h"
#include "status.h"
#include "strutil.h"
#include "timeutil.h"

// semver of the API; clients must match the major version
static const std::string s_ApiVersion = "0.1.0";

static const char* AuthStepToString(AuthStep p_Step)
{
  switch (p_Step)
  {
    case AuthStepInput: return "input";
    case AuthStepQr: return "qr";
    case AuthStepInfo: return "info";
    case AuthStepError: return "error";
    default: return "unknown";
  }
}

static bool IsDirEmpty(const std::string& p_Path)
{
  DIR* dir = opendir(p_Path.c_str());
  if (dir == nullptr) return false;

  bool isEmpty = true;
  while (struct dirent* entry = readdir(dir))
  {
    const std::string name = entry->d_name;
    if ((name != ".") && (name != ".."))
    {
      isEmpty = false;
      break;
    }
  }

  closedir(dir);
  return isEmpty;
}

Core::Core()
{
}

Core::~Core()
{
  Cleanup();
}

std::string Core::GetApiVersion()
{
  return s_ApiVersion;
}

bool Core::Init(std::string& p_Error)
{
  const std::string appDir = FileUtil::GetApplicationDir();
  static const int dirVersion = 1;
  if (!FileUtil::Exists(appDir) || IsDirEmpty(appDir))
  {
    FileUtil::InitDirVersion(appDir, dirVersion);
  }

  m_AppDirLock.reset(new ScopedDirLock(appDir));
  if (!m_AppDirLock->IsLocked())
  {
    p_Error = "unable to acquire lock for " + appDir + ", only one braidsd per config dir is supported";
    m_AppDirLock.reset();
    return false;
  }

  if (FileUtil::GetDirVersion(appDir) != dirVersion)
  {
    p_Error = "invalid config dir " + appDir;
    m_AppDirLock.reset();
    return false;
  }

  Log::Init(appDir + "/log.txt");
  LOG_INFO("%s", AppUtil::GetAppName(true /*p_WithVersion*/, true /*p_WithBranch*/).c_str());
  LOG_INFO("api version %s", s_ApiVersion.c_str());

  AppConfig::Init();
  FileUtil::SetDownloadsDir(AppConfig::GetStr("downloads_dir"));
  Profiles::Init();
  MessageCache::Init();
  FileUtil::InitTempDir();
  m_ProfilesDir = appDir + "/profiles";

  m_ChatModel.reset(new ChatModel([this](const std::string& p_Method, const json& p_Params)
  {
    Emit(p_Method, p_Params);
  },
                                  [this](const std::string& p_AccountId)
  {
    return GetProtocol(p_AccountId);
  }));

  MessageCache::SetMessageHandler([this](std::shared_ptr<ServiceMessage> p_ServiceMessage)
  {
    m_Loop.Post([this, p_ServiceMessage]() { OnProtocolMessage(p_ServiceMessage); });
  });

  Status::SetHandler([this](const std::string& p_ProfileId, uint32_t p_Flags)
  {
    m_Loop.Post([this, p_ProfileId, p_Flags]() { OnStatus(p_ProfileId, p_Flags); });
  });

  m_Loop.Start([this]() { Tick(); });
  m_Loop.Invoke([this]() { LoadAccounts(); });
  m_Initialized = true;
  return true;
}

void Core::Cleanup()
{
  if (!m_Initialized) return;

  m_Initialized = false;
  LOG_INFO("cleanup");
  Status::SetHandler(nullptr);

  // unblock and finish setups and other background work (they may post to the loop)
  std::vector<std::thread> setupThreads;
  m_Loop.Invoke([&]()
  {
    for (auto& setup : m_Setups)
    {
      setup.second.authBridge->Cancel();
    }

    for (auto& account : m_Accounts)
    {
      account.second.authBridge->Cancel();
    }
  });

  JoinBackground();

  m_Loop.Invoke([&]()
  {
    for (auto& setup : m_Setups)
    {
      setupThreads.push_back(std::move(setup.second.thread));
    }
  });

  for (auto& thread : setupThreads)
  {
    if (thread.joinable())
    {
      thread.join();
    }
  }

  m_Loop.Stop();
  JoinBackground(); // tasks run while stopping the loop may have started more
  MessageCache::SetMessageHandler(nullptr);

  for (auto& account : m_Accounts)
  {
    LOG_INFO("logout %s", account.first.c_str());
    account.second.protocol->Logout();
    account.second.protocol->CloseProfile();
  }

  for (auto& setup : m_Setups)
  {
    if (setup.second.protocol)
    {
      setup.second.protocol->Logout();
    }
  }

  m_Accounts.clear();
  m_Setups.clear();
  m_ChatModel.reset();

  FileUtil::CleanupTempDir();
  MessageCache::Cleanup();
  AppConfig::Cleanup();
  Profiles::Cleanup();
  LOG_INFO("exit");
  Log::Cleanup(false /*p_IsLogdumpEnabled*/);
  m_AppDirLock.reset();
}

void Core::SetEventSink(const EventSink& p_EventSink)
{
  std::unique_lock<std::mutex> lock(m_SinkMutex);
  m_EventSink = p_EventSink;
}

void Core::Call(const std::string& p_Method, const json& p_Params, const Reply& p_Reply)
{
  m_Loop.Post([this, p_Method, p_Params, p_Reply]()
  {
    try
    {
      Dispatch(p_Method, p_Params, p_Reply);
    }
    catch (const ApiException& e)
    {
      p_Reply.Error(e.GetCode(), e.what());
    }
    catch (const std::exception& e)
    {
      LOG_ERROR("api %s failed: %s", p_Method.c_str(), e.what());
      p_Reply.Error(ApiErrorInternal, e.what());
    }
  });
}

void Core::Dispatch(const std::string& p_Method, const json& p_Params, const Reply& p_Reply)
{
  LOG_DEBUG("api call %s", p_Method.c_str());
  const json& params = p_Params.is_null() ? json::object() : p_Params;
  if (!params.is_object())
  {
    throw ApiException(ApiErrorInvalidParams, "params must be an object");
  }

  if (p_Method == "hello")
  {
    json protocols = json::array();
    for (const auto& factory : ProtocolFactories::GetAll())
    {
      protocols.push_back(factory->GetName());
    }

    p_Reply.Ok(json{ { "apiVersion", s_ApiVersion }, { "version", AppUtil::GetAppVersion() },
                 { "protocols", protocols } });
  }
  else if (p_Method == "accounts.list")
  {
    json list = json::array();
    for (const auto& account : m_Accounts)
    {
      if (!account.second.isReady) continue;

      list.push_back(AccountToJson(account.second));
    }

    p_Reply.Ok(list);
  }
  else if (p_Method == "accounts.add")
  {
    AddAccount(ParamStr(params, "protocol"), p_Reply);
  }
  else if (p_Method == "accounts.authInput")
  {
    AuthInput(params);
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "accounts.authCancel")
  {
    AuthCancel(params);
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "accounts.remove")
  {
    RemoveAccount(ParamStr(params, "accountId"));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "chats.list")
  {
    p_Reply.Ok(m_ChatModel->GetChats(ParamStrOpt(params, "accountId"),
                                     ParamBool(params, "includeArchived", false)));
  }
  else if (p_Method == "chats.get")
  {
    p_Reply.Ok(m_ChatModel->GetChat(ParamStr(params, "accountId"), ParamStr(params, "chatId")));
  }
  else if (p_Method == "chats.create")
  {
    m_ChatModel->CreateChat(ParamStr(params, "accountId"), ParamStr(params, "userId"));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "chats.pin")
  {
    m_ChatModel->PinChat(ParamStr(params, "accountId"), ParamStr(params, "chatId"),
                         ParamBool(params, "isPinned", true));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "chats.archive")
  {
    m_ChatModel->ArchiveChat(ParamStr(params, "accountId"), ParamStr(params, "chatId"),
                             ParamBool(params, "isArchived", true));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "chats.delete")
  {
    m_ChatModel->DeleteChat(ParamStr(params, "accountId"), ParamStr(params, "chatId"));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "chats.setCurrent")
  {
    m_ChatModel->SetCurrentChat(ParamStr(params, "accountId"), ParamStr(params, "chatId"));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "contacts.list")
  {
    p_Reply.Ok(m_ChatModel->GetContacts(ParamStr(params, "accountId")));
  }
  else if (p_Method == "messages.get")
  {
    m_ChatModel->GetMessages(ParamStr(params, "accountId"), ParamStr(params, "chatId"),
                             ParamStrOpt(params, "fromMsgId"), (int)ParamInt(params, "limit", 30), p_Reply);
  }
  else if (p_Method == "messages.send")
  {
    m_ChatModel->SendMessage(ParamStr(params, "accountId"), ParamStr(params, "chatId"),
                             ParamStrOpt(params, "text"), ParamStrOpt(params, "quotedId"),
                             ParamStrOpt(params, "filePath"));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "messages.edit")
  {
    m_ChatModel->EditMessage(ParamStr(params, "accountId"), ParamStr(params, "chatId"), ParamStr(params, "msgId"),
                             ParamStr(params, "text"));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "messages.delete")
  {
    m_ChatModel->DeleteMessage(ParamStr(params, "accountId"), ParamStr(params, "chatId"),
                               ParamStr(params, "msgId"));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "messages.markRead")
  {
    m_ChatModel->MarkRead(ParamStr(params, "accountId"), ParamStr(params, "chatId"), ParamStr(params, "msgId"));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "messages.find")
  {
    m_ChatModel->FindMessage(ParamStr(params, "accountId"), ParamStr(params, "chatId"), ParamStr(params, "text"),
                             ParamStrOpt(params, "fromMsgId"), p_Reply);
  }
  else if (p_Method == "reactions.send")
  {
    m_ChatModel->SendReaction(ParamStr(params, "accountId"), ParamStr(params, "chatId"), ParamStr(params, "msgId"),
                              ParamStrOpt(params, "emoji"));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "reactions.available")
  {
    m_ChatModel->GetAvailableReactions(ParamStr(params, "accountId"), ParamStr(params, "chatId"),
                                       ParamStrOpt(params, "msgId"), p_Reply);
  }
  else if (p_Method == "files.download")
  {
    m_ChatModel->DownloadFile(ParamStr(params, "accountId"), ParamStr(params, "chatId"), ParamStr(params, "msgId"));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "typing.send")
  {
    m_ChatModel->SendTyping(ParamStr(params, "accountId"), ParamStr(params, "chatId"),
                            ParamBool(params, "isTyping", true));
    p_Reply.Ok(json::object());
  }
  else if (p_Method == "members.get")
  {
    m_ChatModel->GetMembers(ParamStr(params, "accountId"), ParamStr(params, "chatId"), p_Reply);
  }
  else
  {
    throw ApiException(ApiErrorMethodNotFound, "unknown method " + p_Method);
  }
}

void Core::Tick()
{
  if (m_ChatModel)
  {
    m_ChatModel->ProcessTimers(TimeUtil::GetCurrentTimeMSec());
  }

  // join finished setup threads (their completion was posted before they ended)
  for (auto it = m_Setups.begin(); it != m_Setups.end(); /* incremented in loop */)
  {
    if (!it->second.protocol && it->second.thread.joinable())
    {
      it->second.thread.join();
      it = m_Setups.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

// Accounts

void Core::LoadAccounts()
{
  const std::vector<std::string>& profileIds = FileUtil::ListDirNames(m_ProfilesDir);
  std::vector<std::shared_ptr<Protocol>> loginProtocols;
  for (const auto& profileId : profileIds)
  {
    if (profileId == "version") continue;

    const std::string protocolName = StrUtil::GetProtocolName(profileId);
    if (profileId.find("_") == std::string::npos)
    {
      LOG_WARNING("invalid profile name, skipping %s", profileId.c_str());
      continue;
    }

    std::shared_ptr<ProtocolBaseFactory> factory = ProtocolFactories::Get(protocolName);
    if (!factory)
    {
      LOG_WARNING("protocol %s not supported, skipping %s", protocolName.c_str(), profileId.c_str());
      continue;
    }

    std::unique_ptr<ScopedDirLock> dirLock(new ScopedDirLock(m_ProfilesDir + "/" + profileId));
    if (!dirLock->IsLocked())
    {
      LOG_WARNING("unable to acquire lock for %s, skipping", profileId.c_str());
      continue;
    }

    std::shared_ptr<Protocol> protocol = factory->Create();
    if (!protocol)
    {
      LOG_WARNING("failed to create protocol %s", protocolName.c_str());
      continue;
    }

    LOG_INFO("loading account %s", profileId.c_str());
    Account& account = m_Accounts[profileId];
    account.id = profileId;
    account.protocolName = protocolName;
    account.protocol = protocol;
    account.authBridge = MakeAuthBridge("accountId", profileId);
    account.dirLock = std::move(dirLock);
    account.isReady = true;
    account.state = "connecting";
    protocol->SetAuthHandler(account.authBridge);
    if (!protocol->LoadProfile(m_ProfilesDir, profileId))
    {
      LOG_WARNING("failed to load profile %s", profileId.c_str());
      account.state = "error";
      account.error = "Failed to load account data";
      continue;
    }

    protocol->SetMessageHandler(MakeMessageHandler(""));
    m_ChatModel->AddAccount(profileId);
    loginProtocols.push_back(protocol);
  }

  // login may block (network), so do it outside the loop
  RunBackground([loginProtocols]()
  {
    for (auto& protocol : loginProtocols)
    {
      LOG_INFO("login %s", protocol->GetProfileId().c_str());
      protocol->Login();
    }
  });
}

void Core::AddAccount(const std::string& p_ProtocolName, const Reply& p_Reply)
{
  std::shared_ptr<ProtocolBaseFactory> factory = ProtocolFactories::Get(p_ProtocolName);
  if (!factory)
  {
    throw ApiException(ApiErrorInvalidParams, "unknown protocol " + p_ProtocolName);
  }

  std::shared_ptr<Protocol> protocol = factory->Create();
  if (!protocol)
  {
    throw ApiException(ApiErrorFailed, "failed to load protocol " + p_ProtocolName);
  }

  const std::string setupId = "setup-" + std::to_string(m_NextSetupId++);
  LOG_INFO("account setup %s for %s", setupId.c_str(), p_ProtocolName.c_str());
  Setup& setup = m_Setups[setupId];
  setup.id = setupId;
  setup.protocolName = p_ProtocolName;
  setup.protocol = protocol;
  setup.authBridge = MakeAuthBridge("setupId", setupId);
  protocol->SetAuthHandler(setup.authBridge);
  protocol->SetMessageHandler(MakeMessageHandler(setupId));

  p_Reply.Ok(json{ { "setupId", setupId } });

  const std::string profilesDir = m_ProfilesDir;
  setup.thread = std::thread([this, setupId, protocol, profilesDir]()
  {
    std::string profileId;
    bool success = protocol->SetupProfile(profilesDir, profileId);
    if (success)
    {
      // idempotent for protocols that already logged in during setup
      success = protocol->Login();
    }

    m_Loop.Post([this, setupId, success, profileId]() { OnSetupDone(setupId, success, profileId); });
  });
}

void Core::BindSetup(const std::string& p_SetupId, const std::string& p_ProfileId)
{
  // protocols may send notifications before the setup has completed
  auto sit = m_Setups.find(p_SetupId);
  if ((sit == m_Setups.end()) || !sit->second.protocol) return;

  if (m_Accounts.count(p_ProfileId)) return;

  if (p_ProfileId.find('_') == std::string::npos) return; // profile id not yet determined

  LOG_DEBUG("bind setup %s to %s", p_SetupId.c_str(), p_ProfileId.c_str());
  Account& account = m_Accounts[p_ProfileId];
  account.id = p_ProfileId;
  account.protocolName = sit->second.protocolName;
  account.protocol = sit->second.protocol;
  account.authBridge = sit->second.authBridge;
  account.isReady = false;
  account.state = "connecting";
  m_ChatModel->AddAccount(p_ProfileId);
}

void Core::OnSetupDone(const std::string& p_SetupId, bool p_Success, const std::string& p_ProfileId)
{
  auto sit = m_Setups.find(p_SetupId);
  if (sit == m_Setups.end()) return;

  Setup& setup = sit->second;
  std::shared_ptr<Protocol> protocol = setup.protocol;
  setup.protocol.reset(); // marks the setup as done, the thread is joined in Tick()

  auto ait = m_Accounts.find(p_ProfileId);
  const bool isBound = (ait != m_Accounts.end()) && !ait->second.isReady;
  if (p_Success && !p_ProfileId.empty() && ((ait == m_Accounts.end()) || isBound))
  {
    LOG_INFO("account setup %s done %s", p_SetupId.c_str(), p_ProfileId.c_str());
    std::unique_ptr<ScopedDirLock> dirLock(new ScopedDirLock(m_ProfilesDir + "/" + p_ProfileId));
    if (!dirLock->IsLocked())
    {
      LOG_WARNING("unable to acquire lock for %s", p_ProfileId.c_str());
    }

    Account& account = m_Accounts[p_ProfileId];
    account.id = p_ProfileId;
    account.protocolName = setup.protocolName;
    account.protocol = protocol;
    account.authBridge = MakeAuthBridge("accountId", p_ProfileId);
    account.dirLock = std::move(dirLock);
    account.isReady = true;
    protocol->SetAuthHandler(account.authBridge);
    protocol->SetMessageHandler(MakeMessageHandler(""));
    m_ChatModel->AddAccount(p_ProfileId);

    const bool isOnline = ((Status::GetProfile(p_ProfileId) & Status::FlagOnline) != 0);
    account.state = isOnline ? "online" : "connecting";
    EmitDirect("account.added", json{ { "setupId", p_SetupId }, { "account", AccountToJson(account) } });
    return;
  }

  LOG_INFO("account setup %s failed", p_SetupId.c_str());
  if (isBound)
  {
    m_ChatModel->RemoveAccount(p_ProfileId);
    m_Accounts.erase(ait);
  }

  // remove data the failed setup left behind (never that of an existing account)
  const bool removeData = !p_ProfileId.empty() && (p_ProfileId.find('_') != std::string::npos) &&
    (m_Accounts.find(p_ProfileId) == m_Accounts.end());
  const std::string profileDir = m_ProfilesDir + "/" + p_ProfileId;
  RunBackground([protocol, removeData, profileDir, p_ProfileId]()
  {
    protocol->Logout();
    if (removeData)
    {
      LOG_DEBUG("remove data of failed setup %s", p_ProfileId.c_str());
      MessageCache::RemoveProfile(p_ProfileId);
      FileUtil::RmDir(profileDir);
    }
  });

  EmitDirect("account.setupFailed", json{ { "setupId", p_SetupId },
               { "message", "Account setup failed or was cancelled." } });
}

void Core::AuthInput(const json& p_Params)
{
  const std::string setupId = ParamStrOpt(p_Params, "setupId");
  const std::string accountId = ParamStrOpt(p_Params, "accountId");
  const std::string field = ParamStrOpt(p_Params, "field");
  std::string value = ParamStr(p_Params, "value");

  std::shared_ptr<AuthBridge> authBridge;
  if (!setupId.empty())
  {
    auto sit = m_Setups.find(setupId);
    if ((sit == m_Setups.end()) || !sit->second.protocol)
    {
      throw ApiException(ApiErrorInvalidParams, "unknown setup " + setupId);
    }

    // guard against setting up an account that already exists (some protocols
    // would reset its data), the profile id is derived from the phone number
    if (field == s_AuthFieldPhone)
    {
      std::string phone = value;
      phone.erase(std::remove_if(phone.begin(), phone.end(), [](const char& ch)
      {
        return !std::isdigit(static_cast<unsigned char>(ch)) && (ch != '+');
      }), phone.end());
      if (m_Accounts.count(sit->second.protocolName + "_" + phone))
      {
        throw ApiException(ApiErrorFailed, "this account has already been added");
      }
    }

    authBridge = sit->second.authBridge;
  }
  else if (!accountId.empty())
  {
    auto ait = m_Accounts.find(accountId);
    if (ait == m_Accounts.end())
    {
      throw ApiException(ApiErrorUnknownAccount, "unknown account " + accountId);
    }

    authBridge = ait->second.authBridge;
  }
  else
  {
    throw ApiException(ApiErrorInvalidParams, "setupId or accountId required");
  }

  const bool provided = authBridge->ProvideInput(field, value);
  AuthUtil::SecureClear(value);
  if (!provided)
  {
    throw ApiException(ApiErrorFailed, "no input pending" + (field.empty() ? "" : " for " + field));
  }
}

void Core::AuthCancel(const json& p_Params)
{
  const std::string setupId = ParamStrOpt(p_Params, "setupId");
  const std::string accountId = ParamStrOpt(p_Params, "accountId");
  if (!setupId.empty())
  {
    auto sit = m_Setups.find(setupId);
    if (sit == m_Setups.end())
    {
      throw ApiException(ApiErrorInvalidParams, "unknown setup " + setupId);
    }

    sit->second.authBridge->Cancel();
  }
  else if (!accountId.empty())
  {
    auto ait = m_Accounts.find(accountId);
    if (ait == m_Accounts.end())
    {
      throw ApiException(ApiErrorUnknownAccount, "unknown account " + accountId);
    }

    // only the current prompt is skipped, later runtime prompts are shown again
    ait->second.authBridge->Cancel();
    ait->second.authBridge->Reset();
  }
  else
  {
    throw ApiException(ApiErrorInvalidParams, "setupId or accountId required");
  }
}

void Core::RemoveAccount(const std::string& p_AccountId)
{
  auto ait = m_Accounts.find(p_AccountId);
  if ((ait == m_Accounts.end()) || !ait->second.isReady)
  {
    throw ApiException(ApiErrorUnknownAccount, "unknown account " + p_AccountId);
  }

  LOG_INFO("remove account %s", p_AccountId.c_str());
  std::shared_ptr<Protocol> protocol = ait->second.protocol;
  std::shared_ptr<ScopedDirLock> dirLock(ait->second.dirLock.release());
  ait->second.authBridge->Cancel();
  m_Accounts.erase(ait);
  m_ChatModel->RemoveAccount(p_AccountId);

  const std::string profileDir = m_ProfilesDir + "/" + p_AccountId;
  RunBackground([this, protocol, dirLock, profileDir, p_AccountId]() mutable
  {
    // @todo: also log out the device remotely where the protocol supports it
    protocol->Logout();
    protocol->CloseProfile();
    MessageCache::RemoveProfile(p_AccountId);
    dirLock.reset();
    FileUtil::RmDir(profileDir);
    m_Loop.Post([this, p_AccountId]()
    {
      EmitDirect("account.removed", json{ { "accountId", p_AccountId } });
    });
  });
}

// Notifications

void Core::OnProtocolMessage(std::shared_ptr<ServiceMessage> p_ServiceMessage)
{
  if (p_ServiceMessage->GetMessageType() == ConnectNotifyType)
  {
    auto ait = m_Accounts.find(p_ServiceMessage->profileId);
    if (ait != m_Accounts.end())
    {
      std::shared_ptr<ConnectNotify> connectNotify = std::static_pointer_cast<ConnectNotify>(p_ServiceMessage);
      if (connectNotify->success)
      {
        SetAccountState(ait->second, "online", "");
      }
      else
      {
        SetAccountState(ait->second, "error", "Connection failed");
      }
    }
  }

  if (m_ChatModel)
  {
    m_ChatModel->OnServiceMessage(p_ServiceMessage);
  }
}

void Core::OnStatus(const std::string& p_AccountId, uint32_t p_Flags)
{
  auto ait = m_Accounts.find(p_AccountId);
  if (ait == m_Accounts.end()) return;

  Account& account = ait->second;
  if (p_Flags & Status::FlagOnline)
  {
    SetAccountState(account, "online", "");
  }
  else if (p_Flags & Status::FlagConnecting)
  {
    SetAccountState(account, "connecting", account.error);
  }
  else if (account.state != "error")
  {
    SetAccountState(account, "offline", "");
  }
}

void Core::SetAccountState(Account& p_Account, const std::string& p_State, const std::string& p_Error)
{
  if ((p_Account.state == p_State) && (p_Account.error == p_Error)) return;

  p_Account.state = p_State;
  p_Account.error = p_Error;
  Emit("account.state", json{ { "accountId", p_Account.id }, { "state", p_State }, { "error", p_Error } });
}

// Helpers

std::shared_ptr<Protocol> Core::GetProtocol(const std::string& p_AccountId)
{
  auto ait = m_Accounts.find(p_AccountId);
  return (ait != m_Accounts.end()) ? ait->second.protocol : nullptr;
}

std::shared_ptr<AuthBridge> Core::MakeAuthBridge(const std::string& p_ScopeKey, const std::string& p_ScopeId)
{
  // called on protocol threads; values entered by the user are never part of events or logs
  return std::make_shared<AuthBridge>([this, p_ScopeKey, p_ScopeId](const AuthPrompt& p_Prompt)
  {
    json params = json{
      { p_ScopeKey, p_ScopeId },
      { "step", AuthStepToString(p_Prompt.step) },
      { "field", p_Prompt.field },
      { "message", p_Prompt.message },
      { "data", p_Prompt.isSecret ? std::string() : p_Prompt.data },
      { "isSecret", p_Prompt.isSecret },
      { "isOptional", p_Prompt.isOptional },
    };
    EmitDirect("account.auth", params);

    if ((p_Prompt.step == AuthStepError) && (p_ScopeKey == "accountId"))
    {
      const std::string accountId = p_ScopeId;
      const std::string message = p_Prompt.message;
      m_Loop.Post([this, accountId, message]()
      {
        auto ait = m_Accounts.find(accountId);
        if (ait != m_Accounts.end())
        {
          SetAccountState(ait->second, "error", message);
        }
      });
    }
  });
}

std::function<void(std::shared_ptr<ServiceMessage>)> Core::MakeMessageHandler(const std::string& p_SetupId)
{
  return [this, p_SetupId](std::shared_ptr<ServiceMessage> p_ServiceMessage)
  {
    m_Loop.Post([this, p_SetupId, p_ServiceMessage]()
    {
      if (!p_SetupId.empty())
      {
        BindSetup(p_SetupId, p_ServiceMessage->profileId);
      }

      OnProtocolMessage(p_ServiceMessage);
    });
  };
}

json Core::AccountToJson(const Account& p_Account)
{
  std::string displayName = p_Account.protocol->GetProfileDisplayName();
  if (displayName.empty())
  {
    // e.g. Telegram_+6511111111 -> +6511111111
    const size_t pos = p_Account.id.find('_');
    displayName = (pos != std::string::npos) ? p_Account.id.substr(pos + 1) : p_Account.id;
  }

  return json{
    { "accountId", p_Account.id },
    { "protocol", p_Account.protocolName },
    { "displayName", displayName },
    { "selfId", p_Account.protocol->GetSelfId() },
    { "state", p_Account.state },
    { "error", p_Account.error },
    { "capabilities", ChatModel::CapabilitiesToJson(p_Account.protocol, p_Account.protocolName) },
  };
}

void Core::Emit(const std::string& p_Method, const json& p_Params)
{
  // events of accounts still being set up are held back until account.added
  if (p_Params.is_object() && p_Params.contains("accountId") && p_Params["accountId"].is_string())
  {
    auto ait = m_Accounts.find(p_Params["accountId"].get<std::string>());
    if ((ait != m_Accounts.end()) && !ait->second.isReady) return;
  }

  EmitDirect(p_Method, p_Params);
}

void Core::EmitDirect(const std::string& p_Method, const json& p_Params)
{
  std::unique_lock<std::mutex> lock(m_SinkMutex);
  if (m_EventSink)
  {
    m_EventSink(p_Method, p_Params);
  }
}

void Core::JoinBackground()
{
  while (true)
  {
    std::vector<std::thread> threads;
    {
      std::unique_lock<std::mutex> lock(m_ThreadsMutex);
      threads.swap(m_Threads);
    }

    if (threads.empty()) break;

    for (auto& thread : threads)
    {
      if (thread.joinable())
      {
        thread.join();
      }
    }
  }
}

void Core::RunBackground(const std::function<void()>& p_Task)
{
  std::unique_lock<std::mutex> lock(m_ThreadsMutex);
  m_Threads.push_back(std::thread(p_Task));
}
