// gowm_polls.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"time"

	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
)

// Polls: the poll goes to braidsd as ChatMessage.poll (JSON), votes as
// NewPollNotify, counted by braidsd. WhatsApp votes name the options by the
// SHA-256 of their text, so that is the option id.

type pollOptionJSON struct {
	ID   string `json:"id,omitempty"`
	Text string `json:"text"`
}

type pollJSON struct {
	Question string           `json:"question"`
	Options  []pollOptionJSON `json:"options"`
	Multiple bool             `json:"multiple"`
}

func pollOptionId(name string) string {
	sum := sha256.Sum256([]byte(name))
	return hex.EncodeToString(sum[:])
}

// pollCreation: the poll of a message in any of the versions WhatsApp
// uses (current apps send V5 / V6; V4 wraps the message), nil if none.
func pollCreation(msg *waE2E.Message) *waE2E.PollCreationMessage {
	switch {
	case msg == nil:
		return nil
	case msg.GetPollCreationMessageV6() != nil:
		return msg.GetPollCreationMessageV6()
	case msg.GetPollCreationMessageV5() != nil:
		return msg.GetPollCreationMessageV5()
	case msg.GetPollCreationMessageV4() != nil:
		return pollCreation(msg.GetPollCreationMessageV4().GetMessage())
	case msg.GetPollCreationMessageV3() != nil:
		return msg.GetPollCreationMessageV3()
	case msg.GetPollCreationMessageV2() != nil:
		return msg.GetPollCreationMessageV2()
	default:
		return msg.GetPollCreationMessage()
	}
}

func isPollCreation(msg *waE2E.Message) bool {
	return pollCreation(msg) != nil
}

func (handler *WmEventHandler) HandlePollCreationMessage(messageInfo types.MessageInfo, msg *waE2E.Message, isSyncRead bool) {
	connId := handler.connId
	client := GetClient(connId)
	creation := pollCreation(msg)
	if client == nil || creation == nil {
		return
	}

	LOG_TRACE(fmt.Sprintf("poll %s with %d options", messageInfo.ID, len(creation.GetOptions())))

	poll := pollJSON{Question: creation.GetName(), Multiple: creation.GetSelectableOptionsCount() != 1}
	for _, option := range creation.GetOptions() {
		poll.Options = append(poll.Options, pollOptionJSON{ID: pollOptionId(option.GetOptionName()), Text: option.GetOptionName()})
	}

	data, err := json.Marshal(poll)
	if err != nil {
		return
	}

	chatId := GetChatId(client, &messageInfo.Chat, &messageInfo.Sender)
	senderId := GetUserId(client, &messageInfo.Chat, &messageInfo.Sender)
	fromMe := messageInfo.IsFromMe
	isSelfChat := IsSelfChat(client, chatId)
	isRead := IsRead(isSyncRead, isSelfChat, fromMe, messageInfo.Timestamp, GetTimeRead(connId, chatId))
	handler.ProcessMessageInfo(messageInfo)
	CWmNewPollMessageNotify(connId, chatId, messageInfo.ID, senderId, BoolToInt(fromMe), int(messageInfo.Timestamp.Unix()),
		BoolToInt(isRead), string(data))
}

func (handler *WmEventHandler) HandlePollUpdateMessage(messageInfo types.MessageInfo, msg *waE2E.Message) {
	connId := handler.connId
	client := GetClient(connId)
	if client == nil {
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	vote, err := client.DecryptPollVote(ctx, &events.Message{Info: messageInfo, Message: msg})
	if err != nil {
		LOG_WARNING(fmt.Sprintf("decrypt poll vote failed %s: %v", messageInfo.ID, err))
		return
	}

	options := []string{}
	for _, hash := range vote.GetSelectedOptions() {
		options = append(options, hex.EncodeToString(hash))
	}

	data, _ := json.Marshal(options)
	chatId := GetChatId(client, &messageInfo.Chat, &messageInfo.Sender)
	voterId := GetUserId(client, &messageInfo.Chat, &messageInfo.Sender)
	pollId := msg.GetPollUpdateMessage().GetPollCreationMessageKey().GetID()
	CWmNewPollNotify(connId, chatId, pollId, voterId, string(data), 0)
}

// WmSendPoll sends a poll: {"question", "options": [{"text"}], "multiple"}.
func WmSendPoll(connId int, chatId string, pollJson string) int {
	client := GetClient(connId)
	if client == nil {
		return -1
	}

	var poll pollJSON
	if err := json.Unmarshal([]byte(pollJson), &poll); err != nil {
		return -1
	}

	names := []string{}
	for _, option := range poll.Options {
		names = append(names, option.Text)
	}

	selectable := 1
	if poll.Multiple {
		selectable = 0 // any number
	}

	chatJid, err := types.ParseJID(chatId)
	if err != nil {
		return -1
	}

	// as current WhatsApp apps (and mautrix-whatsapp) do: single choice as V3
	message := client.BuildPollCreation(poll.Question, names, selectable)
	if selectable == 1 {
		message.PollCreationMessageV3 = message.PollCreationMessage
		message.PollCreationMessage = nil
	}
	resp, err := client.SendMessage(context.Background(), chatJid, message)
	if err != nil {
		LOG_WARNING(fmt.Sprintf("send poll error %v", err))
		return -1
	}

	// echo, as for other sent messages
	var messageInfo types.MessageInfo
	messageInfo.Chat = chatJid
	messageInfo.IsFromMe = true
	if client.Store.ID != nil {
		messageInfo.Sender = *client.Store.ID
	}
	messageInfo.ID = resp.ID
	messageInfo.Timestamp = resp.Timestamp
	GetHandler(connId).HandleMessage(messageInfo, message, false)
	return 0
}

// WmVotePoll votes in a poll (optionTextsJson: the chosen options' texts, none retracts).
func WmVotePoll(connId int, chatId string, msgId string, pollSenderId string, fromMe int, optionTextsJson string) int {
	client := GetClient(connId)
	if client == nil {
		return -1
	}

	names := []string{}
	if err := json.Unmarshal([]byte(optionTextsJson), &names); err != nil {
		return -1
	}

	chatJid, err := types.ParseJID(chatId)
	if err != nil {
		return -1
	}

	senderJid, _ := types.ParseJID(pollSenderId)
	if fromMe != 0 && client.Store.ID != nil {
		senderJid = client.Store.ID.ToNonAD()
	} else if pollSenderId == "" {
		senderJid = chatJid
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// the poll's secret is stored under the chat and sender as WhatsApp
	// addressed them, which may be the LID where braids has the phone number
	withLID := func(jid types.JID) []types.JID {
		jids := []types.JID{jid}
		if jid.Server == types.DefaultUserServer {
			if lid, err := client.Store.LIDs.GetLIDForPN(ctx, jid); err == nil && !lid.IsEmpty() {
				jids = append(jids, lid)
			}
		}
		return jids
	}

	var message *waE2E.Message
	for _, chat := range withLID(chatJid) {
		for _, sender := range withLID(senderJid) {
			pollInfo := &types.MessageInfo{
				MessageSource: types.MessageSource{Chat: chat, Sender: sender, IsFromMe: fromMe != 0,
					IsGroup: chat.Server == types.GroupServer},
				ID: msgId,
			}
			if message, err = client.BuildPollVote(ctx, pollInfo, names); err == nil {
				break
			}
		}
		if message != nil && err == nil {
			break
		}
	}

	if err != nil || message == nil {
		LOG_WARNING(fmt.Sprintf("build poll vote error %s: %v", msgId, err))
		return -1
	}

	if _, err := client.SendMessage(ctx, chatJid, message); err != nil {
		LOG_WARNING(fmt.Sprintf("send poll vote error %v", err))
		return -1
	}

	return 0
}
