/* Copyright (c) 2019-2022, Evgeniy Baskov. All rights reserved */

#ifndef URI_H_
#define URI_H_ 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

enum uri_match_result {
    urim_ground,
    urim_need_more,
    urim_may_finish,
    urim_finished,
};

enum uri_match_state1 {
    uris1_ground,
    uris1_colon,
    uris1_slash1,
    uris1_slash2,
    uris1_user,
    uris1_host,
    uris1_port,
    uris1_path,
    uris1_query,
    uris1_fragment,
    uris1_p_hex1,
    uris1_p_hex2,
    uris1_filename
};

struct uri_match_state {
    enum uri_match_state1 state;
    enum uri_match_state1 saved;
    enum uri_match_result res;
    bool matched_file_proto;
    size_t size;
    size_t caps;
    char *data;
    bool no_copy;
};

#define EMPTY_URI 0
#define MAX_PROTOCOL_LEN 16

uint32_t uri_add(const char *uri, const char *id);
void uri_ref(uint32_t uri);
void uri_unref(uint32_t uri);
void uri_open(const char *opencmd, uint32_t uri);
const char *uri_get(uint32_t uri);

const uint8_t *match_reverse_proto_tree(struct uri_match_state *stt, const uint8_t *str, ssize_t len);
enum uri_match_result uri_match_next_from_colon(struct uri_match_state *state, uint8_t ch);
void uri_match_reset(struct uri_match_state *state, bool soft);
char *uri_match_get(struct uri_match_state *state);
void uri_release_memory(void);

void init_proto_tree(void);

#endif
