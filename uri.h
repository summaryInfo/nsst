#ifndef URI_H_
#define URI_H_ 1

#include <stdint.h>

enum uri_match_result {
    urim_may_finish,
    urim_finished,
    urim_error,
    urim_need_more
};

struct uri_match_state {
    // TODO
};

#define EMPTY_URI 0

uint32_t uri_add(const char *uri, const char *id);
void uri_ref(uint32_t uri);
void uri_unref(uint32_t uri);
void uri_open(uint32_t uri);
const char *uri_get(uint32_t uri);

enum uri_match_result uri_match_next(struct uri_match_state *state, char ch);
void uri_match_reset(struct uri_match_state *state);

#endif

