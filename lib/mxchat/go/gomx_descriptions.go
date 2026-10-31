// gomx_descriptions.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"strings"
	"time"

	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/id"
)

// MxGetDescription answers with CMxNewDescriptionNotify: the topic of a room
// (!...), empty for users (Matrix has no bio) and rooms without a topic.
// Topic changes arrive with the sync (applyStateEvent).
func MxGetDescription(connId int, descId string) int {
	conn := GetConn(connId)
	if conn == nil {
		return -1
	}

	go func() {
		if !conn.waitReady(2 * time.Minute) {
			return
		}

		topic := ""
		if strings.HasPrefix(descId, "!") {
			ctx, cancel := conn.requestContext()
			var content event.TopicEventContent
			if err := conn.client.StateEvent(ctx, id.RoomID(descId), event.StateTopic, "", &content); err == nil {
				topic = content.Topic
			}
			cancel()
		}

		CMxNewDescriptionNotify(conn.connId, descId, topic)
	}()

	return 0
}
