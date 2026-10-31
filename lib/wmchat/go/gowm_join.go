// gowm_join.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"context"
	"fmt"
	"strings"
	"time"
)

// WmJoinChat joins a group by invite link (https://chat.whatsapp.com/CODE or
// the code alone) and answers with CWmJoinChatNotify.
func WmJoinChat(connId int, link string) int {
	go func() {
		code := strings.TrimSpace(link)
		if i := strings.Index(code, "chat.whatsapp.com/"); i >= 0 {
			code = code[i+len("chat.whatsapp.com/"):]
		}
		code = strings.SplitN(strings.SplitN(code, "?", 2)[0], "/", 2)[0]
		if code == "" || strings.ContainsAny(code, " .:") {
			CWmJoinChatNotify(connId, link, false, "", "Not a WhatsApp invite link (https://chat.whatsapp.com/…).")
			return
		}

		client := GetClient(connId)
		if (client == nil) || (GetState(connId) != Connected) {
			CWmJoinChatNotify(connId, link, false, "", "The account is not connected.")
			return
		}

		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		jid, err := client.JoinGroupWithLink(ctx, code)
		if err != nil {
			LOG_WARNING(fmt.Sprintf("join failed: %v", err))
			CWmJoinChatNotify(connId, link, false, "", "Could not join: "+err.Error())
			return
		}

		CWmJoinChatNotify(connId, link, true, GetChatId(client, &jid, nil), "")
	}()

	return 0
}
