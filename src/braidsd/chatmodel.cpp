// chatmodel.cpp
//
// Copyright (c) 2026 braids contributors
// Copyright (c) 2019-2026 Kristofer Berggren (chat model logic, from nchat UiModel)
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#include "chatmodel.h"

#include <algorithm>
#include <limits>

#include "fileutil.h"
#include "log.h"
#include "messagecache.h"
#include "protocolutil.h"
#include "strutil.h"
#include "timeutil.h"

static const int64_t s_PendingTimeoutMs = 15000;
static const int64_t s_SponsoredTime = std::numeric_limits<int64_t>::max();

static bool IsSameMessage(const ChatMessage& p_Lhs, const ChatMessage& p_Rhs)
{
  return (p_Lhs.text == p_Rhs.text) && (p_Lhs.senderId == p_Rhs.senderId) && (p_Lhs.quotedId == p_Rhs.quotedId) &&
         (p_Lhs.quotedText == p_Rhs.quotedText) && (p_Lhs.fileInfo == p_Rhs.fileInfo) &&
         (p_Lhs.reactions == p_Rhs.reactions) && (p_Lhs.timeSent == p_Rhs.timeSent) &&
         (p_Lhs.isOutgoing == p_Rhs.isOutgoing) && (p_Lhs.isRead == p_Rhs.isRead) &&
         (p_Lhs.isEdited == p_Rhs.isEdited) && (p_Lhs.isDeleted == p_Rhs.isDeleted) &&
         (p_Lhs.isPinned == p_Rhs.isPinned) && (p_Lhs.hasMention == p_Rhs.hasMention);
}

static const char* FileStatusToString(FileStatus p_FileStatus)
{
  switch (p_FileStatus)
  {
    case FileStatusNotDownloaded: return "notDownloaded";
    case FileStatusDownloaded: return "downloaded";
    case FileStatusDownloading: return "downloading";
    case FileStatusDownloadFailed: return "failed";
    case FileStatusNone:
    default:
      return "none";
  }
}

// Protocols whose read receipts are per message (others are "read up to")
static bool HasPerMessageReadReceipts(const std::string& p_ProtocolName)
{
  return (p_ProtocolName == "WhatsAppMd") || (p_ProtocolName == "Signal");
}

ChatModel::ChatModel(const EventSink& p_EventSink, const ProtocolGetter& p_ProtocolGetter)
  : m_EventSink(p_EventSink)
  , m_ProtocolGetter(p_ProtocolGetter)
{
}

void ChatModel::AddAccount(const std::string& p_AccountId)
{
  m_Accounts[p_AccountId];
}

void ChatModel::RemoveAccount(const std::string& p_AccountId)
{
  m_Accounts.erase(p_AccountId);

  for (auto it = m_Pending.begin(); it != m_Pending.end(); /* incremented in loop */)
  {
    if (it->second.accountId == p_AccountId)
    {
      it->second.reply.Error(ApiErrorUnknownAccount, "account removed");
      it = m_Pending.erase(it);
    }
    else
    {
      ++it;
    }
  }

  for (auto it = m_DirtyChats.begin(); it != m_DirtyChats.end(); /* incremented in loop */)
  {
    it = (it->first == p_AccountId) ? m_DirtyChats.erase(it) : std::next(it);
  }

  for (auto it = m_EmittedChats.begin(); it != m_EmittedChats.end(); /* incremented in loop */)
  {
    it = (it->first.first == p_AccountId) ? m_EmittedChats.erase(it) : std::next(it);
  }

  if (m_CurrentChat.first == p_AccountId)
  {
    m_CurrentChat = std::make_pair(std::string(), std::string());
  }
}

void ChatModel::OnServiceMessage(std::shared_ptr<ServiceMessage> p_ServiceMessage)
{
  const std::string accountId = p_ServiceMessage->profileId;
  AccountState& account = m_Accounts[accountId];

  switch (p_ServiceMessage->GetMessageType())
  {
    case ConnectNotifyType:
      {
        std::shared_ptr<ConnectNotify> connectNotify = std::static_pointer_cast<ConnectNotify>(p_ServiceMessage);
        LOG_TRACE("connect notify %s %d", accountId.c_str(), connectNotify->success);
        if (!HasFeature(accountId, FeatureAutoGetChatsOnLogin))
        {
          SendProtocolRequest(accountId, std::make_shared<GetChatsRequest>());
        }

        if (!HasFeature(accountId, FeatureAutoGetContactsOnLogin))
        {
          SendProtocolRequest(accountId, std::make_shared<GetContactsRequest>());
        }

        if (connectNotify->success)
        {
          std::shared_ptr<SetStatusRequest> setStatusRequest = std::make_shared<SetStatusRequest>();
          setStatusRequest->isOnline = true;
          SendProtocolRequest(accountId, setStatusRequest);
        }

        account.connectTime = TimeUtil::GetCurrentTimeMSec();
        Emit("account.connected", json{ { "accountId", accountId }, { "success", connectNotify->success } });
      }
      break;

    case NewContactsNotifyType:
      {
        std::shared_ptr<NewContactsNotify> newContactsNotify =
          std::static_pointer_cast<NewContactsNotify>(p_ServiceMessage);
        if (newContactsNotify->fullSync)
        {
          account.contactInfos.clear();
        }

        json contacts = json::array();
        for (const auto& contactInfo : newContactsNotify->contactInfos)
        {
          account.contactInfos[contactInfo.id] = contactInfo;
          contacts.push_back(ContactToJson(contactInfo));

          // chat names come from the contact with the same id
          if (account.chatInfos.count(contactInfo.id))
          {
            MarkChatDirty(accountId, contactInfo.id);
          }
        }

        Emit("contacts.updated", json{ { "accountId", accountId }, { "fullSync", newContactsNotify->fullSync },
               { "contacts", contacts } });
      }
      break;

    case NewChatsNotifyType:
      {
        std::shared_ptr<NewChatsNotify> newChatsNotify = std::static_pointer_cast<NewChatsNotify>(p_ServiceMessage);
        if (!newChatsNotify->success) break;

        for (const auto& chatInfo : newChatsNotify->chatInfos)
        {
          account.chatInfos[chatInfo.id] = chatInfo;
          UpdateChatFromMessages(accountId, chatInfo.id);
          MarkChatDirty(accountId, chatInfo.id);
          RequestPreview(accountId, chatInfo.id);
        }
      }
      break;

    case NewMessagesNotifyType:
      OnNewMessages(accountId, std::static_pointer_cast<NewMessagesNotify>(p_ServiceMessage));
      break;

    case SendMessageNotifyType:
      {
        std::shared_ptr<SendMessageNotify> sendMessageNotify =
          std::static_pointer_cast<SendMessageNotify>(p_ServiceMessage);
        LOG_TRACE(sendMessageNotify->success ? "send ok" : "send failed");
        if (!sendMessageNotify->success)
        {
          Emit("message.sendFailed", json{ { "accountId", accountId }, { "chatId", sendMessageNotify->chatId } });
        }
      }
      break;

    case MarkMessageReadNotifyType:
      {
        std::shared_ptr<MarkMessageReadNotify> markMessageReadNotify =
          std::static_pointer_cast<MarkMessageReadNotify>(p_ServiceMessage);
        LOG_TRACE(markMessageReadNotify->success ? "mark read ok" : "mark read failed");
      }
      break;

    case DeleteMessageNotifyType:
      {
        std::shared_ptr<DeleteMessageNotify> deleteMessageNotify =
          std::static_pointer_cast<DeleteMessageNotify>(p_ServiceMessage);
        if (!deleteMessageNotify->success) break;

        const std::string& chatId = deleteMessageNotify->chatId;
        const std::string& msgId = deleteMessageNotify->msgId;
        account.chats[chatId].messages.erase(msgId);
        Emit("message.removed", json{ { "accountId", accountId }, { "chatId", chatId }, { "msgId", msgId } });
        UpdateChatFromMessages(accountId, chatId);
      }
      break;

    case SendTypingNotifyType:
    case SetStatusNotifyType:
      break;

    case NewMessageStatusNotifyType:
      {
        std::shared_ptr<NewMessageStatusNotify> newMessageStatusNotify =
          std::static_pointer_cast<NewMessageStatusNotify>(p_ServiceMessage);
        const std::string& chatId = newMessageStatusNotify->chatId;
        auto& messages = account.chats[chatId].messages;
        auto mit = messages.find(newMessageStatusNotify->msgId);
        if (mit != messages.end())
        {
          mit->second.isRead = newMessageStatusNotify->isRead;
          Emit("message.updated", MessageToJson(accountId, chatId, mit->second));
        }

        UpdateChatFromMessages(accountId, chatId);
      }
      break;

    case NewMessageIsPinnedNotifyType:
      {
        std::shared_ptr<NewMessageIsPinnedNotify> newMessageIsPinnedNotify =
          std::static_pointer_cast<NewMessageIsPinnedNotify>(p_ServiceMessage);
        const std::string& chatId = newMessageIsPinnedNotify->chatId;
        auto& messages = account.chats[chatId].messages;
        auto mit = messages.find(newMessageIsPinnedNotify->msgId);
        if (mit != messages.end())
        {
          mit->second.isPinned = newMessageIsPinnedNotify->isPinned;
          Emit("message.updated", MessageToJson(accountId, chatId, mit->second));
        }
      }
      break;

    case NewMessageFileNotifyType:
      {
        std::shared_ptr<NewMessageFileNotify> newMessageFileNotify =
          std::static_pointer_cast<NewMessageFileNotify>(p_ServiceMessage);
        const std::string& chatId = newMessageFileNotify->chatId;
        auto& messages = account.chats[chatId].messages;
        auto mit = messages.find(newMessageFileNotify->msgId);
        if (mit != messages.end())
        {
          mit->second.fileInfo = newMessageFileNotify->fileInfo;
          Emit("message.updated", MessageToJson(accountId, chatId, mit->second));
        }
      }
      break;

    case NewMessageReactionsNotifyType:
      {
        std::shared_ptr<NewMessageReactionsNotify> newMessageReactionsNotify =
          std::static_pointer_cast<NewMessageReactionsNotify>(p_ServiceMessage);

        // reactions needing consolidation are merged by MessageCache, which then
        // sends the consolidated reactions again
        if (newMessageReactionsNotify->reactions.needConsolidationWithCache) break;

        const std::string& chatId = newMessageReactionsNotify->chatId;
        auto& messages = account.chats[chatId].messages;
        auto mit = messages.find(newMessageReactionsNotify->msgId);
        if (mit != messages.end())
        {
          mit->second.reactions = newMessageReactionsNotify->reactions;
          Emit("message.updated", MessageToJson(accountId, chatId, mit->second));
        }
      }
      break;

    case ReceiveTypingNotifyType:
      {
        std::shared_ptr<ReceiveTypingNotify> receiveTypingNotify =
          std::static_pointer_cast<ReceiveTypingNotify>(p_ServiceMessage);
        const std::string& chatId = receiveTypingNotify->chatId;
        const std::string& userId = receiveTypingNotify->userId;
        if (receiveTypingNotify->isTyping)
        {
          account.usersTyping[chatId].insert(userId);
        }
        else
        {
          account.usersTyping[chatId].erase(userId);
        }

        Emit("typing", json{ { "accountId", accountId }, { "chatId", chatId }, { "userId", userId },
               { "userName", GetContactName(accountId, userId) },
               { "isTyping", receiveTypingNotify->isTyping } });
      }
      break;

    case ReceiveStatusNotifyType:
      {
        std::shared_ptr<ReceiveStatusNotify> receiveStatusNotify =
          std::static_pointer_cast<ReceiveStatusNotify>(p_ServiceMessage);
        const std::string& userId = receiveStatusNotify->userId;
        account.userOnline[userId] = receiveStatusNotify->isOnline;
        if (receiveStatusNotify->timeSeen != -1)
        {
          account.userTimeSeen[userId] = receiveStatusNotify->timeSeen;
        }

        Emit("presence", json{ { "accountId", accountId }, { "userId", userId },
               { "isOnline", receiveStatusNotify->isOnline },
               { "timeSeen", receiveStatusNotify->timeSeen } });
      }
      break;

    case CreateChatNotifyType:
      {
        std::shared_ptr<CreateChatNotify> createChatNotify =
          std::static_pointer_cast<CreateChatNotify>(p_ServiceMessage);
        if (!createChatNotify->success) break;

        const ChatInfo& chatInfo = createChatNotify->chatInfo;
        account.chatInfos[chatInfo.id] = chatInfo;
        MarkChatDirty(accountId, chatInfo.id);
        Emit("chat.created", json{ { "accountId", accountId }, { "chatId", chatInfo.id } });
      }
      break;

    case DeleteChatNotifyType:
      {
        std::shared_ptr<DeleteChatNotify> deleteChatNotify =
          std::static_pointer_cast<DeleteChatNotify>(p_ServiceMessage);
        if (!deleteChatNotify->success) break;

        const std::string& chatId = deleteChatNotify->chatId;
        account.chatInfos.erase(chatId);
        account.chats.erase(chatId);
        account.usersTyping.erase(chatId);
        account.groupMembers.erase(chatId);
        m_DirtyChats.erase(std::make_pair(accountId, chatId));
        m_EmittedChats.erase(std::make_pair(accountId, chatId));
        Emit("chat.removed", json{ { "accountId", accountId }, { "chatId", chatId } });
      }
      break;

    case MoveChatNotifyType:
      {
        std::shared_ptr<MoveChatNotify> moveChatNotify = std::static_pointer_cast<MoveChatNotify>(p_ServiceMessage);
        const std::string& chatId = moveChatNotify->chatId;
        const std::string& newChatId = moveChatNotify->newChatId;
        if (!account.chatInfos.count(chatId)) break;

        LOG_TRACE("chat moved %s to %s", chatId.c_str(), newChatId.c_str());

        // replace old chat with new chat, merging into new chat if already present
        const ChatInfo oldChatInfo = account.chatInfos[chatId];
        account.chatInfos.erase(chatId);
        if (account.chatInfos.count(newChatId))
        {
          ChatInfo& newChatInfo = account.chatInfos[newChatId];
          newChatInfo.lastMessageTime = std::max(newChatInfo.lastMessageTime, oldChatInfo.lastMessageTime);
          newChatInfo.isUnread = newChatInfo.isUnread || oldChatInfo.isUnread;
        }
        else
        {
          ChatInfo newChatInfo = oldChatInfo;
          newChatInfo.id = newChatId;
          account.chatInfos[newChatId] = newChatInfo;
        }

        // reset message state, the clients re-fetch merged messages from the cache
        account.chats.erase(chatId);
        account.chats.erase(newChatId);
        m_DirtyChats.erase(std::make_pair(accountId, chatId));
        m_EmittedChats.erase(std::make_pair(accountId, chatId));
        if ((m_CurrentChat.first == accountId) && (m_CurrentChat.second == chatId))
        {
          m_CurrentChat.second = newChatId;
        }

        Emit("chat.moved", json{ { "accountId", accountId }, { "chatId", chatId }, { "newChatId", newChatId } });
        MarkChatDirty(accountId, newChatId);
        RequestPreview(accountId, newChatId);
      }
      break;

    case UpdateMuteNotifyType:
      {
        std::shared_ptr<UpdateMuteNotify> updateMuteNotify =
          std::static_pointer_cast<UpdateMuteNotify>(p_ServiceMessage);
        const std::string& chatId = updateMuteNotify->chatId;
        if (!account.chatInfos.count(chatId)) break;

        account.chatInfos[chatId].isMuted = updateMuteNotify->isMuted;
        MarkChatDirty(accountId, chatId);
      }
      break;

    case UpdatePinNotifyType:
      {
        std::shared_ptr<UpdatePinNotify> updatePinNotify = std::static_pointer_cast<UpdatePinNotify>(p_ServiceMessage);
        const std::string& chatId = updatePinNotify->chatId;
        if (!account.chatInfos.count(chatId)) break;

        ChatInfo& chatInfo = account.chatInfos[chatId];
        chatInfo.isPinned = updatePinNotify->isPinned;
        if (updatePinNotify->isPinned)
        {
          // for pinned chats nchat stores the pin time as sort key
          chatInfo.lastMessageTime = updatePinNotify->timePinned;
        }

        UpdateChatFromMessages(accountId, chatId);
        MarkChatDirty(accountId, chatId);
      }
      break;

    case UpdateArchivedNotifyType:
      {
        std::shared_ptr<UpdateArchivedNotify> updateArchivedNotify =
          std::static_pointer_cast<UpdateArchivedNotify>(p_ServiceMessage);
        const std::string& chatId = updateArchivedNotify->chatId;
        if (!account.chatInfos.count(chatId)) break;

        account.chatInfos[chatId].isArchived = updateArchivedNotify->isArchived;
        if (!updateArchivedNotify->isArchived)
        {
          // request fresh chat details to get correct lastMessageTime
          std::shared_ptr<GetChatsRequest> getChatsRequest = std::make_shared<GetChatsRequest>();
          getChatsRequest->chatIds.insert(chatId);
          SendProtocolRequest(accountId, getChatsRequest);
        }

        MarkChatDirty(accountId, chatId);
      }
      break;

    case NewGroupMembersNotifyType:
      {
        std::shared_ptr<NewGroupMembersNotify> notify =
          std::static_pointer_cast<NewGroupMembersNotify>(p_ServiceMessage);
        const std::string& chatId = notify->chatId;

        // update contacts (names may be new), preserving isSelf flag
        std::vector<std::string> memberIds;
        for (auto contactInfo : notify->contactInfos)
        {
          memberIds.push_back(contactInfo.id);
          if (contactInfo.name.empty()) continue;

          auto existIt = account.contactInfos.find(contactInfo.id);
          if (existIt != account.contactInfos.end())
          {
            contactInfo.isSelf = existIt->second.isSelf;
          }

          account.contactInfos[contactInfo.id] = contactInfo;
        }

        account.groupMembers[chatId] = memberIds;
        ResolvePending(PendingMembers, accountId, chatId, "", false /*p_TimedOut*/);
        Emit("members.updated", MembersJson(accountId, chatId));
      }
      break;

    case AvailableReactionsNotifyType:
      {
        std::shared_ptr<AvailableReactionsNotify> availableReactionsNotify =
          std::static_pointer_cast<AvailableReactionsNotify>(p_ServiceMessage);
        const std::string& chatId = availableReactionsNotify->chatId;
        for (auto it = m_Pending.lower_bound(PendingReactions); it != m_Pending.upper_bound(PendingReactions);
             /* incremented in loop */)
        {
          if ((it->second.accountId == accountId) && (it->second.chatId == chatId))
          {
            json emojis = json::array();
            for (const auto& emoji : availableReactionsNotify->emojis)
            {
              emojis.push_back(emoji);
            }

            it->second.reply.Ok(json{ { "limited", true }, { "emojis", emojis } });
            it = m_Pending.erase(it);
          }
          else
          {
            ++it;
          }
        }
      }
      break;

    case FindMessageNotifyType:
      {
        std::shared_ptr<FindMessageNotify> findMessageNotify =
          std::static_pointer_cast<FindMessageNotify>(p_ServiceMessage);
        const std::string& chatId = findMessageNotify->chatId;
        for (auto it = m_Pending.lower_bound(PendingFind); it != m_Pending.upper_bound(PendingFind);
             /* incremented in loop */)
        {
          if ((it->second.accountId == accountId) && (it->second.chatId == chatId))
          {
            const bool found = findMessageNotify->success && !findMessageNotify->msgId.empty();
            it->second.reply.Ok(json{ { "found", found }, { "msgId", findMessageNotify->msgId } });
            it = m_Pending.erase(it);
            break; // one reply per notify
          }
          else
          {
            ++it;
          }
        }
      }
      break;

    case ProtocolUiControlNotifyType:
    case RequestAppExitNotifyType:
      // terminal ui specific, auth steps are handled through AuthHandler
      LOG_DEBUG("ignoring ui notify %d", p_ServiceMessage->GetMessageType());
      break;

    default:
      LOG_DEBUG("unknown service message %d", p_ServiceMessage->GetMessageType());
      break;
  }

  FlushDirtyChats();
}

void ChatModel::OnNewMessages(const std::string& p_AccountId, std::shared_ptr<NewMessagesNotify> p_Notify)
{
  if (!p_Notify->success) return;

  AccountState& account = m_Accounts[p_AccountId];
  const std::string& chatId = p_Notify->chatId;
  ChatState& chat = account.chats[chatId];
  const bool isPage = p_Notify->sequence || p_Notify->cached;

  // history pages requested through the api are delivered in the reply, not as events
  bool hasPendingPage = false;
  if (isPage)
  {
    for (auto it = m_Pending.lower_bound(PendingMessages); it != m_Pending.upper_bound(PendingMessages); ++it)
    {
      if ((it->second.accountId == p_AccountId) && (it->second.chatId == chatId) &&
          (it->second.key == p_Notify->fromMsgId))
      {
        hasPendingPage = true;
        break;
      }
    }
  }

  LOG_TRACE("new messages %s count %d from %s cached %d", chatId.c_str(), (int)p_Notify->chatMessages.size(),
            p_Notify->fromMsgId.c_str(), p_Notify->cached);

  for (const auto& chatMessage : p_Notify->chatMessages)
  {
    auto mit = chat.messages.find(chatMessage.id);
    if (mit == chat.messages.end())
    {
      chat.messages[chatMessage.id] = chatMessage;
      if (!hasPendingPage)
      {
        Emit("message.added", MessageToJson(p_AccountId, chatId, chatMessage));
      }

      if (!p_Notify->cached)
      {
        MaybeNotify(p_AccountId, chatId, chatMessage);
      }
    }
    else if (!IsSameMessage(mit->second, chatMessage))
    {
      mit->second = chatMessage;
      Emit("message.updated", MessageToJson(p_AccountId, chatId, chatMessage));
    }
  }

  if (!account.chatInfos.count(chatId))
  {
    LOG_TRACE("new message in unknown chat, get chat %s", chatId.c_str());
    std::shared_ptr<GetChatsRequest> getChatsRequest = std::make_shared<GetChatsRequest>();
    getChatsRequest->chatIds.insert(chatId);
    SendProtocolRequest(p_AccountId, getChatsRequest);
  }

  UpdateChatFromMessages(p_AccountId, chatId);

  if (hasPendingPage)
  {
    ResolvePending(PendingMessages, p_AccountId, chatId, p_Notify->fromMsgId, false /*p_TimedOut*/);
  }
}

const ChatMessage* ChatModel::GetLastMessage(const std::string& p_AccountId, const std::string& p_ChatId)
{
  const ChatMessage* last = nullptr;
  const auto& messages = m_Accounts[p_AccountId].chats[p_ChatId].messages;
  for (const auto& message : messages)
  {
    const ChatMessage& chatMessage = message.second;
    if (chatMessage.timeSent == s_SponsoredTime) continue; // skip sponsored messages

    if ((last == nullptr) || (chatMessage.timeSent > last->timeSent) ||
        ((chatMessage.timeSent == last->timeSent) && (chatMessage.id > last->id)))
    {
      last = &chatMessage;
    }
  }

  return last;
}

void ChatModel::UpdateChatFromMessages(const std::string& p_AccountId, const std::string& p_ChatId)
{
  AccountState& account = m_Accounts[p_AccountId];
  auto cit = account.chatInfos.find(p_ChatId);
  if (cit == account.chatInfos.end()) return;

  ChatInfo& chatInfo = cit->second;
  const ChatMessage* lastMessage = GetLastMessage(p_AccountId, p_ChatId);
  if (lastMessage != nullptr)
  {
    if (!chatInfo.isPinned)
    {
      chatInfo.lastMessageTime = lastMessage->timeSent;
    }

    chatInfo.isUnread = lastMessage->isOutgoing ? false : !lastMessage->isRead;
  }

  MarkChatDirty(p_AccountId, p_ChatId);
}

void ChatModel::RequestPreview(const std::string& p_AccountId, const std::string& p_ChatId)
{
  // fetch the latest message of each chat for the chat list preview
  ChatState& chat = m_Accounts[p_AccountId].chats[p_ChatId];
  if (chat.previewRequested || !chat.messages.empty()) return;

  chat.previewRequested = true;
  std::shared_ptr<GetMessagesRequest> getMessagesRequest = std::make_shared<GetMessagesRequest>();
  getMessagesRequest->chatId = p_ChatId;
  getMessagesRequest->limit = 1;
  SendProtocolRequest(p_AccountId, getMessagesRequest);
}

void ChatModel::MaybeNotify(const std::string& p_AccountId, const std::string& p_ChatId,
                            const ChatMessage& p_ChatMessage)
{
  AccountState& account = m_Accounts[p_AccountId];
  if (p_ChatMessage.isOutgoing || p_ChatMessage.isRead) return;

  // only messages received after connect, i.e. not the backlog
  if ((account.connectTime == 0) || (p_ChatMessage.timeSent <= account.connectTime)) return;

  if (p_ChatMessage.timeSent == s_SponsoredTime) return;

  auto cit = account.chatInfos.find(p_ChatId);
  if (cit != account.chatInfos.end())
  {
    if (cit->second.isArchived) return;

    if (cit->second.isMuted && !p_ChatMessage.hasMention) return;
  }

  std::shared_ptr<Protocol> protocol = m_ProtocolGetter(p_AccountId);
  const bool isGroup = protocol && protocol->IsGroupChat(p_ChatId);
  Emit("notification", json{
    { "accountId", p_AccountId },
    { "chatId", p_ChatId },
    { "msgId", p_ChatMessage.id },
    { "chatName", GetContactName(p_AccountId, p_ChatId) },
    { "senderName", GetContactName(p_AccountId, p_ChatMessage.senderId) },
    { "isGroup", isGroup },
    { "hasMention", p_ChatMessage.hasMention },
    { "text", p_ChatMessage.text },
    { "hasFile", !p_ChatMessage.fileInfo.empty() },
  });
}

void ChatModel::ResolvePending(PendingType p_Type, const std::string& p_AccountId, const std::string& p_ChatId,
                               const std::string& p_Key, bool p_TimedOut)
{
  for (auto it = m_Pending.lower_bound(p_Type); it != m_Pending.upper_bound(p_Type); /* incremented in loop */)
  {
    const PendingReply& pending = it->second;
    if ((pending.accountId != p_AccountId) || (pending.chatId != p_ChatId) || (pending.key != p_Key))
    {
      ++it;
      continue;
    }

    switch (p_Type)
    {
      case PendingMessages:
        {
          json result = MessagesPage(p_AccountId, p_ChatId, p_Key, pending.limit);
          result["timedOut"] = p_TimedOut;
          pending.reply.Ok(result);
        }
        break;

      case PendingMembers:
        pending.reply.Ok(MembersJson(p_AccountId, p_ChatId));
        break;

      case PendingReactions:
        pending.reply.Ok(json{ { "limited", true }, { "emojis", json::array() }, { "timedOut", p_TimedOut } });
        break;

      case PendingFind:
        pending.reply.Ok(json{ { "found", false }, { "msgId", "" }, { "timedOut", p_TimedOut } });
        break;

      default:
        break;
    }

    it = m_Pending.erase(it);
  }
}

void ChatModel::ProcessTimers(int64_t p_NowMs)
{
  std::vector<PendingReply> expired;
  std::vector<PendingType> expiredTypes;
  for (auto it = m_Pending.begin(); it != m_Pending.end(); ++it)
  {
    if (it->second.deadline <= p_NowMs)
    {
      expired.push_back(it->second);
      expiredTypes.push_back(it->first);
    }
  }

  for (size_t i = 0; i < expired.size(); ++i)
  {
    LOG_DEBUG("pending request %d timed out", expiredTypes[i]);
    ResolvePending(expiredTypes[i], expired[i].accountId, expired[i].chatId, expired[i].key, true /*p_TimedOut*/);
  }
}

json ChatModel::MessagesPage(const std::string& p_AccountId, const std::string& p_ChatId,
                             const std::string& p_FromMsgId, int p_Limit)
{
  const auto& messages = m_Accounts[p_AccountId].chats[p_ChatId].messages;
  int64_t fromTime = std::numeric_limits<int64_t>::max();
  if (!p_FromMsgId.empty())
  {
    auto fit = messages.find(p_FromMsgId);
    if (fit != messages.end())
    {
      fromTime = fit->second.timeSent;
    }
  }

  // messages older than fromMsgId; messages with the same time are included as
  // their order relative to fromMsgId is unknown (clients de-duplicate by id)
  std::vector<const ChatMessage*> older;
  std::vector<const ChatMessage*> sameTime;
  for (const auto& message : messages)
  {
    const ChatMessage& chatMessage = message.second;
    if (chatMessage.id == p_FromMsgId) continue;

    if (chatMessage.timeSent < fromTime)
    {
      older.push_back(&chatMessage);
    }
    else if ((chatMessage.timeSent == fromTime) && !p_FromMsgId.empty())
    {
      sameTime.push_back(&chatMessage);
    }
  }

  std::sort(older.begin(), older.end(), [](const ChatMessage* p_Lhs, const ChatMessage* p_Rhs)
  {
    return (p_Lhs->timeSent > p_Rhs->timeSent) || ((p_Lhs->timeSent == p_Rhs->timeSent) && (p_Lhs->id > p_Rhs->id));
  });

  if ((int)older.size() > p_Limit)
  {
    older.resize(p_Limit);
  }

  older.insert(older.end(), sameTime.begin(), sameTime.end());
  std::sort(older.begin(), older.end(), [](const ChatMessage* p_Lhs, const ChatMessage* p_Rhs)
  {
    return (p_Lhs->timeSent < p_Rhs->timeSent) || ((p_Lhs->timeSent == p_Rhs->timeSent) && (p_Lhs->id < p_Rhs->id));
  });

  json list = json::array();
  for (const ChatMessage* chatMessage : older)
  {
    list.push_back(MessageToJson(p_AccountId, p_ChatId, *chatMessage));
  }

  return json{ { "accountId", p_AccountId }, { "chatId", p_ChatId }, { "fromMsgId", p_FromMsgId },
    { "messages", list } };
}

json ChatModel::MembersJson(const std::string& p_AccountId, const std::string& p_ChatId)
{
  AccountState& account = m_Accounts[p_AccountId];
  json members = json::array();
  for (const auto& memberId : account.groupMembers[p_ChatId])
  {
    auto cit = account.contactInfos.find(memberId);
    if (cit != account.contactInfos.end())
    {
      members.push_back(ContactToJson(cit->second));
    }
    else
    {
      members.push_back(json{ { "id", memberId }, { "name", memberId }, { "phone", "" }, { "isSelf", false } });
    }
  }

  return json{ { "accountId", p_AccountId }, { "chatId", p_ChatId }, { "members", members } };
}

std::map<std::string, std::string> ChatModel::ParseMentions(const std::string& p_AccountId,
                                                            const std::string& p_ChatId,
                                                            const std::string& p_Text)
{
  std::map<std::string, std::string> mentions;
  AccountState& account = m_Accounts[p_AccountId];
  const std::vector<std::string>& memberIds = account.groupMembers[p_ChatId];
  if (memberIds.empty()) return mentions;

  // build name -> memberId map
  std::map<std::string, std::string> nameMemberMap;
  for (const auto& memberId : memberIds)
  {
    auto cit = account.contactInfos.find(memberId);
    if ((cit != account.contactInfos.end()) && !cit->second.name.empty())
    {
      nameMemberMap[cit->second.name] = memberId;
    }
  }

  if (nameMemberMap.empty()) return mentions;

  // scan text for @[Name With Spaces] and @Name patterns
  size_t pos = 0;
  while (pos < p_Text.size())
  {
    if (p_Text[pos] != '@')
    {
      ++pos;
      continue;
    }

    if (((pos + 1) < p_Text.size()) && (p_Text[pos + 1] == '['))
    {
      // bracket-quoted mention: @[Name With Spaces]
      size_t closePos = p_Text.find(']', pos + 2);
      if (closePos != std::string::npos)
      {
        std::string name = p_Text.substr(pos + 2, closePos - pos - 2);
        auto it = nameMemberMap.find(name);
        if (it != nameMemberMap.end())
        {
          mentions[name] = it->second;
        }

        pos = closePos + 1;
        continue;
      }
    }

    // simple mention: @Word
    size_t end = pos + 1;
    while ((end < p_Text.size()) && (p_Text[end] != ' ') && (p_Text[end] != '\n') && (p_Text[end] != '\t'))
    {
      ++end;
    }

    if (end > (pos + 1))
    {
      std::string name = p_Text.substr(pos + 1, end - pos - 1);
      auto it = nameMemberMap.find(name);
      if (it == nameMemberMap.end())
      {
        // trim trailing punctuation (e.g. @Name, or @Name.) and retry
        while (!name.empty() && ((name.back() == '.') || (name.back() == ',') || (name.back() == '!') ||
                                 (name.back() == '?') || (name.back() == ':') || (name.back() == ';')))
        {
          name.pop_back();
        }

        it = nameMemberMap.find(name);
      }

      if (it != nameMemberMap.end())
      {
        mentions[name] = it->second;
      }
    }

    pos = end;
  }

  return mentions;
}

// API requests

std::shared_ptr<Protocol> ChatModel::GetProtocolOrThrow(const std::string& p_AccountId)
{
  std::shared_ptr<Protocol> protocol = m_ProtocolGetter(p_AccountId);
  if (!protocol)
  {
    throw ApiException(ApiErrorUnknownAccount, "unknown account " + p_AccountId);
  }

  return protocol;
}

ChatModel::AccountState& ChatModel::GetAccountOrThrow(const std::string& p_AccountId)
{
  GetProtocolOrThrow(p_AccountId);
  return m_Accounts[p_AccountId];
}

ChatMessage& ChatModel::GetMessageOrThrow(const std::string& p_AccountId, const std::string& p_ChatId,
                                          const std::string& p_MsgId)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  auto cit = account.chats.find(p_ChatId);
  if (cit != account.chats.end())
  {
    auto mit = cit->second.messages.find(p_MsgId);
    if (mit != cit->second.messages.end()) return mit->second;
  }

  throw ApiException(ApiErrorUnknownMessage, "unknown message " + p_MsgId);
}

void ChatModel::SendProtocolRequest(const std::string& p_AccountId, std::shared_ptr<RequestMessage> p_Request)
{
  std::shared_ptr<Protocol> protocol = m_ProtocolGetter(p_AccountId);
  if (!protocol)
  {
    LOG_WARNING("no protocol for %s, request %d dropped", p_AccountId.c_str(), p_Request->GetMessageType());
    return;
  }

  protocol->SendRequest(p_Request);
}

bool ChatModel::HasFeature(const std::string& p_AccountId, ProtocolFeature p_Feature)
{
  std::shared_ptr<Protocol> protocol = m_ProtocolGetter(p_AccountId);
  return protocol && protocol->HasFeature(p_Feature);
}

json ChatModel::GetChats(const std::string& p_AccountId, bool p_IncludeArchived)
{
  std::vector<std::pair<std::string, const ChatInfo*>> chats;
  for (const auto& account : m_Accounts)
  {
    if (!p_AccountId.empty() && (account.first != p_AccountId)) continue;

    if (!m_ProtocolGetter(account.first)) continue;

    for (const auto& chatInfo : account.second.chatInfos)
    {
      if (chatInfo.second.isArchived && !p_IncludeArchived) continue;

      chats.push_back(std::make_pair(account.first, &chatInfo.second));
    }
  }

  // pinned first, then most recent activity, then chat id for a stable order
  std::sort(chats.begin(), chats.end(),
            [](const std::pair<std::string, const ChatInfo*>& p_Lhs,
               const std::pair<std::string, const ChatInfo*>& p_Rhs) -> bool
  {
    const ChatInfo& lhs = *p_Lhs.second;
    const ChatInfo& rhs = *p_Rhs.second;
    if (lhs.isPinned != rhs.isPinned) return lhs.isPinned;

    if (lhs.lastMessageTime != rhs.lastMessageTime) return lhs.lastMessageTime > rhs.lastMessageTime;

    if (p_Lhs.first != p_Rhs.first) return p_Lhs.first < p_Rhs.first;

    return lhs.id < rhs.id;
  });

  json list = json::array();
  for (const auto& chat : chats)
  {
    list.push_back(ChatToJson(chat.first, *chat.second));
  }

  return list;
}

json ChatModel::GetChat(const std::string& p_AccountId, const std::string& p_ChatId)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  auto cit = account.chatInfos.find(p_ChatId);
  if (cit == account.chatInfos.end())
  {
    throw ApiException(ApiErrorUnknownChat, "unknown chat " + p_ChatId);
  }

  return ChatToJson(p_AccountId, cit->second);
}

json ChatModel::GetContacts(const std::string& p_AccountId)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  json list = json::array();
  for (const auto& contactInfo : account.contactInfos)
  {
    list.push_back(ContactToJson(contactInfo.second));
  }

  return list;
}

void ChatModel::GetMessages(const std::string& p_AccountId, const std::string& p_ChatId,
                            const std::string& p_FromMsgId, int p_Limit, const Reply& p_Reply)
{
  GetAccountOrThrow(p_AccountId);
  if (p_Limit <= 0)
  {
    throw ApiException(ApiErrorInvalidParams, "limit must be positive");
  }

  PendingReply pending;
  pending.accountId = p_AccountId;
  pending.chatId = p_ChatId;
  pending.key = p_FromMsgId;
  pending.limit = p_Limit;
  pending.deadline = TimeUtil::GetCurrentTimeMSec() + s_PendingTimeoutMs;
  pending.reply = p_Reply;
  m_Pending.insert(std::make_pair(PendingMessages, pending));

  // protocols serve history from MessageCache first and fetch from the server otherwise
  static const int minLimit = 12; // up to 10 tgchat messages may share the same timestamp
  std::shared_ptr<GetMessagesRequest> getMessagesRequest = std::make_shared<GetMessagesRequest>();
  getMessagesRequest->chatId = p_ChatId;
  getMessagesRequest->fromMsgId = p_FromMsgId;
  getMessagesRequest->limit = std::max(p_Limit, minLimit);
  SendProtocolRequest(p_AccountId, getMessagesRequest);
}

void ChatModel::SendMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_Text,
                            const std::string& p_QuotedId, const std::string& p_FilePath)
{
  GetAccountOrThrow(p_AccountId);
  if (p_Text.empty() && p_FilePath.empty())
  {
    throw ApiException(ApiErrorInvalidParams, "text or filePath required");
  }

  std::shared_ptr<SendMessageRequest> sendMessageRequest = std::make_shared<SendMessageRequest>();
  sendMessageRequest->chatId = p_ChatId;
  sendMessageRequest->chatMessage.text = p_Text;
  sendMessageRequest->chatMessage.mentions = ParseMentions(p_AccountId, p_ChatId, p_Text);

  if (!p_QuotedId.empty())
  {
    const ChatMessage& quoted = GetMessageOrThrow(p_AccountId, p_ChatId, p_QuotedId);
    sendMessageRequest->chatMessage.quotedId = quoted.id;
    sendMessageRequest->chatMessage.quotedText = quoted.text;
    sendMessageRequest->chatMessage.quotedSender = quoted.senderId;
  }

  if (!p_FilePath.empty())
  {
    if ((p_FilePath.at(0) != '/') || !FileUtil::Exists(p_FilePath))
    {
      throw ApiException(ApiErrorInvalidParams, "filePath must be an existing absolute path");
    }

    FileInfo fileInfo;
    fileInfo.filePath = p_FilePath;
    fileInfo.fileType = FileUtil::GetMimeType(p_FilePath);
    sendMessageRequest->chatMessage.fileInfo = ProtocolUtil::FileInfoToHex(fileInfo);
  }

  SendProtocolRequest(p_AccountId, sendMessageRequest);
}

void ChatModel::EditMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId,
                            const std::string& p_Text)
{
  const ChatMessage& chatMessage = GetMessageOrThrow(p_AccountId, p_ChatId, p_MsgId);
  if (!chatMessage.isOutgoing)
  {
    throw ApiException(ApiErrorUnsupported, "only own messages can be edited");
  }

  if (p_Text.empty())
  {
    throw ApiException(ApiErrorInvalidParams, "text required");
  }

  int64_t editWindowMs = 0;
  if (HasFeature(p_AccountId, FeatureEditMessagesAnytime))
  {
    editWindowMs = -1;
  }
  else if (HasFeature(p_AccountId, FeatureEditMessagesWithinTwoDays))
  {
    editWindowMs = 48 * 3600 * 1000LL;
  }
  else if (HasFeature(p_AccountId, FeatureEditMessagesWithinFifteenMins))
  {
    editWindowMs = 15 * 60 * 1000LL;
  }

  if (editWindowMs == 0)
  {
    throw ApiException(ApiErrorUnsupported, "editing is not supported");
  }

  if ((editWindowMs > 0) && ((TimeUtil::GetCurrentTimeMSec() - chatMessage.timeSent) > editWindowMs))
  {
    throw ApiException(ApiErrorUnsupported, "edit window has expired");
  }

  std::shared_ptr<EditMessageRequest> editMessageRequest = std::make_shared<EditMessageRequest>();
  editMessageRequest->chatId = p_ChatId;
  editMessageRequest->msgId = p_MsgId;

  // copy original message (time sent, quote id, etc)
  editMessageRequest->chatMessage = chatMessage;

  // prepare a new FileInfo struct if original message has a file
  if (!chatMessage.fileInfo.empty())
  {
    FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(chatMessage.fileInfo);
    fileInfo.fileType = FileUtil::GetMimeType(fileInfo.filePath);
    editMessageRequest->chatMessage.fileInfo = ProtocolUtil::FileInfoToHex(fileInfo);
  }

  editMessageRequest->chatMessage.text = p_Text;
  editMessageRequest->chatMessage.mentions = ParseMentions(p_AccountId, p_ChatId, p_Text);
  SendProtocolRequest(p_AccountId, editMessageRequest);
}

void ChatModel::DeleteMessage(const std::string& p_AccountId, const std::string& p_ChatId,
                              const std::string& p_MsgId)
{
  const ChatMessage& chatMessage = GetMessageOrThrow(p_AccountId, p_ChatId, p_MsgId);
  if (chatMessage.isDeleted)
  {
    // already deleted (by other user): remove locally only
    MessageCache::DeleteOneMessage(p_AccountId, p_ChatId, p_MsgId);
    std::shared_ptr<DeleteMessageNotify> deleteMessageNotify = std::make_shared<DeleteMessageNotify>(p_AccountId);
    deleteMessageNotify->success = true;
    deleteMessageNotify->chatId = p_ChatId;
    deleteMessageNotify->msgId = p_MsgId;
    deleteMessageNotify->isOutgoing = true;
    OnServiceMessage(deleteMessageNotify);
    return;
  }

  std::shared_ptr<DeleteMessageRequest> deleteMessageRequest = std::make_shared<DeleteMessageRequest>();
  deleteMessageRequest->chatId = p_ChatId;
  deleteMessageRequest->senderId = chatMessage.senderId;
  deleteMessageRequest->msgId = p_MsgId;
  SendProtocolRequest(p_AccountId, deleteMessageRequest);
}

void ChatModel::PinMessage(const std::string& p_AccountId, const std::string& p_ChatId,
                           const std::string& p_MsgId, bool p_IsPinned)
{
  if (!HasFeature(p_AccountId, FeaturePinMessage))
  {
    throw ApiException(ApiErrorUnsupported, "pinning messages is not supported by this protocol");
  }

  const ChatMessage& chatMessage = GetMessageOrThrow(p_AccountId, p_ChatId, p_MsgId);
  std::shared_ptr<PinMessageRequest> pinMessageRequest = std::make_shared<PinMessageRequest>();
  pinMessageRequest->chatId = p_ChatId;
  pinMessageRequest->senderId = chatMessage.senderId;
  pinMessageRequest->msgId = p_MsgId;
  pinMessageRequest->isPinned = p_IsPinned;
  SendProtocolRequest(p_AccountId, pinMessageRequest);
}

void ChatModel::MarkRead(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId)
{
  const ChatMessage& target = GetMessageOrThrow(p_AccountId, p_ChatId, p_MsgId);
  const int64_t targetTime = target.timeSent;
  const bool markReadEveryView = HasFeature(p_AccountId, FeatureMarkReadEveryView);
  const bool perMessage = HasPerMessageReadReceipts(StrUtil::GetProtocolName(p_AccountId));

  // the read state is modelled as "read up to message X"
  std::vector<ChatMessage*> unread;
  for (auto& message : m_Accounts[p_AccountId].chats[p_ChatId].messages)
  {
    ChatMessage& chatMessage = message.second;
    if (chatMessage.isOutgoing || (chatMessage.timeSent > targetTime)) continue;

    if (!chatMessage.isRead || (markReadEveryView && (chatMessage.id == p_MsgId)))
    {
      unread.push_back(&chatMessage);
    }
  }

  std::sort(unread.begin(), unread.end(), [](const ChatMessage* p_Lhs, const ChatMessage* p_Rhs)
  {
    return p_Lhs->timeSent < p_Rhs->timeSent;
  });

  for (size_t i = 0; i < unread.size(); ++i)
  {
    ChatMessage* chatMessage = unread[i];
    const bool isLast = ((i + 1) == unread.size());
    if (perMessage || isLast)
    {
      std::shared_ptr<MarkMessageReadRequest> markMessageReadRequest = std::make_shared<MarkMessageReadRequest>();
      markMessageReadRequest->chatId = p_ChatId;
      markMessageReadRequest->msgId = chatMessage->id;
      markMessageReadRequest->senderId = chatMessage->senderId;
      SendProtocolRequest(p_AccountId, markMessageReadRequest);
    }

    chatMessage->isRead = true;
  }

  UpdateChatFromMessages(p_AccountId, p_ChatId);
  FlushDirtyChats();
}

void ChatModel::SendReaction(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId,
                             const std::string& p_Emoji)
{
  const ChatMessage& chatMessage = GetMessageOrThrow(p_AccountId, p_ChatId, p_MsgId);
  std::string selfEmoji;
  auto sit = chatMessage.reactions.senderEmojis.find(s_ReactionsSelfId);
  if (sit != chatMessage.reactions.senderEmojis.end())
  {
    selfEmoji = sit->second;
  }

  if (p_Emoji == selfEmoji) return;

  std::shared_ptr<SendReactionRequest> sendReactionRequest = std::make_shared<SendReactionRequest>();
  sendReactionRequest->chatId = p_ChatId;
  sendReactionRequest->senderId = chatMessage.senderId;
  sendReactionRequest->msgId = p_MsgId;
  sendReactionRequest->emoji = p_Emoji;
  sendReactionRequest->prevEmoji = selfEmoji;
  SendProtocolRequest(p_AccountId, sendReactionRequest);
}

void ChatModel::GetAvailableReactions(const std::string& p_AccountId, const std::string& p_ChatId,
                                      const std::string& p_MsgId, const Reply& p_Reply)
{
  GetAccountOrThrow(p_AccountId);
  if (!HasFeature(p_AccountId, FeatureLimitedReactions))
  {
    p_Reply.Ok(json{ { "limited", false }, { "emojis", json::array() } });
    return;
  }

  PendingReply pending;
  pending.accountId = p_AccountId;
  pending.chatId = p_ChatId;
  pending.deadline = TimeUtil::GetCurrentTimeMSec() + s_PendingTimeoutMs;
  pending.reply = p_Reply;
  m_Pending.insert(std::make_pair(PendingReactions, pending));

  std::shared_ptr<GetAvailableReactionsRequest> getAvailableReactionsRequest =
    std::make_shared<GetAvailableReactionsRequest>();
  getAvailableReactionsRequest->chatId = p_ChatId;
  getAvailableReactionsRequest->msgId = p_MsgId;
  SendProtocolRequest(p_AccountId, getAvailableReactionsRequest);
}

void ChatModel::DownloadFile(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId)
{
  ChatMessage& chatMessage = GetMessageOrThrow(p_AccountId, p_ChatId, p_MsgId);
  if (chatMessage.fileInfo.empty())
  {
    throw ApiException(ApiErrorInvalidParams, "message has no attachment");
  }

  FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(chatMessage.fileInfo);
  std::shared_ptr<DownloadFileRequest> downloadFileRequest = std::make_shared<DownloadFileRequest>();
  downloadFileRequest->chatId = p_ChatId;
  downloadFileRequest->msgId = p_MsgId;
  downloadFileRequest->fileId = fileInfo.fileId;
  downloadFileRequest->downloadFileAction = DownloadFileActionNone;
  SendProtocolRequest(p_AccountId, downloadFileRequest);

  if (fileInfo.fileStatus != FileStatusDownloaded)
  {
    fileInfo.fileStatus = FileStatusDownloading;
    chatMessage.fileInfo = ProtocolUtil::FileInfoToHex(fileInfo);
    Emit("message.updated", MessageToJson(p_AccountId, p_ChatId, chatMessage));
  }
}

void ChatModel::SendTyping(const std::string& p_AccountId, const std::string& p_ChatId, bool p_IsTyping)
{
  GetAccountOrThrow(p_AccountId);
  std::shared_ptr<SendTypingRequest> sendTypingRequest = std::make_shared<SendTypingRequest>();
  sendTypingRequest->chatId = p_ChatId;
  sendTypingRequest->isTyping = p_IsTyping;
  SendProtocolRequest(p_AccountId, sendTypingRequest);
}

void ChatModel::GetMembers(const std::string& p_AccountId, const std::string& p_ChatId, const Reply& p_Reply)
{
  GetAccountOrThrow(p_AccountId);

  PendingReply pending;
  pending.accountId = p_AccountId;
  pending.chatId = p_ChatId;
  pending.deadline = TimeUtil::GetCurrentTimeMSec() + s_PendingTimeoutMs;
  pending.reply = p_Reply;
  m_Pending.insert(std::make_pair(PendingMembers, pending));

  std::shared_ptr<GetGroupMembersRequest> getGroupMembersRequest = std::make_shared<GetGroupMembersRequest>();
  getGroupMembersRequest->chatId = p_ChatId;
  SendProtocolRequest(p_AccountId, getGroupMembersRequest);
}

void ChatModel::CreateChat(const std::string& p_AccountId, const std::string& p_UserId)
{
  GetAccountOrThrow(p_AccountId);
  std::shared_ptr<CreateChatRequest> createChatRequest = std::make_shared<CreateChatRequest>();
  createChatRequest->userId = p_UserId;
  SendProtocolRequest(p_AccountId, createChatRequest);
}

void ChatModel::PinChat(const std::string& p_AccountId, const std::string& p_ChatId, bool p_IsPinned)
{
  GetAccountOrThrow(p_AccountId);
  if (!HasFeature(p_AccountId, FeaturePinChat))
  {
    throw ApiException(ApiErrorUnsupported, "pinning chats is not supported");
  }

  std::shared_ptr<PinChatRequest> pinChatRequest = std::make_shared<PinChatRequest>();
  pinChatRequest->chatId = p_ChatId;
  pinChatRequest->isPinned = p_IsPinned;
  SendProtocolRequest(p_AccountId, pinChatRequest);
}

void ChatModel::ArchiveChat(const std::string& p_AccountId, const std::string& p_ChatId, bool p_IsArchived)
{
  GetAccountOrThrow(p_AccountId);
  if (!HasFeature(p_AccountId, FeatureArchiveChat))
  {
    throw ApiException(ApiErrorUnsupported, "archiving chats is not supported");
  }

  std::shared_ptr<ArchiveChatRequest> archiveChatRequest = std::make_shared<ArchiveChatRequest>();
  archiveChatRequest->chatId = p_ChatId;
  archiveChatRequest->isArchived = p_IsArchived;
  SendProtocolRequest(p_AccountId, archiveChatRequest);
}

void ChatModel::DeleteChat(const std::string& p_AccountId, const std::string& p_ChatId)
{
  GetAccountOrThrow(p_AccountId);
  std::shared_ptr<DeleteChatRequest> deleteChatRequest = std::make_shared<DeleteChatRequest>();
  deleteChatRequest->chatId = p_ChatId;
  SendProtocolRequest(p_AccountId, deleteChatRequest);
}

void ChatModel::SetCurrentChat(const std::string& p_AccountId, const std::string& p_ChatId)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  m_CurrentChat = std::make_pair(p_AccountId, p_ChatId);

  std::shared_ptr<SetCurrentChatRequest> setCurrentChatRequest = std::make_shared<SetCurrentChatRequest>();
  setCurrentChatRequest->chatId = p_ChatId;
  SendProtocolRequest(p_AccountId, setCurrentChatRequest);

  // presence of the chat (user) is requested once per chat
  if (account.statusRequested.insert(p_ChatId).second)
  {
    std::shared_ptr<GetStatusRequest> getStatusRequest = std::make_shared<GetStatusRequest>();
    getStatusRequest->userId = p_ChatId;
    SendProtocolRequest(p_AccountId, getStatusRequest);
  }
}

void ChatModel::FindMessage(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_Text,
                            const std::string& p_FromMsgId, const Reply& p_Reply)
{
  GetAccountOrThrow(p_AccountId);
  if (p_Text.empty())
  {
    throw ApiException(ApiErrorInvalidParams, "text required");
  }

  PendingReply pending;
  pending.accountId = p_AccountId;
  pending.chatId = p_ChatId;
  pending.deadline = TimeUtil::GetCurrentTimeMSec() + s_PendingTimeoutMs;
  pending.reply = p_Reply;
  m_Pending.insert(std::make_pair(PendingFind, pending));

  std::shared_ptr<FindMessageRequest> findMessageRequest = std::make_shared<FindMessageRequest>();
  findMessageRequest->chatId = p_ChatId;
  findMessageRequest->fromMsgId = p_FromMsgId;
  findMessageRequest->findText = p_Text;
  SendProtocolRequest(p_AccountId, findMessageRequest);
}

// Events and serialization

void ChatModel::MarkChatDirty(const std::string& p_AccountId, const std::string& p_ChatId)
{
  m_DirtyChats.insert(std::make_pair(p_AccountId, p_ChatId));
}

void ChatModel::FlushDirtyChats()
{
  // one chat.updated per chat and notification batch
  std::set<std::pair<std::string, std::string>> dirtyChats;
  dirtyChats.swap(m_DirtyChats);
  for (const auto& dirtyChat : dirtyChats)
  {
    auto ait = m_Accounts.find(dirtyChat.first);
    if (ait == m_Accounts.end()) continue;

    auto cit = ait->second.chatInfos.find(dirtyChat.second);
    if (cit == ait->second.chatInfos.end()) continue;

    // skip if nothing visible changed since the last event for this chat
    const json chat = ChatToJson(dirtyChat.first, cit->second);
    std::string chatStr = chat.dump(-1, ' ', false, json::error_handler_t::replace);
    std::string& emitted = m_EmittedChats[dirtyChat];
    if (emitted == chatStr) continue;

    emitted.swap(chatStr);
    Emit("chat.updated", chat);
  }
}

void ChatModel::Emit(const std::string& p_Method, const json& p_Params)
{
  if (m_EventSink)
  {
    m_EventSink(p_Method, p_Params);
  }
}

std::string ChatModel::GetContactName(const std::string& p_AccountId, const std::string& p_ContactId)
{
  const AccountState& account = m_Accounts[p_AccountId];
  auto it = account.contactInfos.find(p_ContactId);
  if (it == account.contactInfos.end()) return std::string();

  if (it->second.isSelf) return "You";

  return it->second.name;
}

json ChatModel::ChatToJson(const std::string& p_AccountId, const ChatInfo& p_ChatInfo)
{
  std::shared_ptr<Protocol> protocol = m_ProtocolGetter(p_AccountId);
  const bool isGroup = protocol && protocol->IsGroupChat(p_ChatInfo.id);
  AccountState& account = m_Accounts[p_AccountId];

  json typing = json::array();
  auto tit = account.usersTyping.find(p_ChatInfo.id);
  if (tit != account.usersTyping.end())
  {
    for (const auto& userId : tit->second)
    {
      typing.push_back(userId);
    }
  }

  const ChatMessage* lastMessage = GetLastMessage(p_AccountId, p_ChatInfo.id);
  int64_t lastActivity = (lastMessage != nullptr) ? lastMessage->timeSent : p_ChatInfo.lastMessageTime;
  if (lastActivity == s_SponsoredTime)
  {
    lastActivity = -1;
  }

  return json{
    { "accountId", p_AccountId },
    { "id", p_ChatInfo.id },
    { "name", GetContactName(p_AccountId, p_ChatInfo.id) },
    { "isGroup", isGroup },
    { "isUnread", p_ChatInfo.isUnread },
    { "isUnreadMention", p_ChatInfo.isUnreadMention },
    { "isMuted", p_ChatInfo.isMuted },
    { "isPinned", p_ChatInfo.isPinned },
    { "isArchived", p_ChatInfo.isArchived },
    { "lastActivity", lastActivity },
    { "lastMessage", (lastMessage != nullptr) ? MessageToJson(p_AccountId, p_ChatInfo.id, *lastMessage) : json() },
    { "typing", typing },
  };
}

json ChatModel::MessageToJson(const std::string& p_AccountId, const std::string& p_ChatId,
                              const ChatMessage& p_ChatMessage)
{
  json quoted;
  if (!p_ChatMessage.quotedId.empty())
  {
    quoted = json{
      { "id", p_ChatMessage.quotedId },
      { "text", p_ChatMessage.quotedText },
      { "senderId", p_ChatMessage.quotedSender },
      { "senderName", GetContactName(p_AccountId, p_ChatMessage.quotedSender) },
    };
  }

  json file;
  if (!p_ChatMessage.fileInfo.empty())
  {
    FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(p_ChatMessage.fileInfo);
    file = json{
      { "id", fileInfo.fileId },
      { "path", fileInfo.filePath },
      { "name", FileUtil::BaseName(fileInfo.filePath) },
      { "mimeType", fileInfo.fileType },
      { "status", FileStatusToString(fileInfo.fileStatus) },
    };
  }

  json counts = json::object();
  for (const auto& emojiCount : p_ChatMessage.reactions.emojiCounts)
  {
    counts[emojiCount.first] = emojiCount.second;
  }

  json senders = json::object();
  std::string ownReaction;
  for (const auto& senderEmoji : p_ChatMessage.reactions.senderEmojis)
  {
    if (senderEmoji.first == s_ReactionsSelfId)
    {
      ownReaction = senderEmoji.second;
    }
    else
    {
      senders[senderEmoji.first] = senderEmoji.second;
    }
  }

  json mentions = json::object();
  for (const auto& mention : p_ChatMessage.mentions)
  {
    mentions[mention.first] = mention.second;
  }

  return json{
    { "accountId", p_AccountId },
    { "chatId", p_ChatId },
    { "id", p_ChatMessage.id },
    { "senderId", p_ChatMessage.senderId },
    { "senderName", p_ChatMessage.isOutgoing ? std::string("You")
                                             : GetContactName(p_AccountId, p_ChatMessage.senderId) },
    { "text", p_ChatMessage.text },
    { "time", p_ChatMessage.timeSent },
    { "isOutgoing", p_ChatMessage.isOutgoing },
    { "isRead", p_ChatMessage.isRead },
    { "isEdited", p_ChatMessage.isEdited },
    { "isDeleted", p_ChatMessage.isDeleted },
    { "isPinned", p_ChatMessage.isPinned },
    { "hasMention", p_ChatMessage.hasMention },
    { "quoted", quoted },
    { "file", file },
    { "reactions", json{ { "counts", counts }, { "senders", senders }, { "own", ownReaction } } },
    { "mentions", mentions },
    { "link", p_ChatMessage.link },
  };
}

json ChatModel::ContactToJson(const ContactInfo& p_ContactInfo)
{
  return json{
    { "id", p_ContactInfo.id },
    { "name", p_ContactInfo.name },
    { "phone", p_ContactInfo.phone },
    { "isSelf", p_ContactInfo.isSelf },
  };
}

json ChatModel::CapabilitiesToJson(std::shared_ptr<Protocol> p_Protocol, const std::string& p_ProtocolName)
{
  int64_t editWindowSec = 0;
  if (p_Protocol->HasFeature(FeatureEditMessagesAnytime))
  {
    editWindowSec = -1;
  }
  else if (p_Protocol->HasFeature(FeatureEditMessagesWithinTwoDays))
  {
    editWindowSec = 48 * 3600;
  }
  else if (p_Protocol->HasFeature(FeatureEditMessagesWithinFifteenMins))
  {
    editWindowSec = 15 * 60;
  }

  return json{
    { "editWindowSec", editWindowSec }, // -1 = unlimited, 0 = not supported
    { "pinChat", p_Protocol->HasFeature(FeaturePinChat) },
    { "archiveChat", p_Protocol->HasFeature(FeatureArchiveChat) },
    { "pinMessage", p_Protocol->HasFeature(FeaturePinMessage) },
    { "limitedReactions", p_Protocol->HasFeature(FeatureLimitedReactions) },
    { "readReceiptsPerMessage", HasPerMessageReadReceipts(p_ProtocolName) },
  };
}
