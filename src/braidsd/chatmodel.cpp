// chatmodel.cpp
//
// Copyright (c) 2026 braids contributors
// Copyright (c) 2019-2026 Kristofer Berggren (chat model logic, from nchat UiModel)
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

#include "chatmodel.h"

#include "cacheutil.h"

#include <algorithm>
#include <limits>
#include <map>
#include <tuple>

#include <sys/stat.h>

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
         (p_Lhs.isPinned == p_Rhs.isPinned) && (p_Lhs.hasMention == p_Rhs.hasMention) &&
         (p_Lhs.poll == p_Rhs.poll) && (p_Lhs.pollVotes == p_Rhs.pollVotes);
}

static std::string PollQuestion(const std::string& p_Poll)
{
  const json poll = json::parse(p_Poll, nullptr, false);
  return poll.is_object() ? poll.value("question", "") : "";
}

// Mime type of a file the protocol did not type (e.g. WhatsApp sets only the
// extension, Telegram download updates carry no type).
static std::string GuessMimeType(const FileInfo& p_FileInfo)
{
  static const std::map<std::string, std::string> s_Types = {
    { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "jfif", "image/jpeg" }, { "jpe", "image/jpeg" }, { "png", "image/png" }, { "gif", "image/gif" },
    { "webp", "image/webp" }, { "bmp", "image/bmp" }, { "heic", "image/heic" }, { "svg", "image/svg+xml" },
    { "mp4", "video/mp4" }, { "f4v", "video/mp4" }, { "m4v", "video/mp4" }, { "mov", "video/quicktime" }, { "webm", "video/webm" }, { "mkv", "video/x-matroska" },
    { "3gp", "video/3gpp" }, { "ogg", "audio/ogg" }, { "oga", "audio/ogg" }, { "opus", "audio/opus" },
    { "mp3", "audio/mpeg" }, { "m4a", "audio/mp4" }, { "wav", "audio/wav" }, { "pdf", "application/pdf" },
    { "tgs", "application/x-tgsticker" },
  };

  const std::string& path = p_FileInfo.filePath;
  const size_t dot = path.find_last_of('.');
  const size_t slash = path.find_last_of('/');
  if ((dot != std::string::npos) && ((slash == std::string::npos) || (dot > slash)))
  {
    auto it = s_Types.find(StrUtil::ToLower(path.substr(dot + 1)));
    if (it != s_Types.end()) return it->second;
  }

  if ((p_FileInfo.fileStatus == FileStatusDownloaded) && FileUtil::Exists(path) && !FileUtil::IsDir(path))
  {
    return FileUtil::GetMimeType(path);
  }

  return "";
}

// Mime type and kind ("sticker", "gif" or "") of a file. Protocols mark
// stickers and GIFs (animations without sound) with a "braids=" parameter on
// the type (Telegram) or in the file name ("x.sticker.webp", "x.gif.mp4");
// WebP images and Telegram's .tgs files are stickers in all messengers.
static void FileTypeAndKind(const FileInfo& p_FileInfo, std::string& p_MimeType, std::string& p_Kind)
{
  p_Kind.clear();
  std::string type = p_FileInfo.fileType;
  const size_t semi = type.find(';');
  if (semi != std::string::npos)
  {
    const std::string params = type.substr(semi + 1);
    if (params.find("braids=sticker") != std::string::npos) p_Kind = "sticker";
    else if (params.find("braids=gif") != std::string::npos) p_Kind = "gif";
    type = type.substr(0, semi);
    while (!type.empty() && (type.back() == ' ')) type.pop_back();
  }

  p_MimeType = type.empty() ? GuessMimeType(p_FileInfo) : type;
  if (!p_Kind.empty()) return;

  const std::string name = StrUtil::ToLower(FileUtil::BaseName(p_FileInfo.filePath));
  if ((name.size() > 4) && (name.compare(name.size() - 4, 4, ".tgs") == 0))
  {
    p_MimeType = "application/x-tgsticker"; // gzip to libmagic
  }

  if ((p_MimeType == "application/x-tgsticker") || (p_MimeType == "image/webp") ||
      (name.find(".sticker.") != std::string::npos))
  {
    p_Kind = "sticker";
  }
  else if ((p_MimeType == "image/gif") || (name.find(".gif.") != std::string::npos))
  {
    p_Kind = "gif";
  }
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
// Group events (joined, left, renamed, ...) arrive from the protocols as
// messages with bracketed texts like "[Joined]" or "[Added Pam, Jim]", one per
// line. Returns them as phrases to follow the sender's name ("joined", "added
// Pam, Jim"; "; " between several), or "" for a normal message.
static std::string SystemText(const ChatMessage& p_ChatMessage)
{
  if (!p_ChatMessage.fileInfo.empty() || p_ChatMessage.text.empty()) return "";

  static const std::vector<std::pair<std::string, std::string>> s_Exact = {
    { "Joined", "joined" },
    { "Left", "left" },
    { "Created", "created the group" },
    { "Joined Telegram", "joined Telegram" },
    { "PinMessage", "pinned a message" },
    { "ChatSetTheme", "changed the chat theme" },
    { "ChatSetMessageAutoDeleteTime", "changed the auto-delete timer" },
    { "Changed group name", "changed the group name" },
    { "Changed group description", "changed the group description" },
    { "Changed group avatar", "changed the group picture" },
    { "Changed disappearing messages timer", "changed the disappearing messages timer" },
    { "Invited a member", "invited a member" },
    { "Invitation revoked", "revoked an invitation" },
    { "Member requested to join", "requested to join" },
    { "Join request denied", "denied a join request" },
    { "Enabled announcements only", "allowed only admins to send messages" },
    { "Disabled announcements only", "allowed all members to send messages" },
    { "GroupUpdate", "updated the group" },
  };
  static const std::vector<std::pair<std::string, std::string>> s_Prefix = {
    { "Added ", "added " },
    { "Removed ", "removed " },
    { "Invited ", "invited " },
    { "Changed group name to ", "changed the group name to " },
    { "Changed role for ", "changed the role of " },
    { "Approved ", "approved " }, // "Approved <name> to join"
  };

  std::vector<std::string> phrases;
  for (const std::string& rawLine : StrUtil::Split(p_ChatMessage.text, '\n'))
  {
    std::string line = rawLine;
    StrUtil::Trim(line);
    if (line.empty()) continue;
    if ((line.size() < 3) || (line.front() != '[') || (line.back() != ']')) return "";

    const std::string inner = line.substr(1, line.size() - 2);
    if (inner.find_first_of("[]") != std::string::npos) return "";

    std::string phrase;
    for (const auto& exact : s_Exact)
    {
      if (inner == exact.first)
      {
        phrase = exact.second;
        break;
      }
    }

    for (size_t i = 0; phrase.empty() && (i < s_Prefix.size()); ++i)
    {
      if (StrUtil::StartsWith(inner, s_Prefix[i].first) && (inner.size() > s_Prefix[i].first.size()))
      {
        phrase = s_Prefix[i].second + inner.substr(s_Prefix[i].first.size());
      }
    }

    static const std::string s_Accepted = " accepted invite";
    if (phrase.empty() && (inner.size() > s_Accepted.size()) &&
        (inner.compare(inner.size() - s_Accepted.size(), s_Accepted.size(), s_Accepted) == 0))
    {
      phrase = "accepted the invitation";
    }

    if (phrase.empty()) return "";

    phrases.push_back(phrase);
  }

  return StrUtil::Join(phrases, "; ");
}

static bool HasPerMessageReadReceipts(const std::string& p_ProtocolName)
{
  return (p_ProtocolName == "WhatsAppMd") || (p_ProtocolName == "Signal");
}

ChatModel::ChatModel(const EventSink& p_EventSink, const ProtocolGetter& p_ProtocolGetter)
  : m_EventSink(p_EventSink)
  , m_ProtocolGetter(p_ProtocolGetter)
{
}

void ChatModel::SetProfilesDir(const std::string& p_ProfilesDir)
{
  m_ProfilesDir = p_ProfilesDir;
}

void ChatModel::SetBackgroundRunner(const BackgroundRunner& p_Runner)
{
  m_BackgroundRunner = p_Runner;
}

void ChatModel::AddAccount(const std::string& p_AccountId)
{
  const bool isNew = !m_Accounts.count(p_AccountId);
  m_Accounts[p_AccountId];
  if (isNew)
  {
    LoadAccountState(p_AccountId);
    LoadOutbox(p_AccountId);
    LoadAvatars(p_AccountId);
  }
}

void ChatModel::LoadAvatars(const std::string& p_AccountId)
{
  if (m_ProfilesDir.empty()) return;

  const std::string path = m_ProfilesDir + "/" + p_AccountId + "/braids-avatars.json";
  if (!FileUtil::Exists(path)) return;

  json saved = json::parse(FileUtil::ReadFile(path), nullptr, false);
  if (!saved.is_object()) return;

  AccountState& account = m_Accounts[p_AccountId];
  for (auto& item : saved.items())
  {
    if (!item.value().is_string()) continue;

    // a picture file removed since (cache cleared) is fetched again
    const std::string file = item.value().get<std::string>();
    if (!file.empty() && !FileUtil::Exists(file)) continue;

    account.savedAvatars[item.key()] = file;
  }
}

void ChatModel::SaveAvatars(const std::string& p_AccountId)
{
  AccountState& account = m_Accounts[p_AccountId];
  account.avatarsDirty = false;
  if (m_ProfilesDir.empty()) return;

  const std::string dir = m_ProfilesDir + "/" + p_AccountId;
  if (!FileUtil::IsDir(dir)) return;

  json saved = json::object();
  for (const auto& item : account.savedAvatars)
  {
    saved[item.first] = item.second;
  }

  for (const auto& item : account.avatars)
  {
    saved[item.first] = item.second;
  }

  const std::string path = dir + "/braids-avatars.json";
  FileUtil::WriteFile(path + ".tmp", saved.dump());
  FileUtil::Move(path + ".tmp", path);
}

void ChatModel::LoadAccountState(const std::string& p_AccountId)
{
  if (m_ProfilesDir.empty()) return;

  const std::string path = m_ProfilesDir + "/" + p_AccountId + "/braids-chats.json";
  if (!FileUtil::Exists(path)) return;

  json state = json::parse(FileUtil::ReadFile(path), nullptr, false);
  if (!state.is_object() || !state.contains("muted") || !state["muted"].is_object()) return;

  AccountState& account = m_Accounts[p_AccountId];
  if (state.contains("unread") && state["unread"].is_array())
  {
    for (const auto& chatId : state["unread"])
    {
      if (chatId.is_string())
      {
        account.markedUnread.insert(chatId.get<std::string>());
      }
    }
  }

  for (auto& item : state["muted"].items())
  {
    if (item.value().is_boolean())
    {
      account.muted[item.key()] = item.value().get<bool>();
    }
  }

  account.showJoined = state.value("showJoined", false);
  if (state.contains("hidden") && state["hidden"].is_array())
  {
    for (const auto& chatId : state["hidden"])
    {
      if (chatId.is_string())
      {
        account.hiddenChats.insert(chatId.get<std::string>());
      }
    }
  }
}

void ChatModel::SaveAccountState(const std::string& p_AccountId)
{
  if (m_ProfilesDir.empty()) return;

  const std::string dir = m_ProfilesDir + "/" + p_AccountId;
  if (!FileUtil::IsDir(dir)) return;

  json muted = json::object();
  for (const auto& item : m_Accounts[p_AccountId].muted)
  {
    muted[item.first] = item.second;
  }

  // written to a temp file and moved, so a crash never leaves half a file
  const std::string path = dir + "/braids-chats.json";
  json unread = json::array();
  for (const std::string& chatId : m_Accounts[p_AccountId].markedUnread)
  {
    unread.push_back(chatId);
  }

  json hidden = json::array();
  for (const std::string& chatId : m_Accounts[p_AccountId].hiddenChats)
  {
    hidden.push_back(chatId);
  }

  const json state = json{ { "muted", muted }, { "unread", unread }, { "showJoined", m_Accounts[p_AccountId].showJoined },
                           { "hidden", hidden } };
  FileUtil::WriteFile(path + ".tmp", state.dump(2));
  FileUtil::Move(path + ".tmp", path);
}

void ChatModel::UnreadCount(AccountState& p_Account, const ChatInfo& p_ChatInfo, int& p_Messages, int& p_Events)
{
  p_Messages = 0;
  p_Events = 0;
  const bool markedUnread = p_Account.markedUnread.count(p_ChatInfo.id);
  if (!p_ChatInfo.isUnread && !markedUnread)
  {
    p_Account.cachedUnread.erase(p_ChatInfo.id); // read since
    return;
  }

  // received unread messages after the last own or read one; group events
  // (joined, left, ...) counted apart
  auto cit = p_Account.chats.find(p_ChatInfo.id);
  if (cit != p_Account.chats.end())
  {
    int64_t readTime = 0;
    for (const auto& message : cit->second.messages)
    {
      const ChatMessage& chatMessage = message.second;
      if (chatMessage.isOutgoing || chatMessage.isRead)
      {
        readTime = std::max(readTime, chatMessage.timeSent);
      }
    }

    for (const auto& message : cit->second.messages)
    {
      const ChatMessage& chatMessage = message.second;
      if (!chatMessage.isOutgoing && !chatMessage.isRead && !chatMessage.isDeleted &&
          (chatMessage.timeSent > readTime) && (chatMessage.timeSent != s_SponsoredTime))
      {
        ++(SystemText(chatMessage).empty() ? p_Messages : p_Events);
      }
    }
  }

  auto uit = p_Account.cachedUnread.find(p_ChatInfo.id);
  if (uit != p_Account.cachedUnread.end())
  {
    p_Messages = std::max(p_Messages, uit->second);
  }

  // unread state without known messages (e.g. marked unread): one message
  if (markedUnread || ((p_Messages == 0) && (p_Events == 0)))
  {
    p_Messages = std::max(p_Messages, 1);
  }
}

// length of a downloaded Ogg/Opus file (voice message) in seconds, 0 if
// unknown: the last page's granule position (48 kHz samples) minus the
// OpusHead pre-skip; remembered per path
static int OggOpusDurationSec(const std::string& p_Path)
{
  static std::unordered_map<std::string, int> s_Durations;
  auto it = s_Durations.find(p_Path);
  if (it != s_Durations.end()) return it->second;

  int duration = 0;
  const std::string data = FileUtil::ReadFile(p_Path);
  if ((data.size() >= 64) && (data.compare(0, 4, "OggS") == 0))
  {
    auto le = [&data](size_t p_Pos, int p_Bytes)
    {
      uint64_t value = 0;
      for (int i = p_Bytes - 1; i >= 0; --i)
      {
        value = (value << 8) | static_cast<uint8_t>(data[p_Pos + i]);
      }

      return value;
    };

    uint64_t preSkip = 0;
    const size_t head = data.find("OpusHead");
    if ((head != std::string::npos) && (head + 12 <= data.size()))
    {
      preSkip = le(head + 10, 2);
    }

    const size_t last = data.rfind("OggS");
    if ((last != std::string::npos) && (last + 14 <= data.size()))
    {
      const uint64_t granule = le(last + 6, 8);
      if (granule > preSkip)
      {
        duration = static_cast<int>((granule - preSkip + 24000) / 48000);
      }
    }
  }

  s_Durations[p_Path] = duration;
  return duration;
}

static bool HasLink(const std::string& p_Text)
{
  return (p_Text.find("://") != std::string::npos) || (p_Text.find("www.") != std::string::npos);
}

// Telegram's "<contact> joined Telegram" message
static bool IsJoinedMessage(const ChatMessage& p_ChatMessage)
{
  return p_ChatMessage.fileInfo.empty() && (p_ChatMessage.text == "[Joined Telegram]");
}

json ChatModel::GetMedia(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_Kind,
                         int64_t p_BeforeTime, int p_Limit, const std::string& p_Text)
{
  if ((p_Kind != "media") && (p_Kind != "files") && (p_Kind != "links") && (p_Kind != "pinned") &&
      (p_Kind != "search"))
  {
    throw ApiException(ApiErrorInvalidParams, "kind must be media, files, links or pinned");
  }

  const std::string needle = StrUtil::ToLower(p_Text);
  if ((p_Kind == "search") && needle.empty())
  {
    throw ApiException(ApiErrorInvalidParams, "missing param 'text'");
  }

  if (!p_ChatId.empty() && p_AccountId.empty())
  {
    throw ApiException(ApiErrorInvalidParams, "chatId needs accountId");
  }

  const bool links = (p_Kind == "links");
  const bool byText = links || (p_Kind == "pinned") || (p_Kind == "search"); // no file filter after the query
  const int limit = std::max(1, std::min(p_Limit, 200));
  const int64_t before = (p_BeforeTime > 0) ? p_BeforeTime : std::numeric_limits<int64_t>::max();
  // candidates per account from the cache; media and files share the query
  const int fetch = byText ? limit : (limit * 3);

  std::vector<std::string> accountIds;
  if (!p_AccountId.empty())
  {
    GetAccountOrThrow(p_AccountId);
    accountIds.push_back(p_AccountId);
  }
  else
  {
    for (const auto& account : m_Accounts)
    {
      accountIds.push_back(account.first);
    }
  }

  struct Item
  {
    std::string accountId;
    std::string chatId;
    const ChatMessage* message;
  };

  // the oldest time all accounts were searched down to: older candidates
  // are left to the next page so that pages do not skip any
  int64_t cutoff = 0;
  std::map<std::tuple<std::string, std::string, std::string>, Item> candidates;
  for (const std::string& accountId : accountIds)
  {
    AccountState& account = m_Accounts[accountId];
    std::vector<std::pair<std::string, ChatMessage>> cached =
      MessageCache::FetchMediaSync(accountId, p_ChatId, (p_Kind == "media") ? "files" : p_Kind, p_Text, before,
                                   fetch);
    if ((int)cached.size() == fetch)
    {
      cutoff = std::max(cutoff, cached.back().second.timeSent);
    }

    for (auto& chatIdMessage : cached)
    {
      ChatState& chatState = account.chats[chatIdMessage.first];
      const ChatMessage* chatMessage = nullptr;
      auto mit = chatState.messages.find(chatIdMessage.second.id);
      if (mit != chatState.messages.end())
      {
        chatMessage = &mit->second;
      }
      else
      {
        auto dit = chatState.detached.find(chatIdMessage.second.id);
        if (dit == chatState.detached.end())
        {
          dit = chatState.detached.emplace(chatIdMessage.second.id, chatIdMessage.second).first;
        }

        chatMessage = &dit->second;
      }

      candidates[std::make_tuple(accountId, chatIdMessage.first, chatMessage->id)] =
        Item{ accountId, chatIdMessage.first, chatMessage };
    }

    // loaded messages (newer than the cache, or protocols without it)
    for (auto& chat : account.chats)
    {
      if (!p_ChatId.empty() && (chat.first != p_ChatId)) continue;

      for (const auto& message : chat.second.messages)
      {
        const ChatMessage& chatMessage = message.second;
        if ((chatMessage.timeSent >= before) || (chatMessage.timeSent == s_SponsoredTime)) continue;

        candidates[std::make_tuple(accountId, chat.first, chatMessage.id)] = Item{ accountId, chat.first, &chatMessage };
      }
    }
  }

  std::vector<Item> items;
  for (const auto& candidate : candidates)
  {
    const Item& item = candidate.second;
    const ChatMessage& chatMessage = *item.message;
    if (chatMessage.isDeleted || (chatMessage.timeSent < cutoff)) continue;

    if (links)
    {
      if (!HasLink(chatMessage.text)) continue;
    }
    else if (p_Kind == "pinned")
    {
      if (!chatMessage.isPinned) continue;
    }
    else if (p_Kind == "search")
    {
      if (StrUtil::ToLower(chatMessage.text).find(needle) == std::string::npos) continue;
      if (!SystemText(chatMessage).empty()) continue; // [Joined] etc.
    }
    else
    {
      if (chatMessage.fileInfo.empty()) continue;

      FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(chatMessage.fileInfo);
      std::string mimeType;
      std::string kind;
      FileTypeAndKind(fileInfo, mimeType, kind);
      if (kind == "sticker") continue; // not shared media
      const bool isMedia = StrUtil::StartsWith(mimeType, "image/") || StrUtil::StartsWith(mimeType, "video/");
      if (isMedia != (p_Kind == "media")) continue;
    }

    items.push_back(item);
  }

  std::sort(items.begin(), items.end(), [](const Item& p_Lhs, const Item& p_Rhs)
  {
    return p_Lhs.message->timeSent > p_Rhs.message->timeSent;
  });

  // next page: older than the last item returned, or than the cutoff; 0: end
  int64_t nextBefore = 0;
  if ((int)items.size() > limit)
  {
    items.resize(limit);
    nextBefore = items.back().message->timeSent;
  }
  else if (cutoff > 0)
  {
    nextBefore = cutoff;
  }

  json list = json::array();
  for (const Item& item : items)
  {
    ChatMessage chatMessage = *item.message;
    chatMessage.quotedId.clear(); // no quotes to fetch for these
    list.push_back(MessageToJson(item.accountId, item.chatId, chatMessage));
  }

  return json{ { "items", list }, { "nextBefore", nextBefore } };
}

bool ChatModel::IsMuted(const AccountState& p_Account, const ChatInfo& p_ChatInfo) const
{
  auto it = p_Account.muted.find(p_ChatInfo.id);
  return (it != p_Account.muted.end()) ? it->second : p_ChatInfo.isMuted;
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

        if (account.connectTime == 0)
        {
          // the names known from the cache at once: the messenger's contact
          // sync can take a while after a start
          MessageCache::FetchContacts(accountId);
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
        if (connectNotify->success)
        {
          // counted in the background: a large history must not hold up the API
          auto counts = std::make_shared<std::map<std::string, int>>();
          auto work = [accountId, counts]()
          {
            *counts = MessageCache::GetUnreadCountsSync(accountId);
          };
          auto done = [this, accountId, counts]()
          {
            auto ait = m_Accounts.find(accountId);
            if (ait == m_Accounts.end()) return; // removed meanwhile

            ait->second.cachedUnread = *counts;
            for (const auto& unread : *counts)
            {
              if (ait->second.chatInfos.count(unread.first))
              {
                MarkChatDirty(accountId, unread.first);
              }
            }

            FlushDirtyChats();
          };

          if (m_BackgroundRunner)
          {
            m_BackgroundRunner(work, done);
          }
          else
          {
            work();
            done();
          }
        }

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
        const std::string& localId = sendMessageNotify->chatMessage.id;
        if (StrUtil::StartsWith(localId, "braids-out-"))
        {
          OnSendResult(accountId, localId, sendMessageNotify->success);
        }
        else if (!sendMessageNotify->success)
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
        ChatState& chatState = account.chats[chatId];
        auto mit = chatState.messages.find(newMessageFileNotify->msgId);
        if (mit == chatState.messages.end())
        {
          mit = chatState.detached.find(newMessageFileNotify->msgId);
          if (mit == chatState.detached.end()) break;
        }

        {
          // download updates may not repeat the type
          FileInfo newInfo = ProtocolUtil::FileInfoFromHex(newMessageFileNotify->fileInfo);
          if (newInfo.fileType.empty() && !mit->second.fileInfo.empty())
          {
            newInfo.fileType = ProtocolUtil::FileInfoFromHex(mit->second.fileInfo).fileType;
          }

          mit->second.fileInfo = ProtocolUtil::FileInfoToHex(newInfo);
          Emit("message.updated", MessageToJson(accountId, chatId, mit->second));
        }
      }
      break;

    case NewPollNotifyType:
      OnPoll(accountId, std::static_pointer_cast<NewPollNotify>(p_ServiceMessage));
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

    case JoinChatNotifyType:
      {
        std::shared_ptr<JoinChatNotify> joinChatNotify = std::static_pointer_cast<JoinChatNotify>(p_ServiceMessage);
        if (joinChatNotify->success && !account.chatInfos.count(joinChatNotify->chatId))
        {
          // make sure the chat shows up even if the protocol does not report it by itself
          std::shared_ptr<GetChatsRequest> getChatsRequest = std::make_shared<GetChatsRequest>();
          getChatsRequest->chatIds.insert(joinChatNotify->chatId);
          SendProtocolRequest(accountId, getChatsRequest);
        }

        Emit("chat.joined", json{ { "accountId", accountId }, { "link", joinChatNotify->link },
               { "success", joinChatNotify->success }, { "chatId", joinChatNotify->chatId },
               { "error", joinChatNotify->error } });
      }
      break;

    case CreateGroupNotifyType:
      {
        std::shared_ptr<CreateGroupNotify> createGroupNotify =
          std::static_pointer_cast<CreateGroupNotify>(p_ServiceMessage);
        if (createGroupNotify->success && !account.chatInfos.count(createGroupNotify->chatId))
        {
          // make sure the chat shows up even if the protocol does not report it by itself
          std::shared_ptr<GetChatsRequest> getChatsRequest = std::make_shared<GetChatsRequest>();
          getChatsRequest->chatIds.insert(createGroupNotify->chatId);
          SendProtocolRequest(accountId, getChatsRequest);
        }

        Emit("chat.groupCreated", json{ { "accountId", accountId }, { "name", createGroupNotify->name },
               { "success", createGroupNotify->success }, { "chatId", createGroupNotify->chatId },
               { "error", createGroupNotify->error } });
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

        auto mit = account.muted.find(chatId);
        if (mit != account.muted.end())
        {
          const bool isMuted = mit->second;
          account.muted.erase(mit);
          account.muted[newChatId] = isMuted;
          SaveAccountState(accountId);
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

    case NewAvatarNotifyType:
      {
        std::shared_ptr<NewAvatarNotify> notify = std::static_pointer_cast<NewAvatarNotify>(p_ServiceMessage);
        if (notify->full)
        {
          // a protocol without a bigger picture answers with the small one
          std::string path = notify->filePath;
          if (path.empty())
          {
            auto ait = account.avatars.find(notify->id);
            if (ait != account.avatars.end()) path = ait->second;
          }

          account.fullAvatars[notify->id] = path;
          Emit("avatar.full", json{ { "accountId", accountId }, { "id", notify->id }, { "path", path } });
          break;
        }

        auto ait = account.avatars.find(notify->id);
        if ((ait != account.avatars.end()) && (ait->second == notify->filePath)) break;

        account.avatars[notify->id] = notify->filePath;
        account.fullAvatars.erase(notify->id); // the picture changed
        auto sit = account.savedAvatars.find(notify->id);
        if ((sit == account.savedAvatars.end()) || (sit->second != notify->filePath))
        {
          account.avatarsDirty = true; // saved with the next timer tick
        }
        Emit("avatar.updated", json{ { "accountId", accountId }, { "id", notify->id }, { "path", notify->filePath } });
      }
      break;

    case NewDescriptionNotifyType:
      {
        std::shared_ptr<NewDescriptionNotify> notify = std::static_pointer_cast<NewDescriptionNotify>(p_ServiceMessage);
        auto dit = account.descriptions.find(notify->id);
        if ((dit != account.descriptions.end()) && (dit->second == notify->description)) break;

        account.descriptions[notify->id] = notify->description;
        Emit("description.updated", json{ { "accountId", accountId }, { "id", notify->id },
               { "description", notify->description } });
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

  bool hiddenChanged = false;
  for (auto chatMessage : p_Notify->chatMessages)
  {
    auto mit = chat.messages.find(chatMessage.id);
    if (!account.showJoined && IsJoinedMessage(chatMessage))
    {
      // left out when it arrives; a new chat with nothing else stays out of
      // the list until someone writes (history pages keep it as a group event)
      if (!p_Notify->cached && !isPage)
      {
        const bool hasOther = std::any_of(chat.messages.begin(), chat.messages.end(),
                                          [](const std::pair<const std::string, ChatMessage>& p_Item)
        {
          return !IsJoinedMessage(p_Item.second);
        });
        if (!hasOther && account.hiddenChats.insert(chatId).second)
        {
          LOG_DEBUG("joined message, chat %s left out of the list", chatId.c_str());
          hiddenChanged = true;
          // listed already (the chat list came first)
          if (account.chatInfos.count(chatId))
          {
            m_EmittedChats.erase(std::make_pair(p_AccountId, chatId));
            Emit("chat.removed", json{ { "accountId", p_AccountId }, { "chatId", chatId } });
          }
        }

        continue;
      }

      // nor in the preview of a chat left out
      if (account.hiddenChats.count(chatId)) continue;
    }
    else if (account.hiddenChats.count(chatId) && !IsJoinedMessage(chatMessage))
    {
      // a conversation started: the chat is listed
      account.hiddenChats.erase(chatId);
      hiddenChanged = true;
      MarkChatDirty(p_AccountId, chatId);
    }

    if (!chatMessage.poll.empty() && chatMessage.pollVotes.empty())
    {
      // votes are counted here: kept (with the end) when the protocol sends
      // the poll again
      std::vector<ChatMessage> cached;
      const ChatMessage* known = (mit != chat.messages.end()) ? &mit->second : nullptr;
      if ((known == nullptr) && MessageCache::GetOneMessage(p_AccountId, chatId, chatMessage.id, cached))
      {
        known = &cached.front();
      }

      if (known != nullptr)
      {
        chatMessage.pollVotes = known->pollVotes;
        chatMessage.poll = CacheUtil::MergePoll(known->poll, chatMessage.poll);
      }
    }

    if (!chatMessage.poll.empty())
    {
      auto pit = account.pendingPollVotes.find(chatId + "\x01" + chatMessage.id);
      if (pit != account.pendingPollVotes.end())
      {
        for (const auto& vote : pit->second)
        {
          ApplyPollNotify(chatMessage, *vote);
        }

        account.pendingPollVotes.erase(pit);
        MessageCache::UpdateMessagePoll(p_AccountId, chatId, chatMessage.id, chatMessage.poll,
                                        chatMessage.pollVotes.empty() ? "{}" : chatMessage.pollVotes);
      }
    }

    if (mit == chat.messages.end())
    {
      chat.messages[chatMessage.id] = chatMessage;
      if (!hasPendingPage)
      {
        Emit("message.added", MessageToJson(p_AccountId, chatId, chatMessage));
      }

      if (chatMessage.isOutgoing && !p_Notify->cached && !account.outbox.empty())
      {
        MatchOutboxEcho(p_AccountId, chatId, chatMessage);
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

  if (hiddenChanged)
  {
    SaveAccountState(p_AccountId);
  }

  if (!account.chatInfos.count(chatId))
  {
    LOG_TRACE("new message in unknown chat, get chat %s", chatId.c_str());
    std::shared_ptr<GetChatsRequest> getChatsRequest = std::make_shared<GetChatsRequest>();
    getChatsRequest->chatIds.insert(chatId);
    SendProtocolRequest(p_AccountId, getChatsRequest);
  }

  UpdateChatFromMessages(p_AccountId, chatId);

  for (const auto& chatMessage : p_Notify->chatMessages)
  {
    UpdateQuotingMessages(p_AccountId, chatId, chatMessage.id);
  }

  if (hasPendingPage)
  {
    // the page ends at the oldest message delivered for this request: older
    // messages in the model may be isolated (previews, quotes) with a gap between
    int64_t minTime = std::numeric_limits<int64_t>::max();
    for (const auto& chatMessage : p_Notify->chatMessages)
    {
      minTime = std::min(minTime, chatMessage.timeSent);
    }

    ResolvePending(PendingMessages, p_AccountId, chatId, p_Notify->fromMsgId, false /*p_TimedOut*/, minTime);
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

  // group events (joined, left, ...) are not worth a notification
  if (!SystemText(p_ChatMessage).empty()) return;

  auto cit = account.chatInfos.find(p_ChatId);
  if (cit != account.chatInfos.end())
  {
    if (cit->second.isArchived) return;

    if (IsMuted(account, cit->second) && !p_ChatMessage.hasMention) return;
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
    { "text", p_ChatMessage.poll.empty() ? p_ChatMessage.text
                                         : "📊 " + PollQuestion(p_ChatMessage.poll) },
    { "hasFile", !p_ChatMessage.fileInfo.empty() },
  });
}

void ChatModel::ResolvePending(PendingType p_Type, const std::string& p_AccountId, const std::string& p_ChatId,
                               const std::string& p_Key, bool p_TimedOut, int64_t p_MinTime)
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
          // timed out: nothing is known to be contiguous, the client may retry
          json result = MessagesPage(p_AccountId, p_ChatId, p_Key, pending.limit, p_MinTime);
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
  for (auto& account : m_Accounts)
  {
    // at most every 2 s: many pictures arrive one by one after a start
    if (account.second.avatarsDirty && (p_NowMs - account.second.avatarsSavedAt >= 2000))
    {
      account.second.avatarsSavedAt = p_NowMs;
      SaveAvatars(account.first);
    }
  }

  // confirmed messages whose copy did not come back (or not recognisably):
  // the messenger has them, so no longer shown as pending
  static const int64_t s_SentEchoTimeoutMs = 10000;
  std::vector<std::pair<std::string, std::string>> sentDone;
  for (const auto& account : m_Accounts)
  {
    for (const OutboxEntry& entry : account.second.outbox)
    {
      if ((entry.state == "sent") && (p_NowMs - entry.sentAt > s_SentEchoTimeoutMs))
      {
        sentDone.push_back(std::make_pair(account.first, entry.id));
      }
    }
  }

  for (const auto& done : sentDone)
  {
    RemoveOutboxEntry(done.first, done.second);
  }

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
                             const std::string& p_FromMsgId, int p_Limit, int64_t p_MinTime)
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

    if (chatMessage.timeSent < p_MinTime) continue;

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

  // the newest page: with the messages still in the outbox
  if (p_FromMsgId.empty())
  {
    for (const OutboxEntry& entry : m_Accounts[p_AccountId].outbox)
    {
      if (entry.chatId == p_ChatId)
      {
        list.push_back(OutboxEntryToJson(p_AccountId, entry));
      }
    }
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

    mit = cit->second.detached.find(p_MsgId);
    if (mit != cit->second.detached.end()) return mit->second;
  }

  // an older message only in the cache (e.g. from media.list)
  std::vector<ChatMessage> cached;
  if (MessageCache::GetOneMessage(p_AccountId, p_ChatId, p_MsgId, cached))
  {
    ChatMessage& chatMessage = account.chats[p_ChatId].detached[p_MsgId];
    chatMessage = cached.front();
    return chatMessage;
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

      if (account.second.hiddenChats.count(chatInfo.first)) continue;

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

std::string ChatModel::SendMessage(const std::string& p_AccountId, const std::string& p_ChatId,
                                   const std::string& p_Text, const std::string& p_QuotedId,
                                   const std::string& p_FilePath, bool p_IsVoice, const json& p_Poll)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  std::string poll;
  if (!p_Poll.is_null())
  {
    if (!HasFeature(p_AccountId, FeaturePolls))
    {
      throw ApiException(ApiErrorUnsupported, "polls are not supported by this account");
    }

    auto trimmed = [](std::string p_Str)
    {
      StrUtil::Trim(p_Str);
      return p_Str;
    };
    const std::string question = trimmed(p_Poll.is_object() ? p_Poll.value("question", "") : "");
    json options = json::array();
    if (p_Poll.is_object() && p_Poll.contains("options") && p_Poll["options"].is_array())
    {
      for (const auto& option : p_Poll["options"])
      {
        const std::string text = trimmed(option.is_string() ? option.get<std::string>()
                                         : (option.is_object() ? option.value("text", "") : ""));
        if (!text.empty()) options.push_back(json{ { "text", text } });
      }
    }

    if (question.empty() || (options.size() < 2) || (options.size() > 12))
    {
      throw ApiException(ApiErrorInvalidParams, "a poll needs a question and 2 to 12 options");
    }

    poll = json{ { "question", question }, { "options", options },
                 { "multiple", p_Poll.value("multiple", false) } }.dump();
  }

  if (p_Text.empty() && p_FilePath.empty() && poll.empty())
  {
    throw ApiException(ApiErrorInvalidParams, "text or filePath required");
  }

  if (!poll.empty() && (!p_Text.empty() || !p_FilePath.empty()))
  {
    throw ApiException(ApiErrorInvalidParams, "a poll is sent without text or file");
  }

  if (!p_QuotedId.empty())
  {
    GetMessageOrThrow(p_AccountId, p_ChatId, p_QuotedId);
  }

  if (!p_FilePath.empty() && ((p_FilePath.at(0) != '/') || !FileUtil::Exists(p_FilePath)))
  {
    throw ApiException(ApiErrorInvalidParams, "filePath must be an existing absolute path");
  }

  if (p_IsVoice && p_FilePath.empty())
  {
    throw ApiException(ApiErrorInvalidParams, "isVoice needs a filePath");
  }

  // unique and ordered also within one millisecond
  static int64_t s_LastTime = 0;
  const int64_t now = std::max(TimeUtil::GetCurrentTimeMSec(), s_LastTime + 1);
  s_LastTime = now;

  OutboxEntry entry;
  entry.id = "braids-out-" + std::to_string(now);
  entry.chatId = p_ChatId;
  entry.text = p_Text;
  entry.quotedId = p_QuotedId;
  entry.filePath = p_FilePath;
  entry.isVoice = p_IsVoice;
  entry.poll = poll;
  entry.time = now;
  entry.state = "waiting";
  account.outbox.push_back(entry);
  Emit("message.added", OutboxEntryToJson(p_AccountId, entry));

  if (account.online)
  {
    DispatchOutbox(p_AccountId, account.outbox.back());
  }

  SaveOutbox(p_AccountId);
  return entry.id;
}

void ChatModel::ResendMessage(const std::string& p_AccountId, const std::string& p_ChatId,
                              const std::string& p_MsgId)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  for (OutboxEntry& entry : account.outbox)
  {
    if ((entry.id != p_MsgId) || (entry.chatId != p_ChatId)) continue;

    if (entry.state != "failed")
    {
      throw ApiException(ApiErrorInvalidParams, "message is not failed");
    }

    entry.state = "waiting";
    Emit("message.updated", OutboxEntryToJson(p_AccountId, entry));
    if (account.online)
    {
      DispatchOutbox(p_AccountId, entry);
    }

    SaveOutbox(p_AccountId);
    return;
  }

  throw ApiException(ApiErrorUnknownMessage, "unknown message");
}

void ChatModel::SetAccountOnline(const std::string& p_AccountId, bool p_Online)
{
  auto ait = m_Accounts.find(p_AccountId);
  if (ait == m_Accounts.end()) return;

  AccountState& account = ait->second;
  const bool wasOnline = account.online;
  account.online = p_Online;
  if (!p_Online || wasOnline) return;

  bool sent = false;
  for (OutboxEntry& entry : account.outbox)
  {
    if (entry.state == "waiting")
    {
      DispatchOutbox(p_AccountId, entry);
      sent = true;
    }
  }

  if (sent)
  {
    SaveOutbox(p_AccountId);
  }
}

void ChatModel::DispatchOutbox(const std::string& p_AccountId, OutboxEntry& p_Entry)
{
  if (!p_Entry.filePath.empty() && !FileUtil::Exists(p_Entry.filePath))
  {
    // a temporary file (recording, pasted image) gone since, e.g. after a restart
    LOG_WARNING("outbox %s: attachment file missing", p_Entry.id.c_str());
    p_Entry.state = "failed";
    Emit("message.updated", OutboxEntryToJson(p_AccountId, p_Entry));
    return;
  }

  std::shared_ptr<SendMessageRequest> sendMessageRequest = std::make_shared<SendMessageRequest>();
  sendMessageRequest->chatId = p_Entry.chatId;
  // the local id comes back in SendMessageNotify (protocols do not use it)
  sendMessageRequest->chatMessage.id = p_Entry.id;
  sendMessageRequest->chatMessage.text = p_Entry.text;
  sendMessageRequest->chatMessage.mentions = ParseMentions(p_AccountId, p_Entry.chatId, p_Entry.text);
  sendMessageRequest->chatMessage.poll = p_Entry.poll;

  if (!p_Entry.quotedId.empty())
  {
    const ChatMessage* quoted = FindMessage(p_AccountId, p_Entry.chatId, p_Entry.quotedId);
    sendMessageRequest->chatMessage.quotedId = p_Entry.quotedId;
    if (quoted != nullptr)
    {
      sendMessageRequest->chatMessage.quotedText = quoted->text;
      sendMessageRequest->chatMessage.quotedSender = quoted->senderId;
    }
  }

  if (!p_Entry.filePath.empty())
  {
    FileInfo fileInfo;
    fileInfo.filePath = p_Entry.filePath;
    fileInfo.fileType = FileUtil::GetMimeType(p_Entry.filePath);
    // a voice message is audio, also in a container sniffed as video (webm)
    if (p_Entry.isVoice && StrUtil::StartsWith(fileInfo.fileType, "video/"))
    {
      fileInfo.fileType = "audio/" + fileInfo.fileType.substr(6);
    }

    sendMessageRequest->chatMessage.fileInfo = ProtocolUtil::FileInfoToHex(fileInfo);
    sendMessageRequest->isVoice = p_Entry.isVoice;
  }

  const bool changed = (p_Entry.state != "sending");
  p_Entry.state = "sending";
  if (changed)
  {
    Emit("message.updated", OutboxEntryToJson(p_AccountId, p_Entry));
  }

  SendProtocolRequest(p_AccountId, sendMessageRequest);
}

void ChatModel::OnSendResult(const std::string& p_AccountId, const std::string& p_LocalId, bool p_Success)
{
  AccountState& account = m_Accounts[p_AccountId];
  for (OutboxEntry& entry : account.outbox)
  {
    if (entry.id != p_LocalId) continue;

    if (p_Success)
    {
      // shown until the messenger's own copy of the message arrives
      entry.state = "sent";
      entry.sentAt = TimeUtil::GetCurrentTimeMSec();
    }
    else
    {
      // offline by now: sent again when the account is back
      entry.state = account.online ? "failed" : "waiting";
      Emit("message.updated", OutboxEntryToJson(p_AccountId, entry));
    }

    SaveOutbox(p_AccountId);
    return;
  }
}

// An own message from the messenger replaces the outbox message it is: the
// one with the same text (or file name), else the oldest one on its way
// (the messenger may change the text, e.g. formatting).
void ChatModel::MatchOutboxEcho(const std::string& p_AccountId, const std::string& p_ChatId,
                                const ChatMessage& p_ChatMessage)
{
  AccountState& account = m_Accounts[p_AccountId];
  std::string fileName;
  if (!p_ChatMessage.fileInfo.empty())
  {
    fileName = FileUtil::BaseName(ProtocolUtil::FileInfoFromHex(p_ChatMessage.fileInfo).filePath);
  }

  const OutboxEntry* match = nullptr;
  const OutboxEntry* oldest = nullptr;
  for (const OutboxEntry& entry : account.outbox)
  {
    if ((entry.chatId != p_ChatId) || ((entry.state != "sending") && (entry.state != "sent"))) continue;

    const bool sameText = !entry.text.empty() && (entry.text == p_ChatMessage.text);
    const bool sameFile = !entry.filePath.empty() && !fileName.empty() && (FileUtil::BaseName(entry.filePath) == fileName);
    const bool samePoll = !entry.poll.empty() && !p_ChatMessage.poll.empty() &&
                          (PollQuestion(entry.poll) == PollQuestion(p_ChatMessage.poll));
    if (sameText || (entry.text.empty() && sameFile) || samePoll)
    {
      match = &entry;
      break;
    }

    if (oldest == nullptr)
    {
      oldest = &entry;
    }
  }

  if (match == nullptr)
  {
    match = oldest;
  }

  if (match != nullptr)
  {
    RemoveOutboxEntry(p_AccountId, match->id);
  }
}

void ChatModel::RemoveOutboxEntry(const std::string& p_AccountId, const std::string& p_LocalId)
{
  const std::string localId = p_LocalId; // may refer to the entry erased below
  AccountState& account = m_Accounts[p_AccountId];
  for (auto it = account.outbox.begin(); it != account.outbox.end(); ++it)
  {
    if (it->id != localId) continue;

    const std::string chatId = it->chatId;
    account.outbox.erase(it);
    Emit("message.removed", json{ { "accountId", p_AccountId }, { "chatId", chatId }, { "msgId", localId } });
    SaveOutbox(p_AccountId);
    return;
  }
}

json ChatModel::OutboxEntryToJson(const std::string& p_AccountId, const OutboxEntry& p_Entry)
{
  ChatMessage chatMessage;
  chatMessage.id = p_Entry.id;
  chatMessage.text = p_Entry.text;
  chatMessage.quotedId = p_Entry.quotedId;
  chatMessage.poll = p_Entry.poll;
  chatMessage.timeSent = p_Entry.time;
  chatMessage.isOutgoing = true;
  chatMessage.isRead = false;
  if (!p_Entry.filePath.empty())
  {
    FileInfo fileInfo;
    fileInfo.filePath = p_Entry.filePath;
    fileInfo.fileType = FileUtil::GetMimeType(p_Entry.filePath);
    fileInfo.fileStatus = FileStatusDownloaded;
    chatMessage.fileInfo = ProtocolUtil::FileInfoToHex(fileInfo);
  }

  json message = MessageToJson(p_AccountId, p_Entry.chatId, chatMessage);
  // "sending" also while confirmed but not yet back from the messenger
  message["pending"] = (p_Entry.state == "sent") ? "sending" : p_Entry.state;
  return message;
}

void ChatModel::LoadOutbox(const std::string& p_AccountId)
{
  if (m_ProfilesDir.empty()) return;

  const std::string path = m_ProfilesDir + "/" + p_AccountId + "/braids-outbox.json";
  if (!FileUtil::Exists(path)) return;

  json state = json::parse(FileUtil::ReadFile(path), nullptr, false);
  if (!state.is_object() || !state.contains("outbox") || !state["outbox"].is_array()) return;

  AccountState& account = m_Accounts[p_AccountId];
  for (const auto& item : state["outbox"])
  {
    if (!item.is_object() || !item.contains("id") || !item.contains("chatId")) continue;

    OutboxEntry entry;
    entry.id = item.value("id", "");
    entry.chatId = item.value("chatId", "");
    entry.text = item.value("text", "");
    entry.quotedId = item.value("quotedId", "");
    entry.filePath = item.value("filePath", "");
    entry.isVoice = item.value("isVoice", false);
    entry.poll = item.value("poll", "");
    entry.time = item.value("time", (int64_t)0);
    entry.state = item.value("state", "failed");
    // a message that was on its way when braids stopped may have arrived:
    // not sent again by itself
    if ((entry.state != "waiting") && (entry.state != "failed"))
    {
      entry.state = "failed";
    }

    if (!entry.id.empty() && !entry.chatId.empty())
    {
      account.outbox.push_back(entry);
    }
  }
}

void ChatModel::SaveOutbox(const std::string& p_AccountId)
{
  if (m_ProfilesDir.empty()) return;

  const std::string dir = m_ProfilesDir + "/" + p_AccountId;
  if (!FileUtil::IsDir(dir)) return;

  // message texts: the file is readable by the user only, like the message cache
  json outbox = json::array();
  for (const OutboxEntry& entry : m_Accounts[p_AccountId].outbox)
  {
    if (entry.state == "sent") continue;

    outbox.push_back(json{ { "id", entry.id }, { "chatId", entry.chatId }, { "text", entry.text },
                           { "quotedId", entry.quotedId }, { "filePath", entry.filePath },
                           { "isVoice", entry.isVoice }, { "poll", entry.poll }, { "time", entry.time },
                           { "state", entry.state } });
  }

  const std::string path = dir + "/braids-outbox.json";
  if (outbox.empty())
  {
    if (FileUtil::Exists(path))
    {
      FileUtil::RmFile(path);
    }

    return;
  }

  FileUtil::WriteFile(path + ".tmp", json{ { "outbox", outbox } }.dump(2));
  chmod((path + ".tmp").c_str(), S_IRUSR | S_IWUSR);
  FileUtil::Move(path + ".tmp", path);
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
  if (StrUtil::StartsWith(p_MsgId, "braids-out-"))
  {
    // not sent (yet): only braids has it
    AccountState& account = GetAccountOrThrow(p_AccountId);
    for (const OutboxEntry& entry : account.outbox)
    {
      if ((entry.id == p_MsgId) && (entry.chatId == p_ChatId))
      {
        RemoveOutboxEntry(p_AccountId, p_MsgId);
        return;
      }
    }

    throw ApiException(ApiErrorUnknownMessage, "unknown message");
  }

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

  AccountState& account = m_Accounts[p_AccountId];
  if (account.markedUnread.erase(p_ChatId) > 0)
  {
    SaveAccountState(p_AccountId);
    MarkChatDirty(p_AccountId, p_ChatId);
    FlushDirtyChats();
  }
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

// Known avatars of the given users / chats; the others are requested once per
// session from the protocol and arrive as avatar.updated events.
json ChatModel::GetAvatars(const std::string& p_AccountId, const std::vector<std::string>& p_Ids)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  const bool hasAvatars = HasFeature(p_AccountId, FeatureAvatars);
  json avatars = json::object();
  for (const auto& id : p_Ids)
  {
    auto ait = account.avatars.find(id);
    if (ait != account.avatars.end())
    {
      avatars[id] = ait->second;
      continue;
    }

    // the picture of the last run at once; the protocol checks it again
    auto sit = account.savedAvatars.find(id);
    if (sit != account.savedAvatars.end())
    {
      avatars[id] = sit->second;
    }

    if (hasAvatars && account.avatarsRequested.insert(id).second)
    {
      std::shared_ptr<GetAvatarRequest> getAvatarRequest = std::make_shared<GetAvatarRequest>();
      getAvatarRequest->id = id;
      SendProtocolRequest(p_AccountId, getAvatarRequest);
    }
  }

  return json{ { "accountId", p_AccountId }, { "avatars", avatars } };
}

// Full-size picture of a user / chat for the picture viewer: the path when
// already fetched, else requested from the protocol; it arrives as an
// avatar.full event (the small avatar as path when the protocol has nothing
// bigger, "" when there is no picture).
json ChatModel::GetFullAvatar(const std::string& p_AccountId, const std::string& p_Id)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  json result = json{ { "accountId", p_AccountId }, { "id", p_Id } };
  auto fit = account.fullAvatars.find(p_Id);
  if (fit != account.fullAvatars.end())
  {
    result["path"] = fit->second;
  }
  else if (HasFeature(p_AccountId, FeatureAvatars))
  {
    std::shared_ptr<GetAvatarRequest> getAvatarRequest = std::make_shared<GetAvatarRequest>();
    getAvatarRequest->id = p_Id;
    getAvatarRequest->full = true;
    SendProtocolRequest(p_AccountId, getAvatarRequest);
  }
  else
  {
    auto ait = account.avatars.find(p_Id);
    result["path"] = (ait != account.avatars.end()) ? ait->second : "";
  }

  return result;
}

// Known descriptions (group description, user bio) of the given users /
// chats; the others are requested once per session from the protocol and
// arrive as description.updated events.
json ChatModel::GetDescriptions(const std::string& p_AccountId, const std::vector<std::string>& p_Ids)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  const bool hasDescriptions = HasFeature(p_AccountId, FeatureDescriptions);
  json descriptions = json::object();
  for (const auto& id : p_Ids)
  {
    auto dit = account.descriptions.find(id);
    if (dit != account.descriptions.end())
    {
      descriptions[id] = dit->second;
    }
    else if (hasDescriptions && account.descriptionsRequested.insert(id).second)
    {
      std::shared_ptr<GetDescriptionRequest> getDescriptionRequest = std::make_shared<GetDescriptionRequest>();
      getDescriptionRequest->id = id;
      SendProtocolRequest(p_AccountId, getDescriptionRequest);
    }
  }

  return json{ { "accountId", p_AccountId }, { "descriptions", descriptions } };
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

void ChatModel::JoinChat(const std::string& p_AccountId, const std::string& p_Link)
{
  GetAccountOrThrow(p_AccountId);
  if (!HasFeature(p_AccountId, FeatureJoinChat))
  {
    throw ApiException(ApiErrorUnsupported, "joining groups by link is not supported for this account");
  }

  std::shared_ptr<JoinChatRequest> joinChatRequest = std::make_shared<JoinChatRequest>();
  joinChatRequest->link = p_Link;
  SendProtocolRequest(p_AccountId, joinChatRequest);
}

void ChatModel::CreateGroup(const std::string& p_AccountId, const std::string& p_Name,
                            const std::vector<std::string>& p_UserIds)
{
  GetAccountOrThrow(p_AccountId);
  if (!HasFeature(p_AccountId, FeatureCreateGroup))
  {
    throw ApiException(ApiErrorUnsupported, "creating groups is not supported for this account");
  }

  if (p_UserIds.empty())
  {
    throw ApiException(ApiErrorInvalidParams, "a group needs at least one member");
  }

  std::shared_ptr<CreateGroupRequest> createGroupRequest = std::make_shared<CreateGroupRequest>();
  createGroupRequest->name = p_Name;
  createGroupRequest->userIds = p_UserIds;
  SendProtocolRequest(p_AccountId, createGroupRequest);
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

void ChatModel::MuteChat(const std::string& p_AccountId, const std::string& p_ChatId, bool p_IsMuted)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  auto cit = account.chatInfos.find(p_ChatId);
  if (cit == account.chatInfos.end())
  {
    throw ApiException(ApiErrorUnknownChat, "unknown chat");
  }

  // the same as the messenger's own setting: no override needed
  if (p_IsMuted == cit->second.isMuted)
  {
    account.muted.erase(p_ChatId);
  }
  else
  {
    account.muted[p_ChatId] = p_IsMuted;
  }

  SaveAccountState(p_AccountId);
  MarkChatDirty(p_AccountId, p_ChatId);
  FlushDirtyChats();
}

void ChatModel::MarkUnread(const std::string& p_AccountId, const std::string& p_ChatId)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  if (!account.chatInfos.count(p_ChatId))
  {
    throw ApiException(ApiErrorUnknownChat, "unknown chat");
  }

  account.markedUnread.insert(p_ChatId);
  SaveAccountState(p_AccountId);
  MarkChatDirty(p_AccountId, p_ChatId);
  FlushDirtyChats();
}

void ChatModel::SetShowJoined(const std::string& p_AccountId, bool p_Show)
{
  AccountState& account = GetAccountOrThrow(p_AccountId);
  account.showJoined = p_Show;
  if (p_Show)
  {
    // the chats left out come back
    for (const std::string& chatId : account.hiddenChats)
    {
      MarkChatDirty(p_AccountId, chatId);
    }

    account.hiddenChats.clear();
  }

  SaveAccountState(p_AccountId);
  FlushDirtyChats();
}

bool ChatModel::GetShowJoined(const std::string& p_AccountId)
{
  auto ait = m_Accounts.find(p_AccountId);
  return (ait != m_Accounts.end()) && ait->second.showJoined;
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

std::string ChatModel::SelfId(const std::string& p_AccountId)
{
  std::shared_ptr<Protocol> protocol = m_ProtocolGetter(p_AccountId);
  return protocol ? protocol->GetSelfId() : "";
}

// A vote (voterId: optionIds, none = retracted) or, with poll set, the poll
// as counted by the server; closed ends the poll.
void ChatModel::ApplyPollNotify(ChatMessage& p_ChatMessage, const NewPollNotify& p_Notify)
{
  json poll = json::parse(p_ChatMessage.poll, nullptr, false);
  if (!p_Notify.poll.empty())
  {
    json newPoll = json::parse(p_Notify.poll, nullptr, false);
    if (newPoll.is_object())
    {
      if (poll.is_object() && poll.value("closed", false)) newPoll["closed"] = true;
      poll = newPoll;
    }
  }
  else if (!p_Notify.voterId.empty())
  {
    json votes = json::parse(p_ChatMessage.pollVotes.empty() ? "{}" : p_ChatMessage.pollVotes, nullptr, false);
    if (!votes.is_object()) votes = json::object();
    if (p_Notify.optionIds.empty())
    {
      votes.erase(p_Notify.voterId);
    }
    else
    {
      votes[p_Notify.voterId] = p_Notify.optionIds;
    }

    p_ChatMessage.pollVotes = votes.dump();
  }

  if (p_Notify.closed && poll.is_object())
  {
    poll["closed"] = true;
  }

  if (poll.is_object())
  {
    p_ChatMessage.poll = poll.dump();
  }
}

void ChatModel::OnPoll(const std::string& p_AccountId, std::shared_ptr<NewPollNotify> p_Notify)
{
  AccountState& account = m_Accounts[p_AccountId];
  const std::string& chatId = p_Notify->chatId;
  ChatMessage* chatMessage = nullptr;
  try
  {
    chatMessage = &GetMessageOrThrow(p_AccountId, chatId, p_Notify->msgId);
  }
  catch (const ApiException&)
  {
  }

  if ((chatMessage == nullptr) || chatMessage->poll.empty())
  {
    // the vote came before its poll (history sync): applied when the poll arrives
    auto& pending = account.pendingPollVotes[chatId + "\x01" + p_Notify->msgId];
    if ((account.pendingPollVotes.size() < 2000) && (pending.size() < 500))
    {
      pending.push_back(p_Notify);
    }

    return;
  }

  ApplyPollNotify(*chatMessage, *p_Notify);
  MessageCache::UpdateMessagePoll(p_AccountId, chatId, p_Notify->msgId, chatMessage->poll,
                                  chatMessage->pollVotes.empty() ? "" : chatMessage->pollVotes);
  Emit("message.updated", MessageToJson(p_AccountId, chatId, *chatMessage));
}

void ChatModel::VotePoll(const std::string& p_AccountId, const std::string& p_ChatId, const std::string& p_MsgId,
                         const std::vector<std::string>& p_OptionIds)
{
  GetAccountOrThrow(p_AccountId);
  ChatMessage& chatMessage = GetMessageOrThrow(p_AccountId, p_ChatId, p_MsgId);
  json poll = json::parse(chatMessage.poll, nullptr, false);
  if (!poll.is_object())
  {
    throw ApiException(ApiErrorInvalidParams, "not a poll");
  }

  if (poll.value("closed", false))
  {
    throw ApiException(ApiErrorInvalidParams, "the poll has ended");
  }

  if (!poll.value("multiple", false) && (p_OptionIds.size() > 1))
  {
    throw ApiException(ApiErrorInvalidParams, "only one option can be chosen");
  }

  std::vector<std::string> optionTexts;
  const json options = poll.value("options", json::array());
  for (const auto& optionId : p_OptionIds)
  {
    bool found = false;
    for (size_t i = 0; i < options.size(); ++i)
    {
      const std::string id = options[i].value("id", std::to_string(i));
      if (id == optionId)
      {
        optionTexts.push_back(options[i].value("text", ""));
        found = true;
        break;
      }
    }

    if (!found)
    {
      throw ApiException(ApiErrorInvalidParams, "unknown option " + optionId);
    }
  }

  std::shared_ptr<VotePollRequest> votePollRequest = std::make_shared<VotePollRequest>();
  votePollRequest->chatId = p_ChatId;
  votePollRequest->msgId = p_MsgId;
  votePollRequest->pollSenderId = chatMessage.senderId;
  votePollRequest->pollIsOutgoing = chatMessage.isOutgoing;
  votePollRequest->optionIds = p_OptionIds;
  votePollRequest->optionTexts = optionTexts;
  SendProtocolRequest(p_AccountId, votePollRequest);

  // votes counted here: the own one at once (most messengers do not echo it);
  // where the server counts, its update follows
  if (!poll.contains("counts"))
  {
    NewPollNotify vote(p_AccountId);
    vote.chatId = p_ChatId;
    vote.msgId = p_MsgId;
    vote.voterId = SelfId(p_AccountId);
    vote.optionIds = p_OptionIds;
    if (vote.voterId.empty()) vote.voterId = s_ReactionsSelfId;
    ApplyPollNotify(chatMessage, vote);
    MessageCache::UpdateMessagePoll(p_AccountId, p_ChatId, p_MsgId, "", chatMessage.pollVotes);
    Emit("message.updated", MessageToJson(p_AccountId, p_ChatId, chatMessage));
  }
}

// The poll for the API: options with their votes (voters where the
// messenger reports them), the own choice, closed.
json ChatModel::PollToJson(const std::string& p_AccountId, const ChatMessage& p_ChatMessage)
{
  if (p_ChatMessage.poll.empty()) return json();

  const json poll = json::parse(p_ChatMessage.poll, nullptr, false);
  if (!poll.is_object()) return json();

  const json votes = json::parse(p_ChatMessage.pollVotes.empty() ? "{}" : p_ChatMessage.pollVotes, nullptr, false);
  const bool counted = poll.contains("counts");
  const std::string selfId = SelfId(p_AccountId);
  json options = json::array();
  json own = counted ? poll.value("own", json::array()) : json::array();
  int total = counted ? poll.value("total", 0) : 0;
  const json pollOptions = poll.value("options", json::array());
  for (size_t i = 0; i < pollOptions.size(); ++i)
  {
    const json& option = pollOptions[i];
    const std::string id = option.is_object() ? option.value("id", std::to_string(i)) : std::to_string(i);
    json voters = json::array();
    int count = 0;
    if (counted)
    {
      const json counts = poll["counts"];
      count = (counts.is_object() && counts.contains(id) && counts[id].is_number()) ? counts[id].get<int>() : 0;
    }
    else if (votes.is_object())
    {
      for (const auto& vote : votes.items())
      {
        if (!vote.value().is_array()) continue;

        for (const auto& chosen : vote.value())
        {
          if (chosen.is_string() && (chosen.get<std::string>() == id))
          {
            ++count;
            const bool isSelf = (vote.key() == selfId) || (vote.key() == s_ReactionsSelfId);
            if (isSelf)
            {
              own.push_back(id);
            }
            else
            {
              voters.push_back(vote.key());
            }
          }
        }
      }
    }

    options.push_back(json{ { "id", id }, { "text", option.is_object() ? option.value("text", "") : "" },
                            { "votes", count }, { "voters", voters } });
  }

  if (!counted && votes.is_object())
  {
    for (const auto& vote : votes.items())
    {
      if (vote.value().is_array() && !vote.value().empty()) ++total;
    }
  }

  return json{ { "question", poll.value("question", "") }, { "options", options },
               { "multiple", poll.value("multiple", false) }, { "closed", poll.value("closed", false) },
               { "anonymous", poll.value("anonymous", false) }, { "own", own }, { "total", total } };
}

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

    if (ait->second.hiddenChats.count(dirtyChat.second)) continue;

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

  int unreadMessages = 0;
  int unreadEvents = 0;
  UnreadCount(m_Accounts[p_AccountId], p_ChatInfo, unreadMessages, unreadEvents);

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
    { "isUnread", p_ChatInfo.isUnread || m_Accounts[p_AccountId].markedUnread.count(p_ChatInfo.id) },
    { "isUnreadMention", p_ChatInfo.isUnreadMention },
    { "unreadCount", unreadMessages },
    { "unreadEvents", unreadEvents },
    { "isMuted", IsMuted(m_Accounts[p_AccountId], p_ChatInfo) },
    { "isPinned", p_ChatInfo.isPinned },
    { "isArchived", p_ChatInfo.isArchived },
    { "lastActivity", lastActivity },
    { "lastMessage", (lastMessage != nullptr) ? MessageToJson(p_AccountId, p_ChatInfo.id, *lastMessage) : json() },
    { "typing", typing },
  };
}

const ChatMessage* ChatModel::FindMessage(const std::string& p_AccountId, const std::string& p_ChatId,
                                          const std::string& p_MsgId)
{
  auto ait = m_Accounts.find(p_AccountId);
  if (ait == m_Accounts.end()) return nullptr;

  auto cit = ait->second.chats.find(p_ChatId);
  if (cit == ait->second.chats.end()) return nullptr;

  auto mit = cit->second.messages.find(p_MsgId);
  return (mit != cit->second.messages.end()) ? &mit->second : nullptr;
}

void ChatModel::RequestQuotedMessage(const std::string& p_AccountId, const std::string& p_ChatId,
                                     const std::string& p_MsgId)
{
  const std::string key = p_ChatId + "\n" + p_MsgId;
  if (!m_QuoteRequested[p_AccountId].insert(key).second) return;

  m_QuoteAwaited[p_AccountId].insert(key);
  std::shared_ptr<GetMessageRequest> getMessageRequest = std::make_shared<GetMessageRequest>();
  getMessageRequest->chatId = p_ChatId;
  getMessageRequest->msgId = p_MsgId;
  getMessageRequest->cached = true;
  LOG_TRACE("request quoted message %s in %s", p_MsgId.c_str(), p_ChatId.c_str());
  SendProtocolRequest(p_AccountId, getMessageRequest);
}

// a fetched quoted message arrived: show its text in the replies
void ChatModel::UpdateQuotingMessages(const std::string& p_AccountId, const std::string& p_ChatId,
                                      const std::string& p_MsgId)
{
  auto ait = m_QuoteAwaited.find(p_AccountId);
  if ((ait == m_QuoteAwaited.end()) || (ait->second.erase(p_ChatId + "\n" + p_MsgId) == 0)) return;

  for (const auto& message : m_Accounts[p_AccountId].chats[p_ChatId].messages)
  {
    if (message.second.quotedId == p_MsgId)
    {
      Emit("message.updated", MessageToJson(p_AccountId, p_ChatId, message.second));
    }
  }
}

json ChatModel::MessageToJson(const std::string& p_AccountId, const std::string& p_ChatId,
                              const ChatMessage& p_ChatMessage)
{
  json quoted;
  if (!p_ChatMessage.quotedId.empty())
  {
    // protocols like Matrix send only the quoted id: take text and sender
    // from the quoted message, fetching it when it is not loaded
    std::string quotedText = p_ChatMessage.quotedText;
    std::string quotedSender = p_ChatMessage.quotedSender;
    if (quotedText.empty() || quotedSender.empty())
    {
      const ChatMessage* quotedMessage = FindMessage(p_AccountId, p_ChatId, p_ChatMessage.quotedId);
      if (quotedMessage != nullptr)
      {
        if (quotedText.empty())
        {
          quotedText = quotedMessage->isDeleted ? "Message deleted" : quotedMessage->text;
        }

        if (quotedText.empty() && !quotedMessage->fileInfo.empty())
        {
          FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(quotedMessage->fileInfo);
          quotedText = FileUtil::BaseName(fileInfo.filePath);
        }

        if (quotedSender.empty())
        {
          quotedSender = quotedMessage->senderId;
        }
      }
      else
      {
        RequestQuotedMessage(p_AccountId, p_ChatId, p_ChatMessage.quotedId);
      }
    }

    quoted = json{
      { "id", p_ChatMessage.quotedId },
      { "text", quotedText },
      { "senderId", quotedSender },
      { "senderName", GetContactName(p_AccountId, quotedSender) },
    };
  }

  json file;
  if (!p_ChatMessage.fileInfo.empty())
  {
    FileInfo fileInfo = ProtocolUtil::FileInfoFromHex(p_ChatMessage.fileInfo);
    std::string mimeType;
    std::string kind;
    FileTypeAndKind(fileInfo, mimeType, kind);
    std::string path = fileInfo.filePath;
    FileStatus status = fileInfo.fileStatus;
    std::string name = FileUtil::BaseName(fileInfo.filePath);
    std::string type = mimeType;
    // videos WhatsApp saved as .f4v before the extension fix (the path is
    // fixed when a message arrives): moved to .mp4 once, so players and the
    // web view recognise them
    if ((path.size() > 4) && (StrUtil::ToLower(path.substr(path.size() - 4)) == ".f4v"))
    {
      const std::string mp4 = path.substr(0, path.size() - 4) + ".mp4";
      if (!FileUtil::Exists(mp4) && FileUtil::Exists(path) && (status == FileStatusDownloaded))
      {
        FileUtil::Move(path, mp4);
      }

      if (FileUtil::Exists(mp4))
      {
        path = mp4;
      }

      name = name.substr(0, name.size() - 4) + ".mp4";
      type = "video/mp4";
    }

    // a local file that is gone (cache cleaned, sent from a temp file): to be
    // downloaded again if the messenger has it, else not available
    if (((status == FileStatusDownloaded) || (status == FileStatusNone)) && !path.empty() && (path.at(0) == '/') &&
        !FileUtil::Exists(path))
    {
      status = fileInfo.fileId.empty() ? FileStatusNone : FileStatusNotDownloaded;
      path.clear();
    }

    // voice messages: the length for previews (Ogg/Opus files once downloaded)
    const int durationSec = ((status == FileStatusDownloaded) && !path.empty() &&
                             ((type.find("ogg") != std::string::npos) || (type.find("opus") != std::string::npos)))
      ? OggOpusDurationSec(path) : 0;

    file = json{
      { "id", fileInfo.fileId },
      { "path", path },
      { "name", name },
      { "mimeType", type },
      { "kind", kind },
      { "status", FileStatusToString(status) },
      { "durationSec", durationSec },
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
    { "system", SystemText(p_ChatMessage) },
    { "link", p_ChatMessage.link },
    { "poll", PollToJson(p_AccountId, p_ChatMessage) },
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
    { "muteChat", true }, // braids' own setting, for every protocol
    { "pinMessage", p_Protocol->HasFeature(FeaturePinMessage) },
    { "avatars", p_Protocol->HasFeature(FeatureAvatars) },
    { "descriptions", p_Protocol->HasFeature(FeatureDescriptions) },
    { "joinChat", p_Protocol->HasFeature(FeatureJoinChat) },
    { "createGroup", p_Protocol->HasFeature(FeatureCreateGroup) },
    { "polls", p_Protocol->HasFeature(FeaturePolls) },
    { "pollsInGroupsOnly", p_ProtocolName == "Telegram" }, // Telegram: polls only in groups
    { "limitedReactions", p_Protocol->HasFeature(FeatureLimitedReactions) },
    { "readReceiptsPerMessage", HasPerMessageReadReceipts(p_ProtocolName) },
  };
}
