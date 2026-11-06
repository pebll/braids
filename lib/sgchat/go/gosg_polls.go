// gosg_polls.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"strconv"
	"time"

	"github.com/google/uuid"
	"go.mau.fi/mautrix-signal/pkg/signalmeow"
	"go.mau.fi/mautrix-signal/pkg/signalmeow/protobuf/signalpb"
	"go.mau.fi/mautrix-signal/pkg/signalmeow/types"
	"google.golang.org/protobuf/proto"
)

// Polls: the poll goes to braidsd as ChatMessage.poll (JSON, options by
// index), votes and the end as NewPollNotify, counted by braidsd. A poll is
// named by its sent timestamp, as messages are.

type pollOptionJSON struct {
	ID   string `json:"id,omitempty"`
	Text string `json:"text"`
}

type pollJSON struct {
	Question string           `json:"question"`
	Options  []pollOptionJSON `json:"options"`
	Multiple bool             `json:"multiple"`
}

func pollToJSON(create *signalpb.DataMessage_PollCreate) string {
	poll := pollJSON{Question: create.GetQuestion(), Multiple: create.GetAllowMultiple()}
	for i, option := range create.GetOptions() {
		poll.Options = append(poll.Options, pollOptionJSON{ID: strconv.Itoa(i), Text: option})
	}

	data, err := json.Marshal(poll)
	if err != nil {
		return ""
	}

	return string(data)
}

// handlePoll handles poll messages; false if msg is none.
func (handler *SgEventHandler) handlePoll(chatId string, senderId string, fromMe bool, timeSent int, msg *signalpb.DataMessage) bool {
	connId := handler.connId
	switch {
	case msg.GetPollCreate() != nil:
		poll := pollToJSON(msg.GetPollCreate())
		if poll != "" {
			msgId := fmt.Sprintf("%d", msg.GetTimestamp())
			CSgNewPollMessageNotify(connId, chatId, msgId, senderId, BoolToInt(fromMe), timeSent, BoolToInt(fromMe), poll)
		}
		return true

	case msg.GetPollVote() != nil:
		vote := msg.GetPollVote()
		options := []string{}
		for _, index := range vote.GetOptionIndexes() {
			options = append(options, strconv.Itoa(int(index)))
		}
		data, _ := json.Marshal(options)
		CSgNewPollNotify(connId, chatId, fmt.Sprintf("%d", vote.GetTargetSentTimestamp()), senderId, string(data), 0)
		return true

	case msg.GetPollTerminate() != nil:
		CSgNewPollNotify(connId, chatId, fmt.Sprintf("%d", msg.GetPollTerminate().GetTargetSentTimestamp()), "", "[]", 1)
		return true
	}

	return false
}

func sendDataMessage(ctx context.Context, client *signalmeow.Client, chatId string, dataMsg *signalpb.DataMessage) error {
	content := &signalpb.Content{Content: &signalpb.Content_DataMessage{DataMessage: dataMsg}}
	recipient := StringToServiceID(chatId)
	if recipient.IsEmpty() {
		_, err := client.SendGroupMessage(ctx, types.GroupIdentifier(chatId), content)
		return err
	}

	if result := client.SendMessage(ctx, recipient, content); !result.WasSuccessful {
		return fmt.Errorf("not sent")
	}

	return nil
}

// SgSendPoll sends a poll: {"question", "options": [{"text"}], "multiple"}.
func SgSendPoll(connId int, chatId string, pollJson string) int {
	client := GetClient(connId)
	device := GetDevice(connId)
	if client == nil || device == nil {
		return -1
	}

	var poll pollJSON
	if err := json.Unmarshal([]byte(pollJson), &poll); err != nil {
		return -1
	}

	create := &signalpb.DataMessage_PollCreate{Question: proto.String(poll.Question), AllowMultiple: proto.Bool(poll.Multiple)}
	for _, option := range poll.Options {
		create.Options = append(create.Options, option.Text)
	}

	now := time.Now().UnixMilli()
	dataMsg := &signalpb.DataMessage{
		Timestamp:               proto.Uint64(uint64(now)),
		PollCreate:              create,
		RequiredProtocolVersion: proto.Uint32(uint32(signalpb.DataMessage_POLLS)),
	}

	if err := sendDataMessage(context.TODO(), client, chatId, dataMsg); err != nil {
		LOG_WARNING(fmt.Sprintf("send poll failed: %v", err))
		return -1
	}

	// echo, as for other sent messages
	CSgNewPollMessageNotify(connId, chatId, fmt.Sprintf("%d", now), UUIDToString(device.ACI), 1, int(now/1000), 1,
		pollToJSON(create))
	return 0
}

// SgVotePoll votes in a poll (optionIdsJson: option indexes, none retracts).
func SgVotePoll(connId int, chatId string, msgId string, pollSenderId string, optionIdsJson string) int {
	client := GetClient(connId)
	if client == nil {
		return -1
	}

	ids := []string{}
	if err := json.Unmarshal([]byte(optionIdsJson), &ids); err != nil {
		return -1
	}

	targetTimestamp, err := strconv.ParseUint(msgId, 10, 64)
	author := StringToUUID(pollSenderId)
	if err != nil || author == uuid.Nil {
		return -1
	}

	vote := &signalpb.DataMessage_PollVote{
		TargetAuthorAciBinary: author[:],
		TargetSentTimestamp:   proto.Uint64(targetTimestamp),
		// the latest vote of a voter wins: counted up with time
		VoteCount: proto.Uint32(uint32(time.Now().Unix())),
	}
	for _, id := range ids {
		if index, err := strconv.Atoi(id); err == nil {
			vote.OptionIndexes = append(vote.OptionIndexes, uint32(index))
		}
	}

	dataMsg := &signalpb.DataMessage{
		Timestamp:               proto.Uint64(uint64(time.Now().UnixMilli())),
		PollVote:                vote,
		RequiredProtocolVersion: proto.Uint32(uint32(signalpb.DataMessage_POLLS)),
	}

	if err := sendDataMessage(context.TODO(), client, chatId, dataMsg); err != nil {
		LOG_WARNING(fmt.Sprintf("send poll vote failed: %v", err))
		return -1
	}

	return 0
}
