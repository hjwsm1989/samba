/*
   Unix SMB/CIFS implementation.

   Copyright (C) Stefan Metzmacher 2011-2012
   Copyright (C) Michael Adam 2012

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "includes.h"
#include "system/filesys.h"
#include "smbd/smbd.h"
#include "smbd/globals.h"
#include "dbwrap/dbwrap.h"
#include "dbwrap/dbwrap_rbt.h"
#include "dbwrap/dbwrap_open.h"
#include "session.h"
#include "auth.h"
#include "auth/gensec/gensec.h"
#include "../lib/tsocket/tsocket.h"
#include "../libcli/security/security.h"
#include "messages.h"
#include "lib/util/util_tdb.h"
#include "librpc/gen_ndr/ndr_smbXsrv.h"
#include "serverid.h"

struct smbXsrv_session_table {
	struct {
		struct db_context *db_ctx;
		uint32_t lowest_id;
		uint32_t highest_id;
		uint32_t num_sessions;
	} local;
	struct {
		struct db_context *db_ctx;
	} global;
};

static struct db_context *smbXsrv_session_global_db_ctx = NULL;

NTSTATUS smbXsrv_session_global_init(void)
{
	const char *global_path = NULL;
	struct db_context *db_ctx = NULL;

	if (smbXsrv_session_global_db_ctx != NULL) {
		return NT_STATUS_OK;
	}

	/*
	 * This contains secret information like session keys!
	 */
	global_path = lock_path("smbXsrv_session_global.tdb");

	db_ctx = db_open(NULL, global_path,
			 0, /* hash_size */
			 TDB_DEFAULT |
			 TDB_CLEAR_IF_FIRST |
			 TDB_INCOMPATIBLE_HASH,
			 O_RDWR | O_CREAT, 0600,
			 DBWRAP_LOCK_ORDER_1);
	if (db_ctx == NULL) {
		NTSTATUS status;

		status = map_nt_error_from_unix_common(errno);

		return status;
	}

	smbXsrv_session_global_db_ctx = db_ctx;

	return NT_STATUS_OK;
}

/*
 * NOTE:
 * We need to store the keys in big endian so that dbwrap_rbt's memcmp
 * has the same result as integer comparison between the uint32_t
 * values.
 *
 * TODO: implement string based key
 */

#define SMBXSRV_SESSION_GLOBAL_TDB_KEY_SIZE sizeof(uint32_t)

static TDB_DATA smbXsrv_session_global_id_to_key(uint32_t id,
						 uint8_t *key_buf)
{
	TDB_DATA key;

	RSIVAL(key_buf, 0, id);

	key = make_tdb_data(key_buf, SMBXSRV_SESSION_GLOBAL_TDB_KEY_SIZE);

	return key;
}

#if 0
static NTSTATUS smbXsrv_session_global_key_to_id(TDB_DATA key, uint32_t *id)
{
	if (id == NULL) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (key.dsize != SMBXSRV_SESSION_GLOBAL_TDB_KEY_SIZE) {
		return NT_STATUS_INTERNAL_DB_CORRUPTION;
	}

	*id = RIVAL(key.dptr, 0);

	return NT_STATUS_OK;
}
#endif

#define SMBXSRV_SESSION_LOCAL_TDB_KEY_SIZE sizeof(uint32_t)

static TDB_DATA smbXsrv_session_local_id_to_key(uint32_t id,
						uint8_t *key_buf)
{
	TDB_DATA key;

	RSIVAL(key_buf, 0, id);

	key = make_tdb_data(key_buf, SMBXSRV_SESSION_LOCAL_TDB_KEY_SIZE);

	return key;
}

static NTSTATUS smbXsrv_session_local_key_to_id(TDB_DATA key, uint32_t *id)
{
	if (id == NULL) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (key.dsize != SMBXSRV_SESSION_LOCAL_TDB_KEY_SIZE) {
		return NT_STATUS_INTERNAL_DB_CORRUPTION;
	}

	*id = RIVAL(key.dptr, 0);

	return NT_STATUS_OK;
}

static NTSTATUS smbXsrv_session_table_init(struct smbXsrv_connection *conn,
					   uint32_t lowest_id,
					   uint32_t highest_id)
{
	struct smbXsrv_session_table *table;
	NTSTATUS status;

	table = talloc_zero(conn, struct smbXsrv_session_table);
	if (table == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	table->local.db_ctx = db_open_rbt(table);
	if (table->local.db_ctx == NULL) {
		TALLOC_FREE(table);
		return NT_STATUS_NO_MEMORY;
	}
	table->local.lowest_id = lowest_id;
	table->local.highest_id = highest_id;

	status = smbXsrv_session_global_init();
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(table);
		return status;
	}

	table->global.db_ctx = smbXsrv_session_global_db_ctx;

	conn->session_table = table;
	return NT_STATUS_OK;
}

struct smb1srv_session_local_allocate_state {
	const uint32_t lowest_id;
	const uint32_t highest_id;
	uint32_t last_id;
	uint32_t useable_id;
	NTSTATUS status;
};

static int smb1srv_session_local_allocate_traverse(struct db_record *rec,
						   void *private_data)
{
	struct smb1srv_session_local_allocate_state *state =
		(struct smb1srv_session_local_allocate_state *)private_data;
	TDB_DATA key = dbwrap_record_get_key(rec);
	uint32_t id = 0;
	NTSTATUS status;

	status = smbXsrv_session_local_key_to_id(key, &id);
	if (!NT_STATUS_IS_OK(status)) {
		state->status = status;
		return -1;
	}

	if (id <= state->last_id) {
		state->status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		return -1;
	}
	state->last_id = id;

	if (id > state->useable_id) {
		state->status = NT_STATUS_OK;
		return -1;
	}

	if (state->useable_id == state->highest_id) {
		state->status = NT_STATUS_INSUFFICIENT_RESOURCES;
		return -1;
	}

	state->useable_id +=1;
	return 0;
}

static NTSTATUS smb1srv_session_local_allocate_id(struct db_context *db,
						  uint32_t lowest_id,
						  uint32_t highest_id,
						  TALLOC_CTX *mem_ctx,
						  struct db_record **_rec,
						  uint32_t *_id)
{
	struct smb1srv_session_local_allocate_state state = {
		.lowest_id = lowest_id,
		.highest_id = highest_id,
		.last_id = 0,
		.useable_id = lowest_id,
		.status = NT_STATUS_INTERNAL_ERROR,
	};
	uint32_t i;
	uint32_t range;
	NTSTATUS status;
	int count = 0;

	*_rec = NULL;
	*_id = 0;

	if (lowest_id > highest_id) {
		return NT_STATUS_INSUFFICIENT_RESOURCES;
	}

	/*
	 * first we try randomly
	 */
	range = (highest_id - lowest_id) + 1;

	for (i = 0; i < (range / 2); i++) {
		uint32_t id;
		uint8_t key_buf[SMBXSRV_SESSION_LOCAL_TDB_KEY_SIZE];
		TDB_DATA key;
		TDB_DATA val;
		struct db_record *rec = NULL;

		id = generate_random() % range;
		id += lowest_id;

		if (id < lowest_id) {
			id = lowest_id;
		}
		if (id > highest_id) {
			id = highest_id;
		}

		key = smbXsrv_session_local_id_to_key(id, key_buf);

		rec = dbwrap_fetch_locked(db, mem_ctx, key);
		if (rec == NULL) {
			return NT_STATUS_INSUFFICIENT_RESOURCES;
		}

		val = dbwrap_record_get_value(rec);
		if (val.dsize != 0) {
			TALLOC_FREE(rec);
			continue;
		}

		*_rec = rec;
		*_id = id;
		return NT_STATUS_OK;
	}

	/*
	 * if the range is almost full,
	 * we traverse the whole table
	 * (this relies on sorted behavior of dbwrap_rbt)
	 */
	status = dbwrap_traverse_read(db, smb1srv_session_local_allocate_traverse,
				      &state, &count);
	if (NT_STATUS_IS_OK(status)) {
		if (NT_STATUS_IS_OK(state.status)) {
			return NT_STATUS_INTERNAL_ERROR;
		}

		if (!NT_STATUS_EQUAL(state.status, NT_STATUS_INTERNAL_ERROR)) {
			return state.status;
		}

		if (state.useable_id <= state.highest_id) {
			state.status = NT_STATUS_OK;
		} else {
			return NT_STATUS_INSUFFICIENT_RESOURCES;
		}
	} else if (!NT_STATUS_EQUAL(status, NT_STATUS_INTERNAL_DB_CORRUPTION)) {
		/*
		 * Here we really expect NT_STATUS_INTERNAL_DB_CORRUPTION!
		 *
		 * If we get anything else it is an error, because it
		 * means we did not manage to find a free slot in
		 * the db.
		 */
		return NT_STATUS_INSUFFICIENT_RESOURCES;
	}

	if (NT_STATUS_IS_OK(state.status)) {
		uint32_t id;
		uint8_t key_buf[SMBXSRV_SESSION_LOCAL_TDB_KEY_SIZE];
		TDB_DATA key;
		TDB_DATA val;
		struct db_record *rec = NULL;

		id = state.useable_id;

		key = smbXsrv_session_local_id_to_key(id, key_buf);

		rec = dbwrap_fetch_locked(db, mem_ctx, key);
		if (rec == NULL) {
			return NT_STATUS_INSUFFICIENT_RESOURCES;
		}

		val = dbwrap_record_get_value(rec);
		if (val.dsize != 0) {
			TALLOC_FREE(rec);
			return NT_STATUS_INTERNAL_DB_CORRUPTION;
		}

		*_rec = rec;
		*_id = id;
		return NT_STATUS_OK;
	}

	return state.status;
}

struct smbXsrv_session_local_fetch_state {
	struct smbXsrv_session *session;
	NTSTATUS status;
};

static void smbXsrv_session_local_fetch_parser(TDB_DATA key, TDB_DATA data,
					       void *private_data)
{
	struct smbXsrv_session_local_fetch_state *state =
		(struct smbXsrv_session_local_fetch_state *)private_data;
	void *ptr;

	if (data.dsize != sizeof(ptr)) {
		state->status = NT_STATUS_INTERNAL_DB_ERROR;
		return;
	}

	memcpy(&ptr, data.dptr, data.dsize);
	state->session = talloc_get_type_abort(ptr, struct smbXsrv_session);
	state->status = NT_STATUS_OK;
}

static NTSTATUS smbXsrv_session_local_lookup(struct smbXsrv_session_table *table,
					     uint32_t session_local_id,
					     NTTIME now,
					     struct smbXsrv_session **_session)
{
	struct smbXsrv_session_local_fetch_state state = {
		.session = NULL,
		.status = NT_STATUS_INTERNAL_ERROR,
	};
	uint8_t key_buf[SMBXSRV_SESSION_LOCAL_TDB_KEY_SIZE];
	TDB_DATA key;
	NTSTATUS status;

	*_session = NULL;

	if (session_local_id == 0) {
		return NT_STATUS_USER_SESSION_DELETED;
	}

	if (table == NULL) {
		/* this might happen before the end of negprot */
		return NT_STATUS_USER_SESSION_DELETED;
	}

	if (table->local.db_ctx == NULL) {
		return NT_STATUS_INTERNAL_ERROR;
	}

	key = smbXsrv_session_local_id_to_key(session_local_id, key_buf);

	status = dbwrap_parse_record(table->local.db_ctx, key,
				     smbXsrv_session_local_fetch_parser,
				     &state);
	if (NT_STATUS_EQUAL(status, NT_STATUS_NOT_FOUND)) {
		return NT_STATUS_USER_SESSION_DELETED;
	} else if (!NT_STATUS_IS_OK(status)) {
		return status;
	}
	if (!NT_STATUS_IS_OK(state.status)) {
		return state.status;
	}

	if (NT_STATUS_EQUAL(state.session->status, NT_STATUS_USER_SESSION_DELETED)) {
		return NT_STATUS_USER_SESSION_DELETED;
	}

	state.session->idle_time = now;

	if (!NT_STATUS_IS_OK(state.session->status)) {
		*_session = state.session;
		return state.session->status;
	}

	if (now > state.session->global->expiration_time) {
		state.session->status = NT_STATUS_NETWORK_SESSION_EXPIRED;
	}

	*_session = state.session;
	return state.session->status;
}

static int smbXsrv_session_global_destructor(struct smbXsrv_session_global0 *global)
{
	return 0;
}

static void smbXsrv_session_global_verify_record(struct db_record *db_rec,
					bool *is_free,
					bool *was_free,
					TALLOC_CTX *mem_ctx,
					struct smbXsrv_session_global0 **_g);

static NTSTATUS smbXsrv_session_global_allocate(struct db_context *db,
					TALLOC_CTX *mem_ctx,
					struct smbXsrv_session_global0 **_global)
{
	uint32_t i;
	struct smbXsrv_session_global0 *global = NULL;
	uint32_t last_free = 0;
	const uint32_t min_tries = 3;

	*_global = NULL;

	global = talloc_zero(mem_ctx, struct smbXsrv_session_global0);
	if (global == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	talloc_set_destructor(global, smbXsrv_session_global_destructor);

	/*
	 * Here we just randomly try the whole 32-bit space
	 *
	 * We use just 32-bit, because we want to reuse the
	 * ID for SRVSVC.
	 */
	for (i = 0; i < UINT32_MAX; i++) {
		bool is_free = false;
		bool was_free = false;
		uint32_t id;
		uint8_t key_buf[SMBXSRV_SESSION_GLOBAL_TDB_KEY_SIZE];
		TDB_DATA key;

		if (i >= min_tries && last_free != 0) {
			id = last_free;
		} else {
			id = generate_random();
		}
		if (id == 0) {
			id++;
		}
		if (id == UINT32_MAX) {
			id--;
		}

		key = smbXsrv_session_global_id_to_key(id, key_buf);

		global->db_rec = dbwrap_fetch_locked(db, mem_ctx, key);
		if (global->db_rec == NULL) {
			talloc_free(global);
			return NT_STATUS_INSUFFICIENT_RESOURCES;
		}

		smbXsrv_session_global_verify_record(global->db_rec,
						     &is_free,
						     &was_free,
						     NULL, NULL);

		if (!is_free) {
			TALLOC_FREE(global->db_rec);
			continue;
		}

		if (!was_free && i < min_tries) {
			/*
			 * The session_id is free now,
			 * but was not free before.
			 *
			 * This happens if a smbd crashed
			 * and did not cleanup the record.
			 *
			 * If this is one of our first tries,
			 * then we try to find a real free one.
			 */
			if (last_free == 0) {
				last_free = id;
			}
			TALLOC_FREE(global->db_rec);
			continue;
		}

		global->session_global_id = id;

		*_global = global;
		return NT_STATUS_OK;
	}

	/* should not be reached */
	talloc_free(global);
	return NT_STATUS_INTERNAL_ERROR;
}

static void smbXsrv_session_global_verify_record(struct db_record *db_rec,
					bool *is_free,
					bool *was_free,
					TALLOC_CTX *mem_ctx,
					struct smbXsrv_session_global0 **_g)
{
	TDB_DATA key;
	TDB_DATA val;
	DATA_BLOB blob;
	struct smbXsrv_session_globalB global_blob;
	enum ndr_err_code ndr_err;
	struct smbXsrv_session_global0 *global = NULL;
	bool exists;
	TALLOC_CTX *frame = talloc_stackframe();

	*is_free = false;

	if (was_free) {
		*was_free = false;
	}
	if (_g) {
		*_g = NULL;
	}

	key = dbwrap_record_get_key(db_rec);

	val = dbwrap_record_get_value(db_rec);
	if (val.dsize == 0) {
		TALLOC_FREE(frame);
		*is_free = true;
		if (was_free) {
			*was_free = true;
		}
		return;
	}

	blob = data_blob_const(val.dptr, val.dsize);

	ndr_err = ndr_pull_struct_blob(&blob, frame, &global_blob,
			(ndr_pull_flags_fn_t)ndr_pull_smbXsrv_session_globalB);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		NTSTATUS status = ndr_map_error2ntstatus(ndr_err);
		DEBUG(1,("smbXsrv_session_global_verify_record: "
			 "key '%s' ndr_pull_struct_blob - %s\n",
			 hex_encode_talloc(frame, key.dptr, key.dsize),
			 nt_errstr(status)));
		TALLOC_FREE(frame);
		return;
	}

	DEBUG(10,("smbXsrv_session_global_verify_record\n"));
	if (DEBUGLVL(10)) {
		NDR_PRINT_DEBUG(smbXsrv_session_globalB, &global_blob);
	}

	if (global_blob.version != SMBXSRV_VERSION_0) {
		DEBUG(0,("smbXsrv_session_global_verify_record: "
			 "key '%s' use unsupported version %u\n",
			 hex_encode_talloc(frame, key.dptr, key.dsize),
			 global_blob.version));
		NDR_PRINT_DEBUG(smbXsrv_session_globalB, &global_blob);
		TALLOC_FREE(frame);
		return;
	}

	global = global_blob.info.info0;

	exists = serverid_exists(&global->channels[0].server_id);
	if (!exists) {
		DEBUG(2,("smbXsrv_session_global_verify_record: "
			 "key '%s' server_id %s does not exist.\n",
			 hex_encode_talloc(frame, key.dptr, key.dsize),
			 server_id_str(frame, &global->channels[0].server_id)));
		if (DEBUGLVL(2)) {
			NDR_PRINT_DEBUG(smbXsrv_session_globalB, &global_blob);
		}
		TALLOC_FREE(frame);
		dbwrap_record_delete(db_rec);
		*is_free = true;
		return;
	}

	if (_g) {
		*_g = talloc_move(mem_ctx, &global);
	}
	TALLOC_FREE(frame);
}

static NTSTATUS smbXsrv_session_global_store(struct smbXsrv_session_global0 *global)
{
	struct smbXsrv_session_globalB global_blob;
	DATA_BLOB blob = data_blob_null;
	TDB_DATA key;
	TDB_DATA val;
	NTSTATUS status;
	enum ndr_err_code ndr_err;

	/*
	 * TODO: if we use other versions than '0'
	 * we would add glue code here, that would be able to
	 * store the information in the old format.
	 */

	if (global->db_rec == NULL) {
		return NT_STATUS_INTERNAL_ERROR;
	}

	key = dbwrap_record_get_key(global->db_rec);
	val = dbwrap_record_get_value(global->db_rec);

	ZERO_STRUCT(global_blob);
	global_blob.version = smbXsrv_version_global_current();
	if (val.dsize >= 8) {
		global_blob.seqnum = IVAL(val.dptr, 4);
	}
	global_blob.seqnum += 1;
	global_blob.info.info0 = global;

	ndr_err = ndr_push_struct_blob(&blob, global->db_rec, &global_blob,
			(ndr_push_flags_fn_t)ndr_push_smbXsrv_session_globalB);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		status = ndr_map_error2ntstatus(ndr_err);
		DEBUG(1,("smbXsrv_session_global_store: key '%s' ndr_push - %s\n",
			 hex_encode_talloc(global->db_rec, key.dptr, key.dsize),
			 nt_errstr(status)));
		TALLOC_FREE(global->db_rec);
		return status;
	}

	val = make_tdb_data(blob.data, blob.length);
	status = dbwrap_record_store(global->db_rec, val, TDB_REPLACE);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(1,("smbXsrv_session_global_store: key '%s' store - %s\n",
			 hex_encode_talloc(global->db_rec, key.dptr, key.dsize),
			 nt_errstr(status)));
		TALLOC_FREE(global->db_rec);
		return status;
	}

	if (DEBUGLVL(10)) {
		DEBUG(10,("smbXsrv_session_global_store: key '%s' stored\n",
			 hex_encode_talloc(global->db_rec, key.dptr, key.dsize)));
		NDR_PRINT_DEBUG(smbXsrv_session_globalB, &global_blob);
	}

	TALLOC_FREE(global->db_rec);

	return NT_STATUS_OK;
}

static int smbXsrv_session_destructor(struct smbXsrv_session *session)
{
	NTSTATUS status;

	status = smbXsrv_session_logoff(session);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0, ("smbXsrv_session_destructor: "
			  "smbXsrv_session_logoff() failed: %s\n",
			  nt_errstr(status)));
	}

	TALLOC_FREE(session->global);

	return 0;
}

NTSTATUS smbXsrv_session_create(struct smbXsrv_connection *conn,
				NTTIME now,
				struct smbXsrv_session **_session)
{
	struct smbXsrv_session_table *table = conn->session_table;
	uint32_t max_sessions = table->local.highest_id - table->local.lowest_id;
	struct db_record *local_rec = NULL;
	struct smbXsrv_session *session = NULL;
	void *ptr = NULL;
	TDB_DATA val;
	struct smbXsrv_session_global0 *global = NULL;
	struct smbXsrv_channel_global0 *channels = NULL;
	NTSTATUS status;

	if (table->local.num_sessions >= max_sessions) {
		return NT_STATUS_INSUFFICIENT_RESOURCES;
	}

	session = talloc_zero(table, struct smbXsrv_session);
	if (session == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	session->table = table;
	session->idle_time = now;
	session->status = NT_STATUS_MORE_PROCESSING_REQUIRED;
	session->connection = conn;

	status = smbXsrv_session_global_allocate(table->global.db_ctx,
						 session,
						 &global);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(session);
		return status;
	}
	session->global = global;

	if (conn->protocol >= PROTOCOL_SMB2_02) {
		uint64_t id = global->session_global_id;
		uint8_t key_buf[SMBXSRV_SESSION_LOCAL_TDB_KEY_SIZE];
		TDB_DATA key;

		global->connection_dialect = conn->smb2.server.dialect;

		global->session_wire_id = id;

		status = smb2srv_tcon_table_init(session);
		if (!NT_STATUS_IS_OK(status)) {
			TALLOC_FREE(session);
			return status;
		}

		session->local_id = global->session_global_id;

		key = smbXsrv_session_local_id_to_key(session->local_id, key_buf);

		local_rec = dbwrap_fetch_locked(table->local.db_ctx,
						session, key);
		if (local_rec == NULL) {
			TALLOC_FREE(session);
			return NT_STATUS_NO_MEMORY;
		}

		val = dbwrap_record_get_value(local_rec);
		if (val.dsize != 0) {
			TALLOC_FREE(session);
			return NT_STATUS_INTERNAL_DB_CORRUPTION;
		}
	} else {

		status = smb1srv_session_local_allocate_id(table->local.db_ctx,
							table->local.lowest_id,
							table->local.highest_id,
							session,
							&local_rec,
							&session->local_id);
		if (!NT_STATUS_IS_OK(status)) {
			TALLOC_FREE(session);
			return status;
		}

		global->session_wire_id = session->local_id;
	}

	global->creation_time = now;
	global->expiration_time = GENSEC_EXPIRE_TIME_INFINITY;

	global->num_channels = 1;
	channels = talloc_zero_array(global,
				     struct smbXsrv_channel_global0,
				     global->num_channels);
	if (channels == NULL) {
		TALLOC_FREE(session);
		return NT_STATUS_NO_MEMORY;
	}
	global->channels = channels;

	channels[0].server_id = messaging_server_id(conn->msg_ctx);
	channels[0].local_address = tsocket_address_string(conn->local_address,
							   channels);
	if (channels[0].local_address == NULL) {
		TALLOC_FREE(session);
		return NT_STATUS_NO_MEMORY;
	}
	channels[0].remote_address = tsocket_address_string(conn->remote_address,
							    channels);
	if (channels[0].remote_address == NULL) {
		TALLOC_FREE(session);
		return NT_STATUS_NO_MEMORY;
	}
	channels[0].remote_name = talloc_strdup(channels, conn->remote_hostname);
	if (channels[0].remote_name == NULL) {
		TALLOC_FREE(session);
		return NT_STATUS_NO_MEMORY;
	}
	channels[0].signing_key = data_blob_null;

	ptr = session;
	val = make_tdb_data((uint8_t const *)&ptr, sizeof(ptr));
	status = dbwrap_record_store(local_rec, val, TDB_REPLACE);
	TALLOC_FREE(local_rec);
	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(session);
		return status;
	}
	table->local.num_sessions += 1;

	talloc_set_destructor(session, smbXsrv_session_destructor);

	status = smbXsrv_session_global_store(global);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0,("smbXsrv_session_create: "
			 "global_id (0x%08x) store failed - %s\n",
			 session->global->session_global_id,
			 nt_errstr(status)));
		TALLOC_FREE(session);
		return status;
	}

	if (DEBUGLVL(10)) {
		struct smbXsrv_sessionB session_blob;

		ZERO_STRUCT(session_blob);
		session_blob.version = SMBXSRV_VERSION_0;
		session_blob.info.info0 = session;

		DEBUG(10,("smbXsrv_session_create: global_id (0x%08x) stored\n",
			 session->global->session_global_id));
		NDR_PRINT_DEBUG(smbXsrv_sessionB, &session_blob);
	}

	*_session = session;
	return NT_STATUS_OK;
}

NTSTATUS smbXsrv_session_update(struct smbXsrv_session *session)
{
	struct smbXsrv_session_table *table = session->table;
	NTSTATUS status;
	uint8_t key_buf[SMBXSRV_SESSION_GLOBAL_TDB_KEY_SIZE];
	TDB_DATA key;

	if (session->global->db_rec != NULL) {
		DEBUG(0, ("smbXsrv_session_update(0x%08x): "
			  "Called with db_rec != NULL'\n",
			  session->global->session_global_id));
		return NT_STATUS_INTERNAL_ERROR;
	}

	key = smbXsrv_session_global_id_to_key(
					session->global->session_global_id,
					key_buf);

	session->global->db_rec = dbwrap_fetch_locked(table->global.db_ctx,
						      session->global, key);
	if (session->global->db_rec == NULL) {
		DEBUG(0, ("smbXsrv_session_update(0x%08x): "
			  "Failed to lock global key '%s'\n",
			  session->global->session_global_id,
			  hex_encode_talloc(talloc_tos(), key.dptr,
					    key.dsize)));
		return NT_STATUS_INTERNAL_DB_ERROR;
	}

	status = smbXsrv_session_global_store(session->global);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0,("smbXsrv_session_update: "
			 "global_id (0x%08x) store failed - %s\n",
			 session->global->session_global_id,
			 nt_errstr(status)));
		return status;
	}

	if (DEBUGLVL(10)) {
		struct smbXsrv_sessionB session_blob;

		ZERO_STRUCT(session_blob);
		session_blob.version = SMBXSRV_VERSION_0;
		session_blob.info.info0 = session;

		DEBUG(10,("smbXsrv_session_update: global_id (0x%08x) stored\n",
			  session->global->session_global_id));
		NDR_PRINT_DEBUG(smbXsrv_sessionB, &session_blob);
	}

	return NT_STATUS_OK;
}

NTSTATUS smbXsrv_session_logoff(struct smbXsrv_session *session)
{
	struct smbXsrv_session_table *table;
	struct db_record *local_rec = NULL;
	struct db_record *global_rec = NULL;
	struct smbXsrv_connection *conn;
	NTSTATUS status;
	NTSTATUS error = NT_STATUS_OK;

	if (session->table == NULL) {
		return NT_STATUS_OK;
	}

	table = session->table;
	session->table = NULL;

	conn = session->connection;
	session->connection = NULL;
	session->status = NT_STATUS_USER_SESSION_DELETED;

	global_rec = session->global->db_rec;
	session->global->db_rec = NULL;
	if (global_rec == NULL) {
		uint8_t key_buf[SMBXSRV_SESSION_GLOBAL_TDB_KEY_SIZE];
		TDB_DATA key;

		key = smbXsrv_session_global_id_to_key(
					session->global->session_global_id,
					key_buf);

		global_rec = dbwrap_fetch_locked(table->global.db_ctx,
						 session->global, key);
		if (global_rec == NULL) {
			DEBUG(0, ("smbXsrv_session_logoff(0x%08x): "
				  "Failed to lock global key '%s'\n",
				  session->global->session_global_id,
				  hex_encode_talloc(global_rec, key.dptr,
						    key.dsize)));
			error = NT_STATUS_INTERNAL_ERROR;
		}
	}

	if (global_rec != NULL) {
		status = dbwrap_record_delete(global_rec);
		if (!NT_STATUS_IS_OK(status)) {
			TDB_DATA key = dbwrap_record_get_key(global_rec);

			DEBUG(0, ("smbXsrv_session_logoff(0x%08x): "
				  "failed to delete global key '%s': %s\n",
				  session->global->session_global_id,
				  hex_encode_talloc(global_rec, key.dptr,
						    key.dsize),
				  nt_errstr(status)));
			error = status;
		}
	}
	TALLOC_FREE(global_rec);

	local_rec = session->db_rec;
	if (local_rec == NULL) {
		uint8_t key_buf[SMBXSRV_SESSION_LOCAL_TDB_KEY_SIZE];
		TDB_DATA key;

		key = smbXsrv_session_local_id_to_key(session->local_id,
						      key_buf);

		local_rec = dbwrap_fetch_locked(table->local.db_ctx,
						session, key);
		if (local_rec == NULL) {
			DEBUG(0, ("smbXsrv_session_logoff(0x%08x): "
				  "Failed to lock local key '%s'\n",
				  session->global->session_global_id,
				  hex_encode_talloc(local_rec, key.dptr,
						    key.dsize)));
			error = NT_STATUS_INTERNAL_ERROR;
		}
	}

	if (local_rec != NULL) {
		status = dbwrap_record_delete(local_rec);
		if (!NT_STATUS_IS_OK(status)) {
			TDB_DATA key = dbwrap_record_get_key(local_rec);

			DEBUG(0, ("smbXsrv_session_logoff(0x%08x): "
				  "failed to delete local key '%s': %s\n",
				  session->global->session_global_id,
				  hex_encode_talloc(local_rec, key.dptr,
						    key.dsize),
				  nt_errstr(status)));
			error = status;
		}
		table->local.num_sessions -= 1;
	}
	if (session->db_rec == NULL) {
		TALLOC_FREE(local_rec);
	}
	session->db_rec = NULL;

	if (session->compat) {
		file_close_user(conn->sconn, session->compat->vuid);
	}

	if (conn->protocol >= PROTOCOL_SMB2_02) {
		status = smb2srv_tcon_disconnect_all(session);
		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(0, ("smbXsrv_session_logoff(0x%08x): "
				  "smb2srv_tcon_disconnect_all() failed: %s\n",
				  session->global->session_global_id,
				  nt_errstr(status)));
			error = status;
		}
	}

	if (session->compat) {
		invalidate_vuid(conn->sconn, session->compat->vuid);
		session->compat = NULL;
	}

	return error;
}

struct smbXsrv_session_logoff_all_state {
	NTSTATUS first_status;
	int errors;
};

static int smbXsrv_session_logoff_all_callback(struct db_record *local_rec,
					       void *private_data);

NTSTATUS smbXsrv_session_logoff_all(struct smbXsrv_connection *conn)
{
	struct smbXsrv_session_table *table = conn->session_table;
	struct smbXsrv_session_logoff_all_state state;
	NTSTATUS status;
	int count = 0;

	if (table == NULL) {
		DEBUG(10, ("smbXsrv_session_logoff_all: "
			   "empty session_table, nothing to do.\n"));
		return NT_STATUS_OK;
	}

	ZERO_STRUCT(state);

	status = dbwrap_traverse(table->local.db_ctx,
				 smbXsrv_session_logoff_all_callback,
				 &state, &count);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0, ("smbXsrv_session_logoff_all: "
			  "dbwrap_traverse() failed: %s\n",
			  nt_errstr(status)));
		return status;
	}

	if (!NT_STATUS_IS_OK(state.first_status)) {
		DEBUG(0, ("smbXsrv_session_logoff_all: "
			  "count[%d] errors[%d] first[%s]\n",
			  count, state.errors,
			  nt_errstr(state.first_status)));
		return state.first_status;
	}

	return NT_STATUS_OK;
}

static int smbXsrv_session_logoff_all_callback(struct db_record *local_rec,
					       void *private_data)
{
	struct smbXsrv_session_logoff_all_state *state =
		(struct smbXsrv_session_logoff_all_state *)private_data;
	TDB_DATA val;
	void *ptr = NULL;
	struct smbXsrv_session *session = NULL;
	NTSTATUS status;

	val = dbwrap_record_get_value(local_rec);
	if (val.dsize != sizeof(ptr)) {
		status = NT_STATUS_INTERNAL_ERROR;
		if (NT_STATUS_IS_OK(state->first_status)) {
			state->first_status = status;
		}
		state->errors++;
		return 0;
	}

	memcpy(&ptr, val.dptr, val.dsize);
	session = talloc_get_type_abort(ptr, struct smbXsrv_session);

	session->db_rec = local_rec;
	status = smbXsrv_session_logoff(session);
	if (!NT_STATUS_IS_OK(status)) {
		if (NT_STATUS_IS_OK(state->first_status)) {
			state->first_status = status;
		}
		state->errors++;
		return 0;
	}

	return 0;
}

NTSTATUS smb1srv_session_table_init(struct smbXsrv_connection *conn)
{
	/*
	 * Allow a range from 1..65534.
	 */
	return smbXsrv_session_table_init(conn, 1, UINT16_MAX - 1);
}

NTSTATUS smb1srv_session_lookup(struct smbXsrv_connection *conn,
				uint16_t vuid, NTTIME now,
				struct smbXsrv_session **session)
{
	struct smbXsrv_session_table *table = conn->session_table;
	uint32_t local_id = vuid;

	return smbXsrv_session_local_lookup(table, local_id, now, session);
}

NTSTATUS smb2srv_session_table_init(struct smbXsrv_connection *conn)
{
	/*
	 * For now use the same range as SMB1.
	 *
	 * Allow a range from 1..65534.
	 */
	return smbXsrv_session_table_init(conn, 1, UINT16_MAX - 1);
}

NTSTATUS smb2srv_session_lookup(struct smbXsrv_connection *conn,
				uint64_t session_id, NTTIME now,
				struct smbXsrv_session **session)
{
	struct smbXsrv_session_table *table = conn->session_table;
	uint32_t local_id = session_id & UINT32_MAX;
	uint64_t local_zeros = session_id & 0xFFFFFFFF00000000LLU;

	if (local_zeros != 0) {
		return NT_STATUS_USER_SESSION_DELETED;
	}

	return smbXsrv_session_local_lookup(table, local_id, now, session);
}