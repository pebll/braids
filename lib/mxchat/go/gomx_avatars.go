// gomx_avatars.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"io"
	"mime"
	"net/http"
	"os"
	"strings"
	"time"

	"maunium.net/go/mautrix"
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/id"
)

// at most this many avatar downloads at a time
var avatarSlots = make(chan struct{}, 4)

func avatarsPath(path string) string { return path + "/avatars" }

// MxGetAvatar resolves the avatar of a user (@...) or room (!...; for a
// direct room the other user's) and answers with CMxNewAvatarNotify, an
// empty path when there is none. Images are cached by mxc uri, so only the
// (small) lookup of the uri hits the server again.
func MxGetAvatar(connId int, avatarId string) int {
	conn := GetConn(connId)
	if conn == nil {
		return -1
	}

	go func() {
		if !conn.waitReady(2 * time.Minute) {
			return
		}

		avatarSlots <- struct{}{}
		defer func() { <-avatarSlots }()

		ctx, cancel := conn.requestContext()
		defer cancel()
		uri := conn.avatarURI(ctx, avatarId)
		path := ""
		if !uri.IsEmpty() {
			var err error
			path, err = conn.downloadAvatar(ctx, uri)
			if err != nil {
				LOG_WARNING(fmt.Sprintf("avatar download failed %s: %v", avatarId, err))
			}
		}

		CMxNewAvatarNotify(conn.connId, avatarId, path)
	}()

	return 0
}

func (conn *MxConn) avatarURI(ctx context.Context, avatarId string) id.ContentURI {
	client := conn.client
	if strings.HasPrefix(avatarId, "@") {
		uri, err := client.GetAvatarURL(ctx, id.UserID(avatarId))
		if err != nil {
			return id.ContentURI{}
		}
		return uri
	}

	roomID := id.RoomID(avatarId)
	var content event.RoomAvatarEventContent
	err := client.StateEvent(ctx, roomID, event.StateRoomAvatar, "", &content)
	if err == nil && !content.URL.ParseOrIgnore().IsEmpty() {
		return content.URL.ParseOrIgnore()
	}

	// direct room without its own avatar: the other user's
	conn.mu.Lock()
	other, isDirect := conn.directs[roomID]
	conn.mu.Unlock()
	if isDirect && other != "" {
		uri, err := client.GetAvatarURL(ctx, other)
		if err == nil {
			return uri
		}
	}

	return id.ContentURI{}
}

func (conn *MxConn) downloadAvatar(ctx context.Context, uri id.ContentURI) (string, error) {
	dir := avatarsPath(conn.path)
	sum := sha256.Sum256([]byte(uri.String()))
	base := dir + "/" + hex.EncodeToString(sum[:12])
	for _, ext := range []string{".jpg", ".png", ".webp", ".gif"} {
		if _, err := os.Stat(base + ext); err == nil {
			return base + ext, nil
		}
	}

	if err := os.MkdirAll(dir, 0700); err != nil {
		return "", err
	}

	// a thumbnail is enough for an avatar; servers without thumbnails get the original
	resp, err := conn.client.DownloadThumbnail(ctx, uri, 96, 96, mautrix.DownloadThumbnailExtra{Method: "crop"})
	if err != nil || resp.StatusCode != http.StatusOK {
		if resp != nil {
			resp.Body.Close()
		}
		resp, err = conn.client.Download(ctx, uri)
		if err != nil {
			return "", err
		}
	}
	defer resp.Body.Close()

	data, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return "", err
	}

	ext := ".jpg"
	if exts, err := mime.ExtensionsByType(resp.Header.Get("Content-Type")); err == nil && len(exts) > 0 {
		for _, e := range exts {
			if e == ".png" || e == ".webp" || e == ".gif" || e == ".jpg" {
				ext = e
				break
			}
		}
	}

	path := base + ext
	if err := writePrivateFile(path, data); err != nil {
		return "", err
	}

	return path, nil
}
