/*
 *
 *  Connection Manager
 *
 *  Copyright (C) 2012  BMW Car IT GmbH. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
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

#include <string.h>
#include <errno.h>

#include <gdbus.h>

#define CONNMAN_API_SUBJECT_TO_CHANGE
#include <connman/plugin.h>
#include <connman/device.h>
#include <connman/network.h>
#include <connman/inet.h>
#include <connman/dbus.h>

#define DUNDEE_SERVICE			"org.ofono.dundee"
#define DUNDEE_MANAGER_INTERFACE	DUNDEE_SERVICE ".Manager"
#define DUNDEE_DEVICE_INTERFACE		DUNDEE_SERVICE ".Device"

#define DEVICE_ADDED			"DeviceAdded"
#define DEVICE_REMOVED			"DeviceRemoved"
#define PROPERTY_CHANGED		"PropertyChanged"

#define GET_PROPERTIES			"GetProperties"
#define GET_DEVICES			"GetDevices"

#define TIMEOUT 40000

static DBusConnection *connection;

static GHashTable *dundee_devices = NULL;

struct dundee_data {
	char *path;
	char *name;

	struct connman_device *device;
	struct connman_network *network;

	connman_bool_t active;

	int index;

	/* IPv4 Settings */
	enum connman_ipconfig_method method;
	struct connman_ipaddress *address;
	char *nameservers;
};

static char *get_ident(const char *path)
{
	char *pos;

	if (*path != '/')
		return NULL;

	pos = strrchr(path, '/');
	if (pos == NULL)
		return NULL;

	return pos + 1;
}

static void create_device(struct dundee_data *info)
{
	struct connman_device *device;
	char *ident;

	DBG("%s", info->path);

	ident = g_strdup(get_ident(info->path));
	device = connman_device_create(ident, CONNMAN_DEVICE_TYPE_BLUETOOTH);
	if (device == NULL)
		goto out;

	DBG("device %p", device);

	connman_device_set_ident(device, ident);

	connman_device_set_string(device, "Path", info->path);

	connman_device_set_data(device, info);

	if (connman_device_register(device) < 0) {
		connman_error("Failed to register DUN device");
		connman_device_unref(device);
		goto out;
	}

	info->device = device;

out:
	g_free(ident);
}

static void destroy_device(struct dundee_data *info)
{
	connman_device_set_powered(info->device, FALSE);

	if (info->network != NULL) {
		connman_device_remove_network(info->device, info->network);
		connman_network_unref(info->network);
		info->network = NULL;
	}

	connman_device_unregister(info->device);
	connman_device_unref(info->device);

	info->device = NULL;
}

static void device_destroy(gpointer data)
{
	struct dundee_data *info = data;

	if (info->device != NULL)
		destroy_device(info);

	g_free(info->path);
	g_free(info->name);

	g_free(info);
}

static void create_network(struct dundee_data *info)
{
	struct connman_network *network;
	const char *group;

	DBG("%s", info->path);

	network = connman_network_create(info->path,
				CONNMAN_NETWORK_TYPE_BLUETOOTH_DUN);
	if (network == NULL)
		return;

	DBG("network %p", network);

	connman_network_set_data(network, info);

	connman_network_set_string(network, "Path",
				info->path);

	connman_network_set_name(network, info->name);

	group = get_ident(info->path);
	connman_network_set_group(network, group);

	connman_network_set_available(network, TRUE);

	if (connman_device_add_network(info->device, network) < 0) {
		connman_network_unref(network);
		return;
	}

	info->network = network;
}

static void set_connected(struct dundee_data *info)
{
	DBG("%s", info->path);

	connman_inet_ifup(info->index);

	connman_network_set_index(info->network, info->index);
	connman_network_set_ipv4_method(info->network,
					CONNMAN_IPCONFIG_METHOD_FIXED);
	connman_network_set_ipaddress(info->network, info->address);
	connman_network_set_nameservers(info->network, info->nameservers);

	connman_network_set_connected(info->network, TRUE);
}

static void set_disconnected(struct dundee_data *info)
{
	DBG("%s", info->path);

	connman_network_set_connected(info->network, FALSE);
	connman_inet_ifdown(info->index);
}

static int network_probe(struct connman_network *network)
{
	DBG("network %p", network);

	return 0;
}

static void network_remove(struct connman_network *network)
{
	DBG("network %p", network);
}

static int network_connect(struct connman_network *network)
{
	DBG("network %p", network);

	return 0;
}

static int network_disconnect(struct connman_network *network)
{
	DBG("network %p", network);

	return 0;
}

static struct connman_network_driver network_driver = {
	.name		= "network",
	.type		= CONNMAN_NETWORK_TYPE_BLUETOOTH_DUN,
	.probe		= network_probe,
	.remove		= network_remove,
	.connect	= network_connect,
	.disconnect	= network_disconnect,
};

static int dundee_probe(struct connman_device *device)
{
	DBG("device %p", device);

	return 0;
}

static void dundee_remove(struct connman_device *device)
{
	DBG("device %p", device);
}

static int dundee_enable(struct connman_device *device)
{
	DBG("device %p", device);

	return 0;
}

static int dundee_disable(struct connman_device *device)
{
	DBG("device %p", device);

	return 0;
}

static struct connman_device_driver dundee_driver = {
	.name		= "dundee",
	.type		= CONNMAN_DEVICE_TYPE_BLUETOOTH,
	.probe		= dundee_probe,
	.remove		= dundee_remove,
	.enable		= dundee_enable,
	.disable	= dundee_disable,
};

static char *extract_nameservers(DBusMessageIter *array)
{
	DBusMessageIter entry;
	char *nameservers = NULL;
	char *tmp;

	dbus_message_iter_recurse(array, &entry);

	while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING) {
		const char *nameserver;

		dbus_message_iter_get_basic(&entry, &nameserver);

		if (nameservers == NULL) {
			nameservers = g_strdup(nameserver);
		} else {
			tmp = nameservers;
			nameservers = g_strdup_printf("%s %s", tmp, nameserver);
			g_free(tmp);
		}

		dbus_message_iter_next(&entry);
	}

	return nameservers;
}

static void extract_settings(DBusMessageIter *array,
				struct dundee_data *info)
{
	DBusMessageIter dict;
	char *address = NULL, *gateway = NULL;
	char *nameservers = NULL;
	const char *interface = NULL;
	int index = -1;

	if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
		return;

	dbus_message_iter_recurse(array, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key, *val;

		dbus_message_iter_recurse(&dict, &entry);
		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		if (g_str_equal(key, "Interface") == TRUE) {
			dbus_message_iter_get_basic(&value, &interface);

			DBG("Interface %s", interface);

			index = connman_inet_ifindex(interface);

			DBG("index %d", index);

			if (index < 0)
				break;
		} else if (g_str_equal(key, "Address") == TRUE) {
			dbus_message_iter_get_basic(&value, &val);

			address = g_strdup(val);

			DBG("Address %s", address);
		} else if (g_str_equal(key, "DomainNameServers") == TRUE) {
			nameservers = extract_nameservers(&value);

			DBG("Nameservers %s", nameservers);
		} else if (g_str_equal(key, "Gateway") == TRUE) {
			dbus_message_iter_get_basic(&value, &val);

			gateway = g_strdup(val);

			DBG("Gateway %s", gateway);
		}

		dbus_message_iter_next(&dict);
	}

	if (index < 0)
		goto out;

	info->address = connman_ipaddress_alloc(CONNMAN_IPCONFIG_TYPE_IPV4);
	if (info->address == NULL)
		goto out;

	info->index = index;
	connman_ipaddress_set_ipv4(info->address, address, NULL, gateway);

	info->nameservers = nameservers;

out:
	if (info->nameservers != nameservers)
		g_free(nameservers);

	g_free(address);
	g_free(gateway);
}

static gboolean device_changed(DBusConnection *connection,
				DBusMessage *message,
				void *user_data)
{
	const char *path = dbus_message_get_path(message);
	struct dundee_data *info = NULL;
	DBusMessageIter iter, value;
	const char *key;
	const char *signature =	DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_VARIANT_AS_STRING;

	if (dbus_message_has_signature(message, signature) == FALSE) {
		connman_error("dundee signature does not match");
		return TRUE;
	}

	info = g_hash_table_lookup(dundee_devices, path);
	if (info == NULL)
		return TRUE;

	if (dbus_message_iter_init(message, &iter) == FALSE)
		return TRUE;

	dbus_message_iter_get_basic(&iter, &key);

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &value);

	/*
	 * Dundee guarantees the ordering of Settings and
	 * Active. Settings will always be send before Active = True.
	 * That means we don't have to order here.
	 */
	if (g_str_equal(key, "Active") == TRUE) {
		dbus_message_iter_get_basic(&value, &info->active);

		DBG("%s Active %d", info->path, info->active);

		if (info->active == TRUE)
			set_connected(info);
		else
			set_disconnected(info);
	} else if (g_str_equal(key, "Settings") == TRUE) {
		DBG("%s Settings", info->path);

		extract_settings(&value, info);
	} else if (g_str_equal(key, "Name") == TRUE) {
		char *name;

		dbus_message_iter_get_basic(&value, &name);

		g_free(info->name);
		info->name = g_strdup(name);

		DBG("%s Name %s", info->path, info->name);

		connman_network_set_name(info->network, info->name);
		connman_network_update(info->network);
	}

	return TRUE;
}

static void add_device(const char *path, DBusMessageIter *properties)
{
	struct dundee_data *info;

	info = g_hash_table_lookup(dundee_devices, path);
	if (info != NULL)
		return;

	info = g_try_new0(struct dundee_data, 1);
	if (info == NULL)
		return;

	info->path = g_strdup(path);

	while (dbus_message_iter_get_arg_type(properties) ==
			DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter entry, value;
		const char *key;

		dbus_message_iter_recurse(properties, &entry);
		dbus_message_iter_get_basic(&entry, &key);

		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);

		if (g_str_equal(key, "Active") == TRUE) {
			dbus_message_iter_get_basic(&value, &info->active);

			DBG("%s Active %d", info->path, info->active);
		} else if (g_str_equal(key, "Settings") == TRUE) {
			DBG("%s Settings", info->path);

			extract_settings(&value, info);
		} else if (g_str_equal(key, "Name") == TRUE) {
			char *name;

			dbus_message_iter_get_basic(&value, &name);

			info->name = g_strdup(name);

			DBG("%s Name %s", info->path, info->name);
		}

		dbus_message_iter_next(properties);
	}

	g_hash_table_insert(dundee_devices, g_strdup(path), info);

	create_device(info);
	create_network(info);

	if (info->active == TRUE)
		set_connected(info);
}

static gboolean device_added(DBusConnection *connection, DBusMessage *message,
				void *user_data)
{
	DBusMessageIter iter, properties;
	const char *path;
	const char *signature = DBUS_TYPE_OBJECT_PATH_AS_STRING
		DBUS_TYPE_ARRAY_AS_STRING
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_VARIANT_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING;

	if (dbus_message_has_signature(message, signature) == FALSE) {
		connman_error("dundee signature does not match");
		return TRUE;
	}

	DBG("");

	if (dbus_message_iter_init(message, &iter) == FALSE)
		return TRUE;

	dbus_message_iter_get_basic(&iter, &path);

	dbus_message_iter_next(&iter);
	dbus_message_iter_recurse(&iter, &properties);

	add_device(path, &properties);

	return TRUE;
}

static void remove_device(DBusConnection *connection, const char *path)
{
	DBG("path %s", path);

	g_hash_table_remove(dundee_devices, path);
}

static gboolean device_removed(DBusConnection *connection, DBusMessage *message,
				void *user_data)
{
	const char *path;
	const char *signature = DBUS_TYPE_OBJECT_PATH_AS_STRING;

	if (dbus_message_has_signature(message, signature) == FALSE) {
		connman_error("dundee signature does not match");
		return TRUE;
	}

	dbus_message_get_args(message, NULL, DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);
	remove_device(connection, path);
	return TRUE;
}

static void manager_get_devices_reply(DBusPendingCall *call, void *user_data)
{
	DBusMessage *reply;
	DBusError error;
	DBusMessageIter array, dict;
	const char *signature = DBUS_TYPE_ARRAY_AS_STRING
		DBUS_STRUCT_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_OBJECT_PATH_AS_STRING
		DBUS_TYPE_ARRAY_AS_STRING
		DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
		DBUS_TYPE_STRING_AS_STRING
		DBUS_TYPE_VARIANT_AS_STRING
		DBUS_DICT_ENTRY_END_CHAR_AS_STRING
		DBUS_STRUCT_END_CHAR_AS_STRING;

	DBG("");

	reply = dbus_pending_call_steal_reply(call);

	if (dbus_message_has_signature(reply, signature) == FALSE) {
		connman_error("dundee signature does not match");
		goto done;
	}

	dbus_error_init(&error);

	if (dbus_set_error_from_message(&error, reply) == TRUE) {
		connman_error("%s", error.message);
		dbus_error_free(&error);
		goto done;
	}

	if (dbus_message_iter_init(reply, &array) == FALSE)
		goto done;

	dbus_message_iter_recurse(&array, &dict);

	while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_STRUCT) {
		DBusMessageIter value, properties;
		const char *path;

		dbus_message_iter_recurse(&dict, &value);
		dbus_message_iter_get_basic(&value, &path);

		dbus_message_iter_next(&value);
		dbus_message_iter_recurse(&value, &properties);

		add_device(path, &properties);

		dbus_message_iter_next(&dict);
	}

done:
	dbus_message_unref(reply);

	dbus_pending_call_unref(call);
}

static int manager_get_devices(void)
{
	DBusMessage *message;
	DBusPendingCall *call;

	DBG("");

	message = dbus_message_new_method_call(DUNDEE_SERVICE, "/",
					DUNDEE_MANAGER_INTERFACE, GET_DEVICES);
	if (message == NULL)
		return -ENOMEM;

	if (dbus_connection_send_with_reply(connection, message,
						&call, TIMEOUT) == FALSE) {
		connman_error("Failed to call GetDevices()");
		dbus_message_unref(message);
		return -EINVAL;
	}

	if (call == NULL) {
		connman_error("D-Bus connection not available");
		dbus_message_unref(message);
		return -EINVAL;
	}

	dbus_pending_call_set_notify(call, manager_get_devices_reply,
					NULL, NULL);

	dbus_message_unref(message);

	return -EINPROGRESS;
}

static void dundee_connect(DBusConnection *connection, void *user_data)
{
	DBG("connection %p", connection);

	dundee_devices = g_hash_table_new_full(g_str_hash, g_str_equal,
					g_free, device_destroy);

	manager_get_devices();
}

static void dundee_disconnect(DBusConnection *connection, void *user_data)
{
	DBG("connection %p", connection);

	g_hash_table_destroy(dundee_devices);
	dundee_devices = NULL;
}

static guint watch;
static guint added_watch;
static guint removed_watch;
static guint device_watch;

static int dundee_init(void)
{
	int err;

	connection = connman_dbus_get_connection();
	if (connection == NULL)
		return -EIO;

	watch = g_dbus_add_service_watch(connection, DUNDEE_SERVICE,
			dundee_connect, dundee_disconnect, NULL, NULL);

	added_watch = g_dbus_add_signal_watch(connection, NULL, NULL,
						DUNDEE_MANAGER_INTERFACE,
						DEVICE_ADDED, device_added,
						NULL, NULL);

	removed_watch = g_dbus_add_signal_watch(connection, NULL, NULL,
						DUNDEE_MANAGER_INTERFACE,
						DEVICE_REMOVED, device_removed,
						NULL, NULL);

	device_watch = g_dbus_add_signal_watch(connection, NULL, NULL,
						DUNDEE_DEVICE_INTERFACE,
						PROPERTY_CHANGED,
						device_changed,
						NULL, NULL);


	if (watch == 0 || added_watch == 0 || removed_watch == 0 ||
			device_watch == 0) {
		err = -EIO;
		goto remove;
	}

	err = connman_network_driver_register(&network_driver);
	if (err < 0)
		goto remove;

	err = connman_device_driver_register(&dundee_driver);
	if (err < 0) {
		connman_network_driver_unregister(&network_driver);
		goto remove;
	}

	return 0;

remove:
	g_dbus_remove_watch(connection, watch);
	g_dbus_remove_watch(connection, added_watch);
	g_dbus_remove_watch(connection, removed_watch);
	g_dbus_remove_watch(connection, device_watch);

	dbus_connection_unref(connection);

	return err;
}

static void dundee_exit(void)
{
	g_dbus_remove_watch(connection, watch);
	g_dbus_remove_watch(connection, added_watch);
	g_dbus_remove_watch(connection, removed_watch);
	g_dbus_remove_watch(connection, device_watch);

	connman_device_driver_unregister(&dundee_driver);
	connman_network_driver_unregister(&network_driver);

	dbus_connection_unref(connection);
}

CONNMAN_PLUGIN_DEFINE(dundee, "Dundee plugin", VERSION,
		CONNMAN_PLUGIN_PRIORITY_DEFAULT, dundee_init, dundee_exit)
