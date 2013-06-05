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

#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>

//Open Recovery Version
#define OPEN_RECOVERY_VERSION_NUMBER "2.06"
#define OPEN_RECOVERY_VERSION "Version "OPEN_RECOVERY_VERSION_NUMBER

//Photon Q
#define OPEN_RECOVERY_NAME "Photon Q"
#define OPEN_RECOVERY_NAVIG "Use arrow keys to highlight; enter to select."

#define MOD_AUTHOR_PROP "ro.or.mod.author"
#define MOD_VERSION_PROP "ro.or.mod.version"

#define MOD_AUTHOR_BASE "Mod Created by %s"
#define MOD_VERSION_BASE "Mod Version %s"

#define LOGE(...) ui_print("E:" __VA_ARGS__)
#define LOGW(...) fprintf(stdout, "W:" __VA_ARGS__)
#define LOGI(...) fprintf(stdout, "I:" __VA_ARGS__)

#if 0
#define LOGV(...) fprintf(stdout, "V:" __VA_ARGS__)
#define LOGD(...) fprintf(stdout, "D:" __VA_ARGS__)
#else
#define LOGV(...) do {} while (0)
#define LOGD(...) do {} while (0)
#endif

#define STRINGIFY(x) #x
#define EXPAND(x) STRINGIFY(x)

typedef struct 
{
	// eg. "/cache".  must live in the root directory.
	const char* mount_point;

	// "yaffs2" or "ext4" or "vfat"
	const char* fs_type;

	// MTD partition name if fs_type == "yaffs"
	// block device if fs_type == "ext4" or "vfat"
	const char* device;

	// alternative device to try if fs_type
	// == "ext4" or "vfat" and mounting
	// 'device' fails
	const char* device2;

	// (ext4 partition only) when
	// formatting, size to use for the
	// partition.  0 or negative number
	// means to format all but the last
	// (that much).
	long long length;     
} Volume;

// fopen a file, mounting volumes and making parent dirs as necessary.
FILE* fopen_path(const char *path, const char *mode);

//shows interactive menu when called via imenu
int get_interactive_menu(const char** headers, const char** items, int hide_menu_after);

// load properties
void load_properties();

#endif  // COMMON_H
