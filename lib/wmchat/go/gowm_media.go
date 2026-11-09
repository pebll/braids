// gowm_media.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waMmsRetry"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
)

// Media retry: WhatsApp removes media from its servers after a while (the
// download answers 404 / 410). As WhatsApp's apps do, braids then asks the
// sender's phone to upload it again (a media retry receipt) and downloads it
// from the new path the answer names.

var (
	mediaRetryMutex   sync.Mutex
	mediaRetryWaiting = map[string]chan *events.MediaRetry{} // message id -> waiting download
)

// handleMediaRetry hands the answer to the download waiting for it.
func handleMediaRetry(evt *events.MediaRetry) {
	mediaRetryMutex.Lock()
	ch, ok := mediaRetryWaiting[evt.MessageID]
	mediaRetryMutex.Unlock()
	if ok {
		select {
		case ch <- evt:
		default:
		}
	}
}

// isMediaGone: the media is no longer on WhatsApp's servers.
func isMediaGone(err error) bool {
	return errors.Is(err, whatsmeow.ErrMediaDownloadFailedWith404) ||
		errors.Is(err, whatsmeow.ErrMediaDownloadFailedWith410) ||
		errors.Is(err, whatsmeow.ErrNoURLPresent)
}

// retryMediaDownload asks for the media to be uploaded again and downloads it
// from the new path. fallbackChatId / fallbackMsgId name the message for
// file ids from before braids stored the message's details.
func retryMediaDownload(client *whatsmeow.Client, info DownloadInfo, fallbackChatId string, fallbackMsgId string) ([]byte, error) {
	msgId := info.MsgId
	chatStr := info.ChatJid
	if msgId == "" {
		msgId = fallbackMsgId
	}
	if chatStr == "" {
		chatStr = fallbackChatId
	}

	chat, err := types.ParseJID(chatStr)
	if err != nil || msgId == "" || len(info.MediaKey) == 0 {
		return nil, fmt.Errorf("media retry: message unknown")
	}

	sender := chat
	if info.SenderJid != "" {
		if jid, err := types.ParseJID(info.SenderJid); err == nil {
			sender = jid
		}
	}

	isGroup := chat.Server == types.GroupServer
	if isGroup && info.SenderJid == "" {
		// the group member who sent it is needed; not known for old file ids
		return nil, fmt.Errorf("media retry: sender unknown")
	}

	ch := make(chan *events.MediaRetry, 1)
	mediaRetryMutex.Lock()
	mediaRetryWaiting[msgId] = ch
	mediaRetryMutex.Unlock()
	defer func() {
		mediaRetryMutex.Lock()
		delete(mediaRetryWaiting, msgId)
		mediaRetryMutex.Unlock()
	}()

	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	msgInfo := &types.MessageInfo{
		MessageSource: types.MessageSource{Chat: chat, Sender: sender, IsFromMe: info.FromMe, IsGroup: isGroup},
		ID:            msgId,
	}
	LOG_DEBUG(fmt.Sprintf("media retry %s", msgId))
	if err := client.SendMediaRetryReceipt(ctx, msgInfo, info.MediaKey); err != nil {
		return nil, fmt.Errorf("media retry receipt: %w", err)
	}

	var evt *events.MediaRetry
	select {
	case evt = <-ch:
	case <-ctx.Done():
		return nil, fmt.Errorf("media retry: no answer from the phone")
	}

	notif, err := whatsmeow.DecryptMediaRetryNotification(evt, info.MediaKey)
	if err != nil {
		return nil, fmt.Errorf("media retry: %w", err)
	}
	if notif.GetResult() != waMmsRetry.MediaRetryNotification_SUCCESS || notif.GetDirectPath() == "" {
		return nil, fmt.Errorf("media retry result %v", notif.GetResult())
	}

	LOG_DEBUG(fmt.Sprintf("media retry ok %s", msgId))
	return client.DownloadMediaWithPath(ctx, notif.GetDirectPath(), info.FileEncSha256, info.FileSha256, info.MediaKey, info.MediaType,
		whatsmeow.GetMMSType(info.MediaType), false)
}
