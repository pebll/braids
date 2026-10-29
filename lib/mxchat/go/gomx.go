// gomx.go
//
// Copyright (c) 2026 nchat contributors
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

package main

import (
	"context"
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/rs/zerolog"
	"maunium.net/go/mautrix"
	"maunium.net/go/mautrix/crypto/backup"
	"maunium.net/go/mautrix/crypto/cryptohelper"
	"maunium.net/go/mautrix/event"
	"maunium.net/go/mautrix/id"
	"maunium.net/go/mautrix/pushrules"
)

// keep in sync with go.mod
var mautrixVersion = "v0.32.0"

// keep in sync with enum FileStatus in protocol.h
var FileStatusNone = -1
var FileStatusNotDownloaded = 0
var FileStatusDownloaded = 1
var FileStatusDownloading = 2
var FileStatusDownloadFailed = 3

// keep in sync with enum Flag in status.h
var FlagNone = 0
var FlagOffline = (1 << 0)
var FlagConnecting = (1 << 1)
var FlagOnline = (1 << 2)
var FlagFetching = (1 << 3)
var FlagSending = (1 << 4)
var FlagUpdating = (1 << 5)
var FlagSyncing = (1 << 6)
var FlagAway = (1 << 7)

// keep in sync with enum NotifyType in mxchat.cpp
var NotifyDirect = 0
var NotifyCache = 1
var NotifySendCached = 2

// persisted state version, bump upon incompatible change
var stateVersion = 1

// Session holds the login credentials, stored in session.json (mode 0600).
type Session struct {
	Homeserver  string      `json:"homeserver"`
	UserID      id.UserID   `json:"user_id"`
	DeviceID    id.DeviceID `json:"device_id"`
	AccessToken string      `json:"access_token"`
}

// RoomInfo holds per-room data needed by nchat that the mautrix state store
// does not keep (names, unread state, receipts).
type RoomInfo struct {
	Name        string      `json:"name,omitempty"`
	Alias       string      `json:"alias,omitempty"`
	Heroes      []id.UserID `json:"heroes,omitempty"`
	Joined      int         `json:"joined,omitempty"`
	Invited     int         `json:"invited,omitempty"`
	IsDirect    bool        `json:"direct,omitempty"`
	IsInvite    bool        `json:"invite,omitempty"`
	Inviter     id.UserID   `json:"inviter,omitempty"`
	LastTs      int64       `json:"last_ts,omitempty"`
	Unread      bool        `json:"unread,omitempty"`
	Muted       bool        `json:"muted,omitempty"`
	Pinned      bool        `json:"pinned,omitempty"`
	OwnReadTs   int64       `json:"own_read_ts,omitempty"`
	OtherReadTs int64       `json:"other_read_ts,omitempty"`
}

// ReactionRef maps a reaction event to its target, needed to handle redaction
// (removal) of reactions.
type ReactionRef struct {
	RoomID id.RoomID  `json:"room_id"`
	Target id.EventID `json:"target"`
	Sender id.UserID  `json:"sender"`
	Key    string     `json:"key"`
}

// PersistState is stored in state.json (mode 0600). It holds no secrets and no
// message plaintext; pending holds still-encrypted events awaiting keys.
type PersistState struct {
	Version   int                         `json:"version"`
	Rooms     map[id.RoomID]*RoomInfo     `json:"rooms"`
	Names     map[id.UserID]string        `json:"names"`
	Reactions map[id.EventID]*ReactionRef `json:"reactions"`
	Pending   map[id.EventID]*event.Event `json:"pending"`
}

type OwnMsg struct {
	ID id.EventID
	Ts int64
}

type MxConn struct {
	connId int
	path   string

	mu         sync.Mutex
	session    *Session
	client     *mautrix.Client
	crypto     *cryptohelper.CryptoHelper
	ctx        context.Context
	cancel     context.CancelFunc
	runDone    chan struct{}
	ready      chan struct{}
	readyOnce  sync.Once
	running    bool
	firstSync  bool
	state      *PersistState
	stateDirty bool

	directs   map[id.RoomID]id.UserID
	pushRules *pushrules.PushRuleset
	typing    map[id.RoomID]map[id.UserID]bool
	decrypted map[id.EventID]*event.Event
	utdShown  map[id.EventID]bool
	ownMsgs   map[id.RoomID][]OwnMsg
	contacts  map[string]string
	requested map[id.SessionID]bool
	sentFiles map[id.EventID]string

	reactionQueue []queuedReaction

	backupKey     *backup.MegolmBackupKey
	backupVersion id.KeyBackupVersion
}

var connsMutex sync.Mutex
var conns = make(map[int]*MxConn)
var nextConnId = 0

func GetConn(connId int) *MxConn {
	connsMutex.Lock()
	defer connsMutex.Unlock()
	return conns[connId]
}

func sessionPath(path string) string   { return path + "/session.json" }
func pickleKeyPath(path string) string { return path + "/pickle.key" }
func cryptoDbPath(path string) string  { return path + "/crypto.db" }
func statePath(path string) string     { return path + "/state.json" }
func tmpPath(path string) string       { return path + "/tmp" }

// writePrivateFile atomically writes a file readable by the owner only.
func writePrivateFile(p string, data []byte) error {
	tmp := p + ".tmp"
	if err := os.WriteFile(tmp, data, 0600); err != nil {
		return err
	}

	if err := os.Chmod(tmp, 0600); err != nil {
		return err
	}

	return os.Rename(tmp, p)
}

func BoolToInt(b bool) int {
	if b {
		return 1
	}

	return 0
}

func MxGetVersion() string {
	return mautrixVersion
}

func MxInit(path string) int {
	conn := &MxConn{
		connId:    -1,
		path:      path,
		ready:     make(chan struct{}),
		directs:   make(map[id.RoomID]id.UserID),
		typing:    make(map[id.RoomID]map[id.UserID]bool),
		decrypted: make(map[id.EventID]*event.Event),
		utdShown:  make(map[id.EventID]bool),
		ownMsgs:   make(map[id.RoomID][]OwnMsg),
		contacts:  make(map[string]string),
		requested: make(map[id.SessionID]bool),
		sentFiles: make(map[id.EventID]string),
	}

	_ = os.MkdirAll(path, 0700)
	_ = os.MkdirAll(tmpPath(path), 0700)
	conn.loadState()

	connsMutex.Lock()
	conn.connId = nextConnId
	nextConnId++
	conns[conn.connId] = conn
	connsMutex.Unlock()

	LOG_DEBUG(fmt.Sprintf("init conn %d mautrix %s", conn.connId, mautrixVersion))
	return conn.connId
}

func MxCleanup(connId int) int {
	conn := GetConn(connId)
	if conn == nil {
		return -1
	}

	conn.stop()
	conn.saveState(true)
	if conn.crypto != nil {
		_ = conn.crypto.Close()
		conn.crypto = nil
	}

	connsMutex.Lock()
	delete(conns, connId)
	connsMutex.Unlock()
	return 0
}

func (conn *MxConn) loadState() {
	st := &PersistState{}
	data, err := os.ReadFile(statePath(conn.path))
	if err == nil {
		if jerr := json.Unmarshal(data, st); jerr != nil || st.Version != stateVersion {
			LOG_WARNING("ignoring incompatible state file")
			st = &PersistState{}
		}
	}

	st.Version = stateVersion
	if st.Rooms == nil {
		st.Rooms = make(map[id.RoomID]*RoomInfo)
	}
	if st.Names == nil {
		st.Names = make(map[id.UserID]string)
	}
	if st.Reactions == nil {
		st.Reactions = make(map[id.EventID]*ReactionRef)
	}
	if st.Pending == nil {
		st.Pending = make(map[id.EventID]*event.Event)
	}

	for _, evt := range st.Pending {
		_ = evt.Content.ParseRaw(evt.Type)
	}

	conn.state = st
	for roomID, ri := range st.Rooms {
		if ri.IsDirect && len(ri.Heroes) > 0 {
			conn.directs[roomID] = ri.Heroes[0]
		}
	}
}

// saveState writes state.json if dirty (or always if force). Caller must not
// hold conn.mu.
func (conn *MxConn) saveState(force bool) {
	conn.mu.Lock()
	if !conn.stateDirty && !force {
		conn.mu.Unlock()
		return
	}

	// cap number of tracked reactions
	const maxReactions = 20000
	if len(conn.state.Reactions) > maxReactions {
		for k := range conn.state.Reactions {
			delete(conn.state.Reactions, k)
			if len(conn.state.Reactions) <= (maxReactions * 3 / 4) {
				break
			}
		}
	}

	data, err := json.Marshal(conn.state)
	conn.stateDirty = false
	conn.mu.Unlock()

	if err != nil {
		LOG_WARNING(fmt.Sprintf("state marshal failed %v", err))
		return
	}

	if err := writePrivateFile(statePath(conn.path), data); err != nil {
		LOG_WARNING(fmt.Sprintf("state write failed %v", err))
	}
}

// getRoom returns room info, creating it if needed. Caller must hold conn.mu.
func (conn *MxConn) getRoom(roomID id.RoomID) *RoomInfo {
	ri, ok := conn.state.Rooms[roomID]
	if !ok {
		ri = &RoomInfo{}
		conn.state.Rooms[roomID] = ri
	}

	return ri
}

// MxNormalizeUserId turns "alice:example.org", "@alice:example.org" into a
// canonical mxid. Returns empty string if invalid.
func MxNormalizeUserId(userId string) string {
	userId = strings.TrimSpace(userId)
	if userId == "" {
		return ""
	}

	if !strings.HasPrefix(userId, "@") {
		userId = "@" + userId
	}

	_, server, err := id.UserID(userId).Parse()
	if err != nil || server == "" {
		return ""
	}

	return userId
}

// MxDiscoverHomeserver resolves the homeserver base URL for a user id via
// .well-known, falling back to https://<server>.
func MxDiscoverHomeserver(userId string) string {
	_, server, err := id.UserID(userId).Parse()
	if err != nil || server == "" {
		return ""
	}

	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	wellKnown, err := mautrix.DiscoverClientAPI(ctx, server)
	if err == nil && wellKnown != nil && wellKnown.Homeserver.BaseURL != "" {
		return strings.TrimRight(wellKnown.Homeserver.BaseURL, "/")
	}

	if err != nil {
		LOG_DEBUG(fmt.Sprintf("well-known lookup failed for %s: %v", server, err))
	}

	return "https://" + server
}

// MxSetupLogin performs password login and stores session and pickle key.
// Returns an empty string on success, otherwise an error message.
func MxSetupLogin(connId int, homeserver string, userId string, password string) string {
	conn := GetConn(connId)
	if conn == nil {
		return "invalid connection"
	}

	client, err := mautrix.NewClient(homeserver, "", "")
	if err != nil {
		return fmt.Sprintf("invalid homeserver url: %v", err)
	}
	client.Log = newLogger()

	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	hostname, _ := os.Hostname()
	deviceName := "nchat"
	if hostname != "" {
		deviceName = "nchat (" + hostname + ")"
	}

	resp, err := client.Login(ctx, &mautrix.ReqLogin{
		Type:                     mautrix.AuthTypePassword,
		Identifier:               mautrix.UserIdentifier{Type: mautrix.IdentifierTypeUser, User: userId},
		Password:                 password,
		InitialDeviceDisplayName: deviceName,
	})
	if err != nil {
		var httpErr mautrix.HTTPError
		if errors.As(err, &httpErr) && httpErr.RespError != nil {
			return fmt.Sprintf("login failed: %s", httpErr.RespError.Err)
		}

		return fmt.Sprintf("login failed: %v", err)
	}

	if resp.WellKnown != nil && resp.WellKnown.Homeserver.BaseURL != "" {
		homeserver = strings.TrimRight(resp.WellKnown.Homeserver.BaseURL, "/")
	}

	session := &Session{
		Homeserver:  homeserver,
		UserID:      resp.UserID,
		DeviceID:    resp.DeviceID,
		AccessToken: resp.AccessToken,
	}

	data, _ := json.Marshal(session)
	if err := writePrivateFile(sessionPath(conn.path), data); err != nil {
		return fmt.Sprintf("failed to store session: %v", err)
	}

	pickleKey := make([]byte, 32)
	if _, err := rand.Read(pickleKey); err != nil {
		return fmt.Sprintf("failed to generate pickle key: %v", err)
	}

	if err := writePrivateFile(pickleKeyPath(conn.path), pickleKey); err != nil {
		return fmt.Sprintf("failed to store pickle key: %v", err)
	}

	LOG_INFO(fmt.Sprintf("logged in as %s device %s", resp.UserID, resp.DeviceID))
	return ""
}

func loadSession(path string) (*Session, error) {
	data, err := os.ReadFile(sessionPath(path))
	if err != nil {
		return nil, err
	}

	session := &Session{}
	if err := json.Unmarshal(data, session); err != nil {
		return nil, err
	}

	if session.AccessToken == "" || session.UserID == "" || session.DeviceID == "" {
		return nil, fmt.Errorf("incomplete session")
	}

	return session, nil
}

func MxGetSelfId(connId int) string {
	conn := GetConn(connId)
	if conn == nil {
		return ""
	}

	if conn.session == nil {
		session, err := loadSession(conn.path)
		if err != nil {
			return ""
		}

		conn.session = session
	}

	return string(conn.session.UserID)
}

func MxIsGroupChat(connId int, chatId string) int {
	conn := GetConn(connId)
	if conn == nil {
		return 1
	}

	conn.mu.Lock()
	defer conn.mu.Unlock()
	ri, ok := conn.state.Rooms[id.RoomID(chatId)]
	if !ok {
		return 1
	}

	return BoolToInt(!ri.IsDirect)
}

// MxLogin starts the connection in the background: crypto init (with retry)
// followed by the sync loop. Returns 0 if a session exists.
func MxLogin(connId int) int {
	conn := GetConn(connId)
	if conn == nil {
		return -1
	}

	conn.mu.Lock()
	if conn.running {
		conn.mu.Unlock()
		return 0
	}

	session, err := loadSession(conn.path)
	if err != nil {
		conn.mu.Unlock()
		LOG_ERROR(fmt.Sprintf("no valid session: %v", err))
		return -1
	}

	pickleKey, err := os.ReadFile(pickleKeyPath(conn.path))
	if err != nil || len(pickleKey) == 0 {
		conn.mu.Unlock()
		LOG_ERROR("missing pickle key")
		return -1
	}

	conn.session = session
	conn.ctx, conn.cancel = context.WithCancel(context.Background())
	conn.runDone = make(chan struct{})
	conn.running = true
	conn.mu.Unlock()

	go conn.run(pickleKey)
	return 0
}

func MxLogout(connId int) int {
	conn := GetConn(connId)
	if conn == nil {
		return -1
	}

	conn.stop()
	return 0
}

func (conn *MxConn) stop() {
	conn.mu.Lock()
	if !conn.running {
		conn.mu.Unlock()
		return
	}

	conn.running = false
	cancel := conn.cancel
	client := conn.client
	runDone := conn.runDone
	conn.mu.Unlock()

	cancel()
	if client != nil {
		client.StopSync()
	}

	select {
	case <-runDone:
	case <-time.After(10 * time.Second):
		LOG_WARNING("timeout waiting for sync loop to stop")
	}

	conn.saveState(false)
}

// MxWaitReady waits until crypto is initialized. Returns 0 if ready.
func MxWaitReady(connId int, timeoutSec int) int {
	conn := GetConn(connId)
	if conn == nil {
		return -1
	}

	select {
	case <-conn.ready:
		return 0
	case <-time.After(time.Duration(timeoutSec) * time.Second):
		return -1
	}
}

func (conn *MxConn) isReady() bool {
	select {
	case <-conn.ready:
		return true
	default:
		return false
	}
}

func backoffDelay(attempt int) time.Duration {
	delay := time.Second << min(attempt, 6) // 1s .. 64s
	return min(delay, 60*time.Second)
}

func (conn *MxConn) run(pickleKey []byte) {
	defer close(conn.runDone)

	ctx := conn.ctx
	session := conn.session

	client, err := mautrix.NewClient(session.Homeserver, session.UserID, session.AccessToken)
	if err != nil {
		CMxReportError(conn.connId, fmt.Sprintf("invalid homeserver url: %v", err))
		return
	}

	client.DeviceID = session.DeviceID
	client.Log = newLogger()
	syncer := &MxSyncer{DefaultSyncer: mautrix.NewDefaultSyncer(), conn: conn}
	syncer.FilterJSON = &mautrix.Filter{
		Room: &mautrix.RoomFilter{
			Timeline: &mautrix.FilterPart{Limit: 30},
		},
	}
	client.Syncer = syncer

	helper, err := cryptohelper.NewCryptoHelper(client, pickleKey, cryptoDbPath(conn.path))
	if err != nil {
		CMxReportError(conn.connId, fmt.Sprintf("crypto setup failed: %v", err))
		return
	}

	helper.CustomPostDecrypt = conn.onPostDecrypt
	helper.DecryptErrorCallback = func(evt *event.Event, err error) {
		LOG_DEBUG(fmt.Sprintf("decrypt failed %s: %v", evt.ID, err))
	}

	conn.mu.Lock()
	conn.client = client
	conn.crypto = helper
	conn.mu.Unlock()

	// crypto init needs the network (device key check); retry with backoff
	CMxSetStatus(conn.connId, FlagConnecting)
	for attempt := 0; ; attempt++ {
		err = helper.Init(ctx)
		if err == nil {
			break
		}

		if ctx.Err() != nil {
			CMxClearStatus(conn.connId, FlagConnecting)
			return
		}

		if errors.Is(err, mautrix.MUnknownToken) {
			CMxClearStatus(conn.connId, FlagConnecting)
			conn.reportTokenInvalid()
			return
		}

		var httpErr mautrix.HTTPError
		if !errors.As(err, &httpErr) || httpErr.IsStatus(0) {
			// network error, retry
			LOG_WARNING(fmt.Sprintf("crypto init failed (attempt %d): %v", attempt+1, err))
		} else if httpErr.Response != nil && httpErr.Response.StatusCode >= 500 {
			LOG_WARNING(fmt.Sprintf("crypto init failed (attempt %d): %v", attempt+1, err))
		} else {
			CMxClearStatus(conn.connId, FlagConnecting)
			CMxReportError(conn.connId, fmt.Sprintf("crypto init failed: %v", err))
			return
		}

		select {
		case <-ctx.Done():
			CMxClearStatus(conn.connId, FlagConnecting)
			return
		case <-time.After(backoffDelay(attempt)):
		}
	}

	client.Crypto = helper
	mach := helper.Machine()
	mach.SessionReceived = conn.onSessionReceived
	conn.loadBackupKey(ctx)
	conn.readyOnce.Do(func() { close(conn.ready) })
	LOG_INFO("crypto ready")

	// pending undecryptable events from previous runs may be decryptable now
	go conn.retryAllPending()

	CMxSetStatus(conn.connId, FlagSyncing)
	for attempt := 0; ; attempt++ {
		err = client.SyncWithContext(ctx)
		if ctx.Err() != nil {
			break
		}

		if errors.Is(err, mautrix.MUnknownToken) {
			conn.reportTokenInvalid()
			break
		}

		LOG_WARNING(fmt.Sprintf("sync stopped: %v", err))
		select {
		case <-ctx.Done():
		case <-time.After(backoffDelay(attempt)):
		}

		if ctx.Err() != nil {
			break
		}
	}

	CMxClearStatus(conn.connId, FlagConnecting|FlagSyncing|FlagOnline)
	conn.saveState(false)
}

func (conn *MxConn) reportTokenInvalid() {
	CMxClearStatus(conn.connId, FlagOnline|FlagConnecting|FlagSyncing)
	CMxSetStatus(conn.connId, FlagOffline)
	CMxReportError(conn.connId, "Matrix session is no longer valid (access token expired or device "+
		"logged out). Remove and set up the profile again: nchat --remove, nchat --setup")
}

// MxSyncer wraps DefaultSyncer: crypto and state store handlers run first
// (via the embedded dispatch), then the whole response is processed by nchat.
type MxSyncer struct {
	*mautrix.DefaultSyncer
	conn      *MxConn
	failCount int
}

func (s *MxSyncer) ProcessResponse(ctx context.Context, res *mautrix.RespSync, since string) error {
	if err := s.DefaultSyncer.ProcessResponse(ctx, res, since); err != nil {
		return err
	}

	if s.failCount > 0 || !s.conn.firstSync {
		CMxClearStatus(s.conn.connId, FlagOffline|FlagConnecting)
		CMxSetStatus(s.conn.connId, FlagOnline)
	}
	s.failCount = 0

	s.conn.handleSync(ctx, res, since)
	return nil
}

func (s *MxSyncer) OnFailedSync(res *mautrix.RespSync, err error) (time.Duration, error) {
	if errors.Is(err, mautrix.MUnknownToken) {
		return 0, err
	}

	if errors.Is(err, context.Canceled) {
		return 0, err
	}

	s.failCount++
	if s.failCount == 1 {
		LOG_WARNING(fmt.Sprintf("sync failed: %v", err))
		CMxClearStatus(s.conn.connId, FlagOnline)
		CMxSetStatus(s.conn.connId, FlagConnecting)
	}

	return backoffDelay(s.failCount - 1), nil
}

// zerolog -> nchat log bridge. Only info and above is forwarded, as mautrix
// debug logging may include request details.
type ncLogWriter struct{}

func (w ncLogWriter) Write(p []byte) (int, error) {
	return len(p), nil
}

func (w ncLogWriter) WriteLevel(level zerolog.Level, p []byte) (int, error) {
	msg := "mautrix: " + strings.TrimSpace(string(p))
	switch level {
	case zerolog.InfoLevel:
		LOG_INFO(msg)
	case zerolog.WarnLevel:
		LOG_WARNING(msg)
	case zerolog.ErrorLevel, zerolog.FatalLevel, zerolog.PanicLevel:
		LOG_ERROR(msg)
	}

	return len(p), nil
}

func newLogger() zerolog.Logger {
	return zerolog.New(ncLogWriter{}).Level(zerolog.InfoLevel)
}
