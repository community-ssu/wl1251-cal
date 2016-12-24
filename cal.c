/*
    0xFFFF - Open Free Fiasco Firmware Flasher
    Copyright (c) 2011  Michael Buesch <mb@bu3sch.de>
    Copyright (C) 2012  Pali Rohár <pali.rohar@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 2 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

/* This is simple CAL parser form Calvaria */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <mtd/mtd-user.h>
#include <sys/sysmacros.h>
#endif

#include "cal.h"

#define MAX_SIZE	393216
#define INDEX_LAST	(0xFF + 1)
#define HDR_MAGIC	"ConF"

struct cal {
	ssize_t size;
	void * mem;
};

struct header {
	char magic[4];		/* Magic sequence */
	uint8_t type;		/* Type number */
	uint8_t index;		/* Index number */
	uint16_t flags;		/* Flags */
	char name[16];		/* Human readable section name */
	uint32_t length;	/* Payload length */
	uint32_t datasum;	/* Data CRC32 checksum */
	uint32_t hdrsum;	/* Header CRC32 checksum */
} __attribute__((__packed__));


int cal_init_file(const char * file, struct cal ** cal_out) {

	int fd = -1;
	uint64_t blksize = 0;
	ssize_t size = 0;
	void * mem = NULL;
	struct cal * cal = NULL;
	struct stat st;
#ifdef __linux__
	mtd_info_t mtd_info;
#else
	off_t lsize = 0;
#endif

	fd = open(file, O_RDONLY);

	if ( fd < 0 )
		return -1;

	if ( fstat(fd, &st) != 0 )
		goto err;

	if ( S_ISREG(st.st_mode) )
		size = st.st_size;
	else if ( S_ISBLK(st.st_mode) ) {
#ifdef __linux__
		if ( ioctl(fd, BLKGETSIZE64, &blksize) != 0 )
			goto err;
#else
		lsize = lseek(fd, 0, SEEK_END);
		if ( lsize == (off_t)-1 )
			goto err;
		if ( lseek(fd, 0, SEEK_SET) == (off_t)-1 )
			goto err;
		blksize = lsize;
#endif
#ifdef SSIZE_MAX
		if ( blksize > SSIZE_MAX )
			goto err;
#endif
		size = blksize;
	} else {
#ifdef __linux__
		if ( S_ISCHR(st.st_mode) && major(st.st_rdev) == 90 ) {
			if ( ioctl(fd, MEMGETINFO, &mtd_info) != 0 )
				goto err;
			size = mtd_info.size;
		} else {
			goto err;
		}
#else
		goto err;
#endif
	}

	if ( size == 0 || size > MAX_SIZE )
		goto err;

	mem = malloc(size);

	if ( ! mem )
		goto err;

	if ( read(fd, mem, size) != size )
		goto err;

	cal = malloc(sizeof(struct cal));

	if ( ! cal )
		goto err;

	cal->mem = mem;
	cal->size = size;

	close(fd);

	*cal_out = cal;
	return 0;

err:
	close(fd);
	free(mem);
	free(cal);
	return -1;

}

int cal_init(struct cal ** cal_out) {

	return cal_init_file("/dev/mtd1ro", cal_out);

}

void cal_finish(struct cal * cal) {

	if ( cal ) {
		free(cal->mem);
		free(cal);
	}

}

static uint32_t crc32(uint32_t crc, const void * _data, size_t size) {

	const uint8_t * data = _data;
	uint8_t value;
	unsigned int bit;
	size_t i;
	const uint32_t poly = 0xEDB88320;

	for ( i = 0; i < size; i++ ) {
		value = data[i];
		for ( bit = 8; bit; bit-- ) {
			if ( (crc & 1) != (value & 1) )
				crc = (crc >> 1) ^ poly;
			else
				crc >>= 1;
			value >>= 1;
		}
	}

	return crc;

}

static int is_header(void *data, size_t size) {

	struct header * hdr = data;

	if ( size < sizeof(struct header) )
		return 0;

	if ( memcmp(hdr->magic, HDR_MAGIC, sizeof(hdr->magic)) != 0 )
		return 0;

	return 1;

}

static int64_t find_section(void *start, uint64_t count, int want_index, const char *want_name) {

	int64_t offset = 0, found_offset = -1;
	uint8_t * data = start;
	struct header *hdr;
	char sectname[sizeof(hdr->name) + 1] = { 0, };
	uint32_t payload_len;
	int previous_index = -1;

	while ( 1 ) {

		/* Find header start */
		if ( count < sizeof(struct header) )
			break;

		if ( ! is_header(data + offset, count) ) {
			count--;
			offset++;
			continue;
		}

		hdr = (struct header *)(data + offset);
		payload_len = hdr->length;

		if ( count - sizeof(struct header) < payload_len )
			return -1;

		memcpy(sectname, hdr->name, sizeof(hdr->name));

		if ( want_index == INDEX_LAST ) {
			if ( (int)hdr->index <= previous_index )
				goto next;
		} else {
			if ( want_index >= 0 && want_index != hdr->index )
				goto next;
		}

		if ( want_name && strcmp(sectname, want_name) != 0 )
			goto next;

		/* Found it */
		found_offset = offset;
		if ( want_index == INDEX_LAST )
			previous_index = hdr->index;
		else
			break;

next:
		count -= sizeof(struct header) + payload_len;
		offset += sizeof(struct header) + payload_len;

	}

	return found_offset;

}

int cal_read_block(struct cal * cal, const char * name, void ** ptr, unsigned long * len, unsigned long flags) {

	int64_t find_offset;
	uint64_t filelen = cal->size;
	uint8_t * data = cal->mem;
	struct header * hdr;
	void * offset;

	find_offset = find_section(data, filelen, INDEX_LAST, name);
	if ( find_offset < 0 )
		return -1;

	hdr = (struct header *)(data + find_offset);

	if ( flags && hdr->flags != flags )
		return -1;

	if ( crc32(0, hdr, sizeof(*hdr) - 4) != hdr->hdrsum )
		return -1;

	offset = data + find_offset + sizeof(struct header);

	if ( crc32(0, offset, hdr->length) != hdr->datasum )
		return -1;

	*ptr = malloc(hdr->length);
	if (!*ptr)
		return -1;

	memcpy(*ptr, offset, hdr->length);
	*len = hdr->length;

	return 0;

}
