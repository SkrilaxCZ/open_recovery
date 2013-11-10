/*
 * Copyright (C) 2010-2013 Skrilax_CZ
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

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "qwerty.h"
#include "console.h"

#define CAPSLOCK_BACKLIGHT_FILE  "/sys/class/leds/shift-key-light/brightness"
static int capslock_on = 0;
static FILE* capslock_led = NULL;

keyboard_layout qwerty_layout;

int get_capslock_state()
{
	return capslock_on;
}

void toggle_capslock_state()
{
	capslock_on = !capslock_on;
	if (capslock_led != NULL)
	{
		if (capslock_on)
			fwrite("255\n", 1, strlen("255\n"), capslock_led);
		else
			fwrite("0\n", 1, strlen("0\n"), capslock_led);

		fflush(capslock_led);
	}
}

void init_keypad_layout()
{
	//clear it 
	
	memset(qwerty_layout.normal, 0, KEY_MAX+1);
	memset(qwerty_layout.shifted, 0, KEY_MAX+1);

	//characters
	
	qwerty_layout.normal[KEY_A] = 'a';
	qwerty_layout.shifted[KEY_A] = 'A';

	qwerty_layout.normal[KEY_B] = 'b';
	qwerty_layout.shifted[KEY_B] = 'B';

	qwerty_layout.normal[KEY_C] = 'c';
	qwerty_layout.shifted[KEY_C] = 'C';

	qwerty_layout.normal[KEY_D] = 'd';
	qwerty_layout.shifted[KEY_D] = 'D';

	qwerty_layout.normal[KEY_E] = 'e';
	qwerty_layout.shifted[KEY_E] = 'E';

	qwerty_layout.normal[KEY_F] = 'f';
	qwerty_layout.shifted[KEY_F] = 'F';

	qwerty_layout.normal[KEY_G] = 'g';
	qwerty_layout.shifted[KEY_G] = 'G';

	qwerty_layout.normal[KEY_H] = 'h';
	qwerty_layout.shifted[KEY_H] = 'H';

	qwerty_layout.normal[KEY_I] = 'i';
	qwerty_layout.shifted[KEY_I] = 'I';

	qwerty_layout.normal[KEY_J] = 'j';
	qwerty_layout.shifted[KEY_J] = 'J';

	qwerty_layout.normal[KEY_K] = 'k';
	qwerty_layout.shifted[KEY_K] = 'K';

	qwerty_layout.normal[KEY_L] = 'l';
	qwerty_layout.shifted[KEY_L] = 'L';

	qwerty_layout.normal[KEY_M] = 'm';
	qwerty_layout.shifted[KEY_M] = 'M';

	qwerty_layout.normal[KEY_N] = 'n';
	qwerty_layout.shifted[KEY_N] = 'N';

	qwerty_layout.normal[KEY_O] = 'o';
	qwerty_layout.shifted[KEY_O] = 'O';

	qwerty_layout.normal[KEY_P] = 'p';
	qwerty_layout.shifted[KEY_P] = 'P';

	qwerty_layout.normal[KEY_Q] = 'q';
	qwerty_layout.shifted[KEY_Q] = 'Q';

	qwerty_layout.normal[KEY_R] = 'r';
	qwerty_layout.shifted[KEY_R] = 'R';

	qwerty_layout.normal[KEY_S] = 's';
	qwerty_layout.shifted[KEY_S] = 'S';

	qwerty_layout.normal[KEY_T] = 't';
	qwerty_layout.shifted[KEY_T] = 'T';

	qwerty_layout.normal[KEY_U] = 'u';
	qwerty_layout.shifted[KEY_U] = 'U';

	qwerty_layout.normal[KEY_V] = 'v';
	qwerty_layout.shifted[KEY_V] = 'V';

	qwerty_layout.normal[KEY_W] = 'w';
	qwerty_layout.shifted[KEY_W] = 'W';

	qwerty_layout.normal[KEY_X] = 'x';
	qwerty_layout.shifted[KEY_X] = 'X';

	qwerty_layout.normal[KEY_Y] = 'y';
	qwerty_layout.shifted[KEY_Y] = 'Y';


	qwerty_layout.normal[KEY_Z] = 'z';
	qwerty_layout.shifted[KEY_Z] = 'Z';

	//numbers

	qwerty_layout.normal[KEY_1] = '1';
	qwerty_layout.shifted[KEY_1] = '!';

	qwerty_layout.normal[KEY_2] = '2';
	qwerty_layout.shifted[KEY_2] = '@';

	qwerty_layout.normal[KEY_3] = '3';
	qwerty_layout.shifted[KEY_3] = '#';

	qwerty_layout.normal[KEY_4] = '4';
	qwerty_layout.shifted[KEY_4] = '$';

	qwerty_layout.normal[KEY_5] = '5';
	qwerty_layout.shifted[KEY_5] = '%';

	qwerty_layout.normal[KEY_6] = '6';
	qwerty_layout.shifted[KEY_6] = '^';

	qwerty_layout.normal[KEY_7] = '7';
	qwerty_layout.shifted[KEY_7] = '&';

	qwerty_layout.normal[KEY_8] = '8';
	qwerty_layout.shifted[KEY_8] = '*';

	qwerty_layout.normal[KEY_9] = '9';
	qwerty_layout.shifted[KEY_9] = '(';

	qwerty_layout.normal[KEY_0] = '0';
	qwerty_layout.shifted[KEY_0] = ')';

	//symbols 

	qwerty_layout.normal[KEY_DOT] = '.';
	qwerty_layout.shifted[KEY_DOT] = ':';

	qwerty_layout.normal[KEY_COMMA] = ',';
	qwerty_layout.shifted[KEY_COMMA] = ';';

	qwerty_layout.normal[KEY_SLASH] = '/';
	qwerty_layout.shifted[KEY_SLASH] = '?';

	qwerty_layout.normal[KEY_GRAVE] = '\'';
	qwerty_layout.shifted[KEY_GRAVE] = '\"';

	qwerty_layout.normal[KEY_TAB] = '\t';
	qwerty_layout.shifted[KEY_TAB] = '\t';

	qwerty_layout.normal[KEY_SPACE] = ' ';
	qwerty_layout.shifted[KEY_SPACE] = ' ';

	qwerty_layout.normal[KEY_LEFTSHIFT] = CHAR_NOTHING;
	qwerty_layout.normal[KEY_LEFTSHIFT] = CHAR_NOTHING;

	qwerty_layout.normal[KEY_CAPSLOCK] = CHAR_KEY_CAPSLOCK;
	qwerty_layout.shifted[KEY_CAPSLOCK] = CHAR_KEY_CAPSLOCK;

	qwerty_layout.normal[KEY_REPLY] = CHAR_NOTHING;
	qwerty_layout.shifted[KEY_REPLY] = CHAR_NOTHING;

	qwerty_layout.normal[KEY_APOSTROPHE] = CHAR_NOTHING;
	qwerty_layout.shifted[KEY_APOSTROPHE] = CHAR_NOTHING;

	qwerty_layout.normal[KEY_ENTER] = '\n';
	qwerty_layout.shifted[KEY_ENTER] = '\n';

	qwerty_layout.normal[KEY_BACKSPACE] = '\b';
	qwerty_layout.shifted[KEY_BACKSPACE] = '\b';

	qwerty_layout.normal[KEY_MINUS] = '-';
	qwerty_layout.shifted[KEY_MINUS] = '_';

	qwerty_layout.normal[KEY_EQUAL] = '=';
	qwerty_layout.shifted[KEY_EQUAL] = '+';

	//arrow keys 

	qwerty_layout.normal[KEY_UP] = CHAR_KEY_LEFT; 
	qwerty_layout.shifted[KEY_UP] = '<';

	qwerty_layout.normal[KEY_LEFT] = CHAR_KEY_DOWN;
	qwerty_layout.shifted[KEY_LEFT] = '|';

	qwerty_layout.normal[KEY_RIGHT] = CHAR_KEY_UP;
	qwerty_layout.shifted[KEY_RIGHT] = '~';

	qwerty_layout.normal[KEY_DOWN] = CHAR_KEY_RIGHT;
	qwerty_layout.shifted[KEY_DOWN] = '>';

	//external keys 

	qwerty_layout.normal[KEY_VOLUMEDOWN] = CHAR_SCROLL_DOWN;
	qwerty_layout.shifted[KEY_VOLUMEDOWN] = CHAR_BIG_SCROLL_DOWN;

	qwerty_layout.normal[KEY_VOLUMEUP] = CHAR_SCROLL_UP;
	qwerty_layout.shifted[KEY_VOLUMEUP] = CHAR_BIG_SCROLL_UP;

	qwerty_layout.normal[KEY_CAMERA] = CHAR_NOTHING;
	qwerty_layout.shifted[KEY_CAMERA] = CHAR_NOTHING;

	qwerty_layout.normal[KEY_HP] = CHAR_NOTHING;
	qwerty_layout.shifted[KEY_HP] = CHAR_NOTHING;
	
	//capslock LED

	capslock_led = fopen(CAPSLOCK_BACKLIGHT_FILE, "w");
	if (capslock_led == NULL)
		fprintf(stderr, "Could not open Caps Lock LED.\n");
	else
	{
		fwrite("0\n", 1, strlen("0\n"), capslock_led);
		fflush(capslock_led);
	}
}

char resolve_keypad_character(int keycode, int shift)
{
	return shift ? qwerty_layout.shifted[keycode] : qwerty_layout.normal[keycode];
}

int menu_handle_key(int key_code, int visible)
{
	if (visible) 
	{
		switch (key_code) 
		{
			case KEY_DOWN:
			case KEY_VOLUMEDOWN:
				return HIGHLIGHT_DOWN;

			case KEY_UP:
			case KEY_VOLUMEUP:
				return HIGHLIGHT_UP;

			case KEY_REPLY:
			case KEY_CAMERA:
			case KEY_ENTER:
				return SELECT_ITEM;
		}
	}

	return NO_ACTION;
}
