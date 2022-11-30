#!/usr/bin/env python3
# Copyright (c) 2022, Evgeniy Baskov. All rights reserved

# This program requires UnicodeData.txt and EastAsianWidth.txt
# in the working directory and generates iswide.h, wide.h and
# precompose.h. Need to run it on every Unicode update.
# References:
#     https://www.unicode.org/reports/tr44/
#     https://www.unicode.org/Public/UCD/latest/ucd/

import math

UINT8_MAX=0xFF
UINT16_MAX=0xFFFF
UNICODE_MAX=0x110000

COMPACT_TO=0x40000
COMPACT_FROM=0xE0000

LEVEL2_TABLE_BITS=8
LEVEL2_TABLE_ELEMENT_BITS_LOG=5
LEVEL1_ELEMENTS_PER_LINE=16

LEVEL2_TABLE_ELEMENT_BITS=(1<<LEVEL2_TABLE_ELEMENT_BITS_LOG)
LEVEL2_TABLE_WIDTH=(1<<LEVEL2_TABLE_BITS)
LEVEL2_TABLE_ROW_LEN=LEVEL2_TABLE_WIDTH//LEVEL2_TABLE_ELEMENT_BITS

codepoints=[None]*UNICODE_MAX
level2_table=dict()
level1_total_elems=0
precomp_elems=0
empty_lvl2_row=tuple([0]*LEVEL2_TABLE_ROW_LEN)

def C(value):
    if value >= COMPACT_TO:
        assert(value >= COMPACT_FROM)
        value += COMPACT_TO - COMPACT_FROM
    return value
def UC(value):
    if value >= COMPACT_TO:
        value -= COMPACT_TO - COMPACT_FROM
    return value

class CodePoint:
    def __init__(self, value, name, category, decomposition):
        self.value = value
        self.name = name
        self.category = category
        self.decomposition = decomposition
        if value < UNICODE_MAX:
            codepoints[value] = self

def parse():
    with open('UnicodeData.txt') as ud:
        ranged=None
        for line in ud.readlines():
            fields = line.split(';')
            name, value = fields[1], C(int(fields[0], 16))
            if ranged:
                assert(name.endswith('Last>'))
                for i in range(ranged,value+1):
                    CodePoint(i, f'CODE POINT {UC(i):04X}', fields[2], '')
                ranged=None
                continue
            elif name.endswith('First>'):
                ranged=value
                continue
            CodePoint(value, name, fields[2], fields[5].split(' '))
    with open('EastAsianWidth.txt') as eaw:
        for line in eaw.readlines():
            comment = line.find('#')
            if comment >= 0:
                line = line[0:comment]
            line = line.strip()
            if not line:
                continue
            line = line.split(';')
            if line[0].find('..') > 0:
                r = line[0].split('..')
                start = int(r[0], 16)
                end = int(r[1], 16)
            else:
                start = int(line[0], 16)
                end = start
            for i in range(start, end + 1):
                if not codepoints[C(i)]:
                    CodePoint(value=C(i), name=f'UNDEFINED CODE POINT {i:04X}', category='Cn', decomposition='')
                codepoints[C(i)].widthtype = line[1]

def filter_compose():
    precomp=[]
    for cp in codepoints:
        if not cp:
            continue
        dec = [int(i, 16) for i in cp.decomposition if i and not i.startswith('<')]
        if len(dec) != 2:
            continue
        cp2 = codepoints[dec[1]]
        if not cp2 or cp2.category not in ['Mn', 'Mc']:
            continue
        precomp.append((dec[0],dec[1],cp.value))
    precomp.sort()
    return precomp

def compress_table(table):
    global level1_total_elems
    max_nonempty, min_nonempty, level1_table = None, None, []
    for i in range((UNICODE_MAX+LEVEL2_TABLE_WIDTH-1)//LEVEL2_TABLE_WIDTH):
        row = [0]*LEVEL2_TABLE_ROW_LEN
        for k in range(LEVEL2_TABLE_WIDTH):
            if table[i*LEVEL2_TABLE_WIDTH+k]:
                row[k//LEVEL2_TABLE_ELEMENT_BITS] |= 1 << (k & (LEVEL2_TABLE_ELEMENT_BITS - 1))
                if min_nonempty is None:
                    min_nonempty = k+i*LEVEL2_TABLE_WIDTH
        row = tuple(row)
        if row not in level2_table:
            level2_table[row] = len(level2_table)
        level1_table.append(level2_table[row])
        if empty_lvl2_row != row:
            max_nonempty = len(level1_table)
    level1_table=level1_table[:max_nonempty]
    level1_total_elems += len(level1_table)
    return level1_table, min_nonempty

def write_level1_table(f, g, name, table):
    type = '8' if len(level2_table) <= UINT8_MAX else '16'
    f.write(f'const uint{type}_t {name}_table1_[{len(table)}] = {{\n')
    g.write(f'extern const uint{type}_t {name}_table1_[{len(table)}];\n')
    for i in range((len(table)+LEVEL1_ELEMENTS_PER_LINE-1)//LEVEL1_ELEMENTS_PER_LINE):
        f.write('    ')
        for j in range(LEVEL1_ELEMENTS_PER_LINE):
            index=i*LEVEL1_ELEMENTS_PER_LINE+j
            if index >= len(table):
                break
            f.write(f'{table[index]:3},')
        f.write(f' /* 0x{i*LEVEL1_ELEMENTS_PER_LINE*LEVEL2_TABLE_WIDTH:X}-0x{(i+1)*LEVEL1_ELEMENTS_PER_LINE*LEVEL2_TABLE_WIDTH-1:X} */\n')
    f.write('};\n\n')

def write_level2_table(f, g):
    f.write(f"const uint32_t width_data_[{len(level2_table)}][{LEVEL2_TABLE_ROW_LEN}] = {{\n")
    g.write(f"extern const uint32_t width_data_[{len(level2_table)}][{LEVEL2_TABLE_ROW_LEN}];\n")
    for row in level2_table:
        f.write('    { ')
        for elem in row:
            f.write(f'0x{elem:08X}, ')
        f.write(f'}}, /* {level2_table[row]} */\n')
    f.write('};\n\n')

def write_precompose(f, precomp):
    # Generate two tables a smaller one with 16 bit elements
    # and larger one with 32 bit elements.
    global precomp_elems
    precomp_elems = 0
    f.write('#ifndef _PRECOMPOSE_H\n#define _PRECOMPOSE_H 1\n\n'
            '#include <stdint.h>\n\n'
            'static struct pre1_item {\n    uint16_t src, mod, dst;\n} pre1_tab[] = {\n')
    for i in precomp:
        if i[0] <= UINT16_MAX and i[1] <= UINT16_MAX and i[2] <= UINT16_MAX:
            f.write(f'    {{0x{i[0]:04X}, 0x{i[1]:04X}, 0x{i[2]:04X}}},\n')
            precomp_elems += 1
    f.write('};\n\nstatic struct pre2_item {\n    uint32_t src, mod, dst;\n} pre2_tab[] = {\n')
    for i in precomp:
        if i[0] > UINT16_MAX or i[1] > UINT16_MAX or i[2] > UINT16_MAX:
            f.write(f'    {{0x{i[0]:X}, 0x{i[1]:X}, 0x{i[2]:X}}},\n')
            precomp_elems += 2
    f.write('};\n\n\n#endif\n')

def write_predicate(f, min_elem, x):
    f.write(f'\ninline static bool is{x}_compact(uint32_t x) {{\n'
            f'    return x - 0x{min_elem:X}U < 0x{LEVEL2_TABLE_WIDTH:X}*sizeof({x}_table1_)/sizeof({x}_table1_[0]) - 0x{min_elem:X}U &&\n'
            f'           width_data_[{x}_table1_[x >> {LEVEL2_TABLE_BITS}]][(x >> 5) &'
            f' {(LEVEL2_TABLE_WIDTH//LEVEL2_TABLE_ELEMENT_BITS)-1}] & (1U << (x & 0x1F));\n'
            f'}}\n'
            f'\ninline static bool is{x}(uint32_t x) {{\n'
            f'    return is{x}_compact(compact(x));\n'
            f'}}\n\n')

def set_predicate(table, fun):
    for cp in codepoints:
        if not cp:
            continue
        if fun(cp):
            table[cp.value] = True

def set_ranges(table, value, *ranges):
    for r in ranges:
        if type(r) is int:
            table[r] = value
        else:
            for k in range(r[0],r[1]+1):
                table[k] = value

def mk_wide():
    table=[False]*UNICODE_MAX
    set_predicate(table, lambda cp: cp.widthtype == 'W'
                                and cp.category not in ['Cn', 'Mn'])
    return table

def mk_combining():
    table=[False]*UNICODE_MAX
    set_ranges(table, True, [0x1160, 0x11FF], [0xD7B0, 0xD7C6], [0xD7CB,0xD7FB]) # Hangul
    set_predicate(table, lambda cp: cp.category in ['Me', 'Mn', 'Cf'])
    return table

def mk_ambiguous():
    # FIXME: This suggests to mark every character, that is not wide as ambiguous,
    # not sure if it should be like this...
    #     uniset +0000..DFFF -4e00..9fd5 +F900..10FFFD unknown +2028..2029 c
    #set_ranges(table, [0, 0x4DFF], [0x9FD6, 0xDFFF], [0xF900, 0x10FFFD], [0x2028, 0x2029])
    #for k in range(UNICODE_MAX):
    #    if not codepoints[k]:
    #        table[k] = True

    # uniset +WIDTH-A c
    table=[False]*UNICODE_MAX
    set_predicate(table, lambda cp: cp.widthtype == 'A')
    return table

generated_msg="""/*
 * This file was generated with tools/gen_tables.py.
 * DO NOT EDIT IT DIRECTLY.
 * Edit generator instead.
 */

"""
iswide_preambula="""#ifndef _ISWIDE_H
#define _ISWIDE_H 1

#include <stdbool.h>
#include <stdint.h>

/*
 * Since Unicode does not allocate code points
 * in planes 4-13 (and plane 14 contains only control characters),
 * we can save a few bits for attributes by compressing unicode like:
 *
 *  [0x00000, 0x3FFFF] -> [0x00000, 0x3FFFF] (planes 0-3)
 *  [0x40000, 0xDFFFF] -> nothing
 *  [0xE0000,0x10FFFF] -> [0x40000, 0x7FFFF] (planes 14-16 -- Special Purpose Plane, PUA)
 *
 * And with this encoding scheme
 * we can encode all defined characters only with 19 bits.
 *
 * And so we have as much as 13 bits left for flags and attributes.
 */

#define CELL_ENC_COMPACT_BASE 0x40000
#define CELL_ENC_UTF8_BASE 0xE0000

inline static uint32_t uncompact(uint32_t u) {
    return u < CELL_ENC_COMPACT_BASE ? u : u + (CELL_ENC_UTF8_BASE - CELL_ENC_COMPACT_BASE);
}

inline static uint32_t compact(uint32_t u) {
    return u < CELL_ENC_UTF8_BASE ? u : u - (CELL_ENC_UTF8_BASE - CELL_ENC_COMPACT_BASE);
}

"""

def gen_precompose(precompose_file):
    with open(precompose_file, 'w') as f:
        f.write(generated_msg)
        write_precompose(f, filter_compose())

def gen_wide(wide_file, iswide_file):
    with open(wide_file, 'w') as f, open(iswide_file, 'w') as g:
        # FIXME Support ambiguos characters in nsst
        f.write(generated_msg)
        f.write('#ifndef _WIDE_H\n#define _WIDE_H 1\n\n#include <stdint.h>\n\n')
        g.write(generated_msg)
        g.write(iswide_preambula)
        wtab, wtab_min = compress_table(mk_wide())
        ctab, ctab_min = compress_table(mk_combining())
        #atab, atab_min = compress_table(mk_ambiguous())
        write_level1_table(f, g, "wide", wtab)
        write_level1_table(f, g, "combining", ctab)
        #write_level1_table(f, g, "ambiguos", atab)
        write_level2_table(f, g)
        write_predicate(g, wtab_min, 'wide')
        write_predicate(g, ctab_min, 'combining')
        #write_predicate(g, atab_min, 'ambiguos')
        g.write('#endif\n')
        f.write('#endif\n')

parse()
gen_precompose('precompose-table.h')
gen_wide('width-table.h', 'iswide.h')

size2 = len(level2_table)*(LEVEL2_TABLE_WIDTH//8)
size1 = (1 if len(level2_table) <= UINT8_MAX else 2)*level1_total_elems
size3 = precomp_elems*3*2
print("Generated", size1+size2+size3, "bytes of tables")
