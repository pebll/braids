// gomx_group.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"context"
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"

	"maunium.net/go/mautrix"
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/id"
)

// MxCreateGroup creates an encrypted private room with the given name and
// invites the users (ids one per line); answers with CMxCreateGroupNotify.
func MxCreateGroup(connId int, name string, userIds string) int {
	conn := GetConn(connId)
	if conn == nil {
		return -1
	}

	var invite []id.UserID
	for _, u := range strings.Split(userIds, "\n") {
		u = strings.TrimSpace(u)
		if u == "" {
			continue
		}
		if _, _, err := id.UserID(u).Parse(); err != nil {
			CMxCreateGroupNotify(connId, name, false, "", "Not a Matrix user id: "+u)
			return 0
		}
		invite = append(invite, id.UserID(u))
	}

	go func() {
		if !conn.waitReady(time.Minute) {
			CMxCreateGroupNotify(connId, name, false, "", "The account is not connected.")
			return
		}

		ctx, cancel := conn.requestContext()
		defer cancel()
		req := &mautrix.ReqCreateRoom{
			Preset: "private_chat",
			Name:   name,
			Invite: invite,
			InitialState: []*event.Event{{
				Type:    event.StateEncryption,
				Content: event.Content{Parsed: &event.EncryptionEventContent{Algorithm: id.AlgorithmMegolmV1}},
			}},
		}
		resp, err := conn.client.CreateRoom(ctx, req)
		if err != nil {
			LOG_WARNING(fmt.Sprintf("create group failed: %v", err))
			CMxCreateGroupNotify(connId, name, false, "", "Could not create the group: "+err.Error())
			return
		}

		conn.mu.Lock()
		ri := conn.getRoom(resp.RoomID)
		ri.LastTs = time.Now().UnixMilli()
		conn.stateDirty = true
		conn.mu.Unlock()

		conn.updateContact(string(resp.RoomID), name)
		CMxCreateGroupNotify(connId, name, true, string(resp.RoomID), "")
	}()

	return 0
}

// memberPermissions: whether the own user may invite to the room, and the
// joined members it may kick (a lower power level than its own), from the
// room's power levels. Without them (an error), nothing is allowed: the
// server decides anyway.
func (conn *MxConn) memberPermissions(ctx context.Context, roomID id.RoomID, joined map[id.UserID]mautrix.JoinedMember) (bool, []string) {
	var pl event.PowerLevelsEventContent
	if err := conn.client.StateEvent(ctx, roomID, event.StatePowerLevels, "", &pl); err != nil {
		LOG_WARNING(fmt.Sprintf("get power levels failed: %v", err))
		return false, nil
	}
	// room versions with privileged creators (v12): the creators' power is
	// not in the power levels
	if create, err := conn.client.FullStateEvent(ctx, roomID, event.StateCreate, ""); err == nil {
		pl.CreateEvent = create
	}

	self := conn.client.UserID
	own := pl.GetUserLevel(self)
	canAdd := own >= pl.Invite()
	var canRemove []string
	if own >= pl.Kick() {
		for userID := range joined {
			if userID != self && pl.GetUserLevel(userID) < own {
				canRemove = append(canRemove, string(userID))
			}
		}
		sort.Strings(canRemove)
	}

	return canAdd, canRemove
}

// MxEditMembers invites users to a room (ids one per line) or kicks them;
// answers with CMxEditMembersNotify, then on success sends the member list
// again.
func MxEditMembers(connId int, chatId string, userIds string, remove bool) int {
	conn := GetConn(connId)
	if conn == nil {
		return -1
	}

	var users []id.UserID
	for _, u := range strings.Split(userIds, "\n") {
		u = strings.TrimSpace(u)
		if u == "" {
			continue
		}
		if _, _, err := id.UserID(u).Parse(); err != nil {
			CMxEditMembersNotify(connId, chatId, userIds, remove, false, "Not a Matrix user id: "+u)
			return 0
		}
		users = append(users, id.UserID(u))
	}

	go func() {
		if !conn.waitReady(time.Minute) {
			CMxEditMembersNotify(connId, chatId, userIds, remove, false, "The account is not connected.")
			return
		}

		ctx, cancel := conn.requestContext()
		defer cancel()
		roomID := id.RoomID(chatId)
		var failed []string
		var lastErr error
		for _, u := range users {
			var err error
			if remove {
				_, err = conn.client.KickUser(ctx, roomID, &mautrix.ReqKickUser{UserID: u})
			} else {
				_, err = conn.client.InviteUser(ctx, roomID, &mautrix.ReqInviteUser{UserID: u})
			}
			if err != nil {
				LOG_WARNING(fmt.Sprintf("edit members failed: %v", err))
				failed = append(failed, string(u))
				lastErr = err
			}
		}

		if len(failed) > 0 {
			what := "add"
			if remove {
				what = "remove"
			}
			CMxEditMembersNotify(connId, chatId, userIds, remove, false,
				fmt.Sprintf("Could not %s %s: %s", what, strings.Join(failed, ", "), memberError(lastErr)))
		} else {
			CMxEditMembersNotify(connId, chatId, userIds, remove, true, "")
		}

		if len(failed) < len(users) {
			MxGetGroupMembers(connId, chatId)
		}
	}()

	return 0
}

// the server's reason, without the request details
func memberError(err error) string {
	var httpErr mautrix.HTTPError
	if errors.As(err, &httpErr) && httpErr.RespError != nil && httpErr.RespError.Err != "" {
		return httpErr.RespError.Err
	}
	return err.Error()
}
