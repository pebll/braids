// gomx_crypto.go
//
// Copyright (c) 2026 nchat contributors
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

package main

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"time"

	"maunium.net/go/mautrix"
	"maunium.net/go/mautrix/crypto"
	"maunium.net/go/mautrix/crypto/backup"
	"maunium.net/go/mautrix/crypto/ssss"
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/id"
)

// resolveEncrypted returns the decrypted event, or nil if keys are missing.
func (conn *MxConn) resolveEncrypted(ctx context.Context, evt *event.Event) *event.Event {
	conn.mu.Lock()
	dec, ok := conn.decrypted[evt.ID]
	conn.mu.Unlock()
	if ok {
		return dec
	}

	if conn.crypto == nil {
		return nil
	}

	if _, ok := evt.Content.Parsed.(*event.EncryptedEventContent); !ok {
		if err := evt.Content.ParseRaw(evt.Type); err != nil && !errors.Is(err, event.ErrContentAlreadyParsed) {
			return nil
		}
	}

	dec, err := conn.crypto.Decrypt(ctx, evt)
	if err != nil {
		LOG_DEBUG(fmt.Sprintf("decrypt %s failed: %v", evt.ID, err))
		return nil
	}

	return dec
}

// onPostDecrypt is called by cryptohelper for events decrypted during sync
// (synchronously) or after waiting for keys (asynchronously).
func (conn *MxConn) onPostDecrypt(ctx context.Context, evt *event.Event) {
	conn.mu.Lock()
	shown := conn.utdShown[evt.ID]
	if !shown {
		conn.decrypted[evt.ID] = evt
	}
	conn.mu.Unlock()

	if shown {
		conn.handleLateDecrypted(ctx, evt)
	}
}

// addPending records an undecryptable event for later retry, and asks for the
// keys (other devices, server-side backup).
func (conn *MxConn) addPending(ctx context.Context, evt *event.Event) {
	content, ok := evt.Content.Parsed.(*event.EncryptedEventContent)
	if !ok {
		return
	}

	conn.mu.Lock()
	conn.state.Pending[evt.ID] = evt
	conn.stateDirty = true
	alreadyRequested := conn.requested[content.SessionID]
	conn.requested[content.SessionID] = true
	conn.mu.Unlock()

	if alreadyRequested || conn.crypto == nil {
		return
	}

	go func() {
		if conn.fetchSessionFromBackup(evt.RoomID, content.SessionID) {
			return
		}

		conn.crypto.RequestSession(context.Background(), evt.RoomID, content.SenderKey, content.SessionID,
			evt.Sender, content.DeviceID)
	}()
}

func (conn *MxConn) onSessionReceived(ctx context.Context, roomID id.RoomID, sessionID id.SessionID,
	firstKnownIndex uint32) {
	go func() {
		conn.retryPending(sessionID)
		conn.uploadSessionToBackup(roomID, sessionID)
	}()
}

// retryPending re-decrypts pending events of a session and updates the
// placeholder messages in place (same message id).
func (conn *MxConn) retryPending(sessionID id.SessionID) {
	var evts []*event.Event
	conn.mu.Lock()
	for _, evt := range conn.state.Pending {
		content, ok := evt.Content.Parsed.(*event.EncryptedEventContent)
		if ok && content.SessionID == sessionID {
			evts = append(evts, evt)
		}
	}
	conn.mu.Unlock()

	ctx := context.Background()
	for _, evt := range evts {
		dec := conn.resolveEncrypted(ctx, evt)
		if dec == nil {
			continue
		}

		conn.handleLateDecrypted(ctx, dec)
	}
}

func (conn *MxConn) retryAllPending() {
	sessions := make(map[id.SessionID]bool)
	conn.mu.Lock()
	for _, evt := range conn.state.Pending {
		if content, ok := evt.Content.Parsed.(*event.EncryptedEventContent); ok {
			sessions[content.SessionID] = true
		}
	}
	conn.mu.Unlock()

	for sessionID := range sessions {
		conn.retryPending(sessionID)
	}
}

func (conn *MxConn) handleLateDecrypted(ctx context.Context, dec *event.Event) {
	conn.mu.Lock()
	_, pending := conn.state.Pending[dec.ID]
	delete(conn.state.Pending, dec.ID)
	delete(conn.utdShown, dec.ID)
	conn.stateDirty = true
	conn.mu.Unlock()

	LOG_DEBUG(fmt.Sprintf("late decrypt %s pending %v", dec.ID, pending))
	msg := conn.handleTimelineEvent(ctx, dec, modeLate)
	if msg == nil || msg.MsgID != string(dec.ID) {
		// not a plain message (edit, reaction, ...), remove the placeholder
		CMxDeleteMessageNotify(conn.connId, string(dec.RoomID), string(dec.ID), 1)
	}

	if msg != nil {
		if msg.SenderID != "" {
			conn.updateContact(msg.SenderID, conn.userName(ctx, dec.RoomID, id.UserID(msg.SenderID)))
		}

		conn.emitMessage(msg, 0)
	}

	conn.flushReactions()
	conn.saveState(false)
}

// loadBackupKey restores the key backup key stored after recovery.
func (conn *MxConn) loadBackupKey(ctx context.Context) {
	mach := conn.crypto.Machine()
	secret, err := mach.CryptoStore.GetSecret(ctx, id.SecretMegolmBackupV1)
	if err != nil || secret == "" {
		return
	}

	keyBytes, err := base64.StdEncoding.DecodeString(secret)
	if err != nil {
		return
	}

	backupKey, err := backup.MegolmBackupKeyFromBytes(keyBytes)
	if err != nil {
		return
	}

	versionInfo, err := mach.GetAndVerifyLatestKeyBackupVersion(ctx, backupKey)
	if err != nil || versionInfo == nil {
		LOG_WARNING(fmt.Sprintf("key backup not usable: %v", err))
		return
	}

	conn.mu.Lock()
	conn.backupKey = backupKey
	conn.backupVersion = versionInfo.Version
	conn.mu.Unlock()
	LOG_INFO(fmt.Sprintf("key backup version %s enabled", versionInfo.Version))
}

// fetchSessionFromBackup downloads a single session from key backup. Returns
// true if the session was imported.
func (conn *MxConn) fetchSessionFromBackup(roomID id.RoomID, sessionID id.SessionID) bool {
	conn.mu.Lock()
	backupKey := conn.backupKey
	version := conn.backupVersion
	conn.mu.Unlock()
	if backupKey == nil || version == "" {
		return false
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	mach := conn.crypto.Machine()
	keyData, err := conn.client.GetKeyBackupForRoomAndSession(ctx, version, roomID, sessionID)
	if err != nil || keyData == nil {
		return false
	}

	sessionData, err := keyData.SessionData.Decrypt(backupKey)
	if err != nil {
		LOG_WARNING(fmt.Sprintf("backup session decrypt failed: %v", err))
		return false
	}

	_, err = mach.ImportRoomKeyFromBackup(ctx, version, roomID, sessionID, sessionData)
	if err != nil {
		LOG_WARNING(fmt.Sprintf("backup session import failed: %v", err))
		return false
	}

	return true
}

// uploadSessionToBackup uploads a (new) inbound session to key backup, if
// backup is enabled and the session is not already backed up.
func (conn *MxConn) uploadSessionToBackup(roomID id.RoomID, sessionID id.SessionID) {
	conn.mu.Lock()
	backupKey := conn.backupKey
	version := conn.backupVersion
	conn.mu.Unlock()
	if backupKey == nil || version == "" || conn.crypto == nil {
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	mach := conn.crypto.Machine()
	igs, err := mach.CryptoStore.GetGroupSession(ctx, roomID, sessionID)
	if err != nil || igs == nil {
		return
	}

	if err := conn.uploadGroupSession(ctx, igs, backupKey, version); err != nil {
		LOG_WARNING(fmt.Sprintf("backup upload failed: %v", err))
	}
}

func (conn *MxConn) uploadGroupSession(ctx context.Context, igs *crypto.InboundGroupSession,
	backupKey *backup.MegolmBackupKey, version id.KeyBackupVersion) error {
	if igs.KeySource == id.KeySourceBackup || igs.KeyBackupVersion == version {
		return nil
	}

	firstIndex := igs.Internal.FirstKnownIndex()
	exported, err := igs.Internal.Export(firstIndex)
	if err != nil {
		return err
	}

	sessionData := backup.MegolmSessionData{
		Algorithm:          id.AlgorithmMegolmV1,
		ForwardingKeyChain: igs.ForwardingChains,
		SenderClaimedKeys:  backup.SenderClaimedKeys{Ed25519: igs.SigningKey},
		SenderKey:          igs.SenderKey,
		SessionKey:         string(exported),
		SharedHistory:      igs.SharedHistory,
	}
	if sessionData.ForwardingKeyChain == nil {
		sessionData.ForwardingKeyChain = []string{}
	}

	encrypted, err := backup.EncryptSessionData(backupKey, sessionData)
	if err != nil {
		return err
	}

	encryptedJSON, err := json.Marshal(encrypted)
	if err != nil {
		return err
	}

	_, err = conn.client.PutKeysInBackupForRoomAndSession(ctx, version, igs.RoomID, igs.ID(), &mautrix.ReqKeyBackupData{
		FirstMessageIndex: int(firstIndex),
		ForwardedCount:    len(igs.ForwardingChains),
		IsVerified:        false,
		SessionData:       encryptedJSON,
	})
	if err != nil {
		return err
	}

	igs.KeyBackupVersion = version
	return conn.crypto.Machine().CryptoStore.PutGroupSession(ctx, igs)
}

// uploadAllSessions backs up all local sessions not yet in the given backup.
func (conn *MxConn) uploadAllSessions(backupKey *backup.MegolmBackupKey, version id.KeyBackupVersion) {
	ctx := context.Background()
	mach := conn.crypto.Machine()
	sessions, err := mach.CryptoStore.GetGroupSessionsWithoutKeyBackupVersion(ctx, version).AsList()
	if err != nil {
		LOG_WARNING(fmt.Sprintf("list sessions for backup failed: %v", err))
		return
	}

	count := 0
	for _, igs := range sessions {
		if err := conn.uploadGroupSession(ctx, igs, backupKey, version); err != nil {
			LOG_WARNING(fmt.Sprintf("backup upload failed: %v", err))
		} else {
			count++
		}
	}

	LOG_INFO(fmt.Sprintf("uploaded %d sessions to key backup", count))
}

// isOwnDeviceCrossSigned checks on the server whether this device is signed
// by the account's self-signing key.
func (conn *MxConn) isOwnDeviceCrossSigned(ctx context.Context) (bool, error) {
	selfId := conn.session.UserID
	resp, err := conn.client.QueryKeys(ctx, &mautrix.ReqQueryKeys{
		DeviceKeys: map[id.UserID]mautrix.DeviceIDList{selfId: {conn.session.DeviceID}},
	})
	if err != nil {
		return false, err
	}

	selfSigning, ok := resp.SelfSigningKeys[selfId]
	if !ok {
		return false, nil
	}

	device, ok := resp.DeviceKeys[selfId][conn.session.DeviceID]
	if !ok {
		return false, nil
	}

	for keyID := range selfSigning.Keys {
		if _, signed := device.Signatures[selfId][keyID]; signed {
			return true, nil
		}
	}

	return false, nil
}

// MxNeedsRecovery returns 1 if the account has secret storage and this device
// is not yet cross-signed, or key backup is not yet enabled.
func MxNeedsRecovery(connId int) int {
	conn := GetConn(connId)
	if conn == nil || !conn.isReady() {
		return 0
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	mach := conn.crypto.Machine()
	if _, err := mach.SSSS.GetDefaultKeyID(ctx); err != nil {
		LOG_DEBUG("no secret storage on account")
		return 0
	}

	signed, err := conn.isOwnDeviceCrossSigned(ctx)
	if err != nil {
		LOG_WARNING(fmt.Sprintf("device verification check failed: %v", err))
		return 0
	}

	conn.mu.Lock()
	hasBackup := (conn.backupKey != nil)
	conn.mu.Unlock()

	return BoolToInt(!signed || !hasBackup)
}

func (conn *MxConn) maybePromptRecovery() {
	if MxNeedsRecovery(conn.connId) == 1 {
		CMxRecoveryPrompt(conn.connId)
	}
}

// MxRecover uses a recovery key (or security passphrase) to fetch cross-signing
// keys from secret storage, sign this device, and enable key backup. Returns
// an empty string on success, otherwise an error message.
func MxRecover(connId int, input string) string {
	conn := GetConn(connId)
	if conn == nil {
		return "invalid connection"
	}

	if !conn.isReady() {
		return "not connected"
	}

	input = strings.TrimSpace(input)
	if input == "" {
		return "empty key"
	}

	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()
	mach := conn.crypto.Machine()

	keyID, keyData, err := mach.SSSS.GetDefaultKeyData(ctx)
	if err != nil {
		return "no secret storage (recovery) is set up on this account"
	}

	key, err := keyData.VerifyRecoveryKey(keyID, input)
	if err != nil && !errors.Is(err, ssss.ErrUnverifiableKey) && keyData.Passphrase != nil {
		key, err = keyData.VerifyPassphrase(keyID, input)
	}

	if errors.Is(err, ssss.ErrUnverifiableKey) {
		LOG_WARNING("ssss key is unverifiable, trying anyway")
	} else if err != nil {
		return "invalid recovery key or passphrase"
	}

	// cross-signing: fetch private keys and sign own device and master key
	if err := mach.FetchCrossSigningKeysFromSSSS(ctx, key); err != nil {
		LOG_WARNING(fmt.Sprintf("fetch cross-signing keys failed: %v", err))
		return "failed to fetch cross-signing keys from secret storage"
	}

	if err := mach.SignOwnDevice(ctx, mach.OwnIdentity()); err != nil {
		LOG_WARNING(fmt.Sprintf("sign own device failed: %v", err))
		return "failed to sign this device"
	}

	if err := mach.SignOwnMasterKey(ctx); err != nil {
		LOG_WARNING(fmt.Sprintf("sign own master key failed: %v", err))
	}

	LOG_INFO("device cross-signed")

	// key backup
	backupKeyBytes, err := mach.SSSS.GetDecryptedAccountData(ctx, event.AccountDataMegolmBackupKey, key)
	if err != nil {
		LOG_INFO(fmt.Sprintf("no key backup key in secret storage: %v", err))
		return ""
	}

	backupKey, err := backup.MegolmBackupKeyFromBytes(backupKeyBytes)
	if err != nil {
		LOG_WARNING(fmt.Sprintf("invalid backup key: %v", err))
		return ""
	}

	versionInfo, err := mach.GetAndVerifyLatestKeyBackupVersion(ctx, backupKey)
	if err != nil || versionInfo == nil {
		LOG_WARNING(fmt.Sprintf("key backup verification failed: %v", err))
		return ""
	}

	if err := mach.CryptoStore.PutSecret(ctx, id.SecretMegolmBackupV1,
		base64.StdEncoding.EncodeToString(backupKeyBytes)); err != nil {
		LOG_WARNING(fmt.Sprintf("store backup key failed: %v", err))
	}

	conn.mu.Lock()
	conn.backupKey = backupKey
	conn.backupVersion = versionInfo.Version
	conn.mu.Unlock()
	LOG_INFO(fmt.Sprintf("key backup version %s enabled", versionInfo.Version))

	// restore old keys and back up local ones in the background
	go func() {
		bgCtx := context.Background()
		if err := mach.GetAndStoreKeyBackup(bgCtx, versionInfo.Version, backupKey); err != nil {
			LOG_WARNING(fmt.Sprintf("key backup restore failed: %v", err))
		}

		conn.retryAllPending()
		conn.uploadAllSessions(backupKey, versionInfo.Version)
	}()

	return ""
}
