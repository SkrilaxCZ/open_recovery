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
#include <pthread.h>

#include "bootloader.h"
#include "common.h"
#include "cutils/properties.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "qwerty.h"
#include "console.h"
#include "ui.h"

//console buffers
#define CONSOLE_MATRIX_TOTAL_ROWS                  1024
#define CONSOLE_MATRIX_BUFFER_ROWS                 64
#define CONSOLE_MATRIX_TOTAL_COLUMNS               128

//console characters
#define CONSOLE_BEEP 7
#define CONSOLE_ESC 27

static char console_text_matrix[CONSOLE_MATRIX_TOTAL_ROWS][CONSOLE_MATRIX_TOTAL_COLUMNS];
static color console_color_matrix[CONSOLE_MATRIX_TOTAL_ROWS][CONSOLE_MATRIX_TOTAL_COLUMNS];

static int console_screen_rows = 0;
static int console_screen_columns = 0;

static volatile int console_cursor_sts = 1;
static volatile clock_t console_cursor_last_update_time = 0;

color console_header_color = {.r = 255, .g = 255, .b = 0, .a = 0};
color console_background_color =  {.r = 0, .g = 0, .b = 0, .a = 0};
color console_front_color =  {.r = 229, .g = 229, .b = 229, .a = 0};

color console_term_colors[16] = 
{
	{ .r = 0,   .g = 0,   .b = 0,   .a = 0 }, //CLR30
	{ .r = 205, .g = 0,   .b = 0,   .a = 0 }, //CLR31
	{ .r = 0,   .g = 205, .b = 0,   .a = 0 }, //CLR32
	{ .r = 205, .g = 205, .b = 0,   .a = 0 }, //CLR33
	{ .r = 0,   .g = 0,   .b = 238, .a = 0 }, //CLR34
	{ .r = 205, .g = 0,   .b = 205, .a = 0 }, //CLR35
	{ .r = 0,   .g = 205, .b = 205, .a = 0 }, //CLR36
	{ .r = 229, .g = 229, .b = 229, .a = 0 }, //CLR37
	
	{ .r = 127, .g = 127, .b = 127, .a = 0 }, //CLR90
	{ .r = 255, .g = 0,   .b = 0,   .a = 0 }, //CLR91
	{ .r = 0,   .g = 255, .b = 0,   .a = 0 }, //CLR92
	{ .r = 255, .g = 255, .b = 0,   .a = 0 }, //CLR93
	{ .r = 92,  .g = 91,  .b = 255, .a = 0 }, //CLR94
	{ .r = 255, .g = 0,   .b = 255, .a = 0 }, //CLR95
	{ .r = 0,   .g = 255, .b = 255, .a = 0 }, //CLR96
	{ .r = 255, .g = 255, .b = 255, .a = 0 }, //CLR97
};

static color console_current_color;

static int console_top_row = 0;
static int console_force_top_row_on_text = 0;
static int console_force_top_row_reserve = 0;
static int console_cur_row = 0;
static int console_cur_column = 0;

static int console_escaped_state = 0;
static char console_escaped_buffer[64];
static char* console_escaped_sequence;

static void draw_console_cursor(int row, int column, char letter)
{
	if (!console_cursor_sts)
		return;

	gr_color(console_front_color.r, console_front_color.g, console_front_color.b, 255);
	gr_fill_l(column * char_width, row * char_height, 
					(column+1)*char_width, (row+1)*char_height);

	if (letter != '\0')
	{
		gr_color(console_background_color.r, console_background_color.g, console_background_color.b, 255);

		char text[2];
		text[0] = letter;
		text[1] = '\0';

		gr_text_l(column * char_width, (row+1)*char_height-1, text);
	}
}

static void* console_cursor_thread(void *cookie)
{
	UNUSED(cookie);

	while (ui_get_view_mode() == VIEWMODE_CONSOLE)
	{
		clock_t time_now = clock();
		double since_last_update = ((double)(time_now - console_cursor_last_update_time)) / CLOCKS_PER_SEC;

		if (since_last_update >= 0.5)
		{
			pthread_mutex_lock(&ui_update_mutex);
			console_cursor_sts = console_cursor_sts ? 0 : 1;
			console_cursor_last_update_time = time_now;
			update_screen_locked();
			pthread_mutex_unlock(&ui_update_mutex);
		}
		usleep(20000);
	}

	return NULL;
}

static void draw_console_line(int row, const char* t, const color* c) {
  
	char letter[2];
	letter[1] = '\0';

	int i = 0;

	while(t[i] != '\0') 
	{
		letter[0] = t[i];
		gr_color(c[i].r, c[i].g, c[i].b, c[i].a);
		gr_text_l(i * char_width, (row+1)*char_height-1, letter);
		i++;
	}
}

void draw_console_locked()
{	
	gr_color(console_background_color.r, console_background_color.g, console_background_color.b, 255);
	gr_fill(0, 0, gr_fb_width(), gr_fb_height());

	int draw_cursor = 0;
	
	int i;	
	for (i = console_top_row; i < console_top_row + console_screen_rows; i++)
	{
		draw_console_line(i - console_top_row, console_text_matrix[i], console_color_matrix[i]);
		
		if (i == console_cur_row)
		{
			draw_cursor = 1;
			break;
		}
	}
	
	if(draw_cursor)
		draw_console_cursor(console_cur_row-console_top_row, console_cur_column, console_text_matrix[console_cur_row][console_cur_column]);
}

void ui_console_begin()
{
	int i;
	
	pthread_mutex_lock(&ui_update_mutex);
	ui_set_view_mode(VIEWMODE_CONSOLE);
	console_cursor_sts = 1;
	console_cursor_last_update_time = clock();
	console_top_row = 0;
	console_cur_row = 0;
	console_cur_column = 0;
	console_escaped_state = 0;

	//calculate the number of columns and rows
	console_screen_rows = ui_console_get_height() / char_height;
	console_screen_columns = ui_console_get_width() / char_width + 1; //+1 for null terminator

	console_force_top_row_on_text = 0;
	console_force_top_row_reserve = 1 - console_screen_rows;

	memset(console_text_matrix, ' ', (CONSOLE_MATRIX_TOTAL_ROWS) * (console_screen_columns));
	memset(console_color_matrix, 0, (CONSOLE_MATRIX_TOTAL_ROWS) * (console_screen_columns) * sizeof(color));

	for (i = 0; i < (CONSOLE_MATRIX_TOTAL_ROWS); i++)
		console_text_matrix[i][console_screen_columns - 1] = '\0'; 
	
	console_current_color = console_front_color;

	pthread_t t;
	pthread_create(&t, NULL, console_cursor_thread, NULL);

	update_screen_locked();
	pthread_mutex_unlock(&ui_update_mutex);
}

void ui_console_end()
{
	pthread_mutex_lock(&ui_update_mutex);
	ui_set_view_mode(VIEWMODE_NORMAL);
	update_screen_locked();
	console_screen_rows = 0;
	console_screen_columns = 0;
	pthread_mutex_unlock(&ui_update_mutex);
}

int ui_console_get_num_rows()
{
	return console_screen_rows;
}

int ui_console_get_num_columns()
{
	return console_screen_columns - 1;
}

//landscape, swap em
int ui_console_get_width()
{
	return gr_fb_height();
}

int ui_console_get_height()
{
	return gr_fb_width();
}

void ui_console_scroll_up(int num_rows)
{
	pthread_mutex_lock(&ui_update_mutex);
	
	if (console_top_row - num_rows < 0)
		console_top_row = 0;
	else
		console_top_row -= num_rows;

	update_screen_locked();
	pthread_mutex_unlock(&ui_update_mutex);	
}

void ui_console_scroll_down(int num_rows)
{
	int max_row_top = console_cur_row - console_screen_rows + 1;
	
	if (max_row_top < console_force_top_row_on_text)
		max_row_top = console_force_top_row_on_text;
	
	if (max_row_top < 0)
		max_row_top = 0;
	
	pthread_mutex_lock(&ui_update_mutex);
	if (console_top_row + num_rows > max_row_top)
		console_top_row = max_row_top;
	else
		console_top_row += num_rows;
		
	update_screen_locked();
	pthread_mutex_unlock(&ui_update_mutex);		
}

void ui_console_get_system_front_color(int which, unsigned char* r, unsigned char* g, unsigned char* b)
{
	color c = {.r = 0, .g = 0, .b = 0};
	
	switch(which)
	{
		case CONSOLE_HEADER_COLOR:
			c = console_header_color;
			break;
			
		case CONSOLE_DEFAULT_BACKGROUND_COLOR:
			c = console_background_color;
			break;
			
		case CONSOLE_DEFAULT_FRONT_COLOR:
			c = console_front_color;
			break;
	}
	
	*r = c.r;
	*g = c.g;
	*b = c.b;
}

void ui_console_set_system_front_color(int which)
{
	switch(which)
	{
		case CONSOLE_HEADER_COLOR:
			console_current_color = console_header_color;
			break;
			
		case CONSOLE_DEFAULT_BACKGROUND_COLOR:
			console_current_color = console_background_color;
			break;
			
		case CONSOLE_DEFAULT_FRONT_COLOR:
			console_current_color = console_front_color;
			break;
	}
}

void ui_console_get_front_color(unsigned char* r, unsigned char* g, unsigned char* b)
{
	*r = console_current_color.r;
	*g = console_current_color.g;
	*b = console_current_color.b;
}

void ui_console_set_front_color(unsigned char r, unsigned char g, unsigned char b)
{
	console_current_color.r = r;
	console_current_color.g = g;
	console_current_color.b = b;
}

void console_set_front_term_color(int ascii_code)
{
	if (ascii_code >= 30 && ascii_code < 37)
		ui_console_set_front_color(
			console_term_colors[ascii_code - 30].r,
			console_term_colors[ascii_code - 30].g,
			console_term_colors[ascii_code - 30].b);
	else if (ascii_code >= 90 && ascii_code < 97)
		ui_console_set_front_color(
			console_term_colors[ascii_code - 90 + 8].r,
			console_term_colors[ascii_code - 90 + 8].g,
			console_term_colors[ascii_code - 90 + 8].b);
}

static void console_put_char(char c)
{
	int end;

	switch(c)
	{
		case '\n':
			//fprintf(stderr, "Row %d, Column %d, Char \"LINE BREAK\"\n", console_cur_row, console_cur_column);
			console_cur_row++;
			console_force_top_row_reserve++;
			break;
		
		case '\r':
			//fprintf(stderr, "Row %d, Column %d, Char \"CARRIAGE RETURN\"\n", console_cur_row, console_cur_column);
			console_cur_column = 0;
			break;

		case '\t':
			//fprintf(stderr, "Row %d, Column %d, Char \"TAB\"\n", console_cur_row, console_cur_column);
			//tab is per 5
			end = console_cur_column + (5 - console_cur_column % 5);

			if (end >= console_screen_columns - 2)
			{
				int i;
				for (i = console_cur_column; i < console_screen_columns - 1; i++)
					console_text_matrix[console_cur_row][i] = ' ';

				console_cur_column = 0;
				console_cur_row++;
				console_force_top_row_reserve++;
			}
			else
			{
				int i;
				for (i = console_cur_column; i < end; i++)
					console_text_matrix[console_cur_row][i] = ' ';
				
				console_cur_column = end;
			}
			break;

		case '\b':
			//fprintf(stderr, "Row %d, Column %d, Char \"BACKSPACE\"\n", console_cur_row, console_cur_column);
			if (console_cur_column == 0)
			{
				if (console_cur_row == 0)
					break;
				
				console_cur_column = console_screen_columns - 2;
				console_cur_row--;
			}
			else
				console_cur_column--;		
			break;

		case CONSOLE_BEEP: //BELL - use LED for that
			ui_led_blink(0);
			break;

		default:
			//fprintf(stderr, "Row %d, Column %d, Char %d\n", console_cur_row, console_cur_column, c);
			console_text_matrix[console_cur_row][console_cur_column] = c;
			console_color_matrix[console_cur_row][console_cur_column] = console_current_color;
			console_cur_column++;

			if (console_cur_column > console_screen_columns - 2)
			{
				console_cur_column = 0;
				console_cur_row++;
				console_force_top_row_reserve++;
			}
			break;
	}

	if (console_cur_row == CONSOLE_MATRIX_TOTAL_ROWS)
	{
		int shift = CONSOLE_MATRIX_BUFFER_ROWS;
		//fprintf(stderr, "Shifting the rows by %d.\n", shift);
		int j;

		for (j = 0; j < CONSOLE_MATRIX_TOTAL_ROWS - CONSOLE_MATRIX_BUFFER_ROWS; j++)
		{
			memcpy(console_text_matrix[j], console_text_matrix[j + shift], console_screen_columns);
			memcpy(console_color_matrix[j], console_color_matrix[j + shift], console_screen_columns * sizeof(color));
		}

		for (j = CONSOLE_MATRIX_TOTAL_ROWS - CONSOLE_MATRIX_BUFFER_ROWS; j < CONSOLE_MATRIX_TOTAL_ROWS; j++)
		{
			memset(console_text_matrix[j], ' ', console_screen_columns);
			console_text_matrix[j][console_screen_columns - 1] = '\0';
			memset(console_color_matrix[j], 0, console_screen_columns * sizeof(color)); 
		}

		console_cur_row -= shift;

		console_force_top_row_on_text -= shift;
		if (console_force_top_row_on_text < 0)
			console_force_top_row_on_text = 0;
	}
}

static void console_unescape()
{
	int len = strlen(console_escaped_buffer);
	int was_unescaped = 0;
	int noSqrBrackets = 0;
	int noRoundBracketsLeft = 0;
	int noRoundBracketsRight = 0;
	int noQuestionMarks = 0;
	int parameters[32];
	int noParameters = 0;

	char argument = '\0';
	char *ptr;

	memset(parameters, 0, sizeof(int) * 32);

	//first parse it
	for (ptr = console_escaped_buffer; *ptr != '\0'; ++ptr)
	{
		if (*ptr == '[')
			noSqrBrackets++;
		else if (*ptr == '(')
			noRoundBracketsLeft++;
		else if (*ptr == ')')
			noRoundBracketsRight++;
		else if (*ptr == '?')
			noQuestionMarks++;
		else if (*ptr == ';')
			noParameters++;
		else if (*ptr >= '0' && *ptr <= '9')
			parameters[noParameters] = parameters[noParameters] * 10 + (*ptr - '0');
		else
		{
			argument = *ptr;
			break;
		}
	}

	//was used for indexing, so increment it
	noParameters++;

	fprintf(stderr, "ESCAPE: no. Brackets S%d RL%d RR%d, no. Question Marks %d, no. params %d, argument %c,\n", 
		noSqrBrackets, noRoundBracketsLeft, noRoundBracketsRight, noQuestionMarks, noParameters, argument);
	fprintf(stderr, "PARAMS:");

	int i, j;
	for (i = 0; i < noParameters; i++)
		fprintf(stderr, " %d", parameters[i]);
		
	fprintf(stderr, "\n");

	if (noSqrBrackets == 1 && noRoundBracketsLeft == 0 && noRoundBracketsRight == 0 && noQuestionMarks == 0)
	{
		switch (argument)
		{
			//======================================================================
			// UPPERCASE
			//======================================================================

			//move up n lines, but not out of screen
			case 'A':
				console_cur_row -= parameters[0];
				if (console_force_top_row_on_text > console_cur_row)
					console_cur_row = console_force_top_row_on_text;

				//set the top reserve
				console_force_top_row_reserve = 1 - (console_force_top_row_on_text + 
					console_screen_rows - console_cur_row);
				was_unescaped = 1;
				break;

			//move down n lines, but not out of screen
			case 'B':
				console_cur_row += parameters[0];
				if (console_cur_row >= console_force_top_row_on_text + console_screen_rows)
					console_cur_row = console_force_top_row_on_text + console_screen_rows - 1;

				//set the top reserve
				console_force_top_row_reserve = 1 - (console_force_top_row_on_text + 
					console_screen_rows - console_cur_row);
				was_unescaped = 1;
				break;

			//move right, but not out of the line
			case 'C':
				console_cur_column += parameters[0];
				if ( console_cur_column >= (console_screen_columns - 1) )
					console_cur_column = console_screen_columns - 2;

				was_unescaped = 1;
				break;

			//move left, but not out of the line
			case 'D':
				console_cur_column -= parameters[0];
				
				if (console_cur_column < 0)
					console_cur_column = 0;

				was_unescaped = 1;
				break;

			//cursor to top-left + offset
			case 'H':
				if (parameters[0] >= console_screen_rows)
					parameters[0] = console_screen_rows - 1;

				if (parameters[1] >= (console_screen_columns - 1) )
					parameters[1] = console_screen_columns - 2;

				console_cur_row = console_top_row + parameters[0];
				console_cur_column = parameters[1];
				console_force_top_row_on_text = console_top_row;
				console_force_top_row_reserve = 1 - (console_top_row + 
					console_screen_rows - console_cur_row);				

				was_unescaped = 1;
				break;

			//clear below cursor
			case 'J':
				for (j = console_cur_column; j < (console_screen_columns - 1); j++)
				{
					console_text_matrix[console_cur_row][j] = ' ';
					console_color_matrix[console_cur_row][j].r = 0;
					console_color_matrix[console_cur_row][j].g = 0;
					console_color_matrix[console_cur_row][j].b = 0;
				}

				for (j = console_cur_row + 1; j < CONSOLE_MATRIX_TOTAL_ROWS; j++)
				{
					memset(console_text_matrix[j], ' ', console_screen_columns);
					console_text_matrix[j][console_screen_columns - 1] = '\0';		
					memset(console_color_matrix[j], 0, console_screen_columns * sizeof(color));
				}
				was_unescaped = 1;
				break;

			//clear from cursor cursor
			case 'K':
				if (parameters[0] == 0)
				{
					for (j = console_cur_column; j < (console_screen_columns - 1); j++)
					{
						console_text_matrix[console_cur_row][j] = ' ';
						console_color_matrix[console_cur_row][j].r = 0;
						console_color_matrix[console_cur_row][j].g = 0;
						console_color_matrix[console_cur_row][j].b = 0;
					}
				}
				else if (parameters[0] == 1)
				{
					for (j = 0; j <= console_cur_column; j++)
					{
						console_text_matrix[console_cur_row][j] = ' ';
						console_color_matrix[console_cur_row][j].r = 0;
						console_color_matrix[console_cur_row][j].g = 0;
						console_color_matrix[console_cur_row][j].b = 0;
					}
				}
				else if (parameters[0] == 2)
				{
					for (j = 0; j < (console_screen_columns - 1); j++)
					{
						console_text_matrix[console_cur_row][j] = ' ';
						console_color_matrix[console_cur_row][j].r = 0;
						console_color_matrix[console_cur_row][j].g = 0;
						console_color_matrix[console_cur_row][j].b = 0;
					}
				}

				was_unescaped = 1;
				break;

			//======================================================================
			// LOWERCASE
			//======================================================================
		
			//only coloring, ignore bolding, italic etc.
			//background color changing is not supported
			case 'm':

				//consider 'm' to be succesfully unescaped in all cases
				was_unescaped = 1;

				for (i = 0; i < noParameters; i++)
				{
					switch (parameters[i])
					{
						case 0: //reset
						case 39: //default text color
							ui_console_set_system_front_color(CONSOLE_DEFAULT_FRONT_COLOR);
							break;
						case 30:
						case 31:
						case 32:
						case 33:
						case 34:
						case 35:
						case 36:
						case 37:
						case 90:
						case 91:
						case 92:
						case 93:
						case 94:
						case 95:
						case 96:
						case 97:
							console_set_front_term_color(parameters[i]);
							break;
					}
				}
		}
	}

	if (!was_unescaped) //send it ala text then
	{
		console_put_char('^');
		int e;
		for (e = 0; console_escaped_buffer[e] != '\0'; e++)
			console_put_char(console_escaped_buffer[e]);
	}
}

static void console_put_escape_sequence(char c)
{
	*console_escaped_sequence = c;
	console_escaped_sequence++;

	if (c != '[' && c != '(' && c != '?' && c != ')' && c != ';' && !(c >= '0' && c <= '9'))
	{
		*console_escaped_sequence = '\0';
		fprintf(stderr, "Escape character: %s\n", console_escaped_buffer);
		console_unescape();
		console_escaped_state = 0;	
	}
}

void ui_console_print(const char *text)
{
	pthread_mutex_lock(&ui_update_mutex);
	const char *ptr;
	for (ptr = text; *ptr != '\0'; ++ptr)
	{
		//parse escape characters here
		if (*ptr == CONSOLE_ESC)
		{
			console_escaped_state = 1;
			console_escaped_buffer[0] = '\0';
			console_escaped_sequence = &(console_escaped_buffer[0]);
			continue;
		}
		else if (!console_escaped_state)
			console_put_char(*ptr);
		else
			console_put_escape_sequence(*ptr);
	}

	if (console_force_top_row_reserve > 0)
	{
		console_force_top_row_on_text += console_force_top_row_reserve;
		console_force_top_row_reserve = 0;
	}

	console_top_row = console_force_top_row_on_text;
	if (console_top_row < 0)
		console_top_row = 0;	

	console_cursor_sts = 1;
	console_cursor_last_update_time = clock();	
	update_screen_locked();
	pthread_mutex_unlock(&ui_update_mutex);	
}

static void init_console()
{
	ui_set_background(BACKGROUND_ICON_NONE);
	ui_console_begin();
}

static void exit_console()
{
	if (get_capslock_state())
		toggle_capslock_state();
	
	if (get_altlock_state())
		toggle_altlock_state();
	
	ui_set_background(BACKGROUND_ICON_ERROR);
	ui_console_end();
}

static int create_subprocess(const char* cmd, const char* arg0, const char* arg1, pid_t* process_id) 
{
	char devname[256];
	int ptm;
	pid_t pid;

	ptm = open("/dev/ptmx", O_RDWR); // | O_NOCTTY);
	if(ptm < 0)
	{
		fprintf(stderr, "[ cannot open /dev/ptmx - %s ]\n", strerror(errno));
		return -1;
	}

	fcntl(ptm, F_SETFD, FD_CLOEXEC);

	if(grantpt(ptm) || unlockpt(ptm) || (ptsname_r(ptm, devname, sizeof(devname)) != 0) )
	{
		fprintf(stderr, "[ trouble with /dev/ptmx - %s ]\n", strerror(errno));
		return -1;
	}

	pid = fork();
	if(pid < 0) 
	{
		fprintf(stderr, "- fork failed: %s -\n", strerror(errno));
		return -1;
	}

	if(pid == 0)
	{
		int pts;

		setsid();

		pts = open(devname, O_RDWR);
		if(pts < 0) exit(-1);

		dup2(pts, 0);
		dup2(pts, 1);
		dup2(pts, 2);

		close(ptm);

		//so the profile detects whether it's phone console or not
		setenv("OPEN_RECOVERY_CONSOLE", "1", 1);
		execl(cmd, cmd, arg0, arg1, NULL);
		exit(-1);
	} 
	else 
	{
		*process_id = (int) pid;
		return ptm;
	}
}

static void set_nonblocking(int fd)
{
	//nonblocking mode
	int f = fcntl(fd, F_GETFL, 0);
	//set bit for non-blocking flag
	f |= O_NONBLOCK;
	//change flags on fd
	fcntl(fd, F_SETFL, f);
}

static void send_escape_sequence(int ptmfd, const char* seq)
{
	char cmd[64];
	strcpy(cmd+1, seq);
	cmd[0] = CONSOLE_ESC;
	write(ptmfd, &cmd, strlen(seq)+1); //+1 for the CONSOLE_ESC, null terminator is not sent
}

static pid_t child;
static int childfd;
static volatile int console_read_thread_terminated;

static void* console_read_thread(void *cookie)
{
	UNUSED(cookie);

	//buffer for pipe
	char buffer[1024+1];
	int sts;
	
	while (1)
	{
		int rv = read(childfd, buffer, 1024);		
			
		if (rv <= 0)
		{
			//bash has stopped
			fprintf(stderr, "run_console: there was a read error %d.\n", errno);
			break;
		}
		else
		{
			//if the string is read only partially, it won't be null terminated		  		
			buffer[rv] = 0;
			fprintf(stderr, "run_console: received input of %d characters.\n", rv);
			ui_console_print(buffer);
			fprintf(stderr, "run_console: input succesfully displayed.\n");
		}
	}
	
	console_read_thread_terminated = 1;
	ui_wake_key_waiting_thread();
	return NULL;
}

int run_console()
{
	init_console();

	childfd = create_subprocess("/sbin/bash", "-i", NULL, &child);

	if (childfd < 0)
	{
		exit_console();
		return CONSOLE_FAILED_START;
	}

	//status for the waitpid	
	int sts;
	int shell_error = 0;
	int force_quit = 0;

	//clear keys
	ui_clear_key_queue();

	//set the size
	struct winsize sz;
	sz.ws_row = ui_console_get_num_rows();
	sz.ws_col = ui_console_get_num_columns();
	sz.ws_xpixel = ui_console_get_width();
	sz.ws_ypixel = ui_console_get_height();
	ioctl(childfd, TIOCSWINSZ, &sz);

	ui_console_set_system_front_color(CONSOLE_HEADER_COLOR);
	ui_console_print(get_current_device()->name);
	ui_console_print("\r\n");
	ui_console_print("Open Recovery " OPEN_RECOVERY_VERSION_NUMBER " Console\r\n");

	ui_console_set_system_front_color(CONSOLE_DEFAULT_FRONT_COLOR);
	console_read_thread_terminated = 0;
	
	pthread_t t;
	pthread_create(&t, NULL, console_read_thread, NULL);

	//handle the i/o between the recovery and bash
	//manage the items to print here, but the actual printing is done in another thread
	while (1)
	{
		if (force_quit)
		{
			kill(child, SIGKILL);
			fprintf(stderr, "run_console: forcibly terminated.\n");
			waitpid(child, &sts, 0);
			break;
		}		

		//evaluate one keyevent
		int keycode = ui_wait_key();
		
		if (console_read_thread_terminated)
		{
			//poked from read thread that bash has stopped
			waitpid(child, &sts, 0);
			break;
		}
		
		if (keycode >= 0 && keycode <= KEY_MAX)
		{
			//sym + delete kills bash
			if (ui_key_pressed(KEY_APOSTROPHE) && ui_key_pressed(KEY_BACKSPACE) && ((keycode == KEY_APOSTROPHE) || (keycode == KEY_BACKSPACE)))
			{
				force_quit = 1;
				continue;
			}

			//reply + key sends signal (i.e sym acts like CTRL)
			if (ui_key_pressed(KEY_REPLY) && keycode != KEY_REPLY)
			{
				char my_esc[2];
				char key = qwerty_layout.normal[keycode];

				my_esc[0] = key - 'a' + 1;
				my_esc[1] = '\0';
				send_escape_sequence(childfd, my_esc);
				continue;
			}

			int shift = ui_key_pressed(KEY_LEFTSHIFT) | ui_key_pressed(KEY_RIGHTSHIFT) | get_capslock_state();
			int alt = ui_key_pressed(KEY_LEFTALT) | ui_key_pressed(KEY_RIGHTALT) | get_altlock_state();
			char key = resolve_keypad_character(keycode, shift, alt);

			switch (key)
			{
				case 0:
					fprintf(stderr, "evaluate_key: unhandled keycode %d, shift %d, alt %d.\n", keycode, shift, alt);
					key = 0;
					break;

				case CHAR_NOTHING:
					break;

				case CHAR_SCROLL_DOWN:
					ui_console_scroll_down(1);
					break;

				case CHAR_SCROLL_UP:
					ui_console_scroll_up(1);
					break;

				case CHAR_BIG_SCROLL_DOWN:
					ui_console_scroll_down(10);
					break;

				case CHAR_BIG_SCROLL_UP:
					ui_console_scroll_up(10);
					break;

				case CHAR_KEY_UP:
					send_escape_sequence(childfd, "[A");
					break;

				case CHAR_KEY_LEFT:
					send_escape_sequence(childfd, "[D");
					break;

				case CHAR_KEY_RIGHT:
					send_escape_sequence(childfd, "[C");
					break;

				case CHAR_KEY_DOWN:	
					send_escape_sequence(childfd, "[B");
					break;

				case CHAR_KEY_CAPSLOCK:
					toggle_capslock_state();
					break;

				case CHAR_KEY_ALTLOCK:
					toggle_altlock_state();
					break;

				case CHAR_KEY_ESCAPE:
					send_escape_sequence(childfd, "[");
					break;

				default:
					write(childfd, &key, 1);
					break;
			}
		}
	}

	//check exit status
	if (WIFEXITED(sts)) 
	{
		if (WEXITSTATUS(sts) != 0) 
		{
			fprintf(stderr, "run_console: bash exited with status %d.\n",
			WEXITSTATUS(sts));
			shell_error = WEXITSTATUS(sts);
		}
	} 
	else if (WIFSIGNALED(sts)) 
	{
		fprintf(stderr, "run_console: bash terminated by signal %d.\n",
		WTERMSIG(sts));
		
		if (force_quit)
			shell_error = CONSOLE_FORCE_QUIT;
		else
			shell_error = 1;
	}

	close(childfd);
	exit_console();
	return shell_error;
}
