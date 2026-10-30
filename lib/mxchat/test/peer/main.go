// main.go
//
// Copyright (c) 2026 nchat contributors
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

// peer is a scriptable Matrix client used as the "other side" when testing
// mxchat. It reads one command per line on stdin and prints events and command
// results as JSON lines on stdout. It uses mautrix-go with e2ee, so it can
// stand in for Element in automated tests.
//
// Usage: peer -hs http://localhost:8008 -user bob -pass test-bob-pw -store DIR
//
// Commands:
//   create <invitee> [encrypted]      create room, prints room id
//   join <room>                       join room
//   send <room> <text...>             send text (markdown)
//   reply <room> <event> <text...>    send reply
//   edit <room> <event> <text...>     edit own message
//   react <room> <event> <key>        send reaction
//   redact <room> <event>             redact event
//   topic <room> <text...>            set the room topic (a state event)
//   typing <room> on|off              typing notification
//   read <room> <event>               send read receipt
//   sendfile <room> <path>            send attachment (encrypted in e2ee rooms)
//   bootstrap                         set up cross-signing, secret storage and key
//                                     backup; prints the recovery key
//   backup                            upload all megolm sessions to key backup
//   verified <user> <device>          check if a device is cross-signed by its owner
//   logout                            log out this device
//   sleep <seconds>
//   quit

package main

import (
	"bufio"
	"context"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/rs/zerolog"
	"maunium.net/go/mautrix"
	"maunium.net/go/mautrix/crypto/attachment"
	"maunium.net/go/mautrix/crypto/backup"
	"maunium.net/go/mautrix/crypto/cryptohelper"
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/format"
	"maunium.net/go/mautrix/id"
)

var outMutex sync.Mutex

func out(v map[string]any) {
	outMutex.Lock()
	defer outMutex.Unlock()
	data, _ := json.Marshal(v)
	fmt.Println(string(data))
}

type peer struct {
	client    *mautrix.Client
	helper    *cryptohelper.CryptoHelper
	password  string
	backupKey *backup.MegolmBackupKey
	backupVer id.KeyBackupVersion
}

func main() {
	hs := flag.String("hs", "http://localhost:8008", "homeserver url")
	user := flag.String("user", "bob", "username")
	pass := flag.String("pass", "", "password")
	store := flag.String("store", "", "store directory")
	autojoin := flag.Bool("autojoin", true, "auto-join invites")
	flag.Parse()

	_ = os.MkdirAll(*store, 0700)
	ctx := context.Background()
	client, err := mautrix.NewClient(*hs, "", "")
	if err != nil {
		panic(err)
	}

	logFile, _ := os.OpenFile(filepath.Join(*store, "peer.log"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600)
	client.Log = zerolog.New(logFile).With().Timestamp().Logger().Level(zerolog.DebugLevel)

	syncer := client.Syncer.(*mautrix.DefaultSyncer)
	helper, err := cryptohelper.NewCryptoHelper(client, []byte("peer-pickle-key"), filepath.Join(*store, "crypto.db"))
	if err != nil {
		panic(err)
	}

	helper.LoginAs = &mautrix.ReqLogin{
		Type:                     mautrix.AuthTypePassword,
		Identifier:               mautrix.UserIdentifier{Type: mautrix.IdentifierTypeUser, User: *user},
		Password:                 *pass,
		InitialDeviceDisplayName: "peer",
	}
	if err := helper.Init(ctx); err != nil {
		panic(err)
	}
	client.Crypto = helper

	p := &peer{client: client, helper: helper, password: *pass}
	out(map[string]any{"type": "ready", "user": client.UserID, "device": client.DeviceID})

	syncer.OnEventType(event.EventMessage, func(ctx context.Context, evt *event.Event) {
		content := evt.Content.AsMessage()
		m := map[string]any{"type": "message", "room": evt.RoomID, "id": evt.ID, "sender": evt.Sender,
			"body": content.Body, "msgtype": content.MsgType,
			"encrypted": evt.Mautrix.WasEncrypted}
		if content.FormattedBody != "" {
			m["html"] = content.FormattedBody
		}
		if content.RelatesTo != nil {
			m["relates"] = content.RelatesTo
		}
		if content.NewContent != nil {
			m["new_body"] = content.NewContent.Body
		}
		if content.Mentions != nil && len(content.Mentions.UserIDs) > 0 {
			m["mentions"] = content.Mentions.UserIDs
		}
		if content.URL != "" || content.File != nil {
			m["file"] = true
			m["file_encrypted"] = (content.File != nil)
			data, err := p.download(ctx, content)
			if err == nil {
				m["file_size"] = len(data)
			} else {
				m["file_error"] = err.Error()
			}
		}
		out(m)
	})
	syncer.OnEventType(event.EventReaction, func(ctx context.Context, evt *event.Event) {
		content := evt.Content.AsReaction()
		out(map[string]any{"type": "reaction", "room": evt.RoomID, "id": evt.ID, "sender": evt.Sender,
			"target": content.RelatesTo.EventID, "key": content.RelatesTo.Key})
	})
	syncer.OnEventType(event.EventRedaction, func(ctx context.Context, evt *event.Event) {
		redacts := evt.Redacts
		if redacts == "" {
			redacts = evt.Content.AsRedaction().Redacts
		}
		out(map[string]any{"type": "redaction", "room": evt.RoomID, "id": evt.ID, "sender": evt.Sender,
			"redacts": redacts})
	})
	syncer.OnEventType(event.EphemeralEventReceipt, func(ctx context.Context, evt *event.Event) {
		for eventID, receipts := range *evt.Content.AsReceipt() {
			for rtype, users := range receipts {
				for userID := range users {
					if userID != client.UserID {
						out(map[string]any{"type": "receipt", "room": evt.RoomID, "event": eventID,
							"user": userID, "rtype": rtype})
					}
				}
			}
		}
	})
	syncer.OnEventType(event.EphemeralEventTyping, func(ctx context.Context, evt *event.Event) {
		out(map[string]any{"type": "typing", "room": evt.RoomID, "users": evt.Content.AsTyping().UserIDs})
	})
	syncer.OnEventType(event.StateMember, func(ctx context.Context, evt *event.Event) {
		content := evt.Content.AsMember()
		if *autojoin && content.Membership == event.MembershipInvite && evt.GetStateKey() == client.UserID.String() {
			_, err := client.JoinRoomByID(ctx, evt.RoomID)
			out(map[string]any{"type": "autojoin", "room": evt.RoomID, "ok": err == nil})
		}
	})
	helper.DecryptErrorCallback = func(evt *event.Event, err error) {
		out(map[string]any{"type": "utd", "room": evt.RoomID, "id": evt.ID, "error": err.Error()})
	}

	go func() {
		for {
			if err := client.SyncWithContext(ctx); err != nil {
				out(map[string]any{"type": "sync_error", "error": err.Error()})
				time.Sleep(2 * time.Second)
			}
		}
	}()

	scanner := bufio.NewScanner(os.Stdin)
	scanner.Buffer(make([]byte, 1024*1024), 1024*1024)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		if line == "quit" {
			break
		}

		res, err := p.command(ctx, line)
		r := map[string]any{"type": "result", "cmd": strings.Fields(line)[0]}
		if err != nil {
			r["error"] = err.Error()
		}
		for k, v := range res {
			r[k] = v
		}
		out(r)
	}

	client.StopSync()
	_ = helper.Close()
}

func (p *peer) download(ctx context.Context, content *event.MessageEventContent) ([]byte, error) {
	urlStr := content.URL
	if content.File != nil {
		urlStr = content.File.URL
	}

	uri, err := urlStr.Parse()
	if err != nil {
		return nil, err
	}

	data, err := p.client.DownloadBytes(ctx, uri)
	if err != nil {
		return nil, err
	}

	if content.File != nil {
		if err := content.File.DecryptInPlace(data); err != nil {
			return nil, err
		}
	}

	return data, nil
}

func (p *peer) command(ctx context.Context, line string) (map[string]any, error) {
	f := strings.Fields(line)
	arg := func(i int) string {
		if i < len(f) {
			return f[i]
		}
		return ""
	}
	rest := func(i int) string {
		if i >= len(f) {
			return ""
		}
		return strings.Join(f[i:], " ")
	}
	cli := p.client

	switch f[0] {
	case "create":
		req := &mautrix.ReqCreateRoom{Preset: "private_chat", Invite: []id.UserID{id.UserID(arg(1))}}
		if arg(2) == "encrypted" {
			req.InitialState = []*event.Event{{Type: event.StateEncryption,
				Content: event.Content{Parsed: &event.EncryptionEventContent{Algorithm: id.AlgorithmMegolmV1}}}}
		}
		if arg(3) != "" {
			req.Name = rest(3)
		}
		resp, err := cli.CreateRoom(ctx, req)
		if err != nil {
			return nil, err
		}
		return map[string]any{"room": resp.RoomID}, nil

	case "join":
		_, err := cli.JoinRoomByID(ctx, id.RoomID(arg(1)))
		return nil, err

	case "send", "reply", "edit":
		var content event.MessageEventContent
		switch f[0] {
		case "send":
			content = format.RenderMarkdown(rest(2), true, false)
		case "reply":
			content = format.RenderMarkdown(rest(3), true, false)
			content.RelatesTo = &event.RelatesTo{InReplyTo: &event.InReplyTo{EventID: id.EventID(arg(2))}}
		case "edit":
			content = format.RenderMarkdown(rest(3), true, false)
			content.SetEdit(id.EventID(arg(2)))
		}
		resp, err := cli.SendMessageEvent(ctx, id.RoomID(arg(1)), event.EventMessage, &content)
		if err != nil {
			return nil, err
		}
		return map[string]any{"event": resp.EventID}, nil

	case "react":
		resp, err := cli.SendReaction(ctx, id.RoomID(arg(1)), id.EventID(arg(2)), arg(3))
		if err != nil {
			return nil, err
		}
		return map[string]any{"event": resp.EventID}, nil

	case "redact":
		resp, err := cli.RedactEvent(ctx, id.RoomID(arg(1)), id.EventID(arg(2)))
		if err != nil {
			return nil, err
		}
		return map[string]any{"event": resp.EventID}, nil

	case "topic":
		_, err := cli.SendStateEvent(ctx, id.RoomID(arg(1)), event.StateTopic, "", &event.TopicEventContent{Topic: rest(2)})
		return nil, err

	case "typing":
		_, err := cli.UserTyping(ctx, id.RoomID(arg(1)), arg(2) == "on", 20*time.Second)
		return nil, err

	case "read":
		err := cli.SendReceipt(ctx, id.RoomID(arg(1)), id.EventID(arg(2)), event.ReceiptTypeRead, nil)
		return nil, err

	case "sendfile":
		data, err := os.ReadFile(arg(2))
		if err != nil {
			return nil, err
		}
		mimeType := http.DetectContentType(data)
		content := event.MessageEventContent{MsgType: event.MsgFile, Body: filepath.Base(arg(2)),
			FileName: filepath.Base(arg(2)), Info: &event.FileInfo{MimeType: mimeType, Size: len(data)}}
		if strings.HasPrefix(mimeType, "image/") {
			content.MsgType = event.MsgImage
		}
		encrypted, _ := cli.StateStore.IsEncrypted(ctx, id.RoomID(arg(1)))
		uploadType := mimeType
		var ef *attachment.EncryptedFile
		if encrypted {
			ef = attachment.NewEncryptedFile()
			ef.EncryptInPlace(data)
			uploadType = "application/octet-stream"
		}
		up, err := cli.UploadMedia(ctx, mautrix.ReqUploadMedia{ContentBytes: data, ContentType: uploadType})
		if err != nil {
			return nil, err
		}
		if ef != nil {
			content.File = &event.EncryptedFileInfo{EncryptedFile: *ef, URL: up.ContentURI.CUString()}
		} else {
			content.URL = up.ContentURI.CUString()
		}
		resp, err := cli.SendMessageEvent(ctx, id.RoomID(arg(1)), event.EventMessage, &content)
		if err != nil {
			return nil, err
		}
		return map[string]any{"event": resp.EventID}, nil

	case "bootstrap":
		return p.bootstrap(ctx)

	case "backup":
		count, err := p.uploadBackup(ctx)
		return map[string]any{"count": count}, err

	case "verified":
		userID := id.UserID(arg(1))
		deviceID := id.DeviceID(arg(2))
		resp, err := cli.QueryKeys(ctx, &mautrix.ReqQueryKeys{DeviceKeys: map[id.UserID]mautrix.DeviceIDList{userID: {deviceID}}})
		if err != nil {
			return nil, err
		}
		signed := false
		if ssk, ok := resp.SelfSigningKeys[userID]; ok {
			if dev, ok := resp.DeviceKeys[userID][deviceID]; ok {
				for keyID := range ssk.Keys {
					if _, ok := dev.Signatures[userID][keyID]; ok {
						signed = true
					}
				}
			}
		}
		return map[string]any{"signed": signed}, nil

	case "logout":
		_, err := cli.Logout(ctx)
		return nil, err

	case "sleep":
		secs, _ := strconv.ParseFloat(arg(1), 64)
		time.Sleep(time.Duration(secs * float64(time.Second)))
		return nil, nil
	}

	return nil, fmt.Errorf("unknown command")
}

// bootstrap sets up cross-signing, secret storage and key backup, as Element
// does on first login.
func (p *peer) bootstrap(ctx context.Context) (map[string]any, error) {
	mach := p.helper.Machine()
	recoveryKey, _, err := mach.GenerateAndUploadCrossSigningKeysWithPassword(ctx, p.password, "")
	if err != nil {
		return nil, fmt.Errorf("cross-signing: %w", err)
	}

	if err := mach.SignOwnDevice(ctx, mach.OwnIdentity()); err != nil {
		return nil, fmt.Errorf("sign device: %w", err)
	}

	if err := mach.SignOwnMasterKey(ctx); err != nil {
		return nil, fmt.Errorf("sign master: %w", err)
	}

	keyID, keyData, err := mach.SSSS.GetDefaultKeyData(ctx)
	if err != nil {
		return nil, err
	}

	ssssKey, err := keyData.VerifyRecoveryKey(keyID, recoveryKey)
	if err != nil {
		return nil, err
	}

	backupKey, err := backup.NewMegolmBackupKey()
	if err != nil {
		return nil, err
	}

	resp, err := p.client.CreateKeyBackupVersion(ctx, &mautrix.ReqRoomKeysVersionCreate[backup.MegolmAuthData]{
		Algorithm: id.KeyBackupAlgorithmMegolmBackupV1,
		AuthData: backup.MegolmAuthData{
			PublicKey: id.Ed25519(base64.RawStdEncoding.EncodeToString(backupKey.PublicKey().Bytes())),
		},
	})
	if err != nil {
		return nil, fmt.Errorf("create backup: %w", err)
	}

	if err := mach.SSSS.SetEncryptedAccountData(ctx, event.AccountDataMegolmBackupKey, backupKey.Bytes(), ssssKey); err != nil {
		return nil, fmt.Errorf("store backup key: %w", err)
	}

	p.backupKey = backupKey
	p.backupVer = resp.Version
	count, err := p.uploadBackup(ctx)
	return map[string]any{"recovery_key": recoveryKey, "backup_version": resp.Version, "count": count}, err
}

func (p *peer) uploadBackup(ctx context.Context) (int, error) {
	if p.backupKey == nil {
		return 0, fmt.Errorf("no backup, run bootstrap")
	}

	mach := p.helper.Machine()
	sessions, err := mach.CryptoStore.GetGroupSessionsWithoutKeyBackupVersion(ctx, p.backupVer).AsList()
	if err != nil {
		return 0, err
	}

	count := 0
	for _, igs := range sessions {
		first := igs.Internal.FirstKnownIndex()
		exported, err := igs.Internal.Export(first)
		if err != nil {
			continue
		}
		data := backup.MegolmSessionData{
			Algorithm:          id.AlgorithmMegolmV1,
			ForwardingKeyChain: []string{},
			SenderClaimedKeys:  backup.SenderClaimedKeys{Ed25519: igs.SigningKey},
			SenderKey:          igs.SenderKey,
			SessionKey:         string(exported),
		}
		enc, err := backup.EncryptSessionData(p.backupKey, data)
		if err != nil {
			continue
		}
		encJSON, _ := json.Marshal(enc)
		_, err = p.client.PutKeysInBackupForRoomAndSession(ctx, p.backupVer, igs.RoomID, igs.ID(), &mautrix.ReqKeyBackupData{
			FirstMessageIndex: int(first), SessionData: encJSON,
		})
		if err != nil {
			return count, err
		}
		igs.KeyBackupVersion = p.backupVer
		_ = mach.CryptoStore.PutGroupSession(ctx, igs)
		count++
	}

	return count, nil
}
