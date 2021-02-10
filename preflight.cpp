/*
        usbmuxd - iPhone/iPod Touch USB multiplex server daemon

Copyright (C) 2013      Nikias Bassen <nikias@gmx.li>

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

#include <stdlib.h>
#include <string.h>


#ifdef _MSC_VER
	#include <WinSock2.h>
#else
	#include <unistd.h>
	#include <sys/time.h>
#endif

#include <pthread.h>

#ifdef HAVE_LIBIMOBILEDEVICE
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/notification_proxy.h>
#endif

#ifdef _MSC_VER
	#define sleep(x) Sleep((x) * 1000)
#endif

#include "preflight.h"
#include "device.h"
#include "client.h"
#include "conf.h"
#include "log.h"

#define DEVICE_MONITOR_CHANGE_TIMEOUT (3000)
#define LOCKDOWN_RECREATION_ATTEMPTS (3)

#ifdef HAVE_LIBIMOBILEDEVICE
enum connection_type {
	CONNECTION_USBMUXD = 1
};

struct idevice_private {
	char *udid;
	uint32_t mux_id;
	enum connection_type conn_type;
	void *conn_data;
	int version;
};

struct cb_data {
	idevice_t dev;
	np_client_t np;
	//pthread_mutex_t pairing_lock;
	int version_major;
	int is_device_connected;
	//int is_paired;
	HANDLE is_paired_signal;
};

#ifndef IS_VALID_HANDLE
#define IS_VALID_HANDLE(h) ((NULL != (h)) && (INVALID_HANDLE_VALUE != (h)))
#endif

#define IS_PAIRED(pcb) (WAIT_OBJECT_0 == WaitForSingleObject((pcb)->is_paired_signal, 0))

#define IOS_8_SIGNAL_IS_PAIRED(pcb) if (8 <= (pcb)->version_major) {\
										SetEvent((pcb)->is_paired_signal);\
									}

static void lockdownd_set_untrusted_host_buid(lockdownd_client_t lockdown)
{
	char* system_buid = NULL;
	config_get_system_buid(&system_buid);
	usbmuxd_log(LL_DEBUG, "%s: Setting UntrustedHostBUID to %s", __func__, system_buid);
	lockdownd_set_value(lockdown, NULL, "UntrustedHostBUID", plist_new_string(system_buid));
	free(system_buid);
}

void preflight_device_remove_cb(void *data)
{
	if (!data)
		return;
	struct cb_data *cbdata = (struct cb_data*)data;
	cbdata->is_device_connected = 0;
}

static void np_callback(const char* notification, void* userdata)
{
	usbmuxd_log(LL_INFO, "%s: in", __func__);
	struct cb_data *cbdata = (struct cb_data*)userdata;
	idevice_t dev = cbdata->dev;
	struct idevice_private *_dev = (struct idevice_private*)dev;

	lockdownd_client_t lockdown = NULL;
	lockdownd_error_t lerr;

	if (strlen(notification) == 0) {
		cbdata->np = NULL;
		return;
	}

	if (strcmp(notification, "com.apple.mobile.lockdown.request_pair") == 0) {
		//pthread_mutex_lock(&(cbdata->pairing_lock));
		usbmuxd_log(LL_INFO, "%s: user trusted this computer on device %s, pairing now", __func__, _dev->udid);

		/* TMP */
		/*if (8 == cbdata->version_major) {
			cbdata->is_paired = 1;
		}*/

		lerr = lockdownd_client_new(dev, &lockdown, "usbmuxd");
		if (lerr != LOCKDOWN_E_SUCCESS) {
			usbmuxd_log(LL_ERROR, "%s: ERROR: Could not connect to lockdownd on device %s, lockdown error %d", __func__, _dev->udid, lerr);
			//pthread_mutex_unlock(&(cbdata->pairing_lock));
			IOS_8_SIGNAL_IS_PAIRED(cbdata);
			return;
		}

		lerr = lockdownd_pair(lockdown, NULL);
		if (lerr != LOCKDOWN_E_SUCCESS) {
			usbmuxd_log(LL_ERROR, "%s: ERROR: Pair failed for device %s, lockdown error %d", __func__, _dev->udid, lerr);
			lockdownd_client_free(lockdown);
			//pthread_mutex_unlock(&(cbdata->pairing_lock));
			IOS_8_SIGNAL_IS_PAIRED(cbdata);
			return;
		}
		lockdownd_client_free(lockdown);
		
		/* On iOS devices prior to iOS 8, the device will reconnect by itself at this point.
		 * On iOS 8, the pairing seems to complete without it. */
		//pthread_mutex_unlock(&(cbdata->pairing_lock));
		IOS_8_SIGNAL_IS_PAIRED(cbdata);

	} else if (strcmp(notification, "com.apple.mobile.lockdown.request_host_buid") == 0) {
		lerr = lockdownd_client_new(cbdata->dev, &lockdown, "usbmuxd_preflight");
		if (lerr != LOCKDOWN_E_SUCCESS) {
			usbmuxd_log(LL_ERROR, "%s: ERROR: Could not connect to lockdownd on device %s, lockdown error %d", __func__, _dev->udid, lerr);
		} else {
			lockdownd_set_untrusted_host_buid(lockdown);
			lockdownd_client_free(lockdown);
		}
	}
}

static int itunes_setup_has_completed(lockdownd_client_t lockdown)
{
	plist_t setup_completed_node = NULL;
	int ret = 0;
	if (LOCKDOWN_E_SUCCESS == lockdownd_get_value(lockdown, "com.apple.mobile.iTunes", "iTunesSetupComplete", &setup_completed_node)) {
		if (setup_completed_node && plist_get_node_type(setup_completed_node) == PLIST_BOOLEAN) {
			uint8_t setup_completed = 0;
			plist_get_bool_val(setup_completed_node, &setup_completed);

			ret = (int)setup_completed;
		}

		plist_free(setup_completed_node);
	}

	return ret;
}

#ifdef _MSC_VER
static void preflight_worker_handle_device_add(void* userdata)
#else
static void* preflight_worker_handle_device_add(void* userdata)
#endif
{
	struct device_info *info = (struct device_info*)userdata;
	struct idevice_private *_dev = (struct idevice_private*)malloc(sizeof(struct idevice_private));
	_dev->udid = strdup(info->serial);
	_dev->mux_id = info->id;
	_dev->conn_type = CONNECTION_USBMUXD;
	_dev->conn_data = NULL;
	_dev->version = 0;


	idevice_t dev = (idevice_t)_dev;

	lockdownd_client_t lockdown = NULL;
	lockdownd_error_t lerr;

	plist_t value = NULL;
	char* version_str = NULL;

	usbmuxd_log(LL_INFO, "%s: Starting preflight on device %s...", __func__, _dev->udid);

retry:
	lerr = lockdownd_client_new(dev, &lockdown, "usbmuxd");
	if (lerr != LOCKDOWN_E_SUCCESS) {
		usbmuxd_log(LL_ERROR, "%s: ERROR: Could not connect to lockdownd on device %s, lockdown error %d", __func__, _dev->udid, lerr);
		goto leave;
	}

	char *type = NULL;
	lerr = lockdownd_query_type(lockdown, &type);
	if (!type) {
		usbmuxd_log(LL_ERROR, "%s: ERROR: Could not get lockdownd type from device %s, lockdown error %d", __func__, _dev->udid, lerr);
		goto leave;
	}

	if (strcmp(type, "com.apple.mobile.lockdown") != 0) {
		// make restore mode devices visible
		libimobiledevice_free(type);
		usbmuxd_log(LL_INFO, "%s: Finished preflight on device %s", __func__, _dev->udid);
		client_device_add(info);
		goto leave;
	}
	libimobiledevice_free(type);

	/* Try to "pre pair" with the device */
	if (config_has_local_config()) {
		lerr = lockdownd_validate_pair_force_store(lockdown, NULL);
	} else {
		lerr = config_has_device_record(dev->udid) ? LOCKDOWN_E_SUCCESS : LOCKDOWN_E_INVALID_HOST_ID;
	}

	int is_device_paired = 0;
	char *host_id = NULL;
	if (LOCKDOWN_E_SUCCESS == lerr) {
		config_device_record_get_host_id(dev->udid, &host_id);
		lerr = lockdownd_start_session(lockdown, host_id, NULL, NULL);
		if (host_id)
			free(host_id);
		if (lerr == LOCKDOWN_E_SUCCESS) {
			usbmuxd_log(LL_INFO, "%s: StartSession success for device %s", __func__, _dev->udid);
			usbmuxd_log(LL_INFO, "%s: Finished preflight on device %s", __func__, _dev->udid);
			client_device_add(info);
			goto leave;
		}

		usbmuxd_log(LL_INFO, "%s: StartSession failed on device %s, lockdown error %d", __func__, _dev->udid, lerr);
	} else {
		if (config_has_device_record(dev->udid)) {
			config_remove_device_record(_dev->udid);
		}
	}

	switch (lerr) {
	case LOCKDOWN_E_INVALID_HOST_ID:
		usbmuxd_log(LL_INFO, "%s: Device %s is not paired with this host.", __func__, _dev->udid);
		break;
	case LOCKDOWN_E_SSL_ERROR:
		usbmuxd_log(LL_ERROR, "%s: The stored pair record for device %s is invalid. Removing.", __func__, _dev->udid);
		if (config_remove_device_record(_dev->udid) == 0) {
			lockdownd_client_free(lockdown);
			lockdown = NULL;
			goto retry;
		} else {
			usbmuxd_log(LL_ERROR, "%s: Could not remove pair record for device %s", __func__, _dev->udid);
		}
		break;
	default:
		is_device_paired = 1;
		break;
	}

	lerr = lockdownd_get_value(lockdown, NULL, "ProductVersion", &value);
	if (lerr != LOCKDOWN_E_SUCCESS) {
		usbmuxd_log(LL_ERROR, "%s: ERROR: Could not get ProductVersion from device %s, lockdown error %d", __func__, _dev->udid, lerr);
		goto leave;
	}

	if (value && plist_get_node_type(value) == PLIST_STRING) {
		plist_get_string_val(value, &version_str);
	}

	if (!version_str) {
		usbmuxd_log(LL_ERROR, "%s: Could not get ProductVersion string from device %s handle %d", __func__, _dev->udid, (int)(long)_dev->conn_data);
		goto leave;
	}

	int version_major = strtol(version_str, NULL, 10);
	int version_minor = strtol(strchr(version_str, '.') + 1, NULL, 10);
	if (version_major >= 7) {
		/* iOS 7.0 and later */
		usbmuxd_log(LL_INFO, "%s: Found ProductVersion %s device %s", __func__, version_str, _dev->udid);

		if (version_major == 7)
		{
			/*with iOS 7 devices after the user trusts us, the device will reconnect, so we need to monitor the device */
			if (usb_set_device_monitoring(info->location, DEVICE_MONITOR_ONCE, DEVICE_MONITOR_CHANGE_TIMEOUT) < 0)
			{
				usbmuxd_log(LL_ERROR, "%s: Could not start monitoring device at location %d", __func__, info->location);
				goto leave;
			}
		}

		lockdownd_set_untrusted_host_buid(lockdown);
		lockdownd_error_t pair_err = LOCKDOWN_E_SUCCESS;
		/* if not paired, trigger the trust dialog to make sure it appears */
		if (!is_device_paired) {
			pair_err = lockdownd_pair(lockdown, NULL);
			int should_stop_preflight = 0;
			if (pair_err == LOCKDOWN_E_SUCCESS) {
				/* if device is still showing the setup screen it will pair even without trust dialog 
				 * Note: This seems to happen after a factory reset, when the device is unlocked, before the 
				 * device was connected to iTunes for the first time. */
				usbmuxd_log(LL_INFO, "%s: Pair success for device %s", __func__, _dev->udid);
				usbmuxd_log(LL_INFO, "%s: Finished preflight on device %s", __func__, _dev->udid);
				client_device_add(info);
				should_stop_preflight = 1;			
			} else if (pair_err == LOCKDOWN_E_USER_DENIED_PAIRING) {
				usbmuxd_log(LL_INFO, "%s: User has denied pairing for device device %s", __func__, _dev->udid);
				usbmuxd_log(LL_INFO, "%s: Finished preflight on device %s", __func__, _dev->udid);
				client_device_user_denied_pairing(info);
				should_stop_preflight = 1;
			} else if ((pair_err == LOCKDOWN_E_PASSWORD_PROTECTED) &&  (version_major <= 7 &&  version_minor < 1 ))/*during research for TBX-36464 saw this isn't needed for ios 7.12 +*/ {
				if (!itunes_setup_has_completed(lockdown)) {
					usbmuxd_log(LL_INFO, "%s: Device %s is locked with a passcode, and iTunesSetupComplete wasn't set", __func__, _dev->udid);
					usbmuxd_log(LL_INFO, "%s: Finished preflight on device %s", __func__, _dev->udid);
					client_device_password_protected(info);
					should_stop_preflight = 1;	
				}
			}

			if (should_stop_preflight) {
				/* Cancel the the device monitoring and leave */
				(void)usb_set_device_monitoring(info->location, DEVICE_MONITOR_DISABLE, DEVICE_MONITOR_CHANGE_TIMEOUT);
				goto leave;
			}
		}
		

		lockdownd_service_descriptor_t service = NULL;
		lerr = lockdownd_start_service(lockdown, "com.apple.mobile.insecure_notification_proxy", &service);
		if (lerr != LOCKDOWN_E_SUCCESS) {
			usbmuxd_log(LL_ERROR, "%s: ERROR: Could not start insecure_notification_proxy on %s, lockdown error %d", __func__, _dev->udid, lerr);
			if (pair_err == LOCKDOWN_E_PASSWORD_PROTECTED)
			{
				client_device_password_protected(info);
			}
			/* Cancel the the device monitoring */
			(void)usb_set_device_monitoring(info->location, DEVICE_MONITOR_DISABLE, DEVICE_MONITOR_CHANGE_TIMEOUT);
			goto leave;
		}

		np_client_t np = NULL;
		np_client_new(dev, service, &np);

		lockdownd_service_descriptor_free(service);
		service = NULL;


		/* Let clients know we are waiting for the user to trust this computer */
		client_device_trust_pending(info);

		struct cb_data cbdata = { 0 };
		cbdata.dev = dev;
		cbdata.np = np;
		cbdata.version_major = version_major;
		cbdata.is_device_connected = 1;
		//pthread_mutex_init(&(cbdata.pairing_lock));
		cbdata.is_paired_signal = CreateEvent(NULL, TRUE, FALSE, NULL);
		if (FALSE == IS_VALID_HANDLE(cbdata.is_paired_signal)) {
			goto leave;
		}

		np_set_notify_callback(np, np_callback, (void*)&cbdata);
		device_set_preflight_cb_data(info->id, (void*)&cbdata);

		const char* spec[] = {
			"com.apple.mobile.lockdown.request_pair",
			"com.apple.mobile.lockdown.request_host_buid",
			NULL
		}; 
		np_observe_notifications(np, spec);

		/* TODO send notification to user's desktop */

		usbmuxd_log(LL_INFO, "%s: Waiting for user to trust this computer on device %s", __func__, _dev->udid);

		/* Note: Removed the following line, so the device won't be visible while
		 * a trust dialog is pending on the device. When device user will press "trust",
		 * the device will disconnect and reconnect, so the next preflight thread will
		 * make it visible.
		 * make device visible anyways */
		/*client_device_add(info);*/

		int nLockdownRecreationAttempts = LOCKDOWN_RECREATION_ATTEMPTS;
		while (cbdata.np && (cbdata.is_device_connected == 1) && (FALSE == IS_PAIRED(&cbdata)) && (nLockdownRecreationAttempts >= 0))
		{
			if (8 <= version_major){
				if (WAIT_TIMEOUT != WaitForSingleObject(cbdata.is_paired_signal, 1000)) {
					break;
				}
			} else {
				sleep(1);
			}
			
			//pthread_mutex_lock(&(cbdata.pairing_lock));
			//if (cbdata.is_paired == 0)
			//{
				/* To detect if the user has denied pairing, we must poll the device */
				lerr = lockdownd_pair(lockdown, NULL);
				if (LOCKDOWN_E_USER_DENIED_PAIRING == lerr)
				{
					usbmuxd_log(LL_INFO, "%s: User has denied pairing on device %s", __func__, _dev->udid);
					client_device_user_denied_pairing(info);

					/* Cancel the the device monitoring */
					(void)usb_set_device_monitoring(info->location, DEVICE_MONITOR_DISABLE, DEVICE_MONITOR_CHANGE_TIMEOUT);

					//pthread_mutex_unlock(&(cbdata.pairing_lock));
					break;
				}
				else if (LOCKDOWN_E_MUX_ERROR == lerr) //TBX-36464 passlocked iOS 9 after restart - must refresh lockdown client, np call back isn't called
				{
					usbmuxd_log(LL_INFO, "%s: lockdown pair mux error on device %s, creating a new lockdown client", __func__, _dev->udid);
					nLockdownRecreationAttempts--;
					lockdownd_client_free(lockdown);
					lockdown = NULL;
					lerr = lockdownd_client_new(dev, &lockdown, "usbmuxd");
					if (LOCKDOWN_E_SUCCESS != lerr)
					{
						usbmuxd_log(LL_INFO, "%s: creating a new lockdown client failed on device %s", __func__, _dev->udid);
					}
				}
				else if (LOCKDOWN_E_SUCCESS == lerr)
				{
					IOS_8_SIGNAL_IS_PAIRED(&cbdata);
				}
				else
				{
					usbmuxd_log(LL_INFO, "%s: lockdown pair error with device %s", __func__, _dev->udid);
				}
			//}
			//pthread_mutex_unlock(&(cbdata.pairing_lock));
		}

		if (lockdown) {
			lockdownd_client_free(lockdown);
			lockdown = NULL;
		}

		usbmuxd_log(LL_INFO, "%s: Finished waiting for notification from device %s, is_device_connected %d", __func__, _dev->udid, cbdata.is_device_connected);

		device_set_preflight_cb_data(info->id, NULL);
		if (cbdata.np) {
			np_set_notify_callback(cbdata.np, NULL, NULL);
			np_client_free(cbdata.np);
		}

		/* On iOS 8, it seems like the pairing completes with a reconnection */
		if ((8 <= version_major) && IS_PAIRED(&cbdata)) {
			//lerr = lockdownd_pair(lockdown, NULL);
			client_device_add(info);
			(void)usb_set_device_monitoring(info->location, DEVICE_MONITOR_DISABLE, DEVICE_MONITOR_CHANGE_TIMEOUT);
		}

		CloseHandle(cbdata.is_paired_signal);
		//pthread_mutex_destroy(&(cbdata.pairing_lock));
	} else {
		/* iOS 6.x and earlier */
		lerr = lockdownd_pair(lockdown, NULL);
		if (lerr != LOCKDOWN_E_SUCCESS) {
			if (lerr == LOCKDOWN_E_PASSWORD_PROTECTED) {
				usbmuxd_log(LL_INFO, "%s: Device %s is locked with a passcode. Cannot pair.", __func__, _dev->udid);
				client_device_password_protected(info);
			} else {
				usbmuxd_log(LL_ERROR, "%s: ERROR: Pair failed for device %s, lockdown error %d", __func__, _dev->udid, lerr);
			}

			usbmuxd_log(LL_INFO, "%s: Finished preflight on device %s", __func__, _dev->udid);

			/* Note: Removed the following line, since the pairing has failed and the 
			 * device won't be usable */
			/* client_device_add(info); */

			goto leave;
		}

		host_id = NULL;
		config_device_record_get_host_id(dev->udid, &host_id);
		lerr = lockdownd_start_session(lockdown, host_id, NULL, NULL);
		free(host_id);
		if (lerr != LOCKDOWN_E_SUCCESS) {
			usbmuxd_log(LL_ERROR, "%s: ERROR StartSession failed on device %s, lockdown error %d", __func__, _dev->udid, lerr);
			goto leave;
		}

		lerr = lockdownd_validate_pair(lockdown, NULL);
		if (lerr != LOCKDOWN_E_SUCCESS) {
			usbmuxd_log(LL_ERROR, "%s: ERROR: ValidatePair failed for device %s, lockdown error %d", __func__, _dev->udid, lerr);
			goto leave;
		}

		usbmuxd_log(LL_INFO, "%s: Finished preflight on device %s", __func__, _dev->udid);

		/* emit device added event and thus make device visible to clients */
		client_device_add(info);
	}

leave:
	if (value)
		plist_free(value);
	if (version_str)
		libimobiledevice_free(version_str);
	if (lockdown)
		lockdownd_client_free(lockdown);
	if (dev) {
		free(_dev->udid);
		free(_dev);
	}

	device_preflight_finished(info->id);

	free(info);

	#ifndef _MSC_VER
		return NULL;
	#endif
}
#else
void preflight_device_remove_cb(void *data)
{
}
#endif

void preflight_worker_device_add(struct device_info* info)
{
#ifdef HAVE_LIBIMOBILEDEVICE
	struct device_info *infocopy = (struct device_info*)malloc(sizeof(struct device_info));

	memcpy(infocopy, info, sizeof(struct device_info));

	pthread_t th;
	pthread_create(&th, NULL, preflight_worker_handle_device_add, infocopy);
#else
	client_device_add(info);
#endif
}
