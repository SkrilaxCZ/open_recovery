/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>

#include "mounts.h"
#include "roots.h"
#include "common.h"
#include "ui.h"
#include "install.h"

static const char *INTERNAL_SDCARD_ROOT = "/mnt/sdcard";
static const char *INTERNAL_SDCARD_LEGACY_ROOT = "/sdcard";

static int num_volumes = 0;
static Volume* device_volumes = NULL;
struct selabel_handle *sehandle = NULL;

static int parse_options(char* options, Volume* volume) 
{
	char* option;
	while ( (option = strtok(options, ",")) )
	{
		options = NULL;

		if (strncmp(option, "length=", 7) == 0)
			volume->length = strtoll(option+7, NULL, 10);
		else 
		{
			LOGE("bad option \"%s\"\n", option);
			return -1;
		}
	}
	return 0;
}

void load_volume_table() 
{
	int alloc = 2;
	device_volumes = malloc(alloc * sizeof(Volume));

	// Insert an entry for /tmp, which is the ramdisk and is always mounted.
	device_volumes[0].mount_point = "/tmp";
	device_volumes[0].fs_type = "ramdisk";
	device_volumes[0].device = NULL;
	device_volumes[0].device2 = NULL;
	device_volumes[0].length = 0;

	device_volumes[1].mount_point = "/install";
	device_volumes[1].fs_type = "ramdisk";
	device_volumes[1].device = NULL;
	device_volumes[1].device2 = NULL;
	device_volumes[1].length = 0;

	num_volumes = 2;

	FILE* fstab = fopen("/etc/recovery.fstab", "r");
	if (fstab == NULL) 
	{
		LOGE("failed to open /etc/recovery.fstab (%s)\n", strerror(errno));
		return;
	}

	char buffer[1024];
	int i;
	while (fgets(buffer, sizeof(buffer)-1, fstab)) 
	{
		for (i = 0; buffer[i] && isspace(buffer[i]); ++i);
			if (buffer[i] == '\0' || buffer[i] == '#') 
				continue;

		char* original = strdup(buffer);

		char* mount_point = strtok(buffer+i, " \t\n");
		char* fs_type = strtok(NULL, " \t\n");
		char* device = strtok(NULL, " \t\n");
		// lines may optionally have a second device, to use if
		// mounting the first one fails.
		char* options = NULL;
		char* device2 = strtok(NULL, " \t\n");
		if (device2) 
		{
			if (device2[0] == '/') 
				options = strtok(NULL, " \t\n");
			else 
			{
				options = device2;
				device2 = NULL;
			}
		}

		if (mount_point && fs_type && device) 
		{
			while (num_volumes >= alloc) 
			{
				alloc *= 2;
				device_volumes = realloc(device_volumes, alloc*sizeof(Volume));
			}
			
			device_volumes[num_volumes].mount_point = strdup(mount_point);
			device_volumes[num_volumes].fs_type = strdup(fs_type);
			device_volumes[num_volumes].device = strdup(device);
			device_volumes[num_volumes].device2 =
					device2 ? strdup(device2) : NULL;

			device_volumes[num_volumes].length = 0;
			if (parse_options(options, device_volumes + num_volumes) != 0)
				LOGE("skipping malformed recovery.fstab line: %s\n", original);
			else
				++num_volumes;
		} 
		else
			LOGE("skipping malformed recovery.fstab line: %s\n", original);
		
		free(original);
	}

	fclose(fstab);

	printf("recovery filesystem table\n");
	printf("=========================\n");
	for (i = 0; i < num_volumes; ++i) 
	{
		Volume* v = &device_volumes[i];
		printf("  %d %s %s %s %s %lld\n", i, v->mount_point, v->fs_type,
						v->device, v->device2, v->length);
	}
	printf("\n");
}

Volume* volume_for_path(const char* path) 
{
	int i;
	for (i = 0; i < num_volumes; ++i) 
	{
		Volume* v = device_volumes+i;
		int len = strlen(v->mount_point);
		if (strncmp(path, v->mount_point, len) == 0 && (path[len] == '\0' || path[len] == '/')) 
			return v;
		
	}
	return NULL;
}

int ensure_path_mounted(const char* path) 
{
	// internal sdcard is in data partition
	if (!strncmp(path, INTERNAL_SDCARD_ROOT, strlen(INTERNAL_SDCARD_ROOT)) ||
		!strncmp(path, INTERNAL_SDCARD_LEGACY_ROOT, strlen(INTERNAL_SDCARD_LEGACY_ROOT)))
		path = "/data";

	Volume* v = volume_for_path(path);
	if (v == NULL) 
	{
		LOGE("unknown volume for path [%s]\n", path);
		return -1;
	}
	if (strcmp(v->fs_type, "ramdisk") == 0) 
	{
		// the ramdisk is always mounted.
		return 0;
	}

	int result;
	result = scan_mounted_volumes();
	if (result < 0) 
	{
		LOGE("failed to scan mounted volumes\n");
		return -1;
	}

	const MountedVolume* mv = find_mounted_volume_by_mount_point(v->mount_point);
	if (mv) 
	{
		// volume is already mounted
		return 0;
	}

	mkdir(v->mount_point, 0755);  // in case it doesn't already exist

	if (strcmp(v->fs_type, "ext4") == 0 || strcmp(v->fs_type, "vfat") == 0 || strcmp(v->fs_type, "f2fs") == 0) 
	{
		const char* additional_data = strcmp(v->fs_type, "f2fs") == 0 ? "inline_xattr" : "";
		result = mount(v->device, v->mount_point, v->fs_type, MS_NOATIME | MS_NODEV | MS_NODIRATIME, additional_data);
		if (result == 0) 
			return 0;

		if (v->device2) 
		{
			LOGW("failed to mount %s (%s); trying %s\n", v->device, strerror(errno), v->device2);
			result = mount(v->device2, v->mount_point, v->fs_type, MS_NOATIME | MS_NODEV | MS_NODIRATIME, additional_data);
			if (result == 0) 
				return 0;
		}

		LOGE("failed to mount %s (%s)\n", v->mount_point, strerror(errno));
		return -1;
	}

	LOGE("unknown fs_type \"%s\" for %s\n", v->fs_type, v->mount_point);
	return -1;
}

static int device_node_exists(const char *path)
{
	Volume* v = volume_for_path(path);
	if (v == NULL)
	{
		LOGE("unknown volume for path [%s]\n", path);
		return -1;
	}

	struct stat stFileInfo;
	int sts;

	// Attempt to get the file attributes
	sts = stat(v->device, &stFileInfo);
	if(sts == 0)
		return 1;

	if (v->device2)
	{
		sts = stat(v->device2, &stFileInfo);
		if(sts == 0)
			return 1;
	}

	return 0;
}

void ensure_common_roots_mounted()
{
	ensure_path_mounted("/system");
	ensure_path_mounted("/cache");
	ensure_path_mounted("/data");

	if (get_current_device()->has_external_sdcard && device_node_exists("/mnt/external_sdcard"))
		ensure_path_mounted("/mnt/external_sdcard");
}

void ensure_common_roots_unmounted()
{
	ensure_path_unmounted("/system");
	ensure_path_unmounted("/cache");
	ensure_path_unmounted("/data");

	if (get_current_device()->has_external_sdcard && device_node_exists("/mnt/external_sdcard"))
		ensure_path_unmounted("/mnt/external_sdcard");
}

int ensure_path_unmounted(const char* path)
{
	Volume* v = volume_for_path(path);
	if (v == NULL) 
	{
		LOGE("unknown volume for path [%s]\n", path);
		return -1;
	}
	if (strcmp(v->fs_type, "ramdisk") == 0) 
	{
		// the ramdisk is always mounted; you can't unmount it.
		return -1;
	}

	int result;
	result = scan_mounted_volumes();
	if (result < 0) 
	{
		LOGE("failed to scan mounted volumes\n");
		return -1;
	}

	const MountedVolume* mv = find_mounted_volume_by_mount_point(v->mount_point);
	if (mv == NULL) 
	{
		// volume is already unmounted
		return 0;
	}

	return unmount_mounted_volume(mv);
}

int format_volume(const char* volume) 
{
	Volume* v = volume_for_path(volume);
	if (v == NULL) 
	{
		LOGE("unknown volume \"%s\"\n", volume);
		return -1;
	}

	if (strcmp(v->fs_type, "ramdisk") == 0) 
	{
		// you can't format the ramdisk.
		LOGE("can't format_volume \"%s\"", volume);
		return -1;
	}

	if (strcmp(v->mount_point, volume) != 0) 
	{
		LOGE("can't give path \"%s\" to format_volume\n", volume);
		return -1;
	}

	if (ensure_path_unmounted(volume) != 0) 
	{
		LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
		return -1;
	}

	if (!strcmp(v->mount_point, "/data"))
	{
		//keep /data/media
		ensure_path_mounted("/data");
		int result = run_shell_script("/sbin/erase_data", 1, 0, NULL);
		ensure_path_unmounted("/data");
		
		if (result != 0)
		{
			LOGE("format_volume: erase_data failed\n");
			return -1;
		}
		return 0;
	}

	if (strcmp(v->fs_type, "ext4") == 0) 
	{
        const char* args[] = { "/sbin/make_ext4fs", v->device, NULL };

		int result = run_command("/sbin/make_ext4fs", args);
		if (result != 0) 
		{
			LOGE("format_volume: make_ext4fs failed on %s\n", v->device);
			return -1;
		}
		return 0;
	}

	if (strcmp(v->fs_type, "f2fs") == 0) 
	{
		const char* args[] = { "/sbin/mkfs.f2fs", "-t", v->device, NULL };

		int result = run_command("/sbin/mkfs.f2fs", args);
		if (result != 0) 
		{
			LOGE("format_volume: mkfs.f2fs failed on %s\n", v->device);
			return -1;
		}
		return 0;
	}

	LOGE("format_volume: fs_type \"%s\" unsupported\n", v->fs_type);
	return -1;
}
