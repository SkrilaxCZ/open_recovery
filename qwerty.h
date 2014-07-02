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
 
#ifndef QWERTY_H_
#define QWERTY_H_

/* Pseudo keys */
#define CHAR_ERROR           0
#define CHAR_NOTHING         255
#define CHAR_SCROLL_DOWN     254
#define CHAR_SCROLL_UP       253
#define CHAR_BIG_SCROLL_DOWN 252
#define CHAR_BIG_SCROLL_UP   251

#define CHAR_KEY_UP          250
#define CHAR_KEY_LEFT        249
#define CHAR_KEY_RIGHT       248
#define CHAR_KEY_DOWN        247

#define CHAR_KEY_CAPSLOCK    246
#define CHAR_KEY_ALTLOCK     245

#define CHAR_KEY_ESCAPE      244

#define CHAR_SPECIAL_KEY(k)  (k >= CHAR_KEY_ESCAPE)

/* Menu navigation */
#define NO_ACTION           -1

#define HIGHLIGHT_UP        -2
#define HIGHLIGHT_DOWN      -3
#define SELECT_ITEM         -4

typedef struct
{
	char normal[KEY_MAX+1];
	char shifted[KEY_MAX+1];
	char alternate[KEY_MAX+1];
} keyboard_layout;

extern keyboard_layout qwerty_layout;

int get_capslock_state();
void toggle_capslock_state();

int get_altlock_state();
void toggle_altlock_state();

void init_keypad_layout();
char resolve_keypad_character(int keycode, int shift, int alt);

int menu_handle_key(int key_code, int visible);

#endif //!QWERTY_H_
