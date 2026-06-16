#include "mpris.h"
#include <dbus/dbus.h>
#include <string.h>
#include <stdio.h>

static DBusConnection *conn = NULL;
static char player_name[256] = "";

int mpris_init(void) {
    DBusError err;
    dbus_error_init(&err);
    conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!conn) {
        fprintf(stderr, "[mpris] dbus_bus_get: %s\n", err.message);
        dbus_error_free(&err);
        return 0;
    }
    return 1;
}

void mpris_cleanup(void) {
    if (conn) {
        dbus_connection_unref(conn);
        conn = NULL;
    }
}

/* find the first running MPRIS player */
static int find_player(void) {
    player_name[0] = 0;

    DBusError err;
    dbus_error_init(&err);

    DBusMessage *msg = dbus_message_new_method_call(
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "ListNames");
    if (!msg) return 0;

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 2000, &err);
    dbus_message_unref(msg);
    if (!reply) { dbus_error_free(&err); return 0; }

    DBusMessageIter iter;
    dbus_message_iter_init(reply, &iter);
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return 0;
    }

    DBusMessageIter arr;
    dbus_message_iter_recurse(&iter, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        char *name;
        dbus_message_iter_get_basic(&arr, &name);
        if (strstr(name, "org.mpris.MediaPlayer2.")) {
            strncpy(player_name, name, sizeof(player_name) - 1);
            dbus_message_unref(reply);
            return 1;
        }
        dbus_message_iter_next(&arr);
    }

    dbus_message_unref(reply);
    return 0;
}

void mpris_poll(TrackInfo *info) {
    info->valid = 0;
    info->title[0] = 0;
    info->artist[0] = 0;
    info->album[0] = 0;

    if (!conn) return;

    /* find a player if we don't have one cached */
    if (!player_name[0]) {
        if (!find_player()) return;
    }

    DBusError err;
    dbus_error_init(&err);

    DBusMessage *msg = dbus_message_new_method_call(
        player_name,
        "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties",
        "Get");
    if (!msg) return;

    const char *iface = "org.mpris.MediaPlayer2.Player";
    const char *prop  = "Metadata";
    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &prop,
        DBUS_TYPE_INVALID);

    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 2000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        player_name[0] = 0; /* force re-scan next time */
        return;
    }

    /* reply is Variant containing a{sv} */
    DBusMessageIter root, var, dict;
    dbus_message_iter_init(reply, &root);
    if (dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_VARIANT) {
        dbus_message_unref(reply);
        return;
    }
    dbus_message_iter_recurse(&root, &var);
    if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return;
    }
    dbus_message_iter_recurse(&var, &dict);

    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, valvar, val;
        dbus_message_iter_recurse(&dict, &entry);

        char *key;
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);

        dbus_message_iter_recurse(&entry, &valvar);

        if (strcmp(key, "xesam:title") == 0) {
            char *title = NULL;
            if (dbus_message_iter_get_arg_type(&valvar) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&valvar, &title);
            if (title) {
                strncpy(info->title, title, sizeof(info->title) - 1);
                info->valid = 1;
            }
        } else if (strcmp(key, "xesam:artist") == 0) {
            if (dbus_message_iter_get_arg_type(&valvar) == DBUS_TYPE_ARRAY) {
                dbus_message_iter_recurse(&valvar, &val);
                int pos = 0;
                while (dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_STRING && pos < (int)sizeof(info->artist) - 2) {
                    char *a;
                    dbus_message_iter_get_basic(&val, &a);
                    int alen = strlen(a);
                    if (pos > 0 && pos + 2 < (int)sizeof(info->artist)) {
                        info->artist[pos++] = ',';
                        info->artist[pos++] = ' ';
                    }
                    if (pos + alen < (int)sizeof(info->artist)) {
                        memcpy(info->artist + pos, a, alen);
                        pos += alen;
                    }
                    dbus_message_iter_next(&val);
                }
                info->artist[pos] = 0;
            }
        } else if (strcmp(key, "xesam:album") == 0) {
            char *album = NULL;
            if (dbus_message_iter_get_arg_type(&valvar) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&valvar, &album);
            if (album)
                strncpy(info->album, album, sizeof(info->album) - 1);
        }

        dbus_message_iter_next(&dict);
    }

    dbus_message_unref(reply);
}
