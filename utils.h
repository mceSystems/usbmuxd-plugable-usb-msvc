/*
	usbmuxd - iPhone/iPod Touch USB multiplex server daemon

Copyright (C) 2009	Hector Martin "marcan" <hector@marcansoft.com>
Copyright (C) 2009	Nikias Bassen <nikias@gmx.li>

This library is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as
published by the Free Software Foundation, either version 2.1 of the
License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/

#ifndef __UTILS_H__
#define __UTILS_H__

#include <plist/plist.h>

struct collection {
	void **list;
	int capacity;
};

#define IS_VALID_COLLECTION(col) ((NULL != (col)->list) && (0 != (col)->capacity))

void collection_init(struct collection *col);
void collection_add(struct collection *col, void *element);
void collection_remove(struct collection *col, void *element);
int collection_count(struct collection *col);
void collection_free(struct collection *col);
void collection_copy(struct collection *dest, struct collection *src);

#define MERGE_(a,b) a ## _ ## b
#define LABEL_(a,b) MERGE_(a, b)
#define UNIQUE_VAR(a) LABEL_(a, __LINE__)

#define FOREACH(var, col, item_type) \
	do { \
		int UNIQUE_VAR(_iter); \
		for(UNIQUE_VAR(_iter)=0; UNIQUE_VAR(_iter)<(col)->capacity; UNIQUE_VAR(_iter)++) { \
			if(!(col)->list[UNIQUE_VAR(_iter)]) continue; \
			var = (item_type)((col)->list[UNIQUE_VAR(_iter)]);

#define ENDFOREACH \
		} \
	} while(0);

#ifndef HAVE_STPCPY
char *stpcpy(char * s1, const char * s2);
#endif
char *string_concat(const char *str, ...);

void buffer_read_from_filename(const char *filename, char **buffer, uint64_t *length);
void buffer_write_to_filename(const char *filename, const char *buffer, uint64_t length);

enum plist_format_t {
	PLIST_FORMAT_XML,
	PLIST_FORMAT_BINARY
};

int plist_read_from_filename(plist_t *plist, const char *filename);
int plist_write_to_filename(plist_t plist, const char *filename, enum plist_format_t format);

uint64_t mstime64(void);

#endif
