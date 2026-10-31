// mxchat.h
//
// Copyright (c) 2026 nchat contributors
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

#include "config.h"
#include "protocol.h"

class MxChat : public Protocol
{
public:
  MxChat();
  virtual ~MxChat();
  static std::string GetName() { return "Matrix"; }
  static std::string GetLibName() { return "libmxchat"; }
  static std::string GetCreateFunc() { return "CreateMxChat"; }
  static std::string GetSetupMessage() { return ""; }

  std::string GetProfileId() const;
  std::string GetProfileDisplayName() const;
  bool HasFeature(ProtocolFeature p_ProtocolFeature) const;
  bool IsGroupChat(const std::string& p_ChatId) const;
  std::string GetSelfId() const;

  bool SetupProfile(const std::string& p_ProfilesDir, std::string& p_ProfileId);
  bool LoadProfile(const std::string& p_ProfilesDir, const std::string& p_ProfileId);
  bool CloseProfile();

  bool Login();
  bool Logout();

  void Process();

  void SendRequest(std::shared_ptr<RequestMessage> p_RequestMessage);
  void SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>& p_MessageHandler);

  void SetProtocolUiControl(bool p_IsTakeControl);
  void SetStatus(int p_Flags);
  void ClearStatus(int p_Flags);
  void RecoveryPrompt();
  void ReportError(const std::string& p_Message);

  void AddContactInfo(const ContactInfo& p_ContactInfo);
  std::vector<ContactInfo> GetContactInfos();
  void ClearContactInfos();

  void AddBatchMessage(const std::string& p_ChatId, const ChatMessage& p_ChatMessage);
  std::vector<ChatMessage> TakeBatchMessages(const std::string& p_ChatId);

public:
  static void AddInstance(int p_ConnId, MxChat* p_Instance);
  static void RemoveInstance(int p_ConnId);
  static MxChat* GetInstance(int p_ConnId);
  static std::string SanitizeProfileId(const std::string& p_UserId);

private:
  void InitConfig();
  void CleanupConfig();
  void CallMessageHandler(std::shared_ptr<ServiceMessage> p_ServiceMessage);
  void PerformRequest(std::shared_ptr<RequestMessage> p_RequestMessage);
  bool RunRecoveryPrompt(bool p_IsSetup);

private:
  std::string m_ProfileId;
  std::string m_ProfileDir;
  std::function<void(std::shared_ptr<ServiceMessage>)> m_MessageHandler;

  bool m_Running = false;
  std::thread m_Thread;
  std::deque<std::shared_ptr<RequestMessage>> m_RequestsQueue;
  std::mutex m_ProcessMutex;
  std::condition_variable m_ProcessCondVar;

  int m_ConnId = -1;
  Config m_Config;
  bool m_IsSetup = false;
  bool m_WasOnline = false;

  mutable std::mutex m_Mutex;
  std::string m_SelfUserId;
  std::vector<ContactInfo> m_ContactInfos;
  std::map<std::string, std::vector<ChatMessage>> m_BatchMessages;
  std::mutex m_RecoveryMutex;

  static std::mutex s_ConnIdMapMutex;
  static std::map<int, MxChat*> s_ConnIdMap;
  static const int s_CacheDirVersion = 0;
};

extern "C" {
void MxNewContactsNotify(int p_ConnId, char* p_Id, char* p_Name, int p_IsSelf, int p_Notify);
void MxNewChatsNotify(int p_ConnId, char* p_ChatId, int p_IsUnread, int p_IsMuted, int p_IsPinned,
                      long long p_LastMessageTime);
void MxNewMessagesNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, char* p_SenderId, char* p_Text, int p_FromMe,
                         char* p_QuotedId, char* p_FileId, char* p_FilePath, int p_FileStatus, long long p_TimeSent,
                         int p_IsRead, int p_IsEdited, int p_Batch);
void MxNewMessagesBatchDone(int p_ConnId, char* p_ChatId, char* p_FromMsgId);
void MxNewMessageStatusNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, int p_IsRead);
void MxNewMessageFileNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, char* p_FileId, char* p_FilePath,
                            int p_FileStatus, int p_Action);
void MxNewMessageReactionNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, char* p_SenderId, char* p_Text,
                                int p_FromMe);
void MxNewTypingNotify(int p_ConnId, char* p_ChatId, char* p_UserId, int p_IsTyping);
void MxNewStatusNotify(int p_ConnId, char* p_UserId, int p_IsOnline, long long p_TimeSeen);
void MxNewGroupMembersNotify(int p_ConnId, char* p_ChatId, char* p_MembersJson);
void MxNewAvatarNotify(int p_ConnId, char* p_Id, char* p_FilePath);
void MxNewDescriptionNotify(int p_ConnId, char* p_Id, char* p_Description);
void MxDeleteChatNotify(int p_ConnId, char* p_ChatId);
void MxDeleteMessageNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, int p_IsOutgoing);
void MxUpdateMuteNotify(int p_ConnId, char* p_ChatId, int p_IsMuted);
void MxUpdatePinNotify(int p_ConnId, char* p_ChatId, int p_IsPinned, long long p_TimePinned);
void MxCreateChatNotify(int p_ConnId, char* p_ChatId, int p_Success);
void MxRecoveryPrompt(int p_ConnId);
void MxReportError(int p_ConnId, char* p_Message);
void MxSetStatus(int p_ConnId, int p_Flags);
void MxClearStatus(int p_ConnId, int p_Flags);
void MxLogTrace(char* p_Filename, int p_LineNo, char* p_Message);
void MxLogDebug(char* p_Filename, int p_LineNo, char* p_Message);
void MxLogInfo(char* p_Filename, int p_LineNo, char* p_Message);
void MxLogWarning(char* p_Filename, int p_LineNo, char* p_Message);
void MxLogError(char* p_Filename, int p_LineNo, char* p_Message);
}

extern "C" MxChat* CreateMxChat();
