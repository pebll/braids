// gomx_requests.go
//
// Copyright (c) 2026 nchat contributors
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"maunium.net/go/mautrix"
	"maunium.net/go/mautrix/crypto/attachment"
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/format"
	"maunium.net/go/mautrix/id"
)

func (conn *MxConn) requestContext() (context.Context, context.CancelFunc) {
	parent := conn.ctx
	if parent == nil {
		parent = context.Background()
	}

	return context.WithTimeout(parent, 60*time.Second)
}

func MxGetChats(connId int) int {
	conn := GetConn(connId)
	if conn == nil {
		return -1
	}

	type chat struct {
		roomID id.RoomID
		ri     RoomInfo
	}

	var chats []chat
	conn.mu.Lock()
	for roomID, ri := range conn.state.Rooms {
		chats = append(chats, chat{roomID, *ri})
	}
	conn.mu.Unlock()

	for _, c := range chats {
		CMxNewChatsNotify(conn.connId, string(c.roomID), BoolToInt(c.ri.Unread || c.ri.IsInvite),
			BoolToInt(c.ri.Muted), BoolToInt(c.ri.Pinned), c.ri.LastTs)
	}

	return 0
}

func MxGetContacts(connId int) int {
	conn := GetConn(connId)
	if conn == nil {
		return -1
	}

	if conn.session == nil {
		MxGetSelfId(connId)
	}

	if conn.session == nil {
		return -1
	}

	conn.sendAllContacts()
	return 0
}

// mentionsToMarkdown replaces "@Name" occurrences of given mentions with
// markdown mention links, and returns the mentioned user ids.
func mentionsToMarkdown(text string, mentionsJson string) (string, []id.UserID) {
	mentions := make(map[string]string)
	if mentionsJson != "" {
		_ = json.Unmarshal([]byte(mentionsJson), &mentions)
	}

	// longest names first to avoid partial replacement
	names := make([]string, 0, len(mentions))
	for name := range mentions {
		names = append(names, name)
	}
	sort.Slice(names, func(i, j int) bool { return len(names[i]) > len(names[j]) })

	var userIDs []id.UserID
	for _, name := range names {
		userID := id.UserID(mentions[name])
		if !strings.HasPrefix(string(userID), "@") {
			continue
		}

		// ui inserts @[Name With Spaces] (mentions_quoted) or @Name
		link := format.MarkdownMentionWithName(name, userID)
		bracketPattern := "@[" + name + "]"
		atPattern := "@" + name
		if strings.Contains(text, bracketPattern) {
			text = strings.ReplaceAll(text, bracketPattern, link)
		} else if strings.Contains(text, atPattern) {
			text = strings.ReplaceAll(text, atPattern, link)
		} else {
			continue
		}

		userIDs = append(userIDs, userID)
	}

	return text, userIDs
}

func msgTypeForMime(mimeType string) event.MessageType {
	switch {
	case strings.HasPrefix(mimeType, "image/"):
		return event.MsgImage
	case strings.HasPrefix(mimeType, "video/"):
		return event.MsgVideo
	case strings.HasPrefix(mimeType, "audio/"):
		return event.MsgAudio
	default:
		return event.MsgFile
	}
}

func (conn *MxConn) fileContent(ctx context.Context, roomID id.RoomID, filePath string, text string) (*event.MessageEventContent, error) {
	data, err := os.ReadFile(filePath)
	if err != nil {
		return nil, fmt.Errorf("read file: %w", err)
	}

	mimeType := http.DetectContentType(data)
	if idx := strings.Index(mimeType, ";"); idx >= 0 {
		mimeType = mimeType[:idx]
	}

	fileName := filepath.Base(filePath)
	content := &event.MessageEventContent{
		MsgType:  msgTypeForMime(mimeType),
		Body:     fileName,
		FileName: fileName,
		Info: &event.FileInfo{
			MimeType: mimeType,
			Size:     len(data),
		},
	}

	if text != "" {
		content.Body = text
	}

	isEncrypted, _ := conn.client.StateStore.IsEncrypted(ctx, roomID)
	uploadType := mimeType
	var encFile *attachment.EncryptedFile
	if isEncrypted {
		encFile = attachment.NewEncryptedFile()
		encFile.EncryptInPlace(data)
		uploadType = "application/octet-stream"
	}

	resp, err := conn.client.UploadMedia(ctx, mautrix.ReqUploadMedia{
		ContentBytes: data,
		ContentType:  uploadType,
		FileName:     fileName,
	})
	if err != nil {
		return nil, fmt.Errorf("upload: %w", err)
	}

	if encFile != nil {
		content.File = &event.EncryptedFileInfo{
			EncryptedFile: *encFile,
			URL:           resp.ContentURI.CUString(),
		}
	} else {
		content.URL = resp.ContentURI.CUString()
	}

	return content, nil
}

func MxSendMessage(connId int, chatId string, text string, quotedId string, filePath string, fileType string,
	editMsgId string, mentionsJson string) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	ctx, cancel := conn.requestContext()
	defer cancel()
	roomID := id.RoomID(chatId)
	_ = fileType

	var content *event.MessageEventContent
	if filePath != "" {
		var err error
		content, err = conn.fileContent(ctx, roomID, filePath, "")
		if err != nil {
			LOG_WARNING(fmt.Sprintf("send file failed: %v", err))
			return -1
		}
	} else {
		mdText, userIDs := mentionsToMarkdown(text, mentionsJson)
		rendered := format.RenderMarkdown(mdText, true, false)
		content = &rendered
		content.MsgType = event.MsgText
		content.Mentions = &event.Mentions{UserIDs: userIDs}
	}

	if quotedId != "" && editMsgId == "" {
		content.RelatesTo = &event.RelatesTo{InReplyTo: &event.InReplyTo{EventID: id.EventID(quotedId)}}
	}

	if editMsgId != "" {
		content.SetEdit(id.EventID(editMsgId))
	}

	resp, err := conn.client.SendMessageEvent(ctx, roomID, event.EventMessage, content)
	if err != nil {
		LOG_WARNING(fmt.Sprintf("send message failed: %v", err))
		return -1
	}

	if filePath != "" {
		conn.mu.Lock()
		conn.sentFiles[resp.EventID] = filePath
		conn.mu.Unlock()
	}
	LOG_DEBUG(fmt.Sprintf("sent %s", resp.EventID))
	return 0
}

func MxMarkMessageRead(connId int, chatId string, msgId string) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	if strings.HasPrefix(msgId, "invite:") {
		return 0
	}

	ctx, cancel := conn.requestContext()
	defer cancel()
	roomID := id.RoomID(chatId)
	eventID := id.EventID(msgId)
	err := conn.client.SetReadMarkers(ctx, roomID, &mautrix.ReqSetReadMarkers{
		Read:      eventID,
		FullyRead: eventID,
	})
	if err != nil {
		LOG_WARNING(fmt.Sprintf("mark read failed: %v", err))
		return -1
	}

	conn.mu.Lock()
	ri := conn.getRoom(roomID)
	ri.OwnReadTs = max(ri.OwnReadTs, time.Now().UnixMilli())
	ri.Unread = false
	conn.stateDirty = true
	conn.mu.Unlock()
	return 0
}

func MxDeleteMessage(connId int, chatId string, msgId string) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	if strings.HasPrefix(msgId, "invite:") {
		return 0
	}

	ctx, cancel := conn.requestContext()
	defer cancel()
	_, err := conn.client.RedactEvent(ctx, id.RoomID(chatId), id.EventID(msgId))
	if err != nil {
		LOG_WARNING(fmt.Sprintf("redact failed: %v", err))
		return -1
	}

	return 0
}

func MxDeleteChat(connId int, chatId string) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	ctx, cancel := conn.requestContext()
	defer cancel()
	roomID := id.RoomID(chatId)
	if _, err := conn.client.LeaveRoom(ctx, roomID); err != nil {
		LOG_WARNING(fmt.Sprintf("leave failed: %v", err))
	}

	if _, err := conn.client.ForgetRoom(ctx, roomID); err != nil {
		LOG_DEBUG(fmt.Sprintf("forget failed: %v", err))
	}

	conn.mu.Lock()
	delete(conn.state.Rooms, roomID)
	conn.stateDirty = true
	conn.mu.Unlock()
	return 0
}

func MxPinChat(connId int, chatId string, isPinned int) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	ctx, cancel := conn.requestContext()
	defer cancel()
	roomID := id.RoomID(chatId)
	var err error
	if isPinned == 1 {
		err = conn.client.AddTag(ctx, roomID, event.RoomTagFavourite, 0.5)
	} else {
		err = conn.client.RemoveTag(ctx, roomID, event.RoomTagFavourite)
	}

	if err != nil {
		LOG_WARNING(fmt.Sprintf("pin chat failed: %v", err))
		return -1
	}

	conn.mu.Lock()
	ri := conn.getRoom(roomID)
	ri.Pinned = (isPinned == 1)
	lastTs := ri.LastTs
	conn.stateDirty = true
	conn.mu.Unlock()

	CMxUpdatePinNotify(conn.connId, chatId, isPinned, lastTs)
	return 0
}

func MxSendTyping(connId int, chatId string, isTyping int) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	ctx, cancel := conn.requestContext()
	defer cancel()
	_, err := conn.client.UserTyping(ctx, id.RoomID(chatId), isTyping == 1, 20*time.Second)
	if err != nil {
		LOG_DEBUG(fmt.Sprintf("typing failed: %v", err))
		return -1
	}

	return 0
}

func MxSendStatus(connId int, isOnline int) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	ctx, cancel := conn.requestContext()
	defer cancel()
	presence := event.PresenceUnavailable
	if isOnline == 1 {
		presence = event.PresenceOnline
	}

	if err := conn.client.SetPresence(ctx, mautrix.ReqPresence{Presence: presence}); err != nil {
		LOG_DEBUG(fmt.Sprintf("set presence failed: %v", err))
		return -1
	}

	return 0
}

func MxGetStatus(connId int, userId string) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	// chat ids are room ids; for direct chats query the other user
	userID := id.UserID(userId)
	if strings.HasPrefix(userId, "!") {
		conn.mu.Lock()
		directUser, ok := conn.directs[id.RoomID(userId)]
		ri := conn.getRoom(id.RoomID(userId))
		if !ok && ri.IsDirect && len(ri.Heroes) > 0 {
			directUser, ok = ri.Heroes[0], true
		}
		conn.mu.Unlock()
		if !ok {
			return 0
		}

		userID = directUser
	}

	go func() {
		ctx, cancel := conn.requestContext()
		defer cancel()
		resp, err := conn.client.GetPresence(ctx, userID)
		if err != nil {
			LOG_DEBUG(fmt.Sprintf("get presence failed: %v", err))
			return
		}

		isOnline := (resp.Presence == event.PresenceOnline)
		timeSeen := int64(-1)
		if !isOnline && resp.LastActiveAgo > 0 {
			timeSeen = time.Now().UnixMilli() - int64(resp.LastActiveAgo)
		}

		CMxNewStatusNotify(conn.connId, userId, BoolToInt(isOnline), timeSeen)
	}()

	return 0
}

func MxSendReaction(connId int, chatId string, msgId string, emoji string) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	ctx, cancel := conn.requestContext()
	defer cancel()
	roomID := id.RoomID(chatId)
	target := id.EventID(msgId)
	selfId := conn.session.UserID

	// nchat supports one reaction per user and message: remove previous ones
	var previous []id.EventID
	conn.mu.Lock()
	for evtID, ref := range conn.state.Reactions {
		if ref.Target == target && ref.Sender == selfId && ref.RoomID == roomID {
			if ref.Key == emoji {
				conn.mu.Unlock()
				return 0
			}

			previous = append(previous, evtID)
		}
	}
	conn.mu.Unlock()

	for _, evtID := range previous {
		if _, err := conn.client.RedactEvent(ctx, roomID, evtID); err != nil {
			LOG_WARNING(fmt.Sprintf("redact reaction failed: %v", err))
		}
	}

	if emoji == "" {
		return 0
	}

	if _, err := conn.client.SendReaction(ctx, roomID, target, emoji); err != nil {
		LOG_WARNING(fmt.Sprintf("send reaction failed: %v", err))
		return -1
	}

	return 0
}

func MxDownloadFile(connId int, chatId string, msgId string, fileId string, action int) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	go func() {
		filePath, fileStatus := conn.downloadFile(fileId)
		CMxNewMessageFileNotify(conn.connId, chatId, msgId, fileId, filePath, fileStatus, action)
	}()

	return 0
}

func (conn *MxConn) downloadFile(fileId string) (string, int) {
	var info MxFileInfo
	if err := json.Unmarshal([]byte(fileId), &info); err != nil || info.Version != fileInfoVersion {
		LOG_WARNING("invalid file id")
		return "", FileStatusDownloadFailed
	}

	if _, err := os.Stat(info.TargetPath); err == nil {
		return info.TargetPath, FileStatusDownloaded
	}

	urlStr := info.URL
	if info.File != nil {
		urlStr = info.File.URL
	}

	uri, err := urlStr.Parse()
	if err != nil {
		LOG_WARNING(fmt.Sprintf("invalid content uri: %v", err))
		return "", FileStatusDownloadFailed
	}

	CMxSetStatus(conn.connId, FlagFetching)
	defer CMxClearStatus(conn.connId, FlagFetching)

	ctx, cancel := context.WithTimeout(conn.ctx, 5*time.Minute)
	defer cancel()
	data, err := conn.client.DownloadBytes(ctx, uri)
	if err != nil {
		LOG_WARNING(fmt.Sprintf("download failed: %v", err))
		return "", FileStatusDownloadFailed
	}

	if info.File != nil {
		if err := info.File.DecryptInPlace(data); err != nil {
			LOG_WARNING(fmt.Sprintf("attachment decrypt failed: %v", err))
			return "", FileStatusDownloadFailed
		}
	}

	_ = os.MkdirAll(filepath.Dir(info.TargetPath), 0700)
	if err := os.WriteFile(info.TargetPath, data, 0600); err != nil {
		LOG_WARNING(fmt.Sprintf("write failed: %v", err))
		return "", FileStatusDownloadFailed
	}

	return info.TargetPath, FileStatusDownloaded
}

func jsonEscape(s string) string {
	b, _ := json.Marshal(s)
	return string(b[1 : len(b)-1])
}

func MxGetGroupMembers(connId int, chatId string) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	go func() {
		ctx, cancel := conn.requestContext()
		defer cancel()
		roomID := id.RoomID(chatId)
		resp, err := conn.client.JoinedMembers(ctx, roomID)
		if err != nil {
			LOG_WARNING(fmt.Sprintf("get members failed: %v", err))
			return
		}

		var parts []string
		for userID, member := range resp.Joined {
			name := member.DisplayName
			if name == "" {
				name = userID.Localpart()
			}

			parts = append(parts, fmt.Sprintf("{\"id\":\"%s\",\"name\":\"%s\"}", jsonEscape(string(userID)), jsonEscape(name)))
			conn.updateContact(string(userID), name)
		}

		sort.Strings(parts)
		CMxNewGroupMembersNotify(conn.connId, chatId, "["+strings.Join(parts, ",")+"]")
	}()

	return 0
}

// MxCreateChat opens a chat with a user: an existing direct room, or a newly
// created encrypted direct room. Room ids are passed through.
func MxCreateChat(connId int, userId string) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return -1
	}

	if strings.HasPrefix(userId, "!") {
		CMxCreateChatNotify(conn.connId, userId, 1)
		return 0
	}

	userID := id.UserID(userId)
	conn.mu.Lock()
	for roomID, directUser := range conn.directs {
		if directUser == userID {
			if _, joined := conn.state.Rooms[roomID]; joined {
				conn.mu.Unlock()
				CMxCreateChatNotify(conn.connId, string(roomID), 1)
				return 0
			}
		}
	}
	conn.mu.Unlock()

	// chat with self (nchat "Saved Messages"): a private room without invitees
	isSelf := (userID == conn.session.UserID)

	go func() {
		ctx, cancel := conn.requestContext()
		defer cancel()
		req := &mautrix.ReqCreateRoom{
			Preset:   "trusted_private_chat",
			IsDirect: !isSelf,
			InitialState: []*event.Event{{
				Type:    event.StateEncryption,
				Content: event.Content{Parsed: &event.EncryptionEventContent{Algorithm: id.AlgorithmMegolmV1}},
			}},
		}
		if isSelf {
			req.Name = "Saved Messages"
		} else {
			req.Invite = []id.UserID{userID}
		}

		resp, err := conn.client.CreateRoom(ctx, req)
		if err != nil {
			LOG_WARNING(fmt.Sprintf("create room failed: %v", err))
			CMxCreateChatNotify(conn.connId, "", 0)
			return
		}

		if isSelf {
			conn.updateContact(string(resp.RoomID), req.Name)
			CMxCreateChatNotify(conn.connId, string(resp.RoomID), 1)
			return
		}

		// record as direct chat in account data
		var directs event.DirectChatsEventContent
		_ = conn.client.GetAccountData(ctx, event.AccountDataDirectChats.Type, &directs)
		if directs == nil {
			directs = make(event.DirectChatsEventContent)
		}
		directs[userID] = append(directs[userID], resp.RoomID)
		_ = conn.client.SetAccountData(ctx, event.AccountDataDirectChats.Type, &directs)

		conn.mu.Lock()
		conn.directs[resp.RoomID] = userID
		ri := conn.getRoom(resp.RoomID)
		ri.IsDirect = true
		ri.Heroes = []id.UserID{userID}
		ri.LastTs = time.Now().UnixMilli()
		conn.stateDirty = true
		conn.mu.Unlock()

		conn.updateContact(string(resp.RoomID), conn.roomName(ctx, resp.RoomID))
		CMxCreateChatNotify(conn.connId, string(resp.RoomID), 1)
	}()

	return 0
}
