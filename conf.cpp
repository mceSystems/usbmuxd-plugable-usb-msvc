/*
	usbmuxd - iPhone/iPod Touch USB multiplex server daemon

Copyright (C) 2013	Nikias Bassen <nikias@gmx.li>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 or version 3.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef _MSC_VER
	#include <direct.h>
	#include <dirent.h>
	#define mkdir _mkdir
	#include <Shlwapi.h>

	static char * dirname(char *path)
	{
		if ((NULL == path) || ('\0' == path[0]))
		{
			return ".";
		}

		PathRemoveFileSpec(path);
		return path;
	}
#else
	#include <dirent.h>
	#include <libgen.h>
	#include <sys/stat.h>	
#endif

#include <errno.h>

#ifdef WIN32
#include <shlobj.h>
#endif

#include "conf.h"
#include "utils.h"
#include "log.h"

#include <libimobiledevice\lockdown.h>

/* Copy a plist string into a buffer allocated by us (to prevent problems with different heaps) */
#define COPY_PLIST_STRING_VAL(node,val) char * val##_temp = NULL;\
										plist_get_string_val(node, &val##_temp);\
										*val = strdup(val##_temp);\
										plist_free_memory(val##_temp);

#ifdef WIN32
#define DIR_SEP '\\'
#define DIR_SEP_S "\\"
#else
#define DIR_SEP '/'
#define DIR_SEP_S "/"
#endif

#define CONFIG_SYSTEM_BUID_KEY "SystemBUID"
#define CONFIG_HOST_ID_KEY "HostID"

#define CONFIG_EXT ".plist"

#ifdef WIN32
	#ifdef USE_APPLE_CONFIG_DIR
		#define CONFIG_DIR "Apple"DIR_SEP_S"Lockdown"
	#elif USE_MCE_SESSION_CONFIG_DIR
		#define CONFIG_DIR ("MCE-100\\Sessions")
	#else
		#define CONFIG_DIR "Lockdown"
	#endif
#else
	#define CONFIG_DIR "lockdown"
#endif

#define CONFIG_FILE "SystemConfiguration"CONFIG_EXT

/* The pref of our local, preconfigured, lockdown config files, which contains 
 * pre-generated keys, certs and ids. */
#define LOCAL_CONFIG_FILE "etc"DIR_SEP_S"LockdownConfiguration"CONFIG_EXT

static char *__config_dir = NULL;

#ifdef WIN32
static char *config_utf16_to_utf8(wchar_t *unistr, long len, long *items_read, long *items_written)
{
	if (!unistr || (len <= 0)) return NULL;
	char *outbuf = (char*)malloc(3*(len+1));
	int p = 0;
	int i = 0;

	wchar_t wc;

	while (i < len) {
		wc = unistr[i++];
		if (wc >= 0x800) {
			outbuf[p++] = (char)(0xE0 + ((wc >> 12) & 0xF));
			outbuf[p++] = (char)(0x80 + ((wc >> 6) & 0x3F));
			outbuf[p++] = (char)(0x80 + (wc & 0x3F));
		} else if (wc >= 0x80) {
			outbuf[p++] = (char)(0xC0 + ((wc >> 6) & 0x1F));
			outbuf[p++] = (char)(0x80 + (wc & 0x3F));
		} else {
			outbuf[p++] = (char)(wc & 0x7F);
		}
	}
	if (items_read) {
		*items_read = i;
	}
	if (items_written) {
		*items_written = p;
	}
	outbuf[p] = 0;

	return outbuf;
}
#endif

int config_has_local_config()
{
	struct stat config_file_st;

	if (0 == stat(LOCAL_CONFIG_FILE, &config_file_st)) {
		if (0 != config_file_st.st_size) {
			return 1;
		}
	}

	return 0;
}

const char *config_get_config_dir()
{
	/* changed base_config_dir to be a static array */
	char base_config_dir[MAX_PATH] = { 0 };

	if (__config_dir)
		return __config_dir;

#ifdef WIN32
	#ifdef USE_APPLE_CONFIG_DIR
		wchar_t path[MAX_PATH+1];
		HRESULT hr;
		LPITEMIDLIST pidl = NULL;
		BOOL b = FALSE;

		hr = SHGetSpecialFolderLocation(NULL, CSIDL_COMMON_APPDATA, &pidl);
		if (hr == S_OK) {
			b = SHGetPathFromIDListW(pidl, path);
			if (b) {
				base_config_dir = config_utf16_to_utf8(path, wcslen(path), NULL, NULL);
				CoTaskMemFree(pidl);
			}
		}
#elif USE_MCE_SESSION_CONFIG_DIR
	if (S_OK != SHGetFolderPathA(NULL, CSIDL_COMMON_APPDATA, NULL, 0, base_config_dir))
	{
		return nullptr;
	}
#else
		/* Use the currnet user's temp folder as the base dir */
		base_config_dir = (char *)malloc(MAX_PATH + 1);
		GetTempPath(MAX_PATH + 1, base_config_dir);
#endif
#else
	#ifdef __APPLE__
		base_config_dir = strdup("/var/db");
	#else
		base_config_dir = strdup("/var/lib");
	#endif
#endif
		__config_dir = string_concat(base_config_dir, DIR_SEP_S, CONFIG_DIR, NULL);

	if (__config_dir) {
		int i = strlen(__config_dir)-1;	
		while ((i > 0) && (__config_dir[i] == DIR_SEP)) {
			__config_dir[i--] = '\0';
		}
	}

	/* changed base_config_dir to be a static array */
	//free(base_config_dir);

	usbmuxd_log(LL_DEBUG, "initialized config_dir to %s", __config_dir);

	return __config_dir;
}

static int __mkdir(const char *dir, int mode)
{
#ifdef WIN32
	return mkdir(dir);
#else
	return mkdir(dir, mode);
#endif
}

static int mkdir_with_parents(const char *dir, int mode)
{
	if (!dir) return -1;
	if (__mkdir(dir, mode) == 0) {
		return 0;
	} else {
		if (errno == EEXIST) return 0;	
	}
	int res;
	char *parent = strdup(dir);
	char* parentdir = dirname(parent);
	if (parentdir) {
		res = mkdir_with_parents(parentdir, mode);
	} else {
		res = -1;
	}
	free(parent);
	return res;
}

/**
 * Creates a freedesktop compatible configuration directory.
 */
static void config_create_config_dir(void)
{
	const char *config_path = config_get_config_dir();
	struct stat st;
	if (stat(config_path, &st) != 0) {
		mkdir_with_parents(config_path, 0755);
	}
}

static int get_rand(int min, int max)
{
	int retval = (rand() % (max - min)) + min;
	return retval;
}

static char *config_generate_uuid(int idx)
{
	char *uuid = (char *) malloc(sizeof(char) * 37);
	const char *chars = "ABCDEF0123456789";
	srand(time(NULL) - idx);
	int i = 0;

	for (i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			uuid[i] = '-';
			continue;
		} else {
			uuid[i] = chars[get_rand(0, 16)];
		}
	}
	/* make it a real string */
	uuid[36] = '\0';
	return uuid;
}

/**
 * Generates a valid BUID for this system (which is actually a UUID).
 *
 * @return A null terminated string containing a valid BUID.
 */
static char *config_generate_system_buid()
{
	return config_generate_uuid(1);
}

static int internal_set_value(const char *config_file, const char *key, plist_t value)
{
	if (!config_file)
		return 0;

	/* read file into plist */
	plist_t config = NULL;

	plist_read_from_filename(&config, config_file);
	if (!config) {
		config = plist_new_dict();
		plist_dict_set_item(config, key, value);
	} else {
		plist_t n = plist_dict_get_item(config, key);
		if (n) {
			plist_dict_remove_item(config, key);
		}
		plist_dict_set_item(config, key, value);
		remove(config_file);
	}

	/* store in config file */
	char *value_string = NULL;
	if (plist_get_node_type(value) == PLIST_STRING) {
		plist_get_string_val(value, &value_string);
		usbmuxd_log(LL_DEBUG, "setting key %s to %s in config_file %s", key, value_string, config_file);
		if (value_string)
			plist_free_memory(value_string);
	} else {
		usbmuxd_log(LL_DEBUG, "setting key %s in config_file %s", key, config_file);
	}

	plist_write_to_filename(config, config_file, PLIST_FORMAT_XML);

	plist_free(config);

	return 1;
}

static int config_set_value(const char *key, plist_t value)
{
	const char *config_path = NULL;
	char *config_file = NULL;

	if (config_has_local_config()) {
		return internal_set_value(LOCAL_CONFIG_FILE, key, value);
	}
	
	/* Make sure config directory exists */
	config_create_config_dir();

	config_path = config_get_config_dir();
	config_file = string_concat(config_path, DIR_SEP_S, CONFIG_FILE, NULL);
	
	int result = internal_set_value(config_file, key, value);

	free(config_file);

	return result;
}

static int internal_get_value(const char* config_file, const char *key, plist_t *value)
{
	*value = NULL;

	/* now parse file to get the SystemBUID */
	plist_t config = NULL;
	if (plist_read_from_filename(&config, config_file)) {
		usbmuxd_log(LL_DEBUG, "reading key %s from config_file %s", key, config_file);
		plist_t n = plist_dict_get_item(config, key);
		if (n) {
			*value = plist_copy(n);
			plist_free(n);
			n = NULL;
		}
	}
	plist_free(config);

	return 1;
}

static int config_get_value(const char *key, plist_t *value)
{
	const char *config_path = NULL;
	char *config_file = NULL;

	if (config_has_local_config()) {
		return internal_get_value(LOCAL_CONFIG_FILE, key, value);
	}
	
	config_path = config_get_config_dir();
	config_file = string_concat(config_path, DIR_SEP_S, CONFIG_FILE, NULL);
	
	int result = internal_get_value(config_file, key, value);

	free(config_file);

	return result;
}

/**
 * Store SystemBUID in config file.
 *
 * @param system_buid A null terminated string containing a valid SystemBUID.
 */
static int config_set_system_buid(const char *system_buid)
{
	return config_set_value(CONFIG_SYSTEM_BUID_KEY, plist_new_string(system_buid));
}

/**
 * Determines whether a pairing record is present for the given device.
 *
 * @param udid The device UDID as given by the device.
 *
 * @return 1 if there's a pairing record for the given udid or 0 otherwise.
 */
int config_has_device_record(const char *udid)
{
	int res = 0;
	if (!udid) return 0;

	/* ensure config directory exists */
	config_create_config_dir();

	/* build file path */
	const char *config_path = config_get_config_dir();
	char *device_record_file = string_concat(config_path, DIR_SEP_S, udid, CONFIG_EXT, NULL);

	struct stat st;

	if ((stat(device_record_file, &st) == 0) && S_ISREG(st.st_mode))
		res = 1;

	free(device_record_file);

	return res;
}

/**
 * Reads the BUID from a previously generated configuration file.
 *
 * @param system_buid pointer to a variable that will be set to point to a
 *     newly allocated string containing the BUID.
 *
 * @note It is the responsibility of the calling function to free the returned system_buid
 */
void config_get_system_buid(char **system_buid)
{
	plist_t value = NULL;

	config_get_value(CONFIG_SYSTEM_BUID_KEY, &value);

	if (value && (plist_get_node_type(value) == PLIST_STRING)) {
		COPY_PLIST_STRING_VAL(value, system_buid);
		usbmuxd_log(LL_DEBUG, "got %s %s", CONFIG_SYSTEM_BUID_KEY, *system_buid);
	}

	if (value)
		plist_free(value);

	if (!*system_buid) {
		/* no config, generate system_buid */
		usbmuxd_log(LL_DEBUG, "no previous %s found", CONFIG_SYSTEM_BUID_KEY);
		*system_buid = config_generate_system_buid();
		config_set_system_buid(*system_buid);
	}

	usbmuxd_log(LL_DEBUG, "using %s as %s", *system_buid, CONFIG_SYSTEM_BUID_KEY);
}

/**
 * Store a pairing record for the given device identifier.
 *
 * @param udid device identifier
 * @param record_data buffer containing a pairing record
 * @param record_size size of buffer passed in record_data
 *
 * @return 0 on success or a negative errno otherwise.
 */
int config_set_device_record(const char *udid, char* record_data, uint64_t record_size)
{
	int res = 0;

	if (!udid || !record_data || record_size < 8)
		return -EINVAL;

	plist_t plist = NULL;
	if (memcmp(record_data, "bplist00", 8) == 0) {
		plist_from_bin(record_data, record_size, &plist);
	} else {
		plist_from_xml(record_data, record_size, &plist);
	}

	if (!plist || plist_get_node_type(plist) != PLIST_DICT) {
		if (plist)
			plist_free(plist);
		return -EINVAL;
	}

	/* ensure config directory exists */
	config_create_config_dir();

	/* build file path */
	const char *config_path = config_get_config_dir();
	char *device_record_file = string_concat(config_path, DIR_SEP_S, udid, CONFIG_EXT, NULL);

	remove(device_record_file);

	/* store file */
	if (!plist_write_to_filename(plist, device_record_file, PLIST_FORMAT_XML)) {
		usbmuxd_log(LL_DEBUG, "could not open '%s' for writing: %s", device_record_file, strerror(errno));
		res = -ENOENT;
	}
	free(device_record_file);
	if (plist)
		plist_free(plist);

	return res;
}

/**
 * Retrieve a pairing record for the given device identifier
 *
 * @param udid device identifier
 * @param record_data pointer to a variable that will be set to point to a
 *     newly allocated buffer holding the pairing record
 * @param record_size pointer to a variable that will be set to the size
 *     of the buffer given in record_data.
 *
 * @return 0 on success or a negative errno otherwise.
 */
int config_get_device_record(const char *udid, char **record_data, uint64_t *record_size)
{
	int res = 0;

	/* ensure config directory exists */
	config_create_config_dir();

	/* build file path */
	const char *config_path = config_get_config_dir();
	char *device_record_file = string_concat(config_path, DIR_SEP_S, udid, CONFIG_EXT, NULL);

	/* read file */
	buffer_read_from_filename(device_record_file, record_data, record_size);
	if (!*record_data) {
		usbmuxd_log(LL_ERROR, "%s: failed to read '%s': %s", __func__, device_record_file, strerror(errno));
		res = -ENOENT;
	}
	free(device_record_file);

	return res;
}

/**
 * Remove the pairing record stored for a device from this host.
 *
 * @param udid The udid of the device
 *
 * @return 0 on success or a negative errno otherwise.
 */
int config_remove_device_record(const char *udid)
{
	int res = 0;

	/* build file path */
	const char *config_path = config_get_config_dir();
	char *device_record_file = string_concat(config_path, DIR_SEP_S, udid, CONFIG_EXT, NULL);

	/* remove file */
	if (remove(device_record_file) != 0) {
		res = -errno;
		usbmuxd_log(LL_DEBUG, "could not remove %s: %s", device_record_file, strerror(errno));
	}

	free(device_record_file);

	return res;
}

static int config_device_record_get_value(const char *udid, const char *key, plist_t *value)
{
	const char *config_path = NULL;
	char *config_file = NULL;

	config_path = config_get_config_dir();
	config_file = string_concat(config_path, DIR_SEP_S, udid, CONFIG_EXT, NULL);

	int result = internal_get_value(config_file, key, value);

	free(config_file);

	return result;
}

void config_device_record_get_host_id(const char *udid, char **host_id)
{
	plist_t value = NULL;

	config_device_record_get_value(udid, CONFIG_HOST_ID_KEY, &value);

	if (value && (plist_get_node_type(value) == PLIST_STRING)) {
		COPY_PLIST_STRING_VAL(value, host_id);
	}
	else {
		/* Try to get it from the local config */
		config_get_value(CONFIG_HOST_ID_KEY, &value);
		if (value && (plist_get_node_type(value) == PLIST_STRING)) {
			COPY_PLIST_STRING_VAL(value, host_id);
		}
	}

	if (value)
		plist_free(value);

	if (!*host_id) {
		usbmuxd_log(LL_ERROR, "%s: ERROR couldn't get HostID from pairing record for udid %s", __func__, udid);
	}
}
