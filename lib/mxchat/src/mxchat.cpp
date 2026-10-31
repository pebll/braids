// mxchat.cpp
//
// Copyright (c) 2026 nchat contributors
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "mxchat.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>

#include <sys/stat.h>

#include "appconfig.h"
#include "authutil.h"
#include "fileutil.h"
#include "libcgomx.h"
#include "log.h"
#include "messagecache.h"
#include "protocolutil.h"
#include "status.h"
#include "strutil.h"
#include "timeutil.h"

// keep in sync with vars in gomx.go
enum NotifyType
{
  NotifyDirect = 0,
  NotifyCache = 1,
  NotifySendCached = 2,
};

std::mutex MxChat::s_ConnIdMapMutex;
std::map<int, MxChat*> MxChat::s_ConnIdMap;

static std::string TakeCString(char* p_Str)
{
  std::string str = (p_Str != nullptr) ? std::string(p_Str) : std::string();
  free(p_Str);
  return str;
}

static void SecureClear(std::string& p_Str)
{
  AuthUtil::SecureClear(p_Str);
}

extern "C" MxChat* CreateMxChat()
{
  return new MxChat();
}

MxChat::MxChat()
{
  m_ProfileId = GetName();
}

MxChat::~MxChat()
{
}

std::string MxChat::GetProfileId() const
{
  return m_ProfileId;
}

std::string MxChat::GetProfileDisplayName() const
{
  return m_Config.Get("profile_display_name");
}

bool MxChat::HasFeature(ProtocolFeature p_ProtocolFeature) const
{
  // Matrix has no edit time limit; two days is the most permissive option the ui offers.
  static int customFeatures =
    FeatureEditMessagesAnytime |
    FeatureAutoGetContactsOnLogin |
    FeaturePinChat |
    FeatureAvatars | FeatureDescriptions | FeatureJoinChat;
  return (p_ProtocolFeature & customFeatures);
}

bool MxChat::IsGroupChat(const std::string& p_ChatId) const
{
  if (m_ConnId == -1) return true;

  return (CMxIsGroupChat(m_ConnId, const_cast<char*>(p_ChatId.c_str())) == 1);
}

std::string MxChat::GetSelfId() const
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  return m_SelfUserId;
}

std::string MxChat::SanitizeProfileId(const std::string& p_UserId)
{
  // @alice:example.org -> alice_example.org
  std::string str = p_UserId;
  if (!str.empty() && (str[0] == '@'))
  {
    str = str.substr(1);
  }

  for (auto& ch : str)
  {
    if (!std::isalnum(static_cast<unsigned char>(ch)) && (ch != '.') && (ch != '-') && (ch != '_'))
    {
      ch = '_';
    }
  }

  return str;
}

bool MxChat::SetupProfile(const std::string& p_ProfilesDir, std::string& p_ProfileId)
{
  m_IsSetup = true;

  std::string userIdInput;
  if (!AuthUtil::Input(m_AuthHandler, s_AuthFieldUserId, "Matrix user ID (ex. @alice:matrix.org)", userIdInput))
  {
    m_IsSetup = false;
    return false;
  }

  const std::string userId = TakeCString(CMxNormalizeUserId(const_cast<char*>(userIdInput.c_str())));
  if (userId.empty())
  {
    AuthUtil::ShowError(m_AuthHandler, "Invalid user ID, expected format @user:server (ex. @alice:matrix.org).");
    m_IsSetup = false;
    return false;
  }

  AuthUtil::ShowInfo(m_AuthHandler, "Looking up homeserver...");
  const std::string discovered = TakeCString(CMxDiscoverHomeserver(const_cast<char*>(userId.c_str())));
  std::string homeserver;
  if (!AuthUtil::Input(m_AuthHandler, s_AuthFieldHomeserver, "Homeserver URL", homeserver,
                       false /*p_IsSecret*/, true /*p_IsOptional*/, discovered))
  {
    m_IsSetup = false;
    return false;
  }

  StrUtil::Trim(homeserver);
  if (homeserver.empty())
  {
    homeserver = discovered;
  }

  std::string password;
  if (!AuthUtil::Input(m_AuthHandler, s_AuthFieldPassword, "Password", password, true /*p_IsSecret*/))
  {
    m_IsSetup = false;
    return false;
  }

  m_ProfileId = GetName() + "_" + SanitizeProfileId(userId);
  m_ProfileDir = p_ProfilesDir + "/" + m_ProfileId;
  if (FileUtil::Exists(m_ProfileDir))
  {
    AuthUtil::ShowError(m_AuthHandler, "Account " + userId + " already exists. Remove it first.");
    SecureClear(password);
    m_IsSetup = false;
    return false;
  }

  mkdir(m_ProfileDir.c_str(), 0700);

  m_ConnId = CMxInit(const_cast<char*>(m_ProfileDir.c_str()));
  AddInstance(m_ConnId, this);

  AuthUtil::ShowInfo(m_AuthHandler, "Logging in...");
  const std::string loginError =
    TakeCString(CMxSetupLogin(m_ConnId, const_cast<char*>(homeserver.c_str()),
                              const_cast<char*>(userId.c_str()), const_cast<char*>(password.c_str())));
  SecureClear(password);
  if (!loginError.empty())
  {
    AuthUtil::ShowError(m_AuthHandler, loginError);
    CMxCleanup(m_ConnId);
    RemoveInstance(m_ConnId);
    m_ConnId = -1;
    FileUtil::RmDir(m_ProfileDir);
    m_IsSetup = false;
    return false;
  }

  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_SelfUserId = TakeCString(CMxGetSelfId(m_ConnId));
  }

  MessageCache::AddProfile(m_ProfileId, true /*p_CheckSequence*/, s_CacheDirVersion, true /*p_IsSetup*/,
                           false /*p_AllowReadOnly*/);
  InitConfig();
  p_ProfileId = m_ProfileId;

  if (!Login())
  {
    m_IsSetup = false;
    return false;
  }

  AuthUtil::ShowInfo(m_AuthHandler, "Setting up encryption...");
  if (CMxWaitReady(m_ConnId, 60) == 0)
  {
    if (CMxNeedsRecovery(m_ConnId) == 1)
    {
      RunRecoveryPrompt(true /*p_IsSetup*/);
    }
  }
  else
  {
    AuthUtil::ShowInfo(m_AuthHandler, "Encryption setup not completed yet, it will be retried in the background.");
  }

  m_IsSetup = false;
  return true;
}

bool MxChat::LoadProfile(const std::string& p_ProfilesDir, const std::string& p_ProfileId)
{
  LOG_INFO("load matrix profile");

  if (!p_ProfilesDir.empty() && !p_ProfileId.empty())
  {
    m_ProfileDir = p_ProfilesDir + "/" + p_ProfileId;
    m_ProfileId = p_ProfileId;
  }

  MessageCache::AddProfile(m_ProfileId, true /*p_CheckSequence*/, s_CacheDirVersion, false /*p_IsSetup*/,
                           false /*p_AllowReadOnly*/);

  m_ConnId = CMxInit(const_cast<char*>(m_ProfileDir.c_str()));
  if (m_ConnId == -1) return false;

  AddInstance(m_ConnId, this);

  {
    std::unique_lock<std::mutex> lock(m_Mutex);
    m_SelfUserId = TakeCString(CMxGetSelfId(m_ConnId));
  }

  InitConfig();
  return true;
}

bool MxChat::CloseProfile()
{
  int rv = CMxCleanup(m_ConnId);
  RemoveInstance(m_ConnId);
  m_ConnId = -1;
  CleanupConfig();
  return (rv == 0);
}

void MxChat::InitConfig()
{
  const std::map<std::string, std::string> defaultConfig =
  {
    { "profile_display_name", "" },
    { "recovery_prompt", "1" },
  };
  const std::string configPath(m_ProfileDir + std::string("/matrix.conf"));
  m_Config = Config(configPath, defaultConfig);
}

void MxChat::CleanupConfig()
{
  m_Config.Save();
}

bool MxChat::Login()
{
  int rv = 0;
  if (!m_Running)
  {
    m_Running = true;
    m_Thread = std::thread(&MxChat::Process, this);

    rv = CMxLogin(m_ConnId);

    std::shared_ptr<ConnectNotify> connectNotify = std::make_shared<ConnectNotify>(m_ProfileId);
    connectNotify->success = (rv == 0);
    m_WasOnline = connectNotify->success;

    std::shared_ptr<DeferNotifyRequest> deferNotifyRequest = std::make_shared<DeferNotifyRequest>();
    deferNotifyRequest->serviceMessage = connectNotify;
    SendRequest(deferNotifyRequest);
  }

  return (rv == 0);
}

bool MxChat::Unlink()
{
  if (!m_Running) return false;

  return (CMxUnlink(m_ConnId) == 0);
}

bool MxChat::Logout()
{
  int rv = 0;
  if (m_Running)
  {
    // stops the sync loop only, the session (access token) is kept for next start
    rv = CMxLogout(m_ConnId);
    Status::Clear(m_ProfileId, Status::FlagOnline);

    std::unique_lock<std::mutex> lock(m_ProcessMutex);
    m_Running = false;
    m_ProcessCondVar.notify_one();
  }

  if (m_Thread.joinable())
  {
    m_Thread.join();
  }

  return (rv == 0);
}

void MxChat::Process()
{
  while (m_Running)
  {
    std::shared_ptr<RequestMessage> requestMessage;

    {
      std::unique_lock<std::mutex> lock(m_ProcessMutex);
      while (m_RequestsQueue.empty() && m_Running)
      {
        m_ProcessCondVar.wait(lock);
      }

      if (!m_Running)
      {
        break;
      }

      if (!m_MessageHandler)
      {
        m_ProcessCondVar.wait(lock);
        continue;
      }

      requestMessage = m_RequestsQueue.front();
      m_RequestsQueue.pop_front();
    }

    PerformRequest(requestMessage);
  }
}

void MxChat::SendRequest(std::shared_ptr<RequestMessage> p_RequestMessage)
{
  std::unique_lock<std::mutex> lock(m_ProcessMutex);
  m_RequestsQueue.push_back(p_RequestMessage);
  m_ProcessCondVar.notify_one();
}

void MxChat::SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>& p_MessageHandler)
{
  m_MessageHandler = p_MessageHandler;
  m_ProcessCondVar.notify_one();
}

void MxChat::CallMessageHandler(std::shared_ptr<ServiceMessage> p_ServiceMessage)
{
  MessageCache::AddFromServiceMessage(m_ProfileId, p_ServiceMessage);

  if (!m_MessageHandler)
  {
    LOG_DEBUG("message handler not set");
    return;
  }

  m_MessageHandler(p_ServiceMessage);
}

void MxChat::PerformRequest(std::shared_ptr<RequestMessage> p_RequestMessage)
{
  bool requestIntervalDelay = true;

  switch (p_RequestMessage->GetMessageType())
  {
    case GetContactsRequestType:
      {
        LOG_DEBUG("get contacts");
        CMxGetContacts(m_ConnId);
      }
      break;

    case GetChatsRequestType:
      {
        LOG_DEBUG("get chats");
        std::shared_ptr<GetChatsRequest> getChatsRequest =
          std::static_pointer_cast<GetChatsRequest>(p_RequestMessage);
        MessageCache::FetchChats(m_ProfileId, getChatsRequest->chatIds);
        if (getChatsRequest->chatIds.empty())
        {
          CMxGetChats(m_ConnId);
        }
      }
      break;

    case GetStatusRequestType:
      {
        std::shared_ptr<GetStatusRequest> getStatusRequest =
          std::static_pointer_cast<GetStatusRequest>(p_RequestMessage);
        CMxGetStatus(m_ConnId, const_cast<char*>(getStatusRequest->userId.c_str()));
      }
      break;

    case GetMessageRequestType:
      {
        LOG_DEBUG("get message");
        std::shared_ptr<GetMessageRequest> getMessageRequest =
          std::static_pointer_cast<GetMessageRequest>(p_RequestMessage);
        MessageCache::FetchOneMessage(m_ProfileId, getMessageRequest->chatId, getMessageRequest->msgId,
                                      false /*p_Sync*/);
      }
      break;

    case GetMessagesRequestType:
      {
        LOG_DEBUG("get messages");
        std::shared_ptr<GetMessagesRequest> getMessagesRequest =
          std::static_pointer_cast<GetMessagesRequest>(p_RequestMessage);
        if (MessageCache::FetchMessagesFrom(m_ProfileId, getMessagesRequest->chatId,
                                            getMessagesRequest->fromMsgId,
                                            getMessagesRequest->limit, false /* p_Sync */))
        {
          break;
        }

        int rv = CMxGetMessages(m_ConnId, const_cast<char*>(getMessagesRequest->chatId.c_str()),
                                const_cast<char*>(getMessagesRequest->fromMsgId.c_str()),
                                getMessagesRequest->limit);
        if (rv != 0)
        {
          LOG_DEBUG("get messages not possible now");
        }
      }
      break;

    case SendMessageRequestType:
      {
        LOG_DEBUG("send message");
        Status::Set(m_ProfileId, Status::FlagSending);
        std::shared_ptr<SendMessageRequest> sendMessageRequest =
          std::static_pointer_cast<SendMessageRequest>(p_RequestMessage);
        const ChatMessage& chatMessage = sendMessageRequest->chatMessage;
        std::string filePath;
        std::string fileType;
        if (!chatMessage.fileInfo.empty())
        {
          FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(chatMessage.fileInfo);
          filePath = fileInfo.filePath;
          fileType = fileInfo.fileType;
        }

        std::string editMsgId;
        std::string mentionsJson = ProtocolUtil::MentionsToJson(chatMessage.mentions);
        int rv = CMxSendMessage(m_ConnId, const_cast<char*>(sendMessageRequest->chatId.c_str()),
                                const_cast<char*>(chatMessage.text.c_str()),
                                const_cast<char*>(chatMessage.quotedId.c_str()),
                                const_cast<char*>(filePath.c_str()), const_cast<char*>(fileType.c_str()),
                                const_cast<char*>(editMsgId.c_str()), const_cast<char*>(mentionsJson.c_str()));
        Status::Clear(m_ProfileId, Status::FlagSending);

        std::shared_ptr<SendMessageNotify> sendMessageNotify = std::make_shared<SendMessageNotify>(m_ProfileId);
        sendMessageNotify->success = (rv == 0);
        sendMessageNotify->chatId = sendMessageRequest->chatId;
        sendMessageNotify->chatMessage = sendMessageRequest->chatMessage;
        CallMessageHandler(sendMessageNotify);
      }
      break;

    case EditMessageRequestType:
      {
        LOG_DEBUG("edit message");
        Status::Set(m_ProfileId, Status::FlagSending);
        std::shared_ptr<EditMessageRequest> editMessageRequest =
          std::static_pointer_cast<EditMessageRequest>(p_RequestMessage);
        const ChatMessage& chatMessage = editMessageRequest->chatMessage;
        std::string empty;
        std::string mentionsJson = ProtocolUtil::MentionsToJson(chatMessage.mentions);
        CMxSendMessage(m_ConnId, const_cast<char*>(editMessageRequest->chatId.c_str()),
                       const_cast<char*>(chatMessage.text.c_str()), const_cast<char*>(empty.c_str()),
                       const_cast<char*>(empty.c_str()), const_cast<char*>(empty.c_str()),
                       const_cast<char*>(editMessageRequest->msgId.c_str()),
                       const_cast<char*>(mentionsJson.c_str()));
        Status::Clear(m_ProfileId, Status::FlagSending);
      }
      break;

    case MarkMessageReadRequestType:
      {
        LOG_DEBUG("mark message read");
        std::shared_ptr<MarkMessageReadRequest> markMessageReadRequest =
          std::static_pointer_cast<MarkMessageReadRequest>(p_RequestMessage);
        int rv = CMxMarkMessageRead(m_ConnId, const_cast<char*>(markMessageReadRequest->chatId.c_str()),
                                    const_cast<char*>(markMessageReadRequest->msgId.c_str()));

        std::shared_ptr<MarkMessageReadNotify> markMessageReadNotify =
          std::make_shared<MarkMessageReadNotify>(m_ProfileId);
        markMessageReadNotify->success = (rv == 0);
        markMessageReadNotify->chatId = markMessageReadRequest->chatId;
        markMessageReadNotify->msgId = markMessageReadRequest->msgId;
        CallMessageHandler(markMessageReadNotify);
      }
      break;

    case DeleteMessageRequestType:
      {
        LOG_DEBUG("delete message");
        Status::Set(m_ProfileId, Status::FlagUpdating);
        std::shared_ptr<DeleteMessageRequest> deleteMessageRequest =
          std::static_pointer_cast<DeleteMessageRequest>(p_RequestMessage);
        CMxDeleteMessage(m_ConnId, const_cast<char*>(deleteMessageRequest->chatId.c_str()),
                         const_cast<char*>(deleteMessageRequest->msgId.c_str()));
        Status::Clear(m_ProfileId, Status::FlagUpdating);

        std::shared_ptr<DeleteMessageNotify> deleteMessageNotify = std::make_shared<DeleteMessageNotify>(m_ProfileId);
        deleteMessageNotify->success = true; // ignore result, message may already be redacted
        deleteMessageNotify->chatId = deleteMessageRequest->chatId;
        deleteMessageNotify->msgId = deleteMessageRequest->msgId;
        deleteMessageNotify->isOutgoing = true;
        CallMessageHandler(deleteMessageNotify);
      }
      break;

    case DeleteChatRequestType:
      {
        LOG_DEBUG("delete chat");
        Status::Set(m_ProfileId, Status::FlagUpdating);
        std::shared_ptr<DeleteChatRequest> deleteChatRequest =
          std::static_pointer_cast<DeleteChatRequest>(p_RequestMessage);
        CMxDeleteChat(m_ConnId, const_cast<char*>(deleteChatRequest->chatId.c_str()));
        Status::Clear(m_ProfileId, Status::FlagUpdating);

        std::shared_ptr<DeleteChatNotify> deleteChatNotify = std::make_shared<DeleteChatNotify>(m_ProfileId);
        deleteChatNotify->success = true;
        deleteChatNotify->chatId = deleteChatRequest->chatId;
        CallMessageHandler(deleteChatNotify);
      }
      break;

    case PinChatRequestType:
      {
        LOG_DEBUG("pin chat");
        Status::Set(m_ProfileId, Status::FlagUpdating);
        std::shared_ptr<PinChatRequest> pinChatRequest = std::static_pointer_cast<PinChatRequest>(p_RequestMessage);
        CMxPinChat(m_ConnId, const_cast<char*>(pinChatRequest->chatId.c_str()), pinChatRequest->isPinned ? 1 : 0);
        Status::Clear(m_ProfileId, Status::FlagUpdating);
      }
      break;

    case SendTypingRequestType:
      {
        std::shared_ptr<SendTypingRequest> sendTypingRequest =
          std::static_pointer_cast<SendTypingRequest>(p_RequestMessage);
        int rv = CMxSendTyping(m_ConnId, const_cast<char*>(sendTypingRequest->chatId.c_str()),
                               sendTypingRequest->isTyping ? 1 : 0);

        std::shared_ptr<SendTypingNotify> sendTypingNotify = std::make_shared<SendTypingNotify>(m_ProfileId);
        sendTypingNotify->success = (rv == 0);
        sendTypingNotify->chatId = sendTypingRequest->chatId;
        sendTypingNotify->isTyping = sendTypingRequest->isTyping;
        CallMessageHandler(sendTypingNotify);
      }
      break;

    case SetStatusRequestType:
      {
        std::shared_ptr<SetStatusRequest> setStatusRequest =
          std::static_pointer_cast<SetStatusRequest>(p_RequestMessage);
        int rv = CMxSendStatus(m_ConnId, setStatusRequest->isOnline ? 1 : 0);

        std::shared_ptr<SetStatusNotify> setStatusNotify = std::make_shared<SetStatusNotify>(m_ProfileId);
        setStatusNotify->success = (rv == 0);
        setStatusNotify->isOnline = setStatusRequest->isOnline;
        CallMessageHandler(setStatusNotify);
      }
      break;

    case CreateChatRequestType:
      {
        LOG_DEBUG("create chat");
        std::shared_ptr<CreateChatRequest> createChatRequest =
          std::static_pointer_cast<CreateChatRequest>(p_RequestMessage);
        CMxCreateChat(m_ConnId, const_cast<char*>(createChatRequest->userId.c_str()));
      }
      break;

    case DownloadFileRequestType:
      {
        std::shared_ptr<DownloadFileRequest> downloadFileRequest =
          std::static_pointer_cast<DownloadFileRequest>(p_RequestMessage);
        CMxDownloadFile(m_ConnId, const_cast<char*>(downloadFileRequest->chatId.c_str()),
                        const_cast<char*>(downloadFileRequest->msgId.c_str()),
                        const_cast<char*>(downloadFileRequest->fileId.c_str()),
                        downloadFileRequest->downloadFileAction);
      }
      break;

    case SendReactionRequestType:
      {
        LOG_DEBUG("send reaction");
        std::shared_ptr<SendReactionRequest> sendReactionRequest =
          std::static_pointer_cast<SendReactionRequest>(p_RequestMessage);
        CMxSendReaction(m_ConnId, const_cast<char*>(sendReactionRequest->chatId.c_str()),
                        const_cast<char*>(sendReactionRequest->msgId.c_str()),
                        const_cast<char*>(sendReactionRequest->emoji.c_str()));
      }
      break;

    case DeferNotifyRequestType:
      {
        std::shared_ptr<DeferNotifyRequest> deferNotifyRequest =
          std::static_pointer_cast<DeferNotifyRequest>(p_RequestMessage);
        CallMessageHandler(deferNotifyRequest->serviceMessage);
        requestIntervalDelay = false;
      }
      break;

    case SetCurrentChatRequestType:
      break;

    case FindMessageRequestType:
      {
        std::shared_ptr<FindMessageRequest> findMessageRequest =
          std::static_pointer_cast<FindMessageRequest>(p_RequestMessage);
        MessageCache::FindMessage(m_ProfileId, findMessageRequest->chatId, findMessageRequest->fromMsgId,
                                  findMessageRequest->lastMsgId, findMessageRequest->findText,
                                  findMessageRequest->findMsgId, findMessageRequest->findPinned);
      }
      break;

    case GetAvatarRequestType:
      {
        std::shared_ptr<GetAvatarRequest> getAvatarRequest =
          std::static_pointer_cast<GetAvatarRequest>(p_RequestMessage);
        CMxGetAvatar(m_ConnId, const_cast<char*>(getAvatarRequest->id.c_str()));
      }
      break;

    case GetDescriptionRequestType:
      {
        std::shared_ptr<GetDescriptionRequest> getDescriptionRequest =
          std::static_pointer_cast<GetDescriptionRequest>(p_RequestMessage);
        CMxGetDescription(m_ConnId, const_cast<char*>(getDescriptionRequest->id.c_str()));
      }
      break;

    case JoinChatRequestType:
      {
        std::shared_ptr<JoinChatRequest> joinChatRequest = std::static_pointer_cast<JoinChatRequest>(p_RequestMessage);
        CMxJoinChat(m_ConnId, const_cast<char*>(joinChatRequest->link.c_str()));
      }
      break;

    case GetGroupMembersRequestType:
      {
        std::shared_ptr<GetGroupMembersRequest> getGroupMembersRequest =
          std::static_pointer_cast<GetGroupMembersRequest>(p_RequestMessage);
        CMxGetGroupMembers(m_ConnId, const_cast<char*>(getGroupMembersRequest->chatId.c_str()));
      }
      break;

    default:
      LOG_DEBUG("unsupported request %d", p_RequestMessage->GetMessageType());
      break;
  }

  if (requestIntervalDelay)
  {
    TimeUtil::Sleep(0.010);
  }
}

void MxChat::SetProtocolUiControl(bool p_IsTakeControl)
{
  if (m_IsSetup) return;

  std::shared_ptr<ProtocolUiControlNotify> protocolUiControlNotify =
    std::make_shared<ProtocolUiControlNotify>(m_ProfileId);
  protocolUiControlNotify->isTakeControl = p_IsTakeControl;
  CallMessageHandler(protocolUiControlNotify);

  // if attempting to take control, but failed to do so, keep retrying
  while (p_IsTakeControl && !protocolUiControlNotify->isTakeControl)
  {
    TimeUtil::Sleep(0.500);
    protocolUiControlNotify->isTakeControl = p_IsTakeControl;
    CallMessageHandler(protocolUiControlNotify);
  }

  TimeUtil::Sleep(0.100); // wait more than GetKey timeout
}

bool MxChat::RunRecoveryPrompt(bool p_IsSetup)
{
  std::unique_lock<std::mutex> lock(m_RecoveryMutex, std::try_to_lock);
  if (!lock.owns_lock()) return false;

  if (m_Config.Get("recovery_prompt") == "0") return false;

  // the terminal fallback needs the terminal released by the ui
  const bool useTerminal = !p_IsSetup && !m_AuthHandler;
  if (useTerminal)
  {
    SetProtocolUiControl(true);
    std::cout << "\n";
  }

  AuthUtil::ShowInfo(m_AuthHandler,
                     "Matrix " + GetSelfId() + ": this device is not verified yet.\n"
                     "Enter your recovery key (or security passphrase) to verify this device and to\n"
                     "decrypt older encrypted messages using server-side key backup.\n"
                     "Leave empty to skip for now, or enter 'never' to stop asking.");

  bool verified = false;
  for (int attempt = 0; attempt < 3; ++attempt)
  {
    std::string key;
    if (!AuthUtil::Input(m_AuthHandler, s_AuthFieldRecoveryKey, "Recovery key", key,
                         true /*p_IsSecret*/, true /*p_IsOptional*/))
    {
      break;
    }

    StrUtil::Trim(key);
    if (key.empty())
    {
      break;
    }

    if (key == "never")
    {
      m_Config.Set("recovery_prompt", "0");
      m_Config.Save();
      break;
    }

    AuthUtil::ShowInfo(m_AuthHandler, "Verifying...");
    const std::string error = TakeCString(CMxRecover(m_ConnId, const_cast<char*>(key.c_str())));
    SecureClear(key);
    if (error.empty())
    {
      AuthUtil::ShowInfo(m_AuthHandler, "Device verified. Older messages are decrypted in the background.");
      verified = true;
      break;
    }

    AuthUtil::ShowError(m_AuthHandler, "Failed: " + error);
  }

  if (useTerminal)
  {
    TimeUtil::Sleep(1.5);
    SetProtocolUiControl(false);
  }

  return verified;
}

void MxChat::RecoveryPrompt()
{
  // during setup the prompt is run explicitly by SetupProfile
  if (m_IsSetup) return;

  static std::mutex s_PromptedMutex;
  static std::set<int> s_Prompted;
  {
    std::unique_lock<std::mutex> lock(s_PromptedMutex);
    if (s_Prompted.count(m_ConnId)) return;

    s_Prompted.insert(m_ConnId);
  }

  // wait for ui to be set up
  for (int i = 0; (i < 100) && !m_MessageHandler; ++i)
  {
    TimeUtil::Sleep(0.100);
  }

  if (!m_MessageHandler) return;

  RunRecoveryPrompt(false /*p_IsSetup*/);
}

void MxChat::ReportError(const std::string& p_Message)
{
  LOG_ERROR("%s", p_Message.c_str());
  if (m_IsSetup || m_AuthHandler)
  {
    AuthUtil::ShowError(m_AuthHandler, p_Message);
    return;
  }

  // wait for ui to be set up
  for (int i = 0; (i < 100) && !m_MessageHandler; ++i)
  {
    TimeUtil::Sleep(0.100);
  }

  if (!m_MessageHandler)
  {
    std::cerr << "matrix: " << p_Message << "\n";
    return;
  }

  // fatal errors only: show on terminal until acknowledged
  std::unique_lock<std::mutex> lock(m_RecoveryMutex);
  SetProtocolUiControl(true);
  std::cout << "\nMatrix " << GetSelfId() << " error:\n" << p_Message << "\n\nPress ENTER to continue.";
  std::cout.flush();
  std::string line;
  std::getline(std::cin, line);
  SetProtocolUiControl(false);
}

void MxChat::SetStatus(int p_Flags)
{
  Status::Set(m_ProfileId, p_Flags);
}

void MxChat::ClearStatus(int p_Flags)
{
  Status::Clear(m_ProfileId, p_Flags);
}

void MxChat::AddContactInfo(const ContactInfo& p_ContactInfo)
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  m_ContactInfos.push_back(p_ContactInfo);
}

std::vector<ContactInfo> MxChat::GetContactInfos()
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  return m_ContactInfos;
}

void MxChat::ClearContactInfos()
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  m_ContactInfos.clear();
}

void MxChat::AddBatchMessage(const std::string& p_ChatId, const ChatMessage& p_ChatMessage)
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  m_BatchMessages[p_ChatId].push_back(p_ChatMessage);
}

std::vector<ChatMessage> MxChat::TakeBatchMessages(const std::string& p_ChatId)
{
  std::unique_lock<std::mutex> lock(m_Mutex);
  std::vector<ChatMessage> chatMessages;
  auto it = m_BatchMessages.find(p_ChatId);
  if (it != m_BatchMessages.end())
  {
    chatMessages.swap(it->second);
    m_BatchMessages.erase(it);
  }

  return chatMessages;
}

void MxChat::AddInstance(int p_ConnId, MxChat* p_Instance)
{
  std::unique_lock<std::mutex> lock(s_ConnIdMapMutex);
  s_ConnIdMap[p_ConnId] = p_Instance;
}

void MxChat::RemoveInstance(int p_ConnId)
{
  std::unique_lock<std::mutex> lock(s_ConnIdMapMutex);
  s_ConnIdMap.erase(p_ConnId);
}

MxChat* MxChat::GetInstance(int p_ConnId)
{
  std::unique_lock<std::mutex> lock(s_ConnIdMapMutex);
  auto it = s_ConnIdMap.find(p_ConnId);
  return (it != s_ConnIdMap.end()) ? it->second : nullptr;
}

static void DeferNotify(MxChat* p_Instance, std::shared_ptr<ServiceMessage> p_ServiceMessage)
{
  std::shared_ptr<DeferNotifyRequest> deferNotifyRequest = std::make_shared<DeferNotifyRequest>();
  deferNotifyRequest->serviceMessage = p_ServiceMessage;
  p_Instance->SendRequest(deferNotifyRequest);
}

void MxNewContactsNotify(int p_ConnId, char* p_Id, char* p_Name, int p_IsSelf, int p_Notify)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    ContactInfo contactInfo;
    contactInfo.id = std::string(p_Id);
    contactInfo.name = std::string(p_Name);
    contactInfo.isSelf = (p_IsSelf == 1);

    std::shared_ptr<NewContactsNotify> newContactsNotify;
    if (p_Notify == NotifyDirect)
    {
      newContactsNotify = std::make_shared<NewContactsNotify>(instance->GetProfileId());
      newContactsNotify->fullSync = false;
      newContactsNotify->contactInfos.push_back(contactInfo);
    }
    else if (p_Notify == NotifyCache)
    {
      instance->AddContactInfo(contactInfo);
    }
    else if (p_Notify == NotifySendCached)
    {
      instance->AddContactInfo(contactInfo);
      newContactsNotify = std::make_shared<NewContactsNotify>(instance->GetProfileId());
      newContactsNotify->fullSync = true;
      newContactsNotify->contactInfos = instance->GetContactInfos();
      instance->ClearContactInfos();
    }

    if (newContactsNotify)
    {
      DeferNotify(instance, newContactsNotify);
    }
  }

  free(p_Id);
  free(p_Name);
}

void MxNewChatsNotify(int p_ConnId, char* p_ChatId, int p_IsUnread, int p_IsMuted, int p_IsPinned,
                      long long p_LastMessageTime)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    ChatInfo chatInfo;
    chatInfo.id = std::string(p_ChatId);
    chatInfo.isUnread = (p_IsUnread == 1);
    chatInfo.isMuted = (p_IsMuted == 1);
    chatInfo.isPinned = (p_IsPinned == 1);
    chatInfo.lastMessageTime = static_cast<int64_t>(p_LastMessageTime);

    std::shared_ptr<NewChatsNotify> newChatsNotify = std::make_shared<NewChatsNotify>(instance->GetProfileId());
    newChatsNotify->success = true;
    newChatsNotify->chatInfos = std::vector<ChatInfo>({ chatInfo });
    DeferNotify(instance, newChatsNotify);
  }

  free(p_ChatId);
}

void MxNewMessagesNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, char* p_SenderId, char* p_Text, int p_FromMe,
                         char* p_QuotedId, char* p_FileId, char* p_FilePath, int p_FileStatus, long long p_TimeSent,
                         int p_IsRead, int p_IsEdited, int p_Batch)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    const std::string chatId(p_ChatId);
    ChatMessage chatMessage;
    chatMessage.id = std::string(p_MsgId);
    chatMessage.senderId = std::string(p_SenderId);
    chatMessage.text = std::string(p_Text);
    chatMessage.isOutgoing = (p_FromMe == 1);
    chatMessage.quotedId = std::string(p_QuotedId);
    chatMessage.timeSent = static_cast<int64_t>(p_TimeSent);
    chatMessage.isRead = (p_IsRead == 1);
    chatMessage.isEdited = (p_IsEdited == 1);

    bool skip = false;
    std::vector<ChatMessage> cachedMessages;
    if (MessageCache::GetOneMessage(instance->GetProfileId(), chatId, chatMessage.id, cachedMessages))
    {
      const ChatMessage& cached = cachedMessages.at(0);
      if (chatMessage.isEdited)
      {
        if (cached.senderId != chatMessage.senderId)
        {
          // only the original sender may edit a message
          LOG_WARNING("ignore edit from other sender");
          skip = true;
        }

        // retain original sent time, file info, reply and read state
        chatMessage.timeSent = cached.timeSent;
        chatMessage.fileInfo = cached.fileInfo;
        chatMessage.quotedId = cached.quotedId;
        chatMessage.isRead = cached.isRead;
      }
      else
      {
        // update of existing message (ex: decrypted after keys arrived): keep read state
        chatMessage.isRead = chatMessage.isRead || cached.isRead;
      }

      chatMessage.reactions = cached.reactions;
    }

    if (!chatMessage.isEdited)
    {
      const std::string fileId(p_FileId);
      if (!fileId.empty())
      {
        FileInfo fileInfo;
        fileInfo.fileStatus = static_cast<FileStatus>(p_FileStatus);
        fileInfo.fileId = fileId;
        fileInfo.filePath = std::string(p_FilePath);
        if ((fileInfo.fileStatus == FileStatusNotDownloaded) && FileUtil::Exists(fileInfo.filePath))
        {
          fileInfo.fileStatus = FileStatusDownloaded;
        }

        chatMessage.fileInfo = ProtocolUtil::FileInfoToHex(fileInfo);
      }
    }

    if (!skip)
    {
      if (p_Batch == 1)
      {
        instance->AddBatchMessage(chatId, chatMessage);
      }
      else
      {
        std::shared_ptr<NewMessagesNotify> newMessagesNotify =
          std::make_shared<NewMessagesNotify>(instance->GetProfileId());
        newMessagesNotify->success = true;
        newMessagesNotify->chatId = chatId;
        newMessagesNotify->chatMessages = std::vector<ChatMessage>({ chatMessage });
        newMessagesNotify->cached = false;
        newMessagesNotify->sequence = true;
        DeferNotify(instance, newMessagesNotify);
      }
    }
  }

  free(p_ChatId);
  free(p_MsgId);
  free(p_SenderId);
  free(p_Text);
  free(p_QuotedId);
  free(p_FileId);
  free(p_FilePath);
}

void MxNewMessagesBatchDone(int p_ConnId, char* p_ChatId, char* p_FromMsgId)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    const std::string chatId(p_ChatId);
    std::shared_ptr<NewMessagesNotify> newMessagesNotify =
      std::make_shared<NewMessagesNotify>(instance->GetProfileId());
    newMessagesNotify->success = true;
    newMessagesNotify->chatId = chatId;
    newMessagesNotify->chatMessages = instance->TakeBatchMessages(chatId);
    newMessagesNotify->fromMsgId = std::string(p_FromMsgId);
    newMessagesNotify->cached = false;
    newMessagesNotify->sequence = true;
    DeferNotify(instance, newMessagesNotify);
  }

  free(p_ChatId);
  free(p_FromMsgId);
}

void MxNewMessageStatusNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, int p_IsRead)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    std::shared_ptr<NewMessageStatusNotify> newMessageStatusNotify =
      std::make_shared<NewMessageStatusNotify>(instance->GetProfileId());
    newMessageStatusNotify->chatId = std::string(p_ChatId);
    newMessageStatusNotify->msgId = std::string(p_MsgId);
    newMessageStatusNotify->isRead = (p_IsRead == 1);
    DeferNotify(instance, newMessageStatusNotify);
  }

  free(p_ChatId);
  free(p_MsgId);
}

void MxNewMessageFileNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, char* p_FileId, char* p_FilePath,
                            int p_FileStatus, int p_Action)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    FileInfo fileInfo;
    fileInfo.fileStatus = static_cast<FileStatus>(p_FileStatus);
    fileInfo.fileId = std::string(p_FileId);
    fileInfo.filePath = std::string(p_FilePath);

    std::shared_ptr<NewMessageFileNotify> newMessageFileNotify =
      std::make_shared<NewMessageFileNotify>(instance->GetProfileId());
    newMessageFileNotify->chatId = std::string(p_ChatId);
    newMessageFileNotify->msgId = std::string(p_MsgId);
    newMessageFileNotify->fileInfo = ProtocolUtil::FileInfoToHex(fileInfo);
    newMessageFileNotify->downloadFileAction = static_cast<DownloadFileAction>(p_Action);
    DeferNotify(instance, newMessageFileNotify);
  }

  free(p_ChatId);
  free(p_MsgId);
  free(p_FileId);
  free(p_FilePath);
}

void MxNewMessageReactionNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, char* p_SenderId, char* p_Text,
                                int p_FromMe)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    const std::string senderId = (p_FromMe == 1) ? s_ReactionsSelfId : std::string(p_SenderId);
    Reactions reactions;
    // go via message cache for consolidation and count before handled by ui
    reactions.needConsolidationWithCache = true;
    reactions.updateCountBasedOnSender = true;
    reactions.replaceCount = false;
    reactions.senderEmojis[senderId] = std::string(p_Text);

    std::shared_ptr<NewMessageReactionsNotify> newMessageReactionsNotify =
      std::make_shared<NewMessageReactionsNotify>(instance->GetProfileId());
    newMessageReactionsNotify->chatId = std::string(p_ChatId);
    newMessageReactionsNotify->msgId = std::string(p_MsgId);
    newMessageReactionsNotify->reactions = reactions;
    DeferNotify(instance, newMessageReactionsNotify);
  }

  free(p_ChatId);
  free(p_MsgId);
  free(p_SenderId);
  free(p_Text);
}

void MxNewTypingNotify(int p_ConnId, char* p_ChatId, char* p_UserId, int p_IsTyping)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    std::shared_ptr<ReceiveTypingNotify> receiveTypingNotify =
      std::make_shared<ReceiveTypingNotify>(instance->GetProfileId());
    receiveTypingNotify->chatId = std::string(p_ChatId);
    receiveTypingNotify->userId = std::string(p_UserId);
    receiveTypingNotify->isTyping = (p_IsTyping == 1);
    DeferNotify(instance, receiveTypingNotify);
  }

  free(p_ChatId);
  free(p_UserId);
}

void MxNewStatusNotify(int p_ConnId, char* p_UserId, int p_IsOnline, long long p_TimeSeen)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    std::shared_ptr<ReceiveStatusNotify> receiveStatusNotify =
      std::make_shared<ReceiveStatusNotify>(instance->GetProfileId());
    receiveStatusNotify->userId = std::string(p_UserId);
    receiveStatusNotify->isOnline = (p_IsOnline == 1);
    receiveStatusNotify->timeSeen = static_cast<int64_t>(p_TimeSeen);
    DeferNotify(instance, receiveStatusNotify);
  }

  free(p_UserId);
}

void MxNewGroupMembersNotify(int p_ConnId, char* p_ChatId, char* p_MembersJson)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    std::shared_ptr<NewGroupMembersNotify> notify =
      std::make_shared<NewGroupMembersNotify>(instance->GetProfileId());
    notify->chatId = std::string(p_ChatId);
    notify->contactInfos = ProtocolUtil::ContactInfosFromJson(std::string(p_MembersJson));
    DeferNotify(instance, notify);
  }

  free(p_ChatId);
  free(p_MembersJson);
}

void MxNewAvatarNotify(int p_ConnId, char* p_Id, char* p_FilePath)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    std::shared_ptr<NewAvatarNotify> notify = std::make_shared<NewAvatarNotify>(instance->GetProfileId());
    notify->id = std::string(p_Id);
    notify->filePath = std::string(p_FilePath);
    DeferNotify(instance, notify);
  }

  free(p_Id);
  free(p_FilePath);
}

void MxNewDescriptionNotify(int p_ConnId, char* p_Id, char* p_Description)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    std::shared_ptr<NewDescriptionNotify> notify = std::make_shared<NewDescriptionNotify>(instance->GetProfileId());
    notify->id = std::string(p_Id);
    notify->description = std::string(p_Description);
    DeferNotify(instance, notify);
  }

  free(p_Id);
  free(p_Description);
}

void MxJoinChatNotify(int p_ConnId, char* p_Link, int p_Success, char* p_ChatId, char* p_Error)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    std::shared_ptr<JoinChatNotify> notify = std::make_shared<JoinChatNotify>(instance->GetProfileId());
    notify->link = std::string(p_Link);
    notify->success = (p_Success != 0);
    notify->chatId = std::string(p_ChatId);
    notify->error = std::string(p_Error);
    DeferNotify(instance, notify);
  }

  free(p_Link);
  free(p_ChatId);
  free(p_Error);
}

void MxDeleteChatNotify(int p_ConnId, char* p_ChatId)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    std::shared_ptr<DeleteChatNotify> deleteChatNotify =
      std::make_shared<DeleteChatNotify>(instance->GetProfileId());
    deleteChatNotify->success = true;
    deleteChatNotify->chatId = std::string(p_ChatId);
    DeferNotify(instance, deleteChatNotify);
  }

  free(p_ChatId);
}

void MxDeleteMessageNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, int p_IsOutgoing)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    static const int messageDelete = AppConfig::GetNum("message_delete");
    const bool isOutgoing = (p_IsOutgoing != 0);
    const std::string chatId(p_ChatId);
    const std::string msgId(p_MsgId);

    std::vector<ChatMessage> chatMessages;
    if (!isOutgoing &&
        ((messageDelete == MessageDeleteReplace) || (messageDelete == MessageDeletePrefix)) &&
        MessageCache::GetOneMessage(instance->GetProfileId(), chatId, msgId, chatMessages))
    {
      ChatMessage chatMessage = chatMessages.front();
      chatMessage.isRead = true;
      chatMessage.isDeleted = true;

      if (messageDelete == MessageDeleteReplace)
      {
        chatMessage.text = std::string("[This message was deleted]");
        chatMessage.fileInfo.clear();
      }
      else if (!StrUtil::StartsWith(chatMessage.text, "[This message was deleted]"))
      {
        chatMessage.text = std::string("[This message was deleted]\n") + chatMessage.text;
      }

      std::shared_ptr<NewMessagesNotify> newMessagesNotify =
        std::make_shared<NewMessagesNotify>(instance->GetProfileId());
      newMessagesNotify->success = true;
      newMessagesNotify->chatId = chatId;
      newMessagesNotify->chatMessages = std::vector<ChatMessage>({ chatMessage });
      newMessagesNotify->cached = false;
      newMessagesNotify->sequence = true;
      DeferNotify(instance, newMessagesNotify);
    }
    else
    {
      std::shared_ptr<DeleteMessageNotify> deleteMessageNotify =
        std::make_shared<DeleteMessageNotify>(instance->GetProfileId());
      deleteMessageNotify->success = true;
      deleteMessageNotify->chatId = chatId;
      deleteMessageNotify->msgId = msgId;
      deleteMessageNotify->isOutgoing = isOutgoing;
      DeferNotify(instance, deleteMessageNotify);
    }
  }

  free(p_ChatId);
  free(p_MsgId);
}

void MxUpdateMuteNotify(int p_ConnId, char* p_ChatId, int p_IsMuted)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    std::shared_ptr<UpdateMuteNotify> updateMuteNotify =
      std::make_shared<UpdateMuteNotify>(instance->GetProfileId());
    updateMuteNotify->success = true;
    updateMuteNotify->chatId = std::string(p_ChatId);
    updateMuteNotify->isMuted = (p_IsMuted == 1);
    DeferNotify(instance, updateMuteNotify);
  }

  free(p_ChatId);
}

void MxUpdatePinNotify(int p_ConnId, char* p_ChatId, int p_IsPinned, long long p_TimePinned)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    std::shared_ptr<UpdatePinNotify> updatePinNotify =
      std::make_shared<UpdatePinNotify>(instance->GetProfileId());
    updatePinNotify->success = true;
    updatePinNotify->chatId = std::string(p_ChatId);
    updatePinNotify->isPinned = (p_IsPinned == 1);
    updatePinNotify->timePinned = static_cast<int64_t>(p_TimePinned);
    DeferNotify(instance, updatePinNotify);
  }

  free(p_ChatId);
}

void MxCreateChatNotify(int p_ConnId, char* p_ChatId, int p_Success)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    std::shared_ptr<CreateChatNotify> createChatNotify =
      std::make_shared<CreateChatNotify>(instance->GetProfileId());
    createChatNotify->success = (p_Success == 1);
    createChatNotify->chatInfo.id = std::string(p_ChatId);
    DeferNotify(instance, createChatNotify);
  }

  free(p_ChatId);
}

void MxRecoveryPrompt(int p_ConnId)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    instance->RecoveryPrompt();
  }
}

void MxReportError(int p_ConnId, char* p_Message)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    instance->ReportError(std::string(p_Message));
  }

  free(p_Message);
}

void MxSetStatus(int p_ConnId, int p_Flags)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    instance->SetStatus(p_Flags);
  }
}

void MxClearStatus(int p_ConnId, int p_Flags)
{
  MxChat* instance = MxChat::GetInstance(p_ConnId);
  if (instance != nullptr)
  {
    instance->ClearStatus(p_Flags);
  }
}

void MxLogTrace(char* p_Filename, int p_LineNo, char* p_Message)
{
  Log::Trace(p_Filename, p_LineNo, "%s", p_Message);
  free(p_Filename);
  free(p_Message);
}

void MxLogDebug(char* p_Filename, int p_LineNo, char* p_Message)
{
  Log::Debug(p_Filename, p_LineNo, "%s", p_Message);
  free(p_Filename);
  free(p_Message);
}

void MxLogInfo(char* p_Filename, int p_LineNo, char* p_Message)
{
  Log::Info(p_Filename, p_LineNo, "%s", p_Message);
  free(p_Filename);
  free(p_Message);
}

void MxLogWarning(char* p_Filename, int p_LineNo, char* p_Message)
{
  Log::Warning(p_Filename, p_LineNo, "%s", p_Message);
  free(p_Filename);
  free(p_Message);
}

void MxLogError(char* p_Filename, int p_LineNo, char* p_Message)
{
  Log::Error(p_Filename, p_LineNo, "%s", p_Message);
  free(p_Filename);
  free(p_Message);
}
