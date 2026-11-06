// gomx_polls.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"encoding/json"
	"fmt"
	"strconv"
	"strings"

	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/id"
)

// Polls (MSC3381, the org.matrix.msc3381.* events Element sends). The poll
// goes to braidsd as ChatMessage.poll (JSON); responses and the end as
// NewPollNotify, counted by braidsd (a later response of a user replaces the
// earlier one).

type pollOptionJSON struct {
	ID   string `json:"id,omitempty"`
	Text string `json:"text"`
}

type pollJSON struct {
	Question string           `json:"question"`
	Options  []pollOptionJSON `json:"options"`
	Multiple bool             `json:"multiple"`
	Closed   bool             `json:"closed,omitempty"`
}

type pollRelation struct {
	RelatesTo struct {
		EventID id.EventID `json:"event_id"`
	} `json:"m.relates_to"`
	Response *struct {
		Answers []string `json:"answers"`
	} `json:"org.matrix.msc3381.poll.response"`
}

func rawContent(evt *event.Event, v any) error {
	data := []byte(evt.Content.VeryRaw)
	if len(data) == 0 {
		var err error
		if data, err = json.Marshal(evt.Content.Raw); err != nil {
			return err
		}
	}

	return json.Unmarshal(data, v)
}

func (conn *MxConn) pollStartEvent(evt *event.Event) *OutMsg {
	var content event.PollStartEventContent
	if err := rawContent(evt, &content); err != nil {
		return nil
	}

	start := content.PollStart
	poll := pollJSON{Question: start.Question.GetText(), Multiple: start.MaxSelections > 1}
	for _, answer := range start.Answers {
		poll.Options = append(poll.Options, pollOptionJSON{ID: answer.ID, Text: answer.GetText()})
	}

	if poll.Question == "" || len(poll.Options) == 0 {
		return nil
	}

	data, err := json.Marshal(poll)
	if err != nil {
		return nil
	}

	selfId := conn.session.UserID
	fromMe := evt.Sender == selfId
	conn.mu.Lock()
	isRead := fromMe || conn.isReadLocked(evt.RoomID, evt.Sender, int64(evt.Timestamp))
	conn.mu.Unlock()
	return &OutMsg{
		ChatID:   string(evt.RoomID),
		MsgID:    string(evt.ID),
		SenderID: string(evt.Sender),
		TimeSent: int64(evt.Timestamp),
		FromMe:   fromMe,
		IsRead:   isRead,
		Poll:     string(data),
	}
}

// pollResponseEvent and pollEndEvent: votes and the end of a poll.
func (conn *MxConn) pollRelationEvent(evt *event.Event, closed bool) {
	var content pollRelation
	if err := rawContent(evt, &content); err != nil || content.RelatesTo.EventID == "" {
		return
	}

	answers := []string{}
	if !closed && content.Response != nil {
		answers = content.Response.Answers
	}

	data, _ := json.Marshal(answers)
	CMxNewPollNotify(conn.connId, string(evt.RoomID), string(content.RelatesTo.EventID), string(evt.Sender),
		string(data), BoolToInt(closed))
}

// MxSendPoll sends a poll: {"question", "options": [{"text"}], "multiple"}.
func MxSendPoll(connId int, chatId string, pollJson string) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	var poll pollJSON
	if err := json.Unmarshal([]byte(pollJson), &poll); err != nil {
		return -1
	}

	start := event.PollStart{Kind: "org.matrix.msc3381.poll.disclosed", MaxSelections: 1}
	start.Question.Text = poll.Question
	if poll.Multiple {
		start.MaxSelections = len(poll.Options)
	}

	fallback := []string{poll.Question}
	for i, option := range poll.Options {
		answer := event.PollOption{ID: "option-" + strconv.Itoa(i)}
		answer.Text = option.Text
		start.Answers = append(start.Answers, answer)
		fallback = append(fallback, strconv.Itoa(i+1)+". "+option.Text)
	}

	content := map[string]any{
		"org.matrix.msc3381.poll.start": start,
		// clients without polls show the text
		"org.matrix.msc1767.text": strings.Join(fallback, "\n"),
	}

	ctx, cancel := conn.requestContext()
	defer cancel()
	if _, err := conn.client.SendMessageEvent(ctx, id.RoomID(chatId), event.EventUnstablePollStart, content); err != nil {
		LOG_WARNING(fmt.Sprintf("send poll failed: %v", err))
		return -1
	}

	return 0
}

// MxVotePoll answers a poll (optionIdsJson: ["id"], none retracts the vote).
func MxVotePoll(connId int, chatId string, msgId string, optionIdsJson string) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	answers := []string{}
	if err := json.Unmarshal([]byte(optionIdsJson), &answers); err != nil {
		return -1
	}

	content := event.PollResponseEventContent{
		RelatesTo: event.RelatesTo{Type: event.RelReference, EventID: id.EventID(msgId)},
		Response:  event.PollResponse{Answers: answers},
	}

	ctx, cancel := conn.requestContext()
	defer cancel()
	if _, err := conn.client.SendMessageEvent(ctx, id.RoomID(chatId), event.EventUnstablePollResponse, &content); err != nil {
		LOG_WARNING(fmt.Sprintf("vote failed: %v", err))
		return -1
	}

	return 0
}
