// gomx_test.go
//
// Copyright (c) 2026 nchat contributors
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

// Unit tests for functions that do not call back into nchat (C++).
// Run with: go test -tags goolm ./...

package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strings"
	"testing"
	"time"

	"maunium.net/go/mautrix"
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/format"
	"maunium.net/go/mautrix/id"
	"maunium.net/go/mautrix/pushrules"
)

func TestNormalizeUserId(t *testing.T) {
	cases := map[string]string{
		"@alice:example.org":     "@alice:example.org",
		"alice:example.org":      "@alice:example.org",
		"pebll@matrix.fenker.eu": "@pebll:matrix.fenker.eu",
		"@a@b":                   "",
		"  @bob:localhost:8448":  "@bob:localhost:8448",
		"alice":                  "",
		"":                       "",
	}

	for in, want := range cases {
		if got := MxNormalizeUserId(in); got != want {
			t.Errorf("MxNormalizeUserId(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestMentionsToMarkdown(t *testing.T) {
	mentions, _ := json.Marshal(map[string]string{
		"Carol Danvers": "@carol:localhost",
		"bob":           "@bob:localhost",
		"nobody":        "@nobody:localhost",
	})

	text, userIDs := mentionsToMarkdown("Hey @[Carol Danvers] and @bob!", string(mentions))
	if !strings.Contains(text, "[Carol Danvers](https://matrix.to/#/@carol:localhost)") {
		t.Errorf("bracket mention not converted: %q", text)
	}

	if !strings.Contains(text, "[bob](https://matrix.to/#/@bob:localhost)") {
		t.Errorf("plain mention not converted: %q", text)
	}

	if len(userIDs) != 2 {
		t.Errorf("expected 2 mentioned users, got %v", userIDs)
	}

	rendered := format.RenderMarkdown(text, true, false)
	if !strings.Contains(rendered.FormattedBody, `href="https://matrix.to/#/@carol:localhost"`) {
		t.Errorf("pill missing in html: %q", rendered.FormattedBody)
	}
}

func TestMentionsToMarkdownEmpty(t *testing.T) {
	text, userIDs := mentionsToMarkdown("plain text", "")
	if text != "plain text" || len(userIDs) != 0 {
		t.Errorf("unexpected result %q %v", text, userIDs)
	}
}

func TestHTMLParserPills(t *testing.T) {
	html := `Hi <a href="https://matrix.to/#/@alice:localhost">Alice Liddell</a> and ` +
		`<a href="https://matrix.to/#/@carol:localhost">carol</a>, <del>old</del> <code>x</code> <strong>b</strong>`
	got, _ := format.HTMLToMarkdownFull(htmlParser, html)
	want := "Hi @[Alice Liddell] and @carol, ~~old~~ `x` **b**"
	if got != want {
		t.Errorf("got %q, want %q", got, want)
	}
}

func newTestConn(t *testing.T) *MxConn {
	conn := &MxConn{
		path:      t.TempDir(),
		session:   &Session{UserID: "@alice:localhost", DeviceID: "DEV"},
		directs:   make(map[id.RoomID]id.UserID),
		utdShown:  make(map[id.EventID]bool),
		ownMsgs:   make(map[id.RoomID][]OwnMsg),
		sentFiles: make(map[id.EventID]string),
	}
	conn.state = &PersistState{
		Version:   stateVersion,
		Rooms:     make(map[id.RoomID]*RoomInfo),
		Names:     make(map[id.UserID]string),
		Reactions: make(map[id.EventID]*ReactionRef),
		Pending:   make(map[id.EventID]*event.Event),
	}

	return conn
}

func TestRenderContentReplyFallback(t *testing.T) {
	conn := newTestConn(t)
	content := &event.MessageEventContent{
		MsgType:       event.MsgText,
		Body:          "> <@bob:localhost> original\n\nthe reply",
		Format:        event.FormatHTML,
		FormattedBody: "<mx-reply><blockquote>original</blockquote></mx-reply>the reply",
		RelatesTo:     &event.RelatesTo{InReplyTo: &event.InReplyTo{EventID: "$orig"}},
	}

	if got := conn.renderContent(context.Background(), "", "@bob:localhost", content); got != "the reply" {
		t.Errorf("got %q", got)
	}
}

func TestRenderContentMediaCaption(t *testing.T) {
	conn := newTestConn(t)
	noCaption := &event.MessageEventContent{MsgType: event.MsgImage, Body: "cat.png"}
	if got := conn.renderContent(context.Background(), "", "@bob:localhost", noCaption); got != "" {
		t.Errorf("expected no text for file without caption, got %q", got)
	}

	caption := &event.MessageEventContent{MsgType: event.MsgImage, Body: "look at this", FileName: "cat.png"}
	if got := conn.renderContent(context.Background(), "", "@bob:localhost", caption); got != "look at this" {
		t.Errorf("caption got %q", got)
	}
}

func TestFileInfoFor(t *testing.T) {
	conn := newTestConn(t)
	evt := &event.Event{ID: "$abcDEF/123+xyz-very-long-event-id"}
	content := &event.MessageEventContent{
		MsgType: event.MsgFile,
		Body:    "../../etc/my report.pdf",
		URL:     "mxc://localhost/abc",
		Info:    &event.FileInfo{MimeType: "application/pdf"},
	}

	fileId, filePath, status := conn.fileInfoFor(evt, content)
	if status != FileStatusNotDownloaded {
		t.Errorf("status %d", status)
	}

	if !strings.HasPrefix(filePath, tmpPath(conn.path)+"/") || strings.Contains(filePath, "..") {
		t.Errorf("unsafe path %q", filePath)
	}

	if !strings.HasSuffix(filePath, "/my_report.pdf") {
		t.Errorf("unexpected name %q", filePath)
	}

	var info MxFileInfo
	if err := json.Unmarshal([]byte(fileId), &info); err != nil || info.TargetPath != filePath ||
		info.Version != fileInfoVersion {
		t.Errorf("bad file id %q", fileId)
	}

	if _, _, status := conn.fileInfoFor(evt, &event.MessageEventContent{Body: "text"}); status != FileStatusNone {
		t.Errorf("expected no file for text message")
	}
}

func TestIsRoomMuted(t *testing.T) {
	conn := newTestConn(t)
	var rules pushrules.PushRuleset
	raw := `{"override":[{"rule_id":"!muted:localhost","enabled":true,"actions":[],
	  "conditions":[{"kind":"event_match","key":"room_id","pattern":"!muted:localhost"}]}],
	  "room":[{"rule_id":"!quiet:localhost","enabled":true,"actions":["dont_notify"]},
	          {"rule_id":"!loud:localhost","enabled":true,"actions":["notify"]}]}`
	if err := json.Unmarshal([]byte(raw), &rules); err != nil {
		t.Fatal(err)
	}

	conn.pushRules = &rules
	for roomID, want := range map[id.RoomID]bool{
		"!muted:localhost": true, "!quiet:localhost": true, "!loud:localhost": false, "!other:localhost": false,
	} {
		if got := conn.isRoomMuted(roomID); got != want {
			t.Errorf("isRoomMuted(%s) = %v, want %v", roomID, got, want)
		}
	}
}

func TestIsReadLocked(t *testing.T) {
	conn := newTestConn(t)
	ri := conn.getRoom("!r:localhost")
	ri.OwnReadTs = 1000
	ri.OtherReadTs = 500
	if !conn.isReadLocked("!r:localhost", "@bob:localhost", 900) {
		t.Error("incoming before own receipt should be read")
	}
	if conn.isReadLocked("!r:localhost", "@bob:localhost", 1100) {
		t.Error("incoming after own receipt should be unread")
	}
	if conn.isReadLocked("!r:localhost", "@alice:localhost", 600) {
		t.Error("outgoing after other receipt should be unread")
	}
}

func TestIsTransientError(t *testing.T) {
	network := fmt.Errorf("wrapped: %w", mautrix.HTTPError{Message: "request error", WrappedError: errors.New("refused")})
	server := mautrix.HTTPError{Response: &http.Response{StatusCode: 502}}
	limited := mautrix.HTTPError{Response: &http.Response{StatusCode: 429}}
	forbidden := mautrix.HTTPError{Response: &http.Response{StatusCode: 403}}
	if !isTransientError(network) || !isTransientError(server) || !isTransientError(limited) {
		t.Error("expected transient")
	}
	if isTransientError(forbidden) || isTransientError(errors.New("mismatching identity key")) {
		t.Error("expected fatal")
	}
}

func TestBackoffDelay(t *testing.T) {
	if backoffDelay(0) != time.Second {
		t.Error("first delay should be 1s")
	}
	if backoffDelay(3) != 8*time.Second {
		t.Error("delay 3 should be 8s")
	}
	if backoffDelay(100) != 60*time.Second {
		t.Error("delay should cap at 60s")
	}
}

func TestStateRoundTrip(t *testing.T) {
	conn := newTestConn(t)
	conn.getRoom("!r:localhost").Name = "Room"
	conn.state.Names["@bob:localhost"] = "Bob"
	conn.saveState(true)

	conn2 := newTestConn(t)
	conn2.path = conn.path
	conn2.loadState()
	if conn2.state.Rooms["!r:localhost"] == nil || conn2.state.Rooms["!r:localhost"].Name != "Room" ||
		conn2.state.Names["@bob:localhost"] != "Bob" {
		t.Errorf("state not restored: %+v", conn2.state)
	}
}
