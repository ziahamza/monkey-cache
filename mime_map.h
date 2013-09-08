#ifndef __MIME_MAP_H_
#define __MIME_MAP_H_

#define MIME_MAX_LEN 128

#include "MKPlugin.h"

struct mime_map_t {
    // file extention
    char ext[MIME_MAX_LEN];

    // actual mime
    char mime[MIME_MAX_LEN];
};

struct buf_t mime_map;

// inittialize the mime_map with MIMETYPES section from config
void mime_map_init(struct mk_config *cnf);

mk_pointer mime_map_get(char *path);


#endif
