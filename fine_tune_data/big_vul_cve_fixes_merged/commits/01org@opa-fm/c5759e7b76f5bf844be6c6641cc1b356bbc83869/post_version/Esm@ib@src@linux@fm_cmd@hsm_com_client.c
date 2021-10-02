/* BEGIN_ICS_COPYRIGHT5 ****************************************

Copyright (c) 2015, Intel Corporation

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 * ** END_ICS_COPYRIGHT5   ****************************************/

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
#include "hsm_com_client_api.h"
#include "hsm_com_client_data.h"


int
unix_sck_send_msg(hsm_com_client_hdl_t *hdl, char *snd_buf, int snd_len, char *rcv_buf, int rcv_len, int timeout)
{
	int					nread = 0;
	int					n;
	fd_set				rset;
	struct timeval		tm;
	int					offset = 0;

	if (write(hdl->client_fd,snd_buf,snd_len)<0) {
		printf("return failed.\n");
		return 0;
	} 


	tm.tv_sec = timeout;
	tm.tv_usec = 0;

	FD_ZERO(&rset);


	FD_SET(hdl->client_fd,&rset);

	while(1)
	{

		if ( (n = select(hdl->client_fd + 1,&rset,NULL,NULL,&tm)) < 0){
			return 0;
		}

		if (FD_ISSET(hdl->client_fd, &rset)) 
		{
		
			if ( (nread = unix_sck_read_data(hdl->client_fd,
											 &hdl->scr,
											 hdl->recv_buf, 
											 hdl->buf_len,
											 &offset)) > 0)
			{
				if(nread <= rcv_len){
					memcpy(rcv_buf,hdl->recv_buf,nread);
					return nread;
				}
				// Response too big
				printf("response too big\n");
				return 0;
			}
			else if(nread < 0)
			{
				printf("Skipping since we need more data\n");
				continue;
			}
			else
			{
				// Error
				printf("Response is 0\n");
				return 0;
			}
		}
		
		
	}


	return nread;
}

hsm_com_errno_t
unix_sck_send_conn(hsm_com_client_hdl_t *hdl, int timeout)
{
	hsm_com_con_data_t	msg;

	memset(&msg,0,sizeof(msg));

	msg.header.cmd = HSM_COM_CMD_CONN;
	msg.header.ver = HSM_COM_VER;
	msg.header.trans_id = hdl->trans_id++;
	msg.header.payload_len = sizeof(msg.key);

	msg.key = HSM_COM_KEY;


	if(unix_sck_send_msg(hdl, (char*)&msg, sizeof(msg), (char*)&msg, 
						 sizeof(msg), timeout) != sizeof(msg))
	{
		// COM Error...
		// Close our connection
		close(hdl->client_fd);
		hdl->client_state = HSM_COM_C_STATE_IN;

		return HSM_COM_BAD;
	}

	if(msg.header.resp_code == HSM_COM_RESP_OK){
		return HSM_COM_OK;
	}

	return HSM_COM_BAD;
}


hsm_com_errno_t
unix_sck_send_disconnect(hsm_com_client_hdl_t *hdl, int timeout)
{
	hsm_com_discon_data_t	msg;

	memset(&msg,0,sizeof(msg));

	msg.header.cmd = HSM_COM_CMD_DISC;
	msg.header.ver = HSM_COM_VER;
	msg.header.trans_id = hdl->trans_id++;
	msg.header.payload_len = 0;

	if(unix_sck_send_msg(hdl, (char*)&msg, sizeof(msg), (char*)&msg, 
						 sizeof(msg), timeout) != sizeof(msg))
	{
		// COM Error...
		// Close our connection
		close(hdl->client_fd);
		hdl->client_state = HSM_COM_C_STATE_IN;

		return HSM_COM_BAD;
	}

	if(msg.header.resp_code == HSM_COM_RESP_OK){
		return HSM_COM_OK;
	}

	return HSM_COM_BAD;

}


hsm_com_errno_t
unix_sck_send_ping(hsm_com_client_hdl_t *hdl, int timeout)
{
	hsm_com_ping_data_t	msg;

	memset(&msg,0,sizeof(msg));

	msg.header.cmd = HSM_COM_CMD_PING;
	msg.header.ver = HSM_COM_VER;
	msg.header.trans_id = hdl->trans_id++;
	msg.header.payload_len = 0;

	if(unix_sck_send_msg(hdl, (char*)&msg, sizeof(msg), (char*)&msg, 
						 sizeof(msg), timeout) != sizeof(msg))
	{
		// COM Error...
		// Close our connection
		close(hdl->client_fd);
		hdl->client_state = HSM_COM_C_STATE_IN;

		return HSM_COM_BAD;
	}

	if(msg.header.resp_code == HSM_COM_RESP_OK){
		return HSM_COM_OK;
	}

	return HSM_COM_BAD;

}

hsm_com_errno_t
unix_sck_send_data(hsm_com_client_hdl_t *hdl, int timeout, 
				   hsm_com_datagram_t *send, hsm_com_datagram_t *recv)
{
	hsm_com_msg_t	*msg;
	int total_len;

	msg = (hsm_com_msg_t*)hdl->send_buf;

	msg->common.cmd = HSM_COM_CMD_DATA;
	msg->common.ver = HSM_COM_VER;
	msg->common.trans_id = hdl->trans_id++;
	msg->common.payload_len = send->data_len;

	total_len = sizeof(msg->common) + send->data_len;

	memcpy(&msg->data[0],send->buf,send->data_len);

	if(unix_sck_send_msg(hdl, hdl->send_buf, total_len, hdl->recv_buf, 
						 total_len, timeout) != total_len)
	{

		return HSM_COM_BAD;
	}

	msg = (hsm_com_msg_t*)hdl->recv_buf;
	if(msg->common.resp_code == HSM_COM_RESP_OK){
		memcpy(recv->buf,&msg->data[0],msg->common.payload_len);
		recv->data_len = msg->common.payload_len;
		return HSM_COM_OK;
	}

	return HSM_COM_BAD;

}


hsm_com_errno_t
unix_client_connect(hsm_com_client_hdl_t *hdl)
{
	int					fd, len;
	struct sockaddr_un	unix_addr;
	hsm_com_errno_t		res = HSM_COM_OK;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) 
	{
		return HSM_COM_ERROR;
	}

	memset(&unix_addr,0,sizeof(unix_addr));

	unix_addr.sun_family = AF_UNIX;
	
	if(strlen(hdl->c_path) >= sizeof(unix_addr.sun_path))
	{
		res = HSM_COM_PATH_ERR;
		goto cleanup;
	}

	snprintf(unix_addr.sun_path, sizeof(unix_addr.sun_path), "%s", hdl->c_path);

	len = SUN_LEN(&unix_addr);

	unlink(unix_addr.sun_path);

	if(bind(fd, (struct sockaddr *)&unix_addr, len) < 0)
	{
		res = HSM_COM_BIND_ERR;
		goto cleanup;
	}

	if(chmod(unix_addr.sun_path, S_IRWXU) < 0)
	{
		res = HSM_COM_CHMOD_ERR;
		goto cleanup;
	}

	memset(&unix_addr,0,sizeof(unix_addr));

	unix_addr.sun_family = AF_UNIX;
	strncpy(unix_addr.sun_path, hdl->s_path, sizeof(unix_addr.sun_path));
	unix_addr.sun_path[sizeof(unix_addr.sun_path)-1] = 0;

	len = SUN_LEN(&unix_addr);

	if (connect(fd, (struct sockaddr *) &unix_addr, len) < 0) 
	{
		res = HSM_COM_CONX_ERR;
		goto cleanup;
	}

	hdl->client_fd = fd;
	hdl->client_state = HSM_COM_C_STATE_CT;

	// Send connection data packet
	if(unix_sck_send_conn(hdl, 2) != HSM_COM_OK)
	{
		hdl->client_state = HSM_COM_C_STATE_IN;
		res = HSM_COM_SEND_ERR;
	}

	return res;

cleanup:
	close(fd);
	return res;

}


hsm_com_errno_t
unix_client_disconnect(hsm_com_client_hdl_t *hdl)
{
	// Send connection data packet
	if(unix_sck_send_disconnect(hdl, 2) != HSM_COM_OK)
	{
		return(-1);
	}

	close(hdl->client_fd);
	hdl->client_state = HSM_COM_C_STATE_IN;


	return HSM_COM_OK;

}

