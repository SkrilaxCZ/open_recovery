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
 
#ifndef UI_H
#define UI_H

#include <pthread.h>

//file with properties for ui
#define PROPERTY_FILE "/res/ui.prop"

typedef struct 
{
	// number of frames in indeterminate progress bar animation
	int indeterminate_frames;

	// number of frames per second to try to maintain when animating
	int update_fps;

	// number of frames in installing animation.  may be zero for a
	// static installation icon.
	int installing_frames;

	// the install icon is animated by drawing images containing the
	// changing part over the base icon.  These specify the
	// coordinates of the upper-left corner.
	int install_overlay_offset_x;
	int install_overlay_offset_y;

} ui_parameters_t;

// Global instance
extern ui_parameters_t ui_parameters;

//the structure for color
typedef struct
{
	unsigned char r;
	unsigned char g;
	unsigned char b;
	unsigned char a;
} color;

extern color background_color;
extern color title_color;
extern color menu_color;
extern color menu_sel_color;
extern color script_color;
extern color led_color;

extern int char_width;
extern int char_height;

// Initialize the graphics system.
void ui_init();

// Use KEY_* codes from <linux/input.h> or KEY_DREAM_* from "minui/minui.h".
int ui_get_key();
int ui_wait_key();                 // waits for a key/button press, returns the code
void ui_wake_key_waiting_thread(); // wakes a thread waiting for a key
int ui_key_pressed(int key);       // returns >0 if the code is currently pressed
int ui_text_visible();             // returns >0 if text log is currently visible
int ui_text_ever_visible();        // returns >0 if text log was ever visible
void ui_show_text(int visible);
void ui_clear_key_queue();

// Write a message to the on-screen log shown with Alt-L (also to stderr).
// The screen is small, and users may need to report these messages to support,
// so keep the output short and not too cryptic.
void ui_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ui_print_raw(const char *buf);

// Display some header text followed by a menu of items, which appears
// at the top of the screen (in place of any scrolling ui_print()
// output, if necessary).
void ui_start_menu(const char** headers, const char** items, int title_length, int start_sel);
// Set the menu highlight to the given index, and return it (capped to
// the range [0..numitems).
int ui_menu_select(int sel);
// End menu mode, resetting the text overlay so that ui_print()
// statements will be displayed.
void ui_end_menu();
//Resets the menu
void ui_reset_menu();

// Set the icon (normally the only thing visible besides the progress bar).
enum {
  BACKGROUND_ICON_NONE,
  BACKGROUND_ICON_INSTALLING,
  BACKGROUND_ICON_ERROR,
  NUM_BACKGROUND_ICONS
};
void ui_set_background(int icon);

// Set view mode
enum {
	VIEWMODE_NORMAL,
	VIEWMODE_CONSOLE,
	VIEWMODE_TEXT_INPUT,
	NUM_VIEWMODES
};

void ui_set_view_mode(int mode);
int ui_get_view_mode();

// Show a progress bar and define the scope of the next operation:
//   portion - fraction of the progress bar the next operation will use
//   seconds - expected time interval (progress bar moves at this minimum rate)
void ui_show_progress(float portion, int seconds);
void ui_set_progress(float fraction);  // 0.0 - 1.0 within the defined scope

// Default allocation of progress bar segments to operations
static const int VERIFICATION_PROGRESS_TIME = 60;
static const float VERIFICATION_PROGRESS_FRACTION = 0.25;
static const float DEFAULT_FILES_PROGRESS_FRACTION = 0.4;
static const float DEFAULT_IMAGE_PROGRESS_FRACTION = 0.1;

// Show a rotating "barberpole" for ongoing operations.  Updates automatically.
void ui_show_indeterminate_progress();

// Hide and reset the progress bar.
void ui_reset_progress();

//returns the number of columns for menu / script
int ui_get_num_columns();

//led functions
void ui_led_toggle(int state);
void ui_led_blink(int once);

//interactive user input
void ui_user_input(const char* header, char* reply);

//updating
extern pthread_mutex_t ui_update_mutex;

void update_screen_locked();
void update_progress_locked();

//battery
void ui_set_battery_data(int battery_charge, int battery_charging);

//cancel key
void ui_cancel_wait_key();

//screen on/off
void ui_screen_on();
void ui_screen_off();

#endif //UI_H