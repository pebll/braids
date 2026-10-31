// cgomx.go
//
// Copyright (c) 2026 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

package main

// #cgo linux LDFLAGS: -Wl,-unresolved-symbols=ignore-all
// #cgo darwin LDFLAGS: -Wl,-undefined,dynamic_lookup
// #include <stdlib.h>
// extern void MxNewContactsNotify(int p_ConnId, char* p_Id, char* p_Name, int p_IsSelf, int p_Notify);
// extern void MxNewChatsNotify(int p_ConnId, char* p_ChatId, int p_IsUnread, int p_IsMuted, int p_IsPinned, long long p_LastMessageTime);
// extern void MxNewMessagesNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, char* p_SenderId, char* p_Text, int p_FromMe, char* p_QuotedId, char* p_FileId, char* p_FilePath, int p_FileStatus, long long p_TimeSent, int p_IsRead, int p_IsEdited, int p_Batch);
// extern void MxNewMessagesBatchDone(int p_ConnId, char* p_ChatId, char* p_FromMsgId);
// extern void MxNewMessageStatusNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, int p_IsRead);
// extern void MxNewMessageFileNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, char* p_FileId, char* p_FilePath, int p_FileStatus, int p_Action);
// extern void MxNewMessageReactionNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, char* p_SenderId, char* p_Text, int p_FromMe);
// extern void MxNewTypingNotify(int p_ConnId, char* p_ChatId, char* p_UserId, int p_IsTyping);
// extern void MxNewStatusNotify(int p_ConnId, char* p_UserId, int p_IsOnline, long long p_TimeSeen);
// extern void MxNewGroupMembersNotify(int p_ConnId, char* p_ChatId, char* p_MembersJson);
// extern void MxNewAvatarNotify(int p_ConnId, char* p_Id, char* p_FilePath);
// extern void MxNewDescriptionNotify(int p_ConnId, char* p_Id, char* p_Description);
// extern void MxDeleteChatNotify(int p_ConnId, char* p_ChatId);
// extern void MxDeleteMessageNotify(int p_ConnId, char* p_ChatId, char* p_MsgId, int p_IsOutgoing);
// extern void MxUpdateMuteNotify(int p_ConnId, char* p_ChatId, int p_IsMuted);
// extern void MxUpdatePinNotify(int p_ConnId, char* p_ChatId, int p_IsPinned, long long p_TimePinned);
// extern void MxCreateChatNotify(int p_ConnId, char* p_ChatId, int p_Success);
// extern void MxRecoveryPrompt(int p_ConnId);
// extern void MxReportError(int p_ConnId, char* p_Message);
// extern void MxSetStatus(int p_ConnId, int p_Flags);
// extern void MxClearStatus(int p_ConnId, int p_Flags);
// extern void MxLogTrace(char* p_Filename, int p_LineNo, char* p_Message);
// extern void MxLogDebug(char* p_Filename, int p_LineNo, char* p_Message);
// extern void MxLogInfo(char* p_Filename, int p_LineNo, char* p_Message);
// extern void MxLogWarning(char* p_Filename, int p_LineNo, char* p_Message);
// extern void MxLogError(char* p_Filename, int p_LineNo, char* p_Message);
import "C"

import (
	"path/filepath"
	"runtime"
)

// Exported functions (C++ -> Go). Strings returned as *C.char are malloc'd and
// must be freed by the caller.

//export CMxInit
func CMxInit(path *C.char) int {
	return MxInit(C.GoString(path))
}

//export CMxCleanup
func CMxCleanup(connId int) int {
	return MxCleanup(connId)
}

//export CMxGetVersion
func CMxGetVersion() *C.char {
	return C.CString(MxGetVersion())
}

//export CMxNormalizeUserId
func CMxNormalizeUserId(userId *C.char) *C.char {
	return C.CString(MxNormalizeUserId(C.GoString(userId)))
}

//export CMxDiscoverHomeserver
func CMxDiscoverHomeserver(userId *C.char) *C.char {
	return C.CString(MxDiscoverHomeserver(C.GoString(userId)))
}

//export CMxSetupLogin
func CMxSetupLogin(connId int, homeserver *C.char, userId *C.char, password *C.char) *C.char {
	return C.CString(MxSetupLogin(connId, C.GoString(homeserver), C.GoString(userId), C.GoString(password)))
}

//export CMxLogin
func CMxLogin(connId int) int {
	return MxLogin(connId)
}

//export CMxUnlink
func CMxUnlink(connId int) int {
	return MxUnlink(connId)
}

//export CMxLogout
func CMxLogout(connId int) int {
	return MxLogout(connId)
}

//export CMxWaitReady
func CMxWaitReady(connId int, timeoutSec int) int {
	return MxWaitReady(connId, timeoutSec)
}

//export CMxGetSelfId
func CMxGetSelfId(connId int) *C.char {
	return C.CString(MxGetSelfId(connId))
}

//export CMxIsGroupChat
func CMxIsGroupChat(connId int, chatId *C.char) int {
	return MxIsGroupChat(connId, C.GoString(chatId))
}

//export CMxNeedsRecovery
func CMxNeedsRecovery(connId int) int {
	return MxNeedsRecovery(connId)
}

//export CMxRecover
func CMxRecover(connId int, key *C.char) *C.char {
	return C.CString(MxRecover(connId, C.GoString(key)))
}

//export CMxGetChats
func CMxGetChats(connId int) int {
	return MxGetChats(connId)
}

//export CMxGetContacts
func CMxGetContacts(connId int) int {
	return MxGetContacts(connId)
}

//export CMxGetMessages
func CMxGetMessages(connId int, chatId *C.char, fromMsgId *C.char, limit int) int {
	return MxGetMessages(connId, C.GoString(chatId), C.GoString(fromMsgId), limit)
}

//export CMxSendMessage
func CMxSendMessage(connId int, chatId *C.char, text *C.char, quotedId *C.char, filePath *C.char, fileType *C.char, editMsgId *C.char, mentionsJson *C.char) int {
	return MxSendMessage(connId, C.GoString(chatId), C.GoString(text), C.GoString(quotedId), C.GoString(filePath), C.GoString(fileType), C.GoString(editMsgId), C.GoString(mentionsJson))
}

//export CMxMarkMessageRead
func CMxMarkMessageRead(connId int, chatId *C.char, msgId *C.char) int {
	return MxMarkMessageRead(connId, C.GoString(chatId), C.GoString(msgId))
}

//export CMxDeleteMessage
func CMxDeleteMessage(connId int, chatId *C.char, msgId *C.char) int {
	return MxDeleteMessage(connId, C.GoString(chatId), C.GoString(msgId))
}

//export CMxDeleteChat
func CMxDeleteChat(connId int, chatId *C.char) int {
	return MxDeleteChat(connId, C.GoString(chatId))
}

//export CMxPinChat
func CMxPinChat(connId int, chatId *C.char, isPinned int) int {
	return MxPinChat(connId, C.GoString(chatId), isPinned)
}

//export CMxSendTyping
func CMxSendTyping(connId int, chatId *C.char, isTyping int) int {
	return MxSendTyping(connId, C.GoString(chatId), isTyping)
}

//export CMxSendStatus
func CMxSendStatus(connId int, isOnline int) int {
	return MxSendStatus(connId, isOnline)
}

//export CMxGetStatus
func CMxGetStatus(connId int, userId *C.char) int {
	return MxGetStatus(connId, C.GoString(userId))
}

//export CMxSendReaction
func CMxSendReaction(connId int, chatId *C.char, msgId *C.char, emoji *C.char) int {
	return MxSendReaction(connId, C.GoString(chatId), C.GoString(msgId), C.GoString(emoji))
}

//export CMxDownloadFile
func CMxDownloadFile(connId int, chatId *C.char, msgId *C.char, fileId *C.char, action int) int {
	return MxDownloadFile(connId, C.GoString(chatId), C.GoString(msgId), C.GoString(fileId), action)
}

//export CMxGetAvatar
func CMxGetAvatar(connId int, avatarId *C.char) int {
	return MxGetAvatar(connId, C.GoString(avatarId))
}

//export CMxGetDescription
func CMxGetDescription(connId int, id *C.char) int {
	return MxGetDescription(connId, C.GoString(id))
}

//export CMxGetGroupMembers
func CMxGetGroupMembers(connId int, chatId *C.char) int {
	return MxGetGroupMembers(connId, C.GoString(chatId))
}

//export CMxCreateChat
func CMxCreateChat(connId int, userId *C.char) int {
	return MxCreateChat(connId, C.GoString(userId))
}

// Callbacks (Go -> C++). The C++ side frees all passed strings.

func CMxNewContactsNotify(connId int, userId string, name string, isSelf int, notify int) {
	C.MxNewContactsNotify(C.int(connId), C.CString(userId), C.CString(name), C.int(isSelf), C.int(notify))
}

func CMxNewChatsNotify(connId int, chatId string, isUnread int, isMuted int, isPinned int, lastMessageTime int64) {
	C.MxNewChatsNotify(C.int(connId), C.CString(chatId), C.int(isUnread), C.int(isMuted), C.int(isPinned), C.longlong(lastMessageTime))
}

func CMxNewMessagesNotify(connId int, chatId string, msgId string, senderId string, text string, fromMe int, quotedId string, fileId string, filePath string, fileStatus int, timeSent int64, isRead int, isEdited int, batch int) {
	C.MxNewMessagesNotify(C.int(connId), C.CString(chatId), C.CString(msgId), C.CString(senderId), C.CString(text), C.int(fromMe), C.CString(quotedId), C.CString(fileId), C.CString(filePath), C.int(fileStatus), C.longlong(timeSent), C.int(isRead), C.int(isEdited), C.int(batch))
}

func CMxNewMessagesBatchDone(connId int, chatId string, fromMsgId string) {
	C.MxNewMessagesBatchDone(C.int(connId), C.CString(chatId), C.CString(fromMsgId))
}

func CMxNewMessageStatusNotify(connId int, chatId string, msgId string, isRead int) {
	C.MxNewMessageStatusNotify(C.int(connId), C.CString(chatId), C.CString(msgId), C.int(isRead))
}

func CMxNewMessageFileNotify(connId int, chatId string, msgId string, fileId string, filePath string, fileStatus int, action int) {
	C.MxNewMessageFileNotify(C.int(connId), C.CString(chatId), C.CString(msgId), C.CString(fileId), C.CString(filePath), C.int(fileStatus), C.int(action))
}

func CMxNewMessageReactionNotify(connId int, chatId string, msgId string, senderId string, text string, fromMe int) {
	C.MxNewMessageReactionNotify(C.int(connId), C.CString(chatId), C.CString(msgId), C.CString(senderId), C.CString(text), C.int(fromMe))
}

func CMxNewTypingNotify(connId int, chatId string, userId string, isTyping int) {
	C.MxNewTypingNotify(C.int(connId), C.CString(chatId), C.CString(userId), C.int(isTyping))
}

func CMxNewStatusNotify(connId int, userId string, isOnline int, timeSeen int64) {
	C.MxNewStatusNotify(C.int(connId), C.CString(userId), C.int(isOnline), C.longlong(timeSeen))
}

func CMxNewGroupMembersNotify(connId int, chatId string, membersJson string) {
	C.MxNewGroupMembersNotify(C.int(connId), C.CString(chatId), C.CString(membersJson))
}

func CMxNewAvatarNotify(connId int, avatarId string, filePath string) {
	C.MxNewAvatarNotify(C.int(connId), C.CString(avatarId), C.CString(filePath))
}

func CMxNewDescriptionNotify(connId int, id string, description string) {
	C.MxNewDescriptionNotify(C.int(connId), C.CString(id), C.CString(description))
}

func CMxDeleteChatNotify(connId int, chatId string) {
	C.MxDeleteChatNotify(C.int(connId), C.CString(chatId))
}

func CMxDeleteMessageNotify(connId int, chatId string, msgId string, isOutgoing int) {
	C.MxDeleteMessageNotify(C.int(connId), C.CString(chatId), C.CString(msgId), C.int(isOutgoing))
}

func CMxUpdateMuteNotify(connId int, chatId string, isMuted int) {
	C.MxUpdateMuteNotify(C.int(connId), C.CString(chatId), C.int(isMuted))
}

func CMxUpdatePinNotify(connId int, chatId string, isPinned int, timePinned int64) {
	C.MxUpdatePinNotify(C.int(connId), C.CString(chatId), C.int(isPinned), C.longlong(timePinned))
}

func CMxCreateChatNotify(connId int, chatId string, success int) {
	C.MxCreateChatNotify(C.int(connId), C.CString(chatId), C.int(success))
}

func CMxRecoveryPrompt(connId int) {
	C.MxRecoveryPrompt(C.int(connId))
}

func CMxReportError(connId int, message string) {
	C.MxReportError(C.int(connId), C.CString(message))
}

func CMxSetStatus(connId int, flags int) {
	C.MxSetStatus(C.int(connId), C.int(flags))
}

func CMxClearStatus(connId int, flags int) {
	C.MxClearStatus(C.int(connId), C.int(flags))
}

func callerInfo() (*C.char, C.int) {
	_, filename, lineNo, ok := runtime.Caller(2)
	if ok {
		filename = filepath.Base(filename)
	} else {
		filename = "???"
		lineNo = 0
	}

	return C.CString(filename), C.int(lineNo)
}

// Log functions. Never pass secrets (tokens, passwords, keys) or message
// plaintext to these, at any level.

func LOG_TRACE(message string) {
	filename, lineNo := callerInfo()
	C.MxLogTrace(filename, lineNo, C.CString(message))
}

func LOG_DEBUG(message string) {
	filename, lineNo := callerInfo()
	C.MxLogDebug(filename, lineNo, C.CString(message))
}

func LOG_INFO(message string) {
	filename, lineNo := callerInfo()
	C.MxLogInfo(filename, lineNo, C.CString(message))
}

func LOG_WARNING(message string) {
	filename, lineNo := callerInfo()
	C.MxLogWarning(filename, lineNo, C.CString(message))
}

func LOG_ERROR(message string) {
	filename, lineNo := callerInfo()
	C.MxLogError(filename, lineNo, C.CString(message))
}
