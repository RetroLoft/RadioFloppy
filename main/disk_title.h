/*
 * Disk titles from client file names (plain C, also built in the host tests).
 */
#pragma once

#include <stddef.h>

#define DISK_TITLE_MAX      52      /* characters */
#define DISK_TITLE_SIZE     (DISK_TITLE_MAX + 1)

/*
 * Title from a file name: base name without extension; TOSEC tags such as
 * "(1987)(Sierra)[codes on disk]" are dropped, except "(Disk 1 of 2)";
 * printable ASCII only; at most DISK_TITLE_MAX characters.
 */
void disk_title_from_filename(const char *filename, char out[DISK_TITLE_SIZE]);
