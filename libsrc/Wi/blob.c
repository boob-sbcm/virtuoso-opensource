/*
 *  blob.c
 *
 *  $Id$
 *
 *  BLOBS
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

/*
   Edits
   Apr 17, 97 - added check to blob_write_log to see if page is actually a blob.
 */

#include "sqlnode.h"
#include "sqlfn.h"
#include "multibyte.h"
#include "srvmultibyte.h"
#include "sqltype.h"
#if !defined (__APPLE__)
#include <wchar.h>
#endif
#include "bif_xper.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "langfunc.h"
#ifdef __cplusplus
}
#endif
#include "sqlbif.h"

unsigned blob_page_dir_threshold = BLOBDIR_DEFAULT_THRESHOLD;
#if 0
static long blob_fill_buffer_from_wide_string (caddr_t bh, caddr_t buf, int *at_end, long *char_len);
#endif



typedef enum
  {
    vBINARY, vNARROW, vWIDE, vUTF
  } blob_cvt_type_t;

typedef int (*get_data_generic_proc)(void *arg, unsigned char *buf, int from, int len);
static int wide_blob_buffered_read (void *ses_from, unsigned char* to, int req_chars, blob_state_t * state, int * page_end, int *preadchars, get_data_generic_proc get_data_proc);

static int
bh_fill_page_generic (
	blob_handle_t *bh,
	wcharset_t *charset,
	blob_cvt_type_t in,
	blob_cvt_type_t out,
	unsigned char *buffer,
	int maxin,
	int maxout,
	int *nin,
	int *noutbytes,
	int *noutchars,
	get_data_generic_proc get_data_proc,
	void *proc_arg,
	int pos
	)
{
  assert (vWIDE != out);
  if (0 == maxin)
    {
      *nin = 0;
      *noutbytes = 0;
      *noutchars = 0;
      return 0;
    }
  switch (in) {
    case vBINARY:
    case vNARROW:
      {
	switch (out) {
	  case vBINARY:
	  case vNARROW:
	    {
	      int len = 0;
	      if ((NULL == charset) ||
		(NULL == default_charset) ||
		(charset == default_charset) ||
		(vBINARY == out) ||
		(vBINARY == in))
		{
		  len = get_data_proc (proc_arg, buffer, pos, MIN (maxout, maxin));
		  *nin = len;
		  *noutbytes = len;
		  *noutchars = len;
		}
	      else
		{
		  int i;
		  size_t lt;
		  unsigned char cin;
		  unsigned char ctmp[VIRT_MB_CUR_MAX];

		  len = MIN (maxout, maxin);
		  for (i=0; i < len;  i++)
		    {
		      if (0 == get_data_proc (proc_arg, &cin, pos + i, 1))
			break;
		      lt = cli_narrow_to_utf8 (charset, &cin, 1, ctmp, VIRT_MB_CUR_MAX);
		      cli_utf8_to_narrow (default_charset, ctmp, lt, (unsigned char *) (buffer + i), 1);
		      (*nin) ++;
		      (*noutbytes) ++;
		      (*noutchars) ++;
		    }
		}
	      return 0 == maxout;
	    }
	  case vUTF:
	    {
	      int len = 0, oblom = 0;
	      *nin = 0;
	      *noutbytes = 0;
	      *noutchars = 0;
	      while (maxout >= VIRT_MB_CUR_MAX && maxin > 0)
		{
		  unsigned char cin;
		  if (0 == get_data_proc (proc_arg, &cin, pos + *nin, 1))
		    break;
		  (*nin) ++;
		  maxin --;
		  if (oblom)
		    continue;
		  if ('\0' == cin)
		    {
		      oblom = 1;
		      continue;
		    }
		  len = (int) cli_narrow_to_utf8 ((vBINARY == out)?NULL:charset, &cin, 1, (unsigned char*)(buffer + *noutbytes), VIRT_MB_CUR_MAX);
		  assert (len>0);
		  maxout -= len;
		  (*noutbytes) += len;
		  (*noutchars) ++;
		}
	      if (maxout < VIRT_MB_CUR_MAX)
		return 1;
	      break;
	    }
	  case vWIDE:
	    assert (0);
	};
	break;
      }
    case vWIDE:
      {
	switch (out) {
	  case vBINARY:
	  case vNARROW:
	    {
	      int len = 0, oblom = 0;
	      while (maxout > 0 && maxin >= sizeof(wchar_t))
		{
		  wchar_t cin;
		  if (0 == get_data_proc (proc_arg, (unsigned char*)&cin, pos + *nin, sizeof(wchar_t)))
		    break;
		  maxin -= sizeof(wchar_t);
		  (*nin) += sizeof(wchar_t);
		  if (oblom)
		    continue;
		  if (0 == cin)
		    {
		      oblom = 1;
		      continue;
		    }
		  len = (int) cli_wide_to_narrow (((vBINARY == out)?NULL:charset),
						0, &cin, 1, (unsigned char*)buffer + *noutbytes, 1, NULL, NULL);
		  assert (len>0);
		  maxout --;
		  (*noutbytes) ++;
		  (*noutchars) ++;
		}
	      return 0 == maxout;
	    }
	  case vUTF:
	    {
	      int len = 0, oblom = 0;
	      while (maxout >= VIRT_MB_CUR_MAX && maxin >= sizeof(wchar_t))
		{
		  wchar_t cin;
		  if (0 == get_data_proc (proc_arg, (unsigned char*)&cin, pos + *nin, sizeof(wchar_t)))
		    break;
		  maxin -= sizeof(wchar_t);
		  (*nin) += sizeof(wchar_t);
		  if (oblom)
		    continue;
		  if (0 == cin)
		    {
		      oblom = 1;
		      continue;
		    }
		  len = (int) cli_wide_to_narrow (CHARSET_UTF8,
						0, &cin, 1, (unsigned char*)(buffer + *noutbytes), VIRT_MB_CUR_MAX, NULL, NULL);
		  assert (len>0);
		  maxout -= len;
		  (*noutbytes) += len;
		  (*noutchars) ++;
		}
	      if (maxout < VIRT_MB_CUR_MAX)
		return 1;
	      break;
	    }
	  case vWIDE:
	    assert (0);
	};
	break;
      }
    case vUTF:
      {
	switch (out) {
	  case vBINARY:
	  case vNARROW:
	    {
	      int page_end = 0, oblom = 0;
	      int readbytes = 0;
	      *nin = 0;
	      *noutbytes = 0;
	      *noutchars = 0;
	      while (maxout > 0 && maxin > 0)
		{
		  unsigned char cin[VIRT_MB_CUR_MAX];
		  int nl, len;
		  page_end = 0;
		  readbytes = wide_blob_buffered_read (proc_arg, cin, 1, &bh->bh_state, &page_end, &len, get_data_proc);
		  nl = bh->bh_state.count;
		  if (!page_end)
		    {
		      *((char *) buffer + *noutbytes) = cin[0];
		      maxout --;
		      (*noutbytes) ++;
		      (*noutchars) ++;
		      (*nin) ++;
		      maxin --;
		      continue;
		    }
		  if (maxin < nl + 1)
		    break;
		  wide_blob_buffered_read (proc_arg, cin, nl + 1, &bh->bh_state, &page_end, &len, get_data_proc);

		  maxin -= nl + 1;
		  (*nin) += nl + 1;
		  if (oblom)
		    continue;
		  if ('\0' == cin[0])
		    {
		      oblom = 1;
		      continue;
		    }

		  cli_utf8_to_narrow ((vBINARY == out)?NULL:charset, cin, nl + 1, (unsigned char *) (buffer + *noutbytes), 1);

		  maxout --;
		  (*noutbytes) ++;
		  (*noutchars) ++;
		  if (0 >= maxout)
		    page_end = 1;
		  assert (maxin>=0);
		}
	      return page_end;
	    }
	  case vUTF:
	    {
	      int page_end = 0, len = 0;
	      int ilen = MIN (maxout, maxin);
	      int ret;

	      ret = wide_blob_buffered_read (proc_arg, buffer, ilen,
		  &bh->bh_state, &page_end, &len, get_data_proc);
	      if (ret < 1 && maxin < VIRT_MB_CUR_MAX)
		{
		  log_error (
		      "Incomplete UTF-8 char found in filling up a BLOB."
		      " The wide blob data may be garbaled.");
		  memset (buffer, '?', ilen);
		  ret = ilen;
		}
	      *nin = ret;
	      *noutbytes = ret;
	      *noutchars = len;
	      return page_end;
	    }
	  case vWIDE:
	    assert (0);
	};
	break;
      }
  };
  return 0;
}


box_t
blob_layout_ctor (dtp_t blob_handle_dtp, dp_addr_t start, dp_addr_t dir_start, size_t length, size_t diskbytes,
		  index_tree_t * it)
{
  blob_layout_t *ret;
#ifdef DEBUG
  if ( DV_BLOB_HANDLE != blob_handle_dtp &&
    DV_BLOB_WIDE_HANDLE != blob_handle_dtp &&
    DV_BLOB_XPER_HANDLE != blob_handle_dtp)
    GPF_T1 ("blob_layout_ctor failed");
#endif
  ret = (blob_layout_t *) dk_alloc_box_zero (sizeof (blob_layout_t), DV_CUSTOM);
  ret->bl_blob_handle_dtp = blob_handle_dtp;
  ret->bl_start = start;
  ret->bl_dir_start = dir_start;
  ret->bl_length = length;
  ret->bl_diskbytes = diskbytes;
  ret->bl_delete_later = 0;
  ret->bl_it = it;
  return (box_t) ret;
}


box_t
blob_layout_from_handle_ctor (blob_handle_t *bh)
{
  blob_layout_t *ret = (blob_layout_t *) dk_alloc_box_zero (sizeof (blob_layout_t), DV_CUSTOM);
  ret->bl_blob_handle_dtp = (dtp_t)DV_TYPE_OF (bh);
#ifdef DEBUG
  if ( DV_BLOB_HANDLE != ret->bl_blob_handle_dtp &&
    DV_BLOB_WIDE_HANDLE != ret->bl_blob_handle_dtp &&
    DV_BLOB_XPER_HANDLE != ret->bl_blob_handle_dtp)
    GPF_T1 ("blob_layout_from_handle_ctor failed");
#endif
  ret->bl_start = bh->bh_page;
  ret->bl_dir_start = bh->bh_dir_page;
  ret->bl_pages = (dp_addr_t *) box_copy((box_t) bh->bh_pages);
  ret->bl_page_dir_complete = bh->bh_page_dir_complete;
  ret->bl_length = bh->bh_length;
  ret->bl_diskbytes = bh->bh_diskbytes;
/*  ret->bl_delete_later = 0; not necessary now */
  ret->bl_it = bh->bh_it;
  return (box_t) ret;
}


void
blob_layout_free (blob_layout_t * bl)
{
#ifdef DEBUG
  if ( DV_CUSTOM != DV_TYPE_OF (bl) ||
    sizeof (blob_layout_t) != box_length (bl))
    GPF_T1 ("blob_layout_free failed");
#endif
  dk_free_box ((box_t) bl->bl_pages);
  dk_free_box ((caddr_t) bl);
}


#if 0
#define ASSERT_CHECKED_OUT(s) \
if ((io_action_func) random_read_ready_while_direct_io != \
      SESSION_SCH_DATA (s)->sio_default_read_ready_action) \
GPF_T1 ("Default read ready not off in blob read");
#else
#define ASSERT_CHECKED_OUT(s)
#endif

#define PAGES_COUNT_FOR_DISKBYTES(diskbytes) (((unsigned)(diskbytes) + PAGE_DATA_SZ-1) / PAGE_DATA_SZ)


static void bh_fill_pagedir_buffer (blob_handle_t * bh, buffer_desc_t * buf, it_cursor_t * itc_from, int *at_end, size_t *position);
static long bh_fill_data_buffer (blob_handle_t * bh, buffer_desc_t * buf, it_cursor_t * itc_from, int *status_ret, size_t *data_len_in_bytes);
static void __blob_chain_delete (it_cursor_t * it, dp_addr_t start, dp_addr_t first, int npages, blob_handle_t * bh);
static void blob_delete_via_dir (it_cursor_t * it, blob_layout_t * bl);
/* static int dk_set_blob_del_remove (dk_set_t * set, dp_addr_t addr); */
static int blob_new_page (it_cursor_t * row_itc, buffer_desc_t ** blob_buf, int page_flag);
static dp_addr_t bh_find_page (blob_handle_t * bh, size_t offset);
long blob_releases = 0;
long blob_releases_noread = 0;
long blob_releases_dir = 0;


void
cli_end_blob_read (client_connection_t * cli)
{
  if (cli_is_interactive (cli) && !cli->cli_ws)
    {
      if (cli->cli_session)
	{
	  int is_burst = 0;
	  mutex_enter (thread_mtx);
	  is_burst = !cli->cli_session->dks_fixed_thread &&
	      cli->cli_session->dks_thread_state == DKST_BURST ? 1 : 0;
	  mutex_leave (thread_mtx);
	  if (!is_burst)
	    PrpcCheckIn (cli->cli_session);
	}
    }
}


void
cli_send_data_req (client_connection_t * cli, blob_handle_t * bh)
{
  int is_burst;
  caddr_t *req =
  (caddr_t *) dk_alloc_box (2 * sizeof (caddr_t), DV_ARRAY_OF_POINTER);
  mutex_enter (thread_mtx);
  is_burst = !cli->cli_session->dks_fixed_thread &&
      cli->cli_session->dks_thread_state == DKST_BURST ? 1 : 0;
  mutex_leave (thread_mtx);
  if (!is_burst)
    PrpcCheckOut (cli->cli_session);
  req[0] = (caddr_t) QA_NEED_DATA;
  req[1] = box_num (bh->bh_param_index);
  ASSERT_CHECKED_OUT (cli->cli_session);
  PrpcAddAnswer ((caddr_t) req, DV_ARRAY_OF_POINTER, 1, 1);
  ASSERT_CHECKED_OUT (cli->cli_session);
  dk_free_tree ((box_t) req);
}

void
bh_set_it_fields (blob_handle_t *bh)
{
  short inx;
  bh->bh_key_id = bh->bh_it->it_key->key_id;
  if (KI_TEMP == bh->bh_key_id)
    {
      bh->bh_frag_no = 0;
      return;
    }
  DO_BOX (dbe_key_frag_t *, kf, inx, bh->bh_it->it_key->key_fragments)
    {
      if (kf->kf_it == bh->bh_it)
	{
	  bh->bh_frag_no = inx;
	  return;
	}
    }
  END_DO_BOX;
  GPF_T1 ("bh_it not found in the key");
}


static int
get_data_from_client (void *arg, unsigned char *buf, int pos, int len)
{
  int ret = session_buffered_read ((dk_session_t*)arg, (caddr_t)buf, len);
  return pos = ret;
}


static int
get_data_from_box (void *arg, unsigned char *buf, int pos, int len)
{
  assert (pos + len <= (int)box_length (arg));
  memcpy (buf, ((caddr_t)arg) + pos, len);
  return len;
}


static int
get_data_from_strses (void *arg, unsigned char *buf, int pos, int len)
{
  if (strses_get_part ((dk_session_t *) arg, buf, pos, len))
    GPF_T;
  return len;
}

typedef struct bh_get_layout_s {
	blob_handle_t * bh;
	it_cursor_t * itc_from;
	buffer_desc_t * buf;
	int *at_end;
} bh_get_layout_t;


int 
page_wait_blob_access (it_cursor_t * itc, dp_addr_t dp_to, buffer_desc_t ** buf_ret, int mode, blob_handle_t *bh, int itc_in_map_wrap)
{
  short flag;
  if (itc_in_map_wrap)
    ITC_IN_MAP (itc);
  buf_ret[0] = NULL;
  page_wait_access (itc, dp_to, NULL, NULL, buf_ret, mode, RWG_WAIT_ANY);
  if (PF_OF_DELETED == buf_ret[0])
    {
      log_info ("Attempt to read deleted blob dp = %lu start = %lu.", (unsigned long)dp_to, (unsigned long)((NULL != bh) ? bh->bh_page : 0));
      goto errexit;
    }
  if (NULL == buf_ret[0])
    {
      log_info ("Attempt to read (deleted?) blob dp = %lu start = %lu.", (unsigned long)dp_to, (unsigned long)((NULL != bh) ? bh->bh_page : 0));
      goto errexit;
    }
  flag = SHORT_REF (buf_ret[0]->bd_buffer + DP_FLAGS);
  if ((DPF_BLOB != flag) && (DPF_BLOB_DIR != flag))
    {
      if (PA_WRITE == mode)
        GPF_T1 ("page_wait_blob_access with PA_WRITE and non-BLOB buffer");
      log_info ("Attempt to read (deleted?) blob dp = %lu start = %lu: non-blob page found", (unsigned long)dp_to, (unsigned long)((NULL != bh) ? bh->bh_page : 0));
      remhash (DP_ADDR2VOID (dp_to), itc->itc_space->isp_dp_to_buf);
      buf_ret[0]->bd_space = 0;
      buf_set_last (buf_ret[0]);
      page_leave_inner (buf_ret[0]);
      goto errexit;
    }

#if 0
  if (rand () < (RAND_MAX >> 8))
    {
      page_leave_inner (buf_ret[0]);
      goto errexit;
    }
#endif

  return 1;

errexit:
  buf_ret[0] = NULL;
  if (itc_in_map_wrap)
    ITC_LEAVE_MAP (itc);
  return 0;
}

#define BLOB_AT_END_ERROR 2


static int
bh_pickup_page (blob_handle_t * bh, it_cursor_t * itc_from, int *at_end)
{
  buffer_desc_t *buf_from;
  long next;
  long pagelen;

  if (!page_wait_blob_access (itc_from, bh->bh_current_page, &buf_from, PA_READ, bh, 1))
    {
      *at_end = BLOB_AT_END_ERROR;
      bh->bh_current_page = bh->bh_page;	/* ready for reuse */
      bh->bh_state.bufpos = 0;
      bh->bh_state.buflen = 0;
      return 0;
    }
  pagelen = LONG_REF (buf_from->bd_buffer + DP_BLOB_LEN);
  if (PAGE_DATA_SZ < pagelen)
    GPF_T1 ("Abnormal length of BLOB data in database page");

  assert (NULL != bh->bh_state.buffer);
  memcpy (bh->bh_state.buffer, buf_from->bd_buffer + DP_DATA, pagelen);
  next = LONG_REF (buf_from->bd_buffer + DP_OVERFLOW);
  page_leave_inner (buf_from);
  ITC_LEAVE_MAP (itc_from);
  bh->bh_state.bufpos = 0;
  bh->bh_state.buflen = pagelen ;
  if (!next)
    {
      *at_end = 1;
      bh->bh_current_page = bh->bh_page;	/* ready for reuse */
    }
  else
    {
      bh->bh_current_page = next;
    }
  return pagelen;
}


static int
get_data_from_blob (void *arg, unsigned char *buf, int pos, int len)
{
  struct bh_get_layout_s *pbl = (struct bh_get_layout_s *)arg;
  blob_handle_t * bh = pbl->bh;
  it_cursor_t * itc_from = pbl->itc_from;
  int *at_end = pbl->at_end;
  int copybytes = 0;

  if (NULL == bh->bh_state.buffer)
    {
      bh->bh_state.buffer = dk_alloc_box (PAGE_DATA_SZ + 1, DV_BIN);
      bh->bh_state.bufpos = 0;
      bh->bh_state.buflen = 0;
    }

again:
  if (bh->bh_state.bufpos < bh->bh_state.buflen)
    {
      int l = MIN (len, bh->bh_state.buflen - bh->bh_state.bufpos);
      assert (bh->bh_state.buflen <= PAGE_DATA_SZ && bh->bh_state.bufpos <= PAGE_DATA_SZ && len <= PAGE_DATA_SZ);
      memcpy (buf, bh->bh_state.buffer + bh->bh_state.bufpos, l);
      bh->bh_state.bufpos += l;
      assert (bh->bh_state.buflen <= PAGE_DATA_SZ &&
		bh->bh_state.bufpos <= PAGE_DATA_SZ &&
		bh->bh_state.bufpos <= bh->bh_state.buflen &&
		len <= PAGE_DATA_SZ);
      len -= l;
      copybytes += l;
    }
  if (len && bh->bh_state.bufpos == bh->bh_state.buflen)
    {
      bh_pickup_page (bh, itc_from, at_end);
      if (BLOB_AT_END_ERROR == *at_end)
	return 0;
      goto again;
    }

  return pos = copybytes;
}


int
bh_tag_modify (blob_handle_t * bh, dtp_t tag, dtp_t ask_tag)
{
  dtp_t bh_dtp = (dtp_t)DV_TYPE_OF (bh);
  if (!IS_BLOB_HANDLE_DTP(bh_dtp))
    return 0;
  switch (ask_tag)
    {
    case DV_BLOB_WIDE:
    case DV_BLOB_WIDE_HANDLE:
      bh->bh_state.ask_tag = DV_BLOB_WIDE;
      break;
    case DV_BLOB_BIN:
    case DV_BIN:
      bh->bh_state.ask_tag = DV_BIN;
      break;
    default:
      bh->bh_state.ask_tag = DV_BLOB;
      break;
    }
  switch (tag)
    {
    case DV_BLOB_WIDE:
      bh->bh_state.need_tag = DV_BLOB_WIDE;
      box_tag_modify (bh, DV_BLOB_WIDE_HANDLE);
      break;
    case DV_BLOB_BIN:
      bh->bh_state.need_tag = DV_BLOB_BIN;
      box_tag_modify (bh, DV_BLOB_HANDLE);
      break;
    case DV_BLOB:
      bh->bh_state.need_tag = DV_BLOB;
      box_tag_modify (bh, DV_BLOB_HANDLE);
      break;
    case DV_BLOB_XPER:
      bh->bh_state.need_tag = DV_BLOB_BIN;
      box_tag_modify (bh, DV_BLOB_XPER_HANDLE);
    break;
    default:
      GPF_T1 ("incorrect attempt to modify blob_handle tag");
    }
  return 0;
}


long
bh_get_data_from_user (blob_handle_t * bh, client_connection_t * cli,
    db_buf_t to, int max_bytes)
{
  /* Read blob contents from client or log (in roll forward) */
  dtp_t bh_tag = (dtp_t)box_tag (bh);
  dk_session_t *ses_from = cli->cli_session;
  volatile int to_go = max_bytes, n_in = 0;

  if (bh->bh_ask_from_client == 2)
    {
      if (cli->cli_ws || bh->bh_source_session)
	return bh_get_data_from_http_user (bh, cli, to, max_bytes);
      else
	return 0;
    }
  else if (bh->bh_ask_from_client == 3)
    {
      if (cli->cli_ws || bh->bh_source_session)
	return bh_get_data_from_http_user_no_err (bh, cli, to, max_bytes);
      else
	return 0;
    }
  if (!bh->bh_bytes_coming)
    {
      if (cli_is_interactive (cli))
	{
	  if (bh->bh_all_received == BLOB_NULL_RECEIVED)
	    {
/* We already got NULL for this input so
1) no PrpcCheckOut in cli_send_data_req() and
2) no PrpcCheckIn in cli_end_blob_read()
*/
	      return 0;
	    }
	  if (bh->bh_all_received == BLOB_NONE_RECEIVED)
	    {
	      /* Ask for data, c.f. SQLParamData, SQLPutData */
	      cli_send_data_req (cli, bh);
	      PrpcSessionResetTimeout (ses_from);
	      bh->bh_all_received = BLOB_DATA_COMING;
	    }
	}

      CATCH_READ_FAIL (ses_from)
      {
	bh->bh_state.ask_tag = session_buffered_read_char (ses_from);
	switch (bh->bh_state.ask_tag)
	  {
	  case DV_WIDE:
	    /*box_tag_modify (bh, DV_BLOB_WIDE_HANDLE);*/
	    bh->bh_bytes_coming = (dtp_t) session_buffered_read_char (ses_from);
	    bh->bh_state.ask_tag = DV_BLOB_WIDE;
	    break;
	  case DV_SHORT_STRING_SERIAL:
	    /*box_tag_modify (bh, DV_BLOB_HANDLE);*/
	    bh->bh_bytes_coming = (dtp_t) session_buffered_read_char (ses_from);
	    bh->bh_state.ask_tag = DV_BLOB;
	    break;

	  case DV_BLOB_WIDE:
	  case DV_LONG_WIDE:
	    /*box_tag_modify (bh, DV_BLOB_WIDE_HANDLE);*/
	    bh->bh_bytes_coming = read_long (ses_from);
	    bh->bh_state.ask_tag = DV_BLOB_WIDE;
	    break;
	  case DV_BLOB_BIN:
	    bh->bh_bytes_coming = read_long (ses_from);
	    bh->bh_state.ask_tag = DV_BLOB_BIN;
	    break;
	  case DV_BIN:
	    bh->bh_bytes_coming = (dtp_t) session_buffered_read_char (ses_from);
	    bh->bh_state.ask_tag = DV_BLOB_BIN;
	    break;
	  case DV_BLOB:
	  case DV_LONG_STRING:
	    /*box_tag_modify (bh, DV_BLOB_HANDLE);*/
	    bh->bh_bytes_coming = read_long (ses_from);
	    bh->bh_state.ask_tag = DV_BLOB;
	    break;
	  case DV_BLOB_XPER:
	    box_tag_modify (bh, DV_BLOB_XPER_HANDLE);
	    bh->bh_bytes_coming = read_long (ses_from);
	    bh->bh_state.ask_tag = DV_BLOB;
	    bh_tag = DV_BLOB_HANDLE;
	    break;
	  case DV_DB_NULL:
	    bh->bh_all_received = BLOB_NULL_RECEIVED;
	    bh->bh_bytes_coming = 0;	/* No data expected in the future */
	    END_READ_FAIL (ses_from);
	    cli_end_blob_read (cli);
	    return n_in;
	  default:
	    bh->bh_all_received = BLOB_ALL_RECEIVED;
	    END_READ_FAIL (ses_from);
	    cli_end_blob_read (cli);
	    return n_in;
	  }
      }
      FAILED
      {
	bh->bh_all_received = BLOB_ALL_RECEIVED;
	END_READ_FAIL (ses_from);
	cli_end_blob_read (cli);
	return 0;
      }
      END_READ_FAIL (ses_from);
    }

  CATCH_READ_FAIL (ses_from)
  {
    for (;;)
      {
	if ((BLOB_ALL_RECEIVED == bh->bh_all_received) || (BLOB_NULL_RECEIVED == bh->bh_all_received))
	  {
	    END_READ_FAIL (ses_from);
	    cli_end_blob_read (cli);
	    return n_in;
	  }
	if (bh->bh_bytes_coming)
	  {
	    long readbytes = MIN (bh->bh_bytes_coming, to_go);
	    int page_end = 0;
#if 1
	    int nin = 0, nout = 0, ncout = 0;
	    blob_cvt_type_t flin = vBINARY, flout = vBINARY;
	    switch (bh->bh_state.ask_tag) {
	      case DV_BLOB_WIDE:
		flin = vUTF;
		break;
	      case DV_BLOB:
		flin = vNARROW;
		break;
	      case DV_BLOB_BIN:
		flin = vBINARY;
		break;
	    };
	    switch (bh->bh_state.need_tag) {
	      case DV_BLOB_WIDE:
		flout= vUTF;
		break;
	      case DV_BLOB_BIN:
		flout = vBINARY;
		break;
	      case DV_BLOB:
		flout = vNARROW;
		break;
	      default:
		{
		  switch (bh_tag) {
		    case DV_BLOB_XPER_HANDLE:
		      flout= vBINARY;
		      break;
		    case DV_BLOB_HANDLE:
		      flout= vNARROW;
		      break;
		    case DV_BLOB_WIDE_HANDLE:
		      flout= vUTF;
		      break;
		  };
		  break;
		}
	    };


	    page_end = bh_fill_page_generic (
		bh,
		cli->cli_charset,
		flin,
		flout,
		to + n_in,
		bh->bh_bytes_coming,
		to_go,
		&nin,
		&nout,
		&ncout,
		get_data_from_client,
		ses_from,
		0);

	    bh->bh_bytes_coming -= nin;
	    to_go -= nout;
	    n_in += nout;

	    assert (0 != readbytes);
#else
	    if (bh_tag == DV_BLOB_WIDE_HANDLE && DV_BLOB_WIDE == bh->bh_state.ask_tag)
	      {
 		readbytes = wide_blob_buffered_read (ses_from, (char *) to + n_in, readbytes, &bh->bh_state, &page_end);
	        if (readbytes < 0)
		  {
		    END_READ_FAIL (ses_from);
		    cli_end_blob_read (cli);
		    return n_in;
		  }

		bh->bh_bytes_coming -= readbytes;
		to_go -= readbytes;
		n_in += readbytes;
	      }
	    else if (bh_tag == DV_BLOB_HANDLE && DV_BLOB_WIDE == bh->bh_state.ask_tag)
	      {
		while (to_go > 0 && bh->bh_bytes_coming > 0)
		  {
		    char cin[VIRT_MB_CUR_MAX];
		    int nl, len;
		    page_end = 0;
		    readbytes = wide_blob_buffered_read (ses_from, cin, 1, &bh->bh_state, &page_end);
		    nl = bh->bh_state.count;
		    if (!page_end)
		      {
			*((char *) to + n_in) = cin[0];
			to_go --;
			n_in ++;
			bh->bh_bytes_coming --;
			continue;
		      }
		    if (nl + 1 < to_go)
		      len = wide_blob_buffered_read (ses_from, cin, nl + 1, &bh->bh_state, &page_end);
		    else
		      break;

		    cli_utf8_to_narrow ((wcharset_t*)cli->cli_charset, cin, nl + 1, ((char *) to + n_in), 1);

		    to_go --;
		    n_in ++;
		    bh->bh_bytes_coming -= nl + 1;
		    assert (bh->bh_bytes_coming>=0);
		  }
	      }
	    else if (bh_tag == DV_BLOB_WIDE_HANDLE && DV_BLOB == bh->bh_state.ask_tag)
	      {
		int len = 0;
		while (to_go >= VIRT_MB_CUR_MAX && bh->bh_bytes_coming > 0)
		  {
		    char cin;
		    cin = session_buffered_read_char (ses_from);
		    len = cli_narrow_to_utf8 (cli->cli_charset, &cin, 1, ((char *) to + n_in), VIRT_MB_CUR_MAX);
		    assert (len>0);
		    to_go -= len;
		    n_in += len;
		    bh->bh_bytes_coming --;
		  }
		if (to_go < VIRT_MB_CUR_MAX)
	          to_go = 0;
	      }
	    else /*if (bh_tag == DV_BLOB_HANDLE && DV_BLOB == bh->ask_tag)*/
	      {
		session_buffered_read (ses_from, (char *) to + n_in, readbytes);
		bh->bh_bytes_coming -= readbytes;
		to_go -= readbytes;
		n_in += readbytes;
	      }
/*	    long readbytes = MIN (bh->bh_bytes_coming, to_go);
	    int page_end = 0;
	    if ( DV_TYPE_OF(bh) == DV_BLOB_WIDE_HANDLE)
	      {
 		readbytes = wide_blob_buffered_read (ses_from, (char *) to + n_in, readbytes, &bh->bh_wb_state, &page_end);
	      }
	    else
	    session_buffered_read (ses_from, (char *) to + n_in, readbytes);
	    bh->bh_bytes_coming -= readbytes;
	    to_go -= readbytes;
	    n_in += readbytes;
	    if (page_end)
	      {
		END_READ_FAIL (ses_from);
		return n_in;
	      }*/
#endif
	    if (page_end)
	      {
		END_READ_FAIL (ses_from);
		return n_in;
	      }
	  }

	if (0 == bh->bh_bytes_coming)
	  {
	    bh->bh_state.ask_tag = session_buffered_read_char (ses_from);
	    switch (bh->bh_state.ask_tag)
	      {
	      case DV_WIDE:
		/*box_tag_modify (bh, DV_BLOB_WIDE_HANDLE);*/
		bh->bh_bytes_coming =
		    (dtp_t) session_buffered_read_char (ses_from);
		bh->bh_state.ask_tag = DV_BLOB_WIDE;
		break;
	      case DV_SHORT_STRING_SERIAL:
		/*box_tag_modify (bh, DV_BLOB_HANDLE);*/
		bh->bh_bytes_coming =
		    (dtp_t) session_buffered_read_char (ses_from);
		bh->bh_state.ask_tag = DV_BLOB;
		break;
	      case DV_BLOB_WIDE:
	      case DV_LONG_WIDE:
		/*box_tag_modify (bh, DV_BLOB_WIDE_HANDLE);*/
		bh->bh_bytes_coming = read_long (ses_from);
		bh->bh_state.ask_tag = DV_BLOB_WIDE;
		break;
	      case DV_BLOB_BIN:
		bh->bh_bytes_coming = read_long (ses_from);
		bh->bh_state.ask_tag = DV_BLOB_BIN;
		break;
	      case DV_BIN:
		bh->bh_bytes_coming = (dtp_t) session_buffered_read_char (ses_from);
		bh->bh_state.ask_tag = DV_BLOB_BIN;
		break;
	      case DV_BLOB:
	      case DV_LONG_STRING:
		/*box_tag_modify (bh, DV_BLOB_HANDLE);*/
		bh->bh_state.ask_tag = DV_BLOB;
		bh->bh_bytes_coming = read_long (ses_from);
		break;
	      case DV_BLOB_XPER:
		box_tag_modify (bh, DV_BLOB_XPER_HANDLE);
		bh->bh_bytes_coming = read_long (ses_from);
		bh->bh_state.ask_tag = DV_BLOB;
		bh_tag = DV_BLOB_HANDLE;
		break;
	      case DV_DB_NULL:
		bh->bh_all_received = BLOB_NULL_RECEIVED;
		bh->bh_bytes_coming = 0;	/* No data expected in the future */
		break;
	      default:
		bh->bh_all_received = BLOB_ALL_RECEIVED;
		break;
	      }
	  }
	if (!to_go)
	  {
	    END_READ_FAIL (ses_from);
	    return n_in;
	  }
      }
  }
  FAILED
  {
    bh->bh_all_received = BLOB_ALL_RECEIVED;
  }
  END_READ_FAIL (ses_from);
  return 0;
  /* let the normal procedure handle the disconnect */
}


static void
bh_fill_pagedir_buffer (blob_handle_t * bh, buffer_desc_t * buf, it_cursor_t * itc_from /* unused */,
    int *at_end, size_t *position)
{
  int filled = 0;
  itc_from = NULL; /* unused */
  if (NULL != bh->bh_pages)
    {
      int32 copybytes;
      long len = box_length (bh->bh_pages);
      long bytes = (long) (len - (*position));

      copybytes = (int32) MIN (PAGE_DATA_SZ, bytes);
      memcpy (buf->bd_buffer + DP_DATA, ((char *) bh->bh_pages) + (*position),
	  copybytes);
      (*position) += copybytes;
      if ((long)(*position) == len)
	{
	  (*position) = 0;
	  *at_end = 1;
	}
      LONG_SET (buf->bd_buffer + DP_BLOB_LEN, copybytes);
      filled += copybytes;
    }
}


static long
bh_fill_data_buffer (blob_handle_t * bh, buffer_desc_t * buf, it_cursor_t * itc_from, int *status_ret, size_t *data_len_in_bytes)
{
  /* Buffer is empty. Read from blob and set status_ret to BLOB_ALL_RECEIVED if at end or to BLOB_NULL_RECEIVED if NULL value found. Return bytes read */

  if (bh->bh_string)
    {
#if 1
      dtp_t string_tag = (dtp_t)DV_TYPE_OF (bh->bh_string);
      dtp_t bh_tag = (dtp_t)DV_TYPE_OF (bh);
      int page_end = 0, nin = 0, nout = 0, ncout = 0;
      long len = box_length (bh->bh_string);
      get_data_generic_proc proc = get_data_from_box;
      void *arg = bh->bh_string;
      blob_cvt_type_t flin = vBINARY, flout = vBINARY;
      switch (string_tag) {
	case DV_WIDE: case DV_LONG_WIDE:
	  flin = vWIDE;
	  len -= sizeof(wchar_t);
	break;
        case DV_STRING:
	  flin = vNARROW;
	  len --;
	break;
        case DV_STRING_SESSION:
	  len = strses_length ((dk_session_t *) bh->bh_string);
	  proc = get_data_from_strses;
        default:
	  flin = vBINARY;
	break;
      };
      switch (bh->bh_state.need_tag) {
	case DV_BLOB_WIDE:
	  flout= vUTF;
	  break;
	case DV_BLOB_BIN:
	  flout = vBINARY;
	  break;
	case DV_BLOB:
	  flout = vNARROW;
	  break;
	default:
	  {
	    switch (bh_tag) {
	      case DV_BLOB_XPER_HANDLE:
		flout = vBINARY;
	        break;
	      case DV_BLOB_HANDLE:
	        flout= vNARROW;
	        break;
	      case DV_BLOB_WIDE_HANDLE:
	        flout= vUTF;
	        break;
	    };
	    break;
	  }
      };
      page_end = bh_fill_page_generic (
		bh,
		itc_from->itc_ltrx->lt_client->cli_charset,
		flin,
		flout,
		buf->bd_buffer + DP_DATA,
		len - bh->bh_position,
		PAGE_DATA_SZ,
		&nin,
		&nout,
		&ncout,
		proc,
		arg,
		bh->bh_position);
      bh->bh_position += nin;
      if (bh->bh_position == len)
	{
	  bh->bh_position = 0;
	  status_ret[0] = BLOB_ALL_RECEIVED;
	}
      LONG_SET (buf->bd_buffer + DP_BLOB_LEN, nout);
      *data_len_in_bytes = nout;
      return ncout;
#else
      dtp_t string_tag = DV_TYPE_OF (bh->bh_string);
      long copybytes;
      if (IS_WIDE_STRING_DTP (string_tag))
	{
	  long copy_wchars = 0;
	  copybytes = blob_fill_buffer_from_wide_string ((caddr_t) bh, (caddr_t) buf, at_end, &copy_wchars);
	  LONG_SET (buf->bd_buffer + DP_BLOB_LEN, copybytes);
	  *data_len_in_bytes = copybytes;
	  return copy_wchars;
	}
      else if (string_tag == DV_STRING_SESSION)
	{
	  dk_session_t *ses = (dk_session_t *) bh->bh_string;
	  long len = strses_length (ses);
	  long bytes = len - bh->bh_position;
	  copybytes = MIN (PAGE_DATA_SZ, bytes);
	  if (strses_get_part (ses, buf->bd_buffer + DP_DATA, bh->bh_position, copybytes))
	    GPF_T;
	  bh->bh_position += copybytes;
	  if (bh->bh_position == len)
	    {
	      bh->bh_position = 0;
	      *at_end = 1;
	    }
	  LONG_SET (buf->bd_buffer + DP_BLOB_LEN, copybytes);
	  *data_len_in_bytes = copybytes;
	  return copybytes;
	}
      else
	{
	  long len = box_length (bh->bh_string) - (IS_STRING_DTP (string_tag) ? 1 : 0);
	  long bytes = len - bh->bh_position;
	  copybytes = MIN (PAGE_DATA_SZ, bytes);
	  memcpy (buf->bd_buffer + DP_DATA, bh->bh_string + bh->bh_position,
	      copybytes);
	  bh->bh_position += copybytes;
	  if (bh->bh_position == len)
	    {
	      bh->bh_position = 0;
	      *at_end = 1;
	    }
	  LONG_SET (buf->bd_buffer + DP_BLOB_LEN, copybytes);
	  *data_len_in_bytes = copybytes;
	  return copybytes;
	}
#endif
    }
  if (bh->bh_current_page)
    {
#if 1
      int page_end = 0, nin = 0, nout = 0, ncout = 0, __at_end = 0;
      blob_cvt_type_t flin = vBINARY, flout = vBINARY;
      struct bh_get_layout_s bl;
      dtp_t bh_tag = (dtp_t)DV_TYPE_OF (bh);
      bl.bh = bh;
      bl.itc_from = itc_from;
      bl.buf = buf;
      bl.at_end = &__at_end;

      switch (bh->bh_state.ask_tag) {
	case DV_BLOB_WIDE:
	    flin = vUTF;
	    break;
	case DV_BLOB:
	    flin = vNARROW;
	    break;
	default:
	    flin = vBINARY;
	    break;
      };
      switch (bh->bh_state.need_tag) {
	case DV_BLOB_WIDE:
	    flout= vUTF;
	    break;
	case DV_BLOB_BIN:
	    flout = vBINARY;
	    break;
	case DV_BLOB:
	    flout = vNARROW;
	    break;
	default:
	      {
		switch (bh_tag) {
		  case DV_BLOB_XPER_HANDLE:
		      flout = vBINARY;
		      break;
		  case DV_BLOB_HANDLE:
		      flout= vNARROW;
		      break;
		  case DV_BLOB_WIDE_HANDLE:
		      flout= vUTF;
		      break;
		};
		break;
	      }
      };
      page_end = bh_fill_page_generic (
	  bh,
	  default_charset,
	  flin,
	  flout,
	  buf->bd_buffer + DP_DATA,
	  (int) (bh->bh_diskbytes - bh->bh_position),
	  PAGE_DATA_SZ,
	  &nin,
	  &nout,
	  &ncout,
	  get_data_from_blob,
	  &bl,
	  0);
      bh->bh_position += nin;
      if (bh->bh_diskbytes <= (unsigned)bh->bh_position)
	{
	  bh->bh_position = 0;
	  status_ret[0] = BLOB_ALL_RECEIVED;
	}
      if (BLOB_AT_END_ERROR == __at_end)
	*status_ret = BLOB_ALL_RECEIVED;
      LONG_SET (buf->bd_buffer + DP_BLOB_LEN, nout);
      *data_len_in_bytes = nout;
      return ncout;
#else
      long next;
      buffer_desc_t *buf_from;
      long len;
      long copybytes;
      long copychars;
      ITC_IN_MAP (itc_from);
      FIX ME BEFORE USE: replace page_wait_access (itc_from, bh->bh_current_page, NULL, NULL, &buf_from, PA_READ, RWG_WAIT_ANY) with page_wait_blob_access;
      len = LONG_REF (buf_from->bd_buffer + DP_BLOB_LEN);
      /* copybytes = MIN (len, PAGE_DATA_SZ); -- This is not proper, error should be produced on overflow. */
      if (PAGE_DATA_SZ < len)
	GPF_T1 ("Abnormal length of BLOB data in database page");
      copybytes = len;
      copychars = (DV_TYPE_OF (bh) == DV_BLOB_WIDE_HANDLE ?
	  wide_char_length_of_utf8_string (buf_from->bd_buffer + DP_DATA, copybytes) :
	  copybytes);
      memcpy (buf->bd_buffer + DP_DATA, buf_from->bd_buffer + DP_DATA,
	  copybytes);
      *data_len_in_bytes = copybytes;
      next = LONG_REF (buf_from->bd_buffer + DP_OVERFLOW);
      LONG_SET (buf->bd_buffer + DP_BLOB_LEN, len);
      itc_page_leave (itc_from, buf_from);
      ITC_LEAVE_MAP (itc_from);
      if (!next)
	{
	  *at_end = 1;
	  bh->bh_current_page = bh->bh_page;	/* ready for reuse */
	}
      else
	{
	  bh->bh_current_page = next;
	}
      return copychars;
#endif
    }
  if (bh->bh_ask_from_client)
    {
      long n_in = bh_get_data_from_user (bh, itc_from->itc_ltrx->lt_client,
	  buf->bd_buffer + DP_DATA, PAGE_DATA_SZ);
      LONG_SET (buf->bd_buffer + DP_BLOB_LEN, n_in);
      if ((BLOB_ALL_RECEIVED == bh->bh_all_received) || (BLOB_NULL_RECEIVED == bh->bh_all_received))
	{
	  status_ret[0] = bh->bh_all_received;
	}
      *data_len_in_bytes = n_in;
      return (DV_TYPE_OF (bh) == DV_BLOB_WIDE_HANDLE ?
	  (long) wide_char_length_of_utf8_string (buf->bd_buffer + DP_DATA, n_in) :
	  n_in);
    }
  GPF_T1 ("Blob handle is totally empty");
  return 0;			/* never reached */
}


static void
blob_delete_via_dir (it_cursor_t * it, blob_layout_t * bl)
{
  if (bl->bl_pages)
    {
      int n = box_length (bl->bl_pages) / sizeof (dp_addr_t), i;
      for (i = 0; i < n; i++)
	{
	  ITC_IN_MAP (it);
	  isp_free_blob_dp_no_read (it->itc_space, bl->bl_pages[i]);
	  blob_releases_noread++;
	  blob_releases_dir++;
	  blob_releases++;
	  ITC_LEAVE_MAP (it);
	}
      __blob_chain_delete (it, bl->bl_dir_start, 0, PAGES_COUNT_FOR_DISKBYTES (box_length (bl->bl_pages)), NULL);
    }
}


static void
__blob_chain_delete (it_cursor_t * it, dp_addr_t start, dp_addr_t first, int npages /* unused */, blob_handle_t * bh)
{
  npages = 0; /* unused */
  while (start)
    {
      buffer_desc_t *buf;
      if (!DBS_PAGE_IN_RANGE (wi_inst.wi_master, start))
	{
	  log_error ("Blob next link out of range %ld", (long) start);
	  break;
	}
	  if (!page_wait_blob_access (it, start, &buf, PA_WRITE, NULL, 1))
	    return;
	  if (start == first)
	    {
	      dp_addr_t pdir = LONG_REF (buf->bd_buffer + DP_PARENT);
	      if (pdir)
		{
		  __blob_chain_delete (it, pdir, 0, 0, bh);
		}
	    }
      ITC_IN_MAP (it);
      start = LONG_REF (buf->bd_buffer + DP_OVERFLOW);
      isp_free_page (it->itc_space, buf);
      blob_releases++;
    }
  ITC_LEAVE_MAP (it);
}


void
blob_chain_delete (it_cursor_t * itc, blob_layout_t * bl)
{
  itc_from_it (itc, bl->bl_it);
  if (!bl->bl_pages)
    {
      __blob_chain_delete (itc, bl->bl_start, bl->bl_start, 1, NULL);
    }
  if (!bl->bl_page_dir_complete)
    blob_read_dir (itc, &bl->bl_pages, &bl->bl_page_dir_complete, bl->bl_dir_start);
  if (bl->bl_page_dir_complete) /* see if no error in reading page dir */
  blob_delete_via_dir (itc, bl);
  blob_layout_free (bl);
}


void
blob_log_replace (it_cursor_t * it, blob_layout_t * bl)
{
  dp_addr_t start = bl->bl_start;
  if (REPL_NO_LOG == it->itc_ltrx->lt_replicate)
    return;
  DO_SET (blob_log_t *, blob_log, &(it->itc_ltrx->lt_blob_log))
  {
    if (blob_log && blob_log->bl_start == start)
      {
	dk_free (blob_log, sizeof (blob_log_t));
	iter->data = NULL;
      }
  }
  END_DO_SET ();
}


void
blob_schedule_delayed_delete (it_cursor_t * itc, blob_layout_t *bl, int add_jobs)
{
  if (BLOB_OK != bl_check (bl))
    GPF_T1 ("Scheduling bad bl for delete at commit/rollback");
  if (0 && itc->itc_ltrx->lt_is_excl) /* no immediate delete in atomic mode */
    {
      if ((add_jobs & BL_DELETE_AT_COMMIT))
	{
	  /*if (REPL_NO_LOG != itc->itc_ltrx->lt_replicate) GK: nothing to do w/ log mode */
	    {
	      dk_hash_t ** hash_ptr = &(itc->itc_ltrx->lt_dirty_blobs);
	      if (NULL != hash_ptr[0])
		{
		  blob_layout_t *old_bl = (blob_layout_t *) gethash ((void *) (ptrlong) (bl->bl_start), hash_ptr[0]);
		  if (NULL != old_bl)
		    {
		      remhash ((void *) (ptrlong) (bl->bl_start), hash_ptr[0]);
		      blob_layout_free (old_bl);
		    }
		}
	    }
	  blob_chain_delete (itc, bl);
	  return;
	}
    }
  else
  /* if (REPL_NO_LOG != itc->itc_ltrx->lt_replicate) GK: nothing to do w/ the log mode */
    {
      dk_hash_t ** hash_ptr = &(itc->itc_ltrx->lt_dirty_blobs);
      if (0 != add_jobs)
	{
/* If some jobs should be added, hashtable should be created, if missing, and either old
   hastable item should be updated or new one should be created. */
	  if (NULL == hash_ptr[0])	/* If it's the first call in trx... */
	    hash_ptr[0] = hash_table_allocate (61);	/* ...then new table should be created... */
	  else
	    {
	      blob_layout_t *old_bl = (blob_layout_t *) gethash ((void *) (ptrlong) (bl->bl_start), hash_ptr[0]);
	      if (NULL != old_bl)
		{
		  add_jobs |= old_bl->bl_delete_later;
		  blob_layout_free (old_bl);	/* ...and free old versions */
		}
	    }
	  bl->bl_delete_later = add_jobs;
	  sethash ((void *) (ptrlong) (bl->bl_start), hash_ptr[0], bl);
	  return;
	}
      else
	{
/* If no new jobs should be added, hashtable should not be created, if missing, and new
   hashtable item should not be added. */
	  if (NULL != hash_ptr[0])
	    {
	      blob_layout_t *old_bl = (blob_layout_t *) gethash ((void *) (ptrlong) (bl->bl_start), hash_ptr[0]);
	      if (NULL != old_bl)
		{
		  bl->bl_delete_later = old_bl->bl_delete_later;
/*There was an error here: dk_free_box() instead of blob_layout_free(). */
		  blob_layout_free (old_bl);	/* ...and free old versions. */
		  sethash ((void *) (ptrlong) (bl->bl_start), hash_ptr[0], bl);
		  return;
		}
	    }
	}
    }
/* If no real processing has performed, bl will not be freed in future and
   it should be freed right now */
  blob_layout_free (bl);
}


void
blob_cancel_delayed_delete (it_cursor_t * itc, dp_addr_t first_blob_page, int cancel_jobs)
{
  dk_hash_t *hash;
  blob_layout_t *old_bl;
  assert ((0 != first_blob_page) && (0 != cancel_jobs));
  hash = itc->itc_ltrx->lt_dirty_blobs;
  if (NULL == hash)
    return;
  old_bl = (blob_layout_t *) gethash ((void *) (ptrlong) (first_blob_page), hash);
  if (NULL == old_bl)
    return;
  old_bl->bl_delete_later &= ~cancel_jobs;
  if (0 != old_bl->bl_delete_later)
    return;
/*There was an error here: dk_free_box() instead of blob_layout_free(). */
  blob_layout_free (old_bl);
  remhash ((void *) (ptrlong) (first_blob_page), hash);
}


void
blob_log_write (it_cursor_t * it, dp_addr_t start, dtp_t blob_dtp, dp_addr_t dir_start, long diskbytes,
		oid_t col_id, char * table_name)
{
  it->itc_has_blob_logged = 1;
  if (REPL_NO_LOG == it->itc_ltrx->lt_replicate)
    return;
  else
    {
      NEW_VAR (blob_log_t, blob_log);
      blob_log->bl_start = start;
      blob_log->bl_dir_start = dir_start;
      blob_log->bl_diskbytes = diskbytes;
      blob_log->bl_blob_dtp = blob_dtp;
      blob_log->bl_it = it->itc_tree;
      blob_log->bl_col_id = col_id;
      blob_log->bl_table_name = table_name;
      it->itc_ltrx->lt_blob_log =
	  dk_set_conc (it->itc_ltrx->lt_blob_log,
	  dk_set_cons ((caddr_t) blob_log, NULL));
    }
}


void
blob_log_set_free (s_node_t * set)
{
  dk_set_t next;
  while (set)
    {
      next = set->next;
      if (set->data)
	dk_free ((void *) set->data, sizeof (blob_log_t));
      dk_free ((void *) set, sizeof (s_node_t));
      set = next;
    }
}


int
blob_log_set_delete (dk_set_t * set, dp_addr_t dp)
{
  s_node_t *node = *set;
  dk_set_t *previous = set;
  blob_log_t *blob_log;
  while (node)
    {
      blob_log = (blob_log_t *) node->data;
      if (blob_log && blob_log->bl_start == dp)
	{
	  *previous = node->next;
	  dk_free (node->data, sizeof (blob_log_t));
	  dk_free (node, sizeof (s_node_t));

	  return 1;
	}
      previous = &(node->next);
      node = node->next;
    }
  return 0;
}


static int
blob_new_page (it_cursor_t * row_itc,
    buffer_desc_t ** blob_buf,
    int page_flag)
{
  *blob_buf = isp_new_page (row_itc->itc_space, row_itc->itc_page,
      page_flag, 0, 0);
  if (!*blob_buf)
    {
      row_itc->itc_ltrx->lt_error = LTE_NO_DISK;
      ITC_LEAVE_MAP (row_itc);
      log_error ("Out of disk space for database");
      cli_end_blob_read (row_itc->itc_ltrx->lt_client);
      return LTE_NO_DISK;
    }
  return LTE_OK;
}


blob_layout_t *
bl_from_dv (dtp_t * col, it_cursor_t * itc)
{
  key_id_t key_id;
  int frag_no;
  int inx, n_pages;
  blob_layout_t * bl = (blob_layout_t *) blob_layout_ctor (
	DV_BLOB_HANDLE_DTP_FOR_BLOB_DTP(*col),	/* bl_blob_handle_dtp */
	LONG_REF_NA (col + BL_DP),		/* start */
	LONG_REF_NA (col + BL_PAGE_DIR),	/* dir_start */
	LONG_REF_NA (col + BL_CHAR_LEN),	/* length */
	LONG_REF_NA (col + BL_BYTE_LEN),	/* diskbytes */
	NULL);					/* index_tree (later)*/

  n_pages = (int) BL_N_PAGES (bl->bl_diskbytes);
  if (n_pages < BL_DPS_ON_ROW || !bl->bl_dir_start)
    bl->bl_page_dir_complete = 1;
  bl->bl_pages = (dp_addr_t *) dk_alloc_box (n_pages * sizeof (dp_addr_t), DV_BIN);
  for (inx = 0; inx < n_pages; inx++)
    {
      if (inx < BL_DPS_ON_ROW)
	bl->bl_pages[inx] = LONG_REF_NA (col + BL_DP + 4 * inx);
      else
	bl->bl_pages[inx] = 0;
    }
  key_id = (key_id_t)SHORT_REF_NA (col + BL_KEY_ID);
  frag_no = SHORT_REF_NA (col + BL_FRAG_NO);
  if (KI_TEMP == key_id)
    bl->bl_it = itc->itc_tree;
  else
    {
      dbe_key_t * key = sch_id_to_key (wi_inst.wi_schema, key_id);
      dbe_key_frag_t ** frags;
      if (!key)
	bl->bl_it = itc->itc_tree;
      else
	{
	  int n_frags;
	  frags = key->key_fragments;
	  n_frags = BOX_ELEMENTS (frags);
	  if (frag_no < n_frags)
	    bl->bl_it = frags[frag_no]->kf_it;
	  else
	    bl->bl_it = itc->itc_tree;
	}
    }

  return bl;
}


blob_handle_t *
bh_from_dv (dtp_t * col, it_cursor_t * itc)
{
  key_id_t key_id;
  short frag_no;
  int inx, n_pages;
  blob_handle_t * bh = (blob_handle_t *)
    dk_alloc_box_zero (sizeof (blob_handle_t),
		       DV_BLOB_HANDLE_DTP_FOR_BLOB_DTP(*col));
  bh->bh_length = LONG_REF_NA (col + BL_CHAR_LEN);
  bh->bh_diskbytes = LONG_REF_NA (col + BL_BYTE_LEN);
  bh->bh_page = LONG_REF_NA (col + BL_DP);
  bh->bh_current_page = bh->bh_page;
  bh->bh_dir_page = LONG_REF_NA (col + BL_PAGE_DIR);
  bh->bh_timestamp = LONG_REF_NA (col + BL_TS);
  if (!bh->bh_dir_page)
    bh->bh_page_dir_complete = 1;
  n_pages = (int) BL_N_PAGES (bh->bh_diskbytes);
  bh->bh_pages = (dp_addr_t *) dk_alloc_box (n_pages * sizeof (dp_addr_t), DV_BIN);
  for (inx = 0; inx < n_pages; inx++)
    {
      if (inx < BL_DPS_ON_ROW)
	bh->bh_pages[inx] = LONG_REF_NA (col + BL_DP + 4 * inx);
      else
	bh->bh_pages[inx] = 0;
    }
  key_id = (key_id_t)SHORT_REF_NA (col + BL_KEY_ID);
  frag_no = (short)SHORT_REF_NA (col + BL_FRAG_NO);
  bh->bh_key_id = key_id;
  bh->bh_frag_no = frag_no;
  if (KI_TEMP == key_id)
    {
      bh->bh_it = itc->itc_tree;
    }
  else
    {
      dbe_key_t * key = sch_id_to_key (wi_inst.wi_schema, key_id);
      dbe_key_frag_t ** frags;
      if (!key)
	bh->bh_it = itc->itc_tree;
      else
	{
	  int n_frags;
	  frags = key->key_fragments;
	  n_frags = BOX_ELEMENTS (frags);
	  if (frag_no < n_frags)
	    {
	      bh->bh_it = frags[frag_no]->kf_it;
	      bh->bh_frag_no = frag_no;
	    }
	  else
	    bh->bh_it = itc->itc_tree;
	}
    }
  return bh;
}


int
blob_write_dir (it_cursor_t * row_itc, blob_handle_t * bh, buffer_desc_t * first_buf)
{
  int n_pages = box_length (bh->bh_pages);
  int at_end = 0, rc;
  size_t pos;
  buffer_desc_t * next_blob_buf, *blob_buf;
  bh->bh_page_dir_complete = 1;
  if (n_pages < BL_DPS_ON_ROW)
    {
      bh->bh_dir_page = 0;  /* may have a non-zero dir page if was logged with one and then truncated to 0 in update in same txn. */
      itc_page_leave (row_itc, first_buf);
      return LTE_OK;
    }
  rc = blob_new_page (row_itc, &blob_buf, DPF_BLOB_DIR);
  if (LTE_OK != rc)
    {
      itc_page_leave (row_itc, first_buf);
      return rc;
    }
  LONG_SET (first_buf->bd_buffer + DP_PARENT, blob_buf->bd_page);
  itc_page_leave (row_itc, first_buf);
  bh->bh_dir_page = blob_buf->bd_page;
  pos = 0;
  for (;;)
    {
      bh_fill_pagedir_buffer (bh, blob_buf, row_itc, &at_end, &pos);
      if (at_end)
	{
	  LONG_SET (blob_buf->bd_buffer + DP_OVERFLOW, 0);
	  itc_page_leave (row_itc, blob_buf);
	  break;
	}

      rc = blob_new_page (row_itc, &next_blob_buf, DPF_BLOB_DIR);
      if (rc != LTE_OK)
	{
	  itc_page_leave (row_itc, blob_buf);
	  return rc;
	}
      LONG_SET (blob_buf->bd_buffer + DP_OVERFLOW, next_blob_buf->bd_page);
      itc_page_leave (row_itc, blob_buf);
      blob_buf = next_blob_buf;
    }
  return LTE_OK;
}


void
bh_to_dv (blob_handle_t * bh, dtp_t * col, dtp_t dtp)
{
  int frag_no, inx;
  int n_pages;
  col[0] = dtp;
  LONG_SET_NA (col + BL_CHAR_LEN, bh->bh_length);
  LONG_SET_NA (col + BL_BYTE_LEN, bh->bh_diskbytes);
  SHORT_SET_NA (col + BL_KEY_ID, bh->bh_it->it_key->key_id);
  DO_BOX (dbe_key_frag_t *, frag, frag_no, bh->bh_it->it_key->key_fragments)
    {
      index_tree_t * frag_it = frag->kf_it;
      if (frag_it == bh->bh_it)
	break;
    }
  END_DO_BOX;
  SHORT_SET_NA (col + BL_FRAG_NO, frag_no);
  n_pages = box_length ((caddr_t) bh->bh_pages) / sizeof (dp_addr_t);
  n_pages = MIN (n_pages, BL_DPS_ON_ROW);
  for (inx = 0; inx < BL_DPS_ON_ROW; inx++)
    LONG_SET_NA (col + BL_DP + sizeof (dp_addr_t) * inx,
		 inx < n_pages ? bh->bh_pages[inx] : 0);
  LONG_SET_NA (col + BL_PAGE_DIR, bh->bh_dir_page);
  LONG_SET_NA (col + BL_TS, bh->bh_timestamp);
}


int
itc_set_blob_col (it_cursor_t * row_itc, db_buf_t col,
    caddr_t data, blob_layout_t * replaced_version,
    int log_as_insert, sql_type_t * col_sqt)
{
  dtp_t col_dtp = col_sqt->sqt_dtp;
  blob_handle_t *volatile target_bh = NULL, * volatile source_bh = NULL;
  dk_set_t volatile pages = NULL;
  int n_pages = 0, page_inx;
#ifdef BIF_XML
  dk_session_t *volatile strses = NULL;
#endif
  it_cursor_t blob_itc_auto;
  it_cursor_t *blob_itc = &blob_itc_auto;
  dp_addr_t first_page = 0;
  int read_status = BLOB_NONE_RECEIVED;
  size_t pos;
  size_t data_len = 0;
  dtp_t volatile dtp = (dtp_t)DV_TYPE_OF (data);
  buffer_desc_t *blob_buf = NULL, *next_blob_buf = NULL, *volatile first_buf = NULL;
  int row_is_temporary;

  int rc;
  ITC_LEAVE_MAP (row_itc);	/* blob_itc will need the map */
  if (data && !IS_BOX_POINTER (data))
    {
      rc = LTE_SQL_ERROR;
      LT_ERROR_DETAIL_SET (row_itc->itc_ltrx,
	  box_dv_short_string ("Invalid data type (integer) when making a blob"));
      goto resource_cleanup;
    }
  switch (dtp)
    {
      case DV_TIMESTAMP:
      case DV_DATE:
      case DV_NULL:
      case DV_C_SHORT:
      case DV_SHORT_INT:
      case DV_LONG_INT:
      case DV_SINGLE_FLOAT:
      case DV_DOUBLE_FLOAT:
      case DV_CHARACTER:
      case DV_ARRAY_OF_POINTER:
      case DV_ARRAY_OF_LONG_PACKED:
      case DV_ARRAY_OF_DOUBLE:
      case DV_LIST_OF_POINTER:
      case DV_C_INT:
      case DV_ARRAY_OF_FLOAT:
      case DV_CUSTOM:
      case DV_DB_NULL:
      case DV_ARRAY_OF_LONG:
      case DV_TIME:
      case DV_DATETIME:
      case DV_ARRAY_OF_XQVAL:
      case DV_DICT_HASHTABLE:
      case DV_DICT_ITERATOR:
      case DV_NUMERIC:
      case DV_TINY_INT:
	{
	  rc = LTE_SQL_ERROR;
	  LT_ERROR_DETAIL_SET (row_itc->itc_ltrx,
	      box_sprintf (60,
		"Invalid data type (%.30s) when making a blob",
		dv_type_title (dtp)));
	  goto resource_cleanup;
	}
    }
  if (IS_BLOB_HANDLE_DTP (dtp))
    {
      source_bh = (blob_handle_t *) data;
      source_bh->bh_current_page = source_bh->bh_page;
      if (source_bh->bh_ask_from_client)
	target_bh = source_bh;
      else
	target_bh = bh_alloc ((dtp_t)DV_BLOB_HANDLE_DTP_FOR_BLOB_DTP (col_dtp));
      bh_tag_modify (source_bh, col_dtp, dtp);
      goto bh_is_ready;		/* see below */
    }
#ifdef BIF_XML
  if (DV_XML_ENTITY == dtp && XE_IS_PERSISTENT (data))
    {
      xper_entity_t *cut = xper_cut_xper (NULL, (xper_entity_t *) data);
      blob_handle_t *cut_bh = cut->xe_doc.xpd->xpd_bh;
      if (XPD_NEW == cut->xe_doc.xpd->xpd_state
	  /*&& cut_bh->bh_length > MAX_ROW_BYTES*/ )
	{
	  blob_handle_t *cut_bh = cut->xe_doc.xpd->xpd_bh;
	  /* Migration of pages from space to space -- begin */
	  it_cursor_t *tmp_itc;
	  index_tree_t * it;
	  dp_addr_t *migr_dir = cut_bh->bh_pages;
	  int migr_ctr, migr_no = (box_length (migr_dir) / sizeof (dp_addr_t));
	  void **migr_data = (void **)dk_alloc (migr_no * sizeof (void *) * 3);
	  void **migr_tail = migr_data;
	  dk_hash_t *tmp_remaps, *tmp_bufs /* */, *tmp_cpt_remaps /* */;
	  dk_hash_t *row_remaps, *row_bufs /* */, *row_cpt_remaps /* */;
	  tmp_itc =
	    itc_create (NULL, cut->xe_doc.xd->xd_qi->qi_trx);
	  it = cut_bh->bh_it;
	  if (NULL != it)
	    {
	      itc_from_it (tmp_itc, it);
	    }
	  else
	    {
	      dbe_key_t* xper_key = sch_id_to_key (wi_inst.wi_schema, KI_COLS);
	      itc_from (tmp_itc, xper_key);
	    }
	  tmp_remaps = tmp_itc->itc_space->isp_remap;
	  row_remaps = row_itc->itc_space->isp_remap;
	  tmp_bufs = tmp_itc->itc_space->isp_dp_to_buf;
	  row_bufs = row_itc->itc_space->isp_dp_to_buf;
	  /* */ tmp_cpt_remaps = tmp_itc->itc_tree->it_storage->dbs_cpt_remap; /* */
	  /* */ row_cpt_remaps = row_itc->itc_tree->it_storage->dbs_cpt_remap; /* */
	  ITC_IN_MAP (tmp_itc);
	  for (migr_ctr = 0; migr_ctr < migr_no; migr_ctr++)
	    {
	      dp_addr_t migr = migr_dir[migr_ctr];
	      void *migr_key = DP_ADDR2VOID (migr);
	      void *remap = gethash (migr_key, tmp_remaps);
	      void *buf = gethash (migr_key, tmp_bufs);
#ifdef DEBUG
	      if (0 == migr_key)
		GPF_T;
#endif
	      (migr_tail++)[0] = remap;
	      (migr_tail++)[0] = buf;
	      if (remap)
		{
		  remhash (migr_key, tmp_remaps);
		}
	      if (buf)
		{
		  buffer_desc_t * bd = (buffer_desc_t *)(buf);
		  /* */ void *migr_cpt_key = DP_ADDR2VOID (bd->bd_page);
		  void *cpt_remap = gethash (migr_cpt_key, tmp_cpt_remaps);
		  (migr_tail++)[0] = cpt_remap;
		  if (cpt_remap)
		    remhash (migr_cpt_key, tmp_cpt_remaps); /* */
		  remhash (migr_key, tmp_bufs);
		  bd->bd_in_write_queue++;
		}
	    }
	  ITC_LEAVE_MAP (tmp_itc);
	  migr_tail = migr_data;
	  ITC_IN_MAP (row_itc);
	  for (migr_ctr = 0; migr_ctr < migr_no; migr_ctr++)
	    {
	      dp_addr_t migr = migr_dir[migr_ctr];
	      void *migr_key = DP_ADDR2VOID (migr);
	      void *remap = (migr_tail++)[0];
	      void *buf = (migr_tail++)[0];
	      if (remap)
		{
		  sethash (migr_key, row_remaps, remap);
		}
	      if (buf)
		{
		  buffer_desc_t * bd = (buffer_desc_t *)(buf);
		  /* */ void *migr_cpt_key = DP_ADDR2VOID (bd->bd_page);
		  void *cpt_remap = (migr_tail++)[0];
		  sethash (migr_key, row_bufs, buf);
		  if (cpt_remap)
		    sethash (migr_cpt_key, row_cpt_remaps, cpt_remap); /* */
		  bd->bd_space = row_itc->itc_space;
		  bd->bd_in_write_queue--;
		}
	    }
	  cut_bh->bh_it = row_itc->itc_tree;
	  itc_free (tmp_itc);
	  dk_free (migr_data, (size_t)-1);
	  /* Migration of pages from space to space -- end */
	  /* the blob is new, not refd from any table, hence no copy needed. further, it is long enough not to be inlined */
	  cut->xe_doc.xpd->xpd_state = XPD_PERSISTENT;
	  first_page = cut_bh->bh_pages[0];
	  if (!page_wait_blob_access (row_itc, first_page, (buffer_desc_t **) &first_buf, PA_WRITE, NULL, 0))
	    {
	      ITC_LEAVE_MAP (row_itc);
	      goto lte_not_ok;
	    }
	  buf_set_dirty (first_buf);
	  ITC_LEAVE_MAP (row_itc);
	  rc = blob_write_dir (row_itc, cut_bh, first_buf);
	  if (rc != LTE_OK)
	    {
	      goto lte_not_ok;
	    }
	  if (replaced_version)
	    {
	      blob_log_replace (row_itc, replaced_version);
	      blob_schedule_delayed_delete (row_itc, replaced_version, BL_DELETE_AT_COMMIT);
	      replaced_version = NULL;
	    }
	  blob_log_write (row_itc, cut_bh->bh_page, DV_BLOB_XPER, 0, 0, 0, 0);
	  bh_to_dv (cut_bh, col, DV_BLOB_XPER);
	  blob_schedule_delayed_delete (row_itc, bl_from_dv (col, row_itc), BL_DELETE_AT_ROLLBACK);
	  dk_free_box ((box_t) cut);
	  rc = LTE_OK;
	  goto resource_cleanup;
	}
      source_bh = (blob_handle_t *) box_copy_tree ((caddr_t) cut_bh);
      source_bh->bh_current_page = source_bh->bh_page;
      dk_free_box ((box_t) cut);
      target_bh = bh_alloc (DV_BLOB_XPER_HANDLE);
      goto bh_is_ready;		/* see below */
    }
  if (DV_XML_ENTITY == dtp && !XE_IS_PERSISTENT (data))
    {
      xml_entity_t *xe = (xml_entity_t *) data;
      caddr_t saved_encoding = xe->xe_doc.xd->xout_encoding;
      int saved_decl = xe->xe_doc.xd->xout_omit_xml_declaration;
      int saved_indent = xe->xe_doc.xd->xout_indent;
      caddr_t saved_method = xe->xe_doc.xd->xout_method;
      xe->xe_doc.xd->xout_encoding = "UTF-8";
      xe->xe_doc.xd->xout_omit_xml_declaration = 1;
      xe->xe_doc.xd->xout_indent = 0;
      xe->xe_doc.xd->xout_method = "xml";
      strses = strses_allocate ();
      SES_PRINT (strses, "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>");
      ((xml_entity_t *) data)->_->xe_serialize ((xml_entity_t *) data, strses);
      xe->xe_doc.xd->xout_encoding = saved_encoding;
      xe->xe_doc.xd->xout_omit_xml_declaration = saved_decl;
      xe->xe_doc.xd->xout_indent = saved_indent;
      xe->xe_doc.xd->xout_method = saved_method;
      source_bh = bh_alloc (DV_BLOB_HANDLE);
      source_bh->bh_string = (caddr_t) strses;
      target_bh = bh_alloc (DV_BLOB_HANDLE);
      goto bh_is_ready;		/* see below */
    }
  if (DV_STRING_SESSION == dtp)
    {
      source_bh = bh_alloc (DV_BLOB_HANDLE);
      source_bh->bh_string = data;
      target_bh = bh_alloc (DV_BLOB_HANDLE);
      goto bh_is_ready;		/* see below */
    }
#endif
  if (col_sqt->sqt_class)
    {
      strses = strses_allocate ();
      udt_serialize (data, strses);
      source_bh = bh_alloc (DV_BLOB_HANDLE);
      source_bh->bh_string = (caddr_t) strses;
      target_bh = bh_alloc (DV_BLOB_HANDLE);
      goto bh_is_ready;		/* see below */
    }
  source_bh = bh_alloc ((dtp_t)DV_BLOB_HANDLE_DTP_FOR_BLOB_DTP (col_dtp));
	/*(IS_WIDE_STRING_DTP (dtp) ? DV_BLOB_WIDE_HANDLE : DV_BLOB_HANDLE));*/
  source_bh->bh_string = data;
  bh_tag_modify (source_bh, col_dtp, (dtp_t)DV_TYPE_OF(data));
  target_bh = bh_alloc ((dtp_t)DV_TYPE_OF (source_bh));
bh_is_ready:

  if (replaced_version && replaced_version->bl_start == source_bh->bh_page)
    {
      /* the blob is assigned to itself. Return, */
      bh_to_dv (source_bh, col, DV_BLOB_DTP_FOR_BLOB_HANDLE_DTP (box_tag (source_bh)));
      /* OE: a blob may get inlined. Do not delete the page until commit
	 in case this were rolled back or value-refed in trigger */
      row_itc->itc_has_blob_logged = 1;
      rc = LTE_OK;
      goto resource_cleanup;
    }
  if (replaced_version)
    {
      blob_log_replace (row_itc, replaced_version);
      blob_schedule_delayed_delete (row_itc, replaced_version, BL_DELETE_AT_COMMIT);
      replaced_version = NULL;
    }

  target_bh->bh_timestamp = sqlbif_rnd (&rnd_seed_b) + approx_msec_real_time ();
  /* Too many messages...
  dbg_printf (("itc_set_blob_col: creating ts %ld\n", target_bh->bh_timestamp));
  ... Too many messages */

  ITC_INIT (blob_itc, NULL, row_itc->itc_ltrx);
  itc_from_it (blob_itc, source_bh->bh_it ? source_bh->bh_it : row_itc->itc_tree);

  source_bh->bh_state.utf8_chr = '\0';
  source_bh->bh_state.count = '\0';

  ITC_FAIL (blob_itc)
  {
    rc = blob_new_page (row_itc, &blob_buf, DPF_BLOB);
    if (rc != LTE_OK)
      goto lte_not_ok;
    first_buf = blob_buf;
    first_page = blob_buf->bd_page;
    dk_set_push ((dk_set_t *) &pages, DP_ADDR2VOID (first_page));
    n_pages++;
    pos = 0;
    target_bh->bh_diskbytes = 0;
    target_bh->bh_length = 0;
    for (;;)
      {
	ITC_LEAVE_MAP (row_itc);
	target_bh->bh_length += bh_fill_data_buffer (source_bh, blob_buf, blob_itc, &read_status, &data_len);
	if (read_status)
	  {
	    target_bh->bh_diskbytes += data_len;
	    LONG_SET (blob_buf->bd_buffer + DP_OVERFLOW, 0);
	    LONG_SET (blob_buf->bd_buffer + DP_BLOB_TS, target_bh->bh_timestamp);
	    if (blob_buf != first_buf)
	      itc_page_leave (row_itc, blob_buf);
	    break;
	  }
	/* target_bh->bh_diskbytes += PAGE_DATA_SZ; */
	target_bh->bh_diskbytes += data_len;
	rc = blob_new_page (row_itc, &next_blob_buf, DPF_BLOB);
	if (rc != LTE_OK)
	  {
	    goto lte_not_ok;
	  }
	dk_set_push ((dk_set_t *) &pages, DP_ADDR2VOID ((next_blob_buf->bd_page)));
	n_pages++;
	LONG_SET (blob_buf->bd_buffer + DP_OVERFLOW, next_blob_buf->bd_page);
	LONG_SET (blob_buf->bd_buffer + DP_BLOB_TS, target_bh->bh_timestamp);
	if (blob_buf != first_buf)
	  itc_page_leave (row_itc, blob_buf);
	blob_buf = next_blob_buf;
	ITC_LEAVE_MAP (blob_itc);
      }
    target_bh->bh_it = row_itc->itc_tree;

#ifdef DEBUG
    assert (dk_set_length (pages) == n_pages);
#endif
    target_bh->bh_pages = (dp_addr_t *) dk_alloc_box (sizeof (dp_addr_t) * n_pages, DV_BIN);
    for (page_inx = n_pages; page_inx--; /* no step*/)
      {
	target_bh->bh_pages[page_inx] = (dp_addr_t)(ptrlong)(dk_set_pop ((dk_set_t *) &pages));
      }
    rc = blob_write_dir (row_itc, target_bh, first_buf);
    first_buf = NULL; /* left above */
    if (LTE_OK != rc)
      goto lte_not_ok;
    target_bh->bh_page = first_page;
    target_bh->bh_current_page = first_page;
    row_is_temporary = (KI_TEMP == row_itc->itc_tree->it_key->key_id);
    if ((!row_is_temporary) &&
        ( dtp == DV_BLOB_HANDLE || dtp == DV_BLOB_WIDE_HANDLE
#ifdef BIF_XML
	|| (DV_XML_ENTITY == dtp && XE_IS_PERSISTENT (data))
#endif
	  || BLOB_IN_INSERT == log_as_insert) )
      {
	/* if the blob came as a string, it is logged as such, not as blob */
	if (BLOB_NULL_RECEIVED != read_status)
	  blob_log_write (row_itc, first_page, DV_BLOB_DTP_FOR_BLOB_HANDLE_DTP (box_tag (target_bh)), 0, 0, 0, 0);
      }
    bh_to_dv (target_bh, col, DV_BLOB_DTP_FOR_BLOB_HANDLE_DTP (box_tag (target_bh)));
    if (!row_is_temporary)
    blob_schedule_delayed_delete (row_itc, bl_from_dv (col, row_itc),
				  BL_DELETE_AT_ROLLBACK );
    if (BLOB_NULL_RECEIVED != read_status)
      source_bh->bh_ask_from_client = 0;	/* next time this bh is used it will refer to
				   this blob and won't ask the user again. */
    bh_set_it_fields (target_bh);
    rc = LTE_OK;
  }
  ITC_FAILED
    {
      if (BLOB_DATA_COMING == target_bh->bh_all_received)
        cli_end_blob_read (blob_itc->itc_ltrx->lt_client);
      rc = LTE_TIMEOUT;
    }
  END_FAIL (blob_itc);

  goto resource_cleanup;
 lte_not_ok:
  ITC_LEAVE_MAP (row_itc);
  ITC_LEAVE_MAP (blob_itc);	/* we can be here from between ITC_MAP brackets */
  if (first_buf)
    itc_page_leave (row_itc, first_buf);
  rc = row_itc->itc_ltrx->lt_error;
resource_cleanup:
#ifdef BIF_XML
  if (NULL != strses)
    dk_free_box ((box_t) strses);
#endif
  if (NULL != replaced_version)
    blob_layout_free (replaced_version);
  if (data != (caddr_t) target_bh)
    dk_free_box ((caddr_t) target_bh);
  if (data != (caddr_t) source_bh && target_bh != source_bh)
    dk_free_box ((caddr_t) source_bh);
  dk_set_free (pages);
  return ((BLOB_NULL_RECEIVED != read_status) ? rc : -1);
}



void
row_fixup_blob_refs (it_cursor_t * itc, db_buf_t row)
{
  dbe_key_t * key = itc->itc_row_key;
  itc->itc_insert_key = key;
  itc->itc_row_data = row + IE_FIRST_KEY;
  if (key && key->key_row_var)
    {
      int inx;
      for (inx = 0; key->key_row_var[inx].cl_col_id; inx++)
	{
	  dbe_col_loc_t * cl = &key->key_row_var[inx];
	  dtp_t dtp = cl->cl_sqt.sqt_dtp;
	  if (IS_BLOB_DTP (dtp))
	    {
	      int off, len;
	      if (ITC_NULL_CK (itc, (*cl)))
		continue;
	      ITC_COL (itc, (*cl), off, len);
	      dtp = itc->itc_row_data[off];
	      if (IS_BLOB_DTP (dtp))
		{
		  blob_handle_t *tmp_bh = bh_alloc (DV_BLOB_HANDLE_DTP_FOR_BLOB_DTP (cl->cl_sqt.sqt_dtp));
		  tmp_bh->bh_ask_from_client = 1;
		  itc_set_blob_col (itc, itc->itc_row_data + off, (caddr_t) tmp_bh, 0, BLOB_IN_INSERT, &cl->cl_sqt);
		  bh_free (tmp_bh);
		}
	    }
	}
    }
}


long blob_pages_logged;


long blob_write_crash_recover_log_1 (dk_session_t * log, blob_log_t * bl, dp_addr_t start, long should_be_len)
{
  dp_addr_t page;
  int inx = 0, bit = 0;
  uint32 *array = NULL;
  dbe_storage_t * storage = wi_inst.wi_master;
  static buffer_desc_t * buf = NULL;
  int is_crash = 1;
  char zeros_buf[PAGE_DATA_SZ];
  long fulllen = 0;

  buf = bp_get_buffer(NULL, BP_BUF_REQUIRED);

  if (!no_free_set)
    dbs_locate_free_bit (storage, start, &array, &page, &inx, &bit);
  if (no_free_set
      || (0 != (array[inx] & (1 << bit))
	  && !gethash (DP_ADDR2VOID (start),
		       storage->dbs_cpt_remap)))
    {
      buf->bd_page = buf->bd_physical_page = start;
      buf->bd_storage = storage;
      if (WI_ERROR == buf_disk_read (buf))
	{
	  log_error ("Read of page %ld failed", start);
	  if (is_crash)
	    {
	      is_crash++;
	      goto fin;
	    }
	}
      else
	{
	  buf->bd_readers++;
	  if (DPF_BLOB != SHORT_REF (buf->bd_buffer + DP_FLAGS))
	    {
	      log_error ("Non-blob page %ld remap %ld in logging blob", buf->bd_page, buf->bd_physical_page);
	      if (is_crash)
		{
		  is_crash++;
		  goto fin;
		}
	    }
	  else
	    {
	      long len = LONG_REF (buf->bd_buffer + DP_BLOB_LEN);
	      blob_pages_logged++;
	      CATCH_WRITE_FAIL (log)
		{
		  session_buffered_write_char (bl->bl_blob_dtp, log);
		  print_long (len, log);
		  fulllen=len;
		  session_buffered_write (log, (char *) buf->bd_buffer + DP_DATA, len);
		}
	      END_WRITE_FAIL (log);
	      page_leave_inner (buf);
	      if (!DKSESSTAT_ISSET (log, SST_OK))
		{
		  return -1;		/* out of disk */
		}
	    }
	}
    }
  else
    {
      log_error ("Free blob page %ld", start);
      if (is_crash)
	{
	  is_crash++;
	  goto fin;
	}
    }
 fin:
  if (is_crash > 1)
    {
      memset (zeros_buf, 0, PAGE_DATA_SZ);
      CATCH_WRITE_FAIL (log)
	{
	  session_buffered_write_char (bl->bl_blob_dtp, log);
	  print_long (should_be_len, log);
	  session_buffered_write (log, (char*) zeros_buf, should_be_len);
	}
      END_WRITE_FAIL (log);
    }
  return fulllen;
}


int
blob_write_crash_log_via_dir (dk_session_t * log, blob_log_t *  bl)
{
  dp_addr_t start = bl->bl_start;
  dp_addr_t page;
  int inx = 0, bit = 0;
  uint32 *array = NULL;
  dbe_storage_t * storage = wi_inst.wi_master;
  static buffer_desc_t * buf = NULL;
  int is_crash = 0;
  long fulllen = 0;
  dp_addr_t dir_start = bl->bl_dir_start;
  if (!bl->bl_it)
    is_crash = 1;

  if (!buf)
    buf = bp_get_buffer(NULL, BP_BUF_REQUIRED);

  while (dir_start)
    {
      if (!no_free_set)
	dbs_locate_free_bit (storage, start, &array, &page, &inx, &bit);
      if (no_free_set
	  || (0 != (array[inx] & (1 << bit))
	      && !gethash (DP_ADDR2VOID (start),
			   storage->dbs_cpt_remap)))
	{
	  buf->bd_page = buf->bd_physical_page = start;
	  buf->bd_storage = storage;
	  if (WI_ERROR == buf_disk_read (buf))
	    {
	      log_error ("Read of page %ld failed", start);
	      if (is_crash)
		{
		  is_crash++;
		  goto fin;
		}
	    }
	  else
	    {
#ifdef DBG_BLOB_PAGES_ACCOUNT
	      db_dbg_account_add_page (start);
#endif
	      buf->bd_readers++;
	      if (DPF_BLOB == SHORT_REF (buf->bd_buffer + DP_FLAGS))
		return -2;
	      if (DPF_BLOB_DIR != SHORT_REF (buf->bd_buffer + DP_FLAGS))
		{
		  log_error ("Non-blob-dir page %ld remap %ld in logging blob. %d", buf->bd_page, buf->bd_physical_page,  SHORT_REF (buf->bd_buffer + DP_FLAGS));
		  if (is_crash)
		    {
		      is_crash++;
		      goto fin;
		    }
		}
	      else
		{
		  dp_addr_t next = LONG_REF (buf->bd_buffer + DP_OVERFLOW);
		  long items_on_page = (LONG_REF (buf->bd_buffer + DP_BLOB_LEN)) / sizeof (dp_addr_t);
		  long nidx;
		  long should_be_len = PAGE_DATA_SZ;
		  for (nidx=0; nidx<items_on_page; nidx++)
		    {
		      dp_addr_t blob_start = LONG_REF (buf->bd_buffer + DP_DATA + nidx * sizeof (dp_addr_t));
		      if (!next && nidx+1 == items_on_page)
			should_be_len = bl->bl_dir_start % PAGE_DATA_SZ;

		      fulllen += blob_write_crash_recover_log_1 (log, bl, blob_start, should_be_len);
		    }
		  page_leave_inner (buf);
		  dir_start = next;
		  if (!DKSESSTAT_ISSET (log, SST_OK))
		    {
		      return -1;		/* out of disk */
		    }
		}
	    }
	}
      else
	{
	  log_error ("Free blob page %ld", start);
	  if (is_crash)
	    {
	      is_crash++;
	      goto fin;
	    }
	}
    }
 fin:
  if (is_crash > 1)
    {
	if (!fulllen)
	  {
             CATCH_WRITE_FAIL (log)
               {
                 session_buffered_write_char (bl->bl_blob_dtp, log);
                 print_long (0, log);
               }
             END_WRITE_FAIL (log);
	  }
	{
	  dbe_column_t * col = sch_id_to_column (wi_inst.wi_schema, bl->bl_col_id);
	  log_error ("Blob %s (table %s) is reduced to %ld length (should be %ld)",
		     col ? col->col_name : NULL,
		     bl->bl_table_name,
		     fulllen,
		     bl->bl_diskbytes);
	}
    }
  CATCH_WRITE_FAIL (log)
    session_buffered_write_char (0, log);
  END_WRITE_FAIL (log);
  return 0;

}


int
blob_write_crash_log (lock_trx_t * lt /* unused */, dk_session_t * log, blob_log_t *  bl)
{
  dp_addr_t start = bl->bl_start;
  dp_addr_t page;
  int inx = 0, bit = 0;
  uint32 *array = NULL;
  dbe_storage_t * storage = wi_inst.wi_master;
  static buffer_desc_t * buf = NULL;
  int is_crash = 0;
  long fulllen = 0;
  if (!bl->bl_it)
    is_crash = 1;

  if (is_crash && bl->bl_dir_start)
    {
      int res = blob_write_crash_log_via_dir (log, bl);
      if (res != -2)
	return res;
    }

  lt = NULL; /* unused */
  if (!buf)
    buf = bp_get_buffer(NULL, BP_BUF_REQUIRED);

  while (start)
    {
      if (!no_free_set)
	dbs_locate_free_bit (storage, start, &array, &page, &inx, &bit);
      if (no_free_set
	  || (0 != (array[inx] & (1 << bit))
	      && !gethash (DP_ADDR2VOID (start),
			   storage->dbs_cpt_remap)))
	{
	  buf->bd_page = buf->bd_physical_page = start;
	  buf->bd_storage = storage;
	  if (WI_ERROR == buf_disk_read (buf))
	    {
	      log_error ("Read of page %ld failed", start);
	      if (is_crash)
		{
		  is_crash++;
		  goto fin;
		}
	      LEAVE_TXN;
	      STRUCTURE_FAULT;
	    }
	  else
	    {
#ifdef DBG_BLOB_PAGES_ACCOUNT
	      db_dbg_account_add_page (start);
#endif
	      buf->bd_readers++;
	      if (DPF_BLOB != SHORT_REF (buf->bd_buffer + DP_FLAGS))
		{
		  log_error ("Non-blob page %ld remap %ld in logging blob. %d", buf->bd_page, buf->bd_physical_page,  SHORT_REF (buf->bd_buffer + DP_FLAGS));
		  if (is_crash)
		    {
		      is_crash++;
		      goto fin;
		    }
		  LEAVE_TXN;
		  STRUCTURE_FAULT;
		}
	      else
		{
		  long len = LONG_REF (buf->bd_buffer + DP_BLOB_LEN);
		  long next = LONG_REF (buf->bd_buffer + DP_OVERFLOW);
		  blob_pages_logged++;
		  CATCH_WRITE_FAIL (log)
		    {
		      session_buffered_write_char (bl->bl_blob_dtp, log);
		      print_long (len, log);
		      fulllen+=len;
		      session_buffered_write (log, (char *) buf->bd_buffer + DP_DATA, len);
		    }
		  END_WRITE_FAIL (log);
		  page_leave_inner (buf);
		  start = next;
		  if (!DKSESSTAT_ISSET (log, SST_OK))
		    {
		      return -1;		/* out of disk */
		    }
		}
	    }
	}
      else
	{
	  log_error ("Free blob page %ld", start);
	  if (is_crash)
	    {
	      is_crash++;
	      goto fin;
	    }
	  LEAVE_TXN;
 	  STRUCTURE_FAULT;
	}
    }
 fin:
  if (is_crash > 1)
    {
      dbe_column_t * col = sch_id_to_column (wi_inst.wi_schema, bl->bl_col_id);
      if (!fulllen)
	{
	  CATCH_WRITE_FAIL (log)
	    {
	      session_buffered_write_char (bl->bl_blob_dtp, log);
	      print_long (0, log);
	    }
	  END_WRITE_FAIL (log);
	}
      log_error ("Blob %s (table %s) is reduced to %ld length (should be %ld)",
		 col ? col->col_name : NULL,
		 bl->bl_table_name,
		 fulllen,
		 bl->bl_diskbytes);
    }
  CATCH_WRITE_FAIL (log)
    session_buffered_write_char (0, log);
  END_WRITE_FAIL (log);
  return 0;

}


extern dk_mutex_t * log_write_mtx;

int
blob_write_log (lock_trx_t * lt, dk_session_t * log, blob_log_t * bl)
{
  if (bl)
    {
      dp_addr_t start = bl->bl_start;
      dtp_t blob_dtp = bl->bl_blob_dtp;
      buffer_desc_t *buf;
      it_cursor_t *tmp_itc;
      ASSERT_IN_MTX (log_write_mtx);
      if (!bl->bl_it)
	{ /* only if crash dump */
	  return blob_write_crash_log (lt, log, bl);
	}
      else
	{
	  tmp_itc = itc_create (NULL, lt);
	  itc_from_it (tmp_itc, bl->bl_it);
	}
      while (start)
	{
	  volatile long len, next;
	  ITC_IN_MAP (tmp_itc);
	  buf = page_fault_map_sem (tmp_itc, start, PF_STAY_ATOMIC);
	  buf->bd_readers++;
	  ITC_LEAVE_MAP (tmp_itc);
	  if (DPF_BLOB != SHORT_REF (buf->bd_buffer + DP_FLAGS))
	    {
	      log_error ("Non-blob page %ld remap %ld in logging blob",
			 buf->bd_page, buf->bd_physical_page);
	      ITC_IN_MAP (tmp_itc);
	      page_leave_inner (buf);
	      ITC_LEAVE_MAP (tmp_itc);
	      break;
	    }
#ifdef DBG_BLOB_PAGES_ACCOUNT
	  db_dbg_account_add_page (start);
#endif
	  len = LONG_REF (buf->bd_buffer + DP_BLOB_LEN);
	  next = LONG_REF (buf->bd_buffer + DP_OVERFLOW);
	  blob_pages_logged++;
	  CATCH_WRITE_FAIL (log)
	    {
	      session_buffered_write_char (blob_dtp, log);
	      print_long (len, log);
	      session_buffered_write (log, (char *) buf->bd_buffer + DP_DATA, len);
	    }
	  END_WRITE_FAIL (log);
	  ITC_IN_MAP (tmp_itc);
	  page_leave_inner (buf);
	  start = next;
	  if (!DKSESSTAT_ISSET (log, SST_OK))
	    {
	      itc_free (tmp_itc);
	      return -1;		/* out of disk */
	    }
	}
      itc_free (tmp_itc);
    }
  CATCH_WRITE_FAIL (log)
    {
      session_buffered_write_char (0, log);
    }
  END_WRITE_FAIL (log);
  ASSERT_IN_MTX (log_write_mtx);
  return 0;
}


void
lt_write_blob_log (lock_trx_t * lt, dk_session_t * log)
{
  DO_SET (blob_log_t *, blob_log, &lt->lt_blob_log)
  {
#if 0 /*GK: unused*/
    if (0 == lt->lt_blob_log_start)
      lt->lt_blob_log_start = log->dks_bytes_sent + wi_inst.wi_master->dbs_log_length;
#endif
    blob_write_log (lt, log, blob_log);
    if (!DKSESSTAT_ISSET (log, SST_OK))
      return;
  }
  END_DO_SET ();
  if (lt->lt_blob_log)
    {
      CATCH_WRITE_FAIL (log)
      {
	session_flush (log);
      }
      END_WRITE_FAIL (log);
    }
}

#if 0 /* this was */
#define bh_string_output(isp, lt, bh, omit) \
((box_tag (bh) == DV_BLOB_WIDE_HANDLE) ? \
    bh_string_output_w (isp, lt, bh, omit) : \
    bh_string_output_n (isp, lt, bh, omit))
#else
#define bh_string_output(lt, bh, omit) \
((box_tag (bh) == DV_BLOB_WIDE_HANDLE) ? \
    bh_string_output_w (lt, bh, omit) : \
    bh_string_output_n (lt, bh, omit, 0))
#endif

#ifdef DBG_BLOB_PAGES_ACCOUNT
int is_reg;
#endif

dk_session_t *
bh_string_output_n (/* this was before 3.0: index_space_t * isp, */ lock_trx_t * lt, blob_handle_t * bh, int omit, int free_buffs)
{
  /* take current page at current place and make string of
     n bytes from the place and write to client */
  /*caddr_t page_string;*/
  dk_session_t *string_output = NULL;
  dp_addr_t start = bh->bh_current_page;
  buffer_desc_t *buf = NULL;
  long from_byte = bh->bh_position;
  long bytes_filled = 0, bytes_on_page;
#if 0 /* this was */
  it_cursor_t *tmp_itc = itc_create (isp, lt);
#else
  it_cursor_t *tmp_itc = itc_create (bh->bh_it->it_commit_space, lt);
  itc_from_it (tmp_itc, bh->bh_it);
#endif
  while (start)
    {
      long len, next;
      if (NULL == string_output)
	string_output = strses_allocate();

      ITC_IN_MAP (tmp_itc);
#ifdef DBG_BLOB_PAGES_ACCOUNT
      if (is_reg)
	db_dbg_account_add_page (start);
#endif
      if (!page_wait_blob_access (tmp_itc, start, &buf, PA_READ, bh, 0))
	{
	  ITC_LEAVE_MAP (tmp_itc);
	  break;
	}
      len = LONG_REF (buf->bd_buffer + DP_BLOB_LEN);
      bytes_on_page = len - from_byte;
      if (bytes_on_page)
	{
	  /* dbg_printf (("Read blob page %ld, %ld bytes.\n", start,
	     bytes_on_page)); */
	  if (!omit)
	      session_buffered_write (string_output, (caddr_t)(buf->bd_buffer + DP_DATA + from_byte), bytes_on_page);

	  bytes_filled += bytes_on_page;
	  from_byte += bytes_on_page;
	}
      next = LONG_REF (buf->bd_buffer + DP_OVERFLOW);
      if (start == bh->bh_page)
	{
	  dp_addr_t t = LONG_REF (buf->bd_buffer + DP_PARENT);
	  if (bh->bh_dir_page && t != bh->bh_dir_page)
	    log_info ("Mismatch in directory page ID %d(%x) vs %d(%x).",
		t, t, bh->bh_dir_page, bh->bh_dir_page);
	  bh->bh_dir_page = t;
	}
      ITC_IN_MAP (tmp_itc);
#if 1
      if (free_buffs)
	{
	  if (remhash (DP_ADDR2VOID (buf->bd_page), buf->bd_space->isp_dp_to_buf))
	    {
	      buf_set_last (buf);
	    }
	}
#endif
      page_leave_inner (buf);
      ITC_LEAVE_MAP (tmp_itc);
      bh->bh_current_page = next;
      bh->bh_position = 0;
      from_byte = 0;
      start = next;
    }
  itc_free (tmp_itc);
  return (string_output);
}

#if 0 /* this was */
#define bh_string_list(isp, lt, bh, get_bytes, omit) \
((box_tag (bh) == DV_BLOB_WIDE_HANDLE) ? \
    bh_string_list_w (isp, lt, bh, get_bytes, omit) : \
    bh_string_list_n (isp, lt, bh, get_bytes, omit))
#else
#define bh_string_list(lt, bh, get_bytes, omit) \
((box_tag (bh) == DV_BLOB_WIDE_HANDLE) ? \
    bh_string_list_w (lt, bh, get_bytes, omit) : \
    bh_string_list_n (lt, bh, get_bytes, omit))
#endif

dk_set_t
bh_string_list_n (/* this was before 3.0: index_space_t * isp, */ lock_trx_t * lt, blob_handle_t * bh,
    long get_bytes, int omit)
{
  /* take current page at current place and make string of
     n bytes from the place and write to client */
  caddr_t page_string;
  dk_set_t string_list = NULL;
  dp_addr_t start = bh->bh_current_page;
  buffer_desc_t *buf = NULL;
  long from_byte = bh->bh_position;
  long bytes_filled = 0, bytes_on_page;
#if 0 /* this was */
  it_cursor_t *tmp_itc = itc_create (isp, lt);
#else
  it_cursor_t *tmp_itc = itc_create (bh->bh_it->it_commit_space, lt);
  itc_from_it (tmp_itc, bh->bh_it);
#endif
  while (start)
    {
      long len, next;
      uint32 timestamp;
      int type;

      if (!page_wait_blob_access (tmp_itc, start, &buf, PA_READ, bh, 1))
	break;
      type = SHORT_REF (buf->bd_buffer + DP_FLAGS);
      timestamp = LONG_REF (buf->bd_buffer + DP_BLOB_TS);

      if ((bh->bh_timestamp != BH_ANY) && (bh->bh_timestamp != timestamp))
	{
	  page_leave_inner (buf);
	  ITC_LEAVE_MAP (tmp_itc);
	  return BH_DIRTYREAD;
	}

      if ((DPF_BLOB != type) &&
	  (DPF_BLOB_DIR != type))
	{
	  page_leave_inner (buf);
	  ITC_LEAVE_MAP (tmp_itc);
	  dbg_printf (("wrong blob type\n"));
	  return 0;
	}

      len = LONG_REF (buf->bd_buffer + DP_BLOB_LEN);
      bytes_on_page = MIN (len - from_byte, get_bytes);
      if (bytes_on_page)
	{
	  if (!omit)
	    {
	      if (DK_MEM_RESERVE)
		{
		  SET_DK_MEM_RESERVE_STATE (lt);
		  itc_bust_this_trx (tmp_itc, &buf, ITC_BUST_THROW);
		}
	      page_string = dk_alloc_box (bytes_on_page + 1, DV_LONG_STRING);
	      memcpy (page_string, buf->bd_buffer + DP_DATA + from_byte,
		  bytes_on_page);
	      page_string[bytes_on_page] = 0;
	      dk_set_push (&string_list, page_string);
	    }
	  bytes_filled += bytes_on_page;
	  get_bytes -= bytes_on_page;
	  from_byte += bytes_on_page;
	}
      next = LONG_REF (buf->bd_buffer + DP_OVERFLOW);
      page_leave_inner (buf);
      ITC_LEAVE_MAP (tmp_itc);
      if (0 == get_bytes)
	{
	  bh->bh_position = from_byte;
	  break;
	}
      bh->bh_current_page = next;
      bh->bh_position = 0;
      from_byte = 0;
      start = next;
    }
  itc_free (tmp_itc);
  return (dk_set_nreverse (string_list));
}

#if 0
int
bh_fill_buffer_from_blob (index_space_t * isp, lock_trx_t * lt, blob_handle_t * bh,
    caddr_t outbuf, long get_bytes)
{
  /* take current page at current place and read string of
     n bytes from the place and place them in the outbuf.
     Return the number of bytes read.
   */
  dp_addr_t start = bh->bh_current_page;
  buffer_desc_t *buf = NULL;
  long from_byte = bh->bh_position;
  long bytes_filled = 0, bytes_on_page;
  it_cursor_t *tmp_itc = itc_create (isp, lt);
  caddr_t ptr = outbuf;

  while (start)
    {
      long len, next;
      if (!page_wait_blob_access (tmp_itc, start, &buf, PA_READ, bh, 1))
	break;
      ITC_LEAVE_MAP (tmp_itc);
      len = LONG_REF (buf->bd_buffer + DP_BLOB_LEN);
      bytes_on_page = MIN (len - from_byte, get_bytes);
      if (bytes_on_page)
	{
	  memcpy (ptr, buf->bd_buffer + DP_DATA + from_byte,
	      bytes_on_page);
	  ptr += bytes_on_page;
	  bytes_filled += bytes_on_page;
	  get_bytes -= bytes_on_page;
	  from_byte += bytes_on_page;
	}
      next = LONG_REF (buf->bd_buffer + DP_OVERFLOW);
      ITC_IN_MAP (tmp_itc);
      page_leave_inner (buf);
      ITC_LEAVE_MAP (tmp_itc);
      if (0 == get_bytes)
	{
	  bh->bh_position = from_byte;
	  break;
	}
      bh->bh_current_page = next;
      bh->bh_position = 0;
      from_byte = 0;
      start = next;
    }
  itc_free (tmp_itc);
  return (ptr - outbuf);
}
#endif

int
bh_read_ahead (index_space_t * isp, lock_trx_t * lt, blob_handle_t * bh, unsigned from, unsigned to)
{
  long pidx, n;
  ra_req_t *pra = NULL;
  it_cursor_t *tmp_itc = NULL;
  int dtp = DV_TYPE_OF (bh);
  unsigned length;
#ifdef DEBUG
  if (!IS_BLOB_HANDLE_DTP (dtp))
    {
      char buf[100];
      snprintf (buf, sizeof (buf), "Illegal params in 'bh_read_ahead': dtp=%d", dtp);
      GPF_T1 (buf);
    }
#endif

  if (!bh->bh_pages)
    return 0;
  length = (unsigned) bh->bh_diskbytes;
  if (DV_BLOB_WIDE_HANDLE == dtp)
    {
      if (bh->bh_diskbytes && 0 == from && to == bh->bh_length)
	to = (unsigned) bh->bh_diskbytes;
      else
	return 0;
    }

  while (from > to || to > length)
    {
      log_info ("Strange params in blob read ahead: from=%d to=%d.", from, to);
      if (from > to)
	{
	  n = to;
	  to = from;
	  from = n;
	}
      if (to >= length)
	to = length;
    }
  if (from != to)
    {
      int quota;
      if (!bh->bh_page_dir_complete)
	{
	  bh_fetch_dir (isp, lt, bh);
	}
      tmp_itc = itc_create (isp, lt);
      itc_from_it (tmp_itc, bh->bh_it);
      quota = itc_ra_quota (tmp_itc);
      quota = MIN (RA_MAX_BATCH, quota);
      pidx = from / PAGE_DATA_SZ;
      n = (to - 1) / PAGE_DATA_SZ - pidx + 1;
      pra = (ra_req_t *) dk_alloc_box_zero (sizeof (*pra), DV_CUSTOM);	/* it will be freed in read_ahead2 */
      pra->ra_fill = MIN (quota, n);
      memcpy (pra->ra_dp,
	  bh->bh_pages + pidx,
	  pra->ra_fill * sizeof (dp_addr_t));
      TC (tc_blob_ra);
      tc_blob_ra_size += pra->ra_fill;
      itc_read_ahead_blob (tmp_itc, pra);
      itc_free (tmp_itc);
    }
  return 0;
}


static dp_addr_t
bh_find_page (blob_handle_t * bh, size_t offset)
{
  size_t pidx, n;
  if (!bh->bh_pages)
    return 0;
  pidx = offset / PAGE_DATA_SZ;
  n = box_length (bh->bh_pages) / sizeof (dp_addr_t);
  if (n <= pidx)
    log_info ("Attempt to read inconsistent blob page with idx = %d .", pidx);
  return bh->bh_pages[pidx];
}


int
blob_read_dir (it_cursor_t * tmp_itc, dp_addr_t ** pages, int * is_complete, dp_addr_t start)
{
  int error = 0;
  buffer_desc_t *buf = NULL;
  long items_on_page;
  int n;
  dk_set_t pages_list = NULL;
  if (*is_complete)
    return;
  while (start)
    {
      long next;
      if (!page_wait_blob_access (tmp_itc, start, &buf, PA_READ, NULL, 1))
	{
	  error = 1;
	break;
	}
      items_on_page = (LONG_REF (buf->bd_buffer + DP_BLOB_LEN)) / sizeof (dp_addr_t);
      if (items_on_page)
	{
	  int i;
	  for (i = 0; i < items_on_page; i++)
	    {
	      dk_set_push (&pages_list, (void *) (ptrlong) LONG_REF (buf->bd_buffer + DP_DATA + i * sizeof (dp_addr_t)));
	    }
	}
      next = LONG_REF (buf->bd_buffer + DP_OVERFLOW);
      page_leave_inner (buf);
      ITC_LEAVE_MAP (tmp_itc);
      start = next;
    }
  if (error)
    {
      dk_set_free (pages_list);
      return BLOB_FREE;
      }
  dk_free_box ((box_t) *pages);
  n = dk_set_length (pages_list);
  *is_complete = 1;
  if (n > 0)
    {
      int i;
      dp_addr_t pt;
      pages_list = dk_set_nreverse (pages_list);
/*???      dk_free_box ((caddr_t)(pages[0]));*/
      *pages = (dp_addr_t *) dk_alloc_box (sizeof (dp_addr_t) * n, DV_BIN);
      for (i = 0; (pt = (dp_addr_t) (ptrlong) dk_set_pop (&pages_list)) != 0 && i < n; i++)
	{
	  (*pages)[i] = pt;
	}
      dk_set_free (pages_list);
    }
  return;
}


int
bh_fetch_dir (index_space_t * isp /* unused */, lock_trx_t * lt, blob_handle_t * bh)
{
  it_cursor_t itc_auto;
  it_cursor_t * itc = &itc_auto;
  isp = NULL; /* unused */
  if (bh->bh_page_dir_complete)
    return 0;
  ITC_INIT (itc, isp, lt);
  itc_from_it (itc, bh->bh_it);
  return blob_read_dir (itc, &bh->bh_pages, &bh->bh_page_dir_complete, bh->bh_dir_page);
}


int
blob_check (blob_handle_t * bh)
{
  index_tree_t * it = bh->bh_it;
  int error = 0;
  if (!it)
    return BLOB_OK;
  if (bh->bh_pages)
    {
      int inx, n = box_length ((caddr_t) bh->bh_pages) / sizeof (dp_addr_t);
      dp_addr_t dp = bh->bh_dir_page;
      if (bh->bh_diskbytes && n != (((bh->bh_diskbytes - 1) / PAGE_DATA_SZ) + 1))
	{
	  error = 1;
	  log_info ("Blob disk bytes and page dir length disagree L=%d  bytes= %d dir pages=%d ", bh->bh_page, n, bh->bh_diskbytes);
	}
      if (!bh->bh_page_dir_complete && n > BL_DPS_ON_ROW)
	n = BL_DPS_ON_ROW;
      if (/* dp < 0 ||   -- just tell me how could it be? */ dp > it->it_storage->dbs_n_pages)
	{
	  error = 1;
	  log_info ("Out of range  blob dir page refd start = %d L=%d ", bh->bh_page, dp);
	}
      else if (dp && dbs_is_free_page (it->it_storage, dp))
	{
	  error = 1;
	  log_info ("Free blob dir page refd start = %d L=%d ", bh->bh_page, dp);
	}

      for (inx = 0; inx < n; inx++)
	{
	  dp = bh->bh_pages[inx];
	  if (dp <3 || dp > it->it_storage->dbs_n_pages)
	    {
	      error = 1;
	      log_info ("Out of range  blob page refd start = %d L=%d ", bh->bh_page, dp);
	    }
	  else if (dp && dbs_is_free_page (it->it_storage, dp))
	    {
	      error = 1;
	      log_info ("Free blob page refd start = %d L=%d ", bh->bh_page, dp);
	    }
	}
    }
  if (error)
    return BLOB_FREE;
  return BLOB_OK;
}


int
bl_check (blob_layout_t * bl)
{
  index_tree_t * it = bl->bl_it;
  int error = 0;
  if (!it)
    return BLOB_OK;
  if (bl->bl_pages)
    {
      int inx, n = box_length ((caddr_t) bl->bl_pages) / sizeof (dp_addr_t);
      dp_addr_t dp = bl->bl_dir_start;
      if (bl->bl_diskbytes && n != (((bl->bl_diskbytes - 1) / PAGE_DATA_SZ) + 1))
	{
	  error = 1;
	  log_info ("Blob disk bytes and page dir length disagree L=%d  bytes= %d dir pages=%d ", bl->bl_start, n, bl->bl_diskbytes);
	}
      if (!bl->bl_page_dir_complete && n > BL_DPS_ON_ROW)
	n = BL_DPS_ON_ROW;
      if (dp <0 || dp > it->it_storage->dbs_n_pages)
	{
	  error = 1;
	  log_info ("Out of range  blob dir page refd start = %d L=%d ", bl->bl_start, dp);
	}
      else if (dp && dbs_is_free_page (it->it_storage, dp))
	{
	  error = 1;
	  log_info ("Free blob dir page refd start = %d L=%d ", bl->bl_start, dp);
	}

      for (inx = 0; inx < n; inx++)
	{
	  dp = bl->bl_pages[inx];
	  if (dp <3 || dp > it->it_storage->dbs_n_pages)
	    {
	      error = 1;
	      log_info ("Out of range  blob page refd start = %d L=%d ", bl->bl_start, dp);
	    }
	  else if (dp && dbs_is_free_page (it->it_storage, dp))
	    {
	      error = 1;
	      log_info ("Free blob page refd start = %d L=%d ", bl->bl_start, dp);
	    }
	}
    }
  if (error)
    return BLOB_FREE;
  return BLOB_OK;
}


long
bh_write_out (lock_trx_t * lt, blob_handle_t * bh, dk_session_t * ses)
{
  /* take current page at current place and make string of
     n bytes from the place and write to client */
  dp_addr_t start = bh->bh_current_page;
  buffer_desc_t *buf;
  long from_byte = bh->bh_position;
  long bytes_filled = 0, bytes_on_page;
  it_cursor_t *tmp_itc = itc_create (NULL, lt);
  if (bh->bh_page == bh->bh_current_page
      && bh->bh_diskbytes > 2 * PAGE_DATA_SZ)
    {
      bh_read_ahead (bh->bh_it->it_commit_space, lt, bh, 0, MIN (5000000, bh->bh_diskbytes));
    }
  itc_from_it (tmp_itc, bh->bh_it);

  while (start)
    {
      long len, next;
      if (!page_wait_blob_access (tmp_itc, start, &buf, PA_READ, bh, 1))
        break;
      ITC_LEAVE_MAP (tmp_itc);
      len = LONG_REF (buf->bd_buffer + DP_BLOB_LEN);
      bytes_on_page = len - from_byte;
      if (bytes_on_page)
	{

	  CATCH_WRITE_FAIL (ses)
	    {
	      session_buffered_write (ses, (char *) (buf->bd_buffer + DP_DATA + from_byte), bytes_on_page);
	    }
	  FAILED
	    {
	      ITC_IN_MAP (tmp_itc);
	      page_leave_inner (buf);
	      log_info ("Writing a blob failed.");
	      ITC_LEAVE_MAP (tmp_itc);
	      break;
	    }
	  END_WRITE_FAIL (ses);

	  bytes_filled += bytes_on_page;
	  from_byte += bytes_on_page;
	}
      next = LONG_REF (buf->bd_buffer + DP_OVERFLOW);
      ITC_IN_MAP (tmp_itc);
      page_leave_inner (buf);
      ITC_LEAVE_MAP (tmp_itc);
      bh->bh_current_page = next;
      bh->bh_position = 0;
      from_byte = 0;
      start = next;
    }
  itc_free (tmp_itc);
  return (bytes_filled);
}


void
blob_send_bytes (lock_trx_t * lt, caddr_t bhp, long get_bytes,
    int send_position)
{
  blob_handle_t *bh = (blob_handle_t *) bhp;
  caddr_t arr;
  dk_set_t string_list =
  bh_string_list (/*NULL,*/ lt, (blob_handle_t *) bhp, get_bytes, 0);

  if (BH_DIRTYREAD == string_list)
    {
      caddr_t err = srv_make_new_error ("22023", "SR468",
	"Dirty read is detected, wrong BLOB HANDLE value.");
      PrpcAddAnswer (err, DV_ARRAY_OF_POINTER, 0, 0);
      dk_free_tree (err);
      return;
    }

  if (!string_list)
    {
      PrpcAddAnswer (NULL, DV_ARRAY_OF_POINTER, 0, 0);
      return;
    }
  if (send_position)
    {
      ptrlong *pos = (ptrlong *) dk_alloc_box (3 * sizeof (caddr_t), DV_ARRAY_OF_LONG);
      pos[0] = QA_BLOB_POS;
      pos[1] = bh->bh_current_page;
      pos[2] = bh->bh_position;
      string_list = dk_set_cons ((caddr_t) pos, string_list);
    }
  arr = (caddr_t) dk_set_to_array (string_list);
  dk_set_free (string_list);
  PrpcAddAnswer (arr, DV_ARRAY_OF_POINTER, FINAL, 0);
  dk_free_tree (arr);
}

/* FIXME */
caddr_t
blob_subseq (lock_trx_t * lt, caddr_t bhp, size_t from, size_t to)
{
  dtp_t bh_dtp = (dtp_t)DV_TYPE_OF (bhp);
  int isWide = (bh_dtp == DV_BLOB_WIDE_HANDLE);
  int sizeof_symbol = SIZEOF_SYMBOL_FOR_BLOB_HANDLE_DTP (bh_dtp);
  blob_handle_t *bh = (blob_handle_t *) bhp;
  size_t bytes;
  caddr_t out;
  long fill = 0;
  size_t pidx = from / PAGE_DATA_SZ;
  dk_set_t string_list = NULL;

#ifdef DEBUG
  if (NULL == lt || NULL == bhp)
    GPF_T1 ("Internal error in blob_subseq()");
#endif

  if (to >= bh->bh_length)
    to = bh->bh_length;
  if (from >= to)
    goto return_empty_string;
  /* Now it is true that (from < to) && (to <= bh->bh_length) */

  bytes = (to - from) * sizeof_symbol;
  if (bh->bh_ask_from_client)
    sqlr_new_error ("22023", "SR099",
	"BLOB submitted by client as SQL_DATA_AT_EXEC cannot be converted into anything."
	" It must first be stored into a BLOB column");
  if (bytes >= 10000000)
    sqlr_new_error ("22003", "SR100",
	"The requested subsequence of BLOB is longer than 10Mb, thus it cannot be stored as a string");
  bh->bh_current_page = bh->bh_page;
  if (!isWide)
    {
      bh->bh_position = (int) (from % PAGE_DATA_SZ);
      if (BLOB_FREE == bh_fetch_dir (NULL, lt, bh))
	{
	  sqlr_new_error ("22023", "SR099",
			  "Reading deleted blob in subseq.");
	}
      if (pidx)
	{
	  if (bh->bh_pages)
	    bh->bh_current_page = bh_find_page (bh, from);
	  else
	    {
	      if (from)
		{
		  bh->bh_position = 0;
		  bh_string_list (/*NULL,*/ lt, bh, (long) from, 1);
		}
	    }
	}
      bh_read_ahead (NULL, lt, bh, (unsigned) from, (unsigned) to);
    }
  else
    {
      if (0 != from)
	{

	  bh->bh_position = 0;
	  bh_string_list (/*NULL,*/ lt, bh, (long) from, 1);
	}
      else
	{
	  if (to == bh->bh_length)
	    bh_read_ahead (NULL, lt, bh, (unsigned) from, (unsigned) to);
	}
    }
  string_list = bh_string_list (/*NULL,*/ lt, bh, (long)(to - from), 0);
  bh->bh_current_page = bh->bh_page;
  bh->bh_position = 0;

  if (BH_DIRTYREAD == string_list)
    goto stub_for_corrupted_blob;
  if (DK_MEM_RESERVE)
    {
      caddr_t err;
      SET_DK_MEM_RESERVE_STATE (lt);
      MAKE_TRX_ERROR (lt->lt_error, err, LT_ERROR_DETAIL (lt));
      sqlr_resignal (err);
    }
  if (NULL == (out = dk_try_alloc_box (bytes + sizeof_symbol, isWide ? DV_LONG_WIDE : DV_LONG_STRING)))
    {
      caddr_t err = NULL;
      SET_DK_MEM_RESERVE_STATE (lt);
      MAKE_TRX_ERROR (lt->lt_error, err, LT_ERROR_DETAIL (lt));
      sqlr_resignal (err);
    }

  while (NULL != string_list)
  {
    caddr_t fragment = (caddr_t) dk_set_pop(&string_list);
    long len = box_length (fragment) - sizeof_symbol;
    if (fill+len > bytes)
      {
	dk_free_box (fragment);
	goto stub_for_corrupted_blob;
      }
    memcpy (out + fill, fragment, len);
    fill += len;
    dk_free_box (fragment);
  }

  if (fill != bytes)
    goto stub_for_corrupted_blob;	/* see below */

  if (isWide)
    ((wchar_t *) out)[bytes / sizeof (wchar_t)] = 0;
  else
    out[bytes] = 0;
  return out;

/* If blob handle references to a field of deleted row, or in case of internal error, we should return empty string */
stub_for_corrupted_blob:
  dk_free_box (out);
  while (NULL != string_list)
    dk_free_box ((box_t) dk_set_pop(&string_list));
  log_info ("Attempt to get subsequence from invalid blob at page %d, %ld bytes expected, %ld retrieved%s",
    bh->bh_page, bytes, fill,
    ((0 == fill) ? "; it may be access to deleted page." : "") );

return_empty_string:
  return dk_alloc_box_zero (sizeof_symbol, (dtp_t)(isWide ? DV_WIDE : DV_SHORT_STRING));
}

caddr_t
blob_to_string_isp (lock_trx_t * lt, index_space_t *isp, caddr_t bhp)
{
  dtp_t bh_dtp = (dtp_t)DV_TYPE_OF (bhp);
  int isWide = (bh_dtp == DV_BLOB_WIDE_HANDLE);
  int sizeof_symbol = SIZEOF_SYMBOL_FOR_BLOB_HANDLE_DTP (bh_dtp);
  blob_handle_t *bh = (blob_handle_t *) bhp;
  size_t bytes = bh->bh_length * sizeof_symbol;
  caddr_t out;
  long fill = 0;
  dk_set_t string_list;
  if (bh->bh_ask_from_client)
    sqlr_new_error ("22023", "SR101",
	"BLOB submitted by client as SQL_DATA_AT_EXEC cannot be converted into anything."
	       " It must first be stored into a BLOB column");
  bh->bh_current_page = bh->bh_page;
  bh->bh_position = 0;

  if (!bytes)
    goto return_empty_string;		/* see below */

  if  (BLOB_FREE == bh_fetch_dir (isp, lt, bh))
    {

          sqlr_new_error ("22023", "SR099",
			  "Reading deleted blob in blob_to_string");
    }
  bh_read_ahead (isp, lt, bh, 0, (unsigned) bh->bh_length);

  string_list = bh_string_list (/* isp, */ lt, bh,
      10000000, 0);		/* up to 10MB as varchar */
  bh->bh_current_page = bh->bh_page;
  bh->bh_position = 0;

  if (!string_list || (BH_DIRTYREAD == string_list))
    sqlr_new_error ("22023", "SR469",
	"Dirty read is detected, wrong BLOB HANDLE value.");

  if (DK_MEM_RESERVE)
    {
      caddr_t err;
      SET_DK_MEM_RESERVE_STATE (lt);
      MAKE_TRX_ERROR (lt->lt_error, err, LT_ERROR_DETAIL (lt));
      sqlr_resignal (err);
    }
  if (NULL == (out = dk_try_alloc_box (bytes + sizeof_symbol, isWide ? DV_LONG_WIDE : DV_LONG_STRING)))
    {
      caddr_t err;
      SET_DK_MEM_RESERVE_STATE (lt);
      MAKE_TRX_ERROR (lt->lt_error, err, LT_ERROR_DETAIL (lt));
      sqlr_resignal (err);
    }

  while (NULL != string_list)
  {
    caddr_t fragment = (box_t) dk_set_pop(&string_list);
    long len = box_length (fragment) - sizeof_symbol;
    if (fill+len > bytes)
      {
	dk_free_box (fragment);
	goto stub_for_corrupted_blob;
      }
    memcpy (out + fill, fragment, len);
    fill += len;
    dk_free_box (fragment);
  }

  if (fill != bytes)
    goto stub_for_corrupted_blob;	/* see below */

  if (isWide)
    ((wchar_t *) out)[bytes / sizeof (wchar_t)] = 0;
  else
    out[bytes] = 0;
  return out;

/* If blob handle references to a field of deleted row, or in case of internal error, we should return empty string */
stub_for_corrupted_blob:
  dk_free_box (out);
  while (NULL != string_list)
    dk_free_box ((box_t) dk_set_pop(&string_list));
  log_info ("Attempt to convert invalid blob to string at page %d, %ld bytes expected, %ld retrieved%s",
    bh->bh_page, bytes, fill,
    ((0 == fill) ? "; it may be access to deleted page." : "") );

return_empty_string:
  return dk_alloc_box_zero (sizeof_symbol, (dtp_t)(isWide ? DV_WIDE : DV_SHORT_STRING));
}


caddr_t
blob_to_string (lock_trx_t * lt, caddr_t bhp)
{
  blob_handle_t * bh = (blob_handle_t *) bhp;
  return blob_to_string_isp (lt, bh->bh_it ? bh->bh_it->it_commit_space : NULL, bhp);
}


dk_session_t *
blob_to_string_output (lock_trx_t * lt, caddr_t bhp)
{
  blob_handle_t * bh = (blob_handle_t *) bhp;
  return blob_to_string_output_isp (lt, bh->bh_it ? bh->bh_it->it_commit_space : NULL, bhp);
}


dk_session_t *
blob_to_string_output_isp (lock_trx_t * lt, index_space_t *isp, caddr_t bhp)
{
  dtp_t bh_dtp = (dtp_t)DV_TYPE_OF (bhp);
  /*int isWide = (bh_dtp == DV_BLOB_WIDE_HANDLE);*/
  int sizeof_symbol = SIZEOF_SYMBOL_FOR_BLOB_HANDLE_DTP (bh_dtp);
  blob_handle_t *bh = (blob_handle_t *) bhp;
  size_t bytes = bh->bh_length * sizeof_symbol;
  long fill = 0;
  dk_session_t *res;
  if (bh->bh_ask_from_client)
    sqlr_new_error ("22023", "SR470",
	"BLOB submitted by client as SQL_DATA_AT_EXEC cannot be converted into anything."
	       " It must first be stored into a BLOB column");
  bh->bh_current_page = bh->bh_page;
  bh->bh_position = 0;

  if (!bytes)
    goto return_empty_string;		/* see below */

  bh_fetch_dir (isp, lt, bh);
  bh_read_ahead (isp, lt, bh, 0, (unsigned) bh->bh_length);

  res = bh_string_output (/*isp,*/ lt, bh, 0);

  if (NULL == res)
    goto stub_for_corrupted_blob;	/* see below */

  bh->bh_current_page = bh->bh_page;
  bh->bh_position = 0;

  fill = strses_length(res);
  if (fill != bytes)
    goto stub_for_corrupted_blob;	/* see below */

/*  if (isWide)
    {
      for (fill=0;fill<sizeof (wchar_t);fill++)
	session_buffered_write_char ('\0', res);
    }
  else
    session_buffered_write_char ('\0', res);*/
  return res;

/* If blob handle references to a field of deleted row, or in case of internal error, we should return empty string */
stub_for_corrupted_blob:
  if (NULL != res)
    strses_free (res);
  log_info ("Attempt to convert invalid blob to string_output at page %d, %ld bytes expected, %ld retrieved%s",
    bh->bh_page, bytes, fill,
    ((0 == fill) ? "; it may be access to deleted page." : "") );

return_empty_string:
  return strses_allocate ();
}


#if 0
/*kgeorge: obsolete - now reading done as string session*/
caddr_t
bloblike_pages_to_string (dbe_storage_t * dbs, lock_trx_t * lt, dp_addr_t start)
{
  blob_handle_t bh;
  caddr_t out;
  long fill = 0, bytes = 0;
  dk_set_t string_list;
  memset (&bh, 0, sizeof (blob_handle_t));
  bh.bh_current_page = bh.bh_page = start;
  bh.bh_position = 0;
  bh.bh_it = dbs->dbs_cpt_tree;
  bh.bh_timestamp = BH_ANY;
  string_list = bh_string_list_n (/* NULL, */ lt, &bh,
      10000000, 0);		/* up to 10MB as varchar */
  bh.bh_current_page = bh.bh_page;
  bh.bh_position = 0;

  DO_SET (caddr_t, str, &string_list)
  {
    bytes += box_length (str) - 1;
  }
  END_DO_SET ();

  out = dk_alloc_box (bytes + 1, DV_LONG_STRING);

  DO_SET (caddr_t, str, &string_list)
  {
    long len = box_length (str) - 1;
    memcpy (out + fill, str, len);
    fill += len;
    dk_free_box (str);
  }
  END_DO_SET ();
  dk_set_free (string_list);

  out[bytes] = 0;
  return out;
}
#endif


dk_session_t *
bloblike_pages_to_string_output (dbe_storage_t * dbs, lock_trx_t * lt, dp_addr_t start)
{
  blob_handle_t bh;
  dk_session_t *out;

  memset (&bh, 0, sizeof (blob_handle_t));
  bh.bh_current_page = bh.bh_page = start;
  bh.bh_position = 0;
  bh.bh_it = dbs->dbs_cpt_tree;
  bh.bh_timestamp = BH_ANY;

  /*bh_fetch_dir (isp, lt, &bh);
  bh_read_ahead (isp, lt, &bh, 0, bh->bh_length);*/

#ifdef DBG_BLOB_PAGES_ACCOUNT
  is_reg = 1;
#endif
  out = bh_string_output_n (lt, &bh, 0, 1);
#ifdef DBG_BLOB_PAGES_ACCOUNT
  is_reg = 0;
#endif
  if (NULL == out)
    goto stub_for_corrupted_blob;	/* see below */

  return out;

stub_for_corrupted_blob:
  if (NULL != out)
    strses_free (out);
  log_info ("Attempt to convert invalid blob to string_output at page %d", start);

  return strses_allocate ();
}


#if 0
int
bh_next_page (query_instance_t * qi, blob_handle_t * bh,
    buffer_desc_t ** buf_ret, long do_leave, char **data, int *data_len)
{
  /* return 1 if there is a next page, 0 otherwise */
  dp_addr_t start = bh->bh_current_page;
  buffer_desc_t *buf;
  it_cursor_t *tmp_itc;

  if (*buf_ret)
    {
      IN_PAGE_MAP;
      page_leave_inner (*buf_ret);
      LEAVE_PAGE_MAP;
      if (do_leave)
	return 0;
    }

  if (start)
    {
      tmp_itc = itc_create (qi->qi_space, qi->qi_trx);
      if (!page_wait_blob_access (tmp_itc, start, &buf, PA_READ, bh, 1))
	return 0;
      *data_len = LONG_REF (buf->bd_buffer + DP_BLOB_LEN);
      bh->bh_current_page = LONG_REF (buf->bd_buffer + DP_OVERFLOW);
      *data = (char *) (buf->bd_buffer + DP_DATA);
      *buf_ret = buf;
      itc_free (tmp_itc);
      return 1;
    }
  return 0;
}
#endif







void
blob_outline (it_cursor_t * itc, dbe_col_loc_t * cl, db_buf_t row, int pos, int len, int *end_pos, caddr_t * err_ret)
{
  blob_handle_t * bh;
  db_buf_t inlined = row + pos;
  dbe_key_t * key = itc->itc_row_key;
  long wide_l, saved;
  buffer_desc_t *buf;
  dtp_t dtp = inlined[0];
  ITC_LEAVE_MAP (itc);
  buf = isp_new_page (itc->itc_space, itc->itc_page, DPF_BLOB, 0, 0);

  if (!buf)
    {
      itc->itc_ltrx->lt_error = LTE_NO_DISK;
      *err_ret = srv_make_new_error ("40009", "SR106", "Out of disk on database");
      return;
    }

  if (IS_WIDE_STRING_DTP (dtp))
    {
      wide_l = (long) wide_char_length_of_utf8_string (row + pos + 1, len - 1);
      if (wide_l < 0)
	GPF_T1 ("Non valid UTF8 data into an inlined blob column");
      row[pos] = DV_BLOB_WIDE;
    }
  else
    {
      wide_l = len - 1;
      row[pos] = (cl->cl_sqt.sqt_dtp == DV_BLOB_BIN ? DV_BLOB : cl->cl_sqt.sqt_dtp);
    }
  memcpy (buf->bd_buffer + DP_DATA, row + pos + 1, len - 1);
  LONG_SET (buf->bd_buffer + DP_BLOB_LEN, len - 1);
  bh = bh_alloc (DV_BLOB_HANDLE_DTP_FOR_BLOB_DTP (cl->cl_sqt.sqt_dtp));
  bh->bh_page = buf->bd_page;
  bh->bh_diskbytes = len - 1;
  bh->bh_length = wide_l;
  bh->bh_pages = (dp_addr_t *) dk_alloc_box (sizeof (dp_addr_t), DV_BIN);
  bh->bh_pages[0] = buf->bd_page;
  bh->bh_page_dir_complete = 1;
  bh->bh_it = itc->itc_tree;
  bh_to_dv (bh, inlined, inlined[0]);
  dk_free_box ((caddr_t) bh);
  blob_schedule_delayed_delete (itc, bl_from_dv (inlined, itc), BL_DELETE_AT_ROLLBACK);
  memmove (row + pos + DV_BLOB_LEN, row + pos + len, *end_pos - len);
  ITC_IN_MAP (itc);
  page_leave_inner (buf);
  ITC_LEAVE_MAP (itc);
  saved = len - DV_BLOB_LEN;
  (*end_pos) -= saved;
  for (cl = cl; cl->cl_col_id; cl++)
    {
      if (CL_FIRST_VAR == cl->cl_fixed_len)
	SHORT_REF (row + key->key_length_area) -= (short) saved;
      else
	SHORT_REF (row + 2 - cl->cl_fixed_len) -= (short) saved;
    }
}


int
upd_outline_inline_blobs (it_cursor_t * itc, db_buf_t row, dbe_key_t * key, int * row_len_ret, caddr_t * err_ret)
{
  int n_blobs = 0;
  int nb_length = itc->itc_row_key->key_row_var_start;
  int b_length = 0;
  int savings = 0;
  int inx;
  dbe_col_loc_t * best_cl = NULL;
  int best_len = 0, best_pos = 0;
  int row_len = *row_len_ret;
  for (inx = 0; key->key_row_var[inx].cl_col_id; inx++)
    {
      int len, pos;
      dbe_col_loc_t * cl = &key->key_row_var[inx];
      if (IS_INLINEABLE_BLOB_DTP (cl->cl_sqt.sqt_dtp))
	{
	  n_blobs++;
	  if (cl->cl_null_mask & row[cl->cl_null_flag])
	    continue;
	  KEY_COL_WITHOUT_CHECK (key, row, (*cl), pos, len);
	  b_length += len;
#ifndef NDEBUG
	    {
	      if (len < 0)
		GPF_T;
	      if (pos + len > row_len)
		GPF_T;
	      if ((pos < row_len) && (
		  ((cl->cl_sqt.sqt_dtp == DV_BLOB || cl->cl_sqt.sqt_dtp == DV_BLOB_XPER) &&
		   DV_LONG_STRING != row[pos] && DV_BLOB != row[pos] && DV_BLOB_XPER != row[pos]) ||
		  (cl->cl_sqt.sqt_dtp == DV_BLOB_WIDE &&
		   DV_LONG_WIDE != row[pos] && DV_BLOB_WIDE != row[pos]) ||
		  (cl->cl_sqt.sqt_dtp == DV_BLOB_BIN &&
		   DV_LONG_STRING != row[pos] && DV_BLOB != row[pos] && DV_BLOB_XPER != row[pos])
		  /*GK DV_BLOB_BIN is kept the same as the DV_BLOB */
		  ))
	        GPF_T;
	    }
#endif
	  if (len > DV_BLOB_LEN &&
	      (DV_LONG_STRING == row[pos] ||
	       DV_LONG_WIDE == row[pos] ||
	       DV_LONG_BIN == row[pos]))
	    {
	      savings += len - DV_BLOB_LEN;
	      if (len >= best_len)
		{
		  best_len = len;
		  best_pos = pos;
		  best_cl = cl;
		}
	    }
	}
    }
  nb_length = row_len - b_length;
  if (nb_length + DV_BLOB_LEN * n_blobs > ROW_MAX_DATA)
    {
      *err_ret = srv_make_new_error ("42000", "SR248", "Row too long in update");
      return BLOB_NO_CHANGE;
    }
  if (!best_cl)
    return BLOB_NO_CHANGE;
  blob_outline (itc, best_cl, row, best_pos, best_len, row_len_ret, err_ret);
  return BLOB_CHANGED;
}


int
blob_inline (it_cursor_t * itc, dbe_col_loc_t * cl, db_buf_t row, int inline_pos, int *end_pos, caddr_t * err_ret /* unused */,
	     int log_as_insert)
{
  int added;
  long diskbytes = LONG_REF_NA (row + inline_pos + BL_BYTE_LEN);
  dtp_t dtp = row[inline_pos];
  dp_addr_t dp = LONG_REF_NA (row + inline_pos + BL_DP);
  buffer_desc_t *buf = NULL;
  blob_layout_t *old_outline_layout = bl_from_dv (row + inline_pos, itc);
  err_ret = NULL;

  do
    {
      ITC_IN_MAP (itc);
      page_wait_blob_access (itc, dp, &buf, PA_READ, NULL, 0);
    }
  while (!buf);

  memmove (row + inline_pos + diskbytes + 1,
      row + inline_pos + DV_BLOB_LEN,
      *end_pos - (inline_pos + DV_BLOB_LEN));
  memmove (row + inline_pos + 1, buf->bd_buffer + DP_DATA, diskbytes);

  page_leave_inner (buf);

  row[inline_pos] = DV_LONG_STRING_DTP_FOR_BLOB_DTP (dtp);
#ifndef NDEBUG
  /* GK: DB_BLOB_BIN inlines as DV_LONG_STRING because of compatibility */
  if (DV_LONG_BIN == row[inline_pos] || DV_BIN == row[inline_pos])
    GPF_T;
#endif
  added = diskbytes + 1 - DV_BLOB_LEN;
  for (cl = cl; cl->cl_col_id; cl++)
    {
      if (CL_FIRST_VAR == cl->cl_fixed_len)
	SHORT_REF (row + itc->itc_row_key->key_length_area) += added;
      else
	SHORT_REF (row + 2 - cl->cl_fixed_len) += added;
    }

  *end_pos += added;
  /* It is important that old_outline_layout is used in both branches of "if" below,
   * once per branch, thus no problems with dk_free_box for it */
  if (itc->itc_has_blob_logged)
/*      && BLOB_IN_INSERT != log_as_insert) : even we inlined BLOB on insert one may not freed
   because date mey be used in the same transaction
   (triggers and stored procedures)
 */
    {
      /* if the inlined blob is needed as a blob for logging, delete only at commit, otherwise right now */
      blob_schedule_delayed_delete (itc, old_outline_layout, BL_DELETE_AT_COMMIT);
    }
  else
    {
      blob_cancel_delayed_delete (itc, dp, (BL_DELETE_AT_COMMIT | BL_DELETE_AT_ROLLBACK));
      blob_chain_delete (itc, old_outline_layout);
    }
  if (BLOB_IN_INSERT == log_as_insert)
    /* for uncommitted insert, remove the log entry since the data will be logged with the row */
    blob_log_set_delete (&itc->itc_ltrx->lt_blob_log, dp);

  ITC_LEAVE_MAP (itc);
  return BLOB_CHANGED;
}


int
upd_inline_blobs (it_cursor_t * itc, db_buf_t row, dbe_key_t * key, int * row_len_ret, caddr_t * err_ret,
		  int log_as_insert)
{

  int n_blobs = 0;
  int inx;
  dbe_col_loc_t * best_cl = NULL;
  int best_len = 1000000, best_pos = 0;
  int row_len = *row_len_ret;
  for (inx = 0; key->key_row_var[inx].cl_col_id; inx++)
    {
      dbe_col_loc_t * cl = &key->key_row_var[inx];
      if (IS_INLINEABLE_BLOB_DTP (cl->cl_sqt.sqt_dtp))
	{
	  int len, pos;
	  n_blobs++;
	  if (cl->cl_null_mask & row[cl->cl_null_flag])
	    continue;
	  KEY_COL (key, row, (*cl), pos, len);
#ifndef NDEBUG
	  if (len < 0)
	    GPF_T;
	  if (pos + len > row_len)
	    GPF_T;
	  if ((pos < row_len) && (
	      ((cl->cl_sqt.sqt_dtp == DV_BLOB || cl->cl_sqt.sqt_dtp == DV_BLOB_XPER) &&
	       DV_LONG_STRING != row[pos] && DV_BLOB != row[pos] && DV_BLOB_XPER != row[pos]) ||
	      (cl->cl_sqt.sqt_dtp == DV_BLOB_WIDE &&
	       DV_LONG_WIDE != row[pos] && DV_BLOB_WIDE != row[pos]) ||
	      (cl->cl_sqt.sqt_dtp == DV_BLOB_BIN &&
	       DV_LONG_STRING != row[pos] && DV_BLOB != row[pos] && DV_BLOB_XPER != row[pos])
	      /*GK DV_BLOB_BIN is kept the same as the DV_BLOB */
	      ))
	    GPF_T;
#endif
	  if (IS_INLINEABLE_BLOB_DTP (row[pos]))
	    {
	      len = LONG_REF_NA (row + pos + BL_BYTE_LEN);
	      if (len < best_len)
		{
		  best_len = len;
		  best_pos = pos;
		  best_cl = cl;
		}
	    }
	}
    }
  if (row_len + best_len + 1 - DV_BLOB_LEN > ROW_MAX_DATA)
    return BLOB_NO_CHANGE;
  return (blob_inline (itc, best_cl, row, best_pos, row_len_ret, err_ret, log_as_insert));
}


int
key_n_blobs (dbe_key_t * key)
{
  int inx, n = 0;
  for (inx = 0; key->key_row_var[inx].cl_col_id; inx++)
    if (DV_BLOB == key->key_row_var[inx].cl_sqt.sqt_dtp)
      n++;
  return n;
}


int
upd_blob_opt (it_cursor_t * itc, db_buf_t row,
	      caddr_t * err_ret,
	      int log_as_insert)
{
  int changed = BLOB_NO_CHANGE;
  int rc;
  dbe_key_t * key = itc->itc_row_key;
  int len = row_length (row, key);
  if (!key->key_table->tb_any_blobs)
    return BLOB_NO_CHANGE;
  len -= IE_FIRST_KEY;
  row += IE_FIRST_KEY;
  for (;;)
    {
      rc = upd_outline_inline_blobs (itc, row, key, &len, err_ret);
      if (*err_ret)
	return 0;
      if (rc == BLOB_NO_CHANGE)
	break;
      changed = BLOB_CHANGED;
    }

  for (;;)
    {
      rc = upd_inline_blobs (itc, row, key, &len, err_ret, log_as_insert);
      if (BLOB_NO_CHANGE == rc)
	break;
      changed = BLOB_CHANGED;
    }
#ifndef NDEBUG
  if (row_length (row - IE_FIRST_KEY, key) > MAX_ROW_BYTES)
    {
      if (NULL == *err_ret)
        log_error ("Row layout error: row is too long at the end of upd_blob_opt() and no error returned");
#ifdef DEBUG
      else
        log_warning ("Row is too long so upd_blob_opt() is calling TRX_POISON");
#endif
    }
#endif
  if (*err_ret)
    TRX_POISON (itc->itc_ltrx);
  return changed;
}


static int
wide_blob_buffered_read (
	void* ses_from,
	unsigned char* to,
	int req_chars,
	blob_state_t * state,
	int * page_end,
	int *preadchars,
	get_data_generic_proc get_data_proc)
{
  unsigned char* buf_to = to;
  int readbytes = 0;
  int readchars = 0;
  unsigned char utf8_char = state->utf8_chr;
  while (req_chars)
    {
      if (!utf8_char)
	get_data_proc (ses_from, &utf8_char, 0, 1); /*   there was: utf8_char = session_buffered_read_char (ses_from);*/

      if (utf8_char < 0x80)
	{
	  /* One byte sequence.  */
	  state->count = 0;
	}
      else if ((utf8_char & 0xe0) == 0xc0)
	{
	  state->count = 1;
	}
      else if ((utf8_char & 0xf0) == 0xe0)
	{
	  /* We expect three bytes.  */
	  state->count = 2;
	}
      else if ((utf8_char & 0xf8) == 0xf0)
	{
	  /* We expect four bytes.  */
	  state->count = 3;
	}
      else if ((utf8_char & 0xfc) == 0xf8)
	{
	  /* We expect five bytes.  */
	  state->count = 4;
	}
      else if ((utf8_char & 0xfe) == 0xfc)
	{
	  /* We expect six bytes.  */
	  state->count = 5;
	}
      else
	{
	  /* This is an illegal encoding.  */
	  /* errno = (EILSEQ); */
	  /*GPF_T1 ("Received bad UTF8 string");*/
	  /*return -1;*/
          state->count = 0;
          log_error ("Invalid UTF-8 char (%02X) read in filling up a BLOB. The wide blob data may be garbaled.",
              utf8_char);
          utf8_char = '?';
	}

      if (state->count > req_chars - 1)
	{ /* not enough space */
	  state->utf8_chr = utf8_char;
	  page_end[0] = 1;
	  *preadchars = readchars;
	  return readbytes;
	}

      *buf_to++ = utf8_char;
      if (state->count)
	get_data_proc (ses_from, buf_to, 0, state->count);  /*there was: session_buffered_read (ses_from, buf_to, state->count);*/
      buf_to += state->count;
      readbytes += state->count + 1;
      readchars ++;
      req_chars -= state->count + 1;
      if (req_chars < 0)
	GPF_T;
      utf8_char = 0;
    }
  state->utf8_chr = 0;
  state->count = 0;
  *preadchars = readchars;
  return readbytes;
}

#if 0
static long
blob_fill_buffer_from_wide_string (caddr_t _bh, caddr_t _buf, int *at_end, long *char_len)
{
  blob_handle_t *bh = (blob_handle_t *) _bh;
  buffer_desc_t *buf = (buffer_desc_t *) _buf;
  wchar_t *wstring = (wchar_t *) bh->bh_string;
  wchar_t *wstr = wstring;
  long wchars = (box_length (bh->bh_string) / sizeof (wchar_t)) -
	bh->bh_position - 1;
  long utf8len;
  virt_mbstate_t state;

  wstr += bh->bh_position;
  memset (&state, 0, sizeof (virt_mbstate_t));
  utf8len = virt_wcsnrtombs (buf->bd_buffer + DP_DATA, &wstr, wchars, PAGE_DATA_SZ, &state);
  *char_len = wstr ? (wstr - wstring - bh->bh_position) : wchars - 1;
  bh->bh_position = wstr ? (wstr - ((wchar_t *)bh->bh_string)) : bh->bh_position + wchars - 1;
  if (utf8len <= 0 || *char_len >= wchars)
    {
      *at_end = 1;
      bh->bh_position = 0;
    }
  return (utf8len == -1 ? 0 : utf8len);
}
#endif
