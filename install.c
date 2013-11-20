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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cutils/properties.h>

#include "common.h"
#include "install.h"
#include "mincrypt/rsa.h"
#include "minui/minui.h"
#include "minzip/SysUtil.h"
#include "minzip/Zip.h"
#include "sideloader/adb.h"
#include "interactive/interactive.h"
#include "roots.h"
#include "ui.h"

#define ASSUMED_UPDATE_BINARY_NAME  "META-INF/com/google/android/update-binary"
#define DEFAULT_UPDATE_BINARY_NAME  "/sbin/updater"

#define SIDELOADER_BINARY_NAME      "/sbin/sideloader"

static const char *SIDELOAD_TEMP_DIR = "/tmp";
static const char *EXTERNAL_SDCARD_ROOT = "/mnt/external_sdcard";

static volatile interactive_struct* interactive;

// If the package contains an update binary, extract it and run it.
static int try_update_binary(const char *path, ZipArchive *zip, int* wipe_cache) 
{  
	char* binary; 
	const ZipEntry* binary_entry = mzFindZipEntry(zip, ASSUMED_UPDATE_BINARY_NAME);
	
	if (binary_entry == NULL)
		binary = DEFAULT_UPDATE_BINARY_NAME;
	else 
	{
		binary = "/tmp/update_binary";
		unlink(binary);
		int fd = creat(binary, 0755);
		if (fd < 0) 
		{
			mzCloseZipArchive(zip);
			LOGE("Can't make %s\n", binary);
			return INSTALL_ERROR;
		}
		
		bool ok = mzExtractZipEntryToFile(zip, binary_entry, fd);
		close(fd);
		mzCloseZipArchive(zip);

		if (!ok)
		{
			LOGE("Can't copy %s\n", ASSUMED_UPDATE_BINARY_NAME);
			return INSTALL_ERROR;
		}
	}
	
	int pipefd[2];
	pipe(pipefd);

	// When executing the update binary contained in the package, the
	// arguments passed are:
	//
	//   - the version number for this interface
	//
	//   - an fd to which the program can write in order to update the
	//     progress bar.  The program can write single-line commands:
	//
	//        progress <frac> <secs>
	//            fill up the next <frac> part of of the progress bar
	//            over <secs> seconds.  If <secs> is zero, use
	//            set_progress commands to manually control the
	//            progress of this segment of the bar
	//
	//        set_progress <frac>
	//            <frac> should be between 0.0 and 1.0; sets the
	//            progress bar within the segment defined by the most
	//            recent progress command.
	//
	//        firmware <"hboot"|"radio"> <filename>
	//            arrange to install the contents of <filename> in the
	//            given partition on reboot.
	//
	//            (API v2: <filename> may start with "PACKAGE:" to
	//            indicate taking a file from the OTA package.)
	//
	//            (API v3: this command no longer exists.)
	//
	//        ui_print <string>
	//            display <string> on the screen.
	//
	//   - the name of the package zip file.
	//

	char** args = malloc(sizeof(char*) * 5);
	args[0] = binary;
	args[1] = EXPAND(RECOVERY_API_VERSION);   // defined in Android.mk
	args[2] = malloc(10);
	sprintf(args[2], "%d", pipefd[1]);
	args[3] = (char*)path;
	args[4] = NULL;

	pid_t pid = fork();
	if (pid == 0) 
	{
		close(pipefd[0]);
		execv(binary, args);
		fprintf(stdout, "E:Can't run %s (%s)\n", binary, strerror(errno));
		_exit(-1);
	}
	close(pipefd[1]);

	*wipe_cache = 0;

	char buffer[1024];
	FILE* from_child = fdopen(pipefd[0], "r");
	while (fgets(buffer, sizeof(buffer), from_child) != NULL) 
	{
		char* command = strtok(buffer, " \n");
		if (command == NULL)
			continue;
		else if (strcmp(command, "progress") == 0) 
		{
			char* fraction_s = strtok(NULL, " \n");
			char* seconds_s = strtok(NULL, " \n");

			float fraction = strtof(fraction_s, NULL);
			int seconds = strtol(seconds_s, NULL, 10);

			ui_show_progress(fraction * (1-VERIFICATION_PROGRESS_FRACTION), seconds);
		} 
		else if (strcmp(command, "set_progress") == 0) 
		{
			char* fraction_s = strtok(NULL, " \n");
			float fraction = strtof(fraction_s, NULL);
			ui_set_progress(fraction);
		} 
		else if (strcmp(command, "ui_print") == 0) 
		{
			char* str = strtok(NULL, "\n");
			if (str)
				ui_print("%s", str);
			else
				ui_print("\n");
		}
		else if (strcmp(command, "wipe_cache") == 0)
			*wipe_cache = 1;
		else 
			LOGE("unknown command [%s]\n", command);
	}
	fclose(from_child);

	int status;
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) 
	{
		LOGE("Error in %s\n(Status %d)\n", path, WEXITSTATUS(status));
		return INSTALL_ERROR;
	}

	return INSTALL_SUCCESS;
}

int run_shell_script(const char *command, int stdout_to_ui, int blink_led, char** extra_env_variables) 
{
	const char* argp[] = { "/sbin/bash", "-c", NULL, NULL};

	//check it
	if (!command)
		return 1;

	//add the command
	argp[2] = command;
	fprintf(stderr, "Running Shell Script: \"%s\"\n", command);

	//interactive menu shared memory node
	int imenu_fd = 0;

	//pipes
	int script_pipefd[2];

	//led
	if (blink_led)
		ui_led_blink(1);

	if (stdout_to_ui)
	{
		pipe(script_pipefd);

		if((imenu_fd = open(INTERACTIVE_SHM, (O_CREAT | O_RDWR),
				             666)) < 0 )
		{
			LOGE("Failed opening the shared memory node for interactive input.\n");
			LOGE("Interactive menu disabled.\n");
		}
		else
		{
			ftruncate(imenu_fd, sizeof(interactive_struct));
			if ((interactive = ((volatile interactive_struct*) mmap(0, sizeof(interactive_struct), (PROT_READ | PROT_WRITE),
                   MAP_SHARED, imenu_fd, 0))) == MAP_FAILED)
			{
				LOGE("Failed opening the shared memory node for interactive input.\n");
				LOGE("Interactive input disabled.\n");
				interactive = NULL;
			}
			else
			{
				interactive->in_trigger = 0;
				interactive->out_trigger = 0;
				interactive->header[0] = '\0';
				interactive->items[0][0] = '\0';
			}
		}
	}

	pid_t child = fork();
	if (child == 0)
	{
		if (stdout_to_ui)
		{
			//put stdout to the pipe only
			close(script_pipefd[0]);
			dup2(script_pipefd[1], 1);
		}

		if (extra_env_variables != NULL)
		{
			while (*extra_env_variables)
			{
				fprintf(stderr, "run_shell_script: child env variable %s\n", *extra_env_variables);
				putenv(*extra_env_variables);
				extra_env_variables++;
			}
		}

		execv("/sbin/bash", (char * const *)argp);
		fprintf(stderr, "run_shell_script: execv failed: %s\n", strerror(errno));
		_exit(1);
	}

	//status for the waitpid	
	int sts;

	if (stdout_to_ui)
	{
		char buffer[1024+1];

		//nonblocking mode
		int f = fcntl(script_pipefd[0], F_GETFL, 0);
		// Set bit for non-blocking flag
		f |= O_NONBLOCK;
		// Change flags on fd
		fcntl(script_pipefd[0], F_SETFL, f);

		while (1)
		{
			const char* headers[3];
			const char* items[20];
			char response[50];
			int i;
			
			//maybe use signalling
			if (interactive != NULL && interactive->in_trigger)
			{
				fprintf(stderr, "run_shell_script: interactive triggered: %d\n", interactive->in_trigger);
				
				//first print the rest, but don't bother if there is an error
				int rv = read(script_pipefd[0], buffer, 1024);	
				if (rv > 0)
				{
					buffer[rv] = 0;	
					ui_print_raw(buffer);
				}
				
				switch (interactive->in_trigger)
				{
					case INTERACTIVE_TRIGGER_MENU:
						
						//parse the name and headers		
						headers[0] = (const char*) interactive->header;
						headers[1] = " ";
						headers[2] = NULL;
		
						for (i = 0; interactive->items[i][0] && i < 20; i++)
							items[i] = (const char*) interactive->items[i];
		
						items[i] = NULL;
		
						//show the menu
						fprintf(stderr, "run_shell_script: showing interactive menu\n");
						if (blink_led)
							ui_led_toggle(0);
						int chosen_item = get_interactive_menu(headers, items, 1);
						if (blink_led)
							ui_led_blink(1);
		
						interactive->header[0] = '\0';
						interactive->items[0][0] = '\0';
						interactive->reply[0] = '\0';
						interactive->in_trigger = 0;
						interactive->out_trigger = chosen_item + 1;
						break;

					case INTERACTIVE_TRIGGER_TEXT:

						if (!get_current_device()->has_qwerty)
							goto default_case;

						if (blink_led)
							ui_led_toggle(0);
						ui_user_input((const char*) interactive->header, response);
						if (blink_led)
							ui_led_blink(1);
						
						interactive->header[0] = '\0';
						interactive->items[0][0] = '\0';
						strncpy((char*) interactive->reply, response, 49);
						interactive->in_trigger = 0;
						interactive->out_trigger = 1;
						break;
					
					default:
default_case:
						LOGE("Interactive input - invalid switch %d.\n", interactive->in_trigger);
						interactive->header[0] = '\0';
						interactive->items[0][0] = '\0';
						interactive->reply[0] = '\0';
						interactive->in_trigger = 0;
						interactive->out_trigger = 1;
						break;
				}
			}

			int rv = read(script_pipefd[0], buffer, 1024);

			if (rv <= 0)
			{
				if(errno == EAGAIN)
				{
					if (waitpid(child, &sts, WNOHANG))
					{
						//do one last check (race)
						int rv = read(script_pipefd[0], buffer, 1024);

						if (rv > 0)
						{
							buffer[rv] = 0;
							ui_print_raw(buffer);
						}

						break;
					}
					continue;
				}

				fprintf(stderr, "run_shell_script: there was a read error %d.\n", errno);
				waitpid(child, &sts, 0);
				break;
			}

			//if the string is read only partially, it won't be null terminated
			buffer[rv] = 0;
			ui_print_raw(buffer);
			usleep(5000);
		}
	}
	else
		waitpid(child, &sts, 0);

	//check exit status
	if (WIFEXITED(sts)) 
	{
		if (WEXITSTATUS(sts) != 0) 
		{
			fprintf(stderr, "run_shell_script: child exited with status %d.\n",
			WEXITSTATUS(sts));
		}
	}
	else if (WIFSIGNALED(sts)) 
	{
		fprintf(stderr, "run_shell_script: child terminated by signal %d.\n",
		WTERMSIG(sts));
	}

	if (stdout_to_ui)
	{
		//close the pipe here after killing the process
		close(script_pipefd[1]);
		close(script_pipefd[0]);

		if (imenu_fd > 0)
		{
			close(imenu_fd);
			remove(INTERACTIVE_SHM);
		}

		interactive = NULL;
	}

	if (blink_led)
		ui_led_toggle(0);

	return WEXITSTATUS(sts);
}

static int really_install_package(const char *path, int* wipe_cache)
{
	ui_print("Finding update package...\n");
	ui_show_indeterminate_progress();
	LOGI("Update location: %s\n", path);

	if (ensure_path_mounted(path) != 0) 
	{
		LOGE("Can't mount %s\n", path);
		return INSTALL_CORRUPT;
	}

	ui_print("Opening update package...\n");

	/* Try to open the package.
		*/
	ZipArchive zip;
	int err;
	
	err = mzOpenZipArchive(path, &zip);
	if (err != 0) 
	{
		LOGE("Can't open %s\n(%s)\n", path, err != -1 ? strerror(err) : "bad");
		return INSTALL_CORRUPT;
	}

	/* Install the contents of the package.
		*/
	ui_print("Installing update...\n");
	return try_update_binary(path, &zip, wipe_cache);
}

static char* copy_package(const char* original_path) 
{
	if (ensure_path_mounted(original_path) != 0) 
	{
		LOGE("Can't mount %s\n", original_path);
		return NULL;
	}

	if (ensure_path_mounted(SIDELOAD_TEMP_DIR) != 0) 
	{
		LOGE("Can't mount %s\n", SIDELOAD_TEMP_DIR);
		return NULL;
	}

	if (mkdir(SIDELOAD_TEMP_DIR, 0700) != 0) 
	{
		if (errno != EEXIST) 
		{
			LOGE("Can't mkdir %s (%s)\n", SIDELOAD_TEMP_DIR, strerror(errno));
			return NULL;
		}
	}

	// verify that SIDELOAD_TEMP_DIR is exactly what we expect: a
	// directory, owned by root, readable and writable only by root.
	struct stat st;
	if (stat(SIDELOAD_TEMP_DIR, &st) != 0) 
	{
		LOGE("failed to stat %s (%s)\n", SIDELOAD_TEMP_DIR, strerror(errno));
		return NULL;
	}

	if (!S_ISDIR(st.st_mode)) 
	{
		LOGE("%s isn't a directory\n", SIDELOAD_TEMP_DIR);
		return NULL;
	}

	if ((st.st_mode & 0777) != 0700) 
	{
		LOGE("%s has perms %o\n", SIDELOAD_TEMP_DIR, st.st_mode);
		return NULL;
	}

	if (st.st_uid != 0) 
	{
		LOGE("%s owned by %lu; not root\n", SIDELOAD_TEMP_DIR, st.st_uid);
		return NULL;
	}

	char copy_path[PATH_MAX];
	strcpy(copy_path, SIDELOAD_TEMP_DIR);
	strcat(copy_path, "/package.zip");

	char* buffer = malloc(BUFSIZ);
	if (buffer == NULL) 
	{
		LOGE("Failed to allocate buffer\n");
		return NULL;
	}

	size_t read;
	FILE* fin = fopen(original_path, "rb");
	if (fin == NULL) 
	{
		LOGE("Failed to open %s (%s)\n", original_path, strerror(errno));
		return NULL;
	}

	FILE* fout = fopen(copy_path, "wb");
	if (fout == NULL) 
	{
		LOGE("Failed to open %s (%s)\n", copy_path, strerror(errno));
		return NULL;
	}

	while ((read = fread(buffer, 1, BUFSIZ, fin)) > 0) 
	{
		if (fwrite(buffer, 1, read, fout) != read) 
		{
			LOGE("Short write of %s (%s)\n", copy_path, strerror(errno));
			return NULL;
		}
	}

	free(buffer);

	if (fclose(fout) != 0) 
	{
		LOGE("Failed to close %s (%s)\n", copy_path, strerror(errno));
		return NULL;
	}

	if (fclose(fin) != 0) 
	{
		LOGE("Failed to close %s (%s)\n", original_path, strerror(errno));
		return NULL;
	}

	// "adb push" is happy to overwrite read-only files when it's
	// running as root, but we'll try anyway.
	if (chmod(copy_path, 0400) != 0) 
	{
		LOGE("Failed to chmod %s (%s)\n", copy_path, strerror(errno));
		return NULL;
	}

	return strdup(copy_path);
}

static void* adb_sideload_thread(void* v)
{
	pid_t sideloader = *((pid_t*)v);

	int status;
	waitpid(sideloader , &status, 0);
	LOGI("sideload process finished\n");
    
	ui_cancel_wait_key();

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		ui_print("Sideload status %d\n", WEXITSTATUS(status));

	LOGI("sideload thread finished\n");
	return NULL;
}

int sideload_package(const char* install_file)
{
	pid_t sideloader;
	pthread_t sideload_thread;

	// Stop adbd
	property_set("ctl.stop", "adbd");
	ui_print("Starting sideload...\n");

	sideloader = fork();

	if (sideloader == 0)
	{
		execl(SIDELOADER_BINARY_NAME, SIDELOADER_BINARY_NAME, NULL);
		exit(-1);
	}

	pthread_create(&sideload_thread, NULL, &adb_sideload_thread, &sideloader);

	const char* sideload_headers[] = { "ADB Sideload",
	                                   "",
	                                   NULL
	                                 };
	const char* sideload_items[] = { "Cancel",
	                                  NULL,
                                      };

	int result = get_interactive_menu(sideload_headers, sideload_items, 1);

	// Kill child
	kill(sideloader, SIGTERM);
	pthread_join(sideload_thread, NULL);
	ui_clear_key_queue();

	struct stat st;
	int ret;

	if (result < 0)
	{
		if (stat(ADB_SIDELOAD_FILENAME, &st) != 0)
		{
			if (errno == ENOENT)
				ui_print("No package received.\n");
        		else
				ui_print("Error reading package:\n  %s\n", strerror(errno));

			ret =  INSTALL_ERROR;
		}
		else
		{
			int wipe_cache = 0;
			ret = install_package(ADB_SIDELOAD_FILENAME, &wipe_cache, install_file);
		}
	}
	else
	{
		ui_print("Sideload cancelled.\n");
		ret = INSTALL_ERROR;
	}

	unlink(ADB_SIDELOAD_FILENAME);
	//start adbd
	property_set("ctl.start", "adbd");
	return ret;
}

int install_package(const char* path, int* wipe_cache, const char* install_file)
{
	FILE* install_log = fopen_path(install_file, "w");
	int result = INSTALL_SUCCESS;
	if (install_log) 
	{
		fputs(path, install_log);
		fputc('\n', install_log);
	}
	else
		LOGE("failed to open last_install: %s\n", strerror(errno));

	//does it need to be copied to /tmp?
	int requires_copy = strncmp(path, EXTERNAL_SDCARD_ROOT, strlen(EXTERNAL_SDCARD_ROOT)) && strncmp(path, "/cache", strlen("/cache")) && strncmp(path, "/tmp", strlen("/tmp"));
	char* package_copy = NULL;

	if (requires_copy)
	{
		LOGI("Copying package: %s\n", path);
		package_copy = copy_package(path);

		if (!package_copy)
		{
			result = INSTALL_CORRUPT;
			goto end;
		}
			

		path = package_copy;
	}

	ensure_common_roots_unmounted();
	//keep cache mounted
	ensure_path_mounted("/cache");
	ui_set_background(BACKGROUND_ICON_INSTALLING);
	result = really_install_package(path, wipe_cache);
	ui_set_background(BACKGROUND_ICON_ERROR);
	ensure_common_roots_mounted();

	if (package_copy)
		free(package_copy);

end:
	if (install_log) 
	{
		fputc(result == INSTALL_SUCCESS ? '1' : '0', install_log);
		fputc('\n', install_log);
		fclose(install_log);
	}
	
	return result;
}
