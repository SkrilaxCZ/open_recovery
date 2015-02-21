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
 
#ifndef CONSOLE_H_
#define CONSOLE_H_

#define CONSOLE_FORCE_QUIT    -55
#define CONSOLE_FAILED_START  -56

#include "ui.h"

extern color console_header_color;
extern color console_background_color;
extern color console_front_color;

extern color console_term_colors[16];

//runs the console in the current thread
int run_console();

//shows the console
void ui_console_begin();

//ends the console
void ui_console_end();

//returns the number of rows on the console
int ui_console_get_num_rows();

//returns the number of columns on the console
int ui_console_get_num_columns();

//returns console width in pixels
int ui_console_get_width();

//returns console height in pixels
int ui_console_get_height();

//scrolls up in the console
void ui_console_scroll_up(int num_rows);

//scrolls down in the console
void ui_console_scroll_down(int num_rows);

enum {
  CONSOLE_HEADER_COLOR,
  CONSOLE_DEFAULT_BACKGROUND_COLOR,
  CONSOLE_DEFAULT_FRONT_COLOR  
};

//get system console color
void ui_console_get_system_front_color(int which, unsigned char* r, unsigned char* g, unsigned char* b);

//set system console color
void ui_console_set_system_front_color(int which);

//gets the current console color
void ui_console_get_front_color(unsigned char* r, unsigned char* g, unsigned char* b);

//sets the current console color
void ui_console_set_front_color(unsigned char r, unsigned char g, unsigned char b);

//prints to console
void ui_console_print(const char *text);

#endif //!CONSOLE_H_
