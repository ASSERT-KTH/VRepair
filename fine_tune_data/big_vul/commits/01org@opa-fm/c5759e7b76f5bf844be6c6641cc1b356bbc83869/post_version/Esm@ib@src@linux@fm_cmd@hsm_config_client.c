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
#include "hsm_config_client_api.h"
#include "hsm_config_client_data.h"
#include "iba/public/ibyteswap.h"
#include "ssapi_internal.h"

fm_mgr_config_errno_t
fm_mgr_config_mgr_connect
(
	fm_config_conx_hdl	*hdl, 
	fm_mgr_type_t 		mgr
)
{
	char                    s_path[256];
	char                    c_path[256];
	char                    *mgr_prefix;
	p_hsm_com_client_hdl_t  *mgr_hdl;

	memset(s_path,0,sizeof(s_path));
	memset(c_path,0,sizeof(c_path));

	switch ( mgr )
	{
		case FM_MGR_SM:
			mgr_prefix  = HSM_FM_SCK_SM;
			mgr_hdl     = &hdl->sm_hdl;
			break;
		case FM_MGR_PM:
			mgr_prefix  = HSM_FM_SCK_PM;
			mgr_hdl     = &hdl->pm_hdl;
			break;
		case FM_MGR_FE:
			mgr_prefix  = HSM_FM_SCK_FE;
			mgr_hdl     = &hdl->fe_hdl;
			break;
		default:
			return FM_CONF_INIT_ERR;
	}

	// Fill in the paths for the server and client sockets.
	sprintf(s_path,"%s%s%d",HSM_FM_SCK_PREFIX,mgr_prefix,hdl->instance);

	sprintf(c_path,"%s%s%d_C_XXXXXX",HSM_FM_SCK_PREFIX,mgr_prefix,hdl->instance);

	if ( *mgr_hdl == NULL )
	{
		if ( hcom_client_init(mgr_hdl,s_path,c_path,32768) != HSM_COM_OK )
		{
			return FM_CONF_INIT_ERR;
		}
	}

	if ( hcom_client_connect(*mgr_hdl) == HSM_COM_OK )
	{
		hdl->conx_mask |= mgr;
		return FM_CONF_OK;
	}

	return FM_CONF_CONX_ERR;

}

// init
fm_mgr_config_errno_t
fm_mgr_config_init
(
					OUT	p_fm_config_conx_hdlt		*p_hdl,
				IN		int							instance,
	OPTIONAL	IN		char						*rem_address,
	OPTIONAL	IN		char						*community
)
{
	fm_config_conx_hdl      *hdl;
	fm_mgr_config_errno_t   res = FM_CONF_OK;


	if ( (hdl = calloc(1,sizeof(fm_config_conx_hdl))) == NULL )
	{
		res = FM_CONF_NO_MEM;
		goto cleanup;
	}

	hdl->instance = instance;

	*p_hdl = hdl;

		// connect to the snmp agent via localhost?
	if(!rem_address || (strcmp(rem_address,"localhost") == 0))
	{
		if ( fm_mgr_config_mgr_connect(hdl, FM_MGR_SM) == FM_CONF_INIT_ERR )
		{
			res = FM_CONF_INIT_ERR;
			goto cleanup;
		}

		if ( fm_mgr_config_mgr_connect(hdl, FM_MGR_PM) == FM_CONF_INIT_ERR )
		{
			res = FM_CONF_INIT_ERR;
			goto cleanup;
		}

		if ( fm_mgr_config_mgr_connect(hdl, FM_MGR_FE) == FM_CONF_INIT_ERR )
		{
			res = FM_CONF_INIT_ERR;
			goto cleanup;
		}
	}

cleanup:
	return res;
}


// connect
fm_mgr_config_errno_t
fm_mgr_config_connect
(
	IN      p_fm_config_conx_hdlt       p_hdl
)
{
	fm_config_conx_hdl      *hdl = p_hdl;
	fm_mgr_config_errno_t   res = FM_CONF_OK;
	int						fail_count = 0;

    if ( (res = fm_mgr_config_mgr_connect(hdl, FM_MGR_SM)) == FM_CONF_INIT_ERR )
    {
        res = FM_CONF_INIT_ERR;
        goto cleanup;
    }

    if(res != FM_CONF_OK)
        fail_count++;

    if ( (res = fm_mgr_config_mgr_connect(hdl, FM_MGR_PM)) == FM_CONF_INIT_ERR )
    {
        res = FM_CONF_INIT_ERR;
        goto cleanup;
    }

    if(res != FM_CONF_OK)
        fail_count++;

    if ( (res = fm_mgr_config_mgr_connect(hdl, FM_MGR_FE)) == FM_CONF_INIT_ERR )
    {
        res = FM_CONF_INIT_ERR;
        goto cleanup;
    }

    if(res != FM_CONF_OK)
        fail_count++;

    if(fail_count < 4){
        res = FM_CONF_OK;
    }

	cleanup:

	return res;
}

fm_mgr_config_errno_t
fm_mgr_general_query
(
	IN      p_hsm_com_client_hdl_t      client_hdl,
	IN      fm_mgr_action_t             action,
	IN      fm_datatype_t               data_type_id,
	IN      int                         data_len,
		OUT void                        *data,
		OUT fm_msg_ret_code_t           *ret_code
)
{
	fm_config_datagram_t    *dg;
	hsm_com_datagram_t      com_dg;
	int                     len;
	hsm_com_errno_t			com_res;

	len = data_len + (sizeof(fm_config_datagram_header_t));


	if ( (dg = malloc(len)) == NULL )
	{
		return FM_CONF_NO_MEM;
	}

	dg->header.action = action;
	dg->header.data_id = data_type_id;
	dg->header.data_len = data_len;

	memcpy(&dg->data[0],data,data_len);

	com_dg.buf = (char*)dg;
	com_dg.buf_size = len;
	com_dg.data_len = len;

	if ( (com_res = hcom_client_send_data(client_hdl,60,&com_dg,&com_dg)) != HSM_COM_OK )
	{
		free(dg);
		if(com_res == HSM_COM_NOT_CONNECTED)
		{
			return FM_CONF_ERR_DISC; 
		}
		return FM_CONF_NO_RESP;
	}

	if ( ret_code )
		*ret_code = dg->header.ret_code;

	if ( dg->header.ret_code != FM_RET_OK )
	{
		free(dg);
		return FM_CONF_ERROR;
	} else
	{
		if ( dg->header.data_len != data_len )
		{
			free(dg);
			if ( ret_code )
				*ret_code = FM_RET_BAD_RET_LEN;
			return FM_CONF_ERR_LEN;
		}
		memcpy(data,&dg->data[0],data_len);
	}

	free(dg);
	return FM_CONF_OK;
}



fm_mgr_config_errno_t
fm_mgr_status_query
(
	IN      p_hsm_com_client_hdl_t      client_hdl,
	IN      fm_mgr_action_t             action,
	IN      fm_datatype_t               data_type_id,
	IN      int                         data_len,
		OUT void                        *data,
		OUT fm_msg_ret_code_t           *ret_code
)
{
	fm_config_datagram_t    *dg;
	hsm_com_datagram_t      com_dg;
	int                     len;
	hsm_com_errno_t			com_res;

	len = data_len + (sizeof(fm_config_datagram_header_t));


	if ( (dg = malloc(len)) == NULL )
	{
		return FM_CONF_NO_MEM;
	}

	dg->header.action = action;
	dg->header.data_id = data_type_id;
	dg->header.data_len = data_len;

	memcpy(&dg->data[0],data,data_len);

	com_dg.buf = (char*)dg;
	com_dg.buf_size = len;
	com_dg.data_len = len;

	if ( (com_res = hcom_client_send_data(client_hdl,10,&com_dg,&com_dg)) != HSM_COM_OK )
	{
		free(dg);
		if(com_res == HSM_COM_NOT_CONNECTED)
		{
			return FM_CONF_ERR_DISC; 
		}
		return FM_CONF_NO_RESP;
	}

	if ( ret_code )
		*ret_code = dg->header.ret_code;

	if ( dg->header.ret_code != FM_RET_OK )
	{
		free(dg);
		return FM_CONF_ERROR;
	} else
	{
		if ( dg->header.data_len != data_len )
		{
			free(dg);
			if ( ret_code )
				*ret_code = FM_RET_BAD_RET_LEN;
			return FM_CONF_ERR_LEN;
		}
		memcpy(data,&dg->data[0],data_len);
	}

	free(dg);
	return FM_CONF_OK;
}


p_hsm_com_client_hdl_t
get_mgr_hdl
(
	IN      p_fm_config_conx_hdlt       hdl,
	IN      fm_mgr_type_t               mgr
)
{
	switch ( mgr )
	{
		case FM_MGR_SM:
			return hdl->sm_hdl;
		case FM_MGR_PM:
			return hdl->pm_hdl;
		case FM_MGR_FE:
			return hdl->fe_hdl;
		default:
			return NULL;
	}

	return NULL;

}



/*                                                              
 * simple local instance only queries                                                                
 */
fm_mgr_config_errno_t
fm_mgr_simple_query
(
	IN      p_fm_config_conx_hdlt       hdl,
	IN      fm_mgr_action_t             action,
	IN      fm_datatype_t               data_type_id,
	IN		fm_mgr_type_t				mgr,
	IN      int                         data_len,
		OUT void                        *data,
		OUT fm_msg_ret_code_t           *ret_code
)
{
	p_hsm_com_client_hdl_t client_hdl;

	if ( (client_hdl = get_mgr_hdl(hdl,mgr)) != NULL ) {
		return fm_mgr_general_query(client_hdl,action,data_type_id,data_len,data,ret_code);
	}
	return FM_CONF_ERROR;
}



fm_mgr_config_errno_t
fm_mgr_commong_cfg_query
(
	IN      p_fm_config_conx_hdlt       hdl,
	IN      fm_mgr_type_t               mgr,
	IN      fm_mgr_action_t             action,
		OUT fm_config_common_t          *info,
		OUT fm_msg_ret_code_t           *ret_code
)
{
	p_hsm_com_client_hdl_t client_hdl;

    if ( (client_hdl = get_mgr_hdl(hdl,mgr)) != NULL )
    {
        return fm_mgr_general_query(client_hdl,action,FM_DT_COMMON,sizeof(fm_config_common_t),info,ret_code);
    }

	return FM_CONF_ERROR;

}


fm_mgr_config_errno_t
fm_mgr_fe_cfg_query
(
	IN      p_fm_config_conx_hdlt       hdl,
	IN      fm_mgr_action_t             action,
		OUT fe_config_t                 *info,
		OUT fm_msg_ret_code_t           *ret_code
)
{
	p_hsm_com_client_hdl_t client_hdl;
	
    if ( (client_hdl = get_mgr_hdl(hdl,FM_MGR_FE)) != NULL )
    {
        return fm_mgr_general_query(client_hdl,action,FM_DT_FE_CFG,sizeof(fe_config_t),info,ret_code);
    }

	return FM_CONF_ERROR;


}

fm_mgr_config_errno_t
fm_mgr_pm_cfg_query
(
	IN      p_fm_config_conx_hdlt       hdl,
	IN      fm_mgr_action_t             action,
		OUT pm_config_t                 *info,
		OUT fm_msg_ret_code_t           *ret_code
)
{
	p_hsm_com_client_hdl_t client_hdl;

    if ( (client_hdl = get_mgr_hdl(hdl,FM_MGR_PM)) != NULL )
    {
        return fm_mgr_general_query(client_hdl,action,FM_DT_PM_CFG,sizeof(pm_config_t),info,ret_code);
    }

	return FM_CONF_ERROR;

}



fm_mgr_config_errno_t
fm_mgr_sm_cfg_query
(
	IN      p_fm_config_conx_hdlt       hdl,
	IN      fm_mgr_action_t             action,
		OUT sm_config_t                 *info,
		OUT fm_msg_ret_code_t           *ret_code
)
{
	p_hsm_com_client_hdl_t	client_hdl;

    if ( (client_hdl = get_mgr_hdl(hdl,FM_MGR_SM)) != NULL )
    {
        return fm_mgr_general_query(client_hdl,action,FM_DT_SM_CFG,sizeof(sm_config_t),info,ret_code);
    }
	
	return FM_CONF_ERROR;

}


fm_mgr_config_errno_t
fm_sm_status_query
(
	IN		p_fm_config_conx_hdlt		hdl,
	IN		fm_mgr_action_t				action,
		OUT	fm_sm_status_t				*info,
		OUT	fm_msg_ret_code_t			*ret_code
)
{
	p_hsm_com_client_hdl_t	client_hdl;

    if ( (client_hdl = get_mgr_hdl(hdl,FM_MGR_SM)) != NULL )
    {
        return fm_mgr_general_query(client_hdl,action,FM_DT_SM_STATUS,sizeof(*info),info,ret_code);
    }

	return FM_CONF_ERROR;
}

fm_mgr_config_errno_t
fm_pm_status_query
(
	IN		p_fm_config_conx_hdlt		hdl,
	IN		fm_mgr_action_t				action,
		OUT	fm_pm_status_t				*info,
		OUT	fm_msg_ret_code_t			*ret_code
)
{
	p_hsm_com_client_hdl_t	client_hdl;

    if ( (client_hdl = get_mgr_hdl(hdl,FM_MGR_PM)) != NULL )
    {
        return fm_mgr_general_query(client_hdl,action,FM_DT_PM_STATUS,sizeof(*info),info,ret_code);
    }

	return FM_CONF_ERROR;
}

fm_mgr_config_errno_t
fm_fe_status_query
(
	IN		p_fm_config_conx_hdlt		hdl,
	IN		fm_mgr_action_t				action,
		OUT	fm_fe_status_t				*info,
		OUT	fm_msg_ret_code_t			*ret_code
)
{
	p_hsm_com_client_hdl_t	client_hdl;

    if ( (client_hdl = get_mgr_hdl(hdl,FM_MGR_FE)) != NULL )
    {
        return fm_mgr_general_query(client_hdl,action,FM_DT_FE_STATUS,sizeof(*info),info,ret_code);
    }

	return FM_CONF_ERROR;

}

const char*
fm_mgr_get_error_str
(
	IN		fm_mgr_config_errno_t err
)
{
	switch(err){
		case FM_CONF_ERR_LEN:
			return "Response data legth invalid";
		case FM_CONF_ERR_VERSION:
			return "Client/Server version mismatch";
		case FM_CONF_ERR_DISC:
			return "Not connected";
		case FM_CONF_TEST:
			return "Test message";
		case FM_CONF_OK:
			return "Ok";
		case FM_CONF_ERROR:
			return "General error";
		case FM_CONF_NO_RESOURCES:
			return "Out of resources";
		case FM_CONF_NO_MEM:
			return "No memory";
		case FM_CONF_PATH_ERR:
			return "Invalid path";
		case FM_CONF_BAD:
			return "Bad argument";
		case FM_CONF_BIND_ERR:
			return "Could not bind socket";
		case FM_CONF_SOCK_ERR:
			return "Could not create socket";
		case FM_CONF_CHMOD_ERR:
			return "Invalid permissions on socket";
		case FM_CONF_CONX_ERR:
			return "Connection Error";
		case FM_CONF_SEND_ERR:
			return "Send error";
		case FM_CONF_INIT_ERR:
			return "Could not initalize resource";
		case FM_CONF_NO_RESP:
			return "No Response";
		case FM_CONF_MAX_ERROR_NUM:
		default:
			return "Unknown error";
	}

	return "Unknown error";

}

const char*
fm_mgr_get_resp_error_str
(
	IN		fm_msg_ret_code_t err
)
{
	switch(err){
		case FM_RET_BAD_RET_LEN:
			return "Return data length invalid";
		case FM_RET_OK:
			return "Ok";
		case FM_RET_DT_NOT_SUPPORTED:
			return "Data type not supported";
		case FM_RET_ACT_NOT_SUPPORTED:
			return "Action not supported";
		case FM_RET_INVALID:
			return "Invalid";
		case FM_RET_BAD_LEN:
			return "Send data length invalid";
		case FM_RET_BUSY:
			return "Busy";
		case FM_RET_NOT_FOUND:
			return "Record not found";
		case FM_RET_NO_NEXT:
			return "No next instance";
		case FM_RET_NOT_MASTER:
			return "SM is not master";
		case FM_RET_NOSUCHOBJECT:
			return "SNMP Err: No such object";
		case FM_RET_NOSUCHINSTANCE:
			return "SNMP Err: No such instance";
		case FM_RET_ENDOFMIBVIEW:
			return "SNMP Err: End of view";
		case FM_RET_ERR_NOERROR:
			return "SNMP Err: No error";
		case FM_RET_ERR_TOOBIG:
			return "SNMP Err: Object too big";
		case FM_RET_ERR_NOSUCHNAME:
			return "SNMP Err: no such name";
		case FM_RET_ERR_BADVALUE:
			return "SNMP Err: Bad value";
		case FM_RET_ERR_READONLY:
			return "SNMP Err: Read only";
		case FM_RET_ERR_GENERR:
			return "SNMP Err: General Error";
		case FM_RET_ERR_NOACCESS:
			return "SNMP Err: No Access";
		case FM_RET_ERR_WRONGTYPE:
			return "SNMP Err: Wrong Type";
		case FM_RET_ERR_WRONGLENGTH:
			return "SNMP Err: Wrong length";
		case FM_RET_ERR_WRONGENCODING:
			return "SNMP Err: Wrong encoding";
		case FM_RET_ERR_WRONGVALUE:
			return "SNMP Err: Wrong Value";
		case FM_RET_ERR_NOCREATION:
			return "SNMP Err: No Creation";
		case FM_RET_ERR_INCONSISTENTVALUE:
			return "SNMP Err: Inconsistent value";
		case FM_RET_ERR_RESOURCEUNAVAILABLE:
			return "SNMP Err: Resource Unavailable";
		case FM_RET_ERR_COMMITFAILED:
			return "SNMP Err: Commit failed";
		case FM_RET_ERR_UNDOFAILED:
			return "SNMP Err: Undo failed";
		case FM_RET_ERR_AUTHORIZATIONERROR:
			return "SNMP Err: Authorization error";
		case FM_RET_ERR_NOTWRITABLE:
			return "SNMP Err: Not Writable";
		case FM_RET_TIMEOUT:
			return "SNMP Err: Timeout";
		case FM_RET_UNKNOWN_DT:
			return "Unknown Datatype";
		case FM_RET_END_OF_TABLE:
			return "End of Table";
		case FM_RET_INTERNAL_ERR:
			return "Internal Error";
		case FM_RET_CONX_CLOSED:
			return "Connection Closed";

	}

	return "Unknown code";
}

/* The current error map is copied to the pointer provided */
fm_mgr_config_errno_t
fm_mgr_config_get_error_map
(
	IN		p_fm_config_conx_hdlt	hdl,
		OUT	fm_error_map_t			*error_map
)
{
	if(error_map){
		memcpy(error_map,&hdl->error_map,sizeof(fm_error_map_t));
		return FM_CONF_OK;
	}

	return FM_CONF_ERROR;
}

fm_mgr_config_errno_t
fm_mgr_config_clear_error_map
(
	IN		p_fm_config_conx_hdlt	hdl
)
{
	if(hdl->error_map.err_set)
		memset(&hdl->error_map,0,sizeof(hdl->error_map));

	return FM_CONF_OK;
}


/* The current error map is copied to the pointer provided */
fm_mgr_config_errno_t
fm_mgr_config_get_error_map_entry
(
	IN		p_fm_config_conx_hdlt	hdl,
	IN		uint64_t				mask,
		OUT	fm_mgr_config_errno_t	*error_code
)
{
	int i;

	for(i=0;i<64;i++){
		if(mask & 0x01){
			if((mask >> 1)){
				return FM_CONF_ERROR;
			}
			*error_code = hdl->error_map.map[i];
			return FM_CONF_OK;
		}
		mask >>= 1;
	}

	return FM_CONF_ERROR;
}

/* The current error map is copied to the pointer provided */
fm_mgr_config_errno_t
fm_mgr_config_set_error_map_entry
(
	IN		p_fm_config_conx_hdlt	hdl,
	IN		uint64_t				mask,
	IN		fm_mgr_config_errno_t	error_code
)
{
	int i;

	for(i=0;i<64;i++){
		if(mask & 0x01){
			if((mask >> 1)){
				return FM_CONF_ERROR;
			}
			hdl->error_map.err_set = 1;
			hdl->error_map.map[i] = error_code;
			return FM_CONF_OK;
		}
		mask >>= 1;
	}

	return FM_CONF_ERROR;
}




