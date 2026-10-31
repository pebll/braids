// gosg_descriptions.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"context"
	"strings"
	"time"

	"go.mau.fi/mautrix-signal/pkg/signalmeow"
	"go.mau.fi/mautrix-signal/pkg/signalmeow/types"
)

// SgGetDescription answers with CSgNewDescriptionNotify: a group's
// description or a user's profile "about" (with its emoji), empty when there
// is none. Group description changes arrive with group change messages.
func SgGetDescription(connId int, descId string) int {
	go func() {
		// requests can arrive before the client is up
		var client *signalmeow.Client
		for i := 0; i < 240; i++ {
			if client = GetClient(connId); client != nil {
				break
			}
			time.Sleep(500 * time.Millisecond)
		}

		if client == nil {
			return
		}

		ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()

		description := ""
		sid := StringToServiceID(descId)
		if sid.IsEmpty() {
			group, _, err := client.RetrieveGroupByID(ctx, types.GroupIdentifier(descId), 0)
			if (err == nil) && (group != nil) {
				description = group.Description
			}
		} else if contact, err := client.ContactByACI(ctx, sid.UUID); (err == nil) && (contact != nil) {
			description = strings.TrimSpace(contact.Profile.AboutEmoji + " " + contact.Profile.About)
		}

		CSgNewDescriptionNotify(connId, descId, description)
	}()

	return 0
}
