// gosg_avatars.go
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
	"net/http"
	"os"
	"time"

	"go.mau.fi/mautrix-signal/pkg/signalmeow"
	"go.mau.fi/mautrix-signal/pkg/signalmeow/types"
)

// one download at a time
var avatarQueue = make(chan struct{}, 1)

// SgGetAvatar answers with CSgNewAvatarNotify: a user's profile picture
// (decrypted with their profile key, else the picture from the phone's
// contact sync) or a group's picture (decrypted with the group key). Files
// are cached by the avatar path, which changes with the picture.
func SgGetAvatar(connId int, avatarId string) int {
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

		avatarQueue <- struct{}{}
		path := getAvatar(connId, client, avatarId)
		<-avatarQueue

		CSgNewAvatarNotify(connId, avatarId, path)
	}()

	return 0
}

func getAvatar(connId int, client *signalmeow.Client, avatarId string) string {
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	sid := StringToServiceID(avatarId)
	if sid.IsEmpty() {
		group, _, err := client.RetrieveGroupByID(ctx, types.GroupIdentifier(avatarId), 0)
		if (err != nil) || (group == nil) || (group.AvatarPath == "") {
			return ""
		}

		return cachedAvatar(connId, "group:"+group.AvatarPath, func() ([]byte, error) {
			return client.DownloadGroupAvatar(ctx, group.AvatarPath, group.GroupMasterKey)
		})
	}

	contact, err := client.ContactByACI(ctx, sid.UUID)
	if (err != nil) || (contact == nil) {
		return ""
	}

	if contact.Profile.AvatarPath != "" {
		path := cachedAvatar(connId, "profile:"+contact.Profile.AvatarPath, func() ([]byte, error) {
			return client.DownloadUserAvatar(ctx, contact.Profile.AvatarPath, contact.Profile.Key)
		})
		if path != "" {
			return path
		}
	}

	if len(contact.ContactAvatar.Image) > 0 {
		return cachedAvatar(connId, "contact:"+contact.ContactAvatar.Hash+avatarId, func() ([]byte, error) {
			return contact.ContactAvatar.Image, nil
		})
	}

	return ""
}

// cachedAvatar returns the file for key, fetching it if not present yet.
func cachedAvatar(connId int, key string, fetch func() ([]byte, error)) string {
	dir := GetPath(connId) + "/avatars"
	sum := sha256.Sum256([]byte(key))
	base := dir + "/" + hex.EncodeToString(sum[:12])
	for _, ext := range []string{".jpg", ".png", ".webp", ".gif"} {
		if _, err := os.Stat(base + ext); err == nil {
			return base + ext
		}
	}

	data, err := fetch()
	if err != nil {
		LOG_WARNING(fmt.Sprintf("avatar download failed: %v", err))
		return ""
	}

	ext := ".jpg"
	switch http.DetectContentType(data) {
	case "image/png":
		ext = ".png"
	case "image/webp":
		ext = ".webp"
	case "image/gif":
		ext = ".gif"
	}

	if err := os.MkdirAll(dir, 0700); err != nil {
		return ""
	}

	if err := os.WriteFile(base+ext, data, 0600); err != nil {
		return ""
	}

	return base + ext
}
