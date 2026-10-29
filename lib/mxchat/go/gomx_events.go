// gomx_events.go
//
// Copyright (c) 2026 nchat contributors
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"mime"
	"path/filepath"
	"regexp"
	"strings"

	"maunium.net/go/mautrix"
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/format"
	"maunium.net/go/mautrix/id"
)

const (
	modeSync     = 0 // event from /sync
	modeBackfill = 1 // event from /messages
	modeLate     = 2 // event decrypted after keys arrived
)

const utdText = "🔒 waiting for keys"

type OutMsg struct {
	ChatID     string
	MsgID      string
	SenderID   string
	Text       string
	QuotedID   string
	FileID     string
	FilePath   string
	FileStatus int
	TimeSent   int64
	FromMe     bool
	IsRead     bool
	IsEdited   bool
}

type queuedReaction struct {
	roomID id.RoomID
	target id.EventID
	sender id.UserID
	key    string
}

// fileInfoVersion is stored in fileId, bump upon struct change
var fileInfoVersion = 1

// MxFileInfo is serialized into the nchat fileId of attachments. For encrypted
// attachments it includes the file key (as wmchat does for media keys), so it
// is only stored in the local message cache.
type MxFileInfo struct {
	Version    int                      `json:"v"`
	URL        id.ContentURIString      `json:"url,omitempty"`
	File       *event.EncryptedFileInfo `json:"file,omitempty"`
	Name       string                   `json:"name,omitempty"`
	Mime       string                   `json:"mime,omitempty"`
	TargetPath string                   `json:"path"`
}

var unsafeFileChars = regexp.MustCompile(`[^A-Za-z0-9._-]`)

// htmlParser converts Matrix HTML to nchat (markdown-ish) text, rendering
// user pills as @displayname.
var htmlParser = &format.HTMLParser{
	TabsToSpaces:   4,
	Newline:        "\n",
	HorizontalLine: "\n---\n",
	PillConverter: func(displayname, mxid, eventID string, ctx format.Context) string {
		if len(mxid) > 0 && mxid[0] == '@' {
			name := strings.TrimPrefix(displayname, "@")
			if name == "" {
				name = id.UserID(mxid).Localpart()
			}

			if strings.Contains(name, " ") {
				// same format as nchat mention insertion (mentions_quoted)
				return "@[" + name + "]"
			}

			return "@" + name
		}

		return format.DefaultPillConverter(displayname, mxid, eventID, ctx)
	},
	BoldConverter:          func(s string, _ format.Context) string { return "**" + s + "**" },
	ItalicConverter:        func(s string, _ format.Context) string { return "_" + s + "_" },
	StrikethroughConverter: func(s string, _ format.Context) string { return "~~" + s + "~~" },
	MonospaceConverter:     func(s string, _ format.Context) string { return "`" + s + "`" },
	LinkConverter: func(text, href string, _ format.Context) string {
		if text == href || text == "" {
			return href
		}

		return fmt.Sprintf("%s (%s)", text, href)
	},
}

// isRead determines read state of a message at time ts. Caller holds conn.mu.
func (conn *MxConn) isReadLocked(roomID id.RoomID, sender id.UserID, ts int64) bool {
	ri := conn.getRoom(roomID)
	if sender == conn.session.UserID {
		return ts <= ri.OtherReadTs
	}

	return ts <= ri.OwnReadTs
}

// handleTimelineEvent converts a timeline event. Message-like events are
// returned for batching; reactions are queued and redactions emitted directly.
func (conn *MxConn) handleTimelineEvent(ctx context.Context, evt *event.Event, mode int) *OutMsg {
	if evt.Unsigned.RedactedBecause != nil {
		return nil
	}

	switch evt.Type {
	case event.EventEncrypted:
		dec := conn.resolveEncrypted(ctx, evt)
		if dec == nil {
			if mode == modeLate {
				return nil
			}

			conn.addPending(ctx, evt)
			return conn.utdMessage(evt)
		}

		conn.mu.Lock()
		delete(conn.state.Pending, evt.ID)
		conn.mu.Unlock()
		return conn.handleTimelineEvent(ctx, dec, mode)

	case event.EventMessage, event.EventSticker:
		return conn.messageEvent(ctx, evt, mode)

	case event.EventReaction:
		conn.reactionEvent(evt)
		return nil

	case event.EventRedaction:
		if mode != modeBackfill {
			conn.redactionEvent(evt)
		}

		return nil
	}

	return nil
}

func (conn *MxConn) utdMessage(evt *event.Event) *OutMsg {
	conn.mu.Lock()
	conn.utdShown[evt.ID] = true
	isRead := conn.isReadLocked(evt.RoomID, evt.Sender, int64(evt.Timestamp))
	conn.mu.Unlock()

	return &OutMsg{
		ChatID:   string(evt.RoomID),
		MsgID:    string(evt.ID),
		SenderID: string(evt.Sender),
		Text:     utdText,
		TimeSent: int64(evt.Timestamp),
		FromMe:   (evt.Sender == conn.session.UserID),
		IsRead:   isRead,
	}
}

// renderContent converts message content to display text.
func (conn *MxConn) renderContent(ctx context.Context, roomID id.RoomID, sender id.UserID,
	content *event.MessageEventContent) string {
	if content.RelatesTo.GetReplyTo() != "" {
		content.RemoveReplyFallback()
		if strings.HasPrefix(content.Body, "> <") {
			content.Body = event.TrimReplyFallbackText(content.Body)
		}
	}

	text := content.Body
	if content.Format == event.FormatHTML && content.FormattedBody != "" {
		parsed, _ := format.HTMLToMarkdownFull(htmlParser, content.FormattedBody)
		if strings.TrimSpace(parsed) != "" {
			text = parsed
		}
	}

	switch content.MsgType {
	case event.MsgEmote:
		text = "* " + conn.userName(ctx, roomID, sender) + " " + text

	case event.MsgImage, event.MsgVideo, event.MsgAudio, event.MsgFile:
		// body is a caption only if filename is set and differs
		if content.FileName != "" && content.FileName != content.Body {
			text = content.Body
		} else {
			text = ""
		}

	case event.MsgLocation:
		text = content.Body
		if content.GeoURI != "" {
			text += " (" + content.GeoURI + ")"
		}
	}

	return text
}

func (conn *MxConn) fileInfoFor(evt *event.Event, content *event.MessageEventContent) (string, string, int) {
	if content.URL == "" && content.File == nil {
		return "", "", FileStatusNone
	}

	name := content.FileName
	if name == "" {
		name = content.Body
	}

	mimeType := ""
	if content.Info != nil {
		mimeType = content.Info.MimeType
	}

	ext := filepath.Ext(name)
	if ext == "" && mimeType != "" {
		if exts, err := mime.ExtensionsByType(mimeType); err == nil && len(exts) > 0 {
			ext = exts[0]
		}
	}

	base := strings.TrimSuffix(filepath.Base(name), filepath.Ext(name))
	base = unsafeFileChars.ReplaceAllString(base, "_")
	if len(base) > 64 {
		base = base[:64]
	}

	evtPart := unsafeFileChars.ReplaceAllString(strings.TrimPrefix(string(evt.ID), "$"), "_")
	if len(evtPart) > 16 {
		evtPart = evtPart[:16]
	}

	ext = strings.TrimPrefix(unsafeFileChars.ReplaceAllString(ext, ""), ".")
	// per-event dir keeps the original file name (shown in ui) and avoids collisions
	if base == "" {
		base = "file"
	}

	targetPath := fmt.Sprintf("%s/%s/%s", tmpPath(conn.path), evtPart, base)
	if ext != "" {
		targetPath += "." + ext
	}

	info := MxFileInfo{
		Version:    fileInfoVersion,
		URL:        content.URL,
		File:       content.File,
		Name:       name,
		Mime:       mimeType,
		TargetPath: targetPath,
	}

	data, err := json.Marshal(info)
	if err != nil {
		return "", "", FileStatusNone
	}

	return string(data), targetPath, FileStatusNotDownloaded
}

func (conn *MxConn) messageEvent(ctx context.Context, evt *event.Event, mode int) *OutMsg {
	content, ok := evt.Content.Parsed.(*event.MessageEventContent)
	if !ok {
		if evt.Content.Parsed == nil {
			if err := evt.Content.ParseRaw(evt.Type); err != nil && !errors.Is(err, event.ErrContentAlreadyParsed) {
				return nil
			}
			content, ok = evt.Content.Parsed.(*event.MessageEventContent)
		}

		if !ok {
			return nil
		}
	}

	if evt.Type == event.EventSticker && content.MsgType == "" {
		content.MsgType = event.MsgImage
	}

	selfId := conn.session.UserID
	fromMe := (evt.Sender == selfId)

	// edit
	if replaceID := content.RelatesTo.GetReplaceID(); replaceID != "" {
		if mode == modeBackfill {
			// edits are applied via the bundled latest edit of the original
			return nil
		}

		newContent := content.NewContent
		if newContent == nil {
			newContent = content
			newContent.Body = strings.TrimPrefix(newContent.Body, "* ")
		}

		conn.mu.Lock()
		isRead := conn.isReadLocked(evt.RoomID, evt.Sender, int64(evt.Timestamp))
		conn.mu.Unlock()

		return &OutMsg{
			ChatID:   string(evt.RoomID),
			MsgID:    string(replaceID),
			SenderID: string(evt.Sender),
			Text:     conn.renderContent(ctx, evt.RoomID, evt.Sender, newContent),
			TimeSent: int64(evt.Timestamp),
			FromMe:   fromMe,
			IsRead:   isRead,
			IsEdited: true,
		}
	}

	isEdited := false
	if latest := evt.Unsigned.Relations.GetLatestEdit(); latest != nil && latest.Sender == evt.Sender {
		latest.RoomID = evt.RoomID
		editEvt := latest
		if latest.Type == event.EventEncrypted {
			_ = latest.Content.ParseRaw(latest.Type)
			editEvt = conn.resolveEncrypted(ctx, latest)
		} else {
			_ = latest.Content.ParseRaw(latest.Type)
		}

		if editEvt != nil {
			if editContent, ok := editEvt.Content.Parsed.(*event.MessageEventContent); ok && editContent.NewContent != nil {
				relatesTo := content.RelatesTo
				content = editContent.NewContent
				if content.RelatesTo == nil {
					content.RelatesTo = relatesTo
				}
				isEdited = true
			}
		}
	}

	fileId, filePath, fileStatus := conn.fileInfoFor(evt, content)
	if fromMe && fileId != "" {
		// own attachment sent from this device: refer to the local file
		conn.mu.Lock()
		localPath, ok := conn.sentFiles[evt.ID]
		conn.mu.Unlock()
		if ok {
			filePath = localPath
			fileStatus = FileStatusDownloaded
		}
	}
	quotedId := string(content.RelatesTo.GetReplyTo())
	text := conn.renderContent(ctx, evt.RoomID, evt.Sender, content)

	conn.mu.Lock()
	isRead := fromMe || conn.isReadLocked(evt.RoomID, evt.Sender, int64(evt.Timestamp))
	if fromMe {
		ri := conn.getRoom(evt.RoomID)
		isRead = int64(evt.Timestamp) <= ri.OtherReadTs
		if !isRead && mode != modeBackfill {
			own := append(conn.ownMsgs[evt.RoomID], OwnMsg{evt.ID, int64(evt.Timestamp)})
			if len(own) > 100 {
				own = own[len(own)-100:]
			}
			conn.ownMsgs[evt.RoomID] = own
		}
	}
	conn.mu.Unlock()

	return &OutMsg{
		ChatID:     string(evt.RoomID),
		MsgID:      string(evt.ID),
		SenderID:   string(evt.Sender),
		Text:       text,
		QuotedID:   quotedId,
		FileID:     fileId,
		FilePath:   filePath,
		FileStatus: fileStatus,
		TimeSent:   int64(evt.Timestamp),
		FromMe:     fromMe,
		IsRead:     isRead,
		IsEdited:   isEdited,
	}
}

func (conn *MxConn) reactionEvent(evt *event.Event) {
	content, ok := evt.Content.Parsed.(*event.ReactionEventContent)
	if !ok {
		return
	}

	target := content.RelatesTo.GetAnnotationID()
	key := content.RelatesTo.GetAnnotationKey()
	if target == "" || key == "" {
		return
	}

	conn.mu.Lock()
	conn.state.Reactions[evt.ID] = &ReactionRef{RoomID: evt.RoomID, Target: target, Sender: evt.Sender, Key: key}
	conn.stateDirty = true
	conn.reactionQueue = append(conn.reactionQueue, queuedReaction{evt.RoomID, target, evt.Sender, key})
	conn.mu.Unlock()
}

// flushReactions emits queued reactions (after the messages they target).
func (conn *MxConn) flushReactions() {
	conn.mu.Lock()
	queue := conn.reactionQueue
	conn.reactionQueue = nil
	conn.mu.Unlock()

	selfId := conn.session.UserID
	for _, r := range queue {
		CMxNewMessageReactionNotify(conn.connId, string(r.roomID), string(r.target), string(r.sender), r.key,
			BoolToInt(r.sender == selfId))
	}
}

func (conn *MxConn) redactionEvent(evt *event.Event) {
	redacts := evt.Redacts
	if redacts == "" {
		if content, ok := evt.Content.Parsed.(*event.RedactionEventContent); ok {
			redacts = content.Redacts
		}
	}

	if redacts == "" {
		return
	}

	conn.mu.Lock()
	ref, isReaction := conn.state.Reactions[redacts]
	if isReaction {
		delete(conn.state.Reactions, redacts)
		conn.stateDirty = true
	}
	conn.mu.Unlock()

	selfId := conn.session.UserID
	if isReaction {
		CMxNewMessageReactionNotify(conn.connId, string(ref.RoomID), string(ref.Target), string(ref.Sender), "",
			BoolToInt(ref.Sender == selfId))
		return
	}

	CMxDeleteMessageNotify(conn.connId, string(evt.RoomID), string(redacts), BoolToInt(evt.Sender == selfId))
}

// emitBatch sends messages as one in-sequence notify. Empty batches are still
// completed so the UI receives a (possibly empty) response.
func (conn *MxConn) emitBatch(roomID id.RoomID, msgs []*OutMsg, fromMsgId string) {
	for _, m := range msgs {
		conn.emitMessage(m, 1)
	}

	CMxNewMessagesBatchDone(conn.connId, string(roomID), fromMsgId)
}

func (conn *MxConn) emitMessage(m *OutMsg, batch int) {
	CMxNewMessagesNotify(conn.connId, m.ChatID, m.MsgID, m.SenderID, m.Text, BoolToInt(m.FromMe), m.QuotedID,
		m.FileID, m.FilePath, m.FileStatus, m.TimeSent, BoolToInt(m.IsRead), BoolToInt(m.IsEdited), batch)
}

// MxGetMessages backfills history older than fromMsgId (or latest if empty)
// via /context and /messages.
func MxGetMessages(connId int, chatId string, fromMsgId string, limit int) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	go conn.backfill(id.RoomID(chatId), id.EventID(fromMsgId), limit)
	return 0
}

func (conn *MxConn) backfill(roomID id.RoomID, fromMsgId id.EventID, limit int) {
	ctx := conn.ctx
	client := conn.client
	if strings.HasPrefix(string(fromMsgId), "invite:") {
		CMxNewMessagesBatchDone(conn.connId, string(roomID), string(fromMsgId))
		return
	}

	conn.mu.Lock()
	ri := conn.getRoom(roomID)
	isInvite := ri.IsInvite
	conn.mu.Unlock()
	if isInvite {
		CMxNewMessagesBatchDone(conn.connId, string(roomID), string(fromMsgId))
		return
	}

	if limit <= 0 || limit > 100 {
		limit = 50
	}

	CMxSetStatus(conn.connId, FlagFetching)
	defer CMxClearStatus(conn.connId, FlagFetching)

	token := ""
	if fromMsgId != "" {
		resp, err := client.Context(ctx, roomID, fromMsgId, nil, 0)
		if err != nil {
			LOG_WARNING(fmt.Sprintf("context failed %s: %v", roomID, err))
			CMxNewMessagesBatchDone(conn.connId, string(roomID), string(fromMsgId))
			return
		}

		token = resp.Start
	}

	resp, err := client.Messages(ctx, roomID, token, "", mautrix.DirectionBackward, nil, limit)
	if err != nil {
		LOG_WARNING(fmt.Sprintf("messages failed %s: %v", roomID, err))
		CMxNewMessagesBatchDone(conn.connId, string(roomID), string(fromMsgId))
		return
	}

	var msgs []*OutMsg
	for _, evt := range resp.Chunk {
		evt.RoomID = roomID
		if err := evt.Content.ParseRaw(evt.Type); err != nil && !errors.Is(err, event.ErrContentAlreadyParsed) &&
			!errors.Is(err, event.ErrUnsupportedContentType) {
			continue
		}

		if evt.StateKey != nil {
			continue
		}

		if evt.ID == fromMsgId {
			continue
		}

		msg := conn.handleTimelineEvent(ctx, evt, modeBackfill)
		if msg != nil {
			msgs = append(msgs, msg)
		}
	}

	for _, msg := range msgs {
		if msg.SenderID != "" {
			conn.updateContact(msg.SenderID, conn.userName(ctx, roomID, id.UserID(msg.SenderID)))
		}
	}

	LOG_DEBUG(fmt.Sprintf("backfill %s got %d events %d msgs", roomID, len(resp.Chunk), len(msgs)))
	conn.emitBatch(roomID, msgs, string(fromMsgId))
	conn.flushReactions()
	conn.saveState(false)
}
