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

  // where per-account braids state (mute settings) is kept: <dir>/<accountId>/
  void SetProfilesDir(const std::string& p_ProfilesDir);
  // runs p_Work on a background thread, then p_Done on the event loop (for
  // slow cache queries that must not block the API)
  using BackgroundRunner = std::function<void(std::function<void()> p_Work, std::function<void()> p_Done)>;
  void SetBackgroundRunner(const BackgroundRunner& p_Runner);

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
  // queued in the account's outbox and shown at once (pending); sent now or
  // when the account is online; returns the message's local id
  std::string SendMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_Text,
                          const std::string& p_QuotedId, const std::string& p_FilePath, bool p_IsVoice = false,
                          const json& p_Poll = json());
  // vote in a poll (optionIds empty: retract)
  void VotePoll(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId,
                const std::vector<std::string>& p_OptionIds);
  // a failed outbox message sent again
  void ResendMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId);
  // the account came online / went offline (sends the waiting messages)
  void SetAccountOnline(const std::string& p_AccountId, bool p_Online);
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
  json GetAvatars(const std::string& p_AccountId, const std::vector<std::string>& p_Ids);
  json GetFullAvatar(const std::string& p_AccountId, const std::string& p_Id);
  json GetDescriptions(const std::string& p_AccountId, const std::vector<std::string>& p_Ids);
  void JoinChat(const std::string& p_AccountId, const std::string& p_Link);
  // messages of a chat, or of all chats (p_AccountId and/or p_ChatId empty),
  // newest first: p_Kind "media", "files", "links", "pinned", or "search"
  // (text contains p_Text)
  json GetMedia(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_Kind,
                int64_t p_BeforeTime, int p_Limit, const std::string& p_Text = "");
  void CreateChat(const std::string& p_AccountId, const std::string& p_UserId);
  void CreateGroup(const std::string& p_AccountId, const std::string& p_Name, const std::vector<std::string>& p_UserIds);
  void EditMembers(const std::string& p_AccountId, const std::string& p_ChatId,
                   const std::vector<std::string>& p_UserIds, bool p_Remove);
  void PinChat(const std::string& p_AccountId, const std::string& p_ChatId, bool p_IsPinned);
  void ArchiveChat(const std::string& p_AccountId, const std::string& p_ChatId, bool p_IsArchived);
  void MuteChat(const std::string& p_AccountId, const std::string& p_ChatId, bool p_IsMuted);
  void MarkUnread(const std::string& p_AccountId, const std::string& p_ChatId);
  // "Joined Telegram" messages: off (default) leaves them out and keeps chats
  // that have nothing else out of the list (kept with the mute settings)
  void SetShowJoined(const std::string& p_AccountId, bool p_Show);
  bool GetShowJoined(const std::string& p_AccountId);
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
    // older messages read from the cache for media.list (not part of the
    // loaded history, which must stay contiguous)
    std::unordered_map<std::string, ChatMessage> detached;
    bool previewRequested = false;
  };

  // a sent message until the messenger has it: waiting (account not
  // online), sending (request out), sent (confirmed, until its echo
  // arrives), failed
  struct OutboxEntry
  {
    std::string id; // local, "braids-out-..."
    std::string chatId;
    std::string text;
    std::string quotedId;
    std::string filePath;
    bool isVoice = false;
    std::string poll; // a poll to send, ChatMessage.poll format
    int64_t time = 0;
    std::string state;
    int64_t sentAt = 0;
  };

  struct AccountState
  {
    std::unordered_map<std::string, ChatInfo> chatInfos;
    std::unordered_map<std::string, ContactInfo> contactInfos;
    std::unordered_map<std::string, ChatState> chats;
    std::unordered_map<std::string, std::vector<std::string>> groupMembers;
    // FeatureEditMembers: per group whether self may add members, and the
    // members self may remove
    std::unordered_map<std::string, bool> groupCanAdd;
    std::unordered_map<std::string, std::set<std::string>> groupCanRemove;
    std::unordered_map<std::string, std::set<std::string>> usersTyping;
    std::unordered_map<std::string, bool> userOnline;
    std::unordered_map<std::string, int64_t> userTimeSeen;
    std::unordered_set<std::string> statusRequested;
    std::unordered_map<std::string, std::string> avatars; // user or chat id -> image path ("" none)
    std::unordered_set<std::string> avatarsRequested;
    // the pictures known at the last run (<profile>/braids-avatars.json):
    // shown at once after a start, while the protocol checks them again
    std::unordered_map<std::string, std::string> savedAvatars;
    bool avatarsDirty = false;
    int64_t avatarsSavedAt = 0;
    std::unordered_map<std::string, std::string> fullAvatars; // full-size pictures fetched for the viewer
    std::unordered_map<std::string, std::string> descriptions; // user or chat id -> text ("" none)
    std::unordered_set<std::string> descriptionsRequested;
    // braids' own mute setting per chat, overrides the messenger's (kept in
    // <profile>/braids-chats.json)
    std::unordered_map<std::string, bool> muted;
    // chats marked unread in braids, until read (kept with the mute settings)
    std::unordered_set<std::string> markedUnread;
    // "Joined Telegram" messages shown, and the new chats left out of the
    // list while they had only such a message (kept with the mute settings)
    bool showJoined = false;
    std::unordered_set<std::string> hiddenChats;
    // unread counts from the cache at connect, for chats whose unread
    // messages are not loaded
    std::map<std::string, int> cachedUnread;
    int64_t connectTime = 0;
    std::vector<OutboxEntry> outbox; // oldest first (kept in <profile>/braids-outbox.json)
    // poll votes that arrived before their poll (chat id \x01 msg id -> votes)
    std::map<std::string, std::vector<std::shared_ptr<NewPollNotify>>> pendingPollVotes;
    bool online = false;
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

  // unread received messages and group events (joined, left, ...)
  void UnreadCount(AccountState& p_Account, const ChatInfo& p_ChatInfo, int& p_Messages, int& p_Events);
  bool IsMuted(const AccountState& p_Account, const ChatInfo& p_ChatInfo) const;
  void LoadAccountState(const std::string& p_AccountId);
  void SaveAccountState(const std::string& p_AccountId);

  // outbox
  void DispatchOutbox(const std::string& p_AccountId, OutboxEntry& p_Entry);
  void OnSendResult(const std::string& p_AccountId, const std::string& p_LocalId, bool p_Success);
  void MatchOutboxEcho(const std::string& p_AccountId, const std::string& p_ChatId, const ChatMessage& p_ChatMessage);
  void RemoveOutboxEntry(const std::string& p_AccountId, const std::string& p_LocalId);
  json OutboxEntryToJson(const std::string& p_AccountId, const OutboxEntry& p_Entry);
  void LoadOutbox(const std::string& p_AccountId);
  void LoadAvatars(const std::string& p_AccountId);
  void SaveAvatars(const std::string& p_AccountId);
  void SaveOutbox(const std::string& p_AccountId);

  void MarkChatDirty(const std::string& p_AccountId, const std::string& p_ChatId);
  void FlushDirtyChats();
  void Emit(const std::string& p_Method, const json& p_Params);

  std::string GetContactName(const std::string& p_AccountId, const std::string& p_ContactId);
  json ChatToJson(const std::string& p_AccountId, const ChatInfo& p_ChatInfo);
  json MessageToJson(const std::string& p_AccountId, const std::string& p_ChatId, const ChatMessage& p_ChatMessage);
  json PollToJson(const std::string& p_AccountId, const ChatMessage& p_ChatMessage);
  void OnPoll(const std::string& p_AccountId, std::shared_ptr<NewPollNotify> p_Notify);
  static void ApplyPollNotify(ChatMessage& p_ChatMessage, const NewPollNotify& p_Notify);
  std::string SelfId(const std::string& p_AccountId);
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
  std::string m_ProfilesDir;
  BackgroundRunner m_BackgroundRunner;
};
