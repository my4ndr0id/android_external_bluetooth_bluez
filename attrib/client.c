/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2010  Nokia Corporation
 *  Copyright (C) 2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <glib.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/uuid.h>

#include "adapter.h"
#include "device.h"
#include "log.h"
#include "gdbus.h"
#include "error.h"
#include "dbus-common.h"
#include "btio.h"
#include "storage.h"

#include "att.h"
#include "gattrib.h"
#include "gatt.h"
#include "client.h"

#define CHAR_INTERFACE "org.bluez.Characteristic"

struct gatt_service {
	struct btd_device *dev;
	DBusConnection *conn;
	bdaddr_t sba;
	bdaddr_t dba;
	char *path;
	GSList *primary;
	GAttrib *attrib;
	DBusMessage *msg;
	int psm;
	gboolean listen;
};

struct format {
	guint8 format;
	guint8 exponent;
	guint16 unit;
	guint8 namespace;
	guint16 desc;
} __attribute__ ((packed));

struct primary {
	struct gatt_service *gatt;
	struct att_primary *att;
	DBusMessage *discovery_msg;
	guint	discovery_timer;
	char *path;
	GSList *chars;
	GSList *watchers;
};

struct descriptor {
	char *name;
	char *desc;
	uint16_t cli_conf_hndl;
	uint16_t cli_conf;
	struct format *format;
};

struct characteristic {
	struct primary *prim;
	char *path;
	uint16_t handle;
	uint16_t end;
	uint8_t perm;
	char type[MAX_LEN_UUID_STR + 1];
	struct descriptor desc;
	DBusMessage *msg;
	uint8_t *value;
	size_t vlen;
};

struct query_data {
	struct primary *prim;
	struct characteristic *chr;
	DBusMessage *msg;
	uint16_t handle;
	gboolean last;
	uint16_t len;
	uint8_t value[0];
};

struct watcher {
	guint id;
	char *name;
	char *path;
	struct primary *prim;
};

static GSList *gatt_services = NULL;

static void characteristic_free(void *user_data)
{
	struct characteristic *chr = user_data;

	g_free(chr->path);
	g_free(chr->value);
	g_free(chr->desc.desc);
	g_free(chr->desc.format);
	g_free(chr->desc.name);
	g_free(chr);
}

static void watcher_free(void *user_data)
{
	struct watcher *watcher = user_data;

	g_free(watcher->path);
	g_free(watcher->name);
	g_free(watcher);
}

static void primary_free(void *user_data)
{
	struct primary *prim = user_data;
	GSList *l;

	for (l = prim->watchers; l; l = l->next) {
		struct watcher *watcher = l->data;
		g_dbus_remove_watch(prim->gatt->conn, watcher->id);
	}

	g_slist_foreach(prim->chars, (GFunc) characteristic_free, NULL);
	g_slist_free(prim->chars);
	g_free(prim->path);
	g_free(prim);
}

static void gatt_service_free(void *user_data)
{
	struct gatt_service *gatt = user_data;

	g_slist_foreach(gatt->primary, (GFunc) primary_free, NULL);
	g_slist_free(gatt->primary);
	g_attrib_unref(gatt->attrib);
	g_free(gatt->path);
	btd_device_unref(gatt->dev);
	dbus_connection_unref(gatt->conn);
	g_free(gatt);
}

static int gatt_dev_cmp(gconstpointer a, gconstpointer b)
{
	const struct gatt_service *gatt = a;
	const struct btd_device *dev = b;

	return gatt->dev != dev;
}

static int characteristic_handle_cmp(gconstpointer a, gconstpointer b)
{
	const struct characteristic *chr = a;
	uint16_t handle = GPOINTER_TO_UINT(b);

	return chr->handle - handle;
}

static int watcher_cmp(gconstpointer a, gconstpointer b)
{
	const struct watcher *watcher = a;
	const struct watcher *match = b;
	int ret;

	ret = g_strcmp0(watcher->name, match->name);
	if (ret != 0)
		return ret;

	return g_strcmp0(watcher->path, match->path);
}

static void append_char_dict(DBusMessageIter *iter, struct characteristic *chr)
{
	DBusMessageIter dict;
	const char *name = "";
	char *uuid;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	uuid = g_strdup(chr->type);
	dict_append_entry(&dict, "UUID", DBUS_TYPE_STRING, &uuid);
	g_free(uuid);

	/* FIXME: Translate UUID to name. */
	dict_append_entry(&dict, "Name", DBUS_TYPE_STRING, &name);

	if (chr->desc.desc)
		dict_append_entry(&dict, "Description", DBUS_TYPE_STRING,
								&chr->desc.desc);

	/* FIXME: Only if remote has Client Configuration */
	dict_append_entry(&dict, "ClientConfiguration", DBUS_TYPE_UINT16,
					  &(chr->desc.cli_conf));

	dict_append_entry(&dict, "Properties", DBUS_TYPE_BYTE,
					  &(chr->perm));

	if (chr->value)
		dict_append_array(&dict, "Value", DBUS_TYPE_BYTE, &chr->value,
								chr->vlen);

	/* FIXME: Missing Format, Value and Representation */

	dbus_message_iter_close_container(iter, &dict);
}

static void watcher_exit(DBusConnection *conn, void *user_data)
{
	struct watcher *watcher = user_data;
	struct primary *prim = watcher->prim;
	struct gatt_service *gatt = prim->gatt;

	DBG("%s watcher %s exited", prim->path, watcher->name);

	prim->watchers = g_slist_remove(prim->watchers, watcher);

	g_attrib_unref(gatt->attrib);
}

static int characteristic_set_value(struct characteristic *chr,
					const uint8_t *value, size_t vlen)
{
	chr->value = g_try_realloc(chr->value, vlen);
	if (chr->value == NULL)
		return -ENOMEM;

	memcpy(chr->value, value, vlen);
	chr->vlen = vlen;

	return 0;
}

static int characteristic_set_cli_conf(struct characteristic *chr,
					const uint8_t *value)
{
	memcpy(&chr->desc.cli_conf, value, 2);

	return 0;
}

static void update_watchers(gpointer data, gpointer user_data)
{
	struct watcher *w = data;
	struct characteristic *chr = user_data;
	DBusConnection *conn = w->prim->gatt->conn;
	DBusMessage *msg;

	msg = dbus_message_new_method_call(w->name, w->path,
				"org.bluez.Watcher", "ValueChanged");
	if (msg == NULL)
		return;

	dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &chr->path,
			DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
			&chr->value, chr->vlen, DBUS_TYPE_INVALID);

	dbus_message_set_no_reply(msg, TRUE);
	g_dbus_send_message(conn, msg);
}

static void events_handler(const uint8_t *pdu, uint16_t len,
							gpointer user_data)
{
	struct gatt_service *gatt = user_data;
	struct characteristic *chr;
	struct primary *prim;
	GSList *lprim, *lchr;
	uint8_t opdu[ATT_MAX_MTU];
	guint handle;
	uint16_t olen;

	if (len < 3) {
		DBG("Malformed notification/indication packet (opcode 0x%02x)",
									pdu[0]);
		return;
	}

	handle = att_get_u16(&pdu[1]);

	for (lprim = gatt->primary, prim = NULL, chr = NULL; lprim;
						lprim = lprim->next) {
		prim = lprim->data;

		lchr = g_slist_find_custom(prim->chars,
			GUINT_TO_POINTER(handle), characteristic_handle_cmp);
		if (lchr) {
			chr = lchr->data;
			break;
		}
	}

	if (chr == NULL) {
		DBG("Attribute handle 0x%02x not found", handle);
		return;
	}

	switch (pdu[0]) {
	case ATT_OP_HANDLE_IND:
		olen = enc_confirmation(opdu, sizeof(opdu));
		g_attrib_send(gatt->attrib, 0, opdu[0], opdu, olen,
						NULL, NULL, NULL);
	case ATT_OP_HANDLE_NOTIFY:
		if (characteristic_set_value(chr, &pdu[3], len - 3) < 0)
			DBG("Can't change Characteristic 0x%02x", handle);

		g_slist_foreach(prim->watchers, update_watchers, chr);
		break;
	}
}

static void attrib_destroy(gpointer user_data)
{
	struct gatt_service *gatt = user_data;

	gatt->attrib = NULL;
}

static void stop_discovery(gpointer user_data, gpointer extra_data) {

	struct primary *prim = (struct primary *) user_data;
	struct gatt_service *gatt = prim->gatt;
	DBusMessage *reply;

	DBG("");

	prim->discovery_timer = 0;

	if (!prim->discovery_msg)
		return;

	reply = btd_error_failed(prim->discovery_msg,
			"Discover characteristic values timed out");

	DBG(" %s", prim->path);
	g_dbus_send_message(prim->gatt->conn, reply);

	prim->discovery_msg = NULL;

	g_attrib_unref(gatt->attrib);
}

static gboolean stop_discovery_timeout(gpointer user_data) {
	stop_discovery(user_data,  NULL);
	return FALSE;
}

static void attrib_disconnect(gpointer user_data)
{
	struct gatt_service *gatt = user_data;
	GSList *l;

	DBG("");

	if (!gatt)
		return;

	g_slist_foreach(gatt->primary, stop_discovery, NULL);

	/* Remote initiated disconnection only */
	g_attrib_unref(gatt->attrib);
}

static void connect_cb(GIOChannel *chan, GError *gerr, gpointer user_data)
{
	struct gatt_service *gatt = user_data;

	if (gerr) {
		if (gatt->msg) {
			DBusMessage *reply = btd_error_failed(gatt->msg,
							gerr->message);
			g_dbus_send_message(gatt->conn, reply);
		}

		error("%s", gerr->message);
		goto fail;
	}

	if (gatt->attrib == NULL)
		return;

	/* Listen mode: used for notification and indication */
	if (gatt->listen == TRUE) {
		g_attrib_register(gatt->attrib,
					ATT_OP_HANDLE_NOTIFY,
					events_handler, gatt, NULL);
		g_attrib_register(gatt->attrib,
					ATT_OP_HANDLE_IND,
					events_handler, gatt, NULL);
		return;
	}

	return;
fail:
	g_attrib_unref(gatt->attrib);
}

static int l2cap_connect(struct gatt_service *gatt, GError **gerr,
								gboolean listen)
{
	GIOChannel *io;

	if (gatt->attrib != NULL) {
		gatt->attrib = g_attrib_ref(gatt->attrib);
		gatt->listen = listen;
		return 0;
	}

	/*
	 * FIXME: If the service doesn't support Client Characteristic
	 * Configuration it is necessary to poll the server from time
	 * to time checking for modifications.
	 */
	if (gatt->psm < 0)
		io = bt_io_connect(BT_IO_L2CAP, connect_cb, gatt, NULL, gerr,
			BT_IO_OPT_SOURCE_BDADDR, &gatt->sba,
			BT_IO_OPT_DEST_BDADDR, &gatt->dba,
			BT_IO_OPT_CID, ATT_CID,
			BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
			BT_IO_OPT_INVALID);
	else
		io = bt_io_connect(BT_IO_L2CAP, connect_cb, gatt, NULL, gerr,
			BT_IO_OPT_SOURCE_BDADDR, &gatt->sba,
			BT_IO_OPT_DEST_BDADDR, &gatt->dba,
			BT_IO_OPT_PSM, gatt->psm,
			BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
			BT_IO_OPT_INVALID);
	if (!io)
		return -1;

	gatt->attrib = g_attrib_new(io);
	g_io_channel_unref(io);
	gatt->listen = listen;

	g_attrib_set_destroy_function(gatt->attrib, attrib_destroy, gatt);
	g_attrib_set_disconnect_function(gatt->attrib, attrib_disconnect,
									gatt);

	return 0;
}

DBusMessage *create_discovery_reply(struct primary *prim)
{
	DBusMessage *reply = dbus_message_new_method_return(prim->discovery_msg);
	DBusMessageIter iter, array_iter;
	GSList *l;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
				DBUS_TYPE_OBJECT_PATH_AS_STRING, &array_iter);

	for (l = prim->chars; l; l = l->next) {
		struct characteristic *chr = l->data;

		dbus_message_iter_append_basic(&array_iter,
					DBUS_TYPE_OBJECT_PATH, &chr->path);
	}

	dbus_message_iter_close_container(&iter, &array_iter);

	return reply;
}

static void update_char_value(guint8 status, const guint8 *pdu,
					guint16 len, gpointer user_data)
{
	struct query_data *current = user_data;
	struct primary *prim = current->prim;
	struct gatt_service *gatt = prim->gatt;
	struct characteristic *chr = current->chr;
	DBusMessage *reply;

	DBG("");

	if (status == 0) {
		characteristic_set_value(chr, pdu + 1, len - 1);
	} else if (status == ATT_ECODE_INSUFF_ENC ||
			status == ATT_ECODE_AUTHENTICATION) {
		GIOChannel *io = g_attrib_get_channel(gatt->attrib);

		if (bt_io_set(io, BT_IO_L2CAP, NULL,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_HIGH,
				BT_IO_OPT_INVALID)) {
			gatt_read_char(gatt->attrib, chr->handle, 0,
					update_char_value, current);
			return;
		}
	}

	if (chr->prim->discovery_msg != NULL) {
		if (prim->discovery_timer > 0)
			g_source_remove(prim->discovery_timer);
		prim->discovery_timer = 0;

		if (current->last) {
			DBusMessage *reply = create_discovery_reply(prim);

			g_dbus_send_message(gatt->conn, reply);

			prim->discovery_msg = NULL;
		} else {
			prim->discovery_timer = g_timeout_add_seconds(GATT_TIMEOUT,
					stop_discovery_timeout, prim);
		}
	}  else if (chr->msg != NULL) {

		if (status == 0) {
			reply = dbus_message_new_method_return(chr->msg);
		} else {
			reply = btd_error_failed(chr->msg,
									 "Update characteristic value failed");
		}

		if (reply)
			g_dbus_send_message(gatt->conn, reply);

		chr->msg = NULL;
	}

	g_attrib_unref(gatt->attrib);
	g_free(current);
}

static DBusMessage *register_watcher(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	const char *sender = dbus_message_get_sender(msg);
	struct primary *prim = data;
	struct watcher *watcher;
	GError *gerr = NULL;
	char *path;
	DBusMessage *reply;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	if (l2cap_connect(prim->gatt, &gerr, TRUE) < 0) {
		DBusMessage *reply = btd_error_failed(msg, gerr->message);
		g_error_free(gerr);
		return reply;
	}

	g_attrib_register(prim->gatt->attrib,
					ATT_OP_HANDLE_NOTIFY,
					events_handler, prim->gatt, NULL);
	g_attrib_register(prim->gatt->attrib,
					ATT_OP_HANDLE_IND,
					events_handler, prim->gatt, NULL);

	watcher = g_new0(struct watcher, 1);
	watcher->name = g_strdup(sender);
	watcher->prim = prim;
	watcher->path = g_strdup(path);
	watcher->id = g_dbus_add_disconnect_watch(conn, sender, watcher_exit,
							watcher, watcher_free);

	prim->watchers = g_slist_append(prim->watchers, watcher);

	reply = dbus_message_new_method_return(msg);

	g_attrib_unref(prim->gatt->attrib);

	return reply;
}

static DBusMessage *unregister_watcher(DBusConnection *conn,
						DBusMessage *msg, void *data)
{
	const char *sender = dbus_message_get_sender(msg);
	struct primary *prim = data;
	struct watcher *watcher, *match;
	GSList *l;
	char *path;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
							DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	match = g_new0(struct watcher, 1);
	match->name = g_strdup(sender);
	match->path = g_strdup(path);
	l = g_slist_find_custom(prim->watchers, match, watcher_cmp);
	watcher_free(match);
	if (!l)
		return btd_error_not_authorized(msg);

	watcher = l->data;
	g_dbus_remove_watch(conn, watcher->id);
	prim->watchers = g_slist_remove(prim->watchers, watcher);
	watcher_free(watcher);

	return dbus_message_new_method_return(msg);
}

static void gatt_write_char_resp(guint8 status, const guint8 *pdu,
					guint16 len, gpointer user_data)
{
	struct query_data *current = user_data;
	struct primary *prim = current->prim;
	struct gatt_service *gatt = prim->gatt;
	struct characteristic *chr = current->chr;
	DBusMessage *reply;
	DBusMessage *msg;
	DBusMessageIter iter, sub, sub_value;
	DBusMessageIter dict;
	uint8_t *value;
	int vlen;

	DBG("Gatt Write Char Response Recv, status = %d", status);

	if (status == 0) {
		msg = chr->msg;
		dbus_message_iter_init(msg, &iter);
		dbus_message_iter_next(&iter);
		dbus_message_iter_recurse(&iter, &sub);
		dbus_message_iter_recurse(&sub, &sub_value);
		dbus_message_iter_get_fixed_array(&sub_value, &value, &vlen);

		characteristic_set_value(chr, value, vlen);

		reply = dbus_message_new_method_return(chr->msg);
		if (!reply) {
			chr->msg == NULL;
			return;
		}

		g_dbus_send_message(gatt->conn, reply);

		chr->msg = NULL;

	} else if (status == ATT_ECODE_INSUFF_ENC ||
			status == ATT_ECODE_AUTHENTICATION) {
		GIOChannel *io = g_attrib_get_channel(gatt->attrib);

		if (bt_io_set(io, BT_IO_L2CAP, NULL,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_HIGH,
				BT_IO_OPT_INVALID)) {
			gatt_write_char(gatt->attrib, chr->handle,
					current->value, current->len,
					gatt_write_char_resp, current);
			return;
		}
	} else {
		reply = btd_error_invalid_args(chr->msg);
		if (!reply) {
			DBG("reply is NULL");
			chr->msg == NULL;
		} else {
			g_dbus_send_message(gatt->conn, reply);
			chr->msg = NULL;
		}
	}

	g_attrib_unref(gatt->attrib);

}

static void gatt_write_cli_conf_resp(guint8 status, const guint8 *pdu,
					guint16 len, gpointer user_data)
{
	struct query_data *current = user_data;
	struct primary *prim = current->prim;
	struct gatt_service *gatt = prim->gatt;
	struct characteristic *chr = current->chr;
	DBusMessage *reply;
	DBusMessage *msg;
	DBusMessageIter iter, sub, sub_value;
	DBusMessageIter dict;
	uint8_t *value;
	int vlen;

	DBG("Gatt Write Cli Conf Response Recv, status = %d", status);

	if (status == 0) {
		msg = chr->msg;
		dbus_message_iter_init(msg, &iter);
		dbus_message_iter_next(&iter);
		dbus_message_iter_recurse(&iter, &sub);
		dbus_message_iter_recurse(&sub, &sub_value);
		dbus_message_iter_get_fixed_array(&sub_value, &value, &vlen);

		characteristic_set_cli_conf(chr, value);

		reply = dbus_message_new_method_return(chr->msg);
		if (!reply) {
			chr->msg == NULL;
			return;
		}

		g_dbus_send_message(gatt->conn, reply);
		chr->msg = NULL;

	} else if (status == ATT_ECODE_INSUFF_ENC ||
			status == ATT_ECODE_AUTHENTICATION) {
		GIOChannel *io = g_attrib_get_channel(gatt->attrib);

		if (bt_io_set(io, BT_IO_L2CAP, NULL,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_HIGH,
				BT_IO_OPT_INVALID)) {
			gatt_write_char(gatt->attrib, chr->handle,
					current->value, current->len,
					gatt_write_cli_conf_resp, current);
			return;
		}
	} else {
		reply = btd_error_invalid_args(chr->msg);
		if (!reply) {
			chr->msg == NULL;
		} else {
			g_dbus_send_message(gatt->conn, reply);
			chr->msg = NULL;
		}
	}

	g_attrib_unref(gatt->attrib);
}

static DBusMessage *set_value(DBusConnection *conn, DBusMessage *msg,
			DBusMessageIter *iter, struct characteristic *chr)
{
	struct gatt_service *gatt = chr->prim->gatt;
	struct query_data *qvalue;
	DBusMessageIter sub;
	GError *gerr = NULL;
	uint8_t *value;
	int len;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY ||
			dbus_message_iter_get_element_type(iter) != DBUS_TYPE_BYTE)
		return btd_error_invalid_args(msg);

	dbus_message_iter_recurse(iter, &sub);

	dbus_message_iter_get_fixed_array(&sub, &value, &len);

	if (l2cap_connect(gatt, &gerr, FALSE) < 0) {
		DBusMessage *reply = btd_error_failed(msg, gerr->message);
		g_error_free(gerr);
		return reply;
	}

	qvalue = g_malloc0(sizeof(struct query_data) + len);
	qvalue->prim = chr->prim;
	qvalue->chr = chr;
	qvalue->len = len;
	memcpy(qvalue->value, value, len);

	chr->msg = dbus_message_ref(msg);

	gatt_write_char(gatt->attrib, chr->handle, value,
					len, gatt_write_char_resp, qvalue);

  return NULL;
}

static DBusMessage *set_cli_conf(DBusConnection *conn, DBusMessage *msg,
			DBusMessageIter *iter, struct characteristic *chr)
{
	struct gatt_service *gatt = chr->prim->gatt;
	struct query_data *qvalue;
	DBusMessageIter sub;
	GError *gerr = NULL;
	uint8_t *value;
	int len;

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_ARRAY ||
			dbus_message_iter_get_element_type(iter) != DBUS_TYPE_BYTE)
		return btd_error_invalid_args(msg);

	dbus_message_iter_recurse(iter, &sub);

	dbus_message_iter_get_fixed_array(&sub, &value, &len);

	if (l2cap_connect(gatt, &gerr, FALSE) < 0) {
		DBusMessage *reply = btd_error_failed(msg, gerr->message);
		g_error_free(gerr);
		return reply;
	}

	qvalue = g_malloc0(sizeof(struct query_data) + len);
	qvalue->prim = chr->prim;
	qvalue->chr = chr;
	qvalue->len = len;
	memcpy(qvalue->value, value, len);

	chr->msg = dbus_message_ref(msg);

	gatt_write_char(gatt->attrib, chr->desc.cli_conf_hndl, value, len,
					gatt_write_cli_conf_resp, qvalue);

  return NULL;
}

static DBusMessage *get_properties(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct characteristic *chr = data;
	DBusMessage *reply;
	DBusMessageIter iter;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	append_char_dict(&iter, chr);

	return reply;
}

static DBusMessage *set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct characteristic *chr = data;
	DBusMessageIter iter;
	DBusMessageIter sub;
	const char *property;

	if (!dbus_message_iter_init(msg, &iter))
		return btd_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return btd_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return btd_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &sub);

	if (g_str_equal("Value", property)) {
		return set_value(conn, msg, &sub, chr);
	} else if (g_str_equal("ClientConfiguration", property)) {
		return set_cli_conf(conn, msg, &sub, chr);
	}

	return btd_error_invalid_args(msg);
}

static DBusMessage *fetch_value(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct characteristic *chr = data;
	struct query_data *qvalue;
	GError *gerr = NULL;
	struct primary *prim = chr->prim;
	struct gatt_service *gatt = prim->gatt;

	DBG("");

	if (l2cap_connect(gatt, &gerr, FALSE) < 0) {
		DBusMessage *reply = btd_error_failed(msg, gerr->message);
		g_error_free(gerr);
		return reply;
	}

	qvalue = g_new0(struct query_data, 1);
	qvalue->prim = prim;
	qvalue->chr = chr;

	chr->msg = dbus_message_ref(msg);

	gatt_read_char(gatt->attrib, chr->handle, 0, update_char_value, qvalue);

	return NULL;
}

static GDBusMethodTable char_methods[] = {
	{ "GetProperties",	"",	"a{sv}", get_properties },
	{ "SetProperty",	"sv",	"",	set_property,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "UpdateValue",	"",	"",	fetch_value,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ }
};

static char *characteristic_list_to_string(GSList *chars)
{
	GString *characteristics;
	GSList *l;

	characteristics = g_string_new(NULL);

	for (l = chars; l; l = l->next) {
		struct characteristic *chr = l->data;
		char chr_str[64];

		memset(chr_str, 0, sizeof(chr_str));

		snprintf(chr_str, sizeof(chr_str), "%04X#%02X#%04X#%s ",
				chr->handle, chr->perm, chr->end, chr->type);

		characteristics = g_string_append(characteristics, chr_str);
	}

	return g_string_free(characteristics, FALSE);
}

static void store_characteristics(struct gatt_service *gatt,
							struct primary *prim)
{
	char *characteristics;
	struct att_primary *att = prim->att;

	characteristics = characteristic_list_to_string(prim->chars);

	write_device_characteristics(&gatt->sba, &gatt->dba, att->start,
							characteristics);

	g_free(characteristics);
}

static void register_characteristics(struct primary *prim)
{
	GSList *lc;

	for (lc = prim->chars; lc; lc = lc->next) {
		struct characteristic *chr = lc->data;
		g_dbus_register_interface(prim->gatt->conn, chr->path,
				CHAR_INTERFACE, char_methods,
				NULL, NULL, chr, NULL);
		DBG("Registered: %s", chr->path);
	}
}

static GSList *string_to_characteristic_list(struct primary *prim,
							const char *str)
{
	GSList *l = NULL;
	char **chars;
	int i;

	if (str == NULL)
		return NULL;

	chars = g_strsplit(str, " ", 0);
	if (chars == NULL)
		return NULL;

	for (i = 0; chars[i]; i++) {
		struct characteristic *chr;
		int ret;

		chr = g_new0(struct characteristic, 1);

		ret = sscanf(chars[i], "%04hX#%02hhX#%04hX#%s", &chr->handle,
				&chr->perm, &chr->end, chr->type);
		if (ret < 4) {
			g_free(chr);
			continue;
		}

		chr->prim = prim;
		chr->path = g_strdup_printf("%s/characteristic%04x",
						prim->path, chr->handle);

		l = g_slist_append(l, chr);
	}

	g_strfreev(chars);

	return l;
}

static void load_characteristics(gpointer data, gpointer user_data)
{
	struct primary *prim = data;
	struct att_primary *att = prim->att;
	struct gatt_service *gatt = user_data;
	GSList *chrs_list;
	char *str;

	if (prim->chars) {
		DBG("Characteristics already loaded");
		return;
	}

	str = read_device_characteristics(&gatt->sba, &gatt->dba, att->start);
	if (str == NULL)
		return;

	chrs_list = string_to_characteristic_list(prim, str);

	free(str);

	if (chrs_list == NULL)
		return;

	prim->chars = chrs_list;
	register_characteristics(prim);

	return;
}

static void store_attribute(struct gatt_service *gatt, uint16_t handle,
				uint16_t type, uint8_t *value, gsize len)
{
	bt_uuid_t uuid;
	char *str, *tmp;
	guint i;

	str = g_malloc0(MAX_LEN_UUID_STR + len * 2 + 1);

	bt_uuid16_create(&uuid, type);
	bt_uuid_to_string(&uuid, str, MAX_LEN_UUID_STR);

	str[MAX_LEN_UUID_STR - 1] = '#';

	for (i = 0, tmp = str + MAX_LEN_UUID_STR; i < len; i++, tmp += 2)
		sprintf(tmp, "%02X", value[i]);

	write_device_attribute(&gatt->sba, &gatt->dba, handle, str);
	g_free(str);
}

static void update_char_cli_conf(guint8 status, const guint8 *pdu, guint16 len,
								gpointer user_data)
{
	struct query_data *current = user_data;
	struct gatt_service *gatt = current->prim->gatt;
	struct characteristic *chr = current->chr;

	DBG("");

	if (status != 0)
		goto done;

	if (len != 3)
		goto done;

	memcpy(&chr->desc.cli_conf, pdu + 1, 2);

	store_attribute(gatt, current->handle, GATT_CLIENT_CHARAC_CFG_UUID,
				(void *) &chr->desc.cli_conf, sizeof(chr->desc.cli_conf));

done:
	g_attrib_unref(gatt->attrib);
	g_free(current);
}

static void update_char_desc(guint8 status, const guint8 *pdu, guint16 len,
							gpointer user_data)
{
	struct query_data *current = user_data;
	struct gatt_service *gatt = current->prim->gatt;
	struct characteristic *chr = current->chr;

	if (status == 0) {

		g_free(chr->desc.desc);

		chr->desc.desc = g_malloc(len);
		memcpy(chr->desc.desc, pdu + 1, len - 1);
		chr->desc.desc[len - 1] = '\0';

		store_attribute(gatt, current->handle,
				GATT_CHARAC_USER_DESC_UUID,
				(void *) chr->desc.desc, len);
	} else if (status == ATT_ECODE_INSUFF_ENC ||
			status == ATT_ECODE_AUTHENTICATION) {
		GIOChannel *io = g_attrib_get_channel(gatt->attrib);

		if (bt_io_set(io, BT_IO_L2CAP, NULL,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_HIGH,
				BT_IO_OPT_INVALID)) {
			gatt_read_char(gatt->attrib, current->handle, 0,
					update_char_desc, current);
			return;
		}
	}

	g_attrib_unref(gatt->attrib);
	g_free(current);
}

static void update_char_format(guint8 status, const guint8 *pdu, guint16 len,
								gpointer user_data)
{
	struct query_data *current = user_data;
	struct gatt_service *gatt = current->prim->gatt;
	struct characteristic *chr = current->chr;

	if (status != 0)
		goto done;

	if (len < 8)
		goto done;

	g_free(chr->desc.format);

	chr->desc.format = g_new0(struct format, 1);
	memcpy(chr->desc.format, pdu + 1, 7);

	store_attribute(gatt, current->handle, GATT_CHARAC_FMT_UUID,
				(void *) chr->desc.format, sizeof(*chr->desc.format));

done:
	g_attrib_unref(gatt->attrib);
	g_free(current);
}

static int uuid_desc16_cmp(bt_uuid_t *uuid, guint16 desc)
{
	bt_uuid_t u16;

	bt_uuid16_create(&u16, desc);

	return bt_uuid_cmp(uuid, &u16);
}

static void descriptor_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	struct query_data *current = user_data;
	struct gatt_service *gatt = current->prim->gatt;
	struct att_data_list *list;
	guint8 format;
	int i;

	if (status != 0)
		goto done;

	DBG("Find Information Response received");

	list = dec_find_info_resp(pdu, plen, &format);
	if (list == NULL)
		goto done;

	for (i = 0; i < list->num; i++) {
		guint16 handle;
		bt_uuid_t uuid;
		uint8_t *info = list->data[i];
		struct query_data *qfmt;

		handle = att_get_u16(info);

		if (format == 0x01) {
			uuid = att_get_uuid16(&info[2]);
		} else {
			/* Currently, only "user description" and "presentation
			 * format" descriptors are used, and both have 16-bit
			 * UUIDs. Therefore there is no need to support format
			 * 0x02 yet. */
			continue;
		}
		qfmt = g_new0(struct query_data, 1);
		qfmt->prim = current->prim;
		qfmt->chr = current->chr;
		qfmt->handle = handle;

		if (uuid_desc16_cmp(&uuid, GATT_CLIENT_CHARAC_CFG_UUID) == 0) {
			gatt->attrib = g_attrib_ref(gatt->attrib);
			current->chr->desc.cli_conf_hndl = handle;
			gatt_read_char(gatt->attrib, handle, 0,
						   update_char_cli_conf, qfmt);
		} else if (uuid_desc16_cmp(&uuid, GATT_CHARAC_USER_DESC_UUID) == 0) {
			gatt->attrib = g_attrib_ref(gatt->attrib);
			gatt_read_char(gatt->attrib, handle, 0, update_char_desc,
									qfmt);
		} else if (uuid_desc16_cmp(&uuid, GATT_CHARAC_FMT_UUID) == 0) {
			gatt->attrib = g_attrib_ref(gatt->attrib);
			gatt_read_char(gatt->attrib, handle, 0,
						update_char_format, qfmt);
		} else
			g_free(qfmt);
	}

	att_data_list_free(list);
done:
	g_attrib_unref(gatt->attrib);
	g_free(current);
}

static void update_all_chars(struct primary *prim)
{
	struct query_data *qdesc, *qvalue;
	struct characteristic *chr;
	struct gatt_service *gatt = prim->gatt;
	GSList *l;

	for(l = prim->chars; l; l = l->next) {
		chr = l->data;

		qdesc = g_new0(struct query_data, 1);
		qdesc->prim = prim;
		qdesc->chr = chr;

		gatt->attrib = g_attrib_ref(gatt->attrib);
		gatt_find_info(gatt->attrib, chr->handle + 1, chr->end, descriptor_cb,
					   qdesc);

		qvalue = g_new0(struct query_data, 1);
		qvalue->prim = prim;
		qvalue->chr = chr;
		qvalue->last = (l->next==NULL);

		gatt->attrib = g_attrib_ref(gatt->attrib);

		gatt_read_char(gatt->attrib, chr->handle, 0, update_char_value, qvalue);
	}

	/* Start timer */
	prim->discovery_timer = g_timeout_add_seconds(GATT_TIMEOUT,
						stop_discovery_timeout, prim);

}

static void char_discovered_cb(GSList *characteristics, guint8 status,
							gpointer user_data)
{
	DBusMessage *reply;
	struct query_data *current = user_data;
	struct primary *prim = current->prim;
	struct att_primary *att = prim->att;
	struct gatt_service *gatt = prim->gatt;
	uint16_t *previous_end = NULL;
	GSList *l;

	if (status != 0) {
		const char *str = att_ecode2str(status);

		DBG("Discover all characteristics failed: %s", str);
		reply = btd_error_failed(prim->discovery_msg, str);
		goto fail;
	}

	for (l = characteristics; l; l = l->next) {
		struct att_char *current_chr = l->data;
		struct characteristic *chr;
		guint handle = current_chr->value_handle;
		GSList *lchr;

		lchr = g_slist_find_custom(prim->chars,
			GUINT_TO_POINTER(handle), characteristic_handle_cmp);
		if (lchr)
			continue;

		chr = g_new0(struct characteristic, 1);
		chr->prim = prim;
		chr->perm = current_chr->properties;
		chr->handle = current_chr->value_handle;
		chr->path = g_strdup_printf("%s/characteristic%04x",
						prim->path, chr->handle);
		strncpy(chr->type, current_chr->uuid, sizeof(chr->type));

		if (previous_end)
			*previous_end = current_chr->handle;

		previous_end = &chr->end;

		prim->chars = g_slist_append(prim->chars, chr);

	}

	if (previous_end)
		*previous_end = att->end;

	store_characteristics(gatt, prim);
	register_characteristics(prim);

	update_all_chars(prim);

	g_free(current);

	return;

fail:
	g_dbus_send_message(gatt->conn, reply);
	g_attrib_unref(gatt->attrib);
	g_free(current);
}

static DBusMessage *discover_char(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct primary *prim = data;
	struct att_primary *att = prim->att;
	struct gatt_service *gatt = prim->gatt;
	struct query_data *qchr;
	GError *gerr = NULL;

	DBG(" %s", prim->path);

	if (prim->discovery_msg) {
		DBusMessage *reply = btd_error_failed(msg, "Discovery already in progress");
		g_error_free(gerr);
		return reply;
	}

	if (l2cap_connect(prim->gatt, &gerr, TRUE) < 0) {
		DBusMessage *reply = btd_error_failed(msg, gerr->message);
		g_error_free(gerr);
		return reply;
	}

	qchr = g_new0(struct query_data, 1);
	qchr->prim = prim;

	prim->discovery_msg = dbus_message_ref(msg);

	gatt_discover_char(gatt->attrib, att->start, att->end, NULL,
						char_discovered_cb, qchr);

	return NULL;
}

static DBusMessage *prim_get_properties(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct primary *prim = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	GSList *l;
	char **chars;
	const char *uuid;
	int i;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	chars = g_new0(char *, g_slist_length(prim->chars) + 1);

	for (i = 0, l = prim->chars; l; l = l->next, i++) {
		struct characteristic *chr = l->data;
		chars[i] = chr->path;
	}

	dict_append_array(&dict, "Characteristics", DBUS_TYPE_OBJECT_PATH,
								&chars, i);
	uuid = prim->att->uuid;
	dict_append_entry(&dict, "UUID", DBUS_TYPE_STRING, &uuid);

	g_free(chars);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *disconnect_service(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	struct primary *prim = data;
	GError *gerr = NULL;

	DBG("");

	if(!prim) {
		DBusMessage *reply = btd_error_failed(msg, gerr->message);
		g_error_free(gerr);
		return reply;
	}


	DBG(" %s", prim->path);
	stop_discovery(prim, NULL);
	g_attrib_unref(prim->gatt->attrib);

	return dbus_message_new_method_return(msg);
}

static GDBusMethodTable prim_methods[] = {
	{ "DiscoverCharacteristics",	"",	"ao",	discover_char,
					G_DBUS_METHOD_FLAG_ASYNC	},
	{ "RegisterCharacteristicsWatcher",	"o", "",
						register_watcher	},
	{ "UnregisterCharacteristicsWatcher",	"o", "",
						unregister_watcher	},
	{ "GetProperties",	"",	"a{sv}",prim_get_properties	},
	{ "Disconnect",	"",	"", disconnect_service	},
	{ }
};

static GSList *register_primaries(struct gatt_service *gatt, GSList *primaries)
{
	GSList *l, *paths;

	for (paths = NULL, l = primaries; l; l = l->next) {
		struct att_primary *att = l->data;
		struct primary *prim;

		prim = g_new0(struct primary, 1);
		prim->att = att;
		prim->gatt = gatt;
		prim->path = g_strdup_printf("%s/service%04x", gatt->path,
								att->start);

		g_dbus_register_interface(gatt->conn, prim->path,
				CHAR_INTERFACE, prim_methods,
				NULL, NULL, prim, NULL);

		gatt->primary = g_slist_append(gatt->primary, prim);
		paths = g_slist_append(paths, g_strdup(prim->path));

		load_characteristics(prim, gatt);
	}

	return paths;
}

GSList *attrib_client_register(DBusConnection *connection,
					struct btd_device *device, int psm,
					GAttrib *attrib, GSList *primaries)
{
	struct btd_adapter *adapter = device_get_adapter(device);
	const char *path = device_get_path(device);
	struct gatt_service *gatt;
	bdaddr_t sba, dba;

	adapter_get_address(adapter, &sba);
	device_get_address(device, &dba);

	gatt = g_new0(struct gatt_service, 1);
	gatt->dev = btd_device_ref(device);
	gatt->conn = dbus_connection_ref(connection);
	gatt->listen = FALSE;
	gatt->path = g_strdup(path);
	bacpy(&gatt->sba, &sba);
	bacpy(&gatt->dba, &dba);
	gatt->psm = psm;

	if (attrib)
		gatt->attrib = g_attrib_ref(attrib);

	gatt_services = g_slist_append(gatt_services, gatt);

	return register_primaries(gatt, primaries);
}

void attrib_client_unregister(struct btd_device *device)
{
	struct gatt_service *gatt;
	GSList *l, *lp, *lc;

	DBG("Unregister Client");

	l = g_slist_find_custom(gatt_services, device, gatt_dev_cmp);
	if (!l)
		return;

	gatt = l->data;
	gatt_services = g_slist_remove(gatt_services, gatt);

	for (lp = gatt->primary; lp; lp = lp->next) {
		struct primary *prim = lp->data;
		for (lc = prim->chars; lc; lc = lc->next) {
			struct characteristic *chr = lc->data;
			g_dbus_unregister_interface(gatt->conn, chr->path,
								CHAR_INTERFACE);
		}
		g_dbus_unregister_interface(gatt->conn, prim->path,
								CHAR_INTERFACE);
	}

	gatt_service_free(gatt);
}

void attrib_client_disconnect(struct btd_device *device) {
	GSList *l;
	struct gatt_service *gatt;

	if (gatt_services == NULL)
		return;

	DBG("");

	l = g_slist_find_custom(gatt_services, device, gatt_dev_cmp);
	if (!l)
		return;

	gatt = l->data;
	attrib_disconnect(gatt);

}
