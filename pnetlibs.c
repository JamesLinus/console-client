/* Copyright (c) 2013-2014 Anton Titov.
 * Copyright (c) 2013-2014 pCloud Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of pCloud Ltd nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL pCloud Ltd BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define __STDC_FORMAT_MACROS
#include <stdio.h>
#include <ctype.h>
#include "pnetlibs.h"
#include "pssl.h"
#include "psettings.h"
#include "plibs.h"
#include "ptimer.h"
#include "pstatus.h"
#include "papi.h"
#include "ppool.h"

struct time_bytes {
  time_t tm;
  psync_uint_t bytes;
};

typedef struct {
  unsigned char sha1[PSYNC_SHA1_DIGEST_LEN];
  uint32_t adler;
} psync_block_checksum;

typedef struct {
  uint64_t filesize;
  uint32_t blocksize;
  uint32_t blockcnt;
  uint32_t *next;
  psync_block_checksum blocks[];
} psync_file_checksums;

typedef struct {
  psync_uint_t elementcnt;
  uint32_t elements[];
} psync_file_checksum_hash;

typedef struct {
  uint64_t filesize;
  uint32_t blocksize;
  unsigned char _reserved[12];
} psync_block_checksum_header;

typedef struct {
  uint64_t off;
  uint32_t idx;
  uint32_t type;
} psync_block_action;


static time_t current_download_sec=0;
static psync_uint_t download_bytes_this_sec=0;
static psync_uint_t download_bytes_off=0;
static psync_uint_t download_speed=0;

static time_t current_upload_sec=0;
static psync_uint_t upload_bytes_this_sec=0;
static psync_uint_t upload_bytes_off=0;
static psync_uint_t upload_speed=0;
static psync_uint_t dyn_upload_speed=PSYNC_UPL_AUTO_SHAPER_INITIAL;

static psync_pool *apipool=NULL;

static struct time_bytes download_bytes_sec[PSYNC_SPEED_CALC_AVERAGE_SEC], upload_bytes_sec[PSYNC_SPEED_CALC_AVERAGE_SEC];

static void *psync_get_api(){
  return psync_api_connect(psync_setting_get_bool(_PS(usessl)));
}

static void psync_ret_api(void *ptr){
  psync_socket_close((psync_socket *)ptr);
}

void psync_netlibs_init(){
  apipool=psync_pool_create(psync_get_api, psync_ret_api, PSYNC_APIPOOL_MAXIDLE, PSYNC_APIPOOL_MAXACTIVE, PSYNC_APIPOOL_MAXIDLESEC);
}

psync_socket *psync_apipool_get(){
  psync_socket *ret;
  ret=(psync_socket *)psync_pool_get(apipool);
  if (likely(ret)){
    while (unlikely(psync_socket_isssl(ret)!=psync_setting_get_bool(_PS(usessl)))){
      psync_pool_release_bad(apipool, ret);
      ret=(psync_socket *)psync_pool_get(apipool);
      if (!ret)
        break;
    }
  }
  else
    psync_timer_notify_exception();
  return ret;
}

void psync_apipool_release(psync_socket *api){
  psync_pool_release(apipool, api);
}

void psync_apipool_release_bad(psync_socket *api){
  psync_pool_release_bad(apipool, api);
}


static void rm_all(void *vpath, psync_pstat *st){
  char *path;
  path=psync_strcat((char *)vpath, PSYNC_DIRECTORY_SEPARATOR, st->name, NULL);
  if (psync_stat_isfolder(&st->stat)){
    psync_list_dir(path, rm_all, path);
    psync_rmdir(path);
  }
  else
    psync_file_delete(path);
  psync_free(path);
}

static void rm_ign(void *vpath, psync_pstat *st){
  char *path;
  if (!psync_is_name_to_ignore(st->name))
    return;
  path=psync_strcat((char *)vpath, PSYNC_DIRECTORY_SEPARATOR, st->name, NULL);
  if (psync_stat_isfolder(&st->stat)){
    psync_list_dir(path, rm_all, path);
    psync_rmdir(path);
  }
  else
    psync_file_delete(path);
  psync_free(path);
}

int psync_rmdir_with_trashes(const char *path){
  if (!psync_rmdir(path))
    return 0;
  if (psync_fs_err()!=P_NOTEMPTY && psync_fs_err()!=P_EXIST)
    return -1;
  if (psync_list_dir(path, rm_ign, (void *)path))
    return -1;
  return psync_rmdir(path);
}

int psync_rmdir_recursive(const char *path){
  if (psync_list_dir(path, rm_all, (void *)path))
    return -1;
  return psync_rmdir(path);
}

void psync_set_local_full(int over){
  static int isover=0;
  if (over!=isover){
    isover=over;
    if (isover)
      psync_set_status(PSTATUS_TYPE_DISKFULL, PSTATUS_DISKFULL_FULL);
    else
      psync_set_status(PSTATUS_TYPE_DISKFULL, PSTATUS_DISKFULL_OK);
  }
}

int psync_handle_api_result(uint64_t result){
  if (result==2000){
    psync_set_status(PSTATUS_TYPE_AUTH, PSTATUS_AUTH_BADLOGIN);
    psync_timer_notify_exception();
    return PSYNC_NET_TEMPFAIL;
  }
  else if (result==2003 || result==2009 || result==2005)
    return PSYNC_NET_PERMFAIL;
  else if (result==2007){
    debug(D_ERROR, "trying to delete root folder");
    return PSYNC_NET_PERMFAIL;
  }
  else
    return PSYNC_NET_TEMPFAIL;
}

int psync_get_remote_file_checksum(psync_fileid_t fileid, unsigned char *hexsum, uint64_t *fsize){
  psync_socket *api;
  binresult *res;
  const binresult *meta, *checksum;
  psync_sql_res *sres;
  psync_variant_row row;
  uint64_t result;
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("fileid", fileid)};
  sres=psync_sql_query("SELECT h.checksum, f.size FROM hashchecksum h, file f WHERE f.id=? AND f.hash=h.hash AND f.size=h.size");
  psync_sql_bind_uint(sres, 1, fileid);
  row=psync_sql_fetch_row(sres);
  if (row){
    assertw(row[0].length==PSYNC_HASH_DIGEST_HEXLEN);
    memcpy(hexsum, psync_get_string(row[0]), PSYNC_HASH_DIGEST_HEXLEN);
    if (fsize)
      *fsize=psync_get_number(row[1]);
    psync_sql_free_result(sres);
    return PSYNC_NET_OK;
  }
  psync_sql_free_result(sres);
  api=psync_apipool_get();
  if (unlikely(!api))
    return PSYNC_NET_TEMPFAIL;
  res=send_command(api, "checksumfile", params);
  if (res)
    psync_apipool_release(api);
  else
    psync_apipool_release_bad(api);
  if (unlikely_log(!res)){
    psync_timer_notify_exception();
    return PSYNC_NET_TEMPFAIL;
  }
  result=psync_find_result(res, "result", PARAM_NUM)->num;
  if (result){
    debug(D_ERROR, "checksumfile returned error %lu", (unsigned long)result);
    psync_free(res);
    return psync_handle_api_result(result);
  }
  meta=psync_find_result(res, "metadata", PARAM_HASH);
  checksum=psync_find_result(res, PSYNC_CHECKSUM, PARAM_STR);
  result=psync_find_result(meta, "size", PARAM_NUM)->num;
  if (fsize)
    *fsize=result;
  sres=psync_sql_prep_statement("REPLACE INTO hashchecksum (hash, size, checksum) VALUES (?, ?, ?)");
  psync_sql_bind_uint(sres, 1, psync_find_result(meta, "hash", PARAM_NUM)->num);
  psync_sql_bind_uint(sres, 2, result);
  psync_sql_bind_lstring(sres, 3, checksum->str, checksum->length);
  psync_sql_run_free(sres);
  memcpy(hexsum, checksum->str, checksum->length);
  psync_free(res);
  return PSYNC_NET_OK;
}

int psync_get_local_file_checksum(const char *restrict filename, unsigned char *restrict hexsum, uint64_t *restrict fsize){
  psync_stat_t st;
  psync_hash_ctx hctx;
  uint64_t rsz;
  void *buff;
  size_t rs;
  ssize_t rrs;
  psync_file_t fd;
  unsigned char hashbin[PSYNC_HASH_DIGEST_LEN];
  fd=psync_file_open(filename, P_O_RDONLY, 0);
  if (fd==INVALID_HANDLE_VALUE)
    return PSYNC_NET_PERMFAIL;
  if (unlikely_log(psync_fstat(fd, &st)))
    goto err1;
  buff=psync_malloc(PSYNC_COPY_BUFFER_SIZE);
  psync_hash_init(&hctx);
  rsz=psync_stat_size(&st);
  while (rsz){
    if (rsz>PSYNC_COPY_BUFFER_SIZE)
      rs=PSYNC_COPY_BUFFER_SIZE;
    else
      rs=rsz;
    rrs=psync_file_read(fd, buff, rs);
    if (rrs<=0)
      goto err2;
    psync_yield_cpu();
    psync_hash_update(&hctx, buff, rrs);
    rsz-=rrs;
  }
  psync_free(buff);
  psync_file_close(fd);
  psync_hash_final(hashbin, &hctx);
  psync_binhex(hexsum, hashbin, PSYNC_HASH_DIGEST_LEN);
  if (fsize)
    *fsize=psync_stat_size(&st);
  return PSYNC_NET_OK;
err2:
  psync_free(buff);
err1:
  psync_file_close(fd);
  return PSYNC_NET_PERMFAIL;
}

int psync_file_writeall_checkoverquota(psync_file_t fd, const void *buf, size_t count){
  ssize_t wr;
  while (count){
    wr=psync_file_write(fd, buf, count);
    if (wr==count){
      psync_set_local_full(0);
      return 0;
    }
    else if (wr==-1){
      if (psync_fs_err()==P_NOSPC || psync_fs_err()==P_DQUOT){
        psync_set_local_full(1);
        psync_milisleep(PSYNC_SLEEP_ON_DISK_FULL);
      }
      return -1;
    }
    buf = (unsigned char*)buf+wr;
    count-=wr;
  }
  return 0;
}

int psync_copy_local_file_if_checksum_matches(const char *source, const char *destination, const unsigned char *hexsum, uint64_t fsize){
  psync_file_t sfd, dfd;
  psync_hash_ctx hctx;
  void *buff;
  char *tmpdest;
  size_t rrd;
  ssize_t rd;
  unsigned char hashbin[PSYNC_HASH_DIGEST_LEN];
  char hashhex[PSYNC_HASH_DIGEST_HEXLEN];
  sfd=psync_file_open(source, P_O_RDONLY, 0);
  if (unlikely_log(sfd==INVALID_HANDLE_VALUE))
    goto err0;
  tmpdest=psync_strcat(destination, PSYNC_APPEND_PARTIAL_FILES, NULL);
  if (unlikely_log(psync_file_size(sfd)!=fsize))
    goto err1;
  dfd=psync_file_open(tmpdest, P_O_WRONLY, P_O_CREAT|P_O_TRUNC);
  if (unlikely_log(dfd==INVALID_HANDLE_VALUE))
    goto err1;
  psync_hash_init(&hctx);
  buff=psync_malloc(PSYNC_COPY_BUFFER_SIZE);
  while (fsize){
    if (fsize>PSYNC_COPY_BUFFER_SIZE)
      rrd=PSYNC_COPY_BUFFER_SIZE;
    else
      rrd=fsize;
    rd=psync_file_read(sfd, buff, rrd);
    if (unlikely_log(rd<=0))
      goto err2;
    if (unlikely_log(psync_file_writeall_checkoverquota(dfd, buff, rd)))
      goto err2;
    psync_yield_cpu();
    psync_hash_update(&hctx, buff, rd);
    fsize-=rd;
  }
  psync_hash_final(hashbin, &hctx);
  psync_binhex(hashhex, hashbin, PSYNC_HASH_DIGEST_LEN);
  if (unlikely_log(memcmp(hexsum, hashhex, PSYNC_HASH_DIGEST_HEXLEN)) || unlikely_log(psync_file_sync(dfd)))
    goto err2;
  psync_free(buff);
  if (unlikely_log(psync_file_close(dfd)) || unlikely_log(psync_file_rename_overwrite(tmpdest, destination)))
    goto err1;
  psync_free(tmpdest);
  psync_file_close(sfd);
  return PSYNC_NET_OK;
err2:
  psync_free(buff);
  psync_file_close(dfd);
  psync_file_delete(tmpdest);
err1:
  psync_free(tmpdest);
  psync_file_close(sfd);
err0:
  return PSYNC_NET_PERMFAIL;
}

psync_socket *psync_socket_connect_download(const char *host, int unsigned port, int usessl){
  psync_socket *sock;
  int64_t dwlspeed;
  sock=psync_socket_connect(host, port, usessl);
  if (sock){
    dwlspeed=psync_setting_get_int(_PS(maxdownloadspeed));
    if (dwlspeed!=-1 && dwlspeed<PSYNC_MAX_SPEED_RECV_BUFFER){
      if (dwlspeed==0)
        dwlspeed=PSYNC_RECV_BUFFER_SHAPED;
      psync_socket_set_recvbuf(sock, (uint32_t)dwlspeed);
    }
  }
  return sock;
}

psync_socket *psync_api_connect_download(){
  psync_socket *sock;
  int64_t dwlspeed;
  sock=psync_api_connect(psync_setting_get_bool(_PS(usessl)));
  if (sock){
    dwlspeed=psync_setting_get_int(_PS(maxdownloadspeed));
    if (dwlspeed!=-1 && dwlspeed<PSYNC_MAX_SPEED_RECV_BUFFER){
      if (dwlspeed==0)
        dwlspeed=PSYNC_RECV_BUFFER_SHAPED;
      psync_socket_set_recvbuf(sock, (uint32_t)dwlspeed);
    }
  }
  return sock;
}

void psync_socket_close_download(psync_socket *sock){
  psync_socket_close(sock);
}

/* generally this should be protected by mutex as downloading is multi threaded, but it is not so important to
 * have that accurate download speed
 */

static void account_downloaded_bytes(int unsigned bytes){
  if (current_download_sec==psync_current_time)
    download_bytes_this_sec+=bytes;
  else{
    uint64_t sum;
    psync_uint_t i;
    download_bytes_sec[download_bytes_off].tm=current_download_sec;
    download_bytes_sec[download_bytes_off].bytes=download_bytes_this_sec;
    download_bytes_off=(download_bytes_off+1)%PSYNC_SPEED_CALC_AVERAGE_SEC;
    current_download_sec=psync_current_time;
    download_bytes_this_sec=bytes;
    sum=0;
    for (i=0; i<PSYNC_SPEED_CALC_AVERAGE_SEC; i++)
      if (download_bytes_sec[i].tm>=psync_current_time-PSYNC_SPEED_CALC_AVERAGE_SEC)
        sum+=download_bytes_sec[i].bytes;
    download_speed=sum/PSYNC_SPEED_CALC_AVERAGE_SEC;
    psync_status_set_download_speed(download_speed);
  }
}

static psync_uint_t get_download_bytes_this_sec(){
  if (current_download_sec==psync_current_time)
    return download_bytes_this_sec;
  else
    return 0;
}

int psync_socket_readall_download(psync_socket *sock, void *buff, int num){
  psync_int_t dwlspeed, readbytes, pending, lpending, rd, rrd;
  psync_uint_t thissec, ds;
  dwlspeed=psync_setting_get_int(_PS(maxdownloadspeed));
  if (dwlspeed==0){
    lpending=psync_socket_pendingdata_buf(sock);
    if (download_speed>100*1024)
      ds=download_speed/1024;
    else
      ds=100;
    while (1){
      psync_milisleep(PSYNC_SLEEP_AUTO_SHAPER*100/ds);
      pending=psync_socket_pendingdata_buf(sock);
      if (pending==lpending)
        break;
      else
        lpending=pending;
    }
    if (pending>0)
      sock->pending=1;
  }
  else if (dwlspeed>0){
    readbytes=0;
    while (num){
      while ((thissec=get_download_bytes_this_sec())>=dwlspeed)
        psync_timer_wait_next_sec();
      if (num>dwlspeed-thissec)
        rrd=dwlspeed-thissec;
      else
        rrd=num;
      rd=psync_socket_read(sock, buff, rrd);
      if (rd<=0)
        return readbytes?readbytes:rd;
      num-=rd;
      buff=(char *)buff+rd;
      readbytes+=rd;
      account_downloaded_bytes(rd);
    }
    return readbytes;
  }
  readbytes=psync_socket_readall(sock, buff, num);
  if (readbytes>0)
    account_downloaded_bytes(readbytes);
  return readbytes;
}

static void account_uploaded_bytes(int unsigned bytes){
  if (current_upload_sec==psync_current_time)
    upload_bytes_this_sec+=bytes;
  else{
    uint64_t sum;
    psync_uint_t i;
    upload_bytes_sec[upload_bytes_off].tm=current_upload_sec;
    upload_bytes_sec[upload_bytes_off].bytes=upload_bytes_this_sec;
    upload_bytes_off=(upload_bytes_off+1)%PSYNC_SPEED_CALC_AVERAGE_SEC;
    current_upload_sec=psync_current_time;
    upload_bytes_this_sec=bytes;
    sum=0;
    for (i=0; i<PSYNC_SPEED_CALC_AVERAGE_SEC; i++)
      if (upload_bytes_sec[i].tm>=psync_current_time-PSYNC_SPEED_CALC_AVERAGE_SEC)
        sum+=upload_bytes_sec[i].bytes;
    upload_speed=sum/PSYNC_SPEED_CALC_AVERAGE_SEC;
    psync_status_set_upload_speed(upload_speed);
  }
}

static psync_uint_t get_upload_bytes_this_sec(){
  if (current_upload_sec==psync_current_time)
    return upload_bytes_this_sec;
  else
    return 0;
}

static void set_send_buf(psync_socket *sock){
  psync_socket_set_sendbuf(sock, dyn_upload_speed*PSYNC_UPL_AUTO_SHAPER_BUF_PER/100);
}

int psync_set_default_sendbuf(psync_socket *sock){
  return psync_socket_set_sendbuf(sock, PSYNC_DEFAULT_SEND_BUFF);
}

int psync_socket_writeall_upload(psync_socket *sock, const void *buff, int num){
  psync_int_t uplspeed, writebytes, wr, wwr;
  psync_uint_t thissec;
    uplspeed=psync_setting_get_int(_PS(maxuploadspeed));
  if (uplspeed==0){
    writebytes=0;
    while (num){
      while ((thissec=get_upload_bytes_this_sec())>=dyn_upload_speed){
        dyn_upload_speed=(dyn_upload_speed*PSYNC_UPL_AUTO_SHAPER_INC_PER)/100;
        set_send_buf(sock);
        psync_timer_wait_next_sec();
      }
      debug(D_NOTICE, "dyn_upload_speed=%lu", dyn_upload_speed);
      if (num>dyn_upload_speed-thissec)
        wwr=dyn_upload_speed-thissec;
      else
        wwr=num;
      if (!psync_socket_writable(sock)){
        dyn_upload_speed=(dyn_upload_speed*PSYNC_UPL_AUTO_SHAPER_DEC_PER)/100;
        if (dyn_upload_speed<PSYNC_UPL_AUTO_SHAPER_MIN)
          dyn_upload_speed=PSYNC_UPL_AUTO_SHAPER_MIN;
        set_send_buf(sock);
        psync_milisleep(1000);
      }
      wr=psync_socket_write(sock, buff, wwr);
      if (wr==-1)
        return writebytes?writebytes:wr;
      num-=wr;
      buff=(char *)buff+wr;
      writebytes+=wr;
      account_uploaded_bytes(wr);
    }
    return writebytes;
  }
  else if (uplspeed>0){
    writebytes=0;
    while (num){
      while ((thissec=get_upload_bytes_this_sec())>=uplspeed)
        psync_timer_wait_next_sec();
      if (num>uplspeed-thissec)
        wwr=uplspeed-thissec;
      else
        wwr=num;
      wr=psync_socket_write(sock, buff, wwr);
      if (wr==-1)
        return writebytes?writebytes:wr;
      num-=wr;
      buff=(char *)buff+wr;
      writebytes+=wr;
      account_uploaded_bytes(wr);
    }
    return writebytes;
  }
  writebytes=psync_socket_writeall(sock, buff, num);
  if (writebytes>0)
    account_uploaded_bytes(writebytes);
  return writebytes;
}

psync_http_socket *psync_http_connect(const char *host, const char *path, uint64_t from, uint64_t to){
  psync_socket *sock;
  psync_http_socket *hsock;
  char *readbuff, *ptr;
  int usessl, rl, rb;
  usessl=psync_setting_get_bool(_PS(usessl));
  sock=psync_socket_connect_download(host, usessl?443:80, usessl);
  if (!sock)
    goto err0;
  readbuff=psync_malloc(PSYNC_HTTP_RESP_BUFFER);
  if (from || to){
    if (to)
      rl=snprintf(readbuff, PSYNC_HTTP_RESP_BUFFER, "GET %s HTTP/1.0\015\012Host: %s\015\012Range: bytes=%"P_PRI_U64"-%"P_PRI_U64"\015\012Connection: close\015\012\015\012",
                  path, host, from, to);
    else
      rl=snprintf(readbuff, PSYNC_HTTP_RESP_BUFFER, "GET %s HTTP/1.0\015\012Host: %s\015\012Range: bytes=%"P_PRI_U64"-\015\012Connection: close\015\012\015\012",
                  path, host, from);
  }
  else
    rl=snprintf(readbuff, PSYNC_HTTP_RESP_BUFFER, "GET %s HTTP/1.0\015\012Host: %s\015\012Connection: close\015\012\015\012", path, host);
  if (psync_socket_writeall(sock, readbuff, rl)!=rl || (rb=psync_socket_readall_download(sock, readbuff, PSYNC_HTTP_RESP_BUFFER-1))==-1)
    goto err1;
  readbuff[rb]=0;
  ptr=readbuff;
  while (*ptr && !isspace(*ptr))
    ptr++;
  while (*ptr && isspace(*ptr))
    ptr++;
  if (!isdigit(*ptr) || atoi(ptr)/10!=20)
    goto err1;
  if ((ptr=strstr(readbuff, "\015\012\015\012")))
    ptr+=4;
  else if ((ptr=strstr(readbuff, "\012\012")))
    ptr+=2;
  else
    goto err1;
  rl=ptr-readbuff;
  if (rl==rb){
    psync_free(readbuff);
    readbuff=NULL;
  }
  hsock=psync_new(psync_http_socket);
  hsock->sock=sock;
  hsock->readbuff=readbuff;
  hsock->readbuffoff=rl;
  hsock->readbuffsize=rb;
  return hsock;
err1:
  psync_free(readbuff);
  psync_socket_close_download(sock);
err0:
  return NULL;
}

void psync_http_close(psync_http_socket *http){
  psync_socket_close_download(http->sock);
  if (http->readbuff)
    psync_free(http->readbuff);
  psync_free(http);
}

int psync_http_readall(psync_http_socket *http, void *buff, int num){
  if (http->readbuff){
    int cp;
    if (num<http->readbuffsize-http->readbuffoff)
      cp=num;
    else
      cp=http->readbuffsize-http->readbuffoff;
    memcpy(buff, (unsigned char*)http->readbuff+http->readbuffoff, cp);
    http->readbuffoff+=cp;
    if (http->readbuffoff>=http->readbuffsize){
      psync_free(http->readbuff);
      http->readbuff=NULL;
    }
    if (cp==num)
      return cp;
    num=psync_socket_readall_download(http->sock, (unsigned char*)buff+cp, num-cp);
    if (num<=0)
      return cp;
    else
      return cp+num;
  }
  else
    return psync_socket_readall_download(http->sock, buff, num);
}

static int psync_net_get_checksums(psync_fileid_t fileid, psync_file_checksums **checksums){
  binparam params[]={P_STR("auth", psync_my_auth), P_NUM("fileid", fileid)};
  psync_socket *api;
  binresult *res;
  const binresult *hosts;
  const char *requestpath;
  psync_http_socket *http;
  psync_file_checksums *cs;
  psync_block_checksum_header hdr;
  uint64_t result;
  uint32_t i;
  *checksums=NULL; /* gcc is not smart enough to notice that initialization is not needed */
  api=psync_apipool_get();
  if (unlikely(!api))
    return PSYNC_NET_TEMPFAIL;
  res=send_command(api, "getchecksumlink", params);
  if (res)
    psync_apipool_release(api);
  else
    psync_apipool_release_bad(api);
  if (unlikely_log(!res)){
    psync_timer_notify_exception();
    return PSYNC_NET_TEMPFAIL;
  }
  result=psync_find_result(res, "result", PARAM_NUM)->num;
  if (result){
    debug(D_ERROR, "getchecksumlink returned error %lu", (unsigned long)result);
    psync_free(res);
    return psync_handle_api_result(result);
  }
  hosts=psync_find_result(res, "hosts", PARAM_ARRAY);
  requestpath=psync_find_result(res, "path", PARAM_STR)->str;
  http=NULL;
  for (i=0; i<hosts->length; i++)
    if ((http=psync_http_connect(hosts->array[i]->str, requestpath, 0, 0)))
      break;
  psync_free(res);
  if (unlikely_log(!http))
    return PSYNC_NET_TEMPFAIL;
  if (unlikely_log(psync_http_readall(http, &hdr, sizeof(hdr))!=sizeof(hdr)))
    goto err0;
  i=(hdr.filesize+hdr.blocksize-1)/hdr.blocksize;
  cs=(psync_file_checksums *)psync_malloc(offsetof(psync_file_checksums, blocks)+(sizeof(psync_block_checksum)+sizeof(uint32_t))*i);
  cs->filesize=hdr.filesize;
  cs->blocksize=hdr.blocksize;
  cs->blockcnt=i;
  cs->next=(uint32_t *)((char *)(cs+offsetof(psync_file_checksums, blocks)+sizeof(psync_block_checksum)*i));
  if (unlikely_log(psync_http_readall(http, cs->blocks, sizeof(psync_block_checksum)*i)!=sizeof(psync_block_checksum)*i))
    goto err1;
  psync_http_close(http);
  memset(cs->next, 0, sizeof(uint32_t)*i);
  *checksums=cs;
  return PSYNC_NET_OK;
err1:
  psync_free(cs);
err0:
  psync_http_close(http);
  return PSYNC_NET_TEMPFAIL;
}

/*
static int psync_sha1_cmp(const void *p1, const void *p2){
  psync_block_checksum **b1=(psync_block_checksum **)p1;
  psync_block_checksum **b2=(psync_block_checksum **)p2;
  return memcmp((*b1)->sha1, (*b2)->sha1, PSYNC_SHA1_DIGEST_LEN);
}

static psync_block_checksum **psync_net_get_sorted_checksums(psync_file_checksums *checksums){
  psync_block_checksum **ret;
  uint32_t i;
  ret=psync_new_cnt(psync_block_checksum *, checksums->blockcnt);
  for (i=0; i<checksums->blockcnt; i++)
    ret[i]=&checksums->blocks[i];
  qsort(ret, checksums->blockcnt, sizeof(psync_block_checksum *), psync_sha1_cmp);
  return ret;
}*/

static int psync_is_prime(psync_uint_t num){
  psync_uint_t i;
  for (i=5; i*i<=num; i+=2)
    if (num%i==0)
      return 0;
  return 1;
}

#define MAX_ADLER_COLL 64

/* Since it is fairly easy to generate adler32 collisions, a file can be crafted to contain many colliding blocks.
 * Our hash will drop entries if more than MAX_ADLER_COLL collisions are detected (actually we just don't travel more
 * than MAX_ADLER_COLL from our "perfect" position in the hash).
 */

static psync_file_checksum_hash *psync_net_create_hash(const psync_file_checksums *checksums){
  psync_file_checksum_hash *h;
  psync_uint_t cnt, col;
  uint32_t i, o;
  cnt=((checksums->blockcnt+1)/2)*6+1;
  while (1){
    if (psync_is_prime(cnt))
      break;
    cnt+=4;
    if (psync_is_prime(cnt))
      break;
    cnt+=2;
  }
  h=(psync_file_checksum_hash *)psync_malloc(offsetof(psync_file_checksum_hash, elements)+sizeof(uint32_t)*cnt);
  h->elementcnt=cnt;
  memset(h->elements, 0, sizeof(uint32_t)*cnt);
  for (i=0; i<checksums->blockcnt; i++){
    o=checksums->blocks[i].adler%cnt;
    if (h->elements[o]){
      col=0;
      do {
        if (!memcmp(checksums->blocks[i].sha1, checksums->blocks[h->elements[o]-1].sha1, PSYNC_SHA1_DIGEST_LEN)){
          checksums->next[i]=h->elements[o];
          break;
        }
        if (++o>=cnt)
          o=0;
        if (++col>MAX_ADLER_COLL)
          break;
      } while (h->elements[o]);
      if (col>MAX_ADLER_COLL)
        continue;
    }
    h->elements[o]=i+1;
  }
  return h;
}

static void psync_net_hash_remove(psync_file_checksum_hash *restrict hash, psync_file_checksums *restrict checksums,
                                  uint32_t adler, const unsigned char *sha1){
  uint32_t idx, o, bp;
  o=adler%hash->elementcnt;
  while (1){
    idx=hash->elements[o];
    if (unlikely_log(!idx))
      return;
    else if (checksums->blocks[idx-1].adler==adler && !memcmp(checksums->blocks[idx-1].sha1, sha1, PSYNC_SHA1_DIGEST_LEN))
      break;
    else if (++o>=hash->elementcnt)
      o=0;
  }
  hash->elements[o]=0;
  while (1){
    if (++o>=hash->elementcnt)
      o=0;
    idx=hash->elements[o];
    if (!idx)
      return;
    bp=checksums->blocks[idx].adler%hash->elementcnt;
    if (bp!=o && hash->elements[bp]==0){
      hash->elements[bp]=idx;
      hash->elements[o]=0;
    }
  }  
}

static void psync_net_block_match_found(psync_file_checksum_hash *restrict hash, psync_file_checksums *restrict checksums, 
                                        psync_block_action *restrict blockactions, uint32_t idx, uint32_t fileidx, uint64_t fileoffset){
  uint32_t cidx;
  idx--;
  if (blockactions[idx].type!=PSYNC_RANGE_TRANSFER)
    return;
  cidx=idx;
  while (1) {
    blockactions[cidx].type=PSYNC_RANGE_COPY;
    blockactions[cidx].idx=fileidx;
    blockactions[cidx].off=fileoffset;
    cidx=checksums->next[cidx];
    if (cidx)
      cidx--;
    else
      break;
  }
  psync_net_hash_remove(hash, checksums, checksums->blocks[idx].adler, checksums->blocks[idx].sha1);
}

static int psync_net_hash_has_adler(const psync_file_checksum_hash *hash, const psync_file_checksums *checksums, uint32_t adler){
  uint32_t idx, o;
  o=adler%hash->elementcnt;
  while (1){
    idx=hash->elements[o];
    if (!idx)
      return 0;
    else if (checksums->blocks[idx-1].adler==adler)
      return 1;
    else if (++o>=hash->elementcnt)
      o=0;
  }
}

static uint32_t psync_net_hash_has_adler_and_sha1(const psync_file_checksum_hash *hash, const psync_file_checksums *checksums, uint32_t adler,
                                                  const unsigned char *sha1){
  uint32_t idx, o;
  o=adler%hash->elementcnt;
  while (1){
    idx=hash->elements[o];
    if (!idx)
      return 0;
    else if (checksums->blocks[idx-1].adler==adler && !memcmp(checksums->blocks[idx-1].sha1, sha1, PSYNC_SHA1_DIGEST_LEN))
      return idx;
    else if (++o>=hash->elementcnt)
      o=0;
  }
}

#define ADLER32_1(o) adler+=buff[o]; sum+=adler
#define ADLER32_2(o) ADLER32_1(o); ADLER32_1(o+1)
#define ADLER32_4(o) ADLER32_2(o); ADLER32_2(o+2)
#define ADLER32_8(o) ADLER32_4(o); ADLER32_4(o+4)
#define ADLER32_16() do{ ADLER32_8(0); ADLER32_8(8); } while (0)

#define ADLER32_BASE    65521U
#define ADLER32_NMAX    5552U
#define ADLER32_INITIAL 1U

static uint32_t adler32(uint32_t adler, const unsigned char *buff, size_t len){
  uint32_t sum, i;
  sum=adler>>16;
  adler&=0xffff;
  while (len>=ADLER32_NMAX) {
    len-=ADLER32_NMAX;
    for (i=0; i<ADLER32_NMAX/16; i++){
      ADLER32_16();
      buff+=16;
    }
    adler%=ADLER32_BASE;
    sum%=ADLER32_BASE;
  }
  while (len>=16){
    len-=16;
    ADLER32_16();
    buff+=16;
  }
  while (len--){
    adler+=*buff++;
    sum+=adler;
  }
  adler%=ADLER32_BASE;
  sum%=ADLER32_BASE;
  return adler|(sum<<16);
}

static uint32_t adler32_roll(uint32_t adler, unsigned char byteout, unsigned char bytein, uint32_t len){
  uint32_t sum;
  sum=adler>>16;
  adler&=0xffff;
  adler+=ADLER32_BASE+bytein-byteout;
  sum=(ADLER32_BASE*ADLER32_BASE+sum-len*byteout-ADLER32_INITIAL+adler)%ADLER32_BASE;
  /* dividing after sum calculation gives processor a chance to run the divisions in parallel */
  adler%=ADLER32_BASE;
  return adler|(sum<<16);
}

static void psync_net_check_file_for_blocks(const char *name, psync_file_checksums *restrict checksums, 
                                            psync_file_checksum_hash *restrict hash, psync_block_action *restrict blockactions,
                                            uint32_t fileidx){
  unsigned char *buff;
  uint64_t buffoff;
  psync_uint_t buffersize, hbuffersize, bufferlen, inbyteoff, outbyteoff, blockmask;
  ssize_t rd;
  psync_file_t fd;
  uint32_t adler, off;
  psync_sha1_ctx ctx;
  unsigned char sha1bin[PSYNC_SHA1_DIGEST_LEN];
  fd=psync_file_open(name, P_O_RDONLY, 0);
  if (fd==INVALID_HANDLE_VALUE)
    return;
  if (checksums->blocksize*2>PSYNC_COPY_BUFFER_SIZE)
    buffersize=checksums->blocksize*2;
  else
    buffersize=PSYNC_COPY_BUFFER_SIZE;
  hbuffersize=buffersize/2;
  buff=psync_malloc(buffersize);
  rd=psync_file_read(fd, buff, hbuffersize);
  if (unlikely(rd<(ssize_t)hbuffersize)){
    if (rd<(ssize_t)checksums->blocksize){
      psync_free(buff);
      psync_file_close(fd);
      return;
    }
    bufferlen=(rd+checksums->blocksize-1)/checksums->blocksize*checksums->blocksize;
    memset(buff+rd, 0, bufferlen-rd);
  }
  else
    bufferlen=buffersize;
  adler=adler32(ADLER32_INITIAL, buff, checksums->blocksize);
  outbyteoff=0;
  buffoff=0;
  inbyteoff=checksums->blocksize;
  blockmask=checksums->blocksize-1;
  while (1){
    if (unlikely((inbyteoff&blockmask)==0)){
      if (outbyteoff>=bufferlen)
        outbyteoff=0;
      if (inbyteoff==bufferlen){ /* not >=, bufferlen might be lower than current inbyteoff */
        if (bufferlen!=buffersize)
          break;
        buffoff+=buffersize;
        inbyteoff=0;
        rd=psync_file_read(fd, buff, hbuffersize);
        if (unlikely(rd!=hbuffersize)){
          if (rd<=0)
            break;
          else{
            bufferlen=(rd+checksums->blocksize-1)/checksums->blocksize*checksums->blocksize;
            memset(buff+hbuffersize+rd, 0, bufferlen-rd);
            bufferlen+=hbuffersize;
          }
        }
      }
      else if (inbyteoff==hbuffersize){
        rd=psync_file_read(fd, buff+hbuffersize, hbuffersize);
        if (unlikely(rd!=hbuffersize)){
          if (rd<=0)
            break;
          else{
            bufferlen=(rd+checksums->blocksize-1)/checksums->blocksize*checksums->blocksize;
            memset(buff+hbuffersize+rd, 0, bufferlen-rd);
            bufferlen+=hbuffersize;
          }
        }
      }
    }
    if (psync_net_hash_has_adler(hash, checksums, adler)){
      if (outbyteoff<inbyteoff)
        psync_sha1(buff+outbyteoff, checksums->blocksize, sha1bin);
      else{
        psync_sha1_init(&ctx);
        psync_sha1_update(&ctx, buff+outbyteoff, buffersize-outbyteoff);
        psync_sha1_update(&ctx, buff, inbyteoff);
        psync_sha1_final(sha1bin, &ctx);
      }
      off=psync_net_hash_has_adler_and_sha1(hash, checksums, adler, sha1bin);
      if (off)
        psync_net_block_match_found(hash, checksums, blockactions, off, fileidx, buffoff+outbyteoff);
    }
    adler=adler32_roll(adler, buff[outbyteoff++], buff[inbyteoff++], checksums->blocksize);
  }
  psync_free(buff);
  psync_file_close(fd);
}

int psync_net_download_ranges(psync_list *ranges, psync_fileid_t fileid, uint64_t filesize, char *const *files, uint32_t filecnt){
  psync_range_list_t *range;
  psync_file_checksums *checksums;
  psync_file_checksum_hash *hash;
  psync_block_action *blockactions;
  uint32_t i, bs;
  int rt;
  if (!filecnt)
    goto fulldownload;
  rt=psync_net_get_checksums(fileid, &checksums);
  if (unlikely_log(rt==PSYNC_NET_PERMFAIL))
    goto fulldownload;
  else if (unlikely_log(rt==PSYNC_NET_TEMPFAIL))
    return PSYNC_NET_TEMPFAIL;
  if (unlikely_log(checksums->filesize!=filesize)){
    psync_free(checksums);
    return PSYNC_NET_TEMPFAIL;
  }
  hash=psync_net_create_hash(checksums);
  blockactions=psync_new_cnt(psync_block_action, checksums->blockcnt);
  memset(blockactions, 0, sizeof(psync_block_action)*checksums->blockcnt);
  for (i=0; i<filecnt; i++)
    psync_net_check_file_for_blocks(files[i], checksums, hash, blockactions, i);
  psync_free(hash);
  psync_free(checksums);
  range=psync_new(psync_range_list_t);
  range->len=checksums->blocksize;
  range->type=blockactions[0].type;
  if (range->type==PSYNC_RANGE_COPY){
    range->off=blockactions[0].off;
    range->filename=files[blockactions[0].idx];
  }
  else
    range->off=0;
  psync_list_add_tail(ranges, &range->list);
  for (i=1; i<checksums->blockcnt; i++){
    if (i==checksums->blockcnt-1){
      bs=checksums->filesize%checksums->blocksize;
      if (!bs)
        bs=checksums->blocksize;
    }
    else
      bs=checksums->blocksize;
    if (blockactions[i].type!=range->type || (range->type==PSYNC_RANGE_COPY && 
         (range->filename!=files[blockactions[i].idx] || range->off+range->len!=blockactions[i].off))){
      range=psync_new(psync_range_list_t);
      range->len=bs;
      range->type=blockactions[i].type;
      if (range->type==PSYNC_RANGE_COPY){
        range->off=blockactions[i].off;
        range->filename=files[blockactions[i].idx];
      }
      else
        range->off=(uint64_t)i*checksums->blocksize;
      psync_list_add_tail(ranges, &range->list);
    }
    else
      range->len+=bs;
  }
  psync_free(blockactions);
  return PSYNC_NET_OK;
fulldownload:
  range=psync_new(psync_range_list_t);
  range->off=0;
  range->len=filesize;
  range->type=PSYNC_RANGE_TRANSFER;
  psync_list_add_tail(ranges, &range->list);
  return PSYNC_NET_OK;
}
