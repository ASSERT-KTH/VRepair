/******************************************************************************
 *
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *****************************************************************************
 * Originally developed and contributed by Ittiam Systems Pvt. Ltd, Bangalore
*/
/*****************************************************************************/
/*                                                                           */
/*  File Name         : decoder_api_main.c                                   */
/*                                                                           */
/*  Description       : Functions which recieve the API call from user       */
/*                                                                           */
/*  List of Functions : <List the functions defined in this file>            */
/*                                                                           */
/*  Issues / Problems : None                                                 */
/*                                                                           */
/*  Revision History  :                                                      */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         30 05 2007   Rajneesh        Creation                             */
/*                                                                           */
/*****************************************************************************/

/*****************************************************************************/
/* File Includes                                                             */
/*****************************************************************************/

/* System include files */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

/* User include files */
#include "iv_datatypedef.h"
#include "iv.h"
#include "ivd.h"
#include "ithread.h"

#include "impeg2_job_queue.h"
#include "impeg2_macros.h"
#include "impeg2_buf_mgr.h"
#include "impeg2_disp_mgr.h"
#include "impeg2_defs.h"
#include "impeg2_platform_macros.h"
#include "impeg2_inter_pred.h"
#include "impeg2_idct.h"
#include "impeg2_format_conv.h"
#include "impeg2_mem_func.h"

#include "impeg2d.h"
#include "impeg2d_api.h"
#include "impeg2d_bitstream.h"
#include "impeg2d_debug.h"
#include "impeg2d_structs.h"
#include "impeg2d_mc.h"
#include "impeg2d_pic_proc.h"
#include "impeg2d_deinterlace.h"

#define NUM_FRAMES_LIMIT_ENABLED 0

#ifdef LOGO_EN
#include "impeg2_ittiam_logo.h"
#define INSERT_LOGO(buf_y, buf_u, buf_v, stride, x_pos, y_pos, yuv_fmt,disp_wd,disp_ht) impeg2_insert_logo(buf_y, buf_u, buf_v, stride, x_pos, y_pos, yuv_fmt,disp_wd,disp_ht);
#else
#define INSERT_LOGO(buf_y, buf_u, buf_v, stride, x_pos, y_pos, yuv_fmt,disp_wd,disp_ht)
#endif

#if NUM_FRAMES_LIMIT_ENABLED
#define NUM_FRAMES_LIMIT 10000
#else
#define NUM_FRAMES_LIMIT 0x7FFFFFFF
#endif

#define CODEC_NAME              "MPEG2VDEC"
#define CODEC_RELEASE_TYPE      "eval"
#define CODEC_RELEASE_VER       "01.00"
#define CODEC_VENDOR            "ITTIAM"

#ifdef __ANDROID__
#define VERSION(version_string, codec_name, codec_release_type, codec_release_ver, codec_vendor)    \
    strcpy(version_string,"@(#)Id:");                                                               \
    strcat(version_string,codec_name);                                                              \
    strcat(version_string,"_");                                                                     \
    strcat(version_string,codec_release_type);                                                      \
    strcat(version_string," Ver:");                                                                 \
    strcat(version_string,codec_release_ver);                                                       \
    strcat(version_string," Released by ");                                                         \
    strcat(version_string,codec_vendor);
#else
#define VERSION(version_string, codec_name, codec_release_type, codec_release_ver, codec_vendor)    \
    strcpy(version_string,"@(#)Id:");                                                               \
    strcat(version_string,codec_name);                                                              \
    strcat(version_string,"_");                                                                     \
    strcat(version_string,codec_release_type);                                                      \
    strcat(version_string," Ver:");                                                                 \
    strcat(version_string,codec_release_ver);                                                       \
    strcat(version_string," Released by ");                                                         \
    strcat(version_string,codec_vendor);                                                            \
    strcat(version_string," Build: ");                                                              \
    strcat(version_string,__DATE__);                                                                \
    strcat(version_string," @ ");                                                                       \
    strcat(version_string,__TIME__);
#endif


#define MIN_OUT_BUFS_420    3
#define MIN_OUT_BUFS_422ILE 1
#define MIN_OUT_BUFS_RGB565 1
#define MIN_OUT_BUFS_420SP  2


void impeg2d_init_arch(void *pv_codec);
void impeg2d_init_function_ptr(void *pv_codec);

/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_api_rel_display_frame                            */
/*                                                                           */
/*  Description   : Release displ buffers that will be shared between decoder */
/*                  and application                                          */
/*  Inputs        : Error message                                            */
/*  Globals       : None                                                     */
/*  Processing    : Just prints error message to console                     */
/*  Outputs       : Error mesage to the console                              */
/*  Returns       : None                                                     */
/*                                                                           */
/*  Issues        : <List any issues or problems with this function>         */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         27 05 2006   Sankar          Creation                             */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_rel_display_frame(iv_obj_t *ps_dechdl,
                                                   void *pv_api_ip,
                                                   void *pv_api_op)
{

    ivd_rel_display_frame_ip_t  *dec_rel_disp_ip;
    ivd_rel_display_frame_op_t  *dec_rel_disp_op;

    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;


    dec_rel_disp_ip = (ivd_rel_display_frame_ip_t  *)pv_api_ip;
    dec_rel_disp_op = (ivd_rel_display_frame_op_t  *)pv_api_op;

    dec_rel_disp_op->u4_error_code = 0;
    ps_dec_state_multi_core = (dec_state_multi_core_t *) (ps_dechdl->pv_codec_handle);
    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];


    /* If not in shared disp buf mode, return */
    if(0 == ps_dec_state->u4_share_disp_buf)
        return IV_SUCCESS;

    if(NULL == ps_dec_state->pv_pic_buf_mg)
        return IV_SUCCESS;


    impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg, dec_rel_disp_ip->u4_disp_buf_id, BUF_MGR_DISP);

    return IV_SUCCESS;
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_api_set_display_frame                            */
/*                                                                           */
/*  Description   : Sets display buffers that will be shared between decoder */
/*                  and application                                          */
/*  Inputs        : Error message                                            */
/*  Globals       : None                                                     */
/*  Processing    : Just prints error message to console                     */
/*  Outputs       : Error mesage to the console                              */
/*  Returns       : None                                                     */
/*                                                                           */
/*  Issues        : <List any issues or problems with this function>         */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         27 05 2006   Sankar          Creation                             */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_set_display_frame(iv_obj_t *ps_dechdl,
                                          void *pv_api_ip,
                                          void *pv_api_op)
{

    ivd_set_display_frame_ip_t  *dec_disp_ip;
    ivd_set_display_frame_op_t  *dec_disp_op;

    UWORD32 i;
    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;
    UWORD32 u4_num_disp_bufs;


    dec_disp_ip = (ivd_set_display_frame_ip_t  *)pv_api_ip;
    dec_disp_op = (ivd_set_display_frame_op_t  *)pv_api_op;
    dec_disp_op->u4_error_code = 0;

    u4_num_disp_bufs = dec_disp_ip->num_disp_bufs;
    if(u4_num_disp_bufs > BUF_MGR_MAX_CNT)
        u4_num_disp_bufs = BUF_MGR_MAX_CNT;

    ps_dec_state_multi_core = (dec_state_multi_core_t *) (ps_dechdl->pv_codec_handle);
    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];

    if(ps_dec_state->u4_share_disp_buf)
    {
        pic_buf_t *ps_pic_buf;
        ps_pic_buf = (pic_buf_t *)ps_dec_state->pv_pic_buf_base;
        for(i = 0; i < u4_num_disp_bufs; i++)
        {

            ps_pic_buf->pu1_y = dec_disp_ip->s_disp_buffer[i].pu1_bufs[0];
            if(IV_YUV_420P == ps_dec_state->i4_chromaFormat)
            {
                ps_pic_buf->pu1_u = dec_disp_ip->s_disp_buffer[i].pu1_bufs[1];
                ps_pic_buf->pu1_v = dec_disp_ip->s_disp_buffer[i].pu1_bufs[2];
            }
            else
            {
                ps_pic_buf->pu1_u = ps_dec_state->pu1_chroma_ref_buf[i];
                ps_pic_buf->pu1_v = ps_dec_state->pu1_chroma_ref_buf[i] +
                        ((ps_dec_state->u2_create_max_width * ps_dec_state->u2_create_max_height) >> 2);
            }

            ps_pic_buf->i4_buf_id = i;

            ps_pic_buf->u1_used_as_ref = 0;

            ps_pic_buf->u4_ts = 0;

            impeg2_buf_mgr_add(ps_dec_state->pv_pic_buf_mg, ps_pic_buf, i);
            impeg2_buf_mgr_set_status(ps_dec_state->pv_pic_buf_mg, i, BUF_MGR_DISP);
            ps_pic_buf++;

        }
    }
    memcpy(&(ps_dec_state->as_disp_buffers[0]),
           &(dec_disp_ip->s_disp_buffer),
           u4_num_disp_bufs * sizeof(ivd_out_bufdesc_t));

    return IV_SUCCESS;

}

IV_API_CALL_STATUS_T impeg2d_api_set_num_cores(iv_obj_t *ps_dechdl,
                                               void *pv_api_ip,
                                               void *pv_api_op)
{
    impeg2d_ctl_set_num_cores_ip_t   *ps_ip;
    impeg2d_ctl_set_num_cores_op_t *ps_op;
    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;

    ps_ip  = (impeg2d_ctl_set_num_cores_ip_t *)pv_api_ip;
    ps_op =  (impeg2d_ctl_set_num_cores_op_t *)pv_api_op;

    ps_dec_state_multi_core = (dec_state_multi_core_t *) (ps_dechdl->pv_codec_handle);
    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];

    if(ps_ip->u4_num_cores > 0)
    {


        WORD32 i;
        for(i = 0; i < MAX_THREADS; i++)
            ps_dec_state_multi_core->ps_dec_state[i]->i4_num_cores = ps_ip->u4_num_cores;
    }
    else
    {
        ps_dec_state->i4_num_cores = 1;
    }
    ps_op->u4_error_code = IV_SUCCESS;

    return IV_SUCCESS;
}

IV_API_CALL_STATUS_T impeg2d_api_get_seq_info(iv_obj_t *ps_dechdl,
                                               void *pv_api_ip,
                                               void *pv_api_op)
{
    impeg2d_ctl_get_seq_info_ip_t *ps_ip;
    impeg2d_ctl_get_seq_info_op_t *ps_op;
    dec_state_t *ps_codec;
    dec_state_multi_core_t *ps_dec_state_multi_core;

    ps_ip  = (impeg2d_ctl_get_seq_info_ip_t *)pv_api_ip;
    ps_op =  (impeg2d_ctl_get_seq_info_op_t *)pv_api_op;

    ps_dec_state_multi_core = (dec_state_multi_core_t *) (ps_dechdl->pv_codec_handle);
    ps_codec = ps_dec_state_multi_core->ps_dec_state[0];
    UNUSED(ps_ip);
    if(ps_codec->u2_header_done == 1)
    {
        ps_op->u1_aspect_ratio_information = ps_codec->u2_aspect_ratio_info;
        ps_op->u1_frame_rate_code = ps_codec->u2_frame_rate_code;
        ps_op->u1_frame_rate_extension_n = ps_codec->u2_frame_rate_extension_n;
        ps_op->u1_frame_rate_extension_d = ps_codec->u2_frame_rate_extension_d;
        if(ps_codec->u1_seq_disp_extn_present == 1)
        {
            ps_op->u1_video_format = ps_codec->u1_video_format;
            ps_op->u1_colour_primaries = ps_codec->u1_colour_primaries;
            ps_op->u1_transfer_characteristics = ps_codec->u1_transfer_characteristics;
            ps_op->u1_matrix_coefficients = ps_codec->u1_matrix_coefficients;
            ps_op->u2_display_horizontal_size = ps_codec->u2_display_horizontal_size;
            ps_op->u2_display_vertical_size = ps_codec->u2_display_vertical_size;
        }
        else
        {
            ps_op->u1_video_format = 5;
            ps_op->u1_colour_primaries = 2;
            ps_op->u1_transfer_characteristics = 2;
            ps_op->u1_matrix_coefficients = 2;
            ps_op->u2_display_horizontal_size = ps_codec->u2_horizontal_size;
            ps_op->u2_display_vertical_size = ps_codec->u2_vertical_size;
        }
        ps_op->u4_error_code = IV_SUCCESS;
        return IV_SUCCESS;
    }
    else
    {
        ps_op->u4_error_code = IV_FAIL;
        return IV_FAIL;
    }
}

/**
*******************************************************************************
*
* @brief
*  Sets Processor type
*
* @par Description:
*  Sets Processor type
*
* @param[in] ps_codec_obj
*  Pointer to codec object at API level
*
* @param[in] pv_api_ip
*  Pointer to input argument structure
*
* @param[out] pv_api_op
*  Pointer to output argument structure
*
* @returns  Status
*
* @remarks
*
*
*******************************************************************************
*/

IV_API_CALL_STATUS_T impeg2d_set_processor(iv_obj_t *ps_codec_obj,
                            void *pv_api_ip,
                            void *pv_api_op)
{
    impeg2d_ctl_set_processor_ip_t *ps_ip;
    impeg2d_ctl_set_processor_op_t *ps_op;
    dec_state_t *ps_codec;
    dec_state_multi_core_t *ps_dec_state_multi_core;

    ps_dec_state_multi_core = (dec_state_multi_core_t *) (ps_codec_obj->pv_codec_handle);
    ps_codec = ps_dec_state_multi_core->ps_dec_state[0];

    ps_ip = (impeg2d_ctl_set_processor_ip_t *)pv_api_ip;
    ps_op = (impeg2d_ctl_set_processor_op_t *)pv_api_op;

    ps_codec->e_processor_arch = (IVD_ARCH_T)ps_ip->u4_arch;
    ps_codec->e_processor_soc = (IVD_SOC_T)ps_ip->u4_soc;

    impeg2d_init_function_ptr(ps_codec);


    ps_op->u4_error_code = 0;
    return IV_SUCCESS;
}
/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_fill_mem_rec                                     */
/*                                                                           */
/*  Description   :                                                          */
/*  Inputs        :                                                          */
/*  Globals       :                                                          */
/*  Processing    :                                                          */
/*  Outputs       :                                                          */
/*  Returns       :                                                          */
/*                                                                           */
/*  Issues        :                                                          */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         17 09 2007  Rajendra C Y          Draft                           */
/*                                                                           */
/*****************************************************************************/
void impeg2d_fill_mem_rec(impeg2d_fill_mem_rec_ip_t *ps_ip,
                  impeg2d_fill_mem_rec_op_t *ps_op)
{
    UWORD32 u4_i;

    UWORD8 u1_no_rec = 0;
    UWORD32 max_frm_width,max_frm_height,max_frm_size;
    iv_mem_rec_t *ps_mem_rec = ps_ip->s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location;
    WORD32 i4_num_threads;
    WORD32 i4_share_disp_buf, i4_chroma_format;
    WORD32 i4_chroma_size;
    UWORD32 u4_deinterlace;
    UNUSED(u4_deinterlace);
    max_frm_width = ALIGN16(ps_ip->s_ivd_fill_mem_rec_ip_t.u4_max_frm_wd);
    max_frm_height = ALIGN16(ps_ip->s_ivd_fill_mem_rec_ip_t.u4_max_frm_ht);

    max_frm_size = (max_frm_width * max_frm_height * 3) >> 1;/* 420 P */

    i4_chroma_size = max_frm_width * max_frm_height / 4;

    if(ps_ip->s_ivd_fill_mem_rec_ip_t.u4_size > offsetof(impeg2d_fill_mem_rec_ip_t, u4_share_disp_buf))
    {
#ifndef LOGO_EN
        i4_share_disp_buf = ps_ip->u4_share_disp_buf;
#else
        i4_share_disp_buf = 0;
#endif
    }
    else
    {
        i4_share_disp_buf = 0;
    }
    if(ps_ip->s_ivd_fill_mem_rec_ip_t.u4_size > offsetof(impeg2d_fill_mem_rec_ip_t, e_output_format))
    {
        i4_chroma_format = ps_ip->e_output_format;
    }
    else
    {
        i4_chroma_format = -1;
    }

    if(ps_ip->s_ivd_fill_mem_rec_ip_t.u4_size > offsetof(impeg2d_fill_mem_rec_ip_t, u4_deinterlace))
    {
        u4_deinterlace = ps_ip->u4_deinterlace;
    }
    else
    {
        u4_deinterlace = 0;
    }


    if( (i4_chroma_format != IV_YUV_420P) &&
        (i4_chroma_format != IV_YUV_420SP_UV) &&
        (i4_chroma_format != IV_YUV_420SP_VU))
    {
        i4_share_disp_buf = 0;
    }

    /* Disable deinterlacer in shared mode */
    if(i4_share_disp_buf)
    {
        u4_deinterlace = 0;
    }

    /*************************************************************************/
    /*          Fill the memory requirement XDM Handle         */
    /*************************************************************************/
    /* ! */
    ps_mem_rec->u4_mem_alignment = 128 /* 128 byte alignment*/;
    ps_mem_rec->e_mem_type      = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    ps_mem_rec->u4_mem_size     = sizeof(iv_obj_t);

    ps_mem_rec++;
    u1_no_rec++;

    {
        /*************************************************************************/
        /*        Fill the memory requirement for threads context         */
        /*************************************************************************/
        /* ! */
        ps_mem_rec->u4_mem_alignment = 128 /* 128 byte alignment*/;
        ps_mem_rec->e_mem_type      = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        ps_mem_rec->u4_mem_size     = sizeof(dec_state_multi_core_t);

        ps_mem_rec++;
        u1_no_rec++;
    }

    for(i4_num_threads = 0; i4_num_threads < MAX_THREADS; i4_num_threads++)
    {
        /*************************************************************************/
        /*          Fill the memory requirement for MPEG2 Decoder Context        */
        /*************************************************************************/
        /* ! */
        ps_mem_rec->u4_mem_alignment = 128 /* 128 byte alignment*/;
        ps_mem_rec->e_mem_type      = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        ps_mem_rec->u4_mem_size     = sizeof(dec_state_t);

        ps_mem_rec++;
        u1_no_rec++;

        /* To store thread handle */
        ps_mem_rec->u4_mem_alignment = 128 /* 128 byte alignment*/;
        ps_mem_rec->e_mem_type      = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        ps_mem_rec->u4_mem_size     = ithread_get_handle_size();

        ps_mem_rec++;
        u1_no_rec++;

        /*************************************************************************/
        /*      Fill the memory requirement for Motion Compensation Buffers      */
        /*************************************************************************/
        ps_mem_rec->u4_mem_alignment = 128 /* 128 byte alignment*/;
        ps_mem_rec->e_mem_type      = IV_EXTERNAL_CACHEABLE_SCRATCH_MEM;

        /* for mc_fw_buf.pu1_y */
        ps_mem_rec->u4_mem_size     = MB_LUMA_MEM_SIZE;

        /* for mc_fw_buf.pu1_u */
        ps_mem_rec->u4_mem_size    += MB_CHROMA_MEM_SIZE;

        /* for mc_fw_buf.pu1_v */
        ps_mem_rec->u4_mem_size    += MB_CHROMA_MEM_SIZE;

        /* for mc_bk_buf.pu1_y */
        ps_mem_rec->u4_mem_size    += MB_LUMA_MEM_SIZE;

        /* for mc_bk_buf.pu1_u */
        ps_mem_rec->u4_mem_size    += MB_CHROMA_MEM_SIZE;

        /* for mc_bk_buf.pu1_v */
        ps_mem_rec->u4_mem_size    += MB_CHROMA_MEM_SIZE;

        /* for mc_buf.pu1_y */
        ps_mem_rec->u4_mem_size    += MB_LUMA_MEM_SIZE;

        /* for mc_buf.pu1_u */
        ps_mem_rec->u4_mem_size    += MB_CHROMA_MEM_SIZE;

        /* for mc_buf.pu1_v */
        ps_mem_rec->u4_mem_size    += MB_CHROMA_MEM_SIZE;

        ps_mem_rec++;
        u1_no_rec++;


        /*************************************************************************/
        /*             Fill the memory requirement Stack Context                 */
        /*************************************************************************/
        /* ! */
        ps_mem_rec->u4_mem_alignment = 128 /* 128 byte alignment*/;
        ps_mem_rec->e_mem_type      = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        ps_mem_rec->u4_mem_size     = 392;

        ps_mem_rec++;
        u1_no_rec++;
    }



    {
        /*************************************************************************/
        /*        Fill the memory requirement for Picture Buffer Manager         */
        /*************************************************************************/
        /* ! */
        ps_mem_rec->u4_mem_alignment = 128 /* 128 byte alignment*/;
        ps_mem_rec->e_mem_type      = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        ps_mem_rec->u4_mem_size     = sizeof(buf_mgr_t) + sizeof(pic_buf_t) * BUF_MGR_MAX_CNT;

        ps_mem_rec++;
        u1_no_rec++;
    }
    /*************************************************************************/
    /*             Internal Frame Buffers                                    */
    /*************************************************************************/
/* ! */

    {
        for(u4_i = 0; u4_i < NUM_INT_FRAME_BUFFERS; u4_i++)
        {
            /* ! */
            ps_mem_rec->u4_mem_alignment = 128 /* 128 byte alignment*/;
            ps_mem_rec->e_mem_type      = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
            if(0 == i4_share_disp_buf)
                ps_mem_rec->u4_mem_size     = max_frm_size;
            else if(IV_YUV_420P != i4_chroma_format)
            {
                /* If color format is not 420P and it is shared, then allocate for chroma */
                ps_mem_rec->u4_mem_size     = i4_chroma_size * 2;
            }
            else
                ps_mem_rec->u4_mem_size     = 64;
            ps_mem_rec++;
            u1_no_rec++;
        }
    }



    {
        WORD32 i4_job_queue_size;
        WORD32 i4_num_jobs;

        /* One job per row of MBs */
        i4_num_jobs  = max_frm_height >> 4;

        /* One format convert/frame copy job per row of MBs for non-shared mode*/
        i4_num_jobs  += max_frm_height >> 4;


        i4_job_queue_size = impeg2_jobq_ctxt_size();
        i4_job_queue_size += i4_num_jobs * sizeof(job_t);
        ps_mem_rec->u4_mem_size = i4_job_queue_size;
        ps_mem_rec->u4_mem_alignment = 128;
        ps_mem_rec->e_mem_type       = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;

        ps_mem_rec++;
        u1_no_rec++;

    }

    ps_mem_rec->u4_mem_alignment = 128;
    ps_mem_rec->e_mem_type       = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    ps_mem_rec->u4_mem_size      = impeg2d_deint_ctxt_size();
    ps_mem_rec++;
    u1_no_rec++;

    ps_mem_rec->u4_mem_alignment = 128;
    ps_mem_rec->e_mem_type       = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;

    if(IV_YUV_420P != i4_chroma_format)
        ps_mem_rec->u4_mem_size  = max_frm_size;
    else
        ps_mem_rec->u4_mem_size  = 64;

    ps_mem_rec++;
    u1_no_rec++;

    ps_mem_rec->u4_mem_alignment = 128;
    ps_mem_rec->e_mem_type       = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    ps_mem_rec->u4_mem_size      = sizeof(iv_mem_rec_t) * (NUM_MEM_RECORDS);
    ps_mem_rec++;
    u1_no_rec++;
    ps_op->s_ivd_fill_mem_rec_op_t.u4_num_mem_rec_filled = u1_no_rec;
    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code = 0;
}


/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_api_get_version                                  */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :                                                          */
/*  Globals       : <Does it use any global variables?>                      */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         22 10 2008    100356         Draft                                */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_get_version(iv_obj_t *ps_dechdl,
                                             void *pv_api_ip,
                                             void *pv_api_op)
{
    char au1_version_string[512];

    impeg2d_ctl_getversioninfo_ip_t *ps_ip;
    impeg2d_ctl_getversioninfo_op_t *ps_op;

    UNUSED(ps_dechdl);

    ps_ip = (impeg2d_ctl_getversioninfo_ip_t *)pv_api_ip;
    ps_op = (impeg2d_ctl_getversioninfo_op_t *)pv_api_op;

    ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code = IV_SUCCESS;

    VERSION(au1_version_string, CODEC_NAME, CODEC_RELEASE_TYPE, CODEC_RELEASE_VER,
            CODEC_VENDOR);

    if((WORD32)ps_ip->s_ivd_ctl_getversioninfo_ip_t.u4_version_buffer_size <= 0)
    {
        ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code = IV_FAIL;
        return (IV_FAIL);
    }

    if(ps_ip->s_ivd_ctl_getversioninfo_ip_t.u4_version_buffer_size
                    >= (strlen(au1_version_string) + 1))
    {
        memcpy(ps_ip->s_ivd_ctl_getversioninfo_ip_t.pv_version_buffer,
               au1_version_string, (strlen(au1_version_string) + 1));
        ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code = IV_SUCCESS;
    }
    else
    {
        ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code = IV_FAIL;
    }

    return (IV_SUCCESS);
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_api_get_buf_info                                 */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :                                                          */
/*  Globals       : <Does it use any global variables?>                      */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         22 10 2008    100356         Draft                                */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_get_buf_info(iv_obj_t *ps_dechdl,
                                              void *pv_api_ip,
                                              void *pv_api_op)
{
    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;
    impeg2d_ctl_getbufinfo_ip_t *ps_ctl_bufinfo_ip =
                    (impeg2d_ctl_getbufinfo_ip_t *)pv_api_ip;
    impeg2d_ctl_getbufinfo_op_t *ps_ctl_bufinfo_op =
                    (impeg2d_ctl_getbufinfo_op_t *)pv_api_op;
    UWORD32 u4_i, u4_stride, u4_height;
    UNUSED(ps_ctl_bufinfo_ip);

    ps_dec_state_multi_core =
                    (dec_state_multi_core_t *)(ps_dechdl->pv_codec_handle);
    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];

    ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_num_in_bufs = 1;
    ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_num_out_bufs = 1;

    if(ps_dec_state->i4_chromaFormat == IV_YUV_420P)
    {
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_num_out_bufs =
                        MIN_OUT_BUFS_420;
    }
    else if((ps_dec_state->i4_chromaFormat == IV_YUV_420SP_UV)
                    || (ps_dec_state->i4_chromaFormat == IV_YUV_420SP_VU))
    {
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_num_out_bufs =
                        MIN_OUT_BUFS_420SP;
    }
    else if(ps_dec_state->i4_chromaFormat == IV_YUV_422ILE)
    {
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_num_out_bufs =
                        MIN_OUT_BUFS_422ILE;
    }
    else if(ps_dec_state->i4_chromaFormat == IV_RGB_565)
    {
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_num_out_bufs =
                        MIN_OUT_BUFS_RGB565;
    }
    else
    {
        //Invalid chroma format; Error code may be updated, verify in testing if needed
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_error_code =
                        IVD_INIT_DEC_COL_FMT_NOT_SUPPORTED;
        return IV_FAIL;
    }

    for(u4_i = 0; u4_i < IVD_VIDDEC_MAX_IO_BUFFERS; u4_i++)
    {
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_in_buf_size[u4_i] =
                        0;
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_out_buf_size[u4_i] =
                        0;
    }

    for(u4_i = 0;
        u4_i < ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_num_in_bufs;
        u4_i++)
    {
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_in_buf_size[u4_i] =
                        MAX_BITSTREAM_BUFFER_SIZE;
    }

    if (0 == ps_dec_state->u4_frm_buf_stride)
    {
        if (1 == ps_dec_state->u2_header_done)
        {
            u4_stride   = ps_dec_state->u2_horizontal_size;
        }
        else
        {
            u4_stride   = ps_dec_state->u2_create_max_width;
        }
    }
    else
    {
        u4_stride = ps_dec_state->u4_frm_buf_stride;
    }
    u4_height = ((ps_dec_state->u2_frame_height + 15) >> 4) << 4;

    if(ps_dec_state->i4_chromaFormat == IV_YUV_420P)
    {
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_out_buf_size[0] =
                        (u4_stride * u4_height);
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_out_buf_size[1] =
                        (u4_stride * u4_height) >> 2;
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_out_buf_size[2] =
                        (u4_stride * u4_height) >> 2;
    }
    else if((ps_dec_state->i4_chromaFormat == IV_YUV_420SP_UV)
                    || (ps_dec_state->i4_chromaFormat == IV_YUV_420SP_VU))
    {
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_out_buf_size[0] =
                        (u4_stride * u4_height);
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_out_buf_size[1] =
                        (u4_stride * u4_height) >> 1;
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_out_buf_size[2] = 0;
    }
    else if(ps_dec_state->i4_chromaFormat == IV_YUV_422ILE)
    {
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_out_buf_size[0] =
                        (u4_stride * u4_height) * 2;
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_out_buf_size[1] =
                        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_min_out_buf_size[2] =
                                        0;
    }

    /* Adding initialization for 2 uninitialized values */
    ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_num_disp_bufs = 1;
    if(ps_dec_state->u4_share_disp_buf)
        ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_num_disp_bufs =
                        NUM_INT_FRAME_BUFFERS;
    ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_size = MAX_FRM_SIZE;

    ps_ctl_bufinfo_op->s_ivd_ctl_getbufinfo_op_t.u4_error_code = IV_SUCCESS;

    return (IV_SUCCESS);
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :  impeg2d_api_set_flush_mode                              */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :                                                          */
/*  Globals       : <Does it use any global variables?>                      */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         08 06 2009    100356         RAVI                                 */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_set_flush_mode(iv_obj_t *ps_dechdl,
                                                void *pv_api_ip,
                                                void *pv_api_op)
{
    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;
    impeg2d_ctl_flush_op_t *ps_ctl_dec_op =
                    (impeg2d_ctl_flush_op_t*)pv_api_op;

    UNUSED(pv_api_ip);

    ps_dec_state_multi_core =
                    (dec_state_multi_core_t *)(ps_dechdl->pv_codec_handle);
    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];

    ps_dec_state->u1_flushfrm = 1;

    ps_ctl_dec_op->s_ivd_ctl_flush_op_t.u4_size =
                    sizeof(impeg2d_ctl_flush_op_t);
    ps_ctl_dec_op->s_ivd_ctl_flush_op_t.u4_error_code = IV_SUCCESS;

    return (IV_SUCCESS);
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :  impeg2d_api_set_default                                 */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :                                                          */
/*  Globals       : <Does it use any global variables?>                      */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         08 06 2009    100356         RAVI                                 */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_set_default(iv_obj_t *ps_dechdl,
                                             void *pv_api_ip,
                                             void *pv_api_op)
{
    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;
    impeg2d_ctl_set_config_op_t *ps_ctl_dec_op =
                    (impeg2d_ctl_set_config_op_t *)pv_api_op;

    UNUSED(pv_api_ip);

    ps_ctl_dec_op->s_ivd_ctl_set_config_op_t.u4_error_code  = IV_SUCCESS;
    ps_ctl_dec_op->s_ivd_ctl_set_config_op_t.u4_size        =
                    sizeof(impeg2d_ctl_set_config_op_t);

    ps_dec_state_multi_core =
                    (dec_state_multi_core_t *)(ps_dechdl->pv_codec_handle);
    ps_dec_state            = ps_dec_state_multi_core->ps_dec_state[0];

    ps_dec_state->u1_flushfrm   = 0;
    ps_dec_state->u2_decode_header = 1;

    if (1 == ps_dec_state->u2_header_done)
    {
        ps_dec_state->u4_frm_buf_stride = ps_dec_state->u2_frame_width;
    }

    ps_ctl_dec_op->s_ivd_ctl_set_config_op_t.u4_error_code = IV_SUCCESS;

    return (IV_SUCCESS);

}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :  impeg2d_api_reset                                       */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :                                                          */
/*  Globals       : <Does it use any global variables?>                      */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         08 06 2009    100356         RAVI                                 */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_reset(iv_obj_t *ps_dechdl,
                                       void *pv_api_ip,
                                       void *pv_api_op)
{
    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;
    UNUSED(pv_api_ip);
    impeg2d_ctl_reset_op_t *s_ctl_reset_op = (impeg2d_ctl_reset_op_t *)pv_api_op;

    WORD32 i4_num_threads;

    ps_dec_state_multi_core = (dec_state_multi_core_t *) (ps_dechdl->pv_codec_handle);
    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];

    if(ps_dec_state_multi_core != NULL)
    {
        if(ps_dec_state->aps_ref_pics[1] != NULL)
            impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg, ps_dec_state->aps_ref_pics[1]->i4_buf_id, BUF_MGR_REF);
        if(ps_dec_state->aps_ref_pics[0] != NULL)
            impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg, ps_dec_state->aps_ref_pics[0]->i4_buf_id, BUF_MGR_REF);
        while(1)
        {
            pic_buf_t *ps_disp_pic = impeg2_disp_mgr_get(&ps_dec_state->s_disp_mgr, &ps_dec_state->i4_disp_buf_id);
            if(NULL == ps_disp_pic)
                break;
            if(0 == ps_dec_state->u4_share_disp_buf)
                impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg, ps_disp_pic->i4_buf_id, BUF_MGR_DISP);

        }

        if((ps_dec_state->u4_deinterlace) && (NULL != ps_dec_state->ps_deint_pic))
        {
            impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg,
                                   ps_dec_state->ps_deint_pic->i4_buf_id,
                                   MPEG2_BUF_MGR_DEINT);
        }

        for(i4_num_threads = 0; i4_num_threads < MAX_THREADS; i4_num_threads++)
        {
            ps_dec_state = ps_dec_state_multi_core->ps_dec_state[i4_num_threads];


            /* --------------------------------------------------------------------- */
            /* Initializations */

            ps_dec_state->u2_header_done    = 0;  /* Header decoding not done */
            ps_dec_state->u4_frm_buf_stride = 0;
            ps_dec_state->u2_is_mpeg2       = 0;
            ps_dec_state->aps_ref_pics[0] = NULL;
            ps_dec_state->aps_ref_pics[1] = NULL;
            ps_dec_state->ps_deint_pic = NULL;
        }
    }
    else
    {
        s_ctl_reset_op->s_ivd_ctl_reset_op_t.u4_error_code =
                        IMPEG2D_INIT_NOT_DONE;
    }

    return(IV_SUCCESS);
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :  impeg2d_api_set_params                                  */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :                                                          */
/*  Globals       : <Does it use any global variables?>                      */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         08 06 2009    100356         RAVI                                 */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_set_params(iv_obj_t *ps_dechdl,void *pv_api_ip,void *pv_api_op)
{
    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;
    impeg2d_ctl_set_config_ip_t  *ps_ctl_dec_ip = (impeg2d_ctl_set_config_ip_t  *)pv_api_ip;
    impeg2d_ctl_set_config_op_t  *ps_ctl_dec_op = (impeg2d_ctl_set_config_op_t  *)pv_api_op;

    ps_dec_state_multi_core = (dec_state_multi_core_t *) (ps_dechdl->pv_codec_handle);
    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];

    if((ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.e_vid_dec_mode != IVD_DECODE_HEADER) && (ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.e_vid_dec_mode != IVD_DECODE_FRAME))
    {
        ps_ctl_dec_op->s_ivd_ctl_set_config_op_t.u4_error_code = IV_FAIL;
        return(IV_FAIL);
    }

    if((ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.e_frm_out_mode != IVD_DISPLAY_FRAME_OUT) && (ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.e_frm_out_mode != IVD_DECODE_FRAME_OUT))
    {
        ps_ctl_dec_op->s_ivd_ctl_set_config_op_t.u4_error_code = IV_FAIL;
        return(IV_FAIL);
    }

    if( (WORD32) ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.e_frm_skip_mode < IVD_SKIP_NONE)
    {
        ps_ctl_dec_op->s_ivd_ctl_set_config_op_t.u4_error_code = IV_FAIL;
        return(IV_FAIL);
    }

    if(ps_dec_state->u2_header_done == 1)
    {
        if(((WORD32)ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.u4_disp_wd < 0) ||
            ((ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.u4_disp_wd != 0) && (ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.u4_disp_wd < ps_dec_state->u2_frame_width)))
        {
            ps_ctl_dec_op->s_ivd_ctl_set_config_op_t.u4_error_code = IV_FAIL;
            return(IV_FAIL);
        }

    }


    ps_dec_state->u2_decode_header    = (UWORD8)ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.e_vid_dec_mode;

    if(ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.u4_disp_wd != 0)
    {
        if(ps_dec_state->u2_header_done == 1)
        {
            if (ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.u4_disp_wd > ps_dec_state->u2_frame_width)
            {
                ps_dec_state->u4_frm_buf_stride = ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.u4_disp_wd;
            }
        }
        else
        {
            ps_dec_state->u4_frm_buf_stride = ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.u4_disp_wd;
        }

    }
    else
    {

            if(ps_dec_state->u2_header_done == 1)
            {
                ps_dec_state->u4_frm_buf_stride = ps_dec_state->u2_frame_width;
            }
            else
            {
                ps_dec_state->u4_frm_buf_stride = 0;
            }
    }


        if(ps_ctl_dec_ip->s_ivd_ctl_set_config_ip_t.e_vid_dec_mode  == IVD_DECODE_FRAME)
        {
            ps_dec_state->u1_flushfrm = 0;
        }


    ps_ctl_dec_op->s_ivd_ctl_set_config_op_t.u4_error_code = IV_SUCCESS;
    return(IV_SUCCESS);

}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :  impeg2d_api_get_status                                  */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :                                                          */
/*  Globals       : <Does it use any global variables?>                      */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         08 06 2009    100356         RAVI                                 */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_get_status(iv_obj_t *ps_dechdl,
                                                  void *pv_api_ip,
                                                  void *pv_api_op)
{
    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;
    UWORD32 u4_i,u4_stride,u4_height;
    impeg2d_ctl_getstatus_ip_t *ps_ctl_dec_ip = (impeg2d_ctl_getstatus_ip_t *)pv_api_ip;
    impeg2d_ctl_getstatus_op_t *ps_ctl_dec_op = (impeg2d_ctl_getstatus_op_t *)pv_api_op;
    UNUSED(ps_ctl_dec_ip);

    ps_dec_state_multi_core = (dec_state_multi_core_t *) (ps_dechdl->pv_codec_handle);
    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];

    ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_size             = sizeof(impeg2d_ctl_getstatus_op_t);
    ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_num_disp_bufs    = 1;
    ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_pic_ht           = ps_dec_state->u2_frame_height;
    ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_pic_wd           = ps_dec_state->u2_frame_width;
    ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_frame_rate           = ps_dec_state->u2_framePeriod;


    if(ps_dec_state->u2_progressive_sequence == 1)
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.e_content_type          =   IV_PROGRESSIVE ;
    else
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.e_content_type          = IV_INTERLACED;


    ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.e_output_chroma_format  = (IV_COLOR_FORMAT_T)ps_dec_state->i4_chromaFormat;
    ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_num_in_bufs          = 1;
    ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_num_out_bufs     = 1;


    if(ps_dec_state->i4_chromaFormat == IV_YUV_420P)
    {
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_num_out_bufs     = MIN_OUT_BUFS_420;
    }
    else if(ps_dec_state->i4_chromaFormat == IV_YUV_422ILE)
    {
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_num_out_bufs     = MIN_OUT_BUFS_422ILE;
    }
    else if(ps_dec_state->i4_chromaFormat == IV_RGB_565)
    {
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_num_out_bufs = MIN_OUT_BUFS_RGB565;
    }
    else
    {
        //Invalid chroma format; Error code may be updated, verify in testing if needed
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_error_code   = IVD_INIT_DEC_COL_FMT_NOT_SUPPORTED;
        return IV_FAIL;
    }

    memset(&ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_in_buf_size[0],0,(sizeof(UWORD32)*IVD_VIDDEC_MAX_IO_BUFFERS));
    memset(&ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_out_buf_size[0],0,(sizeof(UWORD32)*IVD_VIDDEC_MAX_IO_BUFFERS));

    for(u4_i = 0; u4_i < ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_num_in_bufs; u4_i++)
    {
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_in_buf_size[u4_i] = MAX_BITSTREAM_BUFFER_SIZE;
    }

    u4_stride = ps_dec_state->u4_frm_buf_stride;
    u4_height = ((ps_dec_state->u2_frame_height + 15) >> 4) << 4;

    if(ps_dec_state->i4_chromaFormat == IV_YUV_420P)
    {
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_out_buf_size[0] = (u4_stride * u4_height);
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_out_buf_size[1] = (u4_stride * u4_height)>>2 ;
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_out_buf_size[2] = (u4_stride * u4_height)>>2;
    }
    else if((ps_dec_state->i4_chromaFormat == IV_YUV_420SP_UV) || (ps_dec_state->i4_chromaFormat == IV_YUV_420SP_VU))
    {
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_out_buf_size[0] = (u4_stride * u4_height);
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_out_buf_size[1] = (u4_stride * u4_height)>>1 ;
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_out_buf_size[2] = 0;
    }
    else if(ps_dec_state->i4_chromaFormat == IV_YUV_422ILE)
    {
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_out_buf_size[0] = (u4_stride * u4_height)*2;
        ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_out_buf_size[1] = ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_min_out_buf_size[2] = 0;
    }

    ps_ctl_dec_op->s_ivd_ctl_getstatus_op_t.u4_error_code = IV_SUCCESS;

    return(IV_SUCCESS);

}

/**
*******************************************************************************
*
* @brief
*  Gets frame dimensions/offsets
*
* @par Description:
*  Gets frame buffer chararacteristics such a x & y offsets  display and
* buffer dimensions
*
* @param[in] ps_codec_obj
*  Pointer to codec object at API level
*
* @param[in] pv_api_ip
*  Pointer to input argument structure
*
* @param[out] pv_api_op
*  Pointer to output argument structure
*
* @returns  Status
*
* @remarks
*
*
*******************************************************************************
*/
IV_API_CALL_STATUS_T impeg2d_get_frame_dimensions(iv_obj_t *ps_codec_obj,
                                   void *pv_api_ip,
                                   void *pv_api_op)
{
    impeg2d_ctl_get_frame_dimensions_ip_t *ps_ip;
    impeg2d_ctl_get_frame_dimensions_op_t *ps_op;
    WORD32 disp_wd, disp_ht, buffer_wd, buffer_ht, x_offset, y_offset;
    dec_state_t *ps_codec;
    dec_state_multi_core_t *ps_dec_state_multi_core;

    ps_dec_state_multi_core = (dec_state_multi_core_t *) (ps_codec_obj->pv_codec_handle);
    ps_codec = ps_dec_state_multi_core->ps_dec_state[0];


    ps_ip = (impeg2d_ctl_get_frame_dimensions_ip_t *)pv_api_ip;
    ps_op = (impeg2d_ctl_get_frame_dimensions_op_t *)pv_api_op;
    UNUSED(ps_ip);
    if(ps_codec->u2_header_done)
    {
        disp_wd = ps_codec->u2_horizontal_size;
        disp_ht = ps_codec->u2_vertical_size;

        if(0 == ps_codec->u4_share_disp_buf)
        {
            buffer_wd = disp_wd;
            buffer_ht = disp_ht;
        }
        else
        {
            buffer_wd = ps_codec->u2_frame_width;
            buffer_ht = ps_codec->u2_frame_height;
        }
    }
    else
    {

        disp_wd = ps_codec->u2_create_max_width;
        disp_ht = ps_codec->u2_create_max_height;

        if(0 == ps_codec->u4_share_disp_buf)
        {
            buffer_wd = disp_wd;
            buffer_ht = disp_ht;
        }
        else
        {
            buffer_wd = ALIGN16(disp_wd);
            buffer_ht = ALIGN16(disp_ht);

        }
    }
    if(ps_codec->u2_frame_width > buffer_wd)
        buffer_wd = ps_codec->u2_frame_width;

    x_offset = 0;
    y_offset = 0;


    ps_op->u4_disp_wd[0] = disp_wd;
    ps_op->u4_disp_ht[0] = disp_ht;
    ps_op->u4_buffer_wd[0] = buffer_wd;
    ps_op->u4_buffer_ht[0] = buffer_ht;
    ps_op->u4_x_offset[0] = x_offset;
    ps_op->u4_y_offset[0] = y_offset;

    ps_op->u4_disp_wd[1] = ps_op->u4_disp_wd[2] = ((ps_op->u4_disp_wd[0] + 1)
                    >> 1);
    ps_op->u4_disp_ht[1] = ps_op->u4_disp_ht[2] = ((ps_op->u4_disp_ht[0] + 1)
                    >> 1);
    ps_op->u4_buffer_wd[1] = ps_op->u4_buffer_wd[2] = (ps_op->u4_buffer_wd[0]
                    >> 1);
    ps_op->u4_buffer_ht[1] = ps_op->u4_buffer_ht[2] = (ps_op->u4_buffer_ht[0]
                    >> 1);
    ps_op->u4_x_offset[1] = ps_op->u4_x_offset[2] = (ps_op->u4_x_offset[0]
                    >> 1);
    ps_op->u4_y_offset[1] = ps_op->u4_y_offset[2] = (ps_op->u4_y_offset[0]
                    >> 1);

    if((ps_codec->i4_chromaFormat == IV_YUV_420SP_UV)
                    || (ps_codec->i4_chromaFormat == IV_YUV_420SP_VU))
    {
        ps_op->u4_disp_wd[2] = 0;
        ps_op->u4_disp_ht[2] = 0;
        ps_op->u4_buffer_wd[2] = 0;
        ps_op->u4_buffer_ht[2] = 0;
        ps_op->u4_x_offset[2] = 0;
        ps_op->u4_y_offset[2] = 0;

        ps_op->u4_disp_wd[1] <<= 1;
        ps_op->u4_buffer_wd[1] <<= 1;
        ps_op->u4_x_offset[1] <<= 1;
    }

    return IV_SUCCESS;

}

IV_API_CALL_STATUS_T impeg2d_api_function (iv_obj_t *ps_dechdl, void *pv_api_ip,void *pv_api_op)
{
    WORD32 i4_cmd;
    IV_API_CALL_STATUS_T u4_error_code;
    UWORD32 *pu4_api_ip;

    u4_error_code = impeg2d_api_check_struct_sanity(ps_dechdl,pv_api_ip,pv_api_op);
    if(IV_SUCCESS != u4_error_code)
    {
        return u4_error_code;
    }


    pu4_api_ip  = (UWORD32 *)pv_api_ip;
    i4_cmd = *(pu4_api_ip + 1);

    switch(i4_cmd)
    {

    case IV_CMD_GET_NUM_MEM_REC:
        u4_error_code = impeg2d_api_num_mem_rec((void *)pv_api_ip,(void *)pv_api_op);
        break;

    case IV_CMD_FILL_NUM_MEM_REC:
        u4_error_code = impeg2d_api_fill_mem_rec((void *)pv_api_ip,(void *)pv_api_op);
        break;

    case IV_CMD_INIT:
        u4_error_code = impeg2d_api_init(ps_dechdl,(void *)pv_api_ip,(void *)pv_api_op);
        break;

    case IVD_CMD_SET_DISPLAY_FRAME:
        u4_error_code = impeg2d_api_set_display_frame(ps_dechdl,(void *)pv_api_ip,(void *)pv_api_op);
        break;

    case IVD_CMD_REL_DISPLAY_FRAME:
        u4_error_code = impeg2d_api_rel_display_frame(ps_dechdl,(void *)pv_api_ip,(void *)pv_api_op);
        break;

    case IVD_CMD_VIDEO_DECODE:
        u4_error_code = impeg2d_api_entity(ps_dechdl, (void *)pv_api_ip,(void *)pv_api_op);
        break;

    case IV_CMD_RETRIEVE_MEMREC:
        u4_error_code = impeg2d_api_retrieve_mem_rec(ps_dechdl,(void *)pv_api_ip,(void *)pv_api_op);
        break;

    case IVD_CMD_VIDEO_CTL:
        u4_error_code = impeg2d_api_ctl(ps_dechdl,(void *)pv_api_ip,(void *)pv_api_op);
        break;

    default:
            break;
    }

    return(u4_error_code);

}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_api_num_mem_rec                                  */
/*                                                                           */
/*  Description   : The function get the number mem records library needs    */
/*  Inputs        : Error message                                            */
/*  Globals       : None                                                     */
/*  Processing    : Just prints error message to console                     */
/*  Outputs       : Error mesage to the console                              */
/*  Returns       : None                                                     */
/*                                                                           */
/*  Issues        : <List any issues or problems with this function>         */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         23 09 2010   Hamsalekha          Creation                             */
/*                                                                           */
/*****************************************************************************/


IV_API_CALL_STATUS_T impeg2d_api_num_mem_rec(void *pv_api_ip,void *pv_api_op)
{
    /* To Query No of Memory Records */
    impeg2d_num_mem_rec_ip_t *ps_query_mem_rec_ip;
    impeg2d_num_mem_rec_op_t *ps_query_mem_rec_op;

    ps_query_mem_rec_ip = (impeg2d_num_mem_rec_ip_t *)pv_api_ip;
    ps_query_mem_rec_op = (impeg2d_num_mem_rec_op_t *)pv_api_op;

    UNUSED(ps_query_mem_rec_ip);
    ps_query_mem_rec_op->s_ivd_num_mem_rec_op_t.u4_size = sizeof(impeg2d_num_mem_rec_op_t);

    ps_query_mem_rec_op->s_ivd_num_mem_rec_op_t.u4_num_mem_rec  = (UWORD32)NUM_MEM_RECORDS;

    ps_query_mem_rec_op->s_ivd_num_mem_rec_op_t.u4_error_code = IV_SUCCESS;


    return(IV_SUCCESS);

}


/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_api_fill_mem_rec                                 */
/*                                                                           */
/*  Description   : Thsi functions fills details of each mem record lib needs*/
/*  Inputs        : Error message                                            */
/*  Globals       : None                                                     */
/*  Processing    : Just prints error message to console                     */
/*  Outputs       : Error mesage to the console                              */
/*  Returns       : None                                                     */
/*                                                                           */
/*  Issues        : <List any issues or problems with this function>         */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         23 09 2010   Hamsalekha          Creation                         */
/*                                                                           */
/*****************************************************************************/


IV_API_CALL_STATUS_T impeg2d_api_fill_mem_rec(void *pv_api_ip,void *pv_api_op)
{

    impeg2d_fill_mem_rec_ip_t *ps_mem_q_ip;
    impeg2d_fill_mem_rec_op_t *ps_mem_q_op;


    ps_mem_q_ip = pv_api_ip;
    ps_mem_q_op = pv_api_op;


    impeg2d_fill_mem_rec((impeg2d_fill_mem_rec_ip_t *)ps_mem_q_ip,
                           (impeg2d_fill_mem_rec_op_t *)ps_mem_q_op);


    return(IV_SUCCESS);

}



/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_api_init                                         */
/*                                                                           */
/*  Description   :                                                          */
/*  Inputs        :                                                          */
/*  Globals       :                                                          */
/*  Processing    :                                                          */
/*  Outputs       :                                                          */
/*  Returns       :                                                          */
/*                                                                           */
/*  Issues        :                                                          */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         17 09 2007  Rajendra C Y          Draft                           */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_init(iv_obj_t *ps_dechdl,
                                      void *ps_ip,
                                      void *ps_op)
{
    UWORD32 i;

    void *pv;
    UWORD32 u4_size;

    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;
    UWORD32 u4_num_mem_rec;
    iv_mem_rec_t *ps_mem_rec ;
    iv_mem_rec_t *ps_frm_buf;
    iv_obj_t *ps_dec_handle;
    WORD32 i4_max_wd, i4_max_ht;

    impeg2d_init_ip_t *ps_dec_init_ip;
    impeg2d_init_op_t *ps_dec_init_op;
    WORD32 i4_num_threads;
    UWORD32 u4_share_disp_buf, u4_chroma_format;
    UWORD32 u4_deinterlace;

    ps_dec_init_ip = (impeg2d_init_ip_t *)ps_ip;
    ps_dec_init_op = (impeg2d_init_op_t *)ps_op;

    i4_max_wd = ALIGN16(ps_dec_init_ip->s_ivd_init_ip_t.u4_frm_max_wd);
    i4_max_ht = ALIGN16(ps_dec_init_ip->s_ivd_init_ip_t.u4_frm_max_ht);

    if(ps_dec_init_ip->s_ivd_init_ip_t.u4_size > offsetof(impeg2d_init_ip_t, u4_share_disp_buf))
    {
#ifndef LOGO_EN
        u4_share_disp_buf = ps_dec_init_ip->u4_share_disp_buf;
#else
        u4_share_disp_buf = 0;
#endif
    }
    else
    {
        u4_share_disp_buf = 0;
    }

    u4_chroma_format = ps_dec_init_ip->s_ivd_init_ip_t.e_output_format;

    if(ps_dec_init_ip->s_ivd_init_ip_t.u4_size > offsetof(impeg2d_init_ip_t, u4_deinterlace))
    {
        u4_deinterlace = ps_dec_init_ip->u4_deinterlace;
    }
    else
    {
        u4_deinterlace = 0;
    }

    if( (u4_chroma_format != IV_YUV_420P) &&
        (u4_chroma_format != IV_YUV_420SP_UV) &&
        (u4_chroma_format != IV_YUV_420SP_VU))
    {
        u4_share_disp_buf = 0;
    }

    /* Disable deinterlacer in shared mode */
    if(u4_share_disp_buf)
    {
        u4_deinterlace = 0;
    }

    ps_mem_rec = ps_dec_init_ip->s_ivd_init_ip_t.pv_mem_rec_location;
    ps_mem_rec ++;


    ps_dec_init_op->s_ivd_init_op_t.u4_size = sizeof(impeg2d_init_op_t);


    /* Except memTab[0], all other memTabs are initialized to zero */
    for(i = 1; i < ps_dec_init_ip->s_ivd_init_ip_t.u4_num_mem_rec; i++)
    {
        memset(ps_mem_rec->pv_base,0,ps_mem_rec->u4_mem_size);
        ps_mem_rec++;
    }

    /* Reinitializing memTab[0] memory base address */
    ps_mem_rec     = ps_dec_init_ip->s_ivd_init_ip_t.pv_mem_rec_location;


    /* memTab[0] is for codec Handle,redundant currently not being used */
    ps_dec_handle  = ps_mem_rec->pv_base;
    u4_num_mem_rec = 1;
    ps_mem_rec++;





    /* decoder handle */
    ps_dec_state_multi_core = ps_mem_rec->pv_base;
    u4_num_mem_rec++;
    ps_mem_rec++;


    {
        ps_dec_handle->pv_codec_handle = (void *)ps_dec_state_multi_core; /* Initializing codec context */

        ps_dechdl->pv_codec_handle =  (void *)ps_dec_state_multi_core;
        ps_dechdl->pv_fxns = (void *)impeg2d_api_function;
    }


    for(i4_num_threads = 0; i4_num_threads < MAX_THREADS; i4_num_threads++)
    {
    /*************************************************************************/
    /*                      For MPEG2 Decoder Context                        */
    /*************************************************************************/
    ps_dec_state = ps_mem_rec->pv_base;

    ps_dec_state_multi_core->ps_dec_state[i4_num_threads] = ps_dec_state;

    ps_dec_state->ps_dec_state_multi_core = ps_dec_state_multi_core;

    ps_dec_state->i4_num_cores = 1;
    /* @ */  /* Used for storing MemRecords */
     u4_num_mem_rec++;
     ps_mem_rec++;

     /* Thread handle */
     ps_dec_state->pv_codec_thread_handle = ps_mem_rec->pv_base;
     u4_num_mem_rec++;
     ps_mem_rec++;

    /*************************************************************************/
    /*                      For Motion Compensation Buffers                  */
    /*************************************************************************/
    pv = ps_mem_rec->pv_base;

    /* for mc_fw_buf.pu1_y */

    ps_dec_state->s_mc_fw_buf.pu1_y = pv;
    pv = (void *)((UWORD8 *)pv + MB_LUMA_MEM_SIZE);

    u4_size = sizeof(UWORD8) * MB_LUMA_MEM_SIZE;
    /* for mc_fw_buf.pu1_u */

    ps_dec_state->s_mc_fw_buf.pu1_u = pv;
    pv = (void *)((UWORD8 *)pv + MB_CHROMA_MEM_SIZE);

    u4_size += sizeof(UWORD8) * MB_CHROMA_MEM_SIZE;

    /* for mc_fw_buf.pu1_v */

    ps_dec_state->s_mc_fw_buf.pu1_v = pv;
    pv = (void *)((UWORD8 *)pv + MB_CHROMA_MEM_SIZE);

    u4_size += sizeof(UWORD8) * MB_CHROMA_MEM_SIZE;

    /* for mc_bk_buf.pu1_y */

    ps_dec_state->s_mc_bk_buf.pu1_y = pv;
    pv = (void *)((UWORD8 *)pv + MB_LUMA_MEM_SIZE);

    u4_size += sizeof(UWORD8) * MB_LUMA_MEM_SIZE;

    /* for mc_bk_buf.pu1_u */

    ps_dec_state->s_mc_bk_buf.pu1_u = pv;
    pv = (void *)((UWORD8 *)pv + MB_CHROMA_MEM_SIZE);

    u4_size += sizeof(UWORD8) * MB_CHROMA_MEM_SIZE;

    /* for mc_bk_buf.pu1_v */

    ps_dec_state->s_mc_bk_buf.pu1_v = pv;
    pv = (void *)((UWORD8 *)pv + MB_CHROMA_MEM_SIZE);

    u4_size += sizeof(UWORD8) * MB_CHROMA_MEM_SIZE;

    /* for mc_buf.pu1_y */

    ps_dec_state->s_mc_buf.pu1_y = pv;
    pv = (void *)((UWORD8 *)pv + MB_LUMA_MEM_SIZE);

    u4_size += sizeof(UWORD8) * MB_LUMA_MEM_SIZE;

    /* for mc_buf.pu1_u */

    ps_dec_state->s_mc_buf.pu1_u = pv;
    pv = (void *)((UWORD8 *)pv + MB_CHROMA_MEM_SIZE);

    u4_size += sizeof(UWORD8) * MB_CHROMA_MEM_SIZE;

    /* for mc_buf.pu1_v */

    ps_dec_state->s_mc_buf.pu1_v = pv;

    u4_size += sizeof(UWORD8) * MB_CHROMA_MEM_SIZE;

    u4_num_mem_rec++;
    ps_mem_rec++;



    ps_dec_state->pv_pic_buf_mg = 0;

    /*************************************************************************/
    /*        For saving stack context to support global error handling      */
    /*************************************************************************/
    ps_dec_state->pv_stack_cntxt = ps_mem_rec->pv_base;
    u4_num_mem_rec++;
    ps_mem_rec++;

    }





    /*************************************************************************/
    /*                          For Picture Buffer Manager                   */
    /*************************************************************************/
    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];

    ps_dec_state->pv_pic_buf_mg = ps_mem_rec->pv_base;
    ps_dec_state->pv_pic_buf_base = (UWORD8 *)ps_mem_rec->pv_base + sizeof(buf_mgr_t);

    u4_num_mem_rec++;
    ps_mem_rec++;



    for(i4_num_threads = 0; i4_num_threads < MAX_THREADS; i4_num_threads++)
    {

        ps_dec_state = ps_dec_state_multi_core->ps_dec_state[i4_num_threads];


        /* --------------------------------------------------------------------- */
        /* Initializations */

        ps_dec_state->u2_header_done  = 0;  /* Header decoding not done */


        {
            UWORD32 u4_max_frm_width,u4_max_frm_height;

            u4_max_frm_width = ALIGN16(ps_dec_init_ip->s_ivd_init_ip_t.u4_frm_max_wd);
            u4_max_frm_height = ALIGN16(ps_dec_init_ip->s_ivd_init_ip_t.u4_frm_max_ht);

            ps_dec_state->u2_create_max_width   = u4_max_frm_width;
            ps_dec_state->u2_create_max_height  = u4_max_frm_height;

            ps_dec_state->i4_chromaFormat = ps_dec_init_ip->s_ivd_init_ip_t.e_output_format;
            ps_dec_state->u4_frm_buf_stride  = 0 ;
            ps_dec_state->u2_frame_width  = u4_max_frm_width;
            ps_dec_state->u2_picture_width  = u4_max_frm_width;
            ps_dec_state->u2_horizontal_size  = u4_max_frm_width;

            ps_dec_state->u2_frame_height = u4_max_frm_height;
            ps_dec_state->u2_vertical_size = u4_max_frm_height;
            ps_dec_state->u4_share_disp_buf = u4_share_disp_buf;
            ps_dec_state->u4_deinterlace = u4_deinterlace;
            ps_dec_state->ps_deint_pic = NULL;
        }
    }


    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];

    if((ps_dec_state->i4_chromaFormat  == IV_YUV_422ILE)
        &&((ps_dec_state->u2_vertical_size & 0x1) != 0))
    {
        //printf("Error! Height should be multiple of 2 if Chroma format is 422ILE\n");
        ps_dec_init_op->s_ivd_init_op_t.u4_error_code = IMPEG2D_INIT_CHROMA_FORMAT_HEIGHT_ERROR;
        return(IV_FAIL);


    }

    /* --------------------------------------------------------------------- */


/* ! */
    // picture buffer manager initialization will be done only for first thread
    impeg2_disp_mgr_init(&ps_dec_state->s_disp_mgr);
    impeg2_buf_mgr_init((buf_mgr_t *)ps_dec_state->pv_pic_buf_mg);

    /*************************************************************************/
    /*             Internal Frame Buffers                                    */
    /*************************************************************************/


    /* Set first frame to grey */
    {
        ps_frm_buf = ps_mem_rec;
        memset(ps_frm_buf->pv_base, 128, ps_frm_buf->u4_mem_size);
        ps_frm_buf++;
    }

    if(0 == ps_dec_state->u4_share_disp_buf)
    {
        pic_buf_t *ps_pic_buf;
        ps_pic_buf = (pic_buf_t *)ps_dec_state->pv_pic_buf_base;
        for(i = 0; i < NUM_INT_FRAME_BUFFERS; i++)
        {
            UWORD8 *pu1_buf;
            pu1_buf = ps_mem_rec->pv_base;

            ps_pic_buf->pu1_y = pu1_buf;
            pu1_buf += i4_max_ht * i4_max_wd;

            ps_pic_buf->pu1_u = pu1_buf;
            pu1_buf += i4_max_ht * i4_max_wd >> 2;

            ps_pic_buf->pu1_v = pu1_buf;
            pu1_buf += i4_max_ht * i4_max_wd >> 2;

            ps_pic_buf->i4_buf_id = i;

            ps_pic_buf->u1_used_as_ref = 0;

            ps_pic_buf->u4_ts = 0;

            impeg2_buf_mgr_add(ps_dec_state->pv_pic_buf_mg, ps_pic_buf, i);
            ps_mem_rec++;
            ps_pic_buf++;
        }
        u4_num_mem_rec += NUM_INT_FRAME_BUFFERS;
    }
    else if (ps_dec_state->i4_chromaFormat  != IV_YUV_420P)
    {
        for(i = 0; i < NUM_INT_FRAME_BUFFERS; i++)
        {
            ps_dec_state->pu1_chroma_ref_buf[i] = ps_mem_rec->pv_base;
            ps_mem_rec++;
        }

        u4_num_mem_rec += NUM_INT_FRAME_BUFFERS;
    }
    else
    {
        ps_mem_rec+=NUM_INT_FRAME_BUFFERS;
        u4_num_mem_rec += NUM_INT_FRAME_BUFFERS;
    }


    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];


    ps_dec_state->pv_jobq_buf = ps_mem_rec->pv_base;
    ps_dec_state->i4_jobq_buf_size = ps_mem_rec->u4_mem_size;
    ps_mem_rec++;

    if(u4_num_mem_rec > ps_dec_init_ip->s_ivd_init_ip_t.u4_num_mem_rec)
    {
        ps_dec_init_op->s_ivd_init_op_t.u4_error_code = IMPEG2D_INIT_NUM_MEM_REC_NOT_SUFFICIENT;
        return(IV_FAIL);

    }

    ps_dec_state->u1_flushfrm = 0;
    ps_dec_state->u1_flushcnt = 0;
    ps_dec_state->pv_jobq = impeg2_jobq_init(ps_dec_state->pv_jobq_buf, ps_dec_state->i4_jobq_buf_size);


    ps_dec_state->pv_deinterlacer_ctxt = ps_mem_rec->pv_base;
    ps_mem_rec++;

    ps_dec_state->pu1_deint_fmt_buf = ps_mem_rec->pv_base;
    ps_mem_rec++;


    /*************************************************************************/
    /*        Last MemTab is used for storing TabRecords                     */
    /*************************************************************************/
    ps_dec_state->pv_memTab     = (void *)ps_mem_rec->pv_base;
    memcpy(ps_mem_rec->pv_base,ps_dec_init_ip->s_ivd_init_ip_t.pv_mem_rec_location, ps_mem_rec->u4_mem_size);
    /* Updating in Decoder Context with memRecords  */
    u4_num_mem_rec++;
    ps_mem_rec++;
    ps_dec_state->u4_num_mem_records = u4_num_mem_rec;


    ps_dec_state->u4_num_frames_decoded    = 0;
    ps_dec_state->aps_ref_pics[0] = NULL;
    ps_dec_state->aps_ref_pics[1] = NULL;

    ps_dec_init_op->s_ivd_init_op_t.u4_error_code = IV_SUCCESS;

    impeg2d_init_arch(ps_dec_state);

    impeg2d_init_function_ptr(ps_dec_state);

    return(IV_SUCCESS);
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_api_retrieve_mem_rec                             */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :                                                          */
/*  Globals       : <Does it use any global variables?>                      */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         22 10 2008    100356         Draft                                */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_retrieve_mem_rec(iv_obj_t *ps_dechdl,
                                            void *pv_api_ip,
                                            void *pv_api_op)
{
    UWORD32 u4_i;
    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;
    iv_mem_rec_t *ps_mem_rec;
    iv_mem_rec_t *ps_temp_rec;



    impeg2d_retrieve_mem_rec_ip_t *ps_retr_mem_rec_ip;
    impeg2d_retrieve_mem_rec_op_t *ps_retr_mem_rec_op;

    ps_retr_mem_rec_ip  = (impeg2d_retrieve_mem_rec_ip_t *)pv_api_ip;
    ps_retr_mem_rec_op  = (impeg2d_retrieve_mem_rec_op_t *)pv_api_op;

    ps_mem_rec          = ps_retr_mem_rec_ip->s_ivd_retrieve_mem_rec_ip_t.pv_mem_rec_location;
    ps_dec_state_multi_core = (dec_state_multi_core_t *) (ps_dechdl->pv_codec_handle);
    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];
    ps_temp_rec        = ps_dec_state->pv_memTab;

    for(u4_i = 0; u4_i < (ps_dec_state->u4_num_mem_records);u4_i++)
    {
        ps_mem_rec[u4_i].u4_mem_size        = ps_temp_rec[u4_i].u4_mem_size;
        ps_mem_rec[u4_i].u4_mem_alignment   = ps_temp_rec[u4_i].u4_mem_alignment;
        ps_mem_rec[u4_i].e_mem_type         = ps_temp_rec[u4_i].e_mem_type;
        ps_mem_rec[u4_i].pv_base            = ps_temp_rec[u4_i].pv_base;
    }

    ps_retr_mem_rec_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code       = IV_SUCCESS;
    ps_retr_mem_rec_op->s_ivd_retrieve_mem_rec_op_t.u4_num_mem_rec_filled   = ps_dec_state->u4_num_mem_records;

    impeg2_jobq_deinit(ps_dec_state->pv_jobq);
    IMPEG2D_PRINT_STATISTICS();


    return(IV_SUCCESS);

}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :   impeg2d_api_ctl                                        */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :                                                          */
/*  Globals       : <Does it use any global variables?>                      */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         22 10 2008    100356         Draft                                */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_ctl(iv_obj_t *ps_dechdl,
                                     void *pv_api_ip,
                                     void *pv_api_op)
{
    WORD32 i4_sub_cmd;
    UWORD32 *pu4_api_ip;
    IV_API_CALL_STATUS_T u4_error_code;

    pu4_api_ip = (UWORD32 *)pv_api_ip;
    i4_sub_cmd = *(pu4_api_ip + 2);

    switch(i4_sub_cmd)
    {
        case IVD_CMD_CTL_GETPARAMS:
            u4_error_code = impeg2d_api_get_status(ps_dechdl, (void *)pv_api_ip,
                                                   (void *)pv_api_op);
            break;

        case IVD_CMD_CTL_SETPARAMS:
            u4_error_code = impeg2d_api_set_params(ps_dechdl, (void *)pv_api_ip,
                                                   (void *)pv_api_op);
            break;

        case IVD_CMD_CTL_RESET:
            u4_error_code = impeg2d_api_reset(ps_dechdl, (void *)pv_api_ip,
                                              (void *)pv_api_op);
            break;

        case IVD_CMD_CTL_SETDEFAULT:
            u4_error_code = impeg2d_api_set_default(ps_dechdl,
                                                          (void *)pv_api_ip,
                                                          (void *)pv_api_op);
            break;

        case IVD_CMD_CTL_FLUSH:
            u4_error_code = impeg2d_api_set_flush_mode(ps_dechdl,
                                                             (void *)pv_api_ip,
                                                             (void *)pv_api_op);
            break;

        case IVD_CMD_CTL_GETBUFINFO:
            u4_error_code = impeg2d_api_get_buf_info(ps_dechdl,
                                                           (void *)pv_api_ip,
                                                           (void *)pv_api_op);
            break;

        case IVD_CMD_CTL_GETVERSION:
            u4_error_code = impeg2d_api_get_version(ps_dechdl, (void *)pv_api_ip,
                                                      (void *)pv_api_op);
            break;

        case IMPEG2D_CMD_CTL_SET_NUM_CORES:
            u4_error_code = impeg2d_api_set_num_cores(ps_dechdl,
                                                         (void *)pv_api_ip,
                                                         (void *)pv_api_op);
            break;

        case IMPEG2D_CMD_CTL_GET_BUFFER_DIMENSIONS:
            u4_error_code = impeg2d_get_frame_dimensions(ps_dechdl,
                                                       (void *)pv_api_ip,
                                                       (void *)pv_api_op);
            break;

        case IMPEG2D_CMD_CTL_GET_SEQ_INFO:
            u4_error_code = impeg2d_api_get_seq_info(ps_dechdl,
                                                         (void *)pv_api_ip,
                                                         (void *)pv_api_op);
            break;

        case IMPEG2D_CMD_CTL_SET_PROCESSOR:
            u4_error_code = impeg2d_set_processor(ps_dechdl, (void *)pv_api_ip,
                                                (void *)pv_api_op);
            break;

        default:
            u4_error_code = IV_FAIL;
            break;
    }

    return (u4_error_code);

}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_api_check_struct_sanity                          */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :                                                          */
/*  Globals       : <Does it use any global variables?>                      */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         22 10 2008    100356         Draft                                */
/*                                                                           */
/*****************************************************************************/
IV_API_CALL_STATUS_T impeg2d_api_check_struct_sanity(iv_obj_t *ps_handle,
                                                    void *pv_api_ip,
                                                    void *pv_api_op)
{
    WORD32  i4_cmd;
    UWORD32 *pu4_api_ip;
    UWORD32 *pu4_api_op;
    WORD32 i,j;

    if(NULL == pv_api_op)
        return(IV_FAIL);

    if(NULL == pv_api_ip)
        return(IV_FAIL);

    pu4_api_ip  = (UWORD32 *)pv_api_ip;
    pu4_api_op  = (UWORD32 *)pv_api_op;
    i4_cmd = (IVD_API_COMMAND_TYPE_T)*(pu4_api_ip + 1);

    /* error checks on handle */
    switch(i4_cmd)
    {
        case IV_CMD_GET_NUM_MEM_REC:
        case IV_CMD_FILL_NUM_MEM_REC:
            break;
        case IV_CMD_INIT:
            if(ps_handle == NULL)
            {
                *(pu4_api_op + 1) |= 1 << IVD_UNSUPPORTEDPARAM;
                *(pu4_api_op + 1) |= IVD_HANDLE_NULL;
                return IV_FAIL;
            }

            if(ps_handle->u4_size != sizeof(iv_obj_t))
            {
                *(pu4_api_op + 1) |= 1 << IVD_UNSUPPORTEDPARAM;
                *(pu4_api_op + 1) |= IVD_HANDLE_STRUCT_SIZE_INCORRECT;
                return IV_FAIL;
            }
            break;
        case IVD_CMD_GET_DISPLAY_FRAME:
        case IVD_CMD_VIDEO_DECODE:
        case IV_CMD_RETRIEVE_MEMREC:
        case IVD_CMD_SET_DISPLAY_FRAME:
        case IVD_CMD_REL_DISPLAY_FRAME:
        case IVD_CMD_VIDEO_CTL:
            {
            if(ps_handle == NULL)
            {
                *(pu4_api_op + 1) |= 1 << IVD_UNSUPPORTEDPARAM;
                *(pu4_api_op + 1) |= IVD_HANDLE_NULL;
                return IV_FAIL;
            }

            if(ps_handle->u4_size != sizeof(iv_obj_t))
            {
                *(pu4_api_op + 1) |= 1 << IVD_UNSUPPORTEDPARAM;
                *(pu4_api_op + 1) |= IVD_HANDLE_STRUCT_SIZE_INCORRECT;
                return IV_FAIL;
            }
            if(ps_handle->pv_fxns != impeg2d_api_function)
            {
                *(pu4_api_op + 1) |= 1 << IVD_UNSUPPORTEDPARAM;
                    *(pu4_api_op + 1) |= IVD_INVALID_HANDLE_NULL;
                return IV_FAIL;
            }

            if(ps_handle->pv_codec_handle == NULL)
            {
                *(pu4_api_op + 1) |= 1 << IVD_UNSUPPORTEDPARAM;
                    *(pu4_api_op + 1) |= IVD_INVALID_HANDLE_NULL;
                return IV_FAIL;
            }
            }
            break;
        default:
            *(pu4_api_op + 1) |= 1 << IVD_UNSUPPORTEDPARAM;
            *(pu4_api_op + 1) |= IVD_INVALID_API_CMD;
            return IV_FAIL;
    }

    switch(i4_cmd)
    {
        case IV_CMD_GET_NUM_MEM_REC:
            {
                impeg2d_num_mem_rec_ip_t *ps_ip = (impeg2d_num_mem_rec_ip_t *)pv_api_ip;
                impeg2d_num_mem_rec_op_t *ps_op = (impeg2d_num_mem_rec_op_t *)pv_api_op;
                ps_op->s_ivd_num_mem_rec_op_t.u4_error_code = 0;

                if(ps_ip->s_ivd_num_mem_rec_ip_t.u4_size != sizeof(impeg2d_num_mem_rec_ip_t))
                {
                    ps_op->s_ivd_num_mem_rec_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_num_mem_rec_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                if(ps_op->s_ivd_num_mem_rec_op_t.u4_size != sizeof(impeg2d_num_mem_rec_op_t))
                {
                    ps_op->s_ivd_num_mem_rec_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_num_mem_rec_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }
            }
            break;
        case IV_CMD_FILL_NUM_MEM_REC:
            {
                impeg2d_fill_mem_rec_ip_t *ps_ip = (impeg2d_fill_mem_rec_ip_t *)pv_api_ip;
                impeg2d_fill_mem_rec_op_t *ps_op = (impeg2d_fill_mem_rec_op_t *)pv_api_op;
                iv_mem_rec_t                  *ps_mem_rec;

                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code = 0;

                if(ps_ip->s_ivd_fill_mem_rec_ip_t.u4_size != sizeof(impeg2d_fill_mem_rec_ip_t))
                {
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                if(ps_op->s_ivd_fill_mem_rec_op_t.u4_size != sizeof(impeg2d_fill_mem_rec_op_t))
                {
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                if(ps_ip->s_ivd_fill_mem_rec_ip_t.u4_max_frm_wd < MIN_WIDTH)
                {
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= IVD_REQUESTED_WIDTH_NOT_SUPPPORTED;
                    return(IV_FAIL);
                }

                if(ps_ip->s_ivd_fill_mem_rec_ip_t.u4_max_frm_wd > MAX_WIDTH)
                {
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= IVD_REQUESTED_WIDTH_NOT_SUPPPORTED;
                    return(IV_FAIL);
                }

                if(ps_ip->s_ivd_fill_mem_rec_ip_t.u4_max_frm_ht < MIN_HEIGHT)
                {
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= IVD_REQUESTED_HEIGHT_NOT_SUPPPORTED;
                    return(IV_FAIL);
                }

                if(ps_ip->s_ivd_fill_mem_rec_ip_t.u4_max_frm_ht > MAX_HEIGHT)
                {
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= IVD_REQUESTED_HEIGHT_NOT_SUPPPORTED;
                    return(IV_FAIL);
                }

                if(NULL == ps_ip->s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location)
                {
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= IVD_NUM_REC_NOT_SUFFICIENT;
                    return(IV_FAIL);
                }

                /* check memrecords sizes are correct */
                ps_mem_rec  = ps_ip->s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location;
                for(i=0;i<NUM_MEM_RECORDS;i++)
                {
                    if(ps_mem_rec[i].u4_size != sizeof(iv_mem_rec_t))
                    {
                        ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= IVD_MEM_REC_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                }
            }
            break;

        case IV_CMD_INIT:
            {
                impeg2d_init_ip_t *ps_ip = (impeg2d_init_ip_t *)pv_api_ip;
                impeg2d_init_op_t *ps_op = (impeg2d_init_op_t *)pv_api_op;
                iv_mem_rec_t          *ps_mem_rec;
                UWORD32 u4_tot_num_mem_recs;

                ps_op->s_ivd_init_op_t.u4_error_code = 0;

                if(ps_ip->s_ivd_init_ip_t.u4_size != sizeof(impeg2d_init_ip_t))
                {
                    ps_op->s_ivd_init_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                if(ps_op->s_ivd_init_op_t.u4_size != sizeof(impeg2d_init_op_t))
                {
                    ps_op->s_ivd_init_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                u4_tot_num_mem_recs = NUM_MEM_RECORDS;




                if(ps_ip->s_ivd_init_ip_t.u4_num_mem_rec > u4_tot_num_mem_recs)
                {
                    ps_op->s_ivd_init_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_NOT_SUFFICIENT;
                    return(IV_FAIL);
                }

                if(ps_ip->s_ivd_init_ip_t.u4_frm_max_wd < MIN_WIDTH)
                {
                    ps_op->s_ivd_init_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_WIDTH_NOT_SUPPPORTED;
                    return(IV_FAIL);
                }

                if(ps_ip->s_ivd_init_ip_t.u4_frm_max_wd > MAX_WIDTH)
                {
                    ps_op->s_ivd_init_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_WIDTH_NOT_SUPPPORTED;
                    return(IV_FAIL);
                }

                if(ps_ip->s_ivd_init_ip_t.u4_frm_max_ht < MIN_HEIGHT)
                {
                    ps_op->s_ivd_init_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_HEIGHT_NOT_SUPPPORTED;
                    return(IV_FAIL);
                }

                if(ps_ip->s_ivd_init_ip_t.u4_frm_max_ht > MAX_HEIGHT)
                {
                    ps_op->s_ivd_init_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_HEIGHT_NOT_SUPPPORTED;
                    return(IV_FAIL);
                }

                if(NULL == ps_ip->s_ivd_init_ip_t.pv_mem_rec_location)
                {
                    ps_op->s_ivd_init_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |= IVD_NUM_REC_NOT_SUFFICIENT;
                    return(IV_FAIL);
                }

                if((ps_ip->s_ivd_init_ip_t.e_output_format != IV_YUV_420P) &&
                    (ps_ip->s_ivd_init_ip_t.e_output_format != IV_YUV_422ILE)&&(ps_ip->s_ivd_init_ip_t.e_output_format != IV_YUV_420SP_UV)&&(ps_ip->s_ivd_init_ip_t.e_output_format != IV_YUV_420SP_VU))
                {
                    ps_op->s_ivd_init_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_COL_FMT_NOT_SUPPORTED;
                    return(IV_FAIL);
                }

                /* verify number of mem records */
                if(ps_ip->s_ivd_init_ip_t.u4_num_mem_rec < NUM_MEM_RECORDS)
                {
                    ps_op->s_ivd_init_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_MEM_REC_NOT_SUFFICIENT;
                    return IV_FAIL;
                }

                ps_mem_rec  = ps_ip->s_ivd_init_ip_t.pv_mem_rec_location;
                /* verify wether first memrecord is handle or not */
                /*
                if(ps_mem_rec->pv_base != ps_handle)
                {
                     // indicate the incorrect handle error
                    ps_op->s_ivd_init_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INVALID_HANDLE;
                    return IV_FAIL;
                }
*/
                /* check memrecords sizes are correct */
                for(i=0;i < (WORD32)ps_ip->s_ivd_init_ip_t.u4_num_mem_rec ; i++)
                {
                    if(ps_mem_rec[i].u4_size != sizeof(iv_mem_rec_t))
                    {
                        ps_op->s_ivd_init_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_init_op_t.u4_error_code |= IVD_MEM_REC_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                }

                /* verify memtabs for overlapping regions */
                {
                    UWORD8 *pau1_start[NUM_MEM_RECORDS];
                    UWORD8 *pau1_end[NUM_MEM_RECORDS];


                    pau1_start[0] = (UWORD8 *)(ps_mem_rec[0].pv_base);
                    pau1_end[0]   = (UWORD8 *)(ps_mem_rec[0].pv_base) + ps_mem_rec[0].u4_mem_size - 1;
                    for(i = 1; i < (WORD32)ps_ip->s_ivd_init_ip_t.u4_num_mem_rec; i++)
                    {
                        /* This array is populated to check memtab overlapp */
                        pau1_start[i] = (UWORD8 *)(ps_mem_rec[i].pv_base);
                        pau1_end[i]   = (UWORD8 *)(ps_mem_rec[i].pv_base) + ps_mem_rec[i].u4_mem_size - 1;

                        for(j = 0; j < i; j++)
                        {
                            if((pau1_start[i] >= pau1_start[j]) && (pau1_start[i] <= pau1_end[j]))
                            {
                                ps_op->s_ivd_init_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_MEM_REC_OVERLAP_ERR;
                                return IV_FAIL;
                            }

                            if((pau1_end[i] >= pau1_start[j]) && (pau1_end[i] <= pau1_end[j]))
                            {
                                ps_op->s_ivd_init_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_MEM_REC_OVERLAP_ERR;
                                return IV_FAIL;
                            }

                            if((pau1_start[i] < pau1_start[j]) && (pau1_end[i] > pau1_end[j]))
                            {
                                ps_op->s_ivd_init_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_MEM_REC_OVERLAP_ERR;
                                return IV_FAIL;
                            }
                        }
                    }
                }




                {
                    iv_mem_rec_t    as_mem_rec_ittiam_api[NUM_MEM_RECORDS];

                    impeg2d_fill_mem_rec_ip_t s_fill_mem_rec_ip;
                    impeg2d_fill_mem_rec_op_t s_fill_mem_rec_op;
                    IV_API_CALL_STATUS_T e_status;
                    WORD32 i4_num_memrec;
                    {

                        iv_num_mem_rec_ip_t s_no_of_mem_rec_query_ip;
                        iv_num_mem_rec_op_t s_no_of_mem_rec_query_op;


                        s_no_of_mem_rec_query_ip.u4_size = sizeof(iv_num_mem_rec_ip_t);
                        s_no_of_mem_rec_query_op.u4_size = sizeof(iv_num_mem_rec_op_t);

                        s_no_of_mem_rec_query_ip.e_cmd   = IV_CMD_GET_NUM_MEM_REC;
                        impeg2d_api_function(NULL,
                                                    (void *)&s_no_of_mem_rec_query_ip,
                                                    (void *)&s_no_of_mem_rec_query_op);

                        i4_num_memrec  = s_no_of_mem_rec_query_op.u4_num_mem_rec;



                    }


                    /* initialize mem records array with sizes */
                    for(i = 0; i < i4_num_memrec; i++)
                    {
                        as_mem_rec_ittiam_api[i].u4_size = sizeof(iv_mem_rec_t);
                    }

                    s_fill_mem_rec_ip.s_ivd_fill_mem_rec_ip_t.u4_size                   = sizeof(impeg2d_fill_mem_rec_ip_t);
                    s_fill_mem_rec_ip.s_ivd_fill_mem_rec_ip_t.e_cmd                     = IV_CMD_FILL_NUM_MEM_REC;
                    s_fill_mem_rec_ip.s_ivd_fill_mem_rec_ip_t.u4_max_frm_wd             = ps_ip->s_ivd_init_ip_t.u4_frm_max_wd;
                    s_fill_mem_rec_ip.s_ivd_fill_mem_rec_ip_t.u4_max_frm_ht             = ps_ip->s_ivd_init_ip_t.u4_frm_max_ht;
                    s_fill_mem_rec_ip.s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location       = as_mem_rec_ittiam_api;
                    s_fill_mem_rec_ip.u4_share_disp_buf                                 = ps_ip->u4_share_disp_buf;
                    s_fill_mem_rec_ip.e_output_format                                   = ps_ip->s_ivd_init_ip_t.e_output_format;
                    s_fill_mem_rec_op.s_ivd_fill_mem_rec_op_t.u4_size                   = sizeof(impeg2d_fill_mem_rec_op_t);


                    e_status = impeg2d_api_function(NULL,
                                                (void *)&s_fill_mem_rec_ip,
                                                (void *)&s_fill_mem_rec_op);
                    if(IV_FAIL == e_status)
                    {
                        ps_op->s_ivd_init_op_t.u4_error_code = s_fill_mem_rec_op.s_ivd_fill_mem_rec_op_t.u4_error_code;
                        return(IV_FAIL);
                    }



                    for(i = 0; i < i4_num_memrec; i ++)
                    {
                        if(ps_mem_rec[i].pv_base == NULL)
                        {
                            ps_op->s_ivd_init_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_MEM_REC_BASE_NULL;
                            return IV_FAIL;
                        }
#ifdef CHECK_ALIGN

                        if((UWORD32)(ps_mem_rec[i].pv_base) & (ps_mem_rec[i].u4_mem_alignment - 1))
                        {
                            ps_op->s_ivd_init_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_MEM_REC_ALIGNMENT_ERR;
                            return IV_FAIL;
                        }
#endif //CHECK_ALIGN
                        if(ps_mem_rec[i].u4_mem_alignment != as_mem_rec_ittiam_api[i].u4_mem_alignment)
                        {
                            ps_op->s_ivd_init_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_MEM_REC_ALIGNMENT_ERR;
                            return IV_FAIL;
                        }

                        if(ps_mem_rec[i].u4_mem_size < as_mem_rec_ittiam_api[i].u4_mem_size)
                        {
                            ps_op->s_ivd_init_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_MEM_REC_INSUFFICIENT_SIZE;
                            return IV_FAIL;
                        }

                        if(ps_mem_rec[i].e_mem_type != as_mem_rec_ittiam_api[i].e_mem_type)
                        {
                            if (IV_EXTERNAL_CACHEABLE_SCRATCH_MEM == as_mem_rec_ittiam_api[i].e_mem_type)
                            {
                                if (IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM == ps_mem_rec[i].e_mem_type)
                                {
                                    continue;
                                }
                            }
                            ps_op->s_ivd_init_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->s_ivd_init_op_t.u4_error_code |= IVD_INIT_DEC_MEM_REC_INCORRECT_TYPE;
                            return IV_FAIL;
                        }
                    }
                }


            }
            break;

        case IVD_CMD_GET_DISPLAY_FRAME:
            {
                impeg2d_get_display_frame_ip_t *ps_ip = (impeg2d_get_display_frame_ip_t *)pv_api_ip;
                impeg2d_get_display_frame_op_t *ps_op = (impeg2d_get_display_frame_op_t *)pv_api_op;

                ps_op->s_ivd_get_display_frame_op_t.u4_error_code = 0;

                if(ps_ip->s_ivd_get_display_frame_ip_t.u4_size != sizeof(impeg2d_get_display_frame_ip_t))
                {
                    ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                if(ps_op->s_ivd_get_display_frame_op_t.u4_size != sizeof(impeg2d_get_display_frame_op_t))
                {
                    ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                if(ps_ip->s_ivd_get_display_frame_ip_t.s_out_buffer.u4_num_bufs == 0)
                {
                    ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= IVD_DISP_FRM_ZERO_OP_BUFS;
                    return IV_FAIL;
                }

                for(i = 0; i< (WORD32)ps_ip->s_ivd_get_display_frame_ip_t.s_out_buffer.u4_num_bufs;i++)
                {
                    if(ps_ip->s_ivd_get_display_frame_ip_t.s_out_buffer.pu1_bufs[i] == NULL)
                    {
                        ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= IVD_DISP_FRM_OP_BUF_NULL;
                        return IV_FAIL;
                    }

                    if(ps_ip->s_ivd_get_display_frame_ip_t.s_out_buffer.u4_min_out_buf_size[i] == 0)
                    {
                        ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= IVD_DISP_FRM_ZERO_OP_BUF_SIZE;
                        return IV_FAIL;
                    }
                    /*
                    if(ps_ip->s_ivd_get_display_frame_ip_t.s_out_buffer.u4_min_out_buf_size[i] == 0)
                    {
                        ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= IVD_DISP_FRM_ZERO_OP_BUF_SIZE;
                        return IV_FAIL;
                    }
                    */
                }
            }
            break;
       case IVD_CMD_REL_DISPLAY_FRAME:
            {
                impeg2d_rel_display_frame_ip_t *ps_ip = (impeg2d_rel_display_frame_ip_t *)pv_api_ip;
                impeg2d_rel_display_frame_op_t *ps_op = (impeg2d_rel_display_frame_op_t *)pv_api_op;

                ps_op->s_ivd_rel_display_frame_op_t.u4_error_code = 0;

                if ((ps_ip->s_ivd_rel_display_frame_ip_t.u4_size != sizeof(impeg2d_rel_display_frame_ip_t))
                        && (ps_ip->s_ivd_rel_display_frame_ip_t.u4_size != sizeof(ivd_rel_display_frame_ip_t)))
                {
                    ps_op->s_ivd_rel_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_rel_display_frame_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                if((ps_op->s_ivd_rel_display_frame_op_t.u4_size != sizeof(impeg2d_rel_display_frame_op_t)) &&
                        (ps_op->s_ivd_rel_display_frame_op_t.u4_size != sizeof(ivd_rel_display_frame_op_t)))
                {
                    ps_op->s_ivd_rel_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_rel_display_frame_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

            }
            break;


        case IVD_CMD_SET_DISPLAY_FRAME:
            {
                impeg2d_set_display_frame_ip_t *ps_ip = (impeg2d_set_display_frame_ip_t *)pv_api_ip;
                impeg2d_set_display_frame_op_t *ps_op = (impeg2d_set_display_frame_op_t *)pv_api_op;
                UWORD32 j, i;

                ps_op->s_ivd_set_display_frame_op_t.u4_error_code = 0;

                if ((ps_ip->s_ivd_set_display_frame_ip_t.u4_size != sizeof(impeg2d_set_display_frame_ip_t))
                        && (ps_ip->s_ivd_set_display_frame_ip_t.u4_size != sizeof(ivd_set_display_frame_ip_t)))
                {
                    ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                if((ps_op->s_ivd_set_display_frame_op_t.u4_size != sizeof(impeg2d_set_display_frame_op_t)) &&
                        (ps_op->s_ivd_set_display_frame_op_t.u4_size != sizeof(ivd_set_display_frame_op_t)))
                {
                    ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                if(ps_ip->s_ivd_set_display_frame_ip_t.num_disp_bufs == 0)
                {
                    ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= IVD_DISP_FRM_ZERO_OP_BUFS;
                    return IV_FAIL;
                }

                for(j = 0; j < ps_ip->s_ivd_set_display_frame_ip_t.num_disp_bufs; j++)
                {
                    if(ps_ip->s_ivd_set_display_frame_ip_t.s_disp_buffer[j].u4_num_bufs == 0)
                    {
                        ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= IVD_DISP_FRM_ZERO_OP_BUFS;
                        return IV_FAIL;
                    }

                    for(i=0;i< ps_ip->s_ivd_set_display_frame_ip_t.s_disp_buffer[j].u4_num_bufs;i++)
                    {
                        if(ps_ip->s_ivd_set_display_frame_ip_t.s_disp_buffer[j].pu1_bufs[i] == NULL)
                        {
                            ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= IVD_DISP_FRM_OP_BUF_NULL;
                            return IV_FAIL;
                        }

                        if(ps_ip->s_ivd_set_display_frame_ip_t.s_disp_buffer[j].u4_min_out_buf_size[i] == 0)
                        {
                            ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= IVD_DISP_FRM_ZERO_OP_BUF_SIZE;
                            return IV_FAIL;
                        }
                    }
                }
            }
            break;

        case IVD_CMD_VIDEO_DECODE:
            {
                impeg2d_video_decode_ip_t *ps_ip = (impeg2d_video_decode_ip_t *)pv_api_ip;
                impeg2d_video_decode_op_t *ps_op = (impeg2d_video_decode_op_t *)pv_api_op;

                ps_op->s_ivd_video_decode_op_t.u4_error_code = 0;

                if(ps_ip->s_ivd_video_decode_ip_t.u4_size != sizeof(impeg2d_video_decode_ip_t))
                {
                    ps_op->s_ivd_video_decode_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_video_decode_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                if(ps_op->s_ivd_video_decode_op_t.u4_size != sizeof(impeg2d_video_decode_op_t))
                {
                    ps_op->s_ivd_video_decode_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_video_decode_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

            }
            break;

        case IV_CMD_RETRIEVE_MEMREC:
            {
                impeg2d_retrieve_mem_rec_ip_t *ps_ip = (impeg2d_retrieve_mem_rec_ip_t *)pv_api_ip;
                impeg2d_retrieve_mem_rec_op_t *ps_op = (impeg2d_retrieve_mem_rec_op_t *)pv_api_op;
                iv_mem_rec_t          *ps_mem_rec;

                ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code = 0;

                if(ps_ip->s_ivd_retrieve_mem_rec_ip_t.u4_size != sizeof(impeg2d_retrieve_mem_rec_ip_t))
                {
                    ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                if(ps_op->s_ivd_retrieve_mem_rec_op_t.u4_size != sizeof(impeg2d_retrieve_mem_rec_op_t))
                {
                    ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                    return(IV_FAIL);
                }

                ps_mem_rec  = ps_ip->s_ivd_retrieve_mem_rec_ip_t.pv_mem_rec_location;
                /* check memrecords sizes are correct */
                for(i=0;i < NUM_MEM_RECORDS ; i++)
                {
                    if(ps_mem_rec[i].u4_size != sizeof(iv_mem_rec_t))
                    {
                        ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |= IVD_MEM_REC_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                }
            }
            break;

        case IVD_CMD_VIDEO_CTL:
            {
                UWORD32 *pu4_ptr_cmd;
                UWORD32 u4_sub_command;

                pu4_ptr_cmd = (UWORD32 *)pv_api_ip;
                pu4_ptr_cmd += 2;
                u4_sub_command = *pu4_ptr_cmd;

                switch(u4_sub_command)
                {
                    case IVD_CMD_CTL_SETPARAMS:
                        {
                            impeg2d_ctl_set_config_ip_t *ps_ip;
                            impeg2d_ctl_set_config_op_t *ps_op;
                            ps_ip = (impeg2d_ctl_set_config_ip_t *)pv_api_ip;
                            ps_op = (impeg2d_ctl_set_config_op_t *)pv_api_op;

                            ps_op->s_ivd_ctl_set_config_op_t.u4_error_code = 0;

                            if(ps_ip->s_ivd_ctl_set_config_ip_t.u4_size != sizeof(impeg2d_ctl_set_config_ip_t))
                            {
                                ps_op->s_ivd_ctl_set_config_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_set_config_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                        }
                    case IVD_CMD_CTL_SETDEFAULT:
                        {
                            impeg2d_ctl_set_config_op_t *ps_op;
                            ps_op = (impeg2d_ctl_set_config_op_t *)pv_api_op;
                            ps_op->s_ivd_ctl_set_config_op_t.u4_error_code   = 0;

                            if(ps_op->s_ivd_ctl_set_config_op_t.u4_size != sizeof(impeg2d_ctl_set_config_op_t))
                            {
                                ps_op->s_ivd_ctl_set_config_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_set_config_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                        }
                        break;

                    case IVD_CMD_CTL_GETPARAMS:
                        {
                            impeg2d_ctl_getstatus_ip_t *ps_ip;
                            impeg2d_ctl_getstatus_op_t *ps_op;

                            ps_ip = (impeg2d_ctl_getstatus_ip_t *)pv_api_ip;
                            ps_op = (impeg2d_ctl_getstatus_op_t *)pv_api_op;

                            ps_op->s_ivd_ctl_getstatus_op_t.u4_error_code   = 0;

                            if(ps_ip->s_ivd_ctl_getstatus_ip_t.u4_size != sizeof(impeg2d_ctl_getstatus_ip_t))
                            {
                                ps_op->s_ivd_ctl_getstatus_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_getstatus_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                            if(ps_op->s_ivd_ctl_getstatus_op_t.u4_size != sizeof(impeg2d_ctl_getstatus_op_t))
                            {
                                ps_op->s_ivd_ctl_getstatus_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_getstatus_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                        }
                        break;

                    case IVD_CMD_CTL_GETBUFINFO:
                        {
                            impeg2d_ctl_getbufinfo_ip_t *ps_ip;
                            impeg2d_ctl_getbufinfo_op_t *ps_op;
                            ps_ip = (impeg2d_ctl_getbufinfo_ip_t *)pv_api_ip;
                            ps_op = (impeg2d_ctl_getbufinfo_op_t *)pv_api_op;

                            ps_op->s_ivd_ctl_getbufinfo_op_t.u4_error_code  = 0;

                            if(ps_ip->s_ivd_ctl_getbufinfo_ip_t.u4_size != sizeof(impeg2d_ctl_getbufinfo_ip_t))
                            {
                                ps_op->s_ivd_ctl_getbufinfo_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_getbufinfo_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                            if(ps_op->s_ivd_ctl_getbufinfo_op_t.u4_size != sizeof(impeg2d_ctl_getbufinfo_op_t))
                            {
                                ps_op->s_ivd_ctl_getbufinfo_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_getbufinfo_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                        }
                        break;

                    case IVD_CMD_CTL_GETVERSION:
                        {
                            impeg2d_ctl_getversioninfo_ip_t *ps_ip;
                            impeg2d_ctl_getversioninfo_op_t *ps_op;
                            ps_ip = (impeg2d_ctl_getversioninfo_ip_t *)pv_api_ip;
                            ps_op = (impeg2d_ctl_getversioninfo_op_t *)pv_api_op;

                            ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code  = 0;

                            if(ps_ip->s_ivd_ctl_getversioninfo_ip_t.u4_size != sizeof(impeg2d_ctl_getversioninfo_ip_t))
                            {
                                ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                            if(ps_op->s_ivd_ctl_getversioninfo_op_t.u4_size != sizeof(impeg2d_ctl_getversioninfo_op_t))
                            {
                                ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                        }
                        break;

                    case IVD_CMD_CTL_FLUSH:
                        {
                            impeg2d_ctl_flush_ip_t *ps_ip;
                            impeg2d_ctl_flush_op_t *ps_op;
                            ps_ip = (impeg2d_ctl_flush_ip_t *)pv_api_ip;
                            ps_op = (impeg2d_ctl_flush_op_t *)pv_api_op;

                            ps_op->s_ivd_ctl_flush_op_t.u4_error_code = 0;

                            if(ps_ip->s_ivd_ctl_flush_ip_t.u4_size != sizeof(impeg2d_ctl_flush_ip_t))
                            {
                                ps_op->s_ivd_ctl_flush_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_flush_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                            if(ps_op->s_ivd_ctl_flush_op_t.u4_size != sizeof(impeg2d_ctl_flush_op_t))
                            {
                                ps_op->s_ivd_ctl_flush_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_flush_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                        }
                        break;

                    case IVD_CMD_CTL_RESET:
                        {
                            impeg2d_ctl_reset_ip_t *ps_ip;
                            impeg2d_ctl_reset_op_t *ps_op;
                            ps_ip = (impeg2d_ctl_reset_ip_t *)pv_api_ip;
                            ps_op = (impeg2d_ctl_reset_op_t *)pv_api_op;

                            ps_op->s_ivd_ctl_reset_op_t.u4_error_code    = 0;

                            if(ps_ip->s_ivd_ctl_reset_ip_t.u4_size != sizeof(impeg2d_ctl_reset_ip_t))
                            {
                                ps_op->s_ivd_ctl_reset_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_reset_op_t.u4_error_code |= IVD_IP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                            if(ps_op->s_ivd_ctl_reset_op_t.u4_size != sizeof(impeg2d_ctl_reset_op_t))
                            {
                                ps_op->s_ivd_ctl_reset_op_t.u4_error_code  |= 1 << IVD_UNSUPPORTEDPARAM;
                                ps_op->s_ivd_ctl_reset_op_t.u4_error_code |= IVD_OP_API_STRUCT_SIZE_INCORRECT;
                                return IV_FAIL;
                            }
                        }
                        break;

                    case IMPEG2D_CMD_CTL_GET_BUFFER_DIMENSIONS:
                    {
                        impeg2d_ctl_get_frame_dimensions_ip_t *ps_ip;
                        impeg2d_ctl_get_frame_dimensions_op_t *ps_op;

                        ps_ip =
                                        (impeg2d_ctl_get_frame_dimensions_ip_t *)pv_api_ip;
                        ps_op =
                                        (impeg2d_ctl_get_frame_dimensions_op_t *)pv_api_op;

                        if(ps_ip->u4_size
                                        != sizeof(impeg2d_ctl_get_frame_dimensions_ip_t))
                        {
                            ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->u4_error_code |=
                                            IVD_IP_API_STRUCT_SIZE_INCORRECT;
                            return IV_FAIL;
                        }

                        if(ps_op->u4_size
                                        != sizeof(impeg2d_ctl_get_frame_dimensions_op_t))
                        {
                            ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->u4_error_code |=
                                            IVD_OP_API_STRUCT_SIZE_INCORRECT;
                            return IV_FAIL;
                        }

                        break;
                    }
                    case IMPEG2D_CMD_CTL_GET_SEQ_INFO:
                    {
                        impeg2d_ctl_get_seq_info_ip_t *ps_ip;
                        impeg2d_ctl_get_seq_info_op_t *ps_op;

                        ps_ip =
                                        (impeg2d_ctl_get_seq_info_ip_t *)pv_api_ip;
                        ps_op =
                                        (impeg2d_ctl_get_seq_info_op_t *)pv_api_op;

                        if(ps_ip->u4_size
                                        != sizeof(impeg2d_ctl_get_seq_info_ip_t))
                        {
                            ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->u4_error_code |=
                                            IVD_IP_API_STRUCT_SIZE_INCORRECT;
                            return IV_FAIL;
                        }

                        if(ps_op->u4_size
                                        != sizeof(impeg2d_ctl_get_seq_info_op_t))
                        {
                            ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->u4_error_code |=
                                            IVD_OP_API_STRUCT_SIZE_INCORRECT;
                            return IV_FAIL;
                        }

                        break;
                    }
                    case IMPEG2D_CMD_CTL_SET_NUM_CORES:
                    {
                        impeg2d_ctl_set_num_cores_ip_t *ps_ip;
                        impeg2d_ctl_set_num_cores_op_t *ps_op;

                        ps_ip = (impeg2d_ctl_set_num_cores_ip_t *)pv_api_ip;
                        ps_op = (impeg2d_ctl_set_num_cores_op_t *)pv_api_op;

                        if(ps_ip->u4_size
                                        != sizeof(impeg2d_ctl_set_num_cores_ip_t))
                        {
                            ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->u4_error_code |=
                                            IVD_IP_API_STRUCT_SIZE_INCORRECT;
                            return IV_FAIL;
                        }

                        if(ps_op->u4_size
                                        != sizeof(impeg2d_ctl_set_num_cores_op_t))
                        {
                            ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->u4_error_code |=
                                            IVD_OP_API_STRUCT_SIZE_INCORRECT;
                            return IV_FAIL;
                        }

#ifdef MULTICORE
                        if((ps_ip->u4_num_cores < 1) || (ps_ip->u4_num_cores > MAX_THREADS))
#else
                        if(ps_ip->u4_num_cores != 1)
#endif
                        {
                            ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            return IV_FAIL;
                        }
                        break;
                    }
                    case IMPEG2D_CMD_CTL_SET_PROCESSOR:
                    {
                        impeg2d_ctl_set_processor_ip_t *ps_ip;
                        impeg2d_ctl_set_processor_op_t *ps_op;

                        ps_ip = (impeg2d_ctl_set_processor_ip_t *)pv_api_ip;
                        ps_op = (impeg2d_ctl_set_processor_op_t *)pv_api_op;

                        if(ps_ip->u4_size
                                        != sizeof(impeg2d_ctl_set_processor_ip_t))
                        {
                            ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->u4_error_code |=
                                            IVD_IP_API_STRUCT_SIZE_INCORRECT;
                            return IV_FAIL;
                        }

                        if(ps_op->u4_size
                                        != sizeof(impeg2d_ctl_set_processor_op_t))
                        {
                            ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                            ps_op->u4_error_code |=
                                            IVD_OP_API_STRUCT_SIZE_INCORRECT;
                            return IV_FAIL;
                        }

                        break;
                    }
                    default:
                        break;

                }
            }
            break;

        default:
            {            *(pu4_api_op + 1) |= 1 << IVD_UNSUPPORTEDPARAM;
                         *(pu4_api_op + 1) |= IVD_UNSUPPORTED_API_CMD;
                         return IV_FAIL;
            }


    }

    return IV_SUCCESS;
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :   impeg2d_api_entity                                     */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :                                                          */
/*  Globals       : <Does it use any global variables?>                      */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         22 10 2008    100356         Draft                                */
/*                                                                           */
/*****************************************************************************/


IV_API_CALL_STATUS_T impeg2d_api_entity(iv_obj_t *ps_dechdl,
                                        void *pv_api_ip,
                                        void *pv_api_op)
{
    iv_obj_t *ps_dec_handle;
    dec_state_t *ps_dec_state;
    dec_state_multi_core_t *ps_dec_state_multi_core;

    impeg2d_video_decode_ip_t    *ps_dec_ip;

    impeg2d_video_decode_op_t    *ps_dec_op;
    WORD32 bytes_remaining;
    pic_buf_t *ps_disp_pic;



    ps_dec_ip = (impeg2d_video_decode_ip_t    *)pv_api_ip;
    ps_dec_op = (impeg2d_video_decode_op_t    *)pv_api_op;

    memset(ps_dec_op,0,sizeof(impeg2d_video_decode_op_t));

    ps_dec_op->s_ivd_video_decode_op_t.u4_size = sizeof(impeg2d_video_decode_op_t);
    ps_dec_op->s_ivd_video_decode_op_t.u4_output_present = 0;
    bytes_remaining = ps_dec_ip->s_ivd_video_decode_ip_t.u4_num_Bytes;

    ps_dec_handle = (iv_obj_t *)ps_dechdl;

    if(ps_dechdl == NULL)
    {
        return(IV_FAIL);
    }



    ps_dec_state_multi_core  = ps_dec_handle->pv_codec_handle;
    ps_dec_state = ps_dec_state_multi_core->ps_dec_state[0];

    ps_dec_state->ps_disp_frm_buf = &(ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf);
    if(0 == ps_dec_state->u4_share_disp_buf)
    {
        ps_dec_state->ps_disp_frm_buf->pv_y_buf  = ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[0];
        ps_dec_state->ps_disp_frm_buf->pv_u_buf  = ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[1];
        ps_dec_state->ps_disp_frm_buf->pv_v_buf  = ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[2];
    }

    ps_dec_state->ps_disp_pic = NULL;
    ps_dec_state->i4_frame_decoded = 0;
    /*rest bytes consumed */
    ps_dec_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = 0;

    ps_dec_op->s_ivd_video_decode_op_t.u4_error_code           = IV_SUCCESS;

    if((ps_dec_ip->s_ivd_video_decode_ip_t.pv_stream_buffer == NULL)&&(ps_dec_state->u1_flushfrm==0))
    {
        ps_dec_op->s_ivd_video_decode_op_t.u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
        ps_dec_op->s_ivd_video_decode_op_t.u4_error_code |= IVD_DEC_FRM_BS_BUF_NULL;
        return IV_FAIL;
    }


    if (ps_dec_state->u4_num_frames_decoded > NUM_FRAMES_LIMIT)
    {
        ps_dec_op->s_ivd_video_decode_op_t.u4_error_code       = IMPEG2D_SAMPLE_VERSION_LIMIT_ERR;
        return(IV_FAIL);
    }

    if(((0 == ps_dec_state->u2_header_done) || (ps_dec_state->u2_decode_header == 1)) && (ps_dec_state->u1_flushfrm == 0))
    {
        impeg2d_dec_hdr(ps_dec_state,ps_dec_ip ,ps_dec_op);
        bytes_remaining -= ps_dec_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed;
    }

    if((1 != ps_dec_state->u2_decode_header) &&
        (((bytes_remaining > 0) && (1 == ps_dec_state->u2_header_done)) || ps_dec_state->u1_flushfrm))
    {
        if(ps_dec_state->u1_flushfrm)
        {
            if(ps_dec_state->aps_ref_pics[1] != NULL)
            {
                impeg2_disp_mgr_add(&ps_dec_state->s_disp_mgr, ps_dec_state->aps_ref_pics[1], ps_dec_state->aps_ref_pics[1]->i4_buf_id);
                impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg, ps_dec_state->aps_ref_pics[1]->i4_buf_id, BUF_MGR_REF);
                impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg, ps_dec_state->aps_ref_pics[0]->i4_buf_id, BUF_MGR_REF);

                ps_dec_state->aps_ref_pics[1] = NULL;
                ps_dec_state->aps_ref_pics[0] = NULL;

            }
            else if(ps_dec_state->aps_ref_pics[0] != NULL)
            {
                impeg2_disp_mgr_add(&ps_dec_state->s_disp_mgr, ps_dec_state->aps_ref_pics[0], ps_dec_state->aps_ref_pics[0]->i4_buf_id);
                impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg, ps_dec_state->aps_ref_pics[0]->i4_buf_id, BUF_MGR_REF);

                ps_dec_state->aps_ref_pics[0] = NULL;
            }
            ps_dec_ip->s_ivd_video_decode_ip_t.u4_size                 = sizeof(impeg2d_video_decode_ip_t);
            ps_dec_op->s_ivd_video_decode_op_t.u4_size                 = sizeof(impeg2d_video_decode_op_t);

            ps_disp_pic = impeg2_disp_mgr_get(&ps_dec_state->s_disp_mgr, &ps_dec_state->i4_disp_buf_id);

            ps_dec_state->ps_disp_pic = ps_disp_pic;
            if(ps_disp_pic == NULL)
            {
                ps_dec_op->s_ivd_video_decode_op_t.u4_output_present = 0;
            }
            else
            {
                WORD32 fmt_conv;
                if(0 == ps_dec_state->u4_share_disp_buf)
                {
                    ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.pv_y_buf  = ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[0];
                    ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.pv_u_buf  = ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[1];
                    ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.pv_v_buf  = ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[2];
                    fmt_conv = 1;
                }
                else
                {
                    ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.pv_y_buf  = ps_disp_pic->pu1_y;
                    if(IV_YUV_420P == ps_dec_state->i4_chromaFormat)
                    {
                        ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.pv_u_buf  = ps_disp_pic->pu1_u;
                        ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.pv_v_buf  = ps_disp_pic->pu1_v;
                        fmt_conv = 0;
                    }
                    else
                    {
                        UWORD8 *pu1_buf;

                        pu1_buf = ps_dec_state->as_disp_buffers[ps_disp_pic->i4_buf_id].pu1_bufs[1];
                        ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.pv_u_buf  = pu1_buf;

                        pu1_buf = ps_dec_state->as_disp_buffers[ps_disp_pic->i4_buf_id].pu1_bufs[2];
                        ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.pv_v_buf  = pu1_buf;
                        fmt_conv = 1;
                    }
                }

                if(fmt_conv == 1)
                {
                    iv_yuv_buf_t *ps_dst;


                    ps_dst = &(ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf);
                    if(ps_dec_state->u4_deinterlace && (0 == ps_dec_state->u2_progressive_frame))
                    {
                        impeg2d_deinterlace(ps_dec_state,
                                            ps_disp_pic,
                                            ps_dst,
                                            0,
                                            ps_dec_state->u2_vertical_size);

                    }
                    else
                    {
                        impeg2d_format_convert(ps_dec_state,
                                               ps_disp_pic,
                                               ps_dst,
                                               0,
                                               ps_dec_state->u2_vertical_size);
                    }
                }

                if(ps_dec_state->u4_deinterlace)
                {
                    if(ps_dec_state->ps_deint_pic)
                    {
                        impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg,
                                               ps_dec_state->ps_deint_pic->i4_buf_id,
                                               MPEG2_BUF_MGR_DEINT);
                    }
                    ps_dec_state->ps_deint_pic = ps_disp_pic;
                }
                if(0 == ps_dec_state->u4_share_disp_buf)
                    impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg, ps_disp_pic->i4_buf_id, BUF_MGR_DISP);

                ps_dec_op->s_ivd_video_decode_op_t.u4_pic_ht = ps_dec_state->u2_vertical_size;
                ps_dec_op->s_ivd_video_decode_op_t.u4_pic_wd = ps_dec_state->u2_horizontal_size;
                ps_dec_op->s_ivd_video_decode_op_t.u4_output_present = 1;

                ps_dec_op->s_ivd_video_decode_op_t.u4_disp_buf_id = ps_disp_pic->i4_buf_id;
                ps_dec_op->s_ivd_video_decode_op_t.u4_ts = ps_disp_pic->u4_ts;

                ps_dec_op->s_ivd_video_decode_op_t.e_output_format = (IV_COLOR_FORMAT_T)ps_dec_state->i4_chromaFormat;

                ps_dec_op->s_ivd_video_decode_op_t.u4_is_ref_flag = (B_PIC != ps_dec_state->e_pic_type);

                ps_dec_op->s_ivd_video_decode_op_t.u4_progressive_frame_flag           = IV_PROGRESSIVE;

                ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_y_wd = ps_dec_state->u2_horizontal_size;
                ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_y_strd = ps_dec_state->u4_frm_buf_stride;
                ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_y_ht = ps_dec_state->u2_vertical_size;

                ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_wd = ps_dec_state->u2_horizontal_size >> 1;
                ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_strd = ps_dec_state->u4_frm_buf_stride >> 1;
                ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_ht = ps_dec_state->u2_vertical_size >> 1;

                ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_v_wd = ps_dec_state->u2_horizontal_size >> 1;
                ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_v_strd = ps_dec_state->u4_frm_buf_stride >> 1;
                ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_v_ht = ps_dec_state->u2_vertical_size >> 1;
                ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_size = sizeof(ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf);

                switch(ps_dec_state->i4_chromaFormat)
                {
                    case IV_YUV_420SP_UV:
                    case IV_YUV_420SP_VU:
                        ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_wd = ps_dec_state->u2_horizontal_size;
                        ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_strd = ps_dec_state->u4_frm_buf_stride;
                    break;
                    case IV_YUV_422ILE:
                        ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_wd = 0;
                        ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_ht = 0;
                        ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_v_wd = 0;
                        ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_v_ht = 0;
                    break;
                    default:
                    break;
                }


            }
            if(ps_dec_op->s_ivd_video_decode_op_t.u4_output_present)
            {
                if(1 == ps_dec_op->s_ivd_video_decode_op_t.u4_output_present)
                {
                    INSERT_LOGO(ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[0],
                                ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[1],
                                ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[2],
                                ps_dec_state->u4_frm_buf_stride,
                                ps_dec_state->u2_horizontal_size,
                                ps_dec_state->u2_vertical_size,
                                ps_dec_state->i4_chromaFormat,
                                ps_dec_state->u2_horizontal_size,
                                ps_dec_state->u2_vertical_size);
                }
                return(IV_SUCCESS);
            }
            else
            {
                ps_dec_state->u1_flushfrm = 0;

                return(IV_FAIL);
            }

        }
        else if(ps_dec_state->u1_flushfrm==0)
        {
            ps_dec_ip->s_ivd_video_decode_ip_t.u4_size                 = sizeof(impeg2d_video_decode_ip_t);
            ps_dec_op->s_ivd_video_decode_op_t.u4_size                 = sizeof(impeg2d_video_decode_op_t);
            if(ps_dec_ip->s_ivd_video_decode_ip_t.u4_num_Bytes < 4)
            {
                ps_dec_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = ps_dec_ip->s_ivd_video_decode_ip_t.u4_num_Bytes;
                return(IV_FAIL);
            }

            if(1 == ps_dec_state->u4_share_disp_buf)
            {
                if(0 == impeg2_buf_mgr_check_free(ps_dec_state->pv_pic_buf_mg))
                {
                    ps_dec_op->s_ivd_video_decode_op_t.u4_error_code =
                                    (IMPEG2D_ERROR_CODES_T)IVD_DEC_REF_BUF_NULL;
                    return IV_FAIL;
                }
            }


            ps_dec_op->s_ivd_video_decode_op_t.e_output_format = (IV_COLOR_FORMAT_T)ps_dec_state->i4_chromaFormat;

            ps_dec_op->s_ivd_video_decode_op_t.u4_is_ref_flag = (B_PIC != ps_dec_state->e_pic_type);

            ps_dec_op->s_ivd_video_decode_op_t.u4_progressive_frame_flag           = IV_PROGRESSIVE;

            if (0 == ps_dec_state->u4_frm_buf_stride)
            {
                ps_dec_state->u4_frm_buf_stride = (ps_dec_state->u2_horizontal_size);
            }

            ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_y_wd = ps_dec_state->u2_horizontal_size;
            ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_y_strd = ps_dec_state->u4_frm_buf_stride;
            ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_y_ht = ps_dec_state->u2_vertical_size;

            ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_wd = ps_dec_state->u2_horizontal_size >> 1;
            ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_strd = ps_dec_state->u4_frm_buf_stride >> 1;
            ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_ht = ps_dec_state->u2_vertical_size >> 1;

            ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_v_wd = ps_dec_state->u2_horizontal_size >> 1;
            ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_v_strd = ps_dec_state->u4_frm_buf_stride >> 1;
            ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_v_ht = ps_dec_state->u2_vertical_size >> 1;
            ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_size = sizeof(ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf);

            switch(ps_dec_state->i4_chromaFormat)
            {
                case IV_YUV_420SP_UV:
                case IV_YUV_420SP_VU:
                    ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_wd = ps_dec_state->u2_horizontal_size;
                    ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_strd = ps_dec_state->u4_frm_buf_stride;
                break;
                case IV_YUV_422ILE:
                    ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_wd = 0;
                    ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_u_ht = 0;
                    ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_v_wd = 0;
                    ps_dec_op->s_ivd_video_decode_op_t.s_disp_frm_buf.u4_v_ht = 0;
                break;
                default:
                break;
            }

            if( ps_dec_state->u1_flushfrm == 0)
            {
                ps_dec_state->u1_flushcnt    = 0;

                /*************************************************************************/
                /*                              Frame Decode                             */
                /*************************************************************************/

                impeg2d_dec_frm(ps_dec_state,ps_dec_ip,ps_dec_op);

                if (IVD_ERROR_NONE ==
                        ps_dec_op->s_ivd_video_decode_op_t.u4_error_code)
                {
                    if(ps_dec_state->u1_first_frame_done == 0)
                    {
                        ps_dec_state->u1_first_frame_done = 1;
                    }

                    if(ps_dec_state->ps_disp_pic)
                    {
                        ps_dec_op->s_ivd_video_decode_op_t.u4_output_present = 1;
                        switch(ps_dec_state->ps_disp_pic->e_pic_type)
                        {
                            case I_PIC :
                            ps_dec_op->s_ivd_video_decode_op_t.e_pic_type = IV_I_FRAME;
                            break;

                            case P_PIC:
                            ps_dec_op->s_ivd_video_decode_op_t.e_pic_type = IV_P_FRAME;
                            break;

                            case B_PIC:
                            ps_dec_op->s_ivd_video_decode_op_t.e_pic_type = IV_B_FRAME;
                            break;

                            case D_PIC:
                            ps_dec_op->s_ivd_video_decode_op_t.e_pic_type = IV_I_FRAME;
                            break;

                            default :
                            ps_dec_op->s_ivd_video_decode_op_t.e_pic_type = IV_FRAMETYPE_DEFAULT;
                            break;
                        }
                    }
                    else
                    {
                        ps_dec_op->s_ivd_video_decode_op_t.u4_output_present = 0;
                        ps_dec_op->s_ivd_video_decode_op_t.e_pic_type = IV_NA_FRAME;
                    }

                    ps_dec_state->u4_num_frames_decoded++;
                }
            }
            else
            {
                ps_dec_state->u1_flushcnt++;
            }
        }
        if(ps_dec_state->ps_disp_pic)
        {
            ps_dec_op->s_ivd_video_decode_op_t.u4_disp_buf_id = ps_dec_state->ps_disp_pic->i4_buf_id;
            ps_dec_op->s_ivd_video_decode_op_t.u4_ts = ps_dec_state->ps_disp_pic->u4_ts;

            if(0 == ps_dec_state->u4_share_disp_buf)
            {
                impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg, ps_dec_state->ps_disp_pic->i4_buf_id, BUF_MGR_DISP);
            }
        }

        if(ps_dec_state->u4_deinterlace)
        {
            if(ps_dec_state->ps_deint_pic)
            {
                impeg2_buf_mgr_release(ps_dec_state->pv_pic_buf_mg,
                                       ps_dec_state->ps_deint_pic->i4_buf_id,
                                       MPEG2_BUF_MGR_DEINT);
            }
            ps_dec_state->ps_deint_pic = ps_dec_state->ps_disp_pic;
        }

        if(1 == ps_dec_op->s_ivd_video_decode_op_t.u4_output_present)
        {
            INSERT_LOGO(ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[0],
                        ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[1],
                        ps_dec_ip->s_ivd_video_decode_ip_t.s_out_buffer.pu1_bufs[2],
                        ps_dec_state->u4_frm_buf_stride,
                        ps_dec_state->u2_horizontal_size,
                        ps_dec_state->u2_vertical_size,
                        ps_dec_state->i4_chromaFormat,
                        ps_dec_state->u2_horizontal_size,
                        ps_dec_state->u2_vertical_size);
        }

    }

    ps_dec_op->s_ivd_video_decode_op_t.u4_progressive_frame_flag = 1;
    ps_dec_op->s_ivd_video_decode_op_t.e4_fld_type     = ps_dec_state->s_disp_op.e4_fld_type;


    if(ps_dec_op->s_ivd_video_decode_op_t.u4_error_code)
        return IV_FAIL;
    else
        return IV_SUCCESS;
}
