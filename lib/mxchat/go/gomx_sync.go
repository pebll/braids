// gomx_sync.go
//
// Copyright (c) 2026 nchat contributors
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

package main

import (
	"context"
	"fmt"
	"sort"
	"strings"
	"time"

	"maunium.net/go/mautrix"
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/id"
	"maunium.net/go/mautrix/pushrules"
)

const invitePrefix = "[Invite] "

// handleSync processes one /sync response after mautrix (crypto, state store)
// has handled it.
func (conn *MxConn) handleSync(ctx context.Context, res *mautrix.RespSync, since string) {
	initial := (since == "")
	selfId := conn.session.UserID

	// global account data
	for _, evt := range res.AccountData.Events {
		switch evt.Type.Type {
		case event.AccountDataDirectChats.Type:
			conn.handleDirectChats(evt)

		case event.AccountDataPushRules.Type:
			rules, err := pushrules.EventToPushRules(evt)
			if err == nil {
				conn.mu.Lock()
				conn.pushRules = rules
				conn.mu.Unlock()
				conn.updateAllMuted(!initial)
			}
		}
	}

	// presence
	for _, evt := range res.Presence.Events {
		content, ok := evt.Content.Parsed.(*event.PresenceEventContent)
		if !ok || evt.Sender == selfId {
			continue
		}

		isOnline := (content.Presence == event.PresenceOnline)
		timeSeen := int64(-1)
		if !isOnline && content.LastActiveAgo > 0 {
			timeSeen = int64(evt.Timestamp) - content.LastActiveAgo
			if evt.Timestamp == 0 {
				timeSeen = -1
			}
		}

		CMxNewStatusNotify(conn.connId, string(evt.Sender), BoolToInt(isOnline), timeSeen)
	}

	// joined rooms
	for roomID, room := range res.Rooms.Join {
		conn.handleJoinedRoom(ctx, roomID, room, initial)
	}

	// invited rooms (shown as chats, not auto-accepted)
	for roomID, room := range res.Rooms.Invite {
		conn.handleInvitedRoom(roomID, room)
	}

	// left rooms
	for roomID := range res.Rooms.Leave {
		conn.mu.Lock()
		_, known := conn.state.Rooms[roomID]
		delete(conn.state.Rooms, roomID)
		delete(conn.directs, roomID)
		conn.stateDirty = true
		conn.mu.Unlock()
		if known {
			LOG_DEBUG(fmt.Sprintf("left room %s", roomID))
			CMxDeleteChatNotify(conn.connId, string(roomID))
		}
	}

	if !conn.firstSync {
		conn.firstSync = true
		CMxClearStatus(conn.connId, FlagSyncing)
		conn.sendAllContacts()
		go conn.maybePromptRecovery()
	}

	conn.saveState(false)
}

func (conn *MxConn) handleDirectChats(evt *event.Event) {
	content, ok := evt.Content.Parsed.(*event.DirectChatsEventContent)
	if !ok {
		return
	}

	conn.mu.Lock()
	conn.directs = make(map[id.RoomID]id.UserID)
	for userID, rooms := range *content {
		for _, roomID := range rooms {
			conn.directs[roomID] = userID
		}
	}

	for roomID, ri := range conn.state.Rooms {
		_, isDirect := conn.directs[roomID]
		if isDirect && !ri.IsDirect {
			ri.IsDirect = true
			conn.stateDirty = true
		}
	}
	conn.mu.Unlock()
}

// isRoomMuted evaluates push rules for room-specific mute. Caller holds conn.mu.
func (conn *MxConn) isRoomMuted(roomID id.RoomID) bool {
	if conn.pushRules == nil {
		return false
	}

	for _, rule := range conn.pushRules.Override {
		if rule.RuleID == string(roomID) && rule.Enabled && !rule.Actions.Should().Notify {
			return true
		}
	}

	if rule, ok := conn.pushRules.Room.Map[string(roomID)]; ok && rule.Enabled && !rule.Actions.Should().Notify {
		return true
	}

	return false
}

func (conn *MxConn) updateAllMuted(notify bool) {
	type change struct {
		roomID id.RoomID
		muted  bool
	}

	var changes []change
	conn.mu.Lock()
	for roomID, ri := range conn.state.Rooms {
		muted := conn.isRoomMuted(roomID)
		if muted != ri.Muted {
			ri.Muted = muted
			conn.stateDirty = true
			changes = append(changes, change{roomID, muted})
		}
	}
	conn.mu.Unlock()

	if notify {
		for _, c := range changes {
			CMxUpdateMuteNotify(conn.connId, string(c.roomID), BoolToInt(c.muted))
		}
	}
}

// userName returns the best known display name for a user. Caller must not
// hold conn.mu.
func (conn *MxConn) userName(ctx context.Context, roomID id.RoomID, userID id.UserID) string {
	if conn.client != nil && conn.client.StateStore != nil && roomID != "" {
		member, err := conn.client.StateStore.TryGetMember(ctx, roomID, userID)
		if err == nil && member != nil && member.Displayname != "" {
			return member.Displayname
		}
	}

	conn.mu.Lock()
	name, ok := conn.state.Names[userID]
	conn.mu.Unlock()
	if ok && name != "" {
		return name
	}

	return userID.Localpart()
}

// roomName computes the display name of a room per the spec algorithm.
// Caller must not hold conn.mu.
func (conn *MxConn) roomName(ctx context.Context, roomID id.RoomID) string {
	conn.mu.Lock()
	ri := conn.getRoom(roomID)
	name := ri.Name
	alias := ri.Alias
	heroes := append([]id.UserID(nil), ri.Heroes...)
	joined := ri.Joined
	invited := ri.Invited
	isInvite := ri.IsInvite
	inviter := ri.Inviter
	directUser, isDirect := conn.directs[roomID]
	conn.mu.Unlock()

	result := ""
	switch {
	case name != "":
		result = name
	case alias != "":
		result = alias
	case isDirect && directUser != "":
		result = conn.userName(ctx, roomID, directUser)
	default:
		selfId := conn.session.UserID
		var names []string
		for _, hero := range heroes {
			if hero != selfId {
				names = append(names, conn.userName(ctx, roomID, hero))
			}
		}

		if len(heroes) == 0 && conn.client != nil && conn.client.StateStore != nil {
			members, err := conn.client.StateStore.GetRoomJoinedOrInvitedMembers(ctx, roomID)
			if err == nil {
				sort.Slice(members, func(i, j int) bool { return members[i] < members[j] })
				for _, member := range members {
					if member != selfId && len(names) < 5 {
						names = append(names, conn.userName(ctx, roomID, member))
					}
				}
			}
		}

		others := joined + invited - 1 - len(names)
		switch {
		case len(names) == 0 && isInvite && inviter != "":
			result = conn.userName(ctx, roomID, inviter)
		case len(names) == 0:
			result = "Empty room"
		case others > 0:
			result = strings.Join(names, ", ") + fmt.Sprintf(" and %d others", others)
		default:
			result = strings.Join(names, ", ")
		}
	}

	if isInvite {
		result = invitePrefix + result
	}

	return result
}

// updateContact emits a contact notify if the name changed.
func (conn *MxConn) updateContact(contactId string, name string) {
	conn.mu.Lock()
	prev, ok := conn.contacts[contactId]
	conn.contacts[contactId] = name
	conn.mu.Unlock()

	if !ok || prev != name {
		isSelf := (contactId == string(conn.session.UserID))
		CMxNewContactsNotify(conn.connId, contactId, name, BoolToInt(isSelf), NotifyDirect)
	}
}

// sendAllContacts emits the complete contact list (rooms and users).
func (conn *MxConn) sendAllContacts() {
	ctx := context.Background()
	conn.mu.Lock()
	roomIDs := make([]id.RoomID, 0, len(conn.state.Rooms))
	for roomID := range conn.state.Rooms {
		roomIDs = append(roomIDs, roomID)
	}

	users := make(map[id.UserID]string)
	for userID, name := range conn.state.Names {
		users[userID] = name
	}
	conn.mu.Unlock()

	type contact struct {
		id   string
		name string
	}

	var list []contact
	for _, roomID := range roomIDs {
		list = append(list, contact{string(roomID), conn.roomName(ctx, roomID)})
	}

	selfId := conn.session.UserID
	if _, ok := users[selfId]; !ok {
		users[selfId] = selfId.Localpart()
	}

	for userID, name := range users {
		if name == "" {
			name = userID.Localpart()
		}

		list = append(list, contact{string(userID), name})
	}

	conn.mu.Lock()
	for _, c := range list {
		conn.contacts[c.id] = c.name
	}
	conn.mu.Unlock()

	for i, c := range list {
		notify := NotifyCache
		if i == len(list)-1 {
			notify = NotifySendCached
		}

		isSelf := (c.id == string(selfId))
		CMxNewContactsNotify(conn.connId, c.id, c.name, BoolToInt(isSelf), notify)
	}
}

func (conn *MxConn) applyStateEvent(ctx context.Context, roomID id.RoomID, evt *event.Event) {
	if evt.StateKey == nil {
		return
	}

	conn.mu.Lock()
	defer conn.mu.Unlock()
	ri := conn.getRoom(roomID)

	switch evt.Type {
	case event.StateRoomName:
		if content, ok := evt.Content.Parsed.(*event.RoomNameEventContent); ok {
			ri.Name = content.Name
			conn.stateDirty = true
		}

	case event.StateCanonicalAlias:
		if content, ok := evt.Content.Parsed.(*event.CanonicalAliasEventContent); ok {
			ri.Alias = string(content.Alias)
			conn.stateDirty = true
		}

	case event.StateMember:
		if content, ok := evt.Content.Parsed.(*event.MemberEventContent); ok {
			userID := id.UserID(*evt.StateKey)
			if content.Displayname != "" && content.Membership == event.MembershipJoin {
				if conn.state.Names[userID] != content.Displayname {
					conn.state.Names[userID] = content.Displayname
					conn.stateDirty = true
				}
			} else if _, ok := conn.state.Names[userID]; !ok {
				conn.state.Names[userID] = ""
				conn.stateDirty = true
			}
		}
	}
}

func (conn *MxConn) handleJoinedRoom(ctx context.Context, roomID id.RoomID, room *mautrix.SyncJoinedRoom,
	initial bool) {
	conn.mu.Lock()
	ri := conn.getRoom(roomID)
	wasInvite := ri.IsInvite
	ri.IsInvite = false
	ri.Inviter = ""
	prevPinned := ri.Pinned
	conn.stateDirty = true
	conn.mu.Unlock()

	// state
	stateEvents := room.State.Events
	if room.StateAfter != nil {
		stateEvents = room.StateAfter.Events
	}

	for _, evt := range stateEvents {
		conn.applyStateEvent(ctx, roomID, evt)
	}

	for _, evt := range room.Timeline.Events {
		if evt.StateKey != nil {
			conn.applyStateEvent(ctx, roomID, evt)
		}
	}

	// summary and counts
	conn.mu.Lock()
	if len(room.Summary.Heroes) > 0 {
		ri.Heroes = room.Summary.Heroes
	}
	if room.Summary.JoinedMemberCount != nil {
		ri.Joined = *room.Summary.JoinedMemberCount
	}
	if room.Summary.InvitedMemberCount != nil {
		ri.Invited = *room.Summary.InvitedMemberCount
	}

	if room.UnreadNotifications != nil {
		ri.Unread = (room.UnreadNotifications.NotificationCount > 0) ||
			(room.UnreadNotifications.HighlightCount > 0)
	}
	if room.MSC2654UnreadCount != nil && *room.MSC2654UnreadCount > 0 {
		ri.Unread = true
	}

	if directUser, ok := conn.directs[roomID]; ok {
		ri.IsDirect = true
		if len(ri.Heroes) == 0 {
			ri.Heroes = []id.UserID{directUser}
		}
	} else if !ri.IsDirect && ri.Name == "" && ri.Alias == "" && (ri.Joined+ri.Invited) == 2 {
		// unnamed two-member room without m.direct, treat as direct
		ri.IsDirect = true
	}

	ri.Muted = conn.isRoomMuted(roomID)
	conn.mu.Unlock()

	// room account data
	for _, evt := range room.AccountData.Events {
		if evt.Type.Type == event.AccountDataRoomTags.Type {
			if content, ok := evt.Content.Parsed.(*event.TagEventContent); ok {
				_, isFav := content.Tags[event.RoomTagFavourite]
				conn.mu.Lock()
				ri.Pinned = isFav
				conn.mu.Unlock()
			}
		}
	}

	// ephemeral: receipts first so that read state of messages is known
	for _, evt := range room.Ephemeral.Events {
		switch evt.Type.Type {
		case event.EphemeralEventReceipt.Type:
			conn.handleReceipts(roomID, evt)

		case event.EphemeralEventTyping.Type:
			conn.handleTyping(roomID, evt)
		}
	}

	// timeline
	var msgs []*OutMsg
	for _, evt := range room.Timeline.Events {
		evt.RoomID = roomID
		if evt.StateKey != nil {
			continue
		}

		msg := conn.handleTimelineEvent(ctx, evt, modeSync)
		if msg != nil {
			msgs = append(msgs, msg)
		}
	}

	// cleanup decrypted cache for this room's events
	conn.mu.Lock()
	for _, evt := range room.Timeline.Events {
		delete(conn.decrypted, evt.ID)
	}

	for _, msg := range msgs {
		if msg.TimeSent > ri.LastTs {
			ri.LastTs = msg.TimeSent
		}
	}
	lastTs := ri.LastTs
	unread := ri.Unread
	muted := ri.Muted
	pinned := ri.Pinned
	conn.mu.Unlock()

	// chat and name
	name := conn.roomName(ctx, roomID)
	conn.updateContact(string(roomID), name)
	CMxNewChatsNotify(conn.connId, string(roomID), BoolToInt(unread), BoolToInt(muted), BoolToInt(pinned), lastTs)
	if !initial && (prevPinned != pinned) {
		CMxUpdatePinNotify(conn.connId, string(roomID), BoolToInt(pinned), lastTs)
	}

	if wasInvite {
		// invite accepted (from another client), remove local invite message
		CMxDeleteMessageNotify(conn.connId, string(roomID), "invite:"+string(roomID), 1)
	}

	// sender names
	for _, msg := range msgs {
		if msg.SenderID != "" {
			userID := id.UserID(msg.SenderID)
			conn.updateContact(msg.SenderID, conn.userName(ctx, roomID, userID))
		}
	}

	// messages, as one batch per room
	conn.emitBatch(roomID, msgs, "")
	conn.flushReactions()

}

func (conn *MxConn) handleInvitedRoom(roomID id.RoomID, room *mautrix.SyncInvitedRoom) {
	ctx := context.Background()
	selfId := conn.session.UserID

	conn.mu.Lock()
	ri := conn.getRoom(roomID)
	ri.IsInvite = true
	conn.stateDirty = true
	conn.mu.Unlock()

	var inviteTs int64
	for _, evt := range room.State.Events {
		_ = evt.Content.ParseRaw(evt.Type)
		conn.applyStateEvent(ctx, roomID, evt)
		if evt.Type == event.StateMember && evt.StateKey != nil && id.UserID(*evt.StateKey) == selfId {
			conn.mu.Lock()
			ri.Inviter = evt.Sender
			if evt.Content.Parsed != nil {
				if content, ok := evt.Content.Parsed.(*event.MemberEventContent); ok && content.IsDirect {
					ri.IsDirect = true
					ri.Heroes = []id.UserID{evt.Sender}
				}
			}
			conn.mu.Unlock()
			inviteTs = int64(evt.Timestamp)
		}
	}

	if inviteTs == 0 {
		// stripped invite state has no timestamps
		inviteTs = time.Now().UnixMilli()
	}

	conn.mu.Lock()
	if ri.LastTs == 0 {
		ri.LastTs = inviteTs
	}
	lastTs := ri.LastTs
	inviter := ri.Inviter
	conn.mu.Unlock()

	name := conn.roomName(ctx, roomID)
	conn.updateContact(string(roomID), name)
	CMxNewChatsNotify(conn.connId, string(roomID), 1, 0, 0, lastTs)

	// synthetic, local-only message describing the invite
	text := "Invitation to join this room. Invites are not auto-accepted; accept or decline it from another Matrix client."
	if inviter != "" {
		conn.updateContact(string(inviter), conn.userName(ctx, roomID, inviter))
	}

	msg := &OutMsg{
		ChatID:   string(roomID),
		MsgID:    "invite:" + string(roomID),
		SenderID: string(inviter),
		Text:     text,
		TimeSent: lastTs,
	}
	conn.emitBatch(roomID, []*OutMsg{msg}, "")
}

func (conn *MxConn) handleReceipts(roomID id.RoomID, evt *event.Event) {
	content, ok := evt.Content.Parsed.(*event.ReceiptEventContent)
	if !ok {
		return
	}

	selfId := conn.session.UserID
	var otherReadTs int64
	conn.mu.Lock()
	ri := conn.getRoom(roomID)
	for _, receipts := range *content {
		for receiptType, users := range receipts {
			if receiptType != event.ReceiptTypeRead && receiptType != event.ReceiptTypeReadPrivate {
				continue
			}

			for userID, receipt := range users {
				ts := receipt.Timestamp.UnixMilli()
				if userID == selfId {
					if ts > ri.OwnReadTs {
						ri.OwnReadTs = ts
						conn.stateDirty = true
					}
				} else if ts > otherReadTs {
					otherReadTs = ts
				}
			}
		}
	}

	var readOwn []id.EventID
	if otherReadTs > ri.OtherReadTs {
		ri.OtherReadTs = otherReadTs
		conn.stateDirty = true
		remaining := conn.ownMsgs[roomID][:0]
		for _, own := range conn.ownMsgs[roomID] {
			if own.Ts <= otherReadTs {
				readOwn = append(readOwn, own.ID)
			} else {
				remaining = append(remaining, own)
			}
		}
		conn.ownMsgs[roomID] = remaining
	}
	conn.mu.Unlock()

	for _, msgID := range readOwn {
		CMxNewMessageStatusNotify(conn.connId, string(roomID), string(msgID), 1)
	}
}

func (conn *MxConn) handleTyping(roomID id.RoomID, evt *event.Event) {
	content, ok := evt.Content.Parsed.(*event.TypingEventContent)
	if !ok {
		return
	}

	selfId := conn.session.UserID
	now := make(map[id.UserID]bool)
	for _, userID := range content.UserIDs {
		if userID != selfId {
			now[userID] = true
		}
	}

	conn.mu.Lock()
	prev := conn.typing[roomID]
	conn.typing[roomID] = now
	conn.mu.Unlock()

	for userID := range now {
		if !prev[userID] {
			CMxNewTypingNotify(conn.connId, string(roomID), string(userID), 1)
		}
	}

	for userID := range prev {
		if !now[userID] {
			CMxNewTypingNotify(conn.connId, string(roomID), string(userID), 0)
		}
	}
}
