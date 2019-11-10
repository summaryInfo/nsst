#ifndef NSS_INPUT_H_
#define NSS_INPUT_H_

#include <xkbcommon/xkbcommon-keysyms.h>
#include <xcb/xcb.h>

#define NSS_M_ALL (0xff)
#define NSS_M_TERM (XCB_MOD_MASK_CONTROL | XCB_MOD_MASK_SHIFT)

#define NSS_M_NOAPPCUR (1 << 0)
#define NSS_M_APPCUR (1 << 1)
#define NSS_M_NOAPPK (1 << 2)
#define NSS_M_NUM (1 << 3)
#define NSS_M_APPK (1 << 4)
#define NSS_M_BSDEL (1 << 5)
#define NSS_M_NOBSDEL (1 << 6)
#define NSS_M_DELDEL (1 << 7)
#define NSS_M_NODELDEL (1 << 8)

enum nss_shortcut_action {
    nss_sa_none,
	nss_sa_break,
	nss_sa_reverse,
	nss_sa_numlock,
	nss_sa_scroll_up,
	nss_sa_scroll_down,
	nss_sa_font_up,
	nss_sa_font_down,
	nss_sa_font_default,
	nss_sa_font_subpixel,
	nss_sa_new_window,
};

typedef struct nss_shortcut {
    uint32_t ksym;
    uint32_t mmask;
    uint32_t mstate;
    enum nss_shortcut_action action;
} nss_shortcut_t;

typedef struct nss_ckey_key {
    uint32_t mmask;
    uint32_t mstate;
    const char *string;
    uint32_t flag;
} nss_ckey_key_t;

typedef struct nss_ckey {
    uint32_t ksym;
    nss_ckey_key_t *inst;
} nss_ckey_t;

nss_shortcut_t cshorts[] = {
    {XKB_KEY_Up, NSS_M_ALL, NSS_M_TERM, nss_sa_scroll_down},
    {XKB_KEY_Down, NSS_M_ALL, NSS_M_TERM, nss_sa_scroll_up},
    {XKB_KEY_Page_Up, NSS_M_ALL, NSS_M_TERM, nss_sa_font_up},
    {XKB_KEY_Page_Down, NSS_M_ALL, NSS_M_TERM, nss_sa_font_down},
    {XKB_KEY_Home, NSS_M_ALL, NSS_M_TERM, nss_sa_font_default},
    {XKB_KEY_End, NSS_M_ALL, NSS_M_TERM, nss_sa_font_subpixel},
    {XKB_KEY_N, NSS_M_ALL, NSS_M_TERM, nss_sa_new_window},
    {XKB_KEY_R, NSS_M_ALL, NSS_M_TERM, nss_sa_reverse},
    {XKB_KEY_Num_Lock, NSS_M_ALL, NSS_M_TERM, nss_sa_numlock},
    {XKB_KEY_Break, 0, 0, nss_sa_break},
};

nss_ckey_t ckeys[] = {
    {XKB_KEY_KP_Home,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2332J",NSS_M_NOAPPCUR},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2H",NSS_M_APPCUR},
        {0,0,"\233H",NSS_M_NOAPPCUR},
        {0,0,"\2331~",NSS_M_APPCUR},
        {0}}},
    {XKB_KEY_KP_Up,(nss_ckey_key_t[]){
        {0,0,"\217x",NSS_M_APPK},
        {0,0,"\233A",NSS_M_NOAPPCUR},
        {0,0,"\217A",NSS_M_APPCUR},
        {0}}},
    {XKB_KEY_KP_Down,(nss_ckey_key_t[]){
        {0,0,"\217r",NSS_M_APPK},
        {0,0,"\233B",NSS_M_NOAPPCUR},
        {0,0,"\217B",NSS_M_APPCUR},
        {0}}},
    {XKB_KEY_KP_Left,(nss_ckey_key_t[]){
        {0,0,"\217t",NSS_M_APPK},
        {0,0,"\233D",NSS_M_NOAPPCUR},
        {0,0,"\217D",NSS_M_APPCUR},
        {0}}},
    {XKB_KEY_KP_Right,(nss_ckey_key_t[]){
        {0,0,"\217v",NSS_M_APPK},
        {0,0,"\233C",NSS_M_NOAPPCUR},
        {0,0,"\217C",NSS_M_APPCUR},
        {0}}},
    {XKB_KEY_KP_Prior,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2335;2~",0},
        {0,0,"\2335~",0},
        {0}}},
    {XKB_KEY_KP_Begin,(nss_ckey_key_t[]){
        {0,0,"\233E",0},
        {0}}},
    {XKB_KEY_KP_End,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\233J",NSS_M_NOAPPK},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2331;5F",NSS_M_APPK},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\233K",NSS_M_NOAPPK},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2F",NSS_M_APPK},
        {0,0,"\2334~",0},
        {0}}},
    {XKB_KEY_KP_Next,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2336;2~",0},
        {0,0,"\2336~",0},
        {0}}},
    {XKB_KEY_KP_Insert,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2332;2~",NSS_M_APPK},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2334l",NSS_M_NOAPPK},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\233L",NSS_M_NOAPPK},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2332;5~",NSS_M_APPK},
        {0,0,"\2334h",NSS_M_NOAPPK},
        {0,0,"\2332~",NSS_M_APPK},
        {0}}},
    {XKB_KEY_KP_Delete,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\233M",NSS_M_NOAPPK | NSS_M_NODELDEL},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2333;5~",NSS_M_APPK | NSS_M_NODELDEL},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2332K",NSS_M_NOAPPK | NSS_M_NODELDEL},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2333;2~",NSS_M_APPK | NSS_M_NODELDEL},
        {0,0,"\233P",NSS_M_NOAPPK | NSS_M_NODELDEL},
        {0,0,"\2333~",NSS_M_APPK | NSS_M_NODELDEL},
        {NSS_M_ALL,0,"\177", NSS_M_DELDEL},
        {NSS_M_ALL,XCB_MOD_MASK_1, "\377", NSS_M_DELDEL},
        {0}}},
    {XKB_KEY_KP_Multiply,(nss_ckey_key_t[]){
        {0,0,"\217j",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_Add,(nss_ckey_key_t[]){
        {0,0,"\217k",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_Enter,(nss_ckey_key_t[]){
        {0,0,"\217M",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_Enter,(nss_ckey_key_t[]){
        {0,0,"\r",NSS_M_NOAPPK},
        {0}}},
    {XKB_KEY_KP_Subtract,(nss_ckey_key_t[]){
        {0,0,"\217m",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_Decimal,(nss_ckey_key_t[]){
        {0,0,"\217n",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_Divide,(nss_ckey_key_t[]){
        {0,0,"\217o",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_0,(nss_ckey_key_t[]){
        {0,0,"\217p",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_1,(nss_ckey_key_t[]){
        {0,0,"\217q",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_2,(nss_ckey_key_t[]){
        {0,0,"\217r",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_3,(nss_ckey_key_t[]){
        {0,0,"\217s",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_4,(nss_ckey_key_t[]){
        {0,0,"\217t",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_5,(nss_ckey_key_t[]){
        {0,0,"\217u",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_6,(nss_ckey_key_t[]){
        {0,0,"\217v",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_7,(nss_ckey_key_t[]){
        {0,0,"\217w",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_8,(nss_ckey_key_t[]){
        {0,0,"\217x",NSS_M_NUM},
        {0}}},
    {XKB_KEY_KP_9,(nss_ckey_key_t[]){
        {0,0,"\217y",NSS_M_NUM},
        {0}}},
    {XKB_KEY_Up,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2A",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\2331;3A",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_1,"\2331;4A",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2331;5A",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_CONTROL,"\2331;6A",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL|XCB_MOD_MASK_1,"\2331;7A",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_CONTROL|XCB_MOD_MASK_1,"\2331;8A",0},
        {0,0,"\233A",NSS_M_NOAPPCUR},
        {0,0,"\217A",NSS_M_APPCUR},
        {0}}},
    {XKB_KEY_Down,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2B",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\2331;3B",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_1,"\2331;4B",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2331;5B", 0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_CONTROL,"\2331;6B",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL|XCB_MOD_MASK_1,"\2331;7B",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_CONTROL|XCB_MOD_MASK_1,"\2331;8B",0},
        {0,0,"\233B",NSS_M_NOAPPCUR},
        {0,0,"\217B",NSS_M_APPCUR},
        {0}}},
    {XKB_KEY_Left,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2D",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\2331;3D",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_1,"\2331;4D",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2331;5D", 0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_CONTROL,"\2331;6D",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL|XCB_MOD_MASK_1,"\2331;7D",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_CONTROL|XCB_MOD_MASK_1,"\2331;8D",0},
        {0,0,"\233D",NSS_M_NOAPPCUR},
        {0,0,"\217D",NSS_M_APPCUR},
        {0}}},
    {XKB_KEY_Right,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2C",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\2331;3C",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_1,"\2331;4C",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2331;5C", 0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_CONTROL,"\2331;6C",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL|XCB_MOD_MASK_1,"\2331;7C",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT|XCB_MOD_MASK_CONTROL|XCB_MOD_MASK_1,"\2331;8C",0},
        {0,0,"\233C",NSS_M_NOAPPCUR},
        {0,0,"\217C",NSS_M_APPCUR},
        {0}}},
    {XKB_KEY_ISO_Left_Tab,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\233Z",0},
        {0}}},
    {XKB_KEY_Return,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_1,"\215",0},
        {0,0,"\r",0},
        {0}}},
    {XKB_KEY_Insert,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2334l",NSS_M_NOAPPK},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2332;2~",NSS_M_APPK},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\233L",NSS_M_NOAPPK},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2332;5~",NSS_M_APPK},
        {0,0,"\2334h",NSS_M_NOAPPK},
        {0,0,"\2332~",NSS_M_APPK},
        {0}}},
    {XKB_KEY_Delete,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\233M",NSS_M_NOAPPK | NSS_M_NODELDEL},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2333;5~",NSS_M_APPK | NSS_M_NODELDEL},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2332K",NSS_M_NOAPPK | NSS_M_NODELDEL},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2333;2~",NSS_M_APPK | NSS_M_NODELDEL},
        {0,0,"\233P",NSS_M_NOAPPK | NSS_M_NODELDEL},
        {0,0,"\2333~",NSS_M_APPK | NSS_M_NODELDEL},
        {NSS_M_ALL,0,"\177", NSS_M_DELDEL},
        {NSS_M_ALL,XCB_MOD_MASK_1, "\377", NSS_M_DELDEL},
        {0}}},
    {XKB_KEY_BackSpace,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\177", NSS_M_BSDEL},
        {NSS_M_ALL,XCB_MOD_MASK_1, "\377", NSS_M_BSDEL},
        {NSS_M_ALL,0,"\010", NSS_M_NOBSDEL},
        {NSS_M_ALL,XCB_MOD_MASK_1, "\210", NSS_M_NOBSDEL},
        {0}}},
    {XKB_KEY_Home,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2332J",NSS_M_NOAPPCUR},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2H",NSS_M_APPCUR},
        {0,0,"\233H",NSS_M_NOAPPCUR},
        {0,0,"\2331~",NSS_M_APPCUR},
        {0}}},
    {XKB_KEY_End,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\233J",NSS_M_NOAPPK},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2331;5F",NSS_M_APPK},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\233K",NSS_M_NOAPPK},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2F",NSS_M_APPK},
        {0,0,"\2334~",0},
        {0}}},
    {XKB_KEY_Prior,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2335;5~",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2335;2~",0},
        {0,0,"\2335~",0},
        {0}}},
    {XKB_KEY_Next,(nss_ckey_key_t[]){
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2336;5~",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2336;2~",0},
        {0,0,"\2336~",0},
        {0}}},
    {XKB_KEY_F1,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\217P",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2P",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2331;5P",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\2331;6P",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\2331;3P",0},
        {NSS_M_ALL,XCB_MOD_MASK_3,"\2331;4P",0},
        {0}}},
    {XKB_KEY_F2,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\217Q",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2Q",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2331;5Q",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\2331;6Q",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\2331;3Q",0},
        {NSS_M_ALL,XCB_MOD_MASK_3,"\2331;4Q",0},
        {0}}},
    {XKB_KEY_F3,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\217R",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2R",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2331;5R",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\2331;6R",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\2331;3R",0},
        {NSS_M_ALL,XCB_MOD_MASK_3,"\2331;4R",0},
        {0}}},
    {XKB_KEY_F4,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\217S",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\2331;2S",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\2331;5S",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\2331;6S",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\2331;3S",0},
        {0}}},
    {XKB_KEY_F5,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23315~",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\23315;2~",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\23315;5~",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\23315;6~",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\23315;3~",0},
        {0}}},
    {XKB_KEY_F6,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23317~",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\23317;2~",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\23317;5~",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\23317;6~",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\23317;3~",0},
        {0}}},
    {XKB_KEY_F7,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23318~",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\23318;2~",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\23318;5~",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\23318;6~",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\23318;3~",0},
        {0}}},
    {XKB_KEY_F8,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23319~",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\23319;2~",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\23319;5~",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\23319;6~",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\23319;3~",0},
        {0}}},
    {XKB_KEY_F9,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23320~",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\23320;2~",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\23320;5~",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\23320;6~",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\23320;3~",0},
        {0}}},
    {XKB_KEY_F10,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23321~",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\23321;2~",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\23321;5~",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\23321;6~",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\23321;3~",0},
        {0}}},
    {XKB_KEY_F11,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23323~",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\23323;2~",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\23323;5~",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\23323;6~",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\23323;3~",0},
        {0}}},
    {XKB_KEY_F12,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23324~",0},
        {NSS_M_ALL,XCB_MOD_MASK_SHIFT,"\23324;2~",0},
        {NSS_M_ALL,XCB_MOD_MASK_CONTROL,"\23324;5~",0},
        {NSS_M_ALL,XCB_MOD_MASK_4,"\23324;6~",0},
        {NSS_M_ALL,XCB_MOD_MASK_1,"\23324;3~",0},
        {0}}},
    {XKB_KEY_F13,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\2331;2P",0},
        {0}}},
    {XKB_KEY_F14,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\2331;2Q",0},
        {0}}},
    {XKB_KEY_F15,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\2331;2R",0},
        {0}}},
    {XKB_KEY_F16,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\2331;2S",0},
        {0}}},
    {XKB_KEY_F17,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23315;2~",0},
        {0}}},
    {XKB_KEY_F18,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23317;2~",0},
        {0}}},
    {XKB_KEY_F19,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23318;2~",0},
        {0}}},
    {XKB_KEY_F20,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23319;2~",0},
        {0}}},
    {XKB_KEY_F21,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23320;2~",0},
        {0}}},
    {XKB_KEY_F22,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23321;2~",0},
        {0}}},
    {XKB_KEY_F23,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23323;2~",0},
        {0}}},
    {XKB_KEY_F24,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23324;2~",0},
        {0}}},
    {XKB_KEY_F25,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\2331;5P",0},
        {0}}},
    {XKB_KEY_F26,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\2331;5Q",0},
        {0}}},
    {XKB_KEY_F27,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\2331;5R",0},
        {0}}},
    {XKB_KEY_F28,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\2331;5S",0},
        {0}}},
    {XKB_KEY_F29,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23315;5~",0},
        {0}}},
    {XKB_KEY_F30,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23317;5~",0},
        {0}}},
    {XKB_KEY_F31,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23318;5~",0},
        {0}}},
    {XKB_KEY_F32,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23319;5~",0},
        {0}}},
    {XKB_KEY_F33,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23320;5~",0},
        {0}}},
    {XKB_KEY_F34,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23321;5~",0},
        {0}}},
    {XKB_KEY_F35,(nss_ckey_key_t[]){
        {NSS_M_ALL,0,"\23323;5~",0},
        {0}}}
};

#endif
