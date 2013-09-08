#define _GNU_SOURCE
#include <fcntl.h>

#include "MKPlugin.h"

#include "pipe_buf.h"
#include "cache_file.h"
#include "socket.h"

int socket_seve_req_fd(struct cache_req_t *req) {
    long nbytes = mk_api->socket_send_file(req->socket, req->file->fd,
                                 &req->bytes_offset, req->bytes_to_send);

    if (nbytes > 0) {
        req->bytes_to_send -= nbytes;
    }

    if (nbytes < 0) {
        perror("cannto send file!");

        return -1;
    }

    return req->bytes_to_send;
}

int socket_serve_req_splice(struct cache_req_t *req) {
    struct cache_file_t *file = req->file;
    long pbytes;

    // loop until the current file pointer is a the end or tmp buffer
    // still has data
    while (req->bytes_to_send > 0 && (req->curr || req->buf->filled)) {
        // until range requests are not supported, this is a bug
        fflush(stdout);
        mk_bug(req->bytes_offset + req->bytes_to_send != file->size);

        // fill in buf pipe with req data
        // if buffer is empty and cache is all set, fill it with the cache
        if (req->curr && cache_req_fill_curr(req) == 0 && !req->buf->filled) {
            // fill curr into buf
            pbytes = tee(req->curr->pipe[0], req->buf->pipe[1],
                req->curr->filled, SPLICE_F_NONBLOCK);

            if (pbytes < 0) {
                if (errno == EAGAIN) {
                    mk_info("Pretty strange, blocking in tee?!");
                    goto SEND_BUFFER;
                }
                else {
                    perror("Cannot tee into tmp buf!!\n");
                    return -1;
                }
            }

            // printf("filled %ld from file cache to req buffer!\n",
            //    pbytes);


            req->buf->filled += pbytes;

            if (&file->cache == req->curr->_head.next) {
                // reached the start so finished with file buffers

                // printf("cache finished, only buffer needs to be "
                //    "flushed (%d bytes)!\n", req->buf->filled);
                req->curr =  NULL;
            }
            else {
                req->curr = mk_list_entry_next(&req->curr->_head,
                  struct pipe_buf_t, _head, &file->cache);
            }

        }

    SEND_BUFFER:
        if (!req->buf->filled) {
            // nothing to do, just get out of the loop
            // mk_info("surprisingly no data to send over to the socket!\n");
            break;
        }

        // finally splicing data to the socket
        pbytes = splice(req->buf->pipe[0], NULL, req->socket, NULL,
            req->buf->filled, SPLICE_F_NONBLOCK | SPLICE_F_MOVE);
        if (pbytes < 0) {
            if (errno == EAGAIN) {
                // blocking socket, lets try in the next cycle
                // printf("socket blocking, trying again in the next cycle!\n");
                break;
            }

            perror("Cannot splice data to the socket!\n");
            return -1;
        }

        req->buf->filled -= pbytes;
        req->bytes_to_send -= pbytes;
        req->bytes_offset += pbytes;

        // printf("writing %ld bytes of data over the socket!\n", pbytes);

        mk_bug(req->bytes_to_send < 0);

        if (req->buf->filled) {
            //mk_info("not all data was sent, i can tell that its gonna "
            //    "block next time ;)");
        }
    }

    // printf("Ending cycle, bytes left: %ld\n\n", req->bytes_to_send);
    return req->bytes_to_send;
}


