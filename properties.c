/*
 * Copyright (C) 2013 Skrilax_CZ
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
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

#include "common.h"
#include "ui.h"
#include "console.h"

// UI properties
#define UI_INDETERMINATE_FRAMES_PROP_NAME       "ui.intederminate.frames"
#define UI_INSTALL_FRAMES_PROP_NAME             "ui.install.frames"
#define UI_INSTALL_LOC_X_PROP_NAME              "ui.install.x"
#define UI_INSTALL_LOC_Y_PROP_NAME              "ui.install.y"

// Colors
#define COLOR_LED_PROP_NAME                     "color.LED"
#define COLOR_BKGROUND_PROP_NAME                "color.background"
#define COLOR_TITLE_PROP_NAME                   "color.title"
#define COLOR_MENU_PROP_NAME                    "color.menu"
#define COLOR_MENU_SEL_PROP_NAME                "color.selection"
#define COLOR_SCRIPT_PROP_NAME                  "color.script"

// Console colors
#define COLOR_CONSOLE_HEADER_PROP_NAME          "color.console.header"
#define COLOR_CONSOLE_BACKGROUND_PROP_NAME      "color.console.background"
#define COLOR_CONSOLE_FRONT_PROP_NAME           "color.console.front"
#define COLOR_CONSOLE_TERMCLR_PROP_NAME_BASE    "color.console.termclr"

/* reads a file with properties, making sure it is terminated with \n \0 */
static void*
read_file(const char *fn, unsigned *_sz)
{
	char *data;
	int sz;
	int fd;
	
	data = 0;
	fd = open(fn, O_RDONLY);
	if(fd < 0) 
		return 0;
	
	sz = lseek(fd, 0, SEEK_END);
	if(sz < 0) 
		goto oops;
	
	if(lseek(fd, 0, SEEK_SET) != 0) 
		goto oops;
	
	data = (char*) malloc(sz + 2); 
	if(data == 0) 
		goto oops;
	
	if(read(fd, data, sz) != sz) 
		goto oops;
	
	close(fd);
	data[sz] = '\n';
	data[sz+1] = 0;
	
	if(_sz) 
		*_sz = sz;
	
	return data;
	
oops:
	close(fd);
	if(data != 0) free(data);
	return 0;
}

static int 
read_byte_hex(const char* text)
{
	unsigned int value = 0;
	unsigned char c;

	if (text[0] > '9')
	{
		c = text[0] - 'A' + 0x0A;
		if (c > 0x0F)
			return -1;
	}
	else
	{
		c = text[0] - '0';
		if (c > 0x09)
			return -1;
	}

	value = c * 0x10;

	if (text[1] > '9')
	{
		c = text[1] - 'A' + 0x0A;
		if (c > 0x0F)
			return -1;
	}
	else
	{
		c = text[1] - '0';
		if (c > 0x09)
			return -1;
	}

	return value + c;
}

static int 
htoc_internal(const char* text, unsigned char* clr_struct, unsigned int clr_bytes)
{
	int c;
	unsigned int i;

	for (i = 0; i < clr_bytes; i++)
	{
		c = read_byte_hex(text);
		if (c == -1)
			return 1;

		(*clr_struct) = c;
		clr_struct++;
		text += 2;
	}

	return 0;
}

static int 
htoc(const char* text, color* color)
{
	if (strlen(text) < 9)
		return 1;

	if (text[0] != '#')
		return 1;

	text++;

	return htoc_internal(text, (unsigned char*)color, 4);
}

static void 
evaluate_property(const char* data, const char* value)
{
	if (!strcmp(data, COLOR_LED_PROP_NAME))
	{
		color c;
		if (!htoc(value, &c))
		{
			led_color = c;
			fprintf(stderr, "%s: Led color set to:\n"
			                "r = %d, g = %d, b = %d, a = %d\n", __func__, c.r, c.g, c.b, c.a);
		}
	}
	else if (!strcmp(data, COLOR_BKGROUND_PROP_NAME))
	{
		color c;
		if (!htoc(value, &c))
		{
			background_color = c;
			fprintf(stderr, "%s: Background color set to:\n"
			                "r = %d, g = %d, b = %d, a = %d\n", __func__, c.r, c.g, c.b, c.a);
		}
	}
	else if (!strcmp(data, COLOR_TITLE_PROP_NAME))
	{
		color c;
		if (!htoc(value, &c))
		{
			title_color = c;
			fprintf(stderr, "%s: Title color set to:\n"
			                "r = %d, g = %d, b = %d, a = %d\n", __func__, c.r, c.g, c.b, c.a);
		}
	}
	else if (!strcmp(data, COLOR_MENU_PROP_NAME))
	{
		color c;
		if (!htoc(value, &c))
		{
			menu_color = c;
			fprintf(stderr, "%s: Menu color set to:\n"
			                "r = %d, g = %d, b = %d, a = %d\n", __func__, c.r, c.g, c.b, c.a);
		}
	}
	else if (!strcmp(data, COLOR_MENU_SEL_PROP_NAME))
	{
		color c;
		if (!htoc(value, &c))
		{
			menu_sel_color = c;
			fprintf(stderr, "%s: Selection color set to:\n"
			                "r = %d, g = %d, b = %d, a = %d\n", __func__, c.r, c.g, c.b, c.a);
		}
	}
	else if (!strcmp(data, COLOR_SCRIPT_PROP_NAME))
	{
		color c;
		if (!htoc(value, &c))
		{
			script_color = c;
			fprintf(stderr, "%s: Script color set to:\n"
			                "r = %d, g = %d, b = %d, a = %d\n", __func__, c.r, c.g, c.b, c.a);
		}
	}
	else if (!strcmp(data, COLOR_CONSOLE_HEADER_PROP_NAME))
	{
		color c;
		if (!htoc(value, &c))
		{
			console_header_color = c;
			fprintf(stderr, "%s: Console header color set to:\n"
			                "r = %d, g = %d, b = %d\n", __func__, c.r, c.g, c.b);
		}
	}
	else if (!strcmp(data, COLOR_CONSOLE_BACKGROUND_PROP_NAME))
	{
		color c;
		if (!htoc(value, &c))
		{
			console_background_color = c;
			fprintf(stderr, "%s: Console background color set to:\n"
			                "r = %d, g = %d, b = %d\n", __func__, c.r, c.g, c.b);
		}
	}
	else if (!strcmp(data, COLOR_CONSOLE_FRONT_PROP_NAME))
	{
		color c;
		if (!htoc(value, &c))
		{
			console_front_color = c;
			fprintf(stderr, "%s: Console front color set to:\n"
			                "r = %d, g = %d, b = %d\n", __func__, c.r, c.g, c.b);
		}
	}
	else if (!strncmp(data, COLOR_CONSOLE_TERMCLR_PROP_NAME_BASE, strlen(COLOR_CONSOLE_TERMCLR_PROP_NAME_BASE)))
	{
		color c;
		if (!htoc(value, &c))
		{
			const char* termclr_ascii = data + strlen(COLOR_CONSOLE_TERMCLR_PROP_NAME_BASE);
			int val = atoi(termclr_ascii);

			if (val >= 30 && val <= 37)
			{
				console_term_colors[val - 30] = c;
				fprintf(stderr, "%s: Console terminal front color no. %d set to:\n"
				                "r = %d, g = %d, b = %d\n", __func__, val, c.r, c.g, c.b);
			}
			else if (val >= 90 && val <= 97)
			{
				console_term_colors[val - 90 + 8] = c;
				fprintf(stderr, "%s: Console terminal bright front color no. %d set to:\n"
				                "r = %d, g = %d, b = %d\n", __func__, val, c.r, c.g, c.b);
			}
		}
	}
	else if (!strncmp(data, UI_INDETERMINATE_FRAMES_PROP_NAME, strlen(UI_INDETERMINATE_FRAMES_PROP_NAME)))
	{
		const char* val_str = data + strlen(UI_INDETERMINATE_FRAMES_PROP_NAME) + 1;
		ui_parameters.indeterminate_frames = atoi(val_str);
		fprintf(stderr, "%s: ui_parameters.indeterminate_frames = %d\n", __func__, ui_parameters.indeterminate_frames);
	}
	else if (!strncmp(data, UI_INSTALL_FRAMES_PROP_NAME, strlen(UI_INSTALL_FRAMES_PROP_NAME)))
	{
		const char* val_str = data + strlen(UI_INSTALL_FRAMES_PROP_NAME) + 1;
		ui_parameters.installing_frames = atoi(val_str);
		fprintf(stderr, "%s: ui_parameters.installing_frames = %d\n", __func__, ui_parameters.installing_frames);
	}
	else if (!strncmp(data, UI_INSTALL_LOC_X_PROP_NAME, strlen(UI_INSTALL_LOC_X_PROP_NAME)))
	{
		const char* val_str = data + strlen(UI_INSTALL_LOC_X_PROP_NAME) + 1;
		ui_parameters.install_overlay_offset_x = atoi(val_str);
		fprintf(stderr, "%s: ui_parameters.install_overlay_offset_x = %d\n", __func__, ui_parameters.install_overlay_offset_x);
	}
	else if (!strncmp(data, UI_INSTALL_LOC_Y_PROP_NAME, strlen(UI_INSTALL_LOC_Y_PROP_NAME)))
	{
		const char* val_str = data + strlen(UI_INSTALL_LOC_Y_PROP_NAME) + 1;
		ui_parameters.install_overlay_offset_y = atoi(val_str);
		fprintf(stderr, "%s: ui_parameters.install_overlay_offset_y = %d\n", __func__, ui_parameters.install_overlay_offset_y);
	}
}

//copied from init
void
load_properties()
{
	unsigned int sz;
	char *key, *value, *eol, *sol, *tmp;
	char* data = read_file(PROPERTY_FILE, &sz);

	if(data == NULL)
		return; 

	sol = data;
	while((eol = strchr(sol, '\n'))) 
	{
		key = sol;
		*eol++ = 0;
		sol = eol;

		value = strchr(key, '=');
		if(value == 0) 
			continue;

		*value++ = 0;

		while(isspace(*key)) 
			key++;

		if(*key == '#') 
			continue;

		tmp = value - 2;
		while((tmp > key) && isspace(*tmp)) 
			*tmp-- = 0;

		while(isspace(*value)) 
			value++;

		tmp = eol - 2;

		while((tmp > value) && isspace(*tmp)) 
			*tmp-- = 0;

		evaluate_property(key, value);
	}
	
	free(data);
}
