#include "mime_map.h"
#include "utils.h"

void mime_map_init(struct mk_config *cnf) {
    buf_init(&mime_map, sizeof(struct mime_map_t), 16);

    struct mk_config_section *section = mk_api->config_section_get(cnf, "MIMETYPES");
    struct mk_list *mime_head;
    struct mk_config_entry *entry;

    struct mime_map_t tmp;
    mk_list_foreach(mime_head, &section->entries) {
        entry = mk_list_entry(mime_head, struct mk_config_entry, _head);

        strncpy(tmp.ext, entry->key, MIME_MAX_LEN);
        snprintf(tmp.mime, MIME_MAX_LEN, "%s\r\n", entry->val);
        buf_push(&mime_map, &tmp);
    }

}

mk_pointer mime_map_get(char *path) {
    const mk_pointer mime = mk_pointer_init("text/plain\r\n");
    int i, j;

    int len = strlen(path);
    for (j = len; j > 0; j--) {
        if (path[j - 1] != '.')
            continue;

        for (i = 0; i < mime_map.size; i++) {
            struct mime_map_t *m = buf_get(&mime_map, i);
            if (strcmp(path + j, m->ext) == 0) {
                mk_pointer tmp = {
                    .data = m->mime,
                    .len = strlen(m->mime)
                };

                return tmp;
            }
        }
        break;
    }

    return mime;
}
