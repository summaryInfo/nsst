
#include <stdlib.h>

#include "term.h"
#include "window.h"

struct nss_term {
    int16_t cursor_x;
    int16_t cursor_y;

    uint32_t *ch_attr;
    uint32_t *ch_symb;
    nss_text_attrib_t *attr;
    nss_rect_t clip;
};

nss_term_t *nss_term_create(void){
    nss_term_t *term = malloc(sizeof(nss_term_t));

    term->cursor_x = 15;
    term->cursor_y = 4;
    term->clip = (nss_rect_t){0,0,127-33,5};
    term->ch_attr = calloc(term->clip.width*term->clip.height,sizeof(uint32_t));
    term->ch_symb = calloc(term->clip.width*term->clip.height,sizeof(uint32_t));
    term->attr = malloc(5*sizeof(nss_text_attrib_t));

    term->attr[1-1] = (nss_text_attrib_t){ .fg = 0xffffffff, .bg = 0xff005500, .flags = nss_attrib_italic | nss_attrib_bold };
    term->attr[2-1] = (nss_text_attrib_t){ .fg = 0xffffffff, .bg = 0xff000000, .flags = nss_attrib_italic | nss_attrib_underlined };
    term->attr[3-1] = (nss_text_attrib_t){ .fg = 0xffffffff, .bg = 0xff000000, .flags = nss_attrib_strikethrough };
    term->attr[4-1] = (nss_text_attrib_t){ .fg = 0xffffffff, .bg = 0xff000000, .flags = nss_attrib_underlined | nss_attrib_inverse };
    term->attr[5-1] = (nss_text_attrib_t){ .fg = 0xffffffff, .bg = 0xff000000, .flags = 0 };

    for(size_t k = 0; k < 5; k++)
        for(size_t i = 33; i < 127; i++){
            term->ch_attr[k*term->clip.width+(i-33)] = k;
            term->ch_symb[k*term->clip.width+(i-33)] = i;
        }

    term->ch_symb[2*term->clip.width+17] = L'';

	return term;
}

void nss_term_draw_damage(nss_context_t *con, nss_window_t *win, nss_term_t *term, nss_rect_t damage){
    //TODO: Better handle groups of same attrib
    //      Preprocess region
    //           Group lines
    //           Group same font foreground
    //           Group background squares

    // Separate array of chars and attribute indeces

    // Write something like nss_win_multi_draw_text
    //    - Gets a set of pattern strings with some attributes 
    //         nss_text_attrib_t *attr;
    //         size_t length;
    //         uint32_t *string;

    
    if(intersect_with(&damage,&term->clip)){
        for(size_t j = damage.y; j < damage.y + damage.height; j++){
            size_t index, count = 0;
            for(size_t i = damage.x; i <= damage.x + damage.width; i++){
                index = j*term->clip.width+i;
                if((i > damage.x && term->ch_attr[index-1] != term->ch_attr[index]) || 
                    i == damage.x + damage.width){
                    nss_text_attrib_t cattr = term->attr[term->ch_attr[index - count]];
                    nss_win_render_ucs4(con, win, count, &term->ch_symb[index - count], &cattr, i-count, j);
					count = 0;
                }
                else count++;
            }
        }
    }
}
void nss_term_get_cursor(nss_term_t *term, int16_t *cursor_x, int16_t *cursor_y){
    if(cursor_x) *cursor_x = term->cursor_x;
    if(cursor_y) *cursor_y = term->cursor_y;
}

void nss_term_free(nss_term_t *term){
    free(term->ch_attr);
    free(term->ch_symb);
    free(term->attr);
    free(term);
}

