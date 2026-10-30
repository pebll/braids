// chatmodel.h
//
// Copyright (c) 2026 braids contributors
// Copyright (c) 2019-2026 Kristofer Berggren (chat model logic, from nchat UiModel)
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "api.h"
#include "protocol.h"

// Unified chat model across all accounts, ported from the non-UI parts of
// nchat's UiModel::Impl. It applies protocol notifications (ServiceMessage) to
// its state and emits API events, and turns API requests into protocol
// requests. Only used from the core event loop thread.
class ChatModel
{
public:
  using ProtocolGetter = std::function<std::shared_ptr<Protocol>(const std::string& p_AccountId)>;

  ChatModel(const EventSink& p_EventSink, const ProtocolGetter& p_ProtocolGetter);

  // Protocol notifications (also from MessageCache)
  void OnServiceMessage(std::shared_ptr<ServiceMessage> p_ServiceMessage);

  void AddAccount(const std::string& p_AccountId);
  void RemoveAccount(const std::string& p_AccountId);
  void ProcessTimers(int64_t p_NowMs);

  // API requests (throw ApiException on invalid input)
  json GetChats(const std::string& p_AccountId, bool p_IncludeArchived);
  json GetChat(const std::string& p_AccountId, const std::string& p_ChatId);
  json GetContacts(const std::string& p_AccountId);
  void GetMessages(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_FromMsgId,
                   int p_Limit, const Reply& p_Reply);
  void SendMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_Text,
                   const std::string& p_QuotedId, const std::string& p_FilePath);
  void EditMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId,
                   const std::string& p_Text);
  void DeleteMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId);
  void PinMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId,
                  bool p_IsPinned);
  void MarkRead(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId);
  void SendReaction(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId,
                    const std::string& p_Emoji);
  void GetAvailableReactions(const std::string& p_AccountId, const std::string& p_ChatId,
                             const std::string& p_MsgId, const Reply& p_Reply);
  void DownloadFile(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId);
  void SendTyping(const std::string& p_AccountId, const std::string& p_ChatId, bool p_IsTyping);
  void GetMembers(const std::string& p_AccountId, const std::string& p_ChatId, const Reply& p_Reply);
  void CreateChat(const std::string& p_AccountId, const std::string& p_UserId);
  void PinChat(const std::string& p_AccountId, const std::string& p_ChatId, bool p_IsPinned);
  void ArchiveChat(const std::string& p_AccountId, const std::string& p_ChatId, bool p_IsArchived);
  void DeleteChat(const std::string& p_AccountId, const std::string& p_ChatId);
  void SetCurrentChat(const std::string& p_AccountId, const std::string& p_ChatId);
  void FindMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_Text,
                   const std::string& p_FromMsgId, const Reply& p_Reply);

  // Serialization helpers, also used by the core
  static json CapabilitiesToJson(std::shared_ptr<Protocol> p_Protocol, const std::string& p_ProtocolName);

private:
  struct ChatState
  {
    std::unordered_map<std::string, ChatMessage> messages;
    bool previewRequested = false;
  };

  struct AccountState
  {
    std::unordered_map<std::string, ChatInfo> chatInfos;
    std::unordered_map<std::string, ContactInfo> contactInfos;
    std::unordered_map<std::string, ChatState> chats;
    std::unordered_map<std::string, std::vector<std::string>> groupMembers;
    std::unordered_map<std::string, std::set<std::string>> usersTyping;
    std::unordered_map<std::string, bool> userOnline;
    std::unordered_map<std::string, int64_t> userTimeSeen;
    std::unordered_set<std::string> statusRequested;
    int64_t connectTime = 0;
  };

  struct PendingReply
  {
    std::string accountId;
    std::string chatId;
    std::string key;
    int limit = 0;
    int64_t deadline = 0;
    Reply reply;
  };

  enum PendingType
  {
    PendingMessages = 0,
    PendingMembers,
    PendingReactions,
    PendingFind,
  };

private:
  std::shared_ptr<Protocol> GetProtocolOrThrow(const std::string& p_AccountId);
  AccountState& GetAccountOrThrow(const std::string& p_AccountId);
  ChatMessage& GetMessageOrThrow(const std::string& p_AccountId, const std::string& p_ChatId,
                                 const std::string& p_MsgId);
  void SendProtocolRequest(const std::string& p_AccountId, std::shared_ptr<RequestMessage> p_Request);
  bool HasFeature(const std::string& p_AccountId, ProtocolFeature p_Feature);

  void OnNewMessages(const std::string& p_AccountId, std::shared_ptr<NewMessagesNotify> p_Notify);
  void UpdateChatFromMessages(const std::string& p_AccountId, const std::string& p_ChatId);
  const ChatMessage* GetLastMessage(const std::string& p_AccountId, const std::string& p_ChatId);
  void RequestPreview(const std::string& p_AccountId, const std::string& p_ChatId);
  void MaybeNotify(const std::string& p_AccountId, const std::string& p_ChatId, const ChatMessage& p_ChatMessage);
  // p_MinTime: for history pages, the time of the oldest message the protocol
  // delivered for the request (max: none)
  void ResolvePending(PendingType p_Type, const std::string& p_AccountId, const std::string& p_ChatId,
                      const std::string& p_Key, bool p_TimedOut,
                      int64_t p_MinTime = std::numeric_limits<int64_t>::max());
  json MessagesPage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_FromMsgId,
                    int p_Limit, int64_t p_MinTime);
  json MembersJson(const std::string& p_AccountId, const std::string& p_ChatId);
  std::map<std::string, std::string> ParseMentions(const std::string& p_AccountId, const std::string& p_ChatId,
                                                   const std::string& p_Text);

  void MarkChatDirty(const std::string& p_AccountId, const std::string& p_ChatId);
  void FlushDirtyChats();
  void Emit(const std::string& p_Method, const json& p_Params);

  std::string GetContactName(const std::string& p_AccountId, const std::string& p_ContactId);
  json ChatToJson(const std::string& p_AccountId, const ChatInfo& p_ChatInfo);
  json MessageToJson(const std::string& p_AccountId, const std::string& p_ChatId, const ChatMessage& p_ChatMessage);
  const ChatMessage* FindMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId);
  void RequestQuotedMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId);
  void UpdateQuotingMessages(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId);
  json ContactToJson(const ContactInfo& p_ContactInfo);

private:
  EventSink m_EventSink;
  ProtocolGetter m_ProtocolGetter;
  std::unordered_map<std::string, AccountState> m_Accounts;
  std::multimap<PendingType, PendingReply> m_Pending;

  // quoted messages fetched to show their text (protocols like Matrix send
  // only the id): accountId -> "chatId\nmsgId", requested once / still awaited
  std::map<std::string, std::set<std::string>> m_QuoteRequested;
  std::map<std::string, std::set<std::string>> m_QuoteAwaited;
  std::set<std::pair<std::string, std::string>> m_DirtyChats;
  std::map<std::pair<std::string, std::string>, std::string> m_EmittedChats; // last chat.updated per chat
  std::pair<std::string, std::string> m_CurrentChat;
};
