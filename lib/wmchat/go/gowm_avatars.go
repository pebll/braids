// gowm_avatars.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/types"
)

// avatarEntry is a cached profile picture: WhatsApp's picture id and the file.
type avatarEntry struct {
	PictureID string `json:"id"`
	File      string `json:"file"`
}

var (
	avatarMutex   sync.Mutex
	avatarIndexes = map[int]map[string]avatarEntry{}
	// one lookup at a time, WhatsApp rate-limits profile picture queries
	avatarQueue = make(chan struct{}, 1)
)

func avatarDir(connId int) string { return GetPath(connId) + "/avatars" }

func loadAvatarIndex(connId int) map[string]avatarEntry {
	if index, ok := avatarIndexes[connId]; ok {
		return index
	}

	index := map[string]avatarEntry{}
	if data, err := os.ReadFile(avatarDir(connId) + "/index.json"); err == nil {
		_ = json.Unmarshal(data, &index)
	}

	avatarIndexes[connId] = index
	return index
}

func saveAvatarIndex(connId int, index map[string]avatarEntry) {
	if data, err := json.Marshal(index); err == nil {
		_ = os.WriteFile(avatarDir(connId)+"/index.json", data, 0600)
	}
}

// WmGetAvatar answers with CWmNewAvatarNotify: the cached picture when
// WhatsApp reports it unchanged, else a freshly downloaded preview (small)
// picture, or an empty path when there is none or it is hidden from us.
func WmGetAvatar(connId int, avatarId string) int {
	go func() {
		// requests can arrive before the connection is up
		for i := 0; (i < 240) && (GetState(connId) != Connected); i++ {
			time.Sleep(500 * time.Millisecond)
		}

		client := GetClient(connId)
		if (client == nil) || (GetState(connId) != Connected) {
			return
		}

		avatarQueue <- struct{}{}
		path := getAvatar(connId, client, avatarId)
		time.Sleep(150 * time.Millisecond)
		<-avatarQueue

		CWmNewAvatarNotify(connId, avatarId, path)
	}()

	return 0
}

func getAvatar(connId int, client *whatsmeow.Client, avatarId string) string {
	jid, err := types.ParseJID(avatarId)
	if err != nil {
		return ""
	}

	avatarMutex.Lock()
	index := loadAvatarIndex(connId)
	entry, known := index[avatarId]
	avatarMutex.Unlock()
	if known {
		if _, err := os.Stat(entry.File); err != nil {
			known = false
		}
	}

	params := &whatsmeow.GetProfilePictureParams{Preview: true}
	if known {
		params.ExistingID = entry.PictureID
	}

	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	info, err := client.GetProfilePictureInfo(ctx, jid, params)
	if errors.Is(err, whatsmeow.ErrProfilePictureNotSet) || errors.Is(err, whatsmeow.ErrProfilePictureUnauthorized) {
		forgetAvatar(connId, avatarId)
		return ""
	} else if err != nil {
		LOG_WARNING(fmt.Sprintf("get profile picture failed %s: %v", avatarId, err))
		if known {
			return entry.File
		}
		return ""
	} else if info == nil {
		// unchanged
		if known {
			return entry.File
		}
		return ""
	}

	sum := sha256.Sum256([]byte(avatarId))
	file := avatarDir(connId) + "/" + hex.EncodeToString(sum[:12]) + ".jpg"
	if err := downloadAvatar(ctx, info.URL, file); err != nil {
		LOG_WARNING(fmt.Sprintf("download profile picture failed %s: %v", avatarId, err))
		return ""
	}

	avatarMutex.Lock()
	index[avatarId] = avatarEntry{PictureID: info.ID, File: file}
	saveAvatarIndex(connId, index)
	avatarMutex.Unlock()
	return file
}

func forgetAvatar(connId int, avatarId string) {
	avatarMutex.Lock()
	defer avatarMutex.Unlock()
	index := loadAvatarIndex(connId)
	if entry, ok := index[avatarId]; ok {
		_ = os.Remove(entry.File)
		delete(index, avatarId)
		saveAvatarIndex(connId, index)
	}
}

func downloadAvatar(ctx context.Context, url string, file string) error {
	if err := os.MkdirAll(filepath.Dir(file), 0700); err != nil {
		return err
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("http %d", resp.StatusCode)
	}

	data, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return err
	}

	tmp := file + ".tmp"
	if err := os.WriteFile(tmp, data, 0600); err != nil {
		return err
	}

	return os.Rename(tmp, file)
}
