// duchat.cpp
//
// Copyright (c) 2020-2026 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "duchat.h"

#include <algorithm>
#include <iostream>
#include <map>

#include <sys/stat.h>

#include "authutil.h"
#include "fileutil.h"
#include "log.h"
#include "status.h"
#include "strutil.h"
#include "timeutil.h"

static const std::string s_GroupId = "The Office_0";

extern "C" DuChat* CreateDuChat()
{
  return new DuChat();
}

DuChat::DuChat()
{
  m_ProfileId = GetName();
}

DuChat::~DuChat()
{
}

std::string DuChat::GetProfileId() const
{
  return m_ProfileId;
}

bool DuChat::HasFeature(ProtocolFeature p_ProtocolFeature) const
{
  static int customFeatures =
    FeatureAutoGetContactsOnLogin | FeatureEditMessagesAnytime | FeatureAvatars | FeatureDescriptions;
  return (p_ProtocolFeature & customFeatures);
}

bool DuChat::IsGroupChat(const std::string& p_ChatId) const
{
  return (p_ChatId == s_GroupId);
}

std::string DuChat::GetSelfId() const
{
  return "Stanley_0";
}

std::string DuChat::GetProfileDisplayName() const
{
  static std::string profileDisplayName = "";
  return profileDisplayName;
}

bool DuChat::SetupProfile(const std::string& p_ProfilesDir, std::string& p_ProfileId)
{
  // exercises the auth flow: phone number, then a verification code ("000000" is rejected)
  std::string phoneNumber;
  if (!AuthUtil::InputPhoneNumber(m_AuthHandler, phoneNumber)) return false;

  std::string code;
  while (true)
  {
    if (!AuthUtil::Input(m_AuthHandler, s_AuthFieldCode, "Verification code (any, except 000000)", code)) return false;

    if (!code.empty() && (code != "000000")) break;

    AuthUtil::ShowError(m_AuthHandler, "Invalid code, please try again.");
  }

  m_ProfileId = m_ProfileId + "_" + phoneNumber;
  std::string profileDir = p_ProfilesDir + "/" + m_ProfileId;

  mkdir(profileDir.c_str(), 0777);
  m_ProfileDir = profileDir;

  p_ProfileId = m_ProfileId;

  return true;
}

bool DuChat::LoadProfile(const std::string& p_ProfilesDir, const std::string& p_ProfileId)
{
  LOG_INFO("load dummy profile");
  m_ProfileId = p_ProfileId;
  m_ProfileDir = p_ProfilesDir + "/" + p_ProfileId;
  return true;
}

bool DuChat::CloseProfile()
{
  m_ProfileId = "";
  return true;
}

bool DuChat::Login()
{
  Status::Set(m_ProfileId, Status::FlagOnline);

  if (!m_Running)
  {
    m_Running = true;
    m_Thread = std::thread(&DuChat::Process, this);

    std::shared_ptr<ConnectNotify> connectNotify =
      std::make_shared<ConnectNotify>(m_ProfileId);
    connectNotify->success = true;

    std::shared_ptr<DeferNotifyRequest> deferNotifyRequest =
      std::make_shared<DeferNotifyRequest>();
    deferNotifyRequest->serviceMessage = connectNotify;
    SendRequest(deferNotifyRequest);
  }
  return true;
}

bool DuChat::Logout()
{
  Status::Clear(m_ProfileId, Status::FlagOnline);

  if (m_Running)
  {
    std::unique_lock<std::mutex> lock(m_ProcessMutex);
    m_Running = false;
    m_ProcessCondVar.notify_one();
  }

  if (m_Thread.joinable())
  {
    m_Thread.join();
  }
  return true;
}

void DuChat::Process()
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

      requestMessage = m_RequestsQueue.front();
      m_RequestsQueue.pop_front();
    }

    PerformRequest(requestMessage);
  }
}

void DuChat::SendRequest(std::shared_ptr<RequestMessage> p_RequestMessage)
{
  std::unique_lock<std::mutex> lock(m_ProcessMutex);
  m_RequestsQueue.push_back(p_RequestMessage);
  m_ProcessCondVar.notify_one();
}


void DuChat::SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>& p_MessageHandler)
{
  m_MessageHandler = p_MessageHandler;
}

void DuChat::PerformRequest(std::shared_ptr<RequestMessage> p_RequestMessage)
{
  if (!m_MessageHandler) return;

  static std::map<std::string, std::vector<ChatMessage>> s_Messages;

  switch (p_RequestMessage->GetMessageType())
  {
    case GetChatsRequestType:
      {
        std::shared_ptr<GetChatsRequest> getChatsRequest =
          std::static_pointer_cast<GetChatsRequest>(p_RequestMessage);
        std::shared_ptr<NewChatsNotify> newChatsNotify =
          std::make_shared<NewChatsNotify>(m_ProfileId);
        std::shared_ptr<NewContactsNotify> newContactsNotify =
          std::make_shared<NewContactsNotify>(m_ProfileId);
        newChatsNotify->success = true;

        static std::vector<std::pair<std::string, std::string>> messages =
        {
          { "Michael",
            "Would I rather be feared or loved? Easy. Both. I want people to "
            "be afraid of how much they love me." },
          { "Dwight",
            "Whenever I'm about to do something, I think, 'Would an idiot do "
            "that?' And if they would, I do not do that thing." },
          { "Jim",
            "Right now, this is just a job. If I advance any higher in this "
            "company, this would be my career. And, uh, if this were my "
            "career, I’d have to throw myself in front of a train." },
          { "Pam", "There's a lot of beauty in ordinary things. Isn't that "
            "kind of the point?" },
          /* { "Stanley", "" }, */
          { "Phyllis",
            "Andy sings beautifully. And he's really good at dancing. He's "
            "a good speaker. But there's just something there you don't want "
            "to look at." },
          { "Kevin",
            "Mini-cupcakes? As in the mini version of regular cupcakes? Which "
            "is already a mini version of cake? Honestly, where does it end "
            "with you people?" },
          { "Ryan", "I'd rather she be alone than with somebody. Is that love?" },
          { "Angela",
            "Malls are just awful and humiliating. They’re just store after "
            "store of these horrible salespeople making a big fuss out of an "
            "adult shopping in a junior’s section. There are petite adults "
            "who are sort of… smaller who need to wear… maybe a kids’ size 10." },
          { "Oscar",
            "Angela's engaged to a gay man. As a gay man, I'm horrified. As a "
            "friend of Angela's, horrified. As a lover of elegant weddings, "
            "I'm a little excited." },
          { "Kelly", "I have a lot of questions. Number one, how dare you?" },
          { "Meredith",
            "Stop fighting! Just on St. Patrick's Day okay? Just one, perfect "
            "day a year. No hassles. No problems. No kids." },
          { "Creed",
            "I am not offended by homosexuality, in the sixties I made love "
            "to many, many women, often outdoors in the mud & rain. It’s "
            "possible a man could’ve slipped in there. There’d be no way of "
            "knowing." },
          { "Darryl",
            "I've been meaning to join a gym for my health. I used to say I "
            "wanted to live long enough to see a black president. I didn't "
            "realize how easy that would be. So now I want to live long "
            "enough to see a really, really gay president. Or a supermodel "
            "president. I want to see all the different kinds of presidents." },
          { "Toby",
            "Oh, I went zip lining my third day in Costa Rica. I guess the "
            "harness wasn't strapped in exactly right. I broke my neck. And, "
            "I've been in the hospital five weeks now. I still haven't seen "
            "the beach. It's nice to have visitors." },
          { "Erin",
            "Whenever I'm sick, it goes away within a few hours. Except that "
            "once when I was in the hospital from age three to six." },
          { "Gabe",
            "Apparently, I bear a passing resemblance to Abraham Lincoln. "
            "Makes it kind of hard for me to go to places like museums, "
            "historical monuments, elementary schools... I don't see it." },
          { "Andy",
            "I went to Cornell. Ever heard of it? I graduated in four years, "
            "I never studied once, I was drunk the whole time, and I sang in "
            "the acapella group, 'Here Comes Treble'." },
          { "Lorem",
            "Lorem ipsum dolor sit amet, ea vel tale virtute qualisque. Ad "
            "eam viris salutandi, per no lucilius corrumpit. Ea gubergren "
            "inciderint cum. An melsaperet insolens dissentiunt. Diam vide "
            "ne sed, elitr causae ea mea. Quo ut graece latine." },
          { "Mixed",
            "Hello 你好! This is a text with mixed English and 中文 characters. "
            "Hello 你好! This is a text with mixed English and 中文 characters. " },
          { "Chinese",
            "一二三。一二三。一二三。一二三。一二三。一二三。一二三。一二三。一二三。一二三。"
            "一二三。一二三。一二三。一二三。一二三。一二三。一二三。一二三。一二三。一二三。" },
          { "No Space",
            "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
            "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz" },
          { "Emoji 🌍",
            "📃📃📃📃📃 📃📃📃📃📃. 📃📃📃📃📃 📃📃📃📃📃. 📃📃📃📃📃 📃📃📃📃📃. "
            "📃📃📃📃📃 📃📃📃📃📃. 📃📃📃📃📃 📃📃📃📃📃." },
          { "Emoji ❤️",
            "👍😊😘😄😅😔😂🎉🙂❤💪🙏🔔🤗👌😮🤔😱👼😍😻😢😬😋🍺😁😛🤞🤣😴😎🍻🖤😇"
            "🤓😝😜😓🪥🎂🥣🎄🥶🌍💚🥪⛈📘🚙💼👏🔐😰🥟😳🍔🎧🗾🍋📝👱🛟😆🐍💬☎📞❌ "
            "💯🥇🥈🥉🎱🧮🛫⏰" },
        };

        static std::vector<std::pair<std::string, std::string>> groupMessages =
        {
          { "Stanley", "Maybe you should go into your office, close the door, "
            "and make some calls about jobs?" },
          { "Michael", "I have a job." },
          { "Andy", "For four more days." },
          { "Pam", "Do you have any leads on a job?" },
          { "Michael",
            "Pam, what you don't understand is that at my level you just don't "
            "look in the want-ads for a job. You are head-hunted." },
          { "Jim", "You've called any headhunters? 🤔" },
          { "Michael", "Any good headhunter knows I am available." },
          { "Dwight",
            "Any really good headhunter would storm your village at sunset with "
            "overwhelming force and cut off your head with a ceremonial knife." },
        };
        std::reverse(groupMessages.begin(), groupMessages.end());

        int64_t t = 1237922000;
        s_Messages.clear();
        newChatsNotify->chatInfos.clear();

        // Individual chats
        for (auto& message : messages)
        {
          std::string name = message.first;
          std::string text = message.second;
          std::string id = name + "_0";
          bool isUnread = false;

          ChatInfo chatInfo;
          chatInfo.id = id;
          chatInfo.lastMessageTime = (t * 1000);
          chatInfo.isUnread = isUnread;
          newChatsNotify->chatInfos.push_back(chatInfo);

          ContactInfo contactInfo;
          contactInfo.id = id;
          contactInfo.name = name;
          newContactsNotify->contactInfos.push_back(contactInfo);

          ChatMessage chatMessage;
          chatMessage.id = id + "_" + std::to_string(t);
          chatMessage.senderId = id;
          chatMessage.text = text;
          chatMessage.timeSent = (t * 1000);
          chatMessage.isOutgoing = false;
          chatMessage.isRead = !isUnread;
          t = t - 100;
          s_Messages[id].push_back(chatMessage);
        }

        // Self
        std::string sname = "Stanley";
        std::string sid = sname + "_0";
        {
          ContactInfo scontactInfo;
          scontactInfo.id = sid;
          scontactInfo.name = sname;
          scontactInfo.isSelf = true;
          newContactsNotify->contactInfos.push_back(scontactInfo);
        }

        // Group chat
        {
          t = 1237962000;
          ChatInfo chatInfo;
          chatInfo.id = s_GroupId;
          chatInfo.lastMessageTime = (t * 1000);
          newChatsNotify->chatInfos.push_back(chatInfo);

          ContactInfo contactInfo;
          contactInfo.id = s_GroupId;
          contactInfo.name = "The Office";
          newContactsNotify->contactInfos.push_back(contactInfo);

          // From others
          bool isFirst = true;
          for (auto& message : groupMessages)
          {
            std::string name = message.first;
            std::string id = name + "_0";

            ChatMessage chatMessage;
            chatMessage.id = id + "_" + std::to_string(t);
            chatMessage.senderId = id;
            chatMessage.text = message.second;
            chatMessage.timeSent = (t * 1000);
            chatMessage.isOutgoing = (id == sid);
            chatMessage.isRead = true;

            if (isFirst)
            {
              isFirst = false;
              chatMessage.reactions.emojiCounts["😨"] = 4;
              chatMessage.reactions.emojiCounts["🤣"] = 2;
            }

            t = t - 100;
            s_Messages[s_GroupId].push_back(chatMessage);
          }
        }

        m_MessageHandler(newChatsNotify);
        m_MessageHandler(newContactsNotify);
      }
      break;

    case GetMessagesRequestType:
      {
        std::shared_ptr<GetMessagesRequest> getMessagesRequest =
          std::static_pointer_cast<GetMessagesRequest>(p_RequestMessage);
        std::shared_ptr<NewMessagesNotify> newMessagesNotify =
          std::make_shared<NewMessagesNotify>(m_ProfileId);
        newMessagesNotify->success = true;
        newMessagesNotify->chatId = getMessagesRequest->chatId;
        newMessagesNotify->cached = false;
        newMessagesNotify->sequence = true;

        newMessagesNotify->fromMsgId = getMessagesRequest->fromMsgId;

        // newest first: the messages older than fromMsgId (all without it)
        const std::vector<ChatMessage>& messages = s_Messages[getMessagesRequest->chatId];
        bool isOlder = getMessagesRequest->fromMsgId.empty();
        for (const auto& chatMessage : messages)
        {
          if (isOlder)
          {
            newMessagesNotify->chatMessages.push_back(chatMessage);
          }
          else if (chatMessage.id == getMessagesRequest->fromMsgId)
          {
            isOlder = true;
          }
        }

        m_MessageHandler(newMessagesNotify);
      }
      break;

    case SendMessageRequestType:
      {
        std::shared_ptr<SendMessageRequest> sendMessageRequest =
          std::static_pointer_cast<SendMessageRequest>(p_RequestMessage);
        const std::string& chatId = sendMessageRequest->chatId;
        ChatMessage chatMessage = sendMessageRequest->chatMessage;
        const int64_t now = TimeUtil::GetCurrentTimeMSec();
        chatMessage.id = GetSelfId() + "_" + std::to_string(now);
        chatMessage.senderId = GetSelfId();
        chatMessage.timeSent = now;
        chatMessage.isOutgoing = true;
        chatMessage.isRead = false;
        for (const auto& quoted : s_Messages[chatId])
        {
          if (quoted.id != chatMessage.quotedId) continue;

          chatMessage.quotedText = quoted.text;
          chatMessage.quotedSender = quoted.senderId;
        }

        s_Messages[chatId].insert(s_Messages[chatId].begin(), chatMessage);

        std::shared_ptr<SendMessageNotify> sendMessageNotify =
          std::make_shared<SendMessageNotify>(m_ProfileId);
        sendMessageNotify->success = true;
        sendMessageNotify->chatId = chatId;
        sendMessageNotify->chatMessage = chatMessage;
        m_MessageHandler(sendMessageNotify);

        // echo, as real protocols do
        std::shared_ptr<NewMessagesNotify> newMessagesNotify =
          std::make_shared<NewMessagesNotify>(m_ProfileId);
        newMessagesNotify->success = true;
        newMessagesNotify->chatId = chatId;
        newMessagesNotify->chatMessages = { chatMessage };
        m_MessageHandler(newMessagesNotify);
      }
      break;

    case EditMessageRequestType:
      {
        std::shared_ptr<EditMessageRequest> editMessageRequest =
          std::static_pointer_cast<EditMessageRequest>(p_RequestMessage);
        for (auto& chatMessage : s_Messages[editMessageRequest->chatId])
        {
          if (chatMessage.id != editMessageRequest->msgId) continue;

          chatMessage.text = editMessageRequest->chatMessage.text;
          chatMessage.isEdited = true;
          std::shared_ptr<NewMessagesNotify> newMessagesNotify =
            std::make_shared<NewMessagesNotify>(m_ProfileId);
          newMessagesNotify->success = true;
          newMessagesNotify->chatId = editMessageRequest->chatId;
          newMessagesNotify->chatMessages = { chatMessage };
          m_MessageHandler(newMessagesNotify);
        }
      }
      break;

    case DeleteMessageRequestType:
      {
        std::shared_ptr<DeleteMessageRequest> deleteMessageRequest =
          std::static_pointer_cast<DeleteMessageRequest>(p_RequestMessage);
        std::vector<ChatMessage>& messages = s_Messages[deleteMessageRequest->chatId];
        messages.erase(std::remove_if(messages.begin(), messages.end(), [&](const ChatMessage& p_Message)
      {
        return p_Message.id == deleteMessageRequest->msgId;
      }), messages.end());

        std::shared_ptr<DeleteMessageNotify> deleteMessageNotify =
          std::make_shared<DeleteMessageNotify>(m_ProfileId);
        deleteMessageNotify->success = true;
        deleteMessageNotify->chatId = deleteMessageRequest->chatId;
        deleteMessageNotify->msgId = deleteMessageRequest->msgId;
        deleteMessageNotify->isOutgoing = true;
        m_MessageHandler(deleteMessageNotify);
      }
      break;

    case SendReactionRequestType:
      {
        std::shared_ptr<SendReactionRequest> sendReactionRequest =
          std::static_pointer_cast<SendReactionRequest>(p_RequestMessage);
        for (auto& chatMessage : s_Messages[sendReactionRequest->chatId])
        {
          if (chatMessage.id != sendReactionRequest->msgId) continue;

          Reactions& reactions = chatMessage.reactions;
          const std::string prev = reactions.senderEmojis[s_ReactionsSelfId];
          if (!prev.empty() && (reactions.emojiCounts[prev] > 0) && (--reactions.emojiCounts[prev] == 0))
          {
            reactions.emojiCounts.erase(prev);
          }

          if (sendReactionRequest->emoji.empty())
          {
            reactions.senderEmojis.erase(s_ReactionsSelfId);
          }
          else
          {
            reactions.senderEmojis[s_ReactionsSelfId] = sendReactionRequest->emoji;
            ++reactions.emojiCounts[sendReactionRequest->emoji];
          }

          std::shared_ptr<NewMessageReactionsNotify> newMessageReactionsNotify =
            std::make_shared<NewMessageReactionsNotify>(m_ProfileId);
          newMessageReactionsNotify->chatId = sendReactionRequest->chatId;
          newMessageReactionsNotify->msgId = sendReactionRequest->msgId;
          newMessageReactionsNotify->reactions = reactions;
          m_MessageHandler(newMessageReactionsNotify);
        }
      }
      break;

    case GetAvatarRequestType:
      {
        // demo pictures for a few contacts and the group, none for the others
        std::shared_ptr<GetAvatarRequest> getAvatarRequest =
          std::static_pointer_cast<GetAvatarRequest>(p_RequestMessage);
        static const std::map<std::string, std::pair<std::string, std::string>> s_Pictures = {
          { "Michael_0", { "#2F64C8", "#F5C26B" } },
          { "Pam_0", { "#C0396B", "#FBE3CF" } },
          { "Jim_0", { "#0F7678", "#E3F2E8" } },
          { s_GroupId, { "#A84A1A", "#F7E8DD" } },
        };

        std::shared_ptr<NewAvatarNotify> newAvatarNotify = std::make_shared<NewAvatarNotify>(m_ProfileId);
        newAvatarNotify->id = getAvatarRequest->id;
        auto pit = s_Pictures.find(getAvatarRequest->id);
        if ((pit != s_Pictures.end()) && !m_ProfileDir.empty())
        {
          const std::string dir = m_ProfileDir + "/avatars";
          mkdir(dir.c_str(), 0700);
          const std::string path = dir + "/" + getAvatarRequest->id + ".svg";
          const std::string svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\">"
            "<rect width=\"64\" height=\"64\" fill=\"" + pit->second.first + "\"/>"
            "<circle cx=\"32\" cy=\"26\" r=\"12\" fill=\"" + pit->second.second + "\"/>"
            "<path d=\"M10 64c2-14 11-22 22-22s20 8 22 22z\" fill=\"" + pit->second.second + "\"/></svg>";
          FileUtil::WriteFile(path, svg);
          newAvatarNotify->filePath = path;
        }

        m_MessageHandler(newAvatarNotify);
      }
      break;

    case GetDescriptionRequestType:
      {
        // demo description for the group and a bio, none for the others
        std::shared_ptr<GetDescriptionRequest> getDescriptionRequest =
          std::static_pointer_cast<GetDescriptionRequest>(p_RequestMessage);
        static const std::map<std::string, std::string> s_Descriptions = {
          { s_GroupId, "Dunder Mifflin Scranton.\nPaper sales and everything else: https://example.com/dundermifflin" },
          { "Michael_0", "World's best boss" },
        };

        std::shared_ptr<NewDescriptionNotify> newDescriptionNotify =
          std::make_shared<NewDescriptionNotify>(m_ProfileId);
        newDescriptionNotify->id = getDescriptionRequest->id;
        auto dit = s_Descriptions.find(getDescriptionRequest->id);
        if (dit != s_Descriptions.end())
        {
          newDescriptionNotify->description = dit->second;
        }

        m_MessageHandler(newDescriptionNotify);
      }
      break;

    case DeferNotifyRequestType:
      {
        std::shared_ptr<DeferNotifyRequest> deferNotifyRequest =
          std::static_pointer_cast<DeferNotifyRequest>(p_RequestMessage);
        m_MessageHandler(deferNotifyRequest->serviceMessage);
      }
      break;

    default:
      LOG_DEBUG("unknown request message %d", p_RequestMessage->GetMessageType());
      break;
  }
}
