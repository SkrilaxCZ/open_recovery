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
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

#include "common.h"
#include <cutils/android_reboot.h>
#include "minui/minui.h"
#include "qwerty.h"
#include "ui.h"

#define MAX_COLS 96
#define MAX_ROWS 64

#define MENU_MAX_ROWS 100

#define UI_WAIT_KEY_TIMEOUT_SEC    120

#define LED_OFF         0x00
#define LED_ON          0x01
#define LED_BLINK       0x02
#define LED_BLINK_ONCE  0x03

#define LED_FILE_RED    "/sys/class/leds/red/brightness"
#define LED_FILE_GREEN  "/sys/class/leds/green/brightness"
#define LED_FILE_BLUE   "/sys/class/leds/blue/brightness"

#define LCD_BACKLIGHT_FILE      "/sys/class/backlight/lcd-backlight/brightness"
#define KEYBOARD_BACKLIGHT_FILE "/sys/class/leds/keyboard-backlight/brightness"
#define GOVERNOR_FILE           "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"

ui_parameters_t ui_parameters = 
{
	0,       // indeterminate progress bar frames
	20,      // fps
	0,       // installation icon frames (0 == static image)
	0, 0,    // installation icon overlay offset
};

pthread_mutex_t ui_update_mutex = PTHREAD_MUTEX_INITIALIZER;
static gr_surface gBackgroundIcon[NUM_BACKGROUND_ICONS];
static gr_surface *gInstallationOverlay;
static gr_surface *gProgressBarIndeterminate;
static gr_surface gProgressBarEmpty;
static gr_surface gProgressBarFill;

static const struct { gr_surface* surface; const char *name; } BITMAPS[] = 
{
	{ &gBackgroundIcon[BACKGROUND_ICON_INSTALLING], "icon_installing" },
	{ &gBackgroundIcon[BACKGROUND_ICON_ERROR],      "icon_error" },
	{ &gProgressBarEmpty,                           "progress_empty" },
	{ &gProgressBarFill,                            "progress_fill" },
	{ NULL,                                         NULL },
};

static int gCurrentIcon = 0;
static int gInstallingFrame = 0;

static enum ProgressBarType 
{
	PROGRESSBAR_TYPE_NONE,
	PROGRESSBAR_TYPE_INDETERMINATE,
	PROGRESSBAR_TYPE_NORMAL,
} gProgressBarType = PROGRESSBAR_TYPE_NONE;

// Progress bar scope of current operation
static float gProgressScopeStart = 0, gProgressScopeSize = 0, gProgress = 0;
static double gProgressScopeTime, gProgressScopeDuration;

static int current_view_mode = VIEWMODE_NORMAL;

// Set to 1 when both graphics pages are the same (except for the progress bar)
static int gPagesIdentical = 0;

// Character size
int char_width = 0;
int char_height = 0;

// Log text overlay, displayed when a magic key is pressed
static char text[MAX_ROWS][MAX_COLS];
static int text_cols = 0, text_rows = 0;
static int text_col = 0, text_row = 0, text_top = 0;
static int show_text = 1;

static char menu[MAX_ROWS][MAX_COLS];
static int show_menu = 0;
static int menu_top = 0, menu_items = 0, menu_item_top = 0, menu_items_screen = 0, menu_sel = 0, menu_title_length = 0;
static int battery_charge = -1, battery_charging = 0;

// Colors
color background_color = {.r = 0, .g = 0, .b = 0, .a = 160};
color title_color = {.r = 255, .g = 55, .b = 5, .a = 255};
color menu_color = {.r = 255, .g = 55, .b = 5, .a = 255};
color menu_sel_color = {.r = 255, .g = 255, .b = 255, .a = 255};
color script_color = {.r = 255, .g = 255, .b = 0, .a = 255};
color led_color = {.r = 255, .g = 0, .b = 0};

// Key event input queue
static pthread_mutex_t key_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t key_queue_cond = PTHREAD_COND_INITIALIZER;
static int key_queue[256], key_queue_len = 0;
static volatile char key_pressed[KEY_MAX + 1];

static pthread_cond_t led_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t led_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile unsigned int led_sts;

// Interactive user input
#define USER_INPUT_TEXT_MAX 32

static char user_input_header[USER_INPUT_TEXT_MAX + 1];
static char user_input_text[USER_INPUT_TEXT_MAX + 1];

void ui_set_view_mode(int mode)
{
	if (mode >= 0 && mode < NUM_VIEWMODES)
		current_view_mode = mode;
}

int ui_get_view_mode()
{
	return current_view_mode;
}

// Return the current time as a double (including fractions of a second).
static double now() 
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1000000.0;
}

// Draw the given frame over the installation overlay animation.  The
// background is not cleared or draw with the base icon first; we
// assume that the frame already contains some other frame of the
// animation.  Does nothing if no overlay animation is defined.
// Should only be called with ui_update_mutex locked.
static void draw_install_overlay_locked(int frame) 
{
	if (gInstallationOverlay == NULL) 
		return;
	
	gr_surface surface = gInstallationOverlay[frame];
	int iconWidth = gr_get_width(surface);
	int iconHeight = gr_get_height(surface);
	gr_blit(surface, 0, 0, iconWidth, iconHeight, ui_parameters.install_overlay_offset_x, ui_parameters.install_overlay_offset_y);
}

// Clear the screen and draw the currently selected background icon (if any).
// Should only be called with ui_update_mutex locked.
static void draw_background_locked(int icon)
{
	gPagesIdentical = 0;
	gr_color(0, 0, 0, 255);
	gr_fill(0, 0, gr_fb_width(), gr_fb_height());

	if (icon) 
	{
		gr_surface surface = gBackgroundIcon[icon];
		int iconWidth = gr_get_width(surface);
		int iconHeight = gr_get_height(surface);
		int iconX = (gr_fb_width() - iconWidth) / 2;
		int iconY = (gr_fb_height() - iconHeight) / 2;
		gr_blit(surface, 0, 0, iconWidth, iconHeight, iconX, iconY);
		if (icon == BACKGROUND_ICON_INSTALLING) 
			draw_install_overlay_locked(gInstallingFrame);
	}
}

// Draw the progress bar (if any) on the screen.  Does not flip pages.
// Should only be called with ui_update_mutex locked.
static void draw_progress_locked()
{
	if (gProgressBarType != PROGRESSBAR_TYPE_NONE) 
	{
		int iconHeight = gr_get_height(gBackgroundIcon[BACKGROUND_ICON_INSTALLING]);
		int width = gr_get_width(gProgressBarEmpty);
		int height = gr_get_height(gProgressBarEmpty);

		int dx = (gr_fb_width() - width)/2;
		int dy = (3*gr_fb_height() + iconHeight - 2*height)/4;

		// Erase behind the progress bar (in case this was a progress-only update)
		gr_color(0, 0, 0, 255);
		gr_fill(dx, dy, width, height);

		if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL) 
		{
			float progress = gProgressScopeStart + gProgress * gProgressScopeSize;
			int pos = (int) (progress * width);

			if (pos > 0) 
				gr_blit(gProgressBarFill, 0, 0, pos, height, dx, dy);

			if (pos < width-1) 
				gr_blit(gProgressBarEmpty, pos, 0, width-pos, height, dx+pos, dy);
		}

		if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE) 
		{
			static int frame = 0;
			gr_blit(gProgressBarIndeterminate[frame], 0, 0, width, height, dx, dy);
			frame = (frame + 1) % ui_parameters.indeterminate_frames;
		}
	}
}

static void draw_text_line(int row, const char* t) 
{
	if (t[0] != '\0') 
		gr_text(0, (row+1)*char_height-1, t);
}

static void draw_user_input_locked()
{
	int box_height = char_height * 3;
	int box_width = char_width * USER_INPUT_TEXT_MAX;
	
	int rx = gr_fb_height()/2 - box_width/2 - char_width/2;
	int ry = gr_fb_width()/2 - box_height/2 - char_height/2;
	
	int tx = gr_fb_height()/2 - box_width/2;
	int ty = gr_fb_width()/2 - box_height/2;
	
	// Draw rectangle and header in menu color
	gr_color(menu_color.r, menu_color.g, menu_color.b, menu_color.a);
	gr_fill_l(rx - 1, ry - 1,	rx + box_width + char_width + 1, ry + 1);
	gr_fill_l(rx + box_width + char_width - 1, ry - 1,	rx + box_width + char_width + 1, ry + box_height + char_height + 1);
	gr_fill_l(rx - 1, ry - 1,	rx + 1, ry + box_height + char_height + 1);
	gr_fill_l(rx - 1, ry + box_height + char_height - 1,	rx + box_width + char_width + 1, ry + box_height + char_height + 1);
	gr_text_l(tx, ty + char_height, user_input_header);
	
	// Draw user input in script color
	gr_color(script_color.r, script_color.g, script_color.b, script_color.a);
	gr_text_l(tx, ty + 3*char_height, user_input_text);
}

// Redraw everything on the screen.  Does not flip pages.
// Should only be called with ui_update_mutex locked.

extern void draw_console_locked();

static void draw_screen_locked()
{	
	switch (current_view_mode)
	{
		case VIEWMODE_NORMAL:
			draw_background_locked(gCurrentIcon);
			draw_progress_locked();
			
			if (show_text) 
			{
				gr_color(background_color.r, background_color.g, background_color.b, background_color.a);
				gr_fill(0, 0, gr_fb_width(), gr_fb_height());
		
				int i = 0;
				if (show_menu) 
				{
					gr_color(menu_color.r, menu_color.g, menu_color.b, menu_color.a);
					gr_fill(0, (menu_top+menu_sel-menu_item_top) * char_height,
									gr_fb_width(), (menu_top+menu_sel-menu_item_top+1)*char_height+1);
		
					//first title - with title color
					gr_color(title_color.r, title_color.g, title_color.b, title_color.a);
					
					if (menu_title_length > 0)
					{
						// add battery status
						char battery_buff[MAX_COLS];
						char text_buff[32];
						char* battery_text;
						
						if (battery_charge != -1)
						{
							sprintf(text_buff, "%d%%", battery_charge);
							
							if (battery_charging)
								strcat(text_buff, "+");
							else
								strcat(text_buff, " ");
								
							battery_text = text_buff;
						}
						else
							battery_text = "N/A";
						
						int batt_text_len = strlen(battery_text);
						int menu_text_len = strlen(menu[i]);
						
						strcpy(battery_buff, menu[i]);
						
						if (menu_text_len > text_cols - batt_text_len)
						{
							battery_buff[text_cols - batt_text_len - 1] = ' ';
							battery_buff[text_cols - batt_text_len] = '\0';
						}
						else
						{
							int j;
							for (j = menu_text_len; j < text_cols - batt_text_len; j++)
								battery_buff[j] = ' ';
								
							battery_buff[text_cols - batt_text_len] = '\0';
						}
						
						strncat(battery_buff, battery_text, text_cols);
						draw_text_line(i, battery_buff);
						i++;
					}
		
					for (; i < menu_title_length; ++i) 
						draw_text_line(i, menu[i]);
		
						//then draw it till top -  with normal color
						gr_color(menu_color.r, menu_color.g, menu_color.b, menu_color.a);
		
						for (; i < menu_top; ++i) 
							draw_text_line(i, menu[i]);
		
						if (menu_items != menu_items_screen)
						{
							for (; i < menu_top + menu_items_screen; ++i)
							{
								if (i + menu_item_top == menu_top + menu_sel) 
								{
									gr_color(menu_sel_color.r, menu_sel_color.g, menu_sel_color.b, menu_sel_color.a);
									draw_text_line(i, menu[i + menu_item_top]);
									gr_color(menu_color.r, menu_color.g, menu_color.b, menu_color.a);
								} 
								else 
									draw_text_line(i, menu[i + menu_item_top]);
							}
							gr_fill(0, i*char_height+char_height/2-1,
											gr_fb_width(), i*char_height+char_height/2+1);
							++i;
		
							//scrollbar
							int width = char_width;
							int height = menu_items_screen*char_height+1;
							
							gr_color(menu_color.r, menu_color.g, menu_color.b, menu_color.a);
							gr_fill(gr_fb_width() - width, menu_top*char_height,
											gr_fb_width(), menu_top*char_height + height);
		
							//highlighting
							float fraction = height / (float)menu_items;
							float highlighted = fraction * menu_items_screen;
							float offset = menu_item_top * fraction;
		
							int offset_i;
							int highlighted_i;
		
							//ensure that if last menu_item is selected, there is no strip in the scrollbar
							if (menu_item_top + menu_items_screen == menu_items)
							{
								highlighted_i = (int)highlighted;
								offset_i = height - highlighted_i;
							}
							else
							{
								offset_i = (int)offset;
								highlighted_i = (int)highlighted;
							}
		
							gr_color(menu_sel_color.r, menu_sel_color.g, menu_sel_color.b, menu_sel_color.a);
							gr_fill(gr_fb_width() - width, menu_top*char_height + offset_i,
											gr_fb_width(), menu_top*char_height + offset_i + highlighted_i);
							
						}
						else
						{
							gr_color(menu_color.r, menu_color.g, menu_color.b, menu_color.a);
							gr_fill(0, (menu_top+menu_sel) * char_height,
											gr_fb_width(), (menu_top+menu_sel+1)*char_height+1);
							
							for (; i < menu_top + menu_items; ++i)
							{
								if (i == menu_top + menu_sel) 
								{
									gr_color(menu_sel_color.r, menu_sel_color.g, menu_sel_color.b, menu_sel_color.a);
									draw_text_line(i, menu[i]);
									gr_color(menu_color.r, menu_color.g, menu_color.b, menu_color.a);
								} 
								else 
									draw_text_line(i, menu[i]);
							}
							gr_fill(0, i*char_height+char_height/2-1,
											gr_fb_width(), i*char_height+char_height/2+1);
							++i;
						}
				}
		
				gr_color(script_color.r, script_color.g, script_color.b, script_color.a);
				
				for (; i < text_rows; ++i) 
					draw_text_line(i, text[(i+text_top) % text_rows]);  
			}
			break;
			
		case VIEWMODE_CONSOLE:
			draw_console_locked();
			break;
			
		case VIEWMODE_TEXT_INPUT:
			draw_background_locked(gCurrentIcon);
			
			gr_color(background_color.r, background_color.g, background_color.b, background_color.a);
			gr_fill(0, 0, gr_fb_width(), gr_fb_height());
			
			draw_user_input_locked();
			break;
	}
}

// Redraw everything on the screen and flip the screen (make it visible).
// Should only be called with ui_update_mutex locked.
void update_screen_locked()
{
	draw_screen_locked();
	gr_flip();
}

// Updates only the progress bar, if possible, otherwise redraws the screen.
// Should only be called with ui_update_mutex locked.
void update_progress_locked()
{
	if (show_text || !gPagesIdentical) 
	{
		draw_screen_locked();    // Must redraw the whole screen
		gPagesIdentical = 1;
	} 
	else
		draw_progress_locked();  // Draw only the progress bar and overlays

	gr_flip();
}

// Keeps the progress bar updated, even when the process is otherwise busy.
static void* progress_thread(void *cookie)
{
	double interval = 1.0 / ui_parameters.update_fps;
	for (;;) 
	{
		double start = now();
		pthread_mutex_lock(&ui_update_mutex);

		int redraw = 0;
		int update_overlay = 0;

		// update the installation animation, if active
		// skip this if we have a text overlay (too expensive to update)
		if (gCurrentIcon == BACKGROUND_ICON_INSTALLING && ui_parameters.installing_frames > 0 && !show_text) 
		{
			gInstallingFrame = (gInstallingFrame + 1) % ui_parameters.installing_frames;
			update_overlay = 1;
		}

		// update the progress bar animation, if active
		if (gProgressBarType == PROGRESSBAR_TYPE_INDETERMINATE)
			redraw = 1;

		// move the progress bar forward on timed intervals, if configured
		int duration = gProgressScopeDuration;
		if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && duration > 0) 
		{
			double elapsed = now() - gProgressScopeTime;
			float progress = 1.0 * elapsed / duration;
			
			if (progress > 1.0) 
				progress = 1.0;
			
			if (progress > gProgress) 
			{
					gProgress = progress;
					redraw = 1;
			}
		}

		if (update_overlay)
			draw_install_overlay_locked(gInstallingFrame);

		if (redraw) 
			update_progress_locked();

		pthread_mutex_unlock(&ui_update_mutex);
		double end = now();
		// minimum of 20ms delay between frames
		double delay = interval - (end-start);
		if (delay < 0.02) 
			delay = 0.02;
		
		usleep((long)(delay * 1000000));
	}
	return NULL;
}

void ui_led_toggle(int state)
{
	pthread_mutex_lock(&led_mutex);

	if(state)
		led_sts = LED_ON;
	else
		led_sts = LED_OFF;

	pthread_cond_signal(&led_cond); 
	pthread_mutex_unlock(&led_mutex);
}

void ui_led_blink(int continuously)
{
	pthread_mutex_lock(&led_mutex);

	if(continuously)
		led_sts = LED_BLINK;
	else
		led_sts = LED_BLINK_ONCE;

	pthread_cond_signal(&led_cond); 
	pthread_mutex_unlock(&led_mutex);
  
}

static void led_on(FILE* ledfp_r, FILE* ledfp_g, FILE* ledfp_b)
{
	char buffer[20];

	sprintf(buffer, "%d\n", led_color.r);
	fwrite(buffer, 1, strlen(buffer), ledfp_r);

	sprintf(buffer, "%d\n", led_color.g);
	fwrite(buffer, 1, strlen(buffer), ledfp_g);

	sprintf(buffer, "%d\n", led_color.b);
	fwrite(buffer, 1, strlen(buffer), ledfp_b);
	
	fflush(ledfp_r);
	fflush(ledfp_g);
	fflush(ledfp_b);
}

static void led_off(FILE* ledfp_r, FILE* ledfp_g, FILE* ledfp_b)
{
	fwrite("0\n", 1, strlen("0\n"), ledfp_r);
	fwrite("0\n", 1, strlen("0\n"), ledfp_g);
	fwrite("0\n", 1, strlen("0\n"), ledfp_b);
	
	fflush(ledfp_r);
	fflush(ledfp_g);
	fflush(ledfp_b);
}

static void* led_thread(void* cookie)
{
	unsigned int state = 0;
	unsigned int waitperiod = 0;
	FILE *ledfp_r, *ledfp_g, *ledfp_b;
  
	ledfp_r = fopen(LED_FILE_RED, "w");
	ledfp_g = fopen(LED_FILE_GREEN, "w");
	ledfp_b = fopen(LED_FILE_BLUE, "w");

	while(1) 
	{
		pthread_mutex_lock(&led_mutex);

		switch (led_sts)
		{
			case LED_OFF:
				state = 0;
				led_off(ledfp_r, ledfp_g, ledfp_b);

				while (led_sts == LED_OFF) 
					pthread_cond_wait(&led_cond, &led_mutex);
				break;
					
				case LED_ON:
					state = 1;
					led_on(ledfp_r, ledfp_g, ledfp_b);	
				
				while (led_sts == LED_ON) 
					pthread_cond_wait(&led_cond, &led_mutex);
				break;

				case LED_BLINK_ONCE:
					state = 1;

					led_on(ledfp_r, ledfp_g, ledfp_b);	
					waitperiod = 800000;
					led_sts = LED_OFF;
					break;

				case LED_BLINK:
					state = state ? 0 : 1;

					if (state)
						led_on(ledfp_r, ledfp_g, ledfp_b);
					else
						led_off(ledfp_r, ledfp_g, ledfp_b);
				
					waitperiod = 800000;
					break;
		}

		pthread_mutex_unlock(&led_mutex);

		//when blinking, we want to finish it, not interrupt it
		if (waitperiod > 0)
		{
			usleep(waitperiod);
			waitperiod = 0;
		}
	}

	fclose(ledfp_r);
	fclose(ledfp_g);
	fclose(ledfp_b);
	return NULL;
}

static int rel_sum = 0;
static int last_key_down = -1;

static int input_callback(int fd, short revents, void* data)
{
	struct input_event ev;
	int ret;
	int fake_key = 0;

	ret = ev_get_input(fd, revents, &ev);
	if (ret)
		return -1;

	if (ev.type == EV_SYN)
		return 0;
	else if (ev.type == EV_REL) 
	{
		if (ev.code == REL_Y) 
		{
			// accumulate the up or down motion reported by
			// the trackball.  When it exceeds a threshold
			// (positive or negative), fake an up/down
			// key event.
			rel_sum += ev.value;
			if (rel_sum > 3) 
			{
				fake_key = 1;
				ev.type = EV_KEY;
				ev.code = KEY_DOWN;
				ev.value = 1;
				rel_sum = 0;
			} 
			else if (rel_sum < -3) 
			{
				fake_key = 1;
				ev.type = EV_KEY;
				ev.code = KEY_UP;
				ev.value = 1;
				rel_sum = 0;
			}
		}
	}
	else
		rel_sum = 0;

	if (ev.type != EV_KEY || ev.code > KEY_MAX)
		return 0;

	pthread_mutex_lock(&key_queue_mutex);
	if (!fake_key) 
	{
		// our "fake" keys only report a key-down event (no
		// key-up), so don't record them in the key_pressed
		// table.
		key_pressed[ev.code] = ev.value;

		if (ev.value == 1)
			last_key_down = ev.code;
		else if (ev.value == 0 && ev.code == last_key_down)
			last_key_down = -1;
	}

	const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
	if (ev.value > 0 && key_queue_len < queue_max) 
	{
		key_queue[key_queue_len++] = ev.code;
		pthread_cond_signal(&key_queue_cond);
	}
	pthread_mutex_unlock(&key_queue_mutex);
	return 0;
}

// Reads input events, handles special hot keys, and adds to the key queue.
static void* input_thread(void* cookie)
{
	for (;;) 
	{
		if (!ev_wait(-1))
			ev_dispatch();
	}
	return NULL;
}

// Sends events for long pressed keys. Ticks every 50ms to check
static void* kbd_thread(void* cookie)
{
	int handling_key = -1, no_accum_ticks = 0, first_accum = 1;

	for (;;)
	{
		if (handling_key != -1)
		{
			if (last_key_down == -1 || last_key_down != handling_key)
				handling_key = -1;
			else
			{
				if (first_accum)
				{
					no_accum_ticks++;
					if (no_accum_ticks == 14)
						first_accum = 0;
				}
				else
				{
					pthread_mutex_lock(&key_queue_mutex);
					const int queue_max = sizeof(key_queue) / sizeof(key_queue[0]);
					if (key_queue_len < queue_max)
					{
						key_queue[key_queue_len++] = handling_key;
						pthread_cond_signal(&key_queue_cond);
					}
					pthread_mutex_unlock(&key_queue_mutex);
				}
			}
		}
		else if (last_key_down != -1 && last_key_down != KEY_LEFTSHIFT &&
				last_key_down != KEY_RIGHTSHIFT && last_key_down != KEY_CAPSLOCK &&
				menu_handle_key(last_key_down, 1) != SELECT_ITEM)
		{
			first_accum = 1;
			no_accum_ticks = 0;
			handling_key = last_key_down;
		}
		usleep(50000);
	}

	return NULL;
}

void ui_screen_on()
{
	FILE* ledfd;
	FILE* govfd;
	
	// LCD
	ledfd = fopen(LCD_BACKLIGHT_FILE, "w");
	fwrite("255\n", 1, strlen("255\n"), ledfd);
	fclose(ledfd);

	// Keyboard
	ledfd = fopen(KEYBOARD_BACKLIGHT_FILE, "w");
	fwrite("255\n", 1, strlen("255\n"), ledfd);
	fclose(ledfd);

	// Governor
	govfd = fopen(GOVERNOR_FILE, "w");
	fwrite("performance\n", 1, strlen("performance\n"), govfd);
	fclose(govfd);
}

void ui_screen_off()
{
	FILE* ledfd;
	FILE* govfd;
	
	// LCD
	ledfd = fopen(LCD_BACKLIGHT_FILE, "w");
	fwrite("0\n", 1, strlen("0\n"), ledfd);
	fclose(ledfd);

	// Keyboard
	ledfd = fopen(KEYBOARD_BACKLIGHT_FILE, "w");
	fwrite("0\n", 1, strlen("0\n"), ledfd);
	fclose(ledfd);

	// Governor
	govfd = fopen(GOVERNOR_FILE, "w");
	fwrite("powersave\n", 1, strlen("powersave\n"), govfd);
	fclose(govfd);
}

void ui_init()
{
	gr_init();
	ev_init(input_callback, NULL);
	
	gr_font_size(&char_width, &char_height);
	fprintf(stderr, "Framebuffer size: %d x %d\n", gr_fb_height(), gr_fb_width());
	fprintf(stderr, "Font size: %d x %d\n", char_height, char_width);

	int i;
	for (i = 0; BITMAPS[i].name != NULL; ++i) 
	{
		int result = res_create_surface(BITMAPS[i].name, BITMAPS[i].surface);
		if (result < 0)
			LOGE("Missing bitmap %s\n(Code %d)\n", BITMAPS[i].name, result);
	}

	gProgressBarIndeterminate = malloc(ui_parameters.indeterminate_frames * sizeof(gr_surface));
	for (i = 0; i < ui_parameters.indeterminate_frames; ++i) 
	{
		char filename[40];
		// "indeterminate01.png", "indeterminate02.png", ...
		sprintf(filename, "indeterminate%02d", i+1);
		int result = res_create_surface(filename, gProgressBarIndeterminate+i);
		if (result < 0)
			LOGE("Missing bitmap %s\n(Code %d)\n", filename, result);
	}

	if (ui_parameters.installing_frames > 0) 
	{
		gInstallationOverlay = malloc(ui_parameters.installing_frames * sizeof(gr_surface));
		for (i = 0; i < ui_parameters.installing_frames; ++i) 
		{
			char filename[40];
			// "icon_installing_overlay01.png",
			// "icon_installing_overlay02.png", ...
			sprintf(filename, "icon_installing_overlay%02d", i+1);
			int result = res_create_surface(filename, gInstallationOverlay+i);
			if (result < 0)
				LOGE("Missing bitmap %s\n(Code %d)\n", filename, result);
		}

		// Adjust the offset to account for the positioning of the
		// base image on the screen.
		if (gBackgroundIcon[BACKGROUND_ICON_INSTALLING] != NULL) 
		{
			gr_surface bg = gBackgroundIcon[BACKGROUND_ICON_INSTALLING];
			ui_parameters.install_overlay_offset_x += (gr_fb_width() - gr_get_width(bg)) / 2;
			ui_parameters.install_overlay_offset_y += (gr_fb_height() - gr_get_height(bg)) / 2;
		}
	} 
	else
		gInstallationOverlay = NULL;

	int icon_height = gr_get_height(gBackgroundIcon[BACKGROUND_ICON_INSTALLING]);
	int progbar_height = gr_get_height(gProgressBarEmpty);
	int progbar_y = (3*gr_fb_height() + icon_height - 2*progbar_height)/4;

	text_col = text_row = 0;
	text_rows = (progbar_y / char_height) - 1;
	if (text_rows > MAX_ROWS)
		text_rows = MAX_ROWS;
	text_top = 1;

	text_cols = gr_fb_width() / char_width;
	if (text_cols > MAX_COLS - 1)
		text_cols = MAX_COLS - 1;

	ui_screen_on();

	// Threads
	pthread_t t;
	pthread_create(&t, NULL, progress_thread, NULL);
	pthread_create(&t, NULL, input_thread, NULL);
	pthread_create(&t, NULL, kbd_thread, NULL);
	pthread_create(&t, NULL, led_thread, NULL);
}

void ui_set_background(int icon)
{
	pthread_mutex_lock(&ui_update_mutex);
	gCurrentIcon = icon;
	update_screen_locked();
	pthread_mutex_unlock(&ui_update_mutex);
}

void ui_show_indeterminate_progress()
{
	pthread_mutex_lock(&ui_update_mutex);
	if (gProgressBarType != PROGRESSBAR_TYPE_INDETERMINATE)
	{
		gProgressBarType = PROGRESSBAR_TYPE_INDETERMINATE;
		update_progress_locked();
	}
	pthread_mutex_unlock(&ui_update_mutex);
}

void ui_show_progress(float portion, int seconds)
{
	pthread_mutex_lock(&ui_update_mutex);
	gProgressBarType = PROGRESSBAR_TYPE_NORMAL;
	gProgressScopeStart += gProgressScopeSize;
	gProgressScopeSize = portion;
	gProgressScopeTime = now();
	gProgressScopeDuration = seconds;
	gProgress = 0;
	update_progress_locked();
	pthread_mutex_unlock(&ui_update_mutex);
}

void ui_set_progress(float fraction)
{
	pthread_mutex_lock(&ui_update_mutex);
	if (fraction < 0.0) fraction = 0.0;
	if (fraction > 1.0) fraction = 1.0;
	if (gProgressBarType == PROGRESSBAR_TYPE_NORMAL && fraction > gProgress) 
	{
		// Skip updates that aren't visibly different.
		int width = gr_get_width(gProgressBarIndeterminate[0]);
		float scale = width * gProgressScopeSize;
		if ((int) (gProgress * scale) != (int) (fraction * scale)) 
		{
			gProgress = fraction;
			update_progress_locked();
		}
	}
	pthread_mutex_unlock(&ui_update_mutex);
}

void ui_reset_progress()
{
	pthread_mutex_lock(&ui_update_mutex);
	gProgressBarType = PROGRESSBAR_TYPE_NONE;
	gProgressScopeStart = gProgressScopeSize = 0;
	gProgressScopeTime = gProgressScopeDuration = 0;
	gProgress = 0;
	update_screen_locked();
	pthread_mutex_unlock(&ui_update_mutex);
}

void ui_print(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, 256, fmt, ap);
	va_end(ap);

	fputs(buf, stdout);

	// This can get called before ui_init(), so be careful.
	pthread_mutex_lock(&ui_update_mutex);
	if (text_rows > 0 && text_cols > 0) 
	{
		char *ptr;
		for (ptr = buf; *ptr != '\0'; ++ptr) 
		{
			if (*ptr == '\n' || text_col >= text_cols) 
			{
				text[text_row][text_col] = '\0';
				text_col = 0;
				text_row = (text_row + 1) % text_rows;
				if (text_row == text_top) 
					text_top = (text_top + 1) % text_rows;
			}
			if (*ptr != '\n') 
				text[text_row][text_col++] = *ptr;
		}
		text[text_row][text_col] = '\0';
		update_screen_locked();
	}
	pthread_mutex_unlock(&ui_update_mutex);
}

void ui_print_raw(const char *buf)
{
	fputs(buf, stderr);
	
	// This can get called before ui_init(), so be careful.
	pthread_mutex_lock(&ui_update_mutex);
	if (text_rows > 0 && text_cols > 0) {
		const char *ptr;
		for (ptr = buf; *ptr != '\0'; ++ptr) {
			if (*ptr == '\n' || text_col >= text_cols) {
				text[text_row][text_col] = '\0';
				text_col = 0;
				text_row = (text_row + 1) % text_rows;
				if (text_row == text_top) text_top = (text_top + 1) % text_rows;
			}
			if (*ptr != '\n') text[text_row][text_col++] = *ptr;
		}
		text[text_row][text_col] = '\0';
		update_screen_locked();
	}
	pthread_mutex_unlock(&ui_update_mutex);
}

void ui_start_menu(const char** headers, const char** items, int title_length, int start_sel) 
{
	int i;
	pthread_mutex_lock(&ui_update_mutex);

  //reset progress
  gProgressBarType = PROGRESSBAR_TYPE_NONE;
  gProgressScopeStart = gProgressScopeSize = 0;
  gProgressScopeTime = gProgressScopeDuration = 0;
  gProgress = 0;

	menu_title_length = title_length;

	if (text_rows > 0 && text_cols > 0)
	{
		for (i = 0; i < MENU_MAX_ROWS; ++i) 
		{
			if (headers[i] == NULL)
				break;

			strncpy(menu[i], headers[i], text_cols);
			menu[i][text_cols] = '\0';
		}
		menu_top = i;
		for (; i < MENU_MAX_ROWS; ++i) 
		{
			if (items[i-menu_top] == NULL)
				break;
			strncpy(menu[i], items[i-menu_top], text_cols);
			menu[i][text_cols] = '\0';
		}

		menu_items = i - menu_top;
		menu_items_screen = (text_rows - 4) - menu_top;
		if (menu_items < menu_items_screen)
			menu_items_screen = menu_items;

		show_menu = 1;
		menu_sel = start_sel;
		if (menu_sel >= menu_items_screen)
			menu_item_top = menu_sel - menu_items_screen + 1;
		else
			menu_item_top = 0;

		update_screen_locked();
	}

	pthread_mutex_unlock(&ui_update_mutex);
}

int ui_menu_select(int sel) 
{
	int old_sel;
	pthread_mutex_lock(&ui_update_mutex);
	if (show_menu > 0) 
	{
		old_sel = menu_sel;
		menu_sel = sel;
		if (menu_sel < 0)
			menu_sel = 0;
		
		if (menu_sel >= menu_items) 
			menu_sel = menu_items-1;
		
		sel = menu_sel;
		if (menu_sel != old_sel)
		{
			if (menu_sel < menu_item_top)
				menu_item_top = menu_sel;
			else if (menu_sel >= menu_item_top + menu_items_screen)
				menu_item_top = menu_sel - menu_items_screen + 1;

			update_screen_locked();
		}
	}
	pthread_mutex_unlock(&ui_update_mutex);
	return sel;
}

void ui_end_menu() 
{
	int i;
	pthread_mutex_lock(&ui_update_mutex);
	
	if (show_menu > 0 && text_rows > 0 && text_cols > 0)
	{
		show_menu = 0;
		update_screen_locked();
	}
	pthread_mutex_unlock(&ui_update_mutex);
}

int ui_text_visible()
{
	pthread_mutex_lock(&ui_update_mutex);
	int visible = show_text;
	pthread_mutex_unlock(&ui_update_mutex);
	return visible;
}

int ui_text_ever_visible()
{
	return 1;
}

void ui_show_text(int visible)
{
	pthread_mutex_lock(&ui_update_mutex);
	show_text = visible;
	update_screen_locked();
	pthread_mutex_unlock(&ui_update_mutex);
}

// Return true if USB is connected.
static int usb_connected() 
{
	int fd = open("/sys/class/android_usb/android0/state", O_RDONLY);
	if (fd < 0) 
	{
		printf("failed to open /sys/class/android_usb/android0/state: %s\n", strerror(errno));
		return 0;
	}

	char buf;
	/* USB is connected if android_usb state is CONNECTED or CONFIGURED */
	int connected = (read(fd, &buf, 1) == 1) && (buf == 'C');
	if (close(fd) < 0) 
		printf("failed to close /sys/class/android_usb/android0/state: %s\n", strerror(errno));
	return connected;
}

int ui_get_key()
{
	int key = -1;
	pthread_mutex_lock(&key_queue_mutex);
	
	if (key_queue_len != 0)
	{
		key = key_queue[0];
		memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
	}
	
	pthread_mutex_unlock(&key_queue_mutex); 
	return key;
}

static volatile int wait_key_break = 0;

int ui_wait_key()
{
	pthread_mutex_lock(&key_queue_mutex);

	// Time out after UI_WAIT_KEY_TIMEOUT_SEC, unless a USB cable is
	// plugged in.
	do 
	{
		struct timeval now;
		struct timespec timeout;
		gettimeofday(&now, NULL);
		timeout.tv_sec = now.tv_sec;
		timeout.tv_nsec = now.tv_usec * 1000;
		timeout.tv_sec += UI_WAIT_KEY_TIMEOUT_SEC;

		int rc = 0;
		while (key_queue_len == 0 && rc != ETIMEDOUT)
		{ 
			rc = pthread_cond_timedwait(&key_queue_cond, &key_queue_mutex, &timeout);
			
			if (wait_key_break)
			{
				wait_key_break = 0;
				pthread_mutex_unlock(&key_queue_mutex);
				return -1;
			}
		}

	} while (usb_connected() && key_queue_len == 0);

	int key = -1;
	if (key_queue_len > 0) 
	{
		key = key_queue[0];
		memcpy(&key_queue[0], &key_queue[1], sizeof(int) * --key_queue_len);
	}
	pthread_mutex_unlock(&key_queue_mutex);
	return key;
}

void ui_wake_key_waiting_thread()
{
	pthread_mutex_lock(&key_queue_mutex);
	wait_key_break = 1;
	pthread_cond_signal(&key_queue_cond);
	pthread_mutex_unlock(&key_queue_mutex);
}

int ui_key_pressed(int key)
{
	// This is a volatile static array, don't bother locking
	return key_pressed[key];
}

void ui_clear_key_queue() 
{
	pthread_mutex_lock(&key_queue_mutex);
	key_queue_len = 0;
	pthread_mutex_unlock(&key_queue_mutex);
}

int ui_get_num_columns()
{
	return text_cols;
}

void ui_user_input(const char* header, char* reply)
{
	int typed_characters = 0;
	int i;
	
	strncpy(user_input_header, header, USER_INPUT_TEXT_MAX);
	
	for (i = 0; i < USER_INPUT_TEXT_MAX; i++)
		user_input_text[i] = '_';
		
	user_input_text[USER_INPUT_TEXT_MAX] = '\0';
	ui_set_view_mode(VIEWMODE_TEXT_INPUT);
	update_screen_locked();
	
	// Wait for user input
	while (1)
	{
		//evaluate one keyevent
		int keycode = ui_wait_key();
		if (keycode >= 0 && keycode <= KEY_MAX)
		{
			char key = resolve_keypad_character(keycode, ui_key_pressed(KEY_LEFTSHIFT) | ui_key_pressed(KEY_RIGHTSHIFT) | get_capslock_state());
			
			if (key == CHAR_KEY_CAPSLOCK)
				toggle_capslock_state();
			else if (key == '\n')
				break;
			else if (key == '\b')
			{
				if (typed_characters > 0)
				{
					typed_characters--;
					user_input_text[typed_characters] = '_';
				}
				
				update_screen_locked();
			}
			else if ( (key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == '-' || key == ' ')
			{
				if (typed_characters < USER_INPUT_TEXT_MAX)
				{
					user_input_text[typed_characters] = key;
					typed_characters++;
				}
				
				update_screen_locked();
			}
		}
	}
	
	if (get_capslock_state())
		toggle_capslock_state();
		
	if (typed_characters > 0)
	{
		strncpy(reply, user_input_text, typed_characters);
		reply[typed_characters] = '\0';
	}
	else
		reply[0] = '\0';
		
	ui_set_view_mode(VIEWMODE_NORMAL);
	update_screen_locked();
}

void ui_set_battery_data(int new_battery_charge, int new_battery_charging)
{
	pthread_mutex_lock(&ui_update_mutex);
	if (battery_charge != new_battery_charge || battery_charging != new_battery_charging)
	{
		battery_charge = new_battery_charge;
		battery_charging = new_battery_charging;
		
		if (show_menu)
			update_screen_locked();
	}
	pthread_mutex_unlock(&ui_update_mutex);
}
