/*
 *  insert.c
 *
 *  $Id$
 *
 *  Insert
 *  
 *  This file is part of the OpenLink Software Virtuoso Open-Source (VOS)
 *  project.
 *  
 *  Copyright (C) 1998-2006 OpenLink Software
 *  
 *  This project is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; only version 2 of the License, dated June 1991.
 *  
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *  General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *  
 *  
*/

#include "sqlnode.h"


#ifdef PAGE_TRACE
#define rdbg_printf_2(q) printf q
#endif


void
pg_map_clear (buffer_desc_t * buf)
{
#ifdef PAGE_TRACE
  memset (&buf->bd_content_map->pm_entries[0], 0, buf->bd_content_map->pm_size * sizeof (short));
  /* Looks nicer in debugger. */
#endif
  buf->bd_content_map->pm_filled_to = DP_DATA;
  buf->bd_content_map->pm_bytes_free = PAGE_SZ - DP_DATA;
  buf->bd_content_map->pm_count = 0;
}


#define PG_ERR_OR_GPF_T \
  { if (assertion_on_read_fail) GPF_T; \
    else return WI_ERROR; }



void
map_resize (page_map_t ** pm_ret, int new_sz)
{
  page_map_t * pm = *pm_ret;
  page_map_t * new_pm = (page_map_t *) resource_get (PM_RC (new_sz));
  memcpy (new_pm, pm, PM_ENTRIES_OFFSET + sizeof (short) * pm->pm_count);
  *pm_ret = new_pm;
  resource_store (PM_RC (pm->pm_size), (void*) pm);
  new_pm->pm_size = new_sz;
}


void
map_append (page_map_t ** pm_ret, int ent)
{
  page_map_t * pm = *pm_ret;
  if (pm->pm_count + 1 > pm->pm_size)
    {
      int new_sz = PM_SIZE (pm->pm_size);
      if (pm->pm_count + 1 > PM_MAX_ENTRIES)
	GPF_T1 ("page map entry count overflow");
      map_resize (pm_ret, new_sz);
      pm = *pm_ret;
    }
  pm->pm_entries[pm->pm_count++] = ent;
}


int
row_length (db_buf_t row, dbe_key_t * key)
{
  int len;
  key_id_t key_id = SHORT_REF (row + IE_KEY_ID);
  if (key_id && key_id != key->key_id)
    {
      /* if obsolete row, subtable etc. If temp, then both given key and row have KI_TEMP and if leaf then id == 0 */
      key = sch_id_to_key (wi_inst.wi_schema, key_id);
    }
  ROW_LENGTH (row, key, len);
  return len;
}


void
row_write_reserved (dtp_t * end, int bytes)
{
  if (bytes < 128)
    end[0] = bytes;
  else
    {
      end[0] = 0;
      SHORT_SET_NA (end + 1, bytes);
    }
}


int
row_reserved_length (db_buf_t row, dbe_key_t * key)
{
  int len;
  key_id_t key_id = SHORT_REF (row + IE_KEY_ID);
  if (key_id && key_id != key->key_id)
    {
      /* if obsolete row, subtable etc. If temp, then both given key and row have KI_TEMP and if leaf then id == 0 */
      if (KI_LEFT_DUMMY == key_id)
	return 8;
      key = sch_id_to_key (wi_inst.wi_schema, key_id);
      if (!key)
	{
	  if (assertion_on_read_fail)
	    GPF_T1 ("Row with bad key");
	  return 8;  /* for crash dump this is not essential. Ret minimum */
	}
    }
  ROW_LENGTH (row, key, len);
  if (IE_ISSET (row, IEF_UPDATE))
    {
      dtp_t gap = row[len];
      if (!SHORT_REF (row + IE_KEY_ID))
	GPF_T1 ("a leaf pointer is not suppposed to have the updated flag on");
      if (gap)
	len += gap;
      else
	len += SHORT_REF_NA (row + len + 1);
    }
  return len;
}


int
pg_make_map (buffer_desc_t * buf)
{
  db_buf_t page = buf->bd_buffer;
  key_id_t k_id = SHORT_REF (page + DP_KEY_ID);
  dbe_key_t * pg_key;
  int free = PAGE_SZ - DP_DATA, sz;
  int pos = SHORT_REF (page + DP_FIRST);
  page_map_t *map = buf->bd_content_map;
  int len, inx = 0, fill = DP_DATA;

  buf->bd_content_map = NULL;
  if (!wi_inst.wi_schema)
    {
      log_error (
	  "Trying to access the database schema data before the schema has been initialized. "
	  "This is usually caused by an unrecovarable corrupted database file. ");
      call_exit (-1);
    }

  pg_key = KI_TEMP == k_id ?
      buf->bd_space->isp_tree->it_key :
      sch_id_to_key (wi_inst.wi_schema, k_id);
  if (!map)
    {
      map = (page_map_t *) resource_get (PM_RC (PM_SZ_1));
    }
  if (pos && !pg_key)
    {
      if (assertion_on_read_fail)
	GPF_T1 ("page read with no key defd");
      map->pm_count = 0;
      buf->bd_content_map = map;
      return 0;
    }
  while (pos)
    {
      if (pos >= PAGE_SZ)
	PG_ERR_OR_GPF_T;	/* Link over end */
      len = row_reserved_length (page + pos, pg_key);
      len = ROW_ALIGN (len);
      if (len < 0)
	STRUCTURE_FAULT;
      free -= len;
      if (inx >= map->pm_size)
	{
	  map->pm_count = inx;
	  map_resize (&map, PM_SIZE (map->pm_size));
	}
      map->pm_entries[inx++] = pos;
      if (pos + len > fill)
	fill = pos + len;
      if (fill > PAGE_SZ)
	STRUCTURE_FAULT;
      pos = IE_NEXT (page + pos);
      if (inx >= PM_MAX_ENTRIES)
	STRUCTURE_FAULT;
    }
  if (free < 0)
    STRUCTURE_FAULT;
  map->pm_bytes_free = free;
  map->pm_count = inx;
  map->pm_filled_to = fill;
  sz = PM_SIZE (map->pm_count);
  if (sz < map->pm_size)
    {
      map_resize (&map, sz);
    }
  buf->bd_content_map = map;
  return 1;
}


#if defined (MTX_DEBUG) | defined (PAGE_TRACE)
void
pg_check_map (buffer_desc_t * buf)
{
  page_map_t org_map;
  int org_free = buf->bd_content_map->pm_bytes_free;
  int pos, ctr = 0;
  db_buf_t page;
  /*memcpy (&org_map, buf->bd_content_map, ((ptrlong)(&((page_map_t*)0)->pm_entries)) + 2 * buf->bd_content_map->pm_count); */
  /* for debug, copy the entries, the whole struct may overflow addr space. */
  if (!buf->bd_is_write)
    GPF_T1 ("must have written access to buffer to check it");
#ifdef MTX_DEBUG
  if (buf->bd_writer != THREAD_CURRENT_THREAD)
    GPF_T1 ("Must have write on buffer to check it");
#endif
  pg_make_map (buf);
  if (org_free != buf->bd_content_map->pm_bytes_free)
    GPF_T1 ("map bytes free out of sync");
#if 0
  /* debug code for catching insert/update of a particular row */
  page = buf->bd_buffer;
  pos = SHORT_REF (page + DP_FIRST);
  while (pos)
    {
      key_id_t ki = SHORT_REF (page + pos + IE_KEY_ID);
      if (1001 == ki)
	{
	  if (1000037 == LONG_REF (page + pos + 4)
	      && 1155072 == LONG_REF (page + pos + 12)
	      && 1369287 == LONG_REF_NA (page + pos + 4 +17))
	    ctr++;
	  if (ctr > 1)
	    printf ("bingbing\n");
	}
      pos = IE_NEXT (page + pos);
    }
#endif
}
#endif

void
pg_move_cursors (it_cursor_t ** temp_itc, int fill, dp_addr_t dp_from,
    int from, dp_addr_t page_to, int to, buffer_desc_t * buf_to)
{
  int n;
  it_cursor_t *it_list;
  index_space_t *isp;
  if (!fill)
    return;

  for (n = 0; n < fill; n++)
    {
      it_list = temp_itc[n];
      if (!it_list)
	continue;
      isp = it_list->itc_space_registered;
      if (it_list->itc_page == dp_from
	  && it_list->itc_position == from)
	{
	  temp_itc[n] = NULL;
	  /* Once a cursor has been moved it will not move again
	     during the same pg_write_compact. Consider: x is moved to
	     future place, then the same place in pre-compacted is moved
	     someplace else.
	     If so the itc gets moved twice which is NEVER RIGHT */
	  it_list->itc_position = to;
	  if (page_to != it_list->itc_page)
	    {
	      itc_unregister (it_list, INSIDE_MAP);
	      it_list->itc_page = page_to;
	      itc_register_cursor (it_list, INSIDE_MAP);
	    }
	}
    }
}


void
pg_delete_move_cursors (it_cursor_t * itc, dp_addr_t dp_from,
    int from, dp_addr_t page_to, int to, buffer_desc_t * buf_to)
{
  it_cursor_t *it_list;

  it_list = (it_cursor_t *)
      gethash (DP_ADDR2VOID (dp_from), itc->itc_space->isp_page_to_cursor);

  while (it_list)
    {
      it_cursor_t *next = it_list->itc_next_on_page;
      if (buf_to)
	{
	  rdbg_printf (("  itc delete move by T=%d moved itc=%x  from L=%d pos=%d to L=%d pos=%d was_on_row=%d \n",
			TRX_NO (itc->itc_ltrx), it_list, it_list->itc_page, it_list->itc_position, page_to, to, it_list->itc_is_on_row));
	  itc_unregister (it_list, INSIDE_MAP);
	  it_list->itc_page = page_to;
	  it_list->itc_position = to;
	  it_list->itc_is_on_row = 0;
	  itc_register_cursor (it_list, INSIDE_MAP);

	}
      else
	{
	  if (it_list->itc_page == dp_from
	      && it_list->itc_position == from)
	    {
	      rdbg_printf (("  itc delete move inside page by T=%d moved itc=%x  from L=%d pos=%d to L=%d pos=%d was_on_row=%d \n",
			    TRX_NO (itc->itc_ltrx), it_list, it_list->itc_page, it_list->itc_position, page_to, to, it_list->itc_is_on_row));

	      it_list->itc_position = to;
	      it_list->itc_is_on_row = 0;
	    }
	}
      it_list = next;
    }
}


/* This function will write one page plus one insert as a compact
   image on one or two pages. All registered cursors will be moved.
   The argument cursor is set to point to the start of the inserted thing */

#define IS_FIRST(p, len) \
  if (!is_to_extend) \
    { \
      if (!first_copied) \
	first_copied = p; \
      last_copied = p + len;  \
    }

#define SWITCH_TO_EXT \
  if (!is_to_extend) \
    { \
      map_to = buf_ext->bd_content_map; \
      is_to_extend = 1; \
      prev_ent = 0; \
      page_to = ext_page; \
      place_to = DP_DATA; \
      pg_map_clear (buf_ext); \
    } \
  else \
    { \
      GPF_T; \
    }

#define LINK_PREV(ent)   \
  if (prev_ent) \
    { \
      IE_SET_NEXT (page_to + prev_ent, ent); \
    } \
  else \
    { \
      if (ent != DP_DATA) \
	GPF_T; \
      SHORT_SET (page_to + DP_FIRST, ent); \
    } \
  prev_ent = ent; \
  IE_SET_NEXT (page_to + prev_ent, 0); \

#define LINK_PREV_NC(ent)   \
  if (prev_ent) \
    { \
      IE_SET_NEXT (page_to + prev_ent, ent); \
    } \
  else \
    { \
      SHORT_SET (page_to + DP_FIRST, ent); \
    } \
  prev_ent = ent; \
  IE_SET_NEXT (page_to + prev_ent, 0); \



#define ADD_MAP_ENT(ent, len) \
  map_append (is_to_extend ? &buf_ext->bd_content_map : &buf_from->bd_content_map, ent); \
  map_to = is_to_extend ? buf_ext->bd_content_map : buf_from->bd_content_map; \
  map_to->pm_filled_to = ent + (int) (ROW_ALIGN (len)); \
  map_to->pm_bytes_free -= (int) (ROW_ALIGN (len)); \


int 
is_ptr_in_array (void**array, int fill, void* ptr)
{
  int inx;
  for (inx = 0; inx < fill; inx++)
    if (array[inx] == ptr)
      return 1;
  return 0;
}


void
itc_keep_together (it_cursor_t * itc, buffer_desc_t * buf, it_cursor_t ** cursors_in_trx, int cr_fill)
{
  it_cursor_t * cr_tmp [1000];
  int inx;

  if (itc->itc_bm_split_left_side)
    {
      /* this inserts the right side of a split bm inx entry. Move the crs registered on the left side to the right side 
       * if they are past the dividing value.  If just landed waiting, mark then to reset the search */
      placeholder_t * left = itc->itc_bm_split_left_side;
      int fill = 0;
      it_cursor_t * registered;
      bitno_t r_split = unbox_iri_int64 (itc->itc_search_params[itc->itc_search_par_fill - 1]);
      ITC_IN_MAP (itc);
      registered = (it_cursor_t*) gethash (DP_ADDR2VOID (left->itc_page), itc->itc_space->isp_page_to_cursor);
      while (registered)
	{
	  if (registered != (it_cursor_t*) left 
	      && registered->itc_position == left->itc_position
	      && !is_ptr_in_array ((void**)cursors_in_trx, cr_fill, (void*)registered))
	    {
	      /* if it is on the split row and above the split point or if it has not yet read the splitting row because it was locked */
	      if (registered->itc_bp.bp_just_landed || registered->itc_bp.bp_value >= r_split)
		cr_tmp[fill++]= registered;
	    }
	  registered = registered->itc_next_on_page;
	}
      for (inx = 0; inx < fill; inx++)
	{
	  itc_unregister (cr_tmp[inx], INSIDE_MAP);
	  cr_tmp[inx]->itc_to_reset = RWG_WAIT_SPLIT; /*if just landed, this will make it reset the search*/
	  cr_tmp[inx]->itc_position = itc->itc_position;
	  cr_tmp[inx]->itc_page = itc->itc_page;
	  cr_tmp[inx]->itc_bp.bp_is_pos_valid = 0;
	  itc_register_cursor (cr_tmp[inx], INSIDE_MAP);
	}
      itc_unregister (left, INSIDE_MAP);
      itc->itc_bm_split_left_side = NULL;
    }

  if (!itc->itc_keep_together_pos)
    return;
  ITC_IN_MAP (itc);
  if (!cursors_in_trx)
    {
      it_cursor_t * registered = (it_cursor_t *)
	gethash (DP_ADDR2VOID (itc->itc_keep_together_dp), itc->itc_space->isp_page_to_cursor);
      /* could be inlined but use generic route for better testability */
      cursors_in_trx = cr_tmp;
      cr_fill = 0;
      while (registered)
	{
	  if (registered->itc_position == itc->itc_keep_together_pos)
	    cursors_in_trx[cr_fill++] = registered;
	  registered = registered->itc_next_on_page;
	}
    }
  for (inx = 0; inx < cr_fill; inx++)
    if (cursors_in_trx[inx]
	&& cursors_in_trx[inx]->itc_position == itc->itc_keep_together_pos
	&& cursors_in_trx[inx]->itc_page == itc->itc_keep_together_dp)
      {
	cursors_in_trx[inx]->itc_bp.bp_is_pos_valid = 0; /* set anyway even if not bm inx */
	TC (tc_update_wait_move);
	/* rdbg_printf (("keep together move on %d\n", itc->itc_page)); */
      }
  pg_move_cursors (cursors_in_trx, cr_fill,
		   itc->itc_keep_together_dp, itc->itc_keep_together_pos,
		   itc->itc_page, itc->itc_position,
		   buf);


  itc->itc_keep_together_pos = 0;
}


int
map_entry_after (page_map_t * pm, int at)
{
  int after = PAGE_SZ, inx;
  for (inx = 0; inx < pm->pm_count; inx++)
    {
      short ent = pm->pm_entries[inx];
      if (ent > at && ent < after)
	after = ent;
    }
  return after;
}


extern long  tc_pg_write_compact;
#define WRITE_NO_GAP ((buffer_desc_t *)1)

void
pg_write_compact (it_cursor_t * it, buffer_desc_t * buf_from,
    int insert_to, db_buf_t insert, int split, buffer_desc_t * buf_ext,
    dp_addr_t dp_ext, row_lock_t * new_rl)
{
  page_map_t org_map;
  page_lock_t * pl_ext;
  page_lock_t * pl_from;
  int insert_len = 0;
  int tail = 0, ins_tail = 0; /*leave this much after each/inserted row for expansion */
  db_buf_t ext_page;
  db_buf_t from = buf_from->bd_buffer;
  unsigned char temp[PAGE_SZ];
  dp_addr_t dp_from = it->itc_page;
  it_cursor_t *cursors_in_trx[1000];
  row_lock_t * rlocks [PM_MAX_ENTRIES];
  int rl_fill = 0;
  int cr_fill = 0, inx;
  it_cursor_t *cr;

  page_map_t *map_to = buf_from->bd_content_map;
  db_buf_t page_to = (db_buf_t) & temp;
  int prev_ent = 0;
  long l;
  int place_from = SHORT_REF (from + DP_FIRST);
  int place_to = DP_DATA;
  int is_to_extend = 0;
  int first_copied = 0, last_copied = 0;


  if (insert)
    insert_len = row_length (insert, it->itc_insert_key);

  if (!buf_ext)
    {
      int space = buf_from->bd_content_map->pm_bytes_free - ROW_ALIGN (insert_len);
      int count = buf_from->bd_content_map->pm_count + (insert ? 1 : 0);
      if (space < 0) GPF_T1 ("negfateive space left in pg_write_compact");
      TC (tc_pg_write_compact);
      tail = (space / count) & ~3;  /* round to lowrr multiple of 4 */
      ins_tail = space - count * tail; /* ins tail is in addition to regular tail for the inserted row */
    }
  else if (WRITE_NO_GAP == buf_ext)
    {
      TC (tc_pg_write_compact);
      buf_ext = NULL;
    }
  ext_page = buf_ext ? buf_ext->bd_buffer : NULL;

  ITC_IN_MAP (it);
  pl_ext = buf_ext ? IT_DP_PL (it->itc_tree, buf_ext->bd_page) : NULL;
  pl_from = IT_DP_PL (it->itc_tree, buf_from->bd_page);
  if (pl_from && !PL_IS_PAGE (pl_from) && pl_from->pl_n_row_locks > map_to->pm_count)
    GPF_T1 ("more locks than rows");

  cr = (it_cursor_t *)
      gethash (DP_ADDR2VOID (it->itc_page), it->itc_space->isp_page_to_cursor);
  pl_rlock_table (pl_from, rlocks, &rl_fill);
  for (cr = cr; cr; cr = cr->itc_next_on_page)
    {
      cursors_in_trx[cr_fill++] = cr;
      if (cr_fill >= sizeof (cursors_in_trx) / sizeof (caddr_t))
	GPF_T1 ("too many cursors on splitting page.");
    }
  if (0 == cr_fill)
    {
      ITC_LEAVE_MAP (it);
    }

  memcpy (&org_map, buf_from->bd_content_map, ((ptrlong)(&((page_map_t*)0)->pm_entries)) + 2 * buf_from->bd_content_map->pm_count); /* for debug, copy the entries, the whole struct may overflow addr space. */
  pg_map_clear (buf_from);
  while (1)
    {
      if (place_from >= PAGE_SZ)
	GPF_T;			/*Link over end */
      if (!is_to_extend && place_to >= split)
	{
	  if (buf_ext)
	    {
	      IS_FIRST (place_to, 0);
	      SWITCH_TO_EXT;
	    }
	  else
	    {
	      /* Over or at split, no extend */
	      if (place_to > PAGE_SZ)
		GPF_T;		/* Page overflows, no extend page */
	    }
	}
      if (insert_to == place_from)
	{
	  if (place_to + insert_len > PAGE_SZ)
	    {
	      IS_FIRST (place_to, 0);
	      SWITCH_TO_EXT;
	    }
	  memcpy (&page_to[place_to], insert, insert_len);
	  IS_FIRST (place_to, insert_len);
	  it->itc_position = place_to;
	  if (is_to_extend)
	    {
	      it->itc_page = dp_ext;
	      ITC_IN_MAP (it);
	      it->itc_pl = IT_DP_PL (it->itc_tree, dp_ext);
	    }
	  itc_keep_together (it, is_to_extend ? buf_ext : buf_from,
			     cursors_in_trx, cr_fill);
	  if (new_rl)
	    itc_insert_rl (it, is_to_extend ? buf_ext : buf_from, it->itc_position, new_rl, RL_NO_ESCALATE);
	  ADD_MAP_ENT (place_to, insert_len);
	  LINK_PREV (place_to);
	  place_to += ROW_ALIGN (insert_len) + tail + ins_tail;
	}

      if (!place_from)
	break;			/* The end test. If insert_to == 0
				   the insert is to the end. */
      l = row_reserved_length (&from[place_from], it->itc_insert_key);
      if (place_to + l > PAGE_SZ)
	{
	  IS_FIRST (place_to, 0);
	  SWITCH_TO_EXT;
	}

      IS_FIRST (place_to, (int) (l));
      memcpy (&page_to[place_to], &from[place_from], (int) (l));
      pg_move_lock (it, rlocks, rl_fill, place_from, place_to,
		    is_to_extend ? pl_ext : pl_from, is_to_extend);
      pg_move_cursors (cursors_in_trx, cr_fill, dp_from, place_from,
	  is_to_extend ? dp_ext : dp_from, place_to,
	  is_to_extend ? buf_ext : buf_from);

      ADD_MAP_ENT (place_to, l);
      LINK_PREV (place_to);
      place_to += ROW_ALIGN (l)  + tail;
      place_from = IE_NEXT (from + place_from);
    }
  IS_FIRST (place_to, 0);
  if (!first_copied)
    GPF_T;
  memcpy (from + first_copied, &temp[first_copied],
      MIN (PAGE_SZ, last_copied) - first_copied);
  SHORT_SET (from + DP_FIRST, DP_DATA);
  SHORT_SET (from + DP_RIGHT_INSERTS, 0);
  if (is_to_extend)
    SHORT_SET (page_to + DP_RIGHT_INSERTS, 0);

  for (inx = 0; inx < rl_fill; inx++)
    if (rlocks [inx] != NULL)
      GPF_T1 ("unmoved row lock");
}


buffer_desc_t *
page_rleaf_enter (it_cursor_t * itc, dp_addr_t dp)
{
  buffer_desc_t * buf = NULL;
  ITC_IN_MAP (itc);
  do
    {
      ITC_IN_MAP (itc);
      buf = page_fault (itc, dp);
    }
  while (!buf);
  ITC_IN_MAP (itc);
  return buf;
}


void
page_rleaf_leave (buffer_desc_t * buf)
{
  /* empty - the rleaf entry did not increment read or write count */
}
/*
   When an inner node splits the leaves to the right of the split must be
   updated.
 */

int
pg_reloc_right_leaves (it_cursor_t * it, db_buf_t page, dp_addr_t dp)
{
  int any = 0;
  dp_addr_t leaf;
  int pos = SHORT_REF (page + DP_FIRST);
  buffer_desc_t *buf;
  while (pos)
    {
      leaf = leaf_pointer (page, pos);
      if (leaf)
	{
	  any = 1;
	  ITC_IN_MAP (it);
	  ITC_AGE_TRX (it, 5);
	  buf = page_rleaf_enter (it, leaf);

	  rdbg_printf_2 (("	Parent of %ld from %ld to %ld.\n",
		  buf->bd_page,
		  LONG_REF (buf->bd_buffer + DP_PARENT), dp));

	  if (gethash (DP_ADDR2VOID (buf->bd_page), it->itc_space->isp_remap))
	    {
	      /* This trx has a delta. Set the parent link. */
	      LONG_SET (buf->bd_buffer + DP_PARENT, dp);
	      buf->bd_is_dirty = 1;
	      page_rleaf_leave (buf);
	    }
	  else
	    {
#ifdef DEBUG
	      buffer_desc_t *old_buf = buf;
#endif
	      itc_delta_this_buffer (it, buf, DELTA_STAY_INSIDE);

	      LONG_SET (buf->bd_buffer + DP_PARENT, dp);
	      /* could have been written out in the meantime */
	      buf->bd_is_dirty = 1;
	      page_rleaf_leave (buf);
	    }

	}
      pos = IE_NEXT (page + pos);
    }
  return any;
}


caddr_t
box_n_bin (dtp_t * bin, int len)
{
  caddr_t res = dk_alloc_box (len, DV_BIN);
  memcpy (res, bin, len);
  return res;
}

/* This makes a leaf pointer with
   dv_cont_string, 0 key id, key parts and DV_LEAF to the given leaf.
   Tha page's leftmost entry is copied and returned. It is assumed the page
   has no gap at the start.  */

db_buf_t
itc_make_leaf_entry (it_cursor_t * itc, db_buf_t row, dp_addr_t to)
{
  dbe_col_loc_t * cl;
  int len;
  db_buf_t row_data;
  key_id_t key_id = SHORT_REF (row + IE_KEY_ID);
  dbe_key_t * key = itc->itc_insert_key;
  dtp_t image[PAGE_DATA_SZ];
  db_buf_t res = &image[0];
  int inx = 0, prev_end;
  itc->itc_row_key = key;
  if (!key_id)
    {
      int len = row_length (row, itc->itc_insert_key);
      if (len - IE_LP_FIRST_KEY > MAX_RULING_PART_BYTES)
	GPF_T1 ("leaf pointer too long in copying a leaf pointer in spliting");
      res = (db_buf_t) box_n_bin ((dtp_t *) row, len);
      LONG_SET (res + IE_LEAF, to);
      return res;
    }
  if (KI_LEFT_DUMMY == key_id)
    {
      res = &image[0];
      SHORT_SET (res + IE_NEXT_IE, 0);
      SHORT_SET (res + IE_KEY_ID, KI_LEFT_DUMMY);
      LONG_SET (res + IE_LEAF, to);
      return ((db_buf_t) box_n_bin ((dtp_t *) res, 8));
    }
  /* need the row's key since location of key vars depends on it, could be obsolete row etc */
  if (KI_TEMP != key_id)
    {
      key = sch_id_to_key (wi_inst.wi_schema, key_id);
      itc->itc_row_key = key;
      itc->itc_row_key_id = key->key_id;
    }
  LONG_SET (res, 0);
  LONG_SET (res + IE_LEAF, to);
  res += IE_LP_FIRST_KEY;
  row_data = row + IE_FIRST_KEY;
  if (key->key_key_fixed)
    {
      for (inx = 0; key->key_key_fixed[inx].cl_col_id; inx++)
	{
	  int off;
	  cl = &key->key_key_fixed[inx];
	  off = cl->cl_pos;
	  memcpy (res + off, row_data + off, cl->cl_fixed_len);
	  if (cl->cl_null_mask)
	    res[cl->cl_null_flag] = row_data[cl->cl_null_flag]; /* copy the byte since all parts have their bit copied */
	}
    }
  prev_end = key->key_key_var_start;
  if (key->key_key_var)
    {
      itc->itc_row_key_id = key_id;
      itc->itc_row_data = row_data;
      for (inx = 0; key->key_key_var[inx].cl_col_id; inx++)
	{
	  int off;
	  cl = &key->key_key_var[inx];
	  ITC_COL (itc, (*cl), off, len);
	  memcpy (res + prev_end, row_data + off, len);
	  if (0 == inx)
	    SHORT_SET (res + key->key_length_area, len + prev_end);
	  else
	    SHORT_SET ((res - cl->cl_fixed_len) + 2, len + prev_end);
	  prev_end = prev_end + len;
	  if (cl->cl_null_mask)
	    res[cl->cl_null_flag] = row_data[cl->cl_null_flag]; /* copy the byte since all parts have their bit copied */
	}
    }
  if (prev_end > MAX_RULING_PART_BYTES)
    GPF_T1 ("leaf pointer too long in making a leaf pointer from a row");
  return ((db_buf_t) box_n_bin (&image[0], IE_LP_FIRST_KEY + prev_end));
}


void
itc_split (it_cursor_t * it, buffer_desc_t ** buf_ret, db_buf_t dv,
	   row_lock_t * new_rl)
{
  buffer_desc_t *buf = *buf_ret;
  db_buf_t page = buf->bd_buffer;
  int is_new_root = 0;
  db_buf_t left_leaf = NULL;
  int ext_pos;
  db_buf_t right_leaf = NULL;
  buffer_desc_t *parent;
  dp_addr_t dp_parent = LONG_REF (page + DP_PARENT);
  long right_count = SHORT_REF (page + DP_RIGHT_INSERTS);
  int split = right_count > 5 ? (PAGE_SZ / 100) * 95 : PAGE_SZ / 2;
  buffer_desc_t *extend;
  thread_t *self = THREAD_CURRENT_THREAD;
  if (!(*buf_ret)->bd_is_dirty)
    GPF_T1 ("buffer not marked dirty in split");

  if (THR_IS_STACK_OVERFLOW (self, &buf, SPLIT_STACK_MARGIN))
    GPF_T1 ("out of stack space in itc_split");
  /* Take extend near parent. if root, extend near old root */
  extend = isp_new_page (it->itc_space,
      dp_parent ? dp_parent : buf->bd_page,
      DPF_INDEX, 0, it->itc_n_pages_on_hold);
  if (dp_parent)
    dp_may_compact ((*buf_ret)->bd_storage, dp_parent);

  ITC_IN_MAP (it);
  ITC_MARK_NEW (it);
  if (!extend)
    GPF_T1 ("Out of disk in split");
  itc_split_lock (it, *buf_ret, extend);
  pg_write_compact (it, buf, it->itc_position, dv, split, extend,
      extend->bd_page, new_rl);
  if ((*buf_ret)->bd_pl && PL_IS_PAGE ((*buf_ret)->bd_pl))
    {
      ITC_IN_MAP (it);
  itc_split_lock_waits (it, *buf_ret, extend);
    }
  right_leaf = itc_make_leaf_entry (it, extend->bd_buffer + DP_DATA, extend->bd_page);

  if (!dp_parent)
    {
      /* Root split */

      rdbg_printf_2 (("Root %ld split.\n", buf->bd_page));

      is_new_root = 1;
      ITC_LEAVE_MAP (it);
      parent = isp_new_page (it->itc_space, buf->bd_page, DPF_INDEX, 0, it->itc_n_pages_on_hold);
      dp_parent = parent->bd_page;
      if (!parent)
	GPF_T1 ("Out of disk in root split");
      ITC_IN_MAP (it);
      LONG_SET (buf->bd_buffer + DP_PARENT, dp_parent);
      left_leaf = itc_make_leaf_entry (it, buf->bd_buffer + DP_DATA, buf->bd_page);
      memcpy (parent->bd_buffer + DP_DATA, left_leaf,
	  (int) (box_length ((caddr_t) left_leaf)));
      SHORT_SET (parent->bd_buffer + DP_FIRST, DP_DATA);
      it->itc_space->isp_root = parent->bd_page;
      pg_make_map (parent);
    }
  else
    {
      do
	{
	  ITC_IN_MAP (it);
	  dp_parent = LONG_REF (page + DP_PARENT);
	  page_wait_access (it, dp_parent, NULL, NULL, &parent, PA_WRITE, RWG_NO_WAIT);
	}
      while (it->itc_to_reset > RWG_NO_WAIT);
      ITC_IN_MAP (it);
      itc_delta_this_buffer (it, parent, DELTA_MAY_LEAVE);

    }

  LONG_SET (&extend->bd_buffer[DP_PARENT], parent->bd_page);

  rdbg_printf_2 (("    Node %ld split %s. Extend to %ld , Parent %ld .\n",
      buf->bd_page, (split > PAGE_SZ / 2 ? "R" : ""),
      extend->bd_page, dp_parent));

  /* Now change the parent pointers of leaves to the right of split */
  switch (pg_reloc_right_leaves (it, extend->bd_buffer, extend->bd_page))
    {
    case 0:
      break;
    case -1:
      GPF_T1 ("Out of disk on relock right leaves");
    default:;
#ifdef DEBUG
/*
      dbg_page_map (parent);
      dbg_page_map (buf);
      dbg_page_map (extend);
 */
#endif
    }

  /* Now search for the place to insert the extend. */
  ext_pos = find_leaf_pointer (parent, buf->bd_page, NULL, NULL);
  if (ext_pos <= 0)
    GPF_T;			/* Parent with no ref. to child leaf */

  it->itc_position = ext_pos;
  it->itc_page = parent->bd_page;
  itc_skip_entry (it, parent->bd_buffer);

  /* Position cursor on parent right after this leaf's pointer.
     The extend page leaf entry will come here. */
  ITC_IN_MAP (it);
  page_mark_change (buf, RWG_WAIT_SPLIT);
  page_leave_inner (buf);
  page_leave_inner (extend);

  itc_insert_dv (it, &parent, right_leaf, 1, NULL);

  /* Descending edge. Deallocate and leave pages. */
  if (left_leaf)
    dk_free_box ((caddr_t) left_leaf);
  dk_free_box ((box_t) right_leaf);
}



/* Statistics */
long right_inserts = 0;
long mid_inserts = 0;


/* Insert before entry at <at>, return pos of previous entry,
   0 if this is first */


int
map_insert (page_map_t ** map_ret, int at, int what)
{
  page_map_t * map = *map_ret;
  int inx, prev = 0, tmp;
  int ct = map->pm_count;
  if (map->pm_count == map->pm_size)
    {
      map_resize (map_ret, PM_SIZE (map->pm_size));
      map = *map_ret;
    }

  if (ct > PM_MAX_ENTRIES - 1)
    GPF_T1 ("Exceeded max entries in page map");
  if (at == 0 || ct == 0)
    {

      map->pm_entries[ct] = what;
      map->pm_count = ct + 1;
      if (ct)
	return (map->pm_entries[ct - 1]);
      else
	return 0;
    }
  else
    {
      for (inx = 0; inx < ct; inx++)
	{
	  tmp = map->pm_entries[inx];
	  if (tmp == at)
	    {

	      memmove (&map->pm_entries[inx + 1], &map->pm_entries[inx],
		  sizeof (short) * (ct - inx));

	      map->pm_count = ct + 1;
	      map->pm_entries[inx] = what;
	      return prev;
	    }
	  prev = tmp;
	}
      GPF_T;			/* Insert point not in the map  */
    }
  /*noreturn */
  return 0;
}



#ifdef MTX_DEBUG
void
ins_leaves_check (buffer_desc_t * buf)
{
  /* see if inserting a leaf on a non leaf page */
  int inx;
  page_map_t * map = buf->bd_content_map;
  for (inx = 0; inx < map->pm_count; inx++)
    {
      key_id_t ki = SHORT_REF (buf->bd_buffer + map->pm_entries[inx] + IE_KEY_ID);
      if (!ki || (KI_LEFT_DUMMY == ki && LONG_REF (buf->bd_buffer + map->pm_entries[inx] +IE_LEAF)))
	{
	  printf ("non lea\N");
	  break;
	}
    }
}
#endif

int
itc_insert_dv (it_cursor_t * it, buffer_desc_t ** buf_ret, db_buf_t dv,
    int is_recursive, row_lock_t * new_rl)
{
  buffer_desc_t *buf = *buf_ret;
  page_map_t *map = buf->bd_content_map;
  int  len;
  int pos = it->itc_position, pos_after;
  db_buf_t page = buf->bd_buffer;
  long right_ins = SHORT_REF (page + DP_RIGHT_INSERTS);
  int data_len;
  ITC_LEAVE_MAP (it);
  buf_set_dirty (*buf_ret);
  if (it->itc_position == 0
      && it->itc_insert_key)
    it->itc_insert_key->key_page_end_inserts++;
  len = row_length (dv, it->itc_insert_key);

#ifdef MTX_DEBUG
  if (!is_recursive)
    ins_leaves_check (buf);
#endif
  if (len > MAX_ROW_BYTES
      + 2 /* GK: this is needed bacuse there may be upgrade rows in rfwd */)

    GPF_T1 ("max row length exceeded in itc_insert_dv");

  data_len = ROW_ALIGN (len);

  if (PAGE_SZ - map->pm_filled_to >= data_len)
    {
      int ins_pos, first;
      ins_pos = map->pm_filled_to;
      memcpy (&page[ins_pos], dv, data_len);
      pos_after = map_insert (&buf->bd_content_map, pos, map->pm_filled_to);
      map = buf->bd_content_map;
      map->pm_filled_to += data_len;
      map->pm_bytes_free -= data_len;
      /* Link it. */

      if (pos_after)
	{
	  int next = IE_NEXT (page + pos_after);
	  IE_SET_NEXT (page + ins_pos, next);
	  IE_SET_NEXT (page + pos_after, ins_pos);

	  if (pos_after != SHORT_REF (page + DP_LAST_INSERT))
	    {
	      mid_inserts++;
	      if (right_inserts > 2)
		SHORT_SET (page + DP_RIGHT_INSERTS, 0);
	    }
	  else
	    {
	      right_inserts++;
	      if (right_ins < 1000)
		SHORT_SET ((page + DP_RIGHT_INSERTS), (short)(right_ins + 1));
	    }
	  SHORT_SET (page + DP_LAST_INSERT, ins_pos);
	}
      else
	{

	  first = SHORT_REF (page + DP_FIRST);
	  if (first && first != pos)
	    GPF_T;		/*Bad first of page */
	  IE_SET_NEXT (page + ins_pos, first);
	  SHORT_SET (page + DP_FIRST, ins_pos);
	}

      it->itc_position = ins_pos;
      ITC_IN_MAP (it);
      itc_keep_together (it, buf, NULL, 0);
      if (new_rl)
	itc_insert_rl (it, buf, ins_pos, new_rl, RL_ESCALATE_OK);
      pg_check_map (buf);
      ITC_IN_MAP (it);
      buf_set_dirty (buf);
      page_mark_change (*buf_ret, RWG_WAIT_KEY);
      itc_page_leave (it, *buf_ret);
      return 1;
    }

  /* No contiguous space. See if we compact this or split */

  if (data_len <= map->pm_bytes_free)
    {
      /* No split, compress. */
      pg_check_map (buf);
      pg_write_compact (it, buf, it->itc_position, dv, PAGE_SZ, NULL, 0, new_rl);
      pg_check_map (buf);
      ITC_IN_MAP (it);
      page_mark_change (*buf_ret, RWG_WAIT_KEY);
      itc_page_leave (it, buf);
      return 1;
    }
  /* Time to split */
  if (!is_recursive)
    itc_hold_pages (it, buf, DP_INSERT_RESERVE);
  itc_split (it, buf_ret, dv, new_rl);
  if (!is_recursive)
    itc_free_hold (it);
  return 1;
}


void
itc_make_exact_spec (it_cursor_t * it, db_buf_t thing)
{
#ifdef O12
  O12;
#else
  long len, head_len, key_len, key_head_len;
  int key_pos;
  dbe_key_t *key;
  key_id_t key_id;

  it->itc_specs = NULL;
  /* Make the search spec, search and insert. */
  db_buf_length (thing, &head_len, &len);
  key_id = SHORT_REF (thing + head_len + IE_KEY_ID);
  key = sch_id_to_key (isp_schema (it->itc_space), key_id);
  it->itc_specs = KEY_INSERT_SPEC (key);
  it->itc_insert_key = key;
  key_pos = (int) head_len + 4;
  ITC_START_SEARCH_PARS (it);
  while (thing[key_pos] != DV_DEPENDENT && thing[key_pos] != DV_LEAF)
    {
      ITC_SEARCH_PARAM (it, &thing[key_pos]);

      db_buf_length (&thing[key_pos], &key_head_len, &key_len);
      key_pos += (int) key_head_len + (int) key_len;
    }
#endif
}


/*
   The top level API for inserting an entry. The entry is a regular index
   entry string with:
   DV_xx_CONT_STRING
   key id (1 long)
   n DV_<xx>  entries, one for each key part.
   DV_LEAF or DV_DEPENDENT to mark the dependent part.
   arbitrary binary data up to the length of the cont string.  */

int
itc_insert_unq_ck (it_cursor_t * it, db_buf_t thing, buffer_desc_t ** unq_buf)
{
  row_lock_t * rl_flag = KI_TEMP != it->itc_insert_key->key_id  ? INS_NEW_RL : NULL;
  int res, was_allowed_duplicate = 0;
  buffer_desc_t *buf;

  FAILCK (it);

  if (it->itc_insert_key)
    {
      if (it->itc_insert_key->key_table && it->itc_insert_key->key_is_primary)
	it->itc_insert_key->key_table->tb_count_delta++;
      it->itc_key_id = it->itc_insert_key->key_id;
      /* for key access statistics */
    }
  it->itc_row_key = it->itc_insert_key;
  it->itc_lock_mode = PL_EXCLUSIVE;
  if (SM_INSERT_AFTER == it->itc_search_mode || SM_INSERT_BEFORE == it->itc_search_mode)
    {
      GPF_T1 ("positioned insert is not enabled");
      buf = *unq_buf;
      if (SM_INSERT_AFTER == it->itc_search_mode)
	res = DVC_LESS;
      else 
	res = DVC_GREATER;
      goto searched;
    }
  it->itc_search_mode = SM_INSERT;
 reset_search:
  buf = itc_reset (it);
  res = itc_search (it, &buf);
 searched:
  if (NO_WAIT != itc_insert_lock (it, buf, &res))
    goto reset_search;
 re_insert:
  if (!buf->bd_is_dirty)
    {
      ITC_IN_MAP (it);
      itc_delta_this_buffer (it, buf, DELTA_MAY_LEAVE);
      ITC_LEAVE_MAP (it);
    }
  if (!buf->bd_is_write)
    GPF_T1 ("insert and no write access to buffer");
  switch (res)
    {
    case DVC_INDEX_END:
    case DVC_LESS:
      /* Insert at leaf end. The cursor's position is perfect. */
      itc_skip_entry (it, buf->bd_buffer);
      ITC_AGE_TRX (it, 2);
      itc_insert_dv (it, &buf, thing, 0, rl_flag);
      break;

    case DVC_GREATER:
      /* Before the thing that is at cursor */

      ITC_AGE_TRX (it, 2);
      itc_insert_dv (it, &buf, thing, 0, rl_flag);
      break;

    case DVC_MATCH:

      if (!itc_check_ins_deleted (it, buf, thing))
	{
	  if (unq_buf == UNQ_ALLOW_DUPLICATES || unq_buf == UNQ_SORT)
	    {
	      if (!was_allowed_duplicate && UNQ_SORT == unq_buf)
		{
		  itc_page_leave (it, buf);
		  was_allowed_duplicate = 1;
		  it->itc_search_mode = SM_READ;
		  it->itc_desc_order = 1;
		  goto reset_search;
		}
	      res = DVC_LESS;
	      goto re_insert;
	    }
	  rdbg_printf (("  Non-unq insert T=%d on L=%d pos=%d \n", TRX_NO (it->itc_ltrx), buf->bd_page, it->itc_position));
	  if (unq_buf)
	    {
	      *unq_buf = buf;
	      it->itc_is_on_row = 1;
	      return DVC_MATCH;
	    }
	  if (it->itc_ltrx)
	    {
	      if (it->itc_insert_key)
		{
		  caddr_t detail = dk_alloc_box (50 + MAX_NAME_LEN + MAX_QUAL_NAME_LEN, DV_SHORT_STRING);
		  snprintf (detail, box_length (detail) - 1,
		      "Violating unique index %.*s on table %.*s",
		      MAX_NAME_LEN, it->itc_insert_key->key_name,
		      MAX_QUAL_NAME_LEN, it->itc_insert_key->key_table->tb_name);
		  LT_ERROR_DETAIL_SET (it->itc_ltrx, detail);
		}
	      it->itc_ltrx->lt_error = LTE_UNIQ;
	    }
	  itc_bust_this_trx (it, &buf, ITC_BUST_THROW);
	}
      break;
    default:
      GPF_T1 ("Bad search result in insert");
    }
  KEY_TOUCH (it->itc_insert_key);
  ITC_LEAVE_MAP (it);
  if (KI_TEMP != it->itc_insert_key->key_id)
    {
      lt_rb_insert (it->itc_ltrx, thing);
    }
  if (was_allowed_duplicate)
    return DVC_MATCH;
  return DVC_LESS;		/* insert OK, no duplicate. */
}


db_buf_t
strses_to_db_buf (dk_session_t * ses)
{
  long length = strses_length (ses);
  db_buf_t buffer;
  if (length < 256)
    {
      buffer = (db_buf_t) dk_alloc ((int) length + 2);
      buffer[0] = DV_SHORT_CONT_STRING;
      buffer[1] = (unsigned char) length;
      strses_to_array (ses, (char *) &buffer[2]);
    }
  else
    {
      buffer = (db_buf_t) dk_alloc ((int) length + 5);
      buffer[0] = DV_LONG_CONT_STRING;
      buffer[1] = (dtp_t) (length >> 24);
      buffer[2] = (dtp_t) (length >> 16);
      buffer[3] = (dtp_t) (length >> 8);
      buffer[4] = (dtp_t) length;
      strses_to_array (ses, (char *) &buffer[5]);
    }
  return buffer;
}


int
map_delete (page_map_t ** map_ret, int pos)
{
  page_map_t * map = *map_ret;
  int inx, prev_pos = 0, sz;
  for (inx = 0; inx < map->pm_count; inx++)
    {
      int ent = map->pm_entries[inx];
      if (ent == pos)
	{
	  for (inx = inx; inx < map->pm_count - 1; inx++)
	    {
	      map->pm_entries[inx] = map->pm_entries[inx + 1];
	    }
	  map->pm_count--;
	  sz = PM_SIZE (map->pm_count);
	  if (sz < map->pm_size && map->pm_count < ((sz / 10) * 8))
	    map_resize (map_ret, sz);
	  return prev_pos;
	}
      prev_pos = ent;
    }
  GPF_T;			/* Entry to delete not in the map */
  return 0;
}


#define ITC_ID_TO_KEY(itc, key_id) \
  itc->itc_row_key


void
itc_delete_blobs (it_cursor_t * itc, db_buf_t page)
{
  /* do a round of the row map and delete if you see a blob */
  long pos = itc->itc_position;
  dbe_key_t * key = itc->itc_row_key;
  itc->itc_insert_key = key;
  itc->itc_row_data = page + pos + IE_FIRST_KEY;
  if (key && key->key_row_var)
    {
      int inx;
      for (inx = 0; key->key_row_var[inx].cl_col_id; inx++)
	{
	  dbe_col_loc_t * cl = &key->key_row_var[inx];
	  dtp_t dtp = cl->cl_sqt.sqt_dtp;
	  if (IS_BLOB_DTP (dtp)
	      && 0 == (itc->itc_row_data[cl->cl_null_flag] & cl->cl_null_mask))
	    {
	      int off, len;
	      ITC_COL (itc, (*cl), off, len);
	      dtp = itc->itc_row_data[off];
	      if (IS_BLOB_DTP (dtp))
		{

		  blob_layout_t * bl = bl_from_dv (itc->itc_row_data + off, itc);
		  blob_log_replace (itc, bl);
		  blob_schedule_delayed_delete (itc,
						bl,
						BL_DELETE_AT_COMMIT );
		  /* don't log the del'd blob if it was written by this trx. */
		}
	    }
	}
    }
}


void
itc_immediate_delete_blobs (it_cursor_t * itc, buffer_desc_t * buf)
{
  /* do a round of the row map and delete if you see a blob */
  db_buf_t page = buf->bd_buffer;
  long pos = itc->itc_position;
#if 0
  key_id_t key_id = SHORT_REF (page + pos + IE_KEY_ID);
#endif
  dbe_key_t * key = ITC_ID_TO_KEY (itc, key_id);
  if (key && key->key_row_var)
    {
      int inx;
      for (inx = 0; key->key_row_var[inx].cl_col_id; inx++)
	{
	  dbe_col_loc_t * cl = &key->key_row_var[inx];
	  dtp_t dtp = cl->cl_sqt.sqt_dtp;
	  if (IS_BLOB_DTP (dtp)
	      && 0 == (itc->itc_row_data[cl->cl_null_flag] & cl->cl_null_mask))
	    {
	      int off, len;
	      ITC_COL (itc, (*cl), off, len)
		dtp = page [pos + off];
	      if (IS_BLOB_DTP (dtp))
		{
		  blob_chain_delete (itc, bl_from_dv (page + pos, itc));
		}
	    }
	}
    }
}



void
itc_delete_as_excl (it_cursor_t * itc, buffer_desc_t ** buf_ret, int maybe_blobs)
{
  /* in drop table, no rb possible etc. */
  buffer_desc_t * buf = *buf_ret;
  db_buf_t page = buf->bd_buffer;
  if (!buf->bd_is_write)
    GPF_T1 ("Delete w/o write access");
  ITC_IN_MAP (itc);
  itc_delta_this_buffer (itc, buf, DELTA_MAY_LEAVE);
  ITC_LEAVE_MAP (itc);

  if (!itc->itc_no_bitmap && itc->itc_insert_key && itc->itc_insert_key->key_is_bitmap)
    {
      if (BM_DEL_DONE == itc_bm_delete (itc, buf_ret))
	return;
    }
  if (maybe_blobs)
    {
      itc_delete_blobs (itc, page);
    }
  itc_commit_delete (itc, buf_ret);
}


void
itc_delete (it_cursor_t * itc, buffer_desc_t ** buf_ret, int maybe_blobs)
{
  int pos = itc->itc_position;
  buffer_desc_t * buf = *buf_ret;
  db_buf_t page = buf->bd_buffer;
  if (itc->itc_ltrx->lt_is_excl)
    {
      itc_delete_as_excl (itc, buf_ret, maybe_blobs);
      return;
    }
  if (!buf->bd_is_write)
    GPF_T1 ("Delete w/o write access");
  if (!ITC_IS_LTRX (itc))
    GPF_T1 ("itc_delete outside of commit space");
  lt_rb_update (itc->itc_ltrx, page + pos);
  if (!buf->bd_is_dirty)
    {
  ITC_IN_MAP (itc);
  itc_delta_this_buffer (itc, buf, DELTA_MAY_LEAVE);
  ITC_LEAVE_MAP (itc);
    }
  if (!itc->itc_no_bitmap && itc->itc_insert_key->key_is_bitmap)
    {
      if (BM_DEL_DONE == itc_bm_delete (itc, buf_ret))
	return;
    }
  pl_set_finalize (itc->itc_pl, buf);
  itc->itc_is_on_row = 0;
  if (IE_ISSET (page + pos, IEF_DELETE))
    {
      /* multiple delete possible if several cr's first on row and then do co */
      TC (tc_double_deletes);
    }
  IE_ADD_FLAGS (page + pos, IEF_DELETE);
  ITC_AGE_TRX (itc, 2);
  if (maybe_blobs)
    {
      itc_delete_blobs (itc, page);
    }
}


void
itc_delete_rl_bust (it_cursor_t * itc, int pos)
{
  page_lock_t * pl = itc->itc_pl;
  row_lock_t * rl;
  if (!pl)
    return;
  rl = pl_row_lock_at (itc->itc_pl, pos);
  if (rl)
    rl->rl_pos = 0;
}


long delete_parent_waits;

#ifdef PAGE_TRACE
#define rdbg_printf_m(a) printf a
#else
#define rdbg_printf_m(a)
#endif

buffer_desc_t *
itc_single_leaf_set_parent (it_cursor_t * itc, dp_addr_t leaf, dp_addr_t parent)
{
  buffer_desc_t * buf;
  ITC_IN_MAP (itc);
  buf = page_rleaf_enter (itc, leaf);
  itc_delta_this_buffer (itc, buf, DELTA_MAY_LEAVE);
  ITC_IN_MAP (itc);
  rdbg_printf_m ((" Parent of L=%ld from L=%ld to L=%ld in single leaf delete\n", (long)(buf->bd_page),
		  (long)(LONG_REF (buf->bd_buffer + DP_PARENT)), (long)parent));
  LONG_SET (buf->bd_buffer + DP_PARENT, parent);
  buf->bd_is_dirty = 1;
  page_rleaf_leave (buf);
  ASSERT_IN_MAP (buf->bd_space->isp_tree);
  return buf;
}


int
itc_delete_single_leaf (it_cursor_t * itc, buffer_desc_t ** buf_ret)
{
  buffer_desc_t * buf = *buf_ret;
  db_buf_t page = buf->bd_buffer;
  dp_addr_t leaf = 0, old_dp;
  if (buf->bd_content_map->pm_count == 1)
    {
      int pos = buf->bd_content_map->pm_entries[0];
      key_id_t key_id = SHORT_REF (page + pos + IE_KEY_ID);
      if (0 == key_id || KI_LEFT_DUMMY == key_id)
	leaf = LONG_REF (page + pos + IE_LEAF);
      if (!leaf)
	return 0;
      /*page consists of a single leaf pointer.  Set tree root or go change the leaf pointer on parent */
      {
	dp_addr_t old_leaf = buf->bd_page;
	buffer_desc_t *old_buf = buf;
	buffer_desc_t *parent;
	old_dp = old_buf->bd_page;
	while (1)
	  {
	    dp_addr_t dp_parent;
	    ITC_IN_MAP (itc);
	    dp_parent = LONG_REF (old_buf->bd_buffer + DP_PARENT);
	    if (!dp_parent)
	      {
		buffer_desc_t * leaf_buf;
		/* The page w/ the single leaf is the root */
		rdbg_printf_m (("Single leaf root popped old root L=%ld new root L=%ld \n", (long)(buf->bd_page), (long)leaf));
		ITC_IN_MAP (itc);
		leaf_buf = itc_single_leaf_set_parent (itc, leaf, 0);
		pg_delete_move_cursors (itc, buf->bd_page, 0,
					leaf_buf->bd_page, SHORT_REF (leaf_buf->bd_buffer + DP_FIRST), leaf_buf);

		isp_free_page (itc->itc_space, buf);
		ITC_IN_MAP (itc);
		itc->itc_tree->it_commit_space->isp_root = leaf;
		do
		  {
		    ITC_IN_MAP (itc);
		    page_wait_access (itc, itc->itc_tree->it_commit_space->isp_root, NULL, NULL, buf_ret, PA_WRITE, RWG_WAIT_SPLIT);
		  } while (itc->itc_to_reset >= RWG_WAIT_SPLIT);
		itc->itc_page = (*buf_ret)->bd_page;
		ITC_LEAVE_MAP (itc);
		return 1;
	      }

	    page_wait_access (itc, dp_parent, NULL, NULL, &parent, PA_WRITE, RWG_WAIT_KEY);
	    if (itc->itc_to_reset <= RWG_WAIT_KEY)
	      break;
	    TC (tc_delete_parent_waits);
	  }

	ITC_IN_MAP (itc);
	buf = parent;
	itc->itc_page = buf->bd_page;
	pos = find_leaf_pointer (buf, old_leaf, NULL, NULL);
	if (!pos)
	  GPF_T1 ("No leaf in super noted in popping a single child non-leaf");
	itc->itc_position = pos;
	itc->itc_is_on_row = 1;


	ITC_IN_MAP (itc);
	pg_delete_move_cursors (itc, old_buf->bd_page, 0,
				buf->bd_page, pos, buf);

	page_mark_change (old_buf, RWG_WAIT_SPLIT);
	pl_page_deleted (IT_DP_PL (itc->itc_tree, old_buf->bd_page), old_buf);
	itc_single_leaf_set_parent (itc, leaf, buf->bd_page);
	isp_free_page (itc->itc_space, old_buf); /*incl. leave buffer */
	itc_delta_this_buffer (itc, buf, DELTA_MAY_LEAVE);
	page_mark_change (buf, RWG_WAIT_KEY);
	buf_set_dirty_inside (buf);
	*buf_ret = buf;
	rdbg_printf_m ((" Single child page L=%ld popped parent L=%ld leaf L=%ld \n", (long)old_dp, (long)(buf->bd_page), (long)leaf));
	LONG_SET (buf->bd_buffer + pos + IE_LEAF, leaf);
	ITC_LEAVE_MAP (itc);
	return 1;
      }
    }
  return 0;
}


int
itc_commit_delete (it_cursor_t * it, buffer_desc_t ** buf_ret)
{
  /* Delete whatever the cursor is on. The cursor will be at the next entry */

  buffer_desc_t *buf = *buf_ret;
  long len;
  db_buf_t page;
  page_map_t *map;
  int pos, prev_pos = 0, new_pos;

  if (!buf->bd_is_write)
    GPF_T;			/* Delete when cursor not on row */

delete_from_cursor:
  map = buf->bd_content_map;
  page = buf->bd_buffer;
  pos = it->itc_position;

  len = row_reserved_length (page + pos, buf->bd_space->isp_tree->it_key);
  prev_pos = map_delete (&buf->bd_content_map, pos);
  map = buf->bd_content_map;
  new_pos = IE_NEXT (page + pos);
  if (prev_pos)
    {
      IE_SET_NEXT (page + prev_pos, new_pos);
    }
  else
    {
      SHORT_SET (page + DP_FIRST, new_pos);
    }

  if (!buf->bd_is_dirty)
    {
      ITC_LEAVE_MAP (it);
      buf_set_dirty (buf);
    }
  /* Did it go empty ? */
  ITC_IN_MAP (it);
  pg_delete_move_cursors (it, it->itc_page, it->itc_position,
      it->itc_page, new_pos, NULL);
  it->itc_position = new_pos;
  map->pm_bytes_free += (short) ROW_ALIGN (len);
  if (map->pm_bytes_free > (PAGE_DATA_SZ * 2) / 3)  /* if less than 2/3 full */
    dp_may_compact ((*buf_ret)->bd_storage, LONG_REF ((*buf_ret)->bd_buffer + DP_PARENT));
  if (itc_delete_single_leaf (it, buf_ret))
    return DVC_MATCH;
  if (0 == SHORT_REF (page + DP_FIRST))
    {
      dp_addr_t old_leaf = buf->bd_page;
      buffer_desc_t *old_buf = buf;
      buffer_desc_t *parent;
      if (map->pm_bytes_free != PAGE_SZ - DP_DATA)
	GPF_T;			/* Bad free count */
      rdbg_printf_2 (("    Deleting page L=%d\n", old_buf->bd_page));
      while (1)
	{
	  dp_addr_t dp_parent;
	  ITC_IN_MAP (it);
	  dp_parent = LONG_REF (old_buf->bd_buffer + DP_PARENT);
	  if (!dp_parent)
	    {
	      /* The root went empty */
	      pg_map_clear (buf);
	      *buf_ret = buf;
	      *buf_ret = buf;
	      ITC_LEAVE_MAP (it);
	      return DVC_MATCH;
	    }

	  page_wait_access (it, dp_parent, NULL, NULL, &parent, PA_WRITE, RWG_WAIT_KEY);
	  if (it->itc_to_reset <= RWG_WAIT_KEY)
	    break;
	  TC (tc_delete_parent_waits);
	}

      ITC_IN_MAP (it);
      buf = parent;
      it->itc_page = buf->bd_page;
      pos = find_leaf_pointer (buf, old_leaf, NULL, NULL);
      if (pos <= 0)
	GPF_T;			/* No leaf pointer in parent in delete */
      it->itc_position = pos;
      it->itc_is_on_row = 1;

      /* The cursor is now in on the parent. Delete on away. */
      ITC_IN_MAP (it);
      pg_delete_move_cursors (it, old_buf->bd_page, 0,
	  buf->bd_page, pos, buf);

      page_mark_change (old_buf, RWG_WAIT_SPLIT);
      pl_page_deleted (IT_DP_PL (it->itc_tree, old_buf->bd_page), old_buf);
      DBG_PT_PRINTF (("  Found leaf ptr for delete of L=%d on L=%d \n", old_buf->bd_page, buf->bd_page));
      isp_free_page (it->itc_space, old_buf); /*incl. leave buffer */
      itc_delta_this_buffer (it, buf, DELTA_MAY_LEAVE);
      *buf_ret = buf;
      goto delete_from_cursor;
    }
  itc_delete_rl_bust (it, pos);
  *buf_ret = buf;
  return DVC_MATCH;
}




dp_addr_t 
ie_leaf (db_buf_t row)
{
  key_id_t k = SHORT_REF (row + IE_KEY_ID);
  if (!k || KI_LEFT_DUMMY == k)
    return LONG_REF (row + IE_LEAF);
  else
    return 0;
}

typedef struct page_rel_s
{
  short		pr_lp_pos;
  dp_addr_t	pr_dp;
  buffer_desc_t *	pr_buf;
  short		pr_old_fill;
  short 		pr_new_fill;
  short		pr_old_lp_len;
  short		pr_new_lp_len;
  short		pr_deleted;
  buffer_desc_t *	pr_new_buf;
  db_buf_t		pr_leaf_ptr;
} page_rel_t;

#define MAX_CP_BATCH (PAGE_DATA_SZ / 12)
#define CP_CHANGED 2
#define CP_STILL_INSIDE 1
#define CP_REENTER 0


#define START_PAGE(pr) \
{  \
  prev_ent = 0; \
  pg_fill = DP_DATA; \
  pr->pr_new_buf = buffer_allocate (DPF_INDEX); \
  page_to = pr->pr_new_buf->bd_buffer; \
}


#ifdef MTX_DEBUG
#define cmp_printf(a) printf a
#else
#define cmp_printf(a) 
#endif

int
it_compact (index_tree_t *it, buffer_desc_t * parent, page_rel_t * pr, int pr_fill, int target_fill, int *pos_ret)
{
  /* the pr's are filled, move the data */
  it_cursor_t itc_auto;
  page_map_t * pm;
  it_cursor_t * itc = NULL;
  page_rel_t * target_pr;
  db_buf_t page_to;
  int n_target_pages, pg_fill, n_del = 0, n_ins = 0, n_leaves = 0;
  int prev_ent = 0, org_count = parent->bd_content_map->pm_count;
  int inx, first_after, first_lp_inx;
  target_pr = &pr[0];
  n_target_pages = 1;
  START_PAGE (target_pr);
  for (inx = 0; inx < pr_fill; inx++)
    {
      buffer_desc_t * buf = pr[inx].pr_buf;
      db_buf_t page = buf->bd_buffer;
      int pos = SHORT_REF (buf->bd_buffer + DP_FIRST);
      if (!pr[inx].pr_deleted)
	n_ins++;
      else 
	n_del++;
      while (pos)
	{
	  int len = ROW_ALIGN (row_length (page + pos, it->it_key));
	  if (pg_fill + len < target_fill)
	    {
	      memcpy (page_to + pg_fill, buf->bd_buffer + pos, len);
	      LINK_PREV (pg_fill);
	      pg_fill += len;
	      pos = IE_NEXT (page + pos);
	      continue;
	    }
	  if (pg_fill < target_fill && pg_fill + len < PAGE_SZ)
	    {
	      memcpy (page_to + pg_fill, buf->bd_buffer + pos, len);
	      LINK_PREV (pg_fill);
	      pg_fill += len;
	      pos = IE_NEXT (page + pos);
	      continue;
	    }
	  if (pg_fill != target_pr->pr_new_fill)
	    GPF_T1 ("different page fills in compact check and actual compact");
	  target_pr++;
	  START_PAGE (target_pr);
	  memcpy (page_to + pg_fill, buf->bd_buffer + pos, len);
	  LINK_PREV (pg_fill);
	  pg_fill += len;
	  pos = IE_NEXT (page + pos);
	}
    }
  if (pg_fill != target_pr->pr_new_fill)
    GPF_T1 ("different page fills on compact check and compact");
  /* update the leaf pointers on the parent page */
  pm = parent->bd_content_map;
  for (inx = 0; inx < pm->pm_count; inx++)
    {
      if (pm->pm_entries[inx] == pr[0].pr_lp_pos)
	{
	  first_lp_inx = inx;
	  break;
	}
    }
  if (0 == first_lp_inx)
    SHORT_SET (parent->bd_buffer + DP_FIRST, IE_NEXT (parent->bd_buffer + pr[pr_fill-1].pr_lp_pos));
  else
    IE_SET_NEXT (parent->bd_buffer + pm->pm_entries[first_lp_inx - 1], IE_NEXT (parent->bd_buffer + pr[pr_fill-1].pr_lp_pos));
  itc = &itc_auto;
  ITC_INIT (itc, NULL, NULL);
  itc_from_it (itc, it);
  {
    int dp_first = SHORT_REF (parent->bd_buffer + DP_FIRST);
    int pos = dp_first;
    int new_count = 0;
    while (pos)
      {
	new_count++;
	pos = IE_NEXT (parent->bd_buffer + pos);
      }
    if (org_count - pr_fill != new_count)
      GPF_T1 ("bad unlink of compacted in compact");
    pg_write_compact (itc, parent, -1, NULL,  0, WRITE_NO_GAP, 0, NULL);
    /* if the page is empty, pg_write_compact erroneously puts 20 as the first row instead of 0 */
    if (!dp_first)
      SHORT_SET (parent->bd_buffer + DP_FIRST, 0);
    pg_make_map (parent);
    if (parent->bd_content_map->pm_count != org_count - pr_fill)
      GPF_T1 ("bad quantity deleted from the parent map in compact");
  }
  pm = parent->bd_content_map;
  page_to = parent->bd_buffer;
  prev_ent = first_lp_inx ? pm->pm_entries[first_lp_inx - 1] : 0;
  first_after= prev_ent ? IE_NEXT (parent->bd_buffer + prev_ent) : SHORT_REF (parent->bd_buffer + DP_FIRST);
  pg_fill = ROW_ALIGN (pm->pm_filled_to);
  for (inx = 0; inx < pr_fill; inx++)
    {
      if (pr[inx].pr_deleted)
	break;
      memcpy (page_to + pg_fill, pr[inx].pr_leaf_ptr, pr[inx].pr_new_lp_len);
      LINK_PREV_NC (pg_fill);
      pg_fill += pr[inx].pr_new_lp_len;
    }
  IE_SET_NEXT (parent->bd_buffer + prev_ent, first_after);
  *pos_ret = prev_ent;
  pg_make_map (parent);
  /* delete the pages that are not needed */
        ITC_IN_MAP (itc);
  for (inx = 0; inx < pr_fill; inx++)
    {
      itc_delta_this_buffer (itc, pr[inx].pr_buf, 1);
      if (pr[inx].pr_deleted)
	{
	  rdbg_printf_2 (("D=%d ", pr[inx].pr_buf->bd_page));
	  isp_free_page (it->it_commit_space, pr[inx].pr_buf);
	}
      else 
	{
	  rdbg_printf_2 (("W=%d ", pr[inx].pr_buf->bd_page));
		  	  memcpy (pr[inx].pr_buf->bd_buffer + DP_DATA, pr[inx].pr_new_buf->bd_buffer + DP_DATA, pr[inx].pr_new_fill - DP_DATA);
	  SHORT_SET (pr[inx].pr_buf->bd_buffer + DP_FIRST, SHORT_REF (pr[inx].pr_new_buf->bd_buffer + DP_FIRST));
	  pg_make_map (pr[inx].pr_buf);
	  n_leaves += pr[inx].pr_buf->bd_content_map->pm_count;
	  page_mark_change (pr[inx].pr_buf, RWG_WAIT_SPLIT);
	  page_leave_inner (pr[inx].pr_buf);
	}
    }
  page_mark_change (parent, RWG_WAIT_SPLIT);
  cmp_printf (("  Compacted %d pages to %d under %ld first =%d, org count=%d\n", pr_fill, pr_fill - n_del, parent->bd_page, first_lp_inx, org_count));
  if (parent->bd_content_map->pm_count != org_count - n_del )
    GPF_T1 ("mismatch of leaves before and after compact");
  return n_leaves;
}

void
pr_free (page_rel_t * pr, int pr_fill)
{
  int inx, leave = 1;
  for (inx = 0; inx < pr_fill; inx++)
    {
      dk_free_box (pr[inx].pr_leaf_ptr);
      if (pr[inx].pr_new_buf)
	{
	  dk_free (pr[inx].pr_new_buf->bd_buffer, -1);
	  dk_free ((caddr_t) pr[inx].pr_new_buf, sizeof (buffer_desc_t));
	}
      if (leave && pr[inx].pr_buf->bd_is_write)
	page_leave_inner (pr[inx].pr_buf);
    }
}


extern long ac_pages_in;
extern long ac_pages_out;
extern long ac_n_busy;




int
it_try_compact (index_tree_t *it, buffer_desc_t * parent, page_rel_t * pr, int pr_fill, int * pos_ret, int mode)
{
  /* look at the pr's and see if can rearrange so as to save one or more pages.
   * if so, verify therearrange and update the pr array. */
  it_cursor_t itc_auto;
  it_cursor_t * itc = NULL;
  page_rel_t * target_pr;
  int total_fill = 0, n_target_pages, pg_fill, n_leaves = 0, n_leaves_2;
  int olp_sum = 0, nlp_sum = 0;
  int target_fill, est_res_pages;
  int inx;
  int n_source_pages = 0;
  for (inx = 0; inx < pr_fill; inx++)
    {
      page_map_t * pm =  pr[inx].pr_buf->bd_content_map;
      int dp_fill = PAGE_DATA_SZ - pm->pm_bytes_free;
      total_fill += dp_fill;
      n_source_pages++;
    }
  est_res_pages = ((total_fill + (total_fill / 12)) / PAGE_DATA_SZ) + 1;
  if (est_res_pages>= n_source_pages)
    return CP_STILL_INSIDE;
  for (inx = 0; inx < pr_fill; inx++)
    {
      BD_SET_IS_WRITE (pr[inx].pr_buf, 1);
      n_leaves += pr[inx].pr_buf->bd_content_map->pm_count;
    }
  LEAVE_PAGE_MAP (it);
  /* could be savings.  Make precise relocation calculations */
  target_fill = MIN (PAGE_SZ, DP_DATA + (total_fill / est_res_pages));
  target_pr = &pr[0];
  n_target_pages = 1;
  pg_fill = DP_DATA;
  itc = &itc_auto;
  ITC_INIT (itc, NULL, NULL);
  itc_from_it (itc, it);

  for (inx = 0; inx < pr_fill; inx++)
    {
      buffer_desc_t * buf = pr[inx].pr_buf;
      db_buf_t page = buf->bd_buffer;
      int pos = SHORT_REF (buf->bd_buffer + DP_FIRST);
      if (!pos)
	{
	  log_error ("Unexpected empty db %d in compact", buf->bd_page);
	  return CP_REENTER;
	}
      if (0 == inx)
	{
	  target_pr->pr_leaf_ptr = itc_make_leaf_entry (itc, page+pos, target_pr->pr_dp);
	  target_pr->pr_new_lp_len = ROW_ALIGN (box_length (target_pr->pr_leaf_ptr));
	}
      while (pos)
	{
	  dp_addr_t leaf = ie_leaf (page + pos);
	  int len = ROW_ALIGN (row_length (page + pos, it->it_key));
	  if (leaf)
	    return CP_REENTER; /* do not compact inner pages, would have to relocate parent dp's of children  */
	  if (pg_fill + len < target_fill)
	    {
	      pg_fill += len;
	      pos = IE_NEXT (page + pos);
	      continue;
	    }
	  if (pg_fill < target_fill && pg_fill + len < PAGE_SZ)
	    {
	      pg_fill += len;
	      pos = IE_NEXT (page + pos);
	      continue;
	    }
	  target_pr->pr_new_fill = pg_fill;
	  if (++n_target_pages == pr_fill)
	    return CP_REENTER; /* as many pages in result and source */
	  target_pr++;
	  pg_fill = DP_DATA + len;
	  target_pr->pr_leaf_ptr = itc_make_leaf_entry (itc, page+pos, target_pr->pr_dp);
	  target_pr->pr_new_lp_len = ROW_ALIGN (box_length (target_pr->pr_leaf_ptr));
	  pos = IE_NEXT (page + pos);
	}
    }
  target_pr->pr_new_fill = pg_fill;
  for (inx = 0; inx < pr_fill; inx++)
    {
      nlp_sum += pr[inx].pr_new_lp_len;
      olp_sum += pr[inx].pr_old_lp_len;
    }
  if (nlp_sum - olp_sum >= ROW_ALIGN (parent->bd_content_map->pm_bytes_free))
    {
      fprintf (stderr, "nlp_sum - olp_sum > pm_bytes_free\n");
      return CP_REENTER; /* the new leaf pointers would not fit on parent.  Do nothing */
    }
  /* can compact now.  Will be at least 1 page shorter */
  ac_pages_in += pr_fill;
  ac_pages_out += n_target_pages;
  for (inx = n_target_pages; inx < pr_fill; inx++)
    {
      pr[inx].pr_deleted = 1;
    }
  n_leaves_2 = it_compact (it, parent, pr, pr_fill, target_fill, pos_ret);
  if (n_leaves != n_leaves_2)
    GPF_T1 ("compact ends up with different leaf counts before and after");
  return CP_CHANGED;
}



#define CHECK_COMPACT \
{  \
  if (pr_fill > 1) \
    {\
      compact_rc = it_try_compact (it, parent, pr, pr_fill, &pos, mode);	\
      if (CP_REENTER == compact_rc) \
	IN_PAGE_MAP (it); \
      ASSERT_IN_MAP (it); \
      pr_free (pr, pr_fill); \
      if (CP_CHANGED == compact_rc) \
        any_change = CP_CHANGED; \
    } \
  pr_fill = 0; \
}


#define BUF_COMPACT_READY(buf, leaf, it) \
  (buf && !buf->bd_is_write && !buf->bd_readers && buf->bd_is_dirty \
   && !buf->bd_being_read && !buf->bd_to_bust && !buf->bd_write_waiting && !buf->bd_waiting_read \
   && !gethash ((void*)(ptrlong)leaf, it->it_commit_space->isp_page_to_cursor) \
   && !gethash ((void*)(ptrlong)leaf, it->it_locks)) 


#define BUF_COMPACT_ALL_READY(buf, leaf, it) \
  (buf && !buf->bd_is_write && !buf->bd_readers  \
   && !buf->bd_being_read && !buf->bd_to_bust && !buf->bd_write_waiting && !buf->bd_waiting_read \
   && !gethash ((void*)(ptrlong)leaf, it->it_commit_space->isp_page_to_cursor) \
   && !gethash ((void*)(ptrlong)leaf, it->it_locks)) 



#define COMPACT_DIRTY 0
#define COMPACT_ALL 1

int
it_cp_check_node (index_tree_t *it, buffer_desc_t *parent, int mode)
{
  page_rel_t pr[MAX_CP_BATCH];
  int pr_fill = 0, pos, any_change = 0, compact_rc;
  db_buf_t page = parent->bd_buffer;
  ASSERT_IN_MAP (it);
  parent->bd_is_write = 1;
  /* loop over present and dirty children, find possible sequences to compact. Rough check first. */
  pos = SHORT_REF (parent->bd_buffer + DP_FIRST);
  while (pos)
    {
      dp_addr_t leaf = ie_leaf (page + pos);
      if (leaf)
	{
	  buffer_desc_t * buf = (buffer_desc_t*) gethash ((void*)(ptrlong) leaf, it->it_commit_space->isp_dp_to_buf);
	  if (COMPACT_ALL == mode ? BUF_COMPACT_ALL_READY (buf, leaf, it) : BUF_COMPACT_READY (buf, leaf, it))
	    {
	      if (mode == COMPACT_DIRTY && !gethash ((void*)(void*)(ptrlong)leaf, it->it_commit_space->isp_remap))
		GPF_T1 ("In compact, no remap dp fpr a dirty buffer");
	      memset (&pr[pr_fill], 0, sizeof (page_rel_t));
	      pr[pr_fill].pr_buf = buf;
	      pr[pr_fill].pr_dp = leaf;
	      pr[pr_fill].pr_old_lp_len = ROW_ALIGN(row_length (page + pos, it->it_key));
	      pr[pr_fill].pr_lp_pos = pos;
	      pr_fill++;
	    }
	  else
	    {
	      CHECK_COMPACT;
	    }
	}
      else 
	{
	  CHECK_COMPACT;
	}
      if (!pos)
	break;
      pos = IE_NEXT (page + pos);
    }
  CHECK_COMPACT;
  if (CP_CHANGED == any_change)
    parent->bd_is_dirty = 1;
  if (COMPACT_DIRTY == mode)
  page_leave_inner (parent);
  return any_change;
}

#define DP_VACUUM_RESERVE ((PAGE_DATA_SZ / 12) + 1) /* max no of leaf pointers + parent */


void 
itc_vacuum_compact (it_cursor_t * itc, buffer_desc_t * buf)
{
  if (gethash ((void*)(ptrlong) itc->itc_page, itc->itc_space->isp_page_to_cursor)
      || itc->itc_pl
      || buf->bd_to_bust || buf->bd_write_waiting)

  itc_hold_pages (itc, buf, DP_VACUUM_RESERVE);
  ITC_IN_MAP (itc);
  if (CP_CHANGED == it_cp_check_node (itc->itc_space->isp_tree, buf, COMPACT_ALL))
    {
      itc_delta_this_buffer (itc, buf, 0);
    }
  ITC_LEAVE_MAP (itc);
  itc_free_hold (itc);
}


dk_hash_t * dp_compact_checked;
dk_mutex_t * dp_compact_mtx;

void
dp_may_compact (dbe_storage_t *dbs, dp_addr_t dp)
{
  mutex_enter (dp_compact_mtx);
  remhash ((void*)(ptrlong)dp, dp_compact_checked);
  mutex_leave (dp_compact_mtx);
}


int
dp_is_compact_checked (dbe_storage_t * dbs, dp_addr_t dp)
{
  int rc;
  mutex_enter (dp_compact_mtx);
  rc = (int)(ptrlong) gethash ((void*)(ptrlong)dp, dp_compact_checked);
  if (!rc)
    sethash ((void*)(ptrlong) dp, dp_compact_checked, (void*) 1);
  mutex_leave (dp_compact_mtx);
  return rc;
}



void
it_check_compact (index_tree_t * it, int age_limit)
{
  int rc;
  void *dp;
  buffer_desc_t *buf, *parent;
  dk_hash_iterator_t hit;
  /* rough check for possible activity.  Can do outside map */
  if (it->it_commit_space->isp_dp_to_buf->ht_count < 50
      || it->it_commit_space->isp_remap->ht_count < 50)
    return;
  IN_PAGE_MAP (it);
 again:
  ASSERT_IN_MAP (it);
  dk_hash_iterator (&hit, it->it_commit_space->isp_dp_to_buf);
  while (dk_hit_next (&hit, &dp, (void**) &buf))
    {
      if (buf->bd_pool && buf->bd_pool->bp_ts - buf->bd_timestamp >= age_limit)
	{
	  dp_addr_t parent_dp = LONG_REF (buf->bd_buffer + DP_PARENT);
	  parent = (buffer_desc_t *) gethash ((void*)(ptrlong) parent_dp,  it->it_commit_space->isp_dp_to_buf);
	  if (BUF_COMPACT_READY (parent, parent_dp, it))
	    {
	      if (!dp_is_compact_checked (parent->bd_storage, parent->bd_page))
		{
		  rc = it_cp_check_node (it, parent, COMPACT_DIRTY);
		  if (CP_CHANGED == rc)
		    goto again;
		}
	    }
	}
    }
  LEAVE_PAGE_MAP (it);
}


void
wi_check_all_compact (int age_limit)
{
  /*  call before writing old dirty out. Also before pre-checkpoint flush of all things.  */
  dbe_storage_t * dbs = wi_inst.wi_master;
#ifndef AUTO_COMPACT
  return;
#endif 
  if (!dbs)
    return; /* atthe very start of init*/
  DO_SET (index_tree_t *, it, &dbs->dbs_trees)
    {
      it_check_compact (it, age_limit);
    }
  END_DO_SET();
}

