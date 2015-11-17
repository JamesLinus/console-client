/* Copyright (c) 2013-2015 pCloud Ltd.
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

#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>

#include "overlay_client.h"
#define POVERLAY_BUFSIZE 512

typedef struct _message {
  uint32_t type;
  uint64_t length;
  char value[];
} message;

char *clsoc = "/mydir/pcloud_unix_soc";

int QueryState(pCloud_FileState *state, char* path)
{
  struct sockaddr_un addr;
  int fd,rc;
  int path_size =  strlen(path);
  int mess_size = sizeof(message)+path_size + 1;
  int bytes_writen = 0;
  char *curbuf = NULL;
  char buf[POVERLAY_BUFSIZE];
  int bytes_read = 0;
  

  
  if ( (fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    return -1;
  }
  memset(&addr, 0, sizeof(addr));

  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, clsoc, sizeof(addr.sun_path)-1);

#if defined(P_OS_MACOSX)
  addr.sun_len = sizeof(addr);
#endif 
  
  if (connect(fd, (struct sockaddr*)&addr,SUN_LEN(&addr)) == -1) {
    return -2;
  }

  message* mes = (message *)malloc(mess_size);
  mes->type = 4;
  strncpy(mes->value, path, path_size);
  mes->length = mess_size;
  curbuf = (char *)mes;
  while ((rc = write(fd,curbuf,(mes->length - bytes_writen))) > 0) {
    bytes_writen += rc;
    curbuf = curbuf + rc;
  }
  
  if (bytes_writen != mes->length)
    return -3;
 
  curbuf = buf;
  while ( (rc=read(fd,curbuf,(POVERLAY_BUFSIZE - bytes_read))) > 0) {
    bytes_read += rc;
    curbuf = curbuf + rc;
  }
  message *rep = (message *)buf;
 
  if (rep->type == 11)
    *state = FileStateInSync;
  else if (rep->type == 12)
    *state = FileStateInProgress;
  else if (rep->type == 11)
    *state = FileStateNoSync;
  else 
    *state = FileStateInvalid;
  
  return 0;
}