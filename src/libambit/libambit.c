/*
 * (C) Copyright 2014 Emil Ljungdahl
 *
 * This file is part of libambit.
 *
 * libambit is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Contributors:
 *
 */
#include "libambit.h"
#include "libambit_int.h"
#include "device_support.h"
#include "device_driver.h"
#include "protocol.h"
#include "debug.h"

#include <errno.h>
#include <iconv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/*
 * Local definitions
 */

/*
 * Static functions
 */
static int device_info_get(ambit_object_t *object, ambit_device_info_t *info);
static ambit_device_info_t * ambit_device_info_new(const struct hid_device_info *dev);
static char * utf8strncpy(char *dst, const char *src, size_t n);

/*
 * Static variables
 */
static uint8_t komposti_version[] = { 0x02, 0x00, 0x2d, 0x00 };

/*
 * Public functions
 */
ambit_device_info_t * libambit_enumerate(void)
{
    ambit_device_info_t *devices = NULL;

    struct hid_device_info *devs = hid_enumerate(0, 0);
    struct hid_device_info *current;

    if (!devs) {
      LOG_WARNING("HID: no USB HID devices found");
      return NULL;
    }

    current = devs;
    while (current) {
        ambit_device_info_t *tmp = ambit_device_info_new(current);

        if (tmp) {
            if (devices) {
                tmp->next = devices;
            }
            else {
                devices = tmp;
            }
        }
        current = current->next;
    }
    hid_free_enumeration(devs);

    return devices;
}

void libambit_free_enumeration(ambit_device_info_t *devices)
{
    while (devices) {
        ambit_device_info_t *next = devices->next;
        free((char *) devices->path);
        free(devices);
        devices = next;
    }
}

ambit_object_t * libambit_new(const ambit_device_info_t *device)
{
    ambit_object_t *object = NULL;
    const ambit_known_device_t *known_device = NULL;
    const char *path = NULL;

    if (!device || !device->path) {
        LOG_ERROR("%s", strerror(EINVAL));
        return NULL;
    }

    path = strdup (device->path);
    if (!path) return NULL;

    if (0 == device->access_status && device->is_supported) {
        // Note, this should never fail if device was properly received with libambit_enumerate
        known_device = libambit_device_support_find(device->vendor_id, device->product_id, device->model, device->fw_version);
        if (known_device != NULL) {
            object = calloc(1, sizeof(*object));
            if (object) {
                object->handle = hid_open_path(path);
                memcpy(&object->device_info, device, sizeof(*device));
                object->device_info.path = path;
                object->driver = known_device->driver;

                if (object->handle) {
                    hid_set_nonblocking(object->handle, true);
                }

                // Initialize driver
                object->driver->init(object, known_device->driver_param);
            }
        }
    }
    if (!object) {
        free((char *) path);
    }

    return object;
}

ambit_object_t * libambit_new_from_pathname(const char* pathname)
{
    ambit_object_t *object = NULL;
    ambit_device_info_t *info;
    ambit_device_info_t *current;

    if (!pathname) {
        LOG_ERROR("%s", strerror(EINVAL));
        return NULL;
    }

    info = libambit_enumerate();
    current = info;
    while (!object && current) {
        if (0 == strcmp(pathname, current->path)) {
            object = libambit_new(current);
        }
        current = current->next;
    }
    libambit_free_enumeration(info);

    return object;
}

void libambit_close(ambit_object_t *object)
{
    LOG_INFO("Closing");
    if (object != NULL) {
        if (object->driver != NULL) {
            // Make sure to clear log lock (if possible)
            if (object->driver->lock_log != NULL) {
                object->driver->lock_log(object, false);
            }
            if (object->driver->deinit != NULL) {
                object->driver->deinit(object);
            }
        }
        if (object->handle != NULL) {
            hid_close(object->handle);
        }

        free((char *) object->device_info.path);
        free(object);
    }
}

void libambit_sync_display_show(ambit_object_t *object)
{
    if (object->driver != NULL && object->driver->lock_log != NULL) {
        object->driver->lock_log(object, true);
    }
}

void libambit_sync_display_clear(ambit_object_t *object)
{
    if (object->driver != NULL && object->driver->lock_log != NULL) {
        object->driver->lock_log(object, false);
    }
}

int libambit_date_time_set(ambit_object_t *object, struct tm *tm)
{
    int ret = -1;

    if (object->driver != NULL && object->driver->date_time_set != NULL) {
        ret = object->driver->date_time_set(object, tm);
    }
    else {
        LOG_WARNING("Driver does not support date_time_set");
    }

    return ret;
}

int libambit_device_status_get(ambit_object_t *object, ambit_device_status_t *status)
{
    int ret = -1;

    if (object->driver != NULL && object->driver->status_get != NULL) {
        ret = object->driver->status_get(object, status);
    }
    else {
        LOG_WARNING("Driver does not support status_get");
    }

    return ret;
}

int libambit_personal_settings_get(ambit_object_t *object, ambit_personal_settings_t *settings)
{
    int ret = -1;

    if (object->driver != NULL && object->driver->personal_settings_get != NULL) {
        ret = object->driver->personal_settings_get(object, settings);
    }
    else {
        LOG_WARNING("Driver does not support personal_settings_get");
    }

    return ret;
}

int libambit_gps_orbit_header_read(ambit_object_t *object, uint8_t data[8])
{
    int ret = -1;

    if (object->driver != NULL && object->driver->gps_orbit_header_read != NULL) {
        ret = object->driver->gps_orbit_header_read(object, data);
    }
    else {
        LOG_WARNING("Driver does not support gps_orbit_header_read");
    }

    return ret;
}

int libambit_gps_orbit_write(ambit_object_t *object, uint8_t *data, size_t datalen)
{
    int ret = -1;

    if (object->driver != NULL && object->driver->gps_orbit_write != NULL) {
        ret = object->driver->gps_orbit_write(object, data, datalen);
    }
    else {
        LOG_WARNING("Driver does not support gps_orbit_write");
    }

    return ret;
}

int libambit_log_read(ambit_object_t *object, ambit_log_skip_cb skip_cb, ambit_log_push_cb push_cb, ambit_log_progress_cb progress_cb, void *userref)
{
    int ret = -1;

    if (object->driver != NULL && object->driver->log_read != NULL) {
        ret = object->driver->log_read(object, skip_cb, push_cb, progress_cb, userref);
    }
    else {
        LOG_WARNING("Driver does not support log_read");
    }

    return ret;
}

void libambit_log_entry_free(ambit_log_entry_t *log_entry)
{
    int i;

    if (log_entry != NULL) {
        if (log_entry->samples != NULL) {
            for (i=0; i<log_entry->samples_count; i++) {
                if (log_entry->samples[i].type == ambit_log_sample_type_periodic) {
                    if (log_entry->samples[i].u.periodic.values != NULL) {
                        free(log_entry->samples[i].u.periodic.values);
                    }
                }
                if (log_entry->samples[i].type == ambit_log_sample_type_gps_base) {
                    if (log_entry->samples[i].u.gps_base.satellites != NULL) {
                        free(log_entry->samples[i].u.gps_base.satellites);
                    }
                }
                if (log_entry->samples[i].type == ambit_log_sample_type_unknown) {
                    if (log_entry->samples[i].u.unknown.data != NULL) {
                        free(log_entry->samples[i].u.unknown.data);
                    }
                }
            }
            free(log_entry->samples);
        }
        free(log_entry);
    }
}

static int device_info_get(ambit_object_t *object, ambit_device_info_t *info)
{
    uint8_t *reply_data = NULL;
    size_t replylen;
    int ret = -1;

    LOG_INFO("Reading device info");

    if (libambit_protocol_command(object, ambit_command_device_info, komposti_version, sizeof(komposti_version), &reply_data, &replylen, 1) == 0) {
        if (info != NULL) {
            const char *p = (char *)reply_data;

            utf8strncpy(info->model, p, LIBAMBIT_MODEL_NAME_LENGTH);
            info->model[LIBAMBIT_MODEL_NAME_LENGTH] = 0;
            p += LIBAMBIT_MODEL_NAME_LENGTH;
            utf8strncpy(info->serial, p, LIBAMBIT_SERIAL_LENGTH);
            info->serial[LIBAMBIT_SERIAL_LENGTH] = 0;
            p += LIBAMBIT_SERIAL_LENGTH;
            memcpy(info->fw_version, p, 4);
            memcpy(info->hw_version, p + 4, 4);
        }
        ret = 0;
    }
    else {
        LOG_WARNING("Failed to device info");
    }

    libambit_protocol_free(reply_data);

    return ret;
}

const size_t LIBAMBIT_VERSION_LENGTH = 13;      /* max: 255.255.65535 */
static inline void version_string(char string[LIBAMBIT_VERSION_LENGTH+1],
                                  const uint8_t version[4])
{
  if (!string || !version) return;

  snprintf(string, LIBAMBIT_VERSION_LENGTH+1, "%d.%d.%d",
           version[0], version[1], (version[2] << 0) | (version[3] << 8));
}

/* Converts a wide-character string to a limited length UTF-8 string.
 * This produces the longest valid UTF-8 string that doesn't exceed n
 * bytes.  If the converted string would be too long, it is shortened
 * one wchar_t at a time until the result is short enough.  Invalid
 * and incomplete multibyte sequences will result in an empty string.
 */
static const char * wcs2nutf8(char *dest, const wchar_t *src, size_t n)
{
    const char *rv = NULL;

    iconv_t cd = iconv_open("UTF-8", "WCHAR_T");

    if ((iconv_t) -1 == cd) {
        LOG_ERROR("iconv_open: %s", strerror(errno));
    }
    else {
        char  *s = (char *) malloc((n + 1) * sizeof(char));
        size_t m = wcslen(src) + 1;

        if (s) {
            size_t sz;

            do {
                char  *ibuf = (char *) src;
                char  *obuf = s;
                size_t ilen = --m * sizeof(wchar_t);
                size_t olen = n;

                sz = iconv(cd, &ibuf, &ilen, &obuf, &olen);

                if ((size_t) -1 == sz) {
                    s[0] = '\0';
                }
                else {          /* we're good, terminate string */
                    s[n - olen] = '\0';
                }
            } while ((size_t) -1 == sz && E2BIG == errno && 0 < m);

            if ((size_t) -1 == sz && E2BIG != errno) {
                LOG_ERROR("iconv: %s", strerror(errno));
            }

            strncpy(dest, s, n);
            rv = s;
        }

        iconv_close(cd);
    }

    return rv;
}

static ambit_device_info_t * ambit_device_info_new(const struct hid_device_info *dev)
{
    ambit_device_info_t *device = NULL;
    const ambit_known_device_t *known_device = NULL;

    const char *dev_path;
    const char *name = NULL;
    const char *uniq = NULL;

    uint16_t vid;
    uint16_t pid;

    hid_device *hid;

    

    if (!dev || !dev->path) {
        LOG_ERROR("internal error: expecting hidraw device");
        return NULL;
    }

    dev_path = dev->path;
    vid = dev->vendor_id;
    pid = dev->product_id;

    if (!libambit_device_support_known(vid, pid)) {
        LOG_INFO("ignoring unknown device (VID/PID: %04x/%04x)", vid, pid);
        return NULL;
    }

    dev_path = strdup(dev_path);
    if (!dev_path) return NULL;

    device = calloc(1, sizeof(*device));
    if (!device) {
        free ((char *) dev_path);
        return NULL;
    }

    device->path = dev_path;
    device->vendor_id  = vid;
    device->product_id = pid;

    if (dev->product_string) {
        name = wcs2nutf8(device->name, dev->product_string,
                         LIBAMBIT_PRODUCT_NAME_LENGTH);
    }

    if (dev->serial_number) {
        uniq = wcs2nutf8(device->serial, dev->serial_number,
                         LIBAMBIT_SERIAL_LENGTH);
    }

    LOG_INFO("HID  : %s: '%s' (serial: %s, VID/PID: %04x/%04x)",
             device->path, device->name, device->serial,
             device->vendor_id, device->product_id);

    hid = hid_open_path(device->path);
    if (hid) {
        /* HACK ALERT: minimally initialize an ambit object so we can
         * call device_info_get() */
        ambit_object_t obj;
        obj.handle = hid;
        obj.sequence_no = 0;
        if (0 == device_info_get(&obj, device)) {
            if (name && 0 != strcmp(name, device->name)) {
                LOG_INFO("preferring F/W name over '%s'", name);
            }
            if (uniq && 0 != strcmp(uniq, device->serial)) {
                LOG_INFO("preferring F/W serial number over '%s'", uniq);
            }

            known_device = libambit_device_support_find(device->vendor_id, device->product_id, device->model, device->fw_version);
            if (known_device != NULL) {
                device->is_supported = known_device->supported;
                if (0 != strcmp(device->name, known_device->name)) {
                    LOG_INFO("preferring know device name over '%s'", device->name);
                    strcpy(device->name, known_device->name);
                }
            }

#ifdef DEBUG_PRINT_INFO
            char fw_version[LIBAMBIT_VERSION_LENGTH+1];
            char hw_version[LIBAMBIT_VERSION_LENGTH+1];
            version_string(fw_version, device->fw_version);
            version_string(hw_version, device->hw_version);
#endif
            LOG_INFO("Ambit: %s: '%s' (serial: %s, VID/PID: %04x/%04x, "
                     "nick: %s, F/W: %s, H/W: %s, supported: %s)",
                     device->path, device->name, device->serial,
                     device->vendor_id, device->product_id,
                     device->model, fw_version, hw_version,
                     (device->is_supported ? "YES" : "NO"));
        }
        else {
            LOG_ERROR("cannot get device info from %s", device->path);
        }
        hid_close(hid);
    }
    else {
        /* Store an educated guess as to why we cannot open the HID
         * device.  Without read/write access we cannot communicate
         * to begin with but there may be other reasons.
         */
        int fd = open(device->path, O_RDWR);

        if (-1 == fd) {
            device->access_status = errno;
            LOG_ERROR("cannot open HID device (%s): %s", device->path,
                      strerror (device->access_status));
        }
        else {
            LOG_WARNING("have read/write access to %s but cannot open HID "
                        "device", device->path);
            close(fd);
        }
    }

    if (name) free((char *) name);
    if (uniq) free((char *) uniq);

    return device;
}

/*! \brief Converts up to \a n octets to a UTF-8 encoded string.
 *
 *  The \a n octets starting at \a src are assumed to have been
 *  obtained from the clock and in a clock-specific encoding.
 *
 *  \todo  Confirm the clock encoding, assuming ASCII for now.
 */
static char * utf8strncpy(char *dst, const char *src, size_t n)
{
  size_t i;

  /* Sanity check the octets we are about to convert.  */
  for (i = 0; i < n && 0 !=src[i]; ++i) {
      if (0 > src[i] && src[i] > 127) {
          LOG_WARNING("non-ASCII byte at position %i: 0x%02x",
                      i, src[i]);
      }
  }
  return strncpy(dst, src, n);
}
