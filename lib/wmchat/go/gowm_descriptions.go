// gowm_descriptions.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"context"
	"time"

	"go.mau.fi/whatsmeow/types"
)

// WmGetDescription answers with CWmNewDescriptionNotify: a group's
// description (topic) or a user's "about" text, empty when there is none or
// it is hidden from us. Group description changes arrive as GroupInfo events.
func WmGetDescription(connId int, descId string) int {
	go func() {
		// requests can arrive before the connection is up
		for i := 0; (i < 240) && (GetState(connId) != Connected); i++ {
			time.Sleep(500 * time.Millisecond)
		}

		client := GetClient(connId)
		if (client == nil) || (GetState(connId) != Connected) {
			return
		}

		jid, err := types.ParseJID(descId)
		if err != nil {
			CWmNewDescriptionNotify(connId, descId, "")
			return
		}

		// shares the avatar queue: WhatsApp rate-limits these queries too
		avatarQueue <- struct{}{}
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		description := ""
		if jid.Server == types.GroupServer {
			if info, err := client.GetGroupInfo(ctx, jid); err == nil && info != nil {
				description = info.Topic
			}
		} else if jid.Server == types.DefaultUserServer || jid.Server == types.HiddenUserServer {
			// the answer may be keyed by a normalized jid
			if infos, err := client.GetUserInfo(ctx, []types.JID{jid}); err == nil {
				for _, info := range infos {
					description = info.Status
				}
			}
		}
		cancel()
		time.Sleep(150 * time.Millisecond)
		<-avatarQueue

		CWmNewDescriptionNotify(connId, descId, description)
	}()

	return 0
}
