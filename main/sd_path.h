/*
 * Paths on the SD card from API requests, plain C (host tests too).
 *
 * A client never gives a path of its own: it names a folder or file
 * *relative to the root directory* (settings root_dir), every part is
 * checked here, and the firmware joins it below SD_MOUNT/root itself.
 *
 * A name (one folder or file): 1..SD_NAME_MAX printable ASCII characters
 * except \ / : * ? " < > | (not allowed by FAT); not starting with '.'
 * or ' ' (no ".", "..", hidden names), not ending with '.' or ' '.
 * A relative path: names separated by single '/', no '/' at the start or
 * end, at most SD_REL_MAX characters; "" is the root directory itself.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SD_NAME_MAX     120
#define SD_REL_MAX      200

bool sd_name_valid(const char *name);
bool sd_relpath_valid(const char *rel);

/* out = mount [/root] [/rel]; root and rel must be valid. false: too long. */
bool sd_path_join(char *out, size_t size, const char *mount, const char *root, const char *rel);

/* Percent-decode a URL query value in place ('+' is a space). false:
 * malformed or a NUL/control character. */
bool sd_url_decode(char *s);
