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
 
#ifndef INTERACTIVE_H_
#define INTERACTIVE_H_

#define INTERACTIVE_SHM       "/tmp/interactive"

#define INTERACTIVE_TRIGGER_MENU  1
#define INTERACTIVE_TRIGGER_TEXT  2

typedef struct
{
	int in_trigger;
	int out_trigger;
	char header[50];
	char items[20][50];
	char reply[50];
} interactive_struct;

#endif //!INTERACTIVE_H_
