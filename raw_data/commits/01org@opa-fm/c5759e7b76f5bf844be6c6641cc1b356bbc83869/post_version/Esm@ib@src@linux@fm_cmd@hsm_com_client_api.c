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
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include "hsm_com_client_api.h"
#include "hsm_com_client_data.h"


   
   
hsm_com_errno_t
hcom_client_init
(
		OUT	p_hsm_com_client_hdl_t	*p_hdl,
	IN		char					*server_path,
	IN		char					*client_path,
	IN		int						max_data_len
)
{
	hsm_com_client_hdl_t	*hdl = NULL;
	hsm_com_errno_t			res = HSM_COM_OK;
	

	if((strlen(server_path) > (HSM_COM_SVR_MAX_PATH - 1)) ||
	   (strlen(server_path) == 0)){
		res = HSM_COM_PATH_ERR;
		goto cleanup;
	}

	if((strlen(client_path) > (HSM_COM_SVR_MAX_PATH - 1)) ||
	   (strlen(client_path) == 0)){
		res = HSM_COM_PATH_ERR;
		goto cleanup;
	}


	if((hdl = calloc(1,sizeof(hsm_com_client_hdl_t))) == NULL)
	{
		res = HSM_COM_NO_MEM;
		goto cleanup;
	}

	if((hdl->scr.scratch = malloc(max_data_len)) == NULL) 
	{
		res = HSM_COM_NO_MEM;
		goto cleanup;
	}

	if((hdl->recv_buf = malloc(max_data_len)) == NULL) 
	{
		res = HSM_COM_NO_MEM;
		goto cleanup;
	}

	if((hdl->send_buf = malloc(max_data_len)) == NULL) 
	{
		res = HSM_COM_NO_MEM;
		goto cleanup;
	}

	hdl->scr.scratch_fill = 0;
	hdl->scr.scratch_len = max_data_len;
	hdl->buf_len = max_data_len;
	hdl->trans_id = 1;


	strcpy(hdl->s_path,server_path);
	strcpy(hdl->c_path,client_path);

	if (mkstemp(hdl->c_path) == -1)
	{
		res = HSM_COM_PATH_ERR;
		goto cleanup;
	}

	hdl->client_state = HSM_COM_C_STATE_IN;

	*p_hdl = hdl;

	return res;

cleanup:
	if(hdl)
	{
		if (hdl->scr.scratch) {
			free(hdl->scr.scratch);
		}
		if (hdl->recv_buf) {
			free(hdl->recv_buf);
		}
		free(hdl);
	}

	return res;

}

hsm_com_errno_t
hcom_client_connect
(
	IN p_hsm_com_client_hdl_t p_hdl		
)
{
	return unix_client_connect(p_hdl);
}


hsm_com_errno_t
hcom_client_disconnect
(
	IN p_hsm_com_client_hdl_t p_hdl		
)
{
	return unix_client_disconnect(p_hdl);
}



hsm_com_errno_t
hcom_client_send_ping
(
	IN	p_hsm_com_client_hdl_t	p_hdl,	
	IN	int						timeout_s
)
{
	return unix_sck_send_ping(p_hdl,timeout_s);
}



hsm_com_errno_t
hcom_client_send_data
(
	IN		p_hsm_com_client_hdl_t	p_hdl,
	IN		int						timeout_s,
	IN		hsm_com_datagram_t		*data,
		OUT	hsm_com_datagram_t		*res
)
{
	if(p_hdl->client_state == HSM_COM_C_STATE_CT)
		return unix_sck_send_data(p_hdl, timeout_s, data, res); 

	return HSM_COM_NOT_CONNECTED;
}

hsm_com_errno_t
hcom_client_create_stream
(
		OUT	p_hsm_com_stream_hdl_t	*p_stream_hdl,
	IN		p_hsm_com_client_hdl_t	p_client_hdl,
	IN		char					*socket_path,
	IN		int						max_conx,		
	IN		int						max_data_len
)
{
	return HSM_COM_OK;
}




hsm_com_errno_t
hcom_client_destroy_stream
(
	IN	p_hsm_com_stream_hdl_t	p_stream_hdl,
	IN	p_hsm_com_client_hdl_t	p_client_hdl
)
{
	return HSM_COM_OK;
}

