/*
-----------------------------------------------------------------------------
This source file is part of OSTIS (Open Semantic Technology for Intelligent Systems)
For the latest info, see http://www.ostis.net

Copyright (c) 2010-2014 OSTIS

OSTIS is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

OSTIS is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with OSTIS.  If not, see <http://www.gnu.org/licenses/>.
-----------------------------------------------------------------------------
*/

#include "sc_storage.h"

#include "sc_defines.h"
#include "sc_segment.h"
#include "sc_element.h"
#include "sc_fs_storage.h"
#include "sc_link_helpers.h"
#include "sc_event.h"
#include "sc_config.h"
#include "sc_iterator.h"

#include "sc_event/sc_event_private.h"
#include "../sc_memory_private.h"

#include <memory.h>
#include <glib.h>

// segments array
sc_segment **segments = 0;
// number of segments
sc_uint32 segments_num = 0;

const sc_uint16 s_max_storage_lock_attempts = 100;
const sc_uint16 s_max_storage_cache_attempts = 10;

sc_bool is_initialized = SC_FALSE;

sc_memory_context *segments_cache_lock_ctx = 0;
sc_int32 segments_cache_count = 0;
sc_segment* segments_cache[SC_SEGMENT_CACHE_SIZE]; // cache of segments that have empty elements

#define CONCURRENCY_TO_CACHE_IDX(x) ((x) % SC_SEGMENT_CACHE_SIZE)

void _sc_segment_cache_lock(const sc_memory_context * ctx)
{
    while (g_atomic_pointer_compare_and_exchange(&segments_cache_lock_ctx, 0, ctx) == FALSE) {}
}

void _sc_segment_cache_unlock(const sc_memory_context *ctx)
{
    g_assert(g_atomic_pointer_get(&segments_cache_lock_ctx) == ctx);
    g_atomic_pointer_set(&segments_cache_lock_ctx, 0);
}

void _sc_segment_cache_append(const sc_memory_context * ctx, sc_segment * seg)
{
    sc_int32 i, idx = CONCURRENCY_TO_CACHE_IDX(ctx->id);
    for (i = 0; i < SC_SEGMENT_CACHE_SIZE; ++i)
    {
        if (g_atomic_pointer_compare_and_exchange(&segments_cache[(idx + i) % SC_SEGMENT_CACHE_SIZE], 0, seg) == TRUE)
        {
            g_atomic_int_inc(&segments_cache_count);
            break;
        }
    }
}

void _sc_segment_cache_remove(const sc_memory_context *ctx, sc_segment *seg)
{
    sc_int32 i, idx = CONCURRENCY_TO_CACHE_IDX(ctx->id);
    for (i = 0; i < SC_SEGMENT_CACHE_SIZE; ++i)
    {
        if (g_atomic_pointer_compare_and_exchange(&segments_cache[(idx + i) % SC_SEGMENT_CACHE_SIZE], seg, 0) == TRUE)
        {
            g_atomic_int_add(&segments_cache_count, -1);
            break;
        }
    }
}

void _sc_segment_cache_clear()
{
    sc_int32 i;
    for (i = 0; i < SC_SEGMENT_CACHE_SIZE; ++i)
        g_atomic_pointer_set(&segments_cache[i], 0);
}

void _sc_segment_cache_update(const sc_memory_context *ctx)
{
    // trying to push segments to cache
    sc_uint32 i;
    for (i = 0; i < g_atomic_int_get(&segments_num); ++i)
    {
        sc_segment *s = g_atomic_pointer_get(&(segments[i]));
        // need to check pointer, because segments_num increments earlier, then segments appends into array
        if (s != nullptr)
        {
            if (sc_segment_has_empty_slot(s))
                _sc_segment_cache_append(ctx, s);
        }

        if (g_atomic_int_get(&segments_cache_count) == SC_SEGMENT_CACHE_SIZE)
            break;
    }
}

sc_segment* _sc_segment_cache_get(const sc_memory_context *ctx)
{
    _sc_segment_cache_lock(ctx);

    sc_segment *seg = 0;
    if (g_atomic_int_get(&segments_cache_count) > 0)
    {
        sc_int32 i, idx = CONCURRENCY_TO_CACHE_IDX(ctx->id);
        for (i = 0; i < SC_SEGMENT_CACHE_SIZE; ++i)
        {
            seg = g_atomic_pointer_get(&segments_cache[(idx + i) % SC_SEGMENT_CACHE_SIZE]);
            if (seg != nullptr)
                goto result;
        }
    }

    // try to update cache
    _sc_segment_cache_update(ctx);

    // if element still not added, then create new segment and append element into it
    sc_int32 seg_num = g_atomic_int_add(&segments_num, 1);
    seg = sc_segment_new(seg_num);
    segments[seg_num] = seg;
    _sc_segment_cache_append(ctx, seg);

    result:
    {
        _sc_segment_cache_unlock(ctx);
    }
    return seg;
}


// -----------------------------------------------------------------------------

sc_bool sc_storage_initialize(const char *path, sc_bool clear)
{
    g_assert( segments == (sc_segment**)0 );
    g_assert( !is_initialized );

    segments = g_new0(sc_segment*, SC_ADDR_SEG_MAX);

    sc_bool res = sc_fs_storage_initialize(path, clear);
    if (res == SC_FALSE)
        return SC_FALSE;

    if (clear == SC_FALSE)
        sc_fs_storage_read_from_path(segments, &segments_num);

    is_initialized = SC_TRUE;

    memset(&(segments_cache[0]), 0, sizeof(sc_segment*) * SC_SEGMENT_CACHE_SIZE);

    return SC_TRUE;
}

void sc_storage_shutdown(sc_bool save_state)
{
    sc_uint idx = 0;
    g_assert( segments != (sc_segment**)0 );


    sc_fs_storage_shutdown(segments, save_state);

    for (idx = 0; idx < SC_ADDR_SEG_MAX; idx++)
    {
        if (segments[idx] == nullptr) continue; // skip segments, that are not loaded
        sc_segment_free(segments[idx]);
    }

    g_free(segments);
    segments = (sc_segment**)0;
    segments_num = 0;

    is_initialized = SC_FALSE;
    _sc_segment_cache_clear();
}

sc_bool sc_storage_is_initialized()
{
    return is_initialized;
}

sc_bool sc_storage_is_element(const sc_memory_context *ctx, sc_addr addr)
{
    sc_element *el = 0;
    if (sc_storage_element_lock(ctx, addr, &el) != SC_RESULT_OK || el == 0)
        return SC_FALSE;

    sc_bool res = SC_TRUE;
    if (el->flags.type == 0)
        res = SC_FALSE;

    sc_storage_element_unlock(ctx, addr);

    return res;
}

sc_element* sc_storage_append_el_into_segments(const sc_memory_context *ctx, sc_element *element, sc_addr *addr)
{
    sc_segment *segment = 0;

    g_assert( addr != 0 );
    SC_ADDR_MAKE_EMPTY(*addr);

    if (g_atomic_int_get(&segments_num) >= sc_config_get_max_loaded_segments())
        return nullptr;

    /// @todo store segment with empty slots
    // try to find segment with empty slots
    sc_segment * seg = (sc_segment*)0x1;
    while (seg != 0)
    {
        sc_segment *seg = _sc_segment_cache_get(ctx);

        if (seg == nullptr)
            break;

        sc_element *el = sc_segment_lock_empty_element(ctx, seg, &addr->offset);
        if (el != nullptr)
        {
            addr->seg = seg->num;
            *el = *element;
            return el;
        }else
            _sc_segment_cache_remove(ctx, seg);
    }

    return nullptr;
}

sc_addr sc_storage_element_new(const sc_memory_context *ctx, sc_type type)
{
    sc_element el;
    sc_addr addr;
    sc_element *res = 0;

    memset(&el, 0, sizeof(el));
    el.flags.type = type;

    res = sc_storage_append_el_into_segments(ctx, &el, &addr);
    sc_storage_element_unlock(ctx, addr);
    g_assert(res != 0);
    return addr;
}

sc_result sc_storage_element_free(const sc_memory_context *ctx, sc_addr addr)
{
    GHashTable *remove_table = 0, *lock_table = 0;
    GSList *remove_list = 0;

    // first of all we need to collect and lock all elements
    sc_element *el;
    if (sc_storage_element_lock(ctx, addr, &el) != SC_RESULT_OK)
        return SC_RESULT_ERROR;
    if (el == nullptr || el->flags.type == 0)
        return SC_RESULT_ERROR;

    remove_table = g_hash_table_new(g_direct_hash, g_direct_equal);
    lock_table = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(remove_table, GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(addr)), el);
    g_hash_table_insert(lock_table, GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(addr)), el);

    remove_list = g_slist_append(remove_list, GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(addr)));
    while (remove_list != 0)
    {
        // get sc-addr for removing
        sc_uint32 addr_int = GPOINTER_TO_UINT(remove_list->data);
        sc_addr _addr;
        _addr.seg = SC_ADDR_LOCAL_SEG_FROM_INT(addr_int);
        _addr.offset = SC_ADDR_LOCAL_OFFSET_FROM_INT(addr_int);

        gpointer p_addr = GUINT_TO_POINTER(addr_int);

        // go to next sc-addr in list
        remove_list = g_slist_delete_link(remove_list, remove_list);

        el = g_hash_table_lookup(lock_table, p_addr);
        if (el == nullptr)
        {
            STORAGE_CHECK_CALL(sc_storage_element_lock(ctx, _addr, &el));

            g_assert(el != 0 && el->flags.type != 0);
            g_hash_table_insert(remove_table, p_addr, el);
            g_hash_table_insert(lock_table, p_addr, el);
        }

        // remove registered events before deletion
        sc_event_notify_element_deleted(_addr);

        if (el->flags.type & sc_type_arc_mask)
        {
            sc_event_emit(el->arc.begin, SC_EVENT_REMOVE_OUTPUT_ARC, _addr);
            sc_event_emit(el->arc.end, SC_EVENT_REMOVE_INPUT_ARC, _addr);

            // lock begin and end elements of arc
            sc_element *el2 = 0;
            p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(el->arc.begin));
            if ((el2 = g_hash_table_lookup(lock_table, p_addr)) == nullptr)
            {
                STORAGE_CHECK_CALL(sc_storage_element_lock(ctx, el->arc.begin, &el2));
                g_hash_table_insert(lock_table, p_addr, el2);
            }
            g_assert(el2 != 0);
            el2->first_out_arc = el->arc.next_out_arc;

            p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(el->arc.end));
            if ((el2 = g_hash_table_lookup(lock_table, p_addr)) == nullptr)
            {
                el2 = 0;
                STORAGE_CHECK_CALL(sc_storage_element_lock(ctx, el->arc.end, &el2));
                g_hash_table_insert(lock_table, p_addr, el2);
            }
            g_assert(el2 != 0);
            el2->first_in_arc = el->arc.next_in_arc;

            // lock next/prev arcs in out/in lists
            if (SC_ADDR_IS_NOT_EMPTY(el->arc.prev_out_arc))
            {
                p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(el->arc.prev_out_arc));
                if (g_hash_table_lookup(lock_table, p_addr) == nullptr)
                {
                    el2 = 0;
                    STORAGE_CHECK_CALL(sc_storage_element_lock(ctx, el->arc.prev_out_arc, &el2));
                    g_assert(el2 != 0);
                    g_hash_table_insert(lock_table, p_addr, el2);
                }
            }

            if (SC_ADDR_IS_NOT_EMPTY(el->arc.prev_in_arc))
            {
                p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(el->arc.prev_in_arc));
                if (g_hash_table_lookup(lock_table, p_addr) == nullptr)
                {
                    el2 = 0;
                    STORAGE_CHECK_CALL(sc_storage_element_lock(ctx, el->arc.prev_in_arc, &el2));
                    g_assert(el2 != 0);
                    g_hash_table_insert(lock_table, p_addr, el2);
                }
            }

            if (SC_ADDR_IS_NOT_EMPTY(el->arc.next_out_arc))
            {
                p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(el->arc.next_out_arc));
                if (g_hash_table_lookup(lock_table, p_addr) == nullptr)
                {
                    el2 = 0;
                    STORAGE_CHECK_CALL(sc_storage_element_lock(ctx, el->arc.next_out_arc, &el2));
                    g_assert(el2 != 0);
                    g_hash_table_insert(lock_table, p_addr, el2);
                }
            }

            if (SC_ADDR_IS_NOT_EMPTY(el->arc.next_in_arc))
            {
                p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(el->arc.next_in_arc));
                if (g_hash_table_lookup(lock_table, p_addr) == nullptr)
                {
                    el2 = 0;
                    STORAGE_CHECK_CALL(sc_storage_element_lock(ctx, el->arc.next_in_arc, &el2));
                    g_assert(el2 != 0);
                    g_hash_table_insert(lock_table, p_addr, el2);
                }
            }
        }

        // Iterate all connectors for deleted element and append them into remove_list
        _addr = el->first_out_arc;
        while (SC_ADDR_IS_NOT_EMPTY(_addr))
        {
            gpointer p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(_addr));
            sc_element *el2 = g_hash_table_lookup(remove_table, p_addr);

            if (el2 == nullptr)
            {
                sc_storage_element_lock(ctx, _addr, &el2);
                g_assert(el2 != nullptr);

                g_hash_table_insert(remove_table, p_addr, el2);
                g_hash_table_insert(lock_table, p_addr, el2);

                remove_list = g_slist_append(remove_list, p_addr);
            }

            _addr = el2->arc.next_out_arc;
        }

        _addr = el->first_in_arc;
        while (SC_ADDR_IS_NOT_EMPTY(_addr))
        {
            gpointer p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(_addr));
            sc_element *el2 = g_hash_table_lookup(remove_table, p_addr);

            if (el2 == nullptr)
            {
                sc_storage_element_lock(ctx, _addr, &el2);
                g_assert(el2 != nullptr);

                g_hash_table_insert(remove_table, p_addr, el2);
                g_hash_table_insert(lock_table, p_addr, el2);

                remove_list = g_slist_append(remove_list, p_addr);
            }

            _addr = el2->arc.next_in_arc;
        }

        // clean temp addr
        SC_ADDR_MAKE_EMPTY(_addr);
    }

    // now we need to erase all elements
    GHashTableIter iter;
    g_hash_table_iter_init(&iter, remove_table);
    gpointer key, value;
    while (g_hash_table_iter_next(&iter, &key, &value) == TRUE)
    {
        el = value;
        sc_uint32 uint_addr = GPOINTER_TO_UINT(key);
        gpointer p_addr;
        addr.offset = SC_ADDR_LOCAL_OFFSET_FROM_INT(uint_addr);
        addr.seg = SC_ADDR_LOCAL_SEG_FROM_INT(uint_addr);

        // delete arcs from output and input lists
        if (el->flags.type & sc_type_arc_mask)
        {
            // output arcs
            sc_addr prev_arc = el->arc.prev_out_arc;
            sc_addr next_arc = el->arc.next_out_arc;

            if (SC_ADDR_IS_NOT_EMPTY(prev_arc))
            {
                p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(prev_arc));
                sc_element *prev_el_arc = g_hash_table_lookup(lock_table, p_addr);
                g_assert(prev_el_arc != nullptr);
                prev_el_arc->arc.next_out_arc = next_arc;

            }

            if (SC_ADDR_IS_NOT_EMPTY(next_arc))
            {
                p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(next_arc));
                sc_element *next_el_arc = g_hash_table_lookup(lock_table, p_addr);
                g_assert(next_el_arc != nullptr);
                next_el_arc->arc.prev_out_arc = prev_arc;
            }

            sc_element *b_el = g_hash_table_lookup(lock_table, GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(el->arc.begin)));
            sc_bool need_unlock = SC_FALSE;
            if (b_el == nullptr)
            {
                STORAGE_CHECK_CALL(sc_storage_element_lock(ctx, el->arc.begin, &b_el));
                need_unlock = SC_TRUE;
            }
            if (SC_ADDR_IS_EQUAL(addr, b_el->first_out_arc))
                b_el->first_out_arc = next_arc;

            if (need_unlock)
                sc_storage_element_unlock(ctx, el->arc.begin);

            // input arcs
            prev_arc = el->arc.prev_in_arc;
            next_arc = el->arc.next_in_arc;

            if (SC_ADDR_IS_NOT_EMPTY(prev_arc))
            {
                p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(prev_arc));
                sc_element *prev_el_arc = g_hash_table_lookup(lock_table, p_addr);
                g_assert(prev_el_arc != nullptr);
                prev_el_arc->arc.next_in_arc = next_arc;
            }

            if (SC_ADDR_IS_NOT_EMPTY(next_arc))
            {
                p_addr = GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(next_arc));
                sc_element *next_el_arc = g_hash_table_lookup(lock_table, p_addr);
                g_assert(next_el_arc != nullptr);
                next_el_arc->arc.prev_in_arc = prev_arc;
            }

            need_unlock = SC_FALSE;
            sc_element *e_el = g_hash_table_lookup(lock_table, GUINT_TO_POINTER(SC_ADDR_LOCAL_TO_INT(el->arc.end)));
            if (e_el == nullptr)
            {
                STORAGE_CHECK_CALL(sc_storage_element_lock(ctx, el->arc.end, &e_el));
                need_unlock = SC_TRUE;
            }
            if (SC_ADDR_IS_EQUAL(addr, b_el->first_in_arc))
                e_el->first_in_arc = next_arc;
            if (need_unlock)
                sc_storage_element_unlock(ctx, el->arc.end);
        }

        sc_segment_erase_element(g_atomic_pointer_get(&segments[addr.seg]), addr.offset);
        _sc_segment_cache_append(ctx, g_atomic_pointer_get(&segments[addr.seg]));
    }

    // now unlock elements
    g_hash_table_iter_init(&iter, lock_table);
    while (g_hash_table_iter_next(&iter, &key, &value) == TRUE)
    {
        sc_uint32 uint_addr = GPOINTER_TO_UINT(key);
        addr.offset = SC_ADDR_LOCAL_OFFSET_FROM_INT(uint_addr);
        addr.seg = SC_ADDR_LOCAL_SEG_FROM_INT(uint_addr);

        sc_storage_element_unlock(ctx, addr);
    }

    g_slist_free(remove_list);
    g_hash_table_destroy(remove_table);
    g_hash_table_destroy(lock_table);

    sc_event_emit(addr, SC_EVENT_REMOVE_ELEMENT, addr);

    return SC_RESULT_OK;
}

sc_addr sc_storage_node_new(const sc_memory_context *ctx, sc_type type )
{
    sc_element el;
    sc_addr addr;

    g_assert( !(sc_type_arc_mask & type) );
    memset(&el, 0, sizeof(el));

    el.flags.type = sc_type_node | type;

    sc_element *locked_el = sc_storage_append_el_into_segments(ctx, &el, &addr);
    if (locked_el == nullptr)
    {
        SC_ADDR_MAKE_EMPTY(addr);
    }
    else
        STORAGE_CHECK_CALL(sc_storage_element_unlock(ctx, addr));
    return addr;
}

sc_addr sc_storage_link_new(const sc_memory_context *ctx)
{
    sc_element el;
    sc_addr addr;

    memset(&el, 0, sizeof(el));
    el.flags.type = sc_type_link;

    sc_element *locked_el = sc_storage_append_el_into_segments(ctx, &el, &addr);
    if (locked_el == nullptr)
    {
        SC_ADDR_MAKE_EMPTY(addr);
    }
    else
        STORAGE_CHECK_CALL(sc_storage_element_unlock(ctx, addr));
    return addr;
}

sc_addr sc_storage_arc_new(const sc_memory_context *ctx, sc_type type, sc_addr beg, sc_addr end)
{
    sc_addr addr;
    sc_element el;

    memset(&el, 0, sizeof(el));
    g_assert( !(sc_type_node & type) );
    el.flags.type = (type & sc_type_arc_mask) ? type : (sc_type_arc_common | type);

    el.arc.begin = beg;
    el.arc.end = end;

    sc_result r;
    SC_ADDR_MAKE_EMPTY(addr);
    while (SC_ADDR_IS_EMPTY(addr))
    {
        sc_element *beg_el = 0, *end_el = 0;
        sc_element *f_out_arc = 0, *f_in_arc = 0;
        sc_element *tmp_el = 0;

        // try to lock begin end end elements
        r = sc_storage_element_lock_try(ctx, beg, s_max_storage_lock_attempts, &beg_el);
        if (beg_el == nullptr)
            goto unlock;

        r = sc_storage_element_lock_try(ctx, end, s_max_storage_lock_attempts, &end_el);
        if (end_el == nullptr)
            goto unlock;

        // lock arcs to change output/input list
        sc_addr first_out_arc = beg_el->first_out_arc;
        if (SC_ADDR_IS_NOT_EMPTY(first_out_arc))
        {
            r = sc_storage_element_lock_try(ctx, first_out_arc, s_max_storage_lock_attempts, &f_out_arc);
            if (f_out_arc == nullptr)
                goto unlock;
        }

        sc_addr first_in_arc = end_el->first_in_arc;
        if (SC_ADDR_IS_NOT_EMPTY(first_in_arc))
        {
            r = sc_storage_element_lock_try(ctx, first_in_arc, s_max_storage_lock_attempts, &f_in_arc);
            if (f_in_arc == nullptr)
                goto unlock;
        }

        // get new element
        tmp_el = sc_storage_append_el_into_segments(ctx, &el, &addr);

        g_assert(tmp_el != 0);
        g_assert(SC_ADDR_IS_NOT_EQUAL(addr, first_in_arc));

        // emit events
        sc_event_emit(beg, SC_EVENT_ADD_OUTPUT_ARC, addr);
        sc_event_emit(end, SC_EVENT_ADD_INPUT_ARC, addr);

        // check values
        g_assert(beg_el != nullptr && end_el != nullptr);
        g_assert(beg_el->flags.type != 0 && end_el->flags.type != 0);

        // set next output arc for our created arc
        tmp_el->arc.next_out_arc = first_out_arc;
        tmp_el->arc.next_in_arc = first_in_arc;

        g_assert(SC_ADDR_IS_NOT_EQUAL(addr, first_out_arc) && SC_ADDR_IS_NOT_EQUAL(addr, first_in_arc));
        if (f_out_arc)
            f_out_arc->arc.prev_out_arc = addr;

        if (f_in_arc)
            f_in_arc->arc.prev_in_arc = addr;

        // set our arc as first output/input at begin/end elements
        beg_el->first_out_arc = addr;
        end_el->first_in_arc = addr;

        unlock:
        {
            if (beg_el)
            {
                if (f_out_arc)
                    sc_storage_element_unlock(ctx, first_out_arc);
                sc_storage_element_unlock(ctx, beg);
            }
            if (end_el)
            {
                if (f_in_arc)
                    sc_storage_element_unlock(ctx, first_in_arc);
                sc_storage_element_unlock(ctx, end);
            }

            if (tmp_el)
                sc_storage_element_unlock(ctx, addr);
            if (r != SC_RESULT_OK)
                return addr;
        }

    }

    return addr;
}

sc_result sc_storage_get_element_type(const sc_memory_context *ctx, sc_addr addr, sc_type *result)
{
    sc_element *el;
    if (sc_storage_element_lock(ctx, addr, &el) != SC_RESULT_OK)
        return SC_RESULT_ERROR;

    if (el == nullptr)
        return SC_RESULT_ERROR;

    *result = el->flags.type;
    return sc_storage_element_unlock(ctx, addr);
}

sc_result sc_storage_change_element_subtype(const sc_memory_context *ctx, sc_addr addr, sc_type type)
{
    if (type & sc_type_element_mask)
        return SC_RESULT_ERROR_INVALID_PARAMS;

    sc_element *el;
    if (sc_storage_element_lock(ctx, addr, &el) != SC_RESULT_OK)
        return SC_RESULT_ERROR;

    if (el == nullptr)
        return SC_RESULT_ERROR;

    el->flags.type = (el->flags.type & sc_type_element_mask) | (type & ~sc_type_element_mask);
    return sc_storage_element_unlock(ctx, addr);
}

sc_result sc_storage_get_arc_begin(const sc_memory_context *ctx, sc_addr addr, sc_addr *result)
{
    sc_element *el;
    if (sc_storage_element_lock(ctx, addr, &el) != SC_RESULT_OK)
        return SC_RESULT_ERROR;

    sc_result res = SC_RESULT_ERROR_INVALID_TYPE;
    if (el->flags.type & sc_type_arc_mask)
    {
        *result = el->arc.begin;
        res = SC_RESULT_OK;
    }
    sc_storage_element_unlock(ctx, addr);
    return res;
}

sc_result sc_storage_get_arc_end(const sc_memory_context *ctx, sc_addr addr, sc_addr *result)
{
    sc_element *el;
    if (sc_storage_element_lock(ctx, addr, &el) != SC_RESULT_OK)
        return SC_RESULT_ERROR;

    sc_result res = SC_RESULT_ERROR_INVALID_TYPE;
    if (el->flags.type & sc_type_arc_mask)
    {
        *result = el->arc.end;
        res = SC_RESULT_OK;
    }
    sc_storage_element_unlock(ctx, addr);
    return res;
}

sc_result sc_storage_set_link_content(const sc_memory_context *ctx, sc_addr addr, const sc_stream *stream)
{
    sc_element *el;

    if (sc_storage_element_lock(ctx, addr, &el) != SC_RESULT_OK || el == nullptr)
        return SC_RESULT_ERROR;

    sc_check_sum check_sum;
    sc_result result = SC_RESULT_ERROR;

    g_assert(stream != nullptr);


    if (!(el->flags.type & sc_type_link))
    {
        result = SC_RESULT_ERROR_INVALID_TYPE;
        goto clean;
    }

    // calculate checksum for data
    if (sc_link_calculate_checksum(stream, &check_sum) == SC_TRUE)
    {
        result = sc_fs_storage_write_content(addr, &check_sum, stream);
        memcpy(el->content.data, check_sum.data, check_sum.len);
        result = SC_RESULT_OK;
    }
    g_assert(result == SC_RESULT_OK);

    clean:
    {
        if (el != nullptr)
            STORAGE_CHECK_CALL(sc_storage_element_unlock(ctx, addr));
    }

    return result;
}

sc_result sc_storage_get_link_content(const sc_memory_context *ctx, sc_addr addr, sc_stream **stream)
{
    sc_element *el;
    if (sc_storage_element_lock(ctx, addr, &el) != SC_RESULT_OK || el == nullptr)
        return SC_RESULT_ERROR;

    sc_result res = SC_RESULT_ERROR;
    if (!(el->flags.type & sc_type_link))
    {
        res = SC_RESULT_ERROR_INVALID_TYPE;
        goto clean;
    }

    // prepare checksum
    sc_check_sum checksum;
    checksum.len = SC_CHECKSUM_LEN;
    memcpy(checksum.data, el->content.data, checksum.len);

    res = sc_fs_storage_get_checksum_content(&checksum, stream);

    clean:
    {
        if (el != nullptr)
            STORAGE_CHECK_CALL(sc_storage_element_unlock(ctx, addr));
    }

    return res;
}

sc_result sc_storage_find_links_with_content(const sc_memory_context *ctx, const sc_stream *stream, sc_addr **result, sc_uint32 *result_count)
{
    g_assert(stream != 0);
    sc_check_sum check_sum;
    if (sc_link_calculate_checksum(stream, &check_sum) == SC_TRUE)
        return sc_fs_storage_find_links_with_content(&check_sum, result, result_count);

    return SC_RESULT_ERROR;
}


sc_result sc_storage_get_elements_stat(const sc_memory_context *ctx, sc_stat *stat)
{
    /// @todo implement function

    g_assert( stat != (sc_stat*)0 );

    memset(stat, 0, sizeof(sc_stat));
    stat->segments_count = sc_storage_get_segments_count();

    sc_uint i;
    for (i = 0; i < g_atomic_int_get(&segments_num); ++i)
    {
        sc_segment *seg = segments[i];
        sc_segment_collect_elements_stat(ctx, seg, stat);
    }

    return SC_TRUE;
}

unsigned int sc_storage_get_segments_count()
{
    return g_atomic_int_get(&segments_num);
}

sc_result sc_storage_element_lock(const sc_memory_context *ctx, sc_addr addr, sc_element **el)
{
    if (addr.seg >= SC_ADDR_SEG_MAX)
    {
        *el = 0;
        return SC_RESULT_ERROR;
    }

    sc_segment *segment = g_atomic_pointer_get(&segments[addr.seg]);
    if (segment == 0)
    {
        *el = 0;
        return SC_RESULT_ERROR;
    }

    *el = sc_segment_lock_element(ctx, segment, addr.offset);
    return SC_RESULT_OK;
}

sc_result sc_storage_element_lock_try(const sc_memory_context *ctx, sc_addr addr, sc_uint16 max_attempts, sc_element **el)
{
    if (addr.seg >= SC_ADDR_SEG_MAX)
    {
        *el = 0;
        return SC_RESULT_ERROR;
    }

    sc_segment *segment = g_atomic_pointer_get(&segments[addr.seg]);
    if (segment == 0)
    {
        *el = 0;
        return SC_RESULT_ERROR;
    }

    *el = sc_segment_lock_element_try(ctx, segment, addr.offset, max_attempts);
    return SC_RESULT_OK;
}

sc_result sc_storage_element_unlock(const sc_memory_context *ctx, sc_addr addr)
{
    if (addr.seg >= SC_ADDR_SEG_MAX)
        return SC_RESULT_ERROR;

    sc_segment *segment = g_atomic_pointer_get(&segments[addr.seg]);
    if (segment == 0)
        return SC_RESULT_ERROR;

    sc_segment_unlock_element(ctx, segment, addr.offset);
    return SC_RESULT_OK;
}

