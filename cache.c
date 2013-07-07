/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <netdb.h>
#include <fcntl.h>

#include "MKPlugin.h"

MONKEY_PLUGIN("cache",         /* shortname */
              "Monkey Cache", /* name */
              VERSION,        /* version */
              MK_PLUGIN_STAGE_30); /* hooks */


const mk_pointer mk_default_mime = mk_pointer_init("text/plain\r\n");

int _mkp_init(struct plugin_api **api, char *confdir)
{
    (void) confdir;

    mk_api = *api;

    return 0;
}

void _mkp_exit()
{
}


int _mkp_stage_30(struct plugin *plugin, struct client_session *cs,
                  struct session_request *sr)
{

    (void) plugin;

    if (sr->file_info.size < 0 || sr->file_info.is_file == MK_FALSE || sr->file_info.read_access == MK_FALSE) {
        mk_info("not a file, passing on the file :)");
        return MK_PLUGIN_RET_NOT_ME;
    }

    mk_info("cache plugin taking over the request :)");

    mk_api->header_set_http_status(sr, MK_HTTP_OK);

    sr->headers.content_length = sr->file_info.size;
    sr->headers.real_length = sr->file_info.size;

    sr->fd_file = open(sr->real_path.data, sr->file_info.flags_read_only);
    if (sr->fd_file == -1) {
        return MK_PLUGIN_RET_NOT_ME;
    }
    sr->bytes_to_send = sr->file_info.size;

    sr->headers.content_type = mk_default_mime;

    mk_api->header_send(cs->socket, cs, sr);

    mk_api->socket_send_file(cs->socket, sr->fd_file,
                                 &sr->bytes_offset, sr->bytes_to_send);

    return MK_PLUGIN_RET_CONTINUE;
}
