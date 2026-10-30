// gosg_groups.go
//
// Copyright (c) 2026 braids contributors
//
// braids is distributed under the GNU AGPL v3 license, see LICENSE for details.

package main

import (
	"context"
	"database/sql"
	"fmt"
	"time"

	"go.mau.fi/mautrix-signal/pkg/signalmeow"
	"go.mau.fi/mautrix-signal/pkg/signalmeow/types"
)

// RefreshGroupNames looks up the title of every group in the store and
// reports it as contact name. Group names are otherwise only learned from
// the initial history sync and from new messages, so a group whose lookup
// failed then (or that was renamed meanwhile) kept showing its id.
func RefreshGroupNames(connId int) {
	go func() {
		// called at startup, possibly before the client is up
		var client *signalmeow.Client
		for i := 0; i < 240; i++ {
			if client = GetClient(connId); client != nil {
				break
			}
			time.Sleep(500 * time.Millisecond)
		}

		device := GetDevice(connId)
		if (client == nil) || (device == nil) {
			return
		}

		groupIDs, err := storedGroupIDs(GetPath(connId), device.ACI.String())
		if err != nil {
			LOG_WARNING(fmt.Sprintf("list groups error: %v", err))
			return
		}

		LOG_DEBUG(fmt.Sprintf("refresh %d group names", len(groupIDs)))
		for _, groupID := range groupIDs {
			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			group, _, err := client.RetrieveGroupByID(ctx, types.GroupIdentifier(groupID), 0)
			cancel()
			if (err != nil) || (group == nil) || (group.Title == "") {
				continue
			}

			if !HasContact(connId, groupID) || (GetContactName(connId, groupID) != group.Title) {
				AddContactName(connId, groupID, group.Title)
				CSgNewContactsNotify(connId, groupID, group.Title, "", BoolToInt(false), BoolToInt(false), NotifyDirect)
			}

			// gentle on the server
			time.Sleep(100 * time.Millisecond)
		}
	}()
}

// storedGroupIDs lists the groups whose master key signalmeow has stored
// (the group store has no list method).
func storedGroupIDs(path string, accountID string) ([]string, error) {
	db, err := sql.Open("sqlite3", fmt.Sprintf("file:%s/signal.db?mode=ro", path))
	if err != nil {
		return nil, err
	}
	defer db.Close()

	rows, err := db.Query(`SELECT group_identifier FROM signalmeow_groups WHERE account_id=?`, accountID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var ids []string
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}

	return ids, rows.Err()
}
