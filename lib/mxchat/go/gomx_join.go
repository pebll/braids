// gomx_join.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"fmt"
	"net/url"
	"strings"
	"time"

	"maunium.net/go/mautrix"
)

// joinTarget extracts the room alias or id (and via servers) from
// "#room:server", "!id:server" or a https://matrix.to/#/... link.
func joinTarget(link string) (string, []string) {
	link = strings.TrimSpace(link)
	if i := strings.Index(link, "matrix.to/#/"); i >= 0 {
		link = link[i+len("matrix.to/#/"):]
	}

	var via []string
	if i := strings.Index(link, "?"); i >= 0 {
		if q, err := url.ParseQuery(link[i+1:]); err == nil {
			via = q["via"]
		}
		link = link[:i]
	}

	if unescaped, err := url.PathUnescape(link); err == nil {
		link = unescaped
	}

	return link, via
}

// MxJoinChat joins a room by alias, id or matrix.to link and answers with
// CMxJoinChatNotify; the room itself arrives with the sync.
func MxJoinChat(connId int, link string) int {
	conn := GetConn(connId)
	if conn == nil {
		return -1
	}

	go func() {
		target, via := joinTarget(link)
		if !strings.HasPrefix(target, "#") && !strings.HasPrefix(target, "!") {
			CMxJoinChatNotify(connId, link, false, "", "Not a Matrix room address (#room:server or a matrix.to link).")
			return
		}

		if !conn.waitReady(time.Minute) {
			CMxJoinChatNotify(connId, link, false, "", "The account is not connected.")
			return
		}

		ctx, cancel := conn.requestContext()
		defer cancel()
		resp, err := conn.client.JoinRoom(ctx, target, &mautrix.ReqJoinRoom{Via: via})
		if err != nil {
			LOG_WARNING(fmt.Sprintf("join failed: %v", err))
			CMxJoinChatNotify(connId, link, false, "", "Could not join: "+matrixErrorText(err))
			return
		}

		CMxJoinChatNotify(connId, link, true, string(resp.RoomID), "")
	}()

	return 0
}

func matrixErrorText(err error) string {
	if httpErr, ok := err.(mautrix.HTTPError); ok && httpErr.RespError != nil && httpErr.RespError.Err != "" {
		return httpErr.RespError.Err
	}

	return err.Error()
}
