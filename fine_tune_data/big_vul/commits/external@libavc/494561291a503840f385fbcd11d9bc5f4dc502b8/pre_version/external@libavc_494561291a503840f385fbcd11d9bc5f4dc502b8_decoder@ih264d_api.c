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
/*  File Name         : ih264d_api.c                                         */
/*                                                                           */
/*  Description       : Has all  API related functions                       */
/*                                                                           */
/*                                                                           */
/*  List of Functions : api_check_struct_sanity                              */
/*          ih264d_set_processor                                             */
/*          ih264d_get_num_rec                                               */
/*          ih264d_init_decoder                                              */
/*          ih264d_init_video_decoder                                        */
/*          ih264d_fill_num_mem_rec                                          */
/*          ih264d_clr                                                       */
/*          ih264d_init                                                      */
/*          ih264d_map_error                                                 */
/*          ih264d_video_decode                                              */
/*          ih264d_get_version                                               */
/*          ih264d_get_display_frame                                         */
/*          ih264d_set_display_frame                                         */
/*          ih264d_set_flush_mode                                            */
/*          ih264d_get_status                                                */
/*          ih264d_get_buf_info                                              */
/*          ih264d_set_params                                                */
/*          ih264d_set_default_params                                        */
/*          ih264d_reset                                                     */
/*          ih264d_ctl                                                       */
/*          ih264d_rel_display_frame                                         */
/*          ih264d_set_degrade                                               */
/*          ih264d_get_frame_dimensions                                      */
/*          ih264d_set_num_cores                                             */
/*          ih264d_fill_output_struct_from_context                           */
/*          ih264d_api_function                                              */
/*                                                                           */
/*  Issues / Problems : None                                                 */
/*                                                                           */
/*  Revision History  :                                                      */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         14 10 2008   100356(SKV)     Draft                                */
/*                                                                           */
/*****************************************************************************/
#include "ih264_typedefs.h"
#include "ih264_macros.h"
#include "ih264_platform_macros.h"
#include "ih264d_tables.h"
#include "iv.h"
#include "ivd.h"
#include "ih264d.h"
#include "ih264d_defs.h"

#include <string.h>
#include <limits.h>
#include <stddef.h>

#include "ih264d_inter_pred.h"

#include "ih264d_structs.h"
#include "ih264d_nal.h"
#include "ih264d_error_handler.h"

#include "ih264d_defs.h"

#include "ithread.h"
#include "ih264d_parse_slice.h"
#include "ih264d_function_selector.h"
#include "ih264_error.h"
#include "ih264_disp_mgr.h"
#include "ih264_buf_mgr.h"
#include "ih264d_deblocking.h"
#include "ih264d_parse_cavlc.h"
#include "ih264d_parse_cabac.h"
#include "ih264d_utils.h"
#include "ih264d_format_conv.h"
#include "ih264d_parse_headers.h"
#include "ih264d_thread_compute_bs.h"
#include <assert.h>


/*********************/
/* Codec Versioning  */
/*********************/
//Move this to where it is used
#define CODEC_NAME              "H264VDEC"
#define CODEC_RELEASE_TYPE      "production"
#define CODEC_RELEASE_VER       "04.00"
#define CODEC_VENDOR            "ITTIAM"
#define MAXVERSION_STRLEN       511
#define VERSION(version_string, codec_name, codec_release_type, codec_release_ver, codec_vendor)    \
    snprintf(version_string, MAXVERSION_STRLEN,                                                     \
             "@(#)Id:%s_%s Ver:%s Released by %s Build: %s @ %s",                                   \
             codec_name, codec_release_type, codec_release_ver, codec_vendor, __DATE__, __TIME__)

#define MAX_NAL_UNIT_SIZE       MAX((H264_MAX_FRAME_HEIGHT * H264_MAX_FRAME_HEIGHT),MIN_NALUNIT_SIZE)
#define MIN_NALUNIT_SIZE        200000


#define MIN_IN_BUFS             1
#define MIN_OUT_BUFS_420        3
#define MIN_OUT_BUFS_422ILE     1
#define MIN_OUT_BUFS_RGB565     1
#define MIN_OUT_BUFS_420SP      2
#define MIN_IN_BUF_SIZE (2*1024*1024)  // Currently, i4_size set to 500kb, CHECK LATER

#define NUM_FRAMES_LIMIT_ENABLED 0

#if NUM_FRAMES_LIMIT_ENABLED
#define NUM_FRAMES_LIMIT 10000
#else
#define NUM_FRAMES_LIMIT 0x7FFFFFFF
#endif


UWORD32 ih264d_get_extra_mem_external(UWORD32 width, UWORD32 height);
WORD32 ih264d_get_frame_dimensions(iv_obj_t *dec_hdl,
                                   void *pv_api_ip,
                                   void *pv_api_op);
WORD32 ih264d_set_num_cores(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op);

WORD32 ih264d_deblock_display(dec_struct_t *ps_dec);

void ih264d_signal_decode_thread(dec_struct_t *ps_dec);

void ih264d_signal_bs_deblk_thread(dec_struct_t *ps_dec);
void ih264d_decode_picture_thread(dec_struct_t *ps_dec);

WORD32 ih264d_set_degrade(iv_obj_t *ps_codec_obj,
                          void *pv_api_ip,
                          void *pv_api_op);

void ih264d_fill_output_struct_from_context(dec_struct_t *ps_dec,
                                            ivd_video_decode_op_t *ps_dec_op);

static IV_API_CALL_STATUS_T api_check_struct_sanity(iv_obj_t *ps_handle,
                                                    void *pv_api_ip,
                                                    void *pv_api_op)
{
    IVD_API_COMMAND_TYPE_T e_cmd;
    UWORD32 *pu4_api_ip;
    UWORD32 *pu4_api_op;
    UWORD32 i, j;

    if(NULL == pv_api_op)
        return (IV_FAIL);

    if(NULL == pv_api_ip)
        return (IV_FAIL);

    pu4_api_ip = (UWORD32 *)pv_api_ip;
    pu4_api_op = (UWORD32 *)pv_api_op;
    e_cmd = *(pu4_api_ip + 1);

    /* error checks on handle */
    switch((WORD32)e_cmd)
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
                H264_DEC_DEBUG_PRINT(
                                "Sizes do not match. Expected: %d, Got: %d",
                                sizeof(iv_obj_t), ps_handle->u4_size);
                return IV_FAIL;
            }
            break;
        case IVD_CMD_REL_DISPLAY_FRAME:
        case IVD_CMD_SET_DISPLAY_FRAME:
        case IVD_CMD_GET_DISPLAY_FRAME:
        case IVD_CMD_VIDEO_DECODE:
        case IV_CMD_RETRIEVE_MEMREC:
        case IVD_CMD_VIDEO_CTL:
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

            if(ps_handle->pv_fxns != ih264d_api_function)
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
            break;
        default:
            *(pu4_api_op + 1) |= 1 << IVD_UNSUPPORTEDPARAM;
            *(pu4_api_op + 1) |= IVD_INVALID_API_CMD;
            return IV_FAIL;
    }

    switch((WORD32)e_cmd)
    {
        case IV_CMD_GET_NUM_MEM_REC:
        {
            ih264d_num_mem_rec_ip_t *ps_ip =
                            (ih264d_num_mem_rec_ip_t *)pv_api_ip;
            ih264d_num_mem_rec_op_t *ps_op =
                            (ih264d_num_mem_rec_op_t *)pv_api_op;
            ps_op->s_ivd_num_mem_rec_op_t.u4_error_code = 0;

            if(ps_ip->s_ivd_num_mem_rec_ip_t.u4_size
                            != sizeof(ih264d_num_mem_rec_ip_t))
            {
                ps_op->s_ivd_num_mem_rec_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_num_mem_rec_op_t.u4_error_code |=
                                IVD_IP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

            if(ps_op->s_ivd_num_mem_rec_op_t.u4_size
                            != sizeof(ih264d_num_mem_rec_op_t))
            {
                ps_op->s_ivd_num_mem_rec_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_num_mem_rec_op_t.u4_error_code |=
                                IVD_OP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }
        }
            break;
        case IV_CMD_FILL_NUM_MEM_REC:
        {
            ih264d_fill_mem_rec_ip_t *ps_ip =
                            (ih264d_fill_mem_rec_ip_t *)pv_api_ip;
            ih264d_fill_mem_rec_op_t *ps_op =
                            (ih264d_fill_mem_rec_op_t *)pv_api_op;
            iv_mem_rec_t *ps_mem_rec;
            WORD32 max_wd = ps_ip->s_ivd_fill_mem_rec_ip_t.u4_max_frm_wd;
            WORD32 max_ht = ps_ip->s_ivd_fill_mem_rec_ip_t.u4_max_frm_ht;

            max_wd = ALIGN16(max_wd);
            max_ht = ALIGN32(max_ht);

            ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code = 0;

            if((ps_ip->s_ivd_fill_mem_rec_ip_t.u4_size
                            > sizeof(ih264d_fill_mem_rec_ip_t))
                            || (ps_ip->s_ivd_fill_mem_rec_ip_t.u4_size
                                            < sizeof(iv_fill_mem_rec_ip_t)))
            {
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |=
                                IVD_IP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

            if((ps_op->s_ivd_fill_mem_rec_op_t.u4_size
                            != sizeof(ih264d_fill_mem_rec_op_t))
                            && (ps_op->s_ivd_fill_mem_rec_op_t.u4_size
                                            != sizeof(iv_fill_mem_rec_op_t)))
            {
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |=
                                IVD_OP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

            if(max_wd < H264_MIN_FRAME_WIDTH)
            {
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |=
                                IVD_REQUESTED_WIDTH_NOT_SUPPPORTED;
                return (IV_FAIL);
            }

            if(max_wd > H264_MAX_FRAME_WIDTH)
            {
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |=
                                IVD_REQUESTED_WIDTH_NOT_SUPPPORTED;
                return (IV_FAIL);
            }

            if(max_ht < H264_MIN_FRAME_HEIGHT)
            {
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |=
                                IVD_REQUESTED_HEIGHT_NOT_SUPPPORTED;
                return (IV_FAIL);
            }

            if((max_ht * max_wd)
                            > (H264_MAX_FRAME_HEIGHT * H264_MAX_FRAME_WIDTH))

            {
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |=
                                IVD_REQUESTED_HEIGHT_NOT_SUPPPORTED;
                return (IV_FAIL);
            }

            if(NULL == ps_ip->s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location)
            {
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |=
                                IVD_NUM_REC_NOT_SUFFICIENT;
                return (IV_FAIL);
            }

            /* check memrecords sizes are correct */
            ps_mem_rec = ps_ip->s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location;
            for(i = 0; i < MEM_REC_CNT; i++)
            {
                if(ps_mem_rec[i].u4_size != sizeof(iv_mem_rec_t))
                {
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= 1
                                    << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_fill_mem_rec_op_t.u4_error_code |=
                                    IVD_MEM_REC_STRUCT_SIZE_INCORRECT;
                    return IV_FAIL;
                }
            }
        }
            break;

        case IV_CMD_INIT:
        {
            ih264d_init_ip_t *ps_ip = (ih264d_init_ip_t *)pv_api_ip;
            ih264d_init_op_t *ps_op = (ih264d_init_op_t *)pv_api_op;
            iv_mem_rec_t *ps_mem_rec;
            WORD32 max_wd = ps_ip->s_ivd_init_ip_t.u4_frm_max_wd;
            WORD32 max_ht = ps_ip->s_ivd_init_ip_t.u4_frm_max_ht;

            max_wd = ALIGN16(max_wd);
            max_ht = ALIGN32(max_ht);

            ps_op->s_ivd_init_op_t.u4_error_code = 0;

            if((ps_ip->s_ivd_init_ip_t.u4_size > sizeof(ih264d_init_ip_t))
                            || (ps_ip->s_ivd_init_ip_t.u4_size
                                            < sizeof(ivd_init_ip_t)))
            {
                ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_init_op_t.u4_error_code |=
                                IVD_IP_API_STRUCT_SIZE_INCORRECT;
                H264_DEC_DEBUG_PRINT("\n");
                return (IV_FAIL);
            }

            if((ps_op->s_ivd_init_op_t.u4_size != sizeof(ih264d_init_op_t))
                            && (ps_op->s_ivd_init_op_t.u4_size
                                            != sizeof(ivd_init_op_t)))
            {
                ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_init_op_t.u4_error_code |=
                                IVD_OP_API_STRUCT_SIZE_INCORRECT;
                H264_DEC_DEBUG_PRINT("\n");
                return (IV_FAIL);
            }

            if(ps_ip->s_ivd_init_ip_t.u4_num_mem_rec != MEM_REC_CNT)
            {
                ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_init_op_t.u4_error_code |=
                                IVD_INIT_DEC_NOT_SUFFICIENT;
                H264_DEC_DEBUG_PRINT("\n");
                return (IV_FAIL);
            }

            if(max_wd < H264_MIN_FRAME_WIDTH)
            {
                ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_init_op_t.u4_error_code |=
                                IVD_INIT_DEC_WIDTH_NOT_SUPPPORTED;
                H264_DEC_DEBUG_PRINT("\n");
                return (IV_FAIL);
            }

            if(max_wd > H264_MAX_FRAME_WIDTH)
            {
                ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_init_op_t.u4_error_code |=
                                IVD_INIT_DEC_WIDTH_NOT_SUPPPORTED;
                H264_DEC_DEBUG_PRINT("\n");
                return (IV_FAIL);
            }

            if(max_ht < H264_MIN_FRAME_HEIGHT)
            {
                ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_init_op_t.u4_error_code |=
                                IVD_INIT_DEC_HEIGHT_NOT_SUPPPORTED;
                H264_DEC_DEBUG_PRINT("\n");
                return (IV_FAIL);
            }

            if((max_ht * max_wd)
                            > (H264_MAX_FRAME_HEIGHT * H264_MAX_FRAME_WIDTH))

            {
                ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_init_op_t.u4_error_code |=
                                IVD_INIT_DEC_HEIGHT_NOT_SUPPPORTED;
                H264_DEC_DEBUG_PRINT("\n");
                return (IV_FAIL);
            }

            if(NULL == ps_ip->s_ivd_init_ip_t.pv_mem_rec_location)
            {
                ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_init_op_t.u4_error_code |=
                                IVD_NUM_REC_NOT_SUFFICIENT;
                H264_DEC_DEBUG_PRINT("\n");
                return (IV_FAIL);
            }

            if((ps_ip->s_ivd_init_ip_t.e_output_format != IV_YUV_420P)
                            && (ps_ip->s_ivd_init_ip_t.e_output_format
                                            != IV_YUV_422ILE)
                            && (ps_ip->s_ivd_init_ip_t.e_output_format
                                            != IV_RGB_565)
                            && (ps_ip->s_ivd_init_ip_t.e_output_format
                                            != IV_YUV_420SP_UV)
                            && (ps_ip->s_ivd_init_ip_t.e_output_format
                                            != IV_YUV_420SP_VU))
            {
                ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_init_op_t.u4_error_code |=
                                IVD_INIT_DEC_COL_FMT_NOT_SUPPORTED;
                H264_DEC_DEBUG_PRINT("\n");
                return (IV_FAIL);
            }

            /* verify number of mem records */
            if(ps_ip->s_ivd_init_ip_t.u4_num_mem_rec < MEM_REC_CNT)
            {
                ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_init_op_t.u4_error_code |=
                                IVD_INIT_DEC_MEM_REC_NOT_SUFFICIENT;
                H264_DEC_DEBUG_PRINT("\n");
                return IV_FAIL;
            }

            ps_mem_rec = ps_ip->s_ivd_init_ip_t.pv_mem_rec_location;
            /* check memrecords sizes are correct */
            for(i = 0; i < ps_ip->s_ivd_init_ip_t.u4_num_mem_rec; i++)
            {
                if(ps_mem_rec[i].u4_size != sizeof(iv_mem_rec_t))
                {
                    ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                    << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |=
                                    IVD_MEM_REC_STRUCT_SIZE_INCORRECT;
                    H264_DEC_DEBUG_PRINT("i: %d\n", i);
                    return IV_FAIL;
                }
                /* check memrecords pointers are not NULL */

                if(ps_mem_rec[i].pv_base == NULL)
                {

                    ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                    << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_init_op_t.u4_error_code |=
                                    IVD_INIT_DEC_MEM_REC_BASE_NULL;
                    H264_DEC_DEBUG_PRINT("i: %d\n", i);
                    return IV_FAIL;

                }

            }

            /* verify memtabs for overlapping regions */
            {
                void *start[MEM_REC_CNT];
                void *end[MEM_REC_CNT];

                start[0] = (void *)(ps_mem_rec[0].pv_base);
                end[0] = (void *)((UWORD8 *)ps_mem_rec[0].pv_base
                                + ps_mem_rec[0].u4_mem_size - 1);
                for(i = 1; i < MEM_REC_CNT; i++)
                {
                    /* This array is populated to check memtab overlapp */
                    start[i] = (void *)(ps_mem_rec[i].pv_base);
                    end[i] = (void *)((UWORD8 *)ps_mem_rec[i].pv_base
                                    + ps_mem_rec[i].u4_mem_size - 1);

                    for(j = 0; j < i; j++)
                    {
                        if((start[i] >= start[j]) && (start[i] <= end[j]))
                        {
                            ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                            << IVD_UNSUPPORTEDPARAM;
                            ps_op->s_ivd_init_op_t.u4_error_code |=
                                            IVD_INIT_DEC_MEM_REC_OVERLAP_ERR;
                            H264_DEC_DEBUG_PRINT("i: %d, j: %d\n", i, j);
                            return IV_FAIL;
                        }

                        if((end[i] >= start[j]) && (end[i] <= end[j]))
                        {
                            ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                            << IVD_UNSUPPORTEDPARAM;
                            ps_op->s_ivd_init_op_t.u4_error_code |=
                                            IVD_INIT_DEC_MEM_REC_OVERLAP_ERR;
                            H264_DEC_DEBUG_PRINT("i: %d, j: %d\n", i, j);
                            return IV_FAIL;
                        }

                        if((start[i] < start[j]) && (end[i] > end[j]))
                        {
                            ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                            << IVD_UNSUPPORTEDPARAM;
                            ps_op->s_ivd_init_op_t.u4_error_code |=
                                            IVD_INIT_DEC_MEM_REC_OVERLAP_ERR;
                            H264_DEC_DEBUG_PRINT("i: %d, j: %d\n", i, j);
                            return IV_FAIL;
                        }
                    }

                }
            }

            {
                iv_mem_rec_t mem_rec_ittiam_api[MEM_REC_CNT];
                ih264d_fill_mem_rec_ip_t s_fill_mem_rec_ip;
                ih264d_fill_mem_rec_op_t s_fill_mem_rec_op;
                IV_API_CALL_STATUS_T e_status;

                UWORD32 i;
                s_fill_mem_rec_ip.s_ivd_fill_mem_rec_ip_t.e_cmd =
                                IV_CMD_FILL_NUM_MEM_REC;
                s_fill_mem_rec_ip.s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location =
                                mem_rec_ittiam_api;
                s_fill_mem_rec_ip.s_ivd_fill_mem_rec_ip_t.u4_max_frm_wd =
                                max_wd;
                s_fill_mem_rec_ip.s_ivd_fill_mem_rec_ip_t.u4_max_frm_ht =
                                max_ht;

                if(ps_ip->s_ivd_init_ip_t.u4_size
                                > offsetof(ih264d_init_ip_t, i4_level))
                {
                    s_fill_mem_rec_ip.i4_level = ps_ip->i4_level;
                }
                else
                {
                    s_fill_mem_rec_ip.i4_level = H264_LEVEL_3_1;
                }

                if(ps_ip->s_ivd_init_ip_t.u4_size
                                > offsetof(ih264d_init_ip_t, u4_num_ref_frames))
                {
                    s_fill_mem_rec_ip.u4_num_ref_frames =
                                    ps_ip->u4_num_ref_frames;
                }
                else
                {
                    s_fill_mem_rec_ip.u4_num_ref_frames =
                                    (H264_MAX_REF_PICS + 1);
                }

                if(ps_ip->s_ivd_init_ip_t.u4_size
                                > offsetof(ih264d_init_ip_t,
                                           u4_num_reorder_frames))
                {
                    s_fill_mem_rec_ip.u4_num_reorder_frames =
                                    ps_ip->u4_num_reorder_frames;
                }
                else
                {
                    s_fill_mem_rec_ip.u4_num_reorder_frames = (H264_MAX_REF_PICS
                                    + 1);
                }

                if(ps_ip->s_ivd_init_ip_t.u4_size
                                > offsetof(ih264d_init_ip_t,
                                           u4_num_extra_disp_buf))
                {
                    s_fill_mem_rec_ip.u4_num_extra_disp_buf =
                                    ps_ip->u4_num_extra_disp_buf;
                }
                else
                {
                    s_fill_mem_rec_ip.u4_num_extra_disp_buf = 0;
                }

                if(ps_ip->s_ivd_init_ip_t.u4_size
                                > offsetof(ih264d_init_ip_t, u4_share_disp_buf))
                {
#ifndef LOGO_EN
                    s_fill_mem_rec_ip.u4_share_disp_buf =
                                    ps_ip->u4_share_disp_buf;
#else
                    s_fill_mem_rec_ip.u4_share_disp_buf = 0;
#endif
                }
                else
                {
                    s_fill_mem_rec_ip.u4_share_disp_buf = 0;
                }

                s_fill_mem_rec_ip.e_output_format =
                                ps_ip->s_ivd_init_ip_t.e_output_format;

                if((s_fill_mem_rec_ip.e_output_format != IV_YUV_420P)
                                && (s_fill_mem_rec_ip.e_output_format
                                                != IV_YUV_420SP_UV)
                                && (s_fill_mem_rec_ip.e_output_format
                                                != IV_YUV_420SP_VU))
                {
                    s_fill_mem_rec_ip.u4_share_disp_buf = 0;
                }

                s_fill_mem_rec_ip.s_ivd_fill_mem_rec_ip_t.u4_size =
                                sizeof(ih264d_fill_mem_rec_ip_t);
                s_fill_mem_rec_op.s_ivd_fill_mem_rec_op_t.u4_size =
                                sizeof(ih264d_fill_mem_rec_op_t);

                for(i = 0; i < MEM_REC_CNT; i++)
                    mem_rec_ittiam_api[i].u4_size = sizeof(iv_mem_rec_t);

                e_status = ih264d_api_function(NULL,
                                                    (void *)&s_fill_mem_rec_ip,
                                                    (void *)&s_fill_mem_rec_op);
                if(IV_FAIL == e_status)
                {
                    ps_op->s_ivd_init_op_t.u4_error_code =
                                    s_fill_mem_rec_op.s_ivd_fill_mem_rec_op_t.u4_error_code;
                    H264_DEC_DEBUG_PRINT("Fail\n");
                    return (IV_FAIL);
                }

                for(i = 0; i < MEM_REC_CNT; i++)
                {
                    if(ps_mem_rec[i].u4_mem_size
                                    < mem_rec_ittiam_api[i].u4_mem_size)
                    {
                        ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_init_op_t.u4_error_code |=
                                        IVD_INIT_DEC_MEM_REC_INSUFFICIENT_SIZE;
                        H264_DEC_DEBUG_PRINT("i: %d \n", i);
                        return IV_FAIL;
                    }
                    if(ps_mem_rec[i].u4_mem_alignment
                                    != mem_rec_ittiam_api[i].u4_mem_alignment)
                    {
                        ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_init_op_t.u4_error_code |=
                                        IVD_INIT_DEC_MEM_REC_ALIGNMENT_ERR;
                        H264_DEC_DEBUG_PRINT("i: %d \n", i);
                        return IV_FAIL;
                    }
                    if(ps_mem_rec[i].e_mem_type
                                    != mem_rec_ittiam_api[i].e_mem_type)
                    {
                        UWORD32 check = IV_SUCCESS;
                        UWORD32 diff = mem_rec_ittiam_api[i].e_mem_type
                                        - ps_mem_rec[i].e_mem_type;

                        if((ps_mem_rec[i].e_mem_type
                                        <= IV_EXTERNAL_CACHEABLE_SCRATCH_MEM)
                                        && (mem_rec_ittiam_api[i].e_mem_type
                                                        >= IV_INTERNAL_NONCACHEABLE_PERSISTENT_MEM))
                        {
                            check = IV_FAIL;
                        }
                        if(3 != MOD(mem_rec_ittiam_api[i].e_mem_type, 4))
                        {
                            /*
                             * It is not IV_EXTERNAL_NONCACHEABLE_PERSISTENT_MEM or IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM
                             */
                            if((diff < 1) || (diff > 3))
                            {
                                // Difference between 1 and 3 is okay for all cases other than the two filtered
                                // with the MOD condition above
                                check = IV_FAIL;
                            }
                        }
                        else
                        {
                            if(diff == 1)
                            {
                                /*
                                 * This particular case is when codec asked for External Persistent, but got
                                 * Internal Scratch.
                                 */
                                check = IV_FAIL;
                            }
                            if((diff != 2) && (diff != 3))
                            {
                                check = IV_FAIL;
                            }
                        }
                        if(check == IV_FAIL)
                        {
                            ps_op->s_ivd_init_op_t.u4_error_code |= 1
                                            << IVD_UNSUPPORTEDPARAM;
                            ps_op->s_ivd_init_op_t.u4_error_code |=
                                            IVD_INIT_DEC_MEM_REC_INCORRECT_TYPE;
                            H264_DEC_DEBUG_PRINT("i: %d \n", i);
                            return IV_FAIL;
                        }
                    }
                }
            }

        }
            break;

        case IVD_CMD_GET_DISPLAY_FRAME:
        {
            ih264d_get_display_frame_ip_t *ps_ip =
                            (ih264d_get_display_frame_ip_t *)pv_api_ip;
            ih264d_get_display_frame_op_t *ps_op =
                            (ih264d_get_display_frame_op_t *)pv_api_op;

            ps_op->s_ivd_get_display_frame_op_t.u4_error_code = 0;

            if((ps_ip->s_ivd_get_display_frame_ip_t.u4_size
                            != sizeof(ih264d_get_display_frame_ip_t))
                            && (ps_ip->s_ivd_get_display_frame_ip_t.u4_size
                                            != sizeof(ivd_get_display_frame_ip_t)))
            {
                ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_get_display_frame_op_t.u4_error_code |=
                                IVD_IP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

            if((ps_op->s_ivd_get_display_frame_op_t.u4_size
                            != sizeof(ih264d_get_display_frame_op_t))
                            && (ps_op->s_ivd_get_display_frame_op_t.u4_size
                                            != sizeof(ivd_get_display_frame_op_t)))
            {
                ps_op->s_ivd_get_display_frame_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_get_display_frame_op_t.u4_error_code |=
                                IVD_OP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }
        }
            break;

        case IVD_CMD_REL_DISPLAY_FRAME:
        {
            ih264d_rel_display_frame_ip_t *ps_ip =
                            (ih264d_rel_display_frame_ip_t *)pv_api_ip;
            ih264d_rel_display_frame_op_t *ps_op =
                            (ih264d_rel_display_frame_op_t *)pv_api_op;

            ps_op->s_ivd_rel_display_frame_op_t.u4_error_code = 0;

            if((ps_ip->s_ivd_rel_display_frame_ip_t.u4_size
                            != sizeof(ih264d_rel_display_frame_ip_t))
                            && (ps_ip->s_ivd_rel_display_frame_ip_t.u4_size
                                            != sizeof(ivd_rel_display_frame_ip_t)))
            {
                ps_op->s_ivd_rel_display_frame_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_rel_display_frame_op_t.u4_error_code |=
                                IVD_IP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

            if((ps_op->s_ivd_rel_display_frame_op_t.u4_size
                            != sizeof(ih264d_rel_display_frame_op_t))
                            && (ps_op->s_ivd_rel_display_frame_op_t.u4_size
                                            != sizeof(ivd_rel_display_frame_op_t)))
            {
                ps_op->s_ivd_rel_display_frame_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_rel_display_frame_op_t.u4_error_code |=
                                IVD_OP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

        }
            break;

        case IVD_CMD_SET_DISPLAY_FRAME:
        {
            ih264d_set_display_frame_ip_t *ps_ip =
                            (ih264d_set_display_frame_ip_t *)pv_api_ip;
            ih264d_set_display_frame_op_t *ps_op =
                            (ih264d_set_display_frame_op_t *)pv_api_op;
            UWORD32 j;

            ps_op->s_ivd_set_display_frame_op_t.u4_error_code = 0;

            if((ps_ip->s_ivd_set_display_frame_ip_t.u4_size
                            != sizeof(ih264d_set_display_frame_ip_t))
                            && (ps_ip->s_ivd_set_display_frame_ip_t.u4_size
                                            != sizeof(ivd_set_display_frame_ip_t)))
            {
                ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_set_display_frame_op_t.u4_error_code |=
                                IVD_IP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

            if((ps_op->s_ivd_set_display_frame_op_t.u4_size
                            != sizeof(ih264d_set_display_frame_op_t))
                            && (ps_op->s_ivd_set_display_frame_op_t.u4_size
                                            != sizeof(ivd_set_display_frame_op_t)))
            {
                ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_set_display_frame_op_t.u4_error_code |=
                                IVD_OP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

            if(ps_ip->s_ivd_set_display_frame_ip_t.num_disp_bufs == 0)
            {
                ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_set_display_frame_op_t.u4_error_code |=
                                IVD_DISP_FRM_ZERO_OP_BUFS;
                return IV_FAIL;
            }

            for(j = 0; j < ps_ip->s_ivd_set_display_frame_ip_t.num_disp_bufs;
                            j++)
            {
                if(ps_ip->s_ivd_set_display_frame_ip_t.s_disp_buffer[j].u4_num_bufs
                                == 0)
                {
                    ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1
                                    << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_set_display_frame_op_t.u4_error_code |=
                                    IVD_DISP_FRM_ZERO_OP_BUFS;
                    return IV_FAIL;
                }

                for(i = 0;
                                i
                                                < ps_ip->s_ivd_set_display_frame_ip_t.s_disp_buffer[j].u4_num_bufs;
                                i++)
                {
                    if(ps_ip->s_ivd_set_display_frame_ip_t.s_disp_buffer[j].pu1_bufs[i]
                                    == NULL)
                    {
                        ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_set_display_frame_op_t.u4_error_code |=
                                        IVD_DISP_FRM_OP_BUF_NULL;
                        return IV_FAIL;
                    }

                    if(ps_ip->s_ivd_set_display_frame_ip_t.s_disp_buffer[j].u4_min_out_buf_size[i]
                                    == 0)
                    {
                        ps_op->s_ivd_set_display_frame_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_set_display_frame_op_t.u4_error_code |=
                                        IVD_DISP_FRM_ZERO_OP_BUF_SIZE;
                        return IV_FAIL;
                    }
                }
            }
        }
            break;

        case IVD_CMD_VIDEO_DECODE:
        {
            ih264d_video_decode_ip_t *ps_ip =
                            (ih264d_video_decode_ip_t *)pv_api_ip;
            ih264d_video_decode_op_t *ps_op =
                            (ih264d_video_decode_op_t *)pv_api_op;

            H264_DEC_DEBUG_PRINT("The input bytes is: %d",
                                 ps_ip->s_ivd_video_decode_ip_t.u4_num_Bytes);
            ps_op->s_ivd_video_decode_op_t.u4_error_code = 0;

            if(ps_ip->s_ivd_video_decode_ip_t.u4_size
                            != sizeof(ih264d_video_decode_ip_t)&&
                            ps_ip->s_ivd_video_decode_ip_t.u4_size != offsetof(ivd_video_decode_ip_t, s_out_buffer))
            {
                ps_op->s_ivd_video_decode_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_video_decode_op_t.u4_error_code |=
                                IVD_IP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

            if(ps_op->s_ivd_video_decode_op_t.u4_size
                            != sizeof(ih264d_video_decode_op_t)&&
                            ps_op->s_ivd_video_decode_op_t.u4_size != offsetof(ivd_video_decode_op_t, u4_output_present))
            {
                ps_op->s_ivd_video_decode_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_video_decode_op_t.u4_error_code |=
                                IVD_OP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

        }
            break;

        case IV_CMD_RETRIEVE_MEMREC:
        {
            ih264d_retrieve_mem_rec_ip_t *ps_ip =
                            (ih264d_retrieve_mem_rec_ip_t *)pv_api_ip;
            ih264d_retrieve_mem_rec_op_t *ps_op =
                            (ih264d_retrieve_mem_rec_op_t *)pv_api_op;
            iv_mem_rec_t *ps_mem_rec;

            ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code = 0;

            if(ps_ip->s_ivd_retrieve_mem_rec_ip_t.u4_size
                            != sizeof(ih264d_retrieve_mem_rec_ip_t))
            {
                ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |=
                                IVD_IP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

            if(ps_op->s_ivd_retrieve_mem_rec_op_t.u4_size
                            != sizeof(ih264d_retrieve_mem_rec_op_t))
            {
                ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |= 1
                                << IVD_UNSUPPORTEDPARAM;
                ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |=
                                IVD_OP_API_STRUCT_SIZE_INCORRECT;
                return (IV_FAIL);
            }

            ps_mem_rec = ps_ip->s_ivd_retrieve_mem_rec_ip_t.pv_mem_rec_location;
            /* check memrecords sizes are correct */
            for(i = 0; i < MEM_REC_CNT; i++)
            {
                if(ps_mem_rec[i].u4_size != sizeof(iv_mem_rec_t))
                {
                    ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |= 1
                                    << IVD_UNSUPPORTEDPARAM;
                    ps_op->s_ivd_retrieve_mem_rec_op_t.u4_error_code |=
                                    IVD_MEM_REC_STRUCT_SIZE_INCORRECT;
                    return IV_FAIL;
                }
            }
        }
            break;

        case IVD_CMD_VIDEO_CTL:
        {
            UWORD32 *pu4_ptr_cmd;
            UWORD32 sub_command;

            pu4_ptr_cmd = (UWORD32 *)pv_api_ip;
            pu4_ptr_cmd += 2;
            sub_command = *pu4_ptr_cmd;

            switch(sub_command)
            {
                case IVD_CMD_CTL_SETPARAMS:
                {
                    ih264d_ctl_set_config_ip_t *ps_ip;
                    ih264d_ctl_set_config_op_t *ps_op;
                    ps_ip = (ih264d_ctl_set_config_ip_t *)pv_api_ip;
                    ps_op = (ih264d_ctl_set_config_op_t *)pv_api_op;

                    if(ps_ip->s_ivd_ctl_set_config_ip_t.u4_size
                                    != sizeof(ih264d_ctl_set_config_ip_t))
                    {
                        ps_op->s_ivd_ctl_set_config_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_set_config_op_t.u4_error_code |=
                                        IVD_IP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                }
                    //no break; is needed here
                case IVD_CMD_CTL_SETDEFAULT:
                {
                    ih264d_ctl_set_config_op_t *ps_op;
                    ps_op = (ih264d_ctl_set_config_op_t *)pv_api_op;
                    if(ps_op->s_ivd_ctl_set_config_op_t.u4_size
                                    != sizeof(ih264d_ctl_set_config_op_t))
                    {
                        ps_op->s_ivd_ctl_set_config_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_set_config_op_t.u4_error_code |=
                                        IVD_OP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                }
                    break;

                case IVD_CMD_CTL_GETPARAMS:
                {
                    ih264d_ctl_getstatus_ip_t *ps_ip;
                    ih264d_ctl_getstatus_op_t *ps_op;

                    ps_ip = (ih264d_ctl_getstatus_ip_t *)pv_api_ip;
                    ps_op = (ih264d_ctl_getstatus_op_t *)pv_api_op;
                    if(ps_ip->s_ivd_ctl_getstatus_ip_t.u4_size
                                    != sizeof(ih264d_ctl_getstatus_ip_t))
                    {
                        ps_op->s_ivd_ctl_getstatus_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_getstatus_op_t.u4_error_code |=
                                        IVD_IP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                    if(ps_op->s_ivd_ctl_getstatus_op_t.u4_size
                                    != sizeof(ih264d_ctl_getstatus_op_t))
                    {
                        ps_op->s_ivd_ctl_getstatus_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_getstatus_op_t.u4_error_code |=
                                        IVD_OP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                }
                    break;

                case IVD_CMD_CTL_GETBUFINFO:
                {
                    ih264d_ctl_getbufinfo_ip_t *ps_ip;
                    ih264d_ctl_getbufinfo_op_t *ps_op;
                    ps_ip = (ih264d_ctl_getbufinfo_ip_t *)pv_api_ip;
                    ps_op = (ih264d_ctl_getbufinfo_op_t *)pv_api_op;

                    if(ps_ip->s_ivd_ctl_getbufinfo_ip_t.u4_size
                                    != sizeof(ih264d_ctl_getbufinfo_ip_t))
                    {
                        ps_op->s_ivd_ctl_getbufinfo_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_getbufinfo_op_t.u4_error_code |=
                                        IVD_IP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                    if(ps_op->s_ivd_ctl_getbufinfo_op_t.u4_size
                                    != sizeof(ih264d_ctl_getbufinfo_op_t))
                    {
                        ps_op->s_ivd_ctl_getbufinfo_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_getbufinfo_op_t.u4_error_code |=
                                        IVD_OP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                }
                    break;

                case IVD_CMD_CTL_GETVERSION:
                {
                    ih264d_ctl_getversioninfo_ip_t *ps_ip;
                    ih264d_ctl_getversioninfo_op_t *ps_op;
                    ps_ip = (ih264d_ctl_getversioninfo_ip_t *)pv_api_ip;
                    ps_op = (ih264d_ctl_getversioninfo_op_t *)pv_api_op;
                    if(ps_ip->s_ivd_ctl_getversioninfo_ip_t.u4_size
                                    != sizeof(ih264d_ctl_getversioninfo_ip_t))
                    {
                        ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code |=
                                        IVD_IP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                    if(ps_op->s_ivd_ctl_getversioninfo_op_t.u4_size
                                    != sizeof(ih264d_ctl_getversioninfo_op_t))
                    {
                        ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_getversioninfo_op_t.u4_error_code |=
                                        IVD_OP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                }
                    break;

                case IVD_CMD_CTL_FLUSH:
                {
                    ih264d_ctl_flush_ip_t *ps_ip;
                    ih264d_ctl_flush_op_t *ps_op;
                    ps_ip = (ih264d_ctl_flush_ip_t *)pv_api_ip;
                    ps_op = (ih264d_ctl_flush_op_t *)pv_api_op;
                    if(ps_ip->s_ivd_ctl_flush_ip_t.u4_size
                                    != sizeof(ih264d_ctl_flush_ip_t))
                    {
                        ps_op->s_ivd_ctl_flush_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_flush_op_t.u4_error_code |=
                                        IVD_IP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                    if(ps_op->s_ivd_ctl_flush_op_t.u4_size
                                    != sizeof(ih264d_ctl_flush_op_t))
                    {
                        ps_op->s_ivd_ctl_flush_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_flush_op_t.u4_error_code |=
                                        IVD_OP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                }
                    break;

                case IVD_CMD_CTL_RESET:
                {
                    ih264d_ctl_reset_ip_t *ps_ip;
                    ih264d_ctl_reset_op_t *ps_op;
                    ps_ip = (ih264d_ctl_reset_ip_t *)pv_api_ip;
                    ps_op = (ih264d_ctl_reset_op_t *)pv_api_op;
                    if(ps_ip->s_ivd_ctl_reset_ip_t.u4_size
                                    != sizeof(ih264d_ctl_reset_ip_t))
                    {
                        ps_op->s_ivd_ctl_reset_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_reset_op_t.u4_error_code |=
                                        IVD_IP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                    if(ps_op->s_ivd_ctl_reset_op_t.u4_size
                                    != sizeof(ih264d_ctl_reset_op_t))
                    {
                        ps_op->s_ivd_ctl_reset_op_t.u4_error_code |= 1
                                        << IVD_UNSUPPORTEDPARAM;
                        ps_op->s_ivd_ctl_reset_op_t.u4_error_code |=
                                        IVD_OP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }
                }
                    break;

                case IH264D_CMD_CTL_DEGRADE:
                {
                    ih264d_ctl_degrade_ip_t *ps_ip;
                    ih264d_ctl_degrade_op_t *ps_op;

                    ps_ip = (ih264d_ctl_degrade_ip_t *)pv_api_ip;
                    ps_op = (ih264d_ctl_degrade_op_t *)pv_api_op;

                    if(ps_ip->u4_size != sizeof(ih264d_ctl_degrade_ip_t))
                    {
                        ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->u4_error_code |=
                                        IVD_IP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }

                    if(ps_op->u4_size != sizeof(ih264d_ctl_degrade_op_t))
                    {
                        ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->u4_error_code |=
                                        IVD_OP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }

                    if((ps_ip->i4_degrade_pics < 0)
                                    || (ps_ip->i4_degrade_pics > 4)
                                    || (ps_ip->i4_nondegrade_interval < 0)
                                    || (ps_ip->i4_degrade_type < 0)
                                    || (ps_ip->i4_degrade_type > 15))
                    {
                        ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        return IV_FAIL;
                    }

                    break;
                }

                case IH264D_CMD_CTL_GET_BUFFER_DIMENSIONS:
                {
                    ih264d_ctl_get_frame_dimensions_ip_t *ps_ip;
                    ih264d_ctl_get_frame_dimensions_op_t *ps_op;

                    ps_ip = (ih264d_ctl_get_frame_dimensions_ip_t *)pv_api_ip;
                    ps_op = (ih264d_ctl_get_frame_dimensions_op_t *)pv_api_op;

                    if(ps_ip->u4_size
                                    != sizeof(ih264d_ctl_get_frame_dimensions_ip_t))
                    {
                        ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->u4_error_code |=
                                        IVD_IP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }

                    if(ps_op->u4_size
                                    != sizeof(ih264d_ctl_get_frame_dimensions_op_t))
                    {
                        ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->u4_error_code |=
                                        IVD_OP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }

                    break;
                }

                case IH264D_CMD_CTL_SET_NUM_CORES:
                {
                    ih264d_ctl_set_num_cores_ip_t *ps_ip;
                    ih264d_ctl_set_num_cores_op_t *ps_op;

                    ps_ip = (ih264d_ctl_set_num_cores_ip_t *)pv_api_ip;
                    ps_op = (ih264d_ctl_set_num_cores_op_t *)pv_api_op;

                    if(ps_ip->u4_size != sizeof(ih264d_ctl_set_num_cores_ip_t))
                    {
                        ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->u4_error_code |=
                                        IVD_IP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }

                    if(ps_op->u4_size != sizeof(ih264d_ctl_set_num_cores_op_t))
                    {
                        ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->u4_error_code |=
                                        IVD_OP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }

                    if((ps_ip->u4_num_cores != 1) && (ps_ip->u4_num_cores != 2)
                                    && (ps_ip->u4_num_cores != 3)
                                    && (ps_ip->u4_num_cores != 4))
                    {
                        ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        return IV_FAIL;
                    }
                    break;
                }
                case IH264D_CMD_CTL_SET_PROCESSOR:
                {
                    ih264d_ctl_set_processor_ip_t *ps_ip;
                    ih264d_ctl_set_processor_op_t *ps_op;

                    ps_ip = (ih264d_ctl_set_processor_ip_t *)pv_api_ip;
                    ps_op = (ih264d_ctl_set_processor_op_t *)pv_api_op;

                    if(ps_ip->u4_size != sizeof(ih264d_ctl_set_processor_ip_t))
                    {
                        ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->u4_error_code |=
                                        IVD_IP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }

                    if(ps_op->u4_size != sizeof(ih264d_ctl_set_processor_op_t))
                    {
                        ps_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                        ps_op->u4_error_code |=
                                        IVD_OP_API_STRUCT_SIZE_INCORRECT;
                        return IV_FAIL;
                    }

                    break;
                }
                default:
                    *(pu4_api_op + 1) |= 1 << IVD_UNSUPPORTEDPARAM;
                    *(pu4_api_op + 1) |= IVD_UNSUPPORTED_API_CMD;
                    return IV_FAIL;
                    break;
            }
        }
            break;
    }

    return IV_SUCCESS;
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

WORD32 ih264d_set_processor(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{
    ih264d_ctl_set_processor_ip_t *ps_ip;
    ih264d_ctl_set_processor_op_t *ps_op;
    dec_struct_t *ps_codec = (dec_struct_t *)dec_hdl->pv_codec_handle;

    ps_ip = (ih264d_ctl_set_processor_ip_t *)pv_api_ip;
    ps_op = (ih264d_ctl_set_processor_op_t *)pv_api_op;

    ps_codec->e_processor_arch = (IVD_ARCH_T)ps_ip->u4_arch;
    ps_codec->e_processor_soc = (IVD_SOC_T)ps_ip->u4_soc;

    ih264d_init_function_ptr(ps_codec);

    ps_op->u4_error_code = 0;
    return IV_SUCCESS;
}
/*****************************************************************************/
/*                                                                           */
/*  Function Name :  ih264d_get_num_rec                                      */
/*                                                                           */
/*  Description   : returns number of mem records required                   */
/*                                                                           */
/*  Inputs        : pv_api_ip input api structure                            */
/*                : pv_api_op output api structure                           */
/*  Outputs       :                                                          */
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
WORD32 ih264d_get_num_rec(void *pv_api_ip, void *pv_api_op)
{
    iv_num_mem_rec_ip_t *ps_mem_q_ip;
    iv_num_mem_rec_op_t *ps_mem_q_op;
    ps_mem_q_ip = (iv_num_mem_rec_ip_t *)pv_api_ip;
    ps_mem_q_op = (iv_num_mem_rec_op_t *)pv_api_op;
    UNUSED(ps_mem_q_ip);
    ps_mem_q_op->u4_num_mem_rec = MEM_REC_CNT;

    return IV_SUCCESS;

}


/**************************************************************************
 * \if Function name : ih264d_init_decoder \endif
 *
 *
 * \brief
 *    Initializes the decoder
 *
 * \param apiVersion               : Version of the api being used.
 * \param errorHandlingMechanism   : Mechanism to be used for errror handling.
 * \param postFilteringType: Type of post filtering operation to be used.
 * \param uc_outputFormat: Format of the decoded picture [default 4:2:0].
 * \param uc_dispBufs: Number of Display Buffers.
 * \param p_NALBufAPI: Pointer to NAL Buffer API.
 * \param p_DispBufAPI: Pointer to Display Buffer API.
 * \param ih264d_dec_mem_manager  :Pointer to the function that will be called by decoder
 *                        for memory allocation and freeing.
 *
 * \return
 *    0 on Success and -1 on error
 *
 **************************************************************************
 */
void ih264d_init_decoder(void * ps_dec_params)
{
    dec_struct_t * ps_dec = (dec_struct_t *)ps_dec_params;
    dec_slice_params_t *ps_cur_slice;
    pocstruct_t *ps_prev_poc, *ps_cur_poc;
    WORD32 size;

    size = sizeof(dec_err_status_t);
    memset(ps_dec->ps_dec_err_status, 0, size);

    size = sizeof(sei);
    memset(ps_dec->ps_sei, 0, size);

    size = sizeof(dpb_commands_t);
    memset(ps_dec->ps_dpb_cmds, 0, size);

    size = sizeof(dec_bit_stream_t);
    memset(ps_dec->ps_bitstrm, 0, size);

    size = sizeof(dec_slice_params_t);
    memset(ps_dec->ps_cur_slice, 0, size);

    size = MAX(sizeof(dec_seq_params_t), sizeof(dec_pic_params_t));
    memset(ps_dec->pv_scratch_sps_pps, 0, size);



    /* Set pic_parameter_set_id to -1 */



    ps_cur_slice = ps_dec->ps_cur_slice;
    ps_dec->init_done = 0;

    ps_dec->u4_num_cores = 1;

    ps_dec->u2_pic_ht = ps_dec->u2_pic_wd = 0;

    ps_dec->u1_separate_parse = DEFAULT_SEPARATE_PARSE;
    ps_dec->u4_app_disable_deblk_frm = 0;
    ps_dec->i4_degrade_type = 0;
    ps_dec->i4_degrade_pics = 0;

    ps_dec->i4_app_skip_mode = IVD_SKIP_NONE;
    ps_dec->i4_dec_skip_mode = IVD_SKIP_NONE;

    memset(ps_dec->ps_pps, 0,
           ((sizeof(dec_pic_params_t)) * MAX_NUM_PIC_PARAMS));
    memset(ps_dec->ps_sps, 0,
           ((sizeof(dec_seq_params_t)) * MAX_NUM_SEQ_PARAMS));

    /* Initialization of function pointers ih264d_deblock_picture function*/

    ps_dec->p_DeblockPicture[0] = ih264d_deblock_picture_non_mbaff;
    ps_dec->p_DeblockPicture[1] = ih264d_deblock_picture_mbaff;

    ps_dec->s_cab_dec_env.pv_codec_handle = ps_dec;

    ps_dec->u4_num_fld_in_frm = 0;

    ps_dec->ps_dpb_mgr->pv_codec_handle = ps_dec;

    /* Initialize the sei validity u4_flag with zero indiacting sei is not valid*/
    ps_dec->ps_sei->u1_is_valid = 0;

    /* decParams Initializations */
    ps_dec->ps_cur_pps = NULL;
    ps_dec->ps_cur_sps = NULL;
    ps_dec->u1_init_dec_flag = 0;
    ps_dec->u1_first_slice_in_stream = 1;
    ps_dec->u1_first_pb_nal_in_pic = 1;
    ps_dec->u1_last_pic_not_decoded = 0;
    ps_dec->u4_app_disp_width = 0;
    ps_dec->i4_header_decoded = 0;
    ps_dec->u4_total_frames_decoded = 0;

    ps_dec->i4_error_code = 0;
    ps_dec->i4_content_type = -1;
    ps_dec->ps_cur_slice->u1_mbaff_frame_flag = 0;

    ps_dec->ps_dec_err_status->u1_err_flag = ACCEPT_ALL_PICS; //REJECT_PB_PICS;
    ps_dec->ps_dec_err_status->u1_cur_pic_type = PIC_TYPE_UNKNOWN;
    ps_dec->ps_dec_err_status->u4_frm_sei_sync = SYNC_FRM_DEFAULT;
    ps_dec->ps_dec_err_status->u4_cur_frm = INIT_FRAME;
    ps_dec->ps_dec_err_status->u1_pic_aud_i = PIC_TYPE_UNKNOWN;

    ps_dec->u1_pr_sl_type = 0xFF;
    ps_dec->u2_mbx = 0xffff;
    ps_dec->u2_mby = 0;
    ps_dec->u2_total_mbs_coded = 0;

    /* POC initializations */
    ps_prev_poc = &ps_dec->s_prev_pic_poc;
    ps_cur_poc = &ps_dec->s_cur_pic_poc;
    ps_prev_poc->i4_pic_order_cnt_lsb = ps_cur_poc->i4_pic_order_cnt_lsb = 0;
    ps_prev_poc->i4_pic_order_cnt_msb = ps_cur_poc->i4_pic_order_cnt_msb = 0;
    ps_prev_poc->i4_delta_pic_order_cnt_bottom =
                    ps_cur_poc->i4_delta_pic_order_cnt_bottom = 0;
    ps_prev_poc->i4_delta_pic_order_cnt[0] =
                    ps_cur_poc->i4_delta_pic_order_cnt[0] = 0;
    ps_prev_poc->i4_delta_pic_order_cnt[1] =
                    ps_cur_poc->i4_delta_pic_order_cnt[1] = 0;
    ps_prev_poc->u1_mmco_equalto5 = ps_cur_poc->u1_mmco_equalto5 = 0;
    ps_prev_poc->i4_top_field_order_count = ps_cur_poc->i4_top_field_order_count =
                    0;
    ps_prev_poc->i4_bottom_field_order_count =
                    ps_cur_poc->i4_bottom_field_order_count = 0;
    ps_prev_poc->u1_bot_field = ps_cur_poc->u1_bot_field = 0;
    ps_prev_poc->u1_mmco_equalto5 = ps_cur_poc->u1_mmco_equalto5 = 0;
    ps_prev_poc->i4_prev_frame_num_ofst = ps_cur_poc->i4_prev_frame_num_ofst = 0;
    ps_cur_slice->u1_mmco_equalto5 = 0;
    ps_cur_slice->u2_frame_num = 0;

    ps_dec->i4_max_poc = 0;
    ps_dec->i4_prev_max_display_seq = 0;
    ps_dec->u1_recon_mb_grp = 4;

    /* Field PIC initializations */
    ps_dec->u1_second_field = 0;
    ps_dec->s_prev_seq_params.u1_eoseq_pending = 0;

    /* Set the cropping parameters as zero */
    ps_dec->u2_crop_offset_y = 0;
    ps_dec->u2_crop_offset_uv = 0;

    /* The Initial Frame Rate Info is not Present */
    ps_dec->i4_vui_frame_rate = -1;
    ps_dec->i4_pic_type = -1;
    ps_dec->i4_frametype = -1;
    ps_dec->i4_content_type = -1;

    ps_dec->u1_res_changed = 0;


    ps_dec->u1_frame_decoded_flag = 0;

    /* Set the default frame seek mask mode */
    ps_dec->u4_skip_frm_mask = SKIP_NONE;

    /********************************************************/
    /* Initialize CAVLC residual decoding function pointers */
    /********************************************************/
    ps_dec->pf_cavlc_4x4res_block[0] = ih264d_cavlc_4x4res_block_totalcoeff_1;
    ps_dec->pf_cavlc_4x4res_block[1] =
                    ih264d_cavlc_4x4res_block_totalcoeff_2to10;
    ps_dec->pf_cavlc_4x4res_block[2] =
                    ih264d_cavlc_4x4res_block_totalcoeff_11to16;

    ps_dec->pf_cavlc_parse4x4coeff[0] = ih264d_cavlc_parse4x4coeff_n0to7;
    ps_dec->pf_cavlc_parse4x4coeff[1] = ih264d_cavlc_parse4x4coeff_n8;

    ps_dec->pf_cavlc_parse_8x8block[0] =
                    ih264d_cavlc_parse_8x8block_none_available;
    ps_dec->pf_cavlc_parse_8x8block[1] =
                    ih264d_cavlc_parse_8x8block_left_available;
    ps_dec->pf_cavlc_parse_8x8block[2] =
                    ih264d_cavlc_parse_8x8block_top_available;
    ps_dec->pf_cavlc_parse_8x8block[3] =
                    ih264d_cavlc_parse_8x8block_both_available;

    /***************************************************************************/
    /* Initialize Bs calculation function pointers for P and B, 16x16/non16x16 */
    /***************************************************************************/
    ps_dec->pf_fill_bs1[0][0] = ih264d_fill_bs1_16x16mb_pslice;
    ps_dec->pf_fill_bs1[0][1] = ih264d_fill_bs1_non16x16mb_pslice;

    ps_dec->pf_fill_bs1[1][0] = ih264d_fill_bs1_16x16mb_bslice;
    ps_dec->pf_fill_bs1[1][1] = ih264d_fill_bs1_non16x16mb_bslice;

    ps_dec->pf_fill_bs_xtra_left_edge[0] =
                    ih264d_fill_bs_xtra_left_edge_cur_frm;
    ps_dec->pf_fill_bs_xtra_left_edge[1] =
                    ih264d_fill_bs_xtra_left_edge_cur_fld;

    /* Initialize Reference Pic Buffers */
    ih264d_init_ref_bufs(ps_dec->ps_dpb_mgr);

#if VERT_SCALE_UP_AND_422
    ps_dec->u1_vert_up_scale_flag = 1;
#else
    ps_dec->u1_vert_up_scale_flag = 0;
#endif

    ps_dec->u2_prv_frame_num = 0;
    ps_dec->u1_top_bottom_decoded = 0;
    ps_dec->u1_dangling_field = 0;

    ps_dec->s_cab_dec_env.cabac_table = gau4_ih264d_cabac_table;

    ps_dec->pu1_left_mv_ctxt_inc = ps_dec->u1_left_mv_ctxt_inc_arr[0];
    ps_dec->pi1_left_ref_idx_ctxt_inc =
                    &ps_dec->i1_left_ref_idx_ctx_inc_arr[0][0];
    ps_dec->pu1_left_yuv_dc_csbp = &ps_dec->u1_yuv_dc_csbp_topmb;

    /* ! */
    /* Initializing flush frame u4_flag */
    ps_dec->u1_flushfrm = 0;

    {
        ps_dec->s_cab_dec_env.pv_codec_handle = (void*)ps_dec;
        ps_dec->ps_bitstrm->pv_codec_handle = (void*)ps_dec;
        ps_dec->ps_cur_slice->pv_codec_handle = (void*)ps_dec;
        ps_dec->ps_dpb_mgr->pv_codec_handle = (void*)ps_dec;
    }

    memset(ps_dec->disp_bufs, 0, (MAX_DISP_BUFS_NEW) * sizeof(disp_buf_t));
    memset(ps_dec->u4_disp_buf_mapping, 0,
           (MAX_DISP_BUFS_NEW) * sizeof(UWORD32));
    memset(ps_dec->u4_disp_buf_to_be_freed, 0,
           (MAX_DISP_BUFS_NEW) * sizeof(UWORD32));
    memset(ps_dec->ps_cur_slice, 0, sizeof(dec_slice_params_t));

    ih264d_init_arch(ps_dec);
    ih264d_init_function_ptr(ps_dec);

    ps_dec->init_done = 1;
    ps_dec->process_called = 1;

    ps_dec->pv_pic_buf_mgr = NULL;
    ps_dec->pv_mv_buf_mgr = NULL;
}

/**************************************************************************
 * \if Function name : ih264d_init_video_decoder \endif
 *
 * \brief
 *    Wrapper for the decoder init
 *
 * \param p_NALBufAPI: Pointer to NAL Buffer API.
 * \param ih264d_dec_mem_manager  :Pointer to the function that will be called by decoder
 *                        for memory allocation and freeing.
 *
 * \return
 *    pointer to the decparams
 *
 **************************************************************************
 */

WORD32 ih264d_init_video_decoder(iv_obj_t *dec_hdl,
                                 ih264d_init_ip_t *ps_init_ip,
                                 ih264d_init_op_t *ps_init_op)
{
    dec_struct_t * ps_dec;
    iv_mem_rec_t *memtab;
    UWORD8 *pu1_extra_mem_base,*pu1_mem_base;

    memtab = ps_init_ip->s_ivd_init_ip_t.pv_mem_rec_location;

    dec_hdl->pv_codec_handle = memtab[MEM_REC_CODEC].pv_base;
    ps_dec = dec_hdl->pv_codec_handle;

    memset(ps_dec, 0, sizeof(dec_struct_t));

    if(ps_init_ip->s_ivd_init_ip_t.u4_size
                    > offsetof(ih264d_init_ip_t, i4_level))
    {
        ps_dec->u4_level_at_init = ps_init_ip->i4_level;
    }
    else
    {
        ps_dec->u4_level_at_init = H264_LEVEL_3_1;
    }

    if(ps_init_ip->s_ivd_init_ip_t.u4_size
                    > offsetof(ih264d_init_ip_t, u4_num_ref_frames))
    {
        ps_dec->u4_num_ref_frames_at_init = ps_init_ip->u4_num_ref_frames;
    }
    else
    {
        ps_dec->u4_num_ref_frames_at_init = H264_MAX_REF_PICS;
    }

    if(ps_init_ip->s_ivd_init_ip_t.u4_size
                    > offsetof(ih264d_init_ip_t, u4_num_reorder_frames))
    {
        ps_dec->u4_num_reorder_frames_at_init =
                        ps_init_ip->u4_num_reorder_frames;
    }
    else
    {
        ps_dec->u4_num_reorder_frames_at_init = H264_MAX_REF_PICS;
    }

    if(ps_init_ip->s_ivd_init_ip_t.u4_size
                    > offsetof(ih264d_init_ip_t, u4_num_extra_disp_buf))
    {
        ps_dec->u4_num_extra_disp_bufs_at_init =
                        ps_init_ip->u4_num_extra_disp_buf;
    }
    else
    {
        ps_dec->u4_num_extra_disp_bufs_at_init = 0;
    }

    if(ps_init_ip->s_ivd_init_ip_t.u4_size
                    > offsetof(ih264d_init_ip_t, u4_share_disp_buf))
    {
#ifndef LOGO_EN
        ps_dec->u4_share_disp_buf = ps_init_ip->u4_share_disp_buf;
#else
        ps_dec->u4_share_disp_buf = 0;
#endif
    }
    else
    {
        ps_dec->u4_share_disp_buf = 0;
    }

    if((ps_init_ip->s_ivd_init_ip_t.e_output_format != IV_YUV_420P)
                    && (ps_init_ip->s_ivd_init_ip_t.e_output_format
                                    != IV_YUV_420SP_UV)
                    && (ps_init_ip->s_ivd_init_ip_t.e_output_format
                                    != IV_YUV_420SP_VU))
    {
        ps_dec->u4_share_disp_buf = 0;
    }

    if((ps_dec->u4_level_at_init < MIN_LEVEL_SUPPORTED)
                    || (ps_dec->u4_level_at_init > MAX_LEVEL_SUPPORTED))
    {
        ps_init_op->s_ivd_init_op_t.u4_error_code |= ERROR_LEVEL_UNSUPPORTED;
        return (IV_FAIL);
    }

    if(ps_dec->u4_num_ref_frames_at_init > H264_MAX_REF_PICS)
    {
        ps_init_op->s_ivd_init_op_t.u4_error_code |= ERROR_NUM_REF;
        ps_dec->u4_num_ref_frames_at_init = H264_MAX_REF_PICS;
    }

    if(ps_dec->u4_num_reorder_frames_at_init > H264_MAX_REF_PICS)
    {
        ps_init_op->s_ivd_init_op_t.u4_error_code |= ERROR_NUM_REF;
        ps_dec->u4_num_reorder_frames_at_init = H264_MAX_REF_PICS;
    }

    if(ps_dec->u4_num_extra_disp_bufs_at_init > H264_MAX_REF_PICS)
    {
        ps_init_op->s_ivd_init_op_t.u4_error_code |= ERROR_NUM_REF;
        ps_dec->u4_num_extra_disp_bufs_at_init = 0;
    }

    if(0 == ps_dec->u4_share_disp_buf)
        ps_dec->u4_num_extra_disp_bufs_at_init = 0;

    ps_dec->u4_num_disp_bufs_requested = 1;

    ps_dec->u4_width_at_init = ps_init_ip->s_ivd_init_ip_t.u4_frm_max_wd;
    ps_dec->u4_height_at_init = ps_init_ip->s_ivd_init_ip_t.u4_frm_max_ht;

    ps_dec->u4_width_at_init = ALIGN16(ps_dec->u4_width_at_init);
    ps_dec->u4_height_at_init = ALIGN32(ps_dec->u4_height_at_init);

    ps_dec->pv_dec_thread_handle = memtab[MEM_REC_THREAD_HANDLE].pv_base;

    pu1_mem_base = memtab[MEM_REC_THREAD_HANDLE].pv_base;
    ps_dec->pv_bs_deblk_thread_handle = pu1_mem_base
                    + ithread_get_handle_size();

    ps_dec->u4_extra_mem_used = 0;

    pu1_extra_mem_base = memtab[MEM_REC_EXTRA_MEM].pv_base;

    ps_dec->ps_dec_err_status = (dec_err_status_t *)(pu1_extra_mem_base + ps_dec->u4_extra_mem_used);
    ps_dec->u4_extra_mem_used += (((sizeof(dec_err_status_t) + 127) >> 7) << 7);

    ps_dec->ps_mem_tab = memtab[MEM_REC_BACKUP].pv_base;

    memcpy(ps_dec->ps_mem_tab, memtab, sizeof(iv_mem_rec_t) * MEM_REC_CNT);

    ps_dec->ps_pps = memtab[MEM_REC_PPS].pv_base;

    ps_dec->ps_sps = memtab[MEM_REC_SPS].pv_base;

    ps_dec->ps_sei = (sei *)(pu1_extra_mem_base + ps_dec->u4_extra_mem_used);
    ps_dec->u4_extra_mem_used += sizeof(sei);

    ps_dec->ps_dpb_mgr = memtab[MEM_REC_DPB_MGR].pv_base;

    ps_dec->ps_dpb_cmds = (dpb_commands_t *)(pu1_extra_mem_base + ps_dec->u4_extra_mem_used);
    ps_dec->u4_extra_mem_used += sizeof(dpb_commands_t);

    ps_dec->ps_bitstrm = (dec_bit_stream_t *)(pu1_extra_mem_base + ps_dec->u4_extra_mem_used);
    ps_dec->u4_extra_mem_used += sizeof(dec_bit_stream_t);

    ps_dec->ps_cur_slice =(dec_slice_params_t *) (pu1_extra_mem_base + ps_dec->u4_extra_mem_used);
    ps_dec->u4_extra_mem_used += sizeof(dec_slice_params_t);

    ps_dec->pv_scratch_sps_pps = (void *)(pu1_extra_mem_base + ps_dec->u4_extra_mem_used);


    ps_dec->u4_extra_mem_used += MAX(sizeof(dec_seq_params_t),
                                     sizeof(dec_pic_params_t));
    ps_dec->ps_pred_pkd = memtab[MEM_REC_PRED_INFO_PKD].pv_base;


    ps_dec->ps_dpb_mgr->pv_codec_handle = ps_dec;

    ps_dec->pv_dec_out = (void *)ps_init_op;
    ps_dec->pv_dec_in = (void *)ps_init_ip;

    ps_dec->u1_chroma_format =
                    (UWORD8)(ps_init_ip->s_ivd_init_ip_t.e_output_format);



    ih264d_init_decoder(ps_dec);

    return (IV_SUCCESS);

}


/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_fill_num_mem_rec                                  */
/*                                                                           */
/*  Description   :  fills memory records                                    */
/*                                                                           */
/*  Inputs        : pv_api_ip input api structure                            */
/*                : pv_api_op output api structure                           */
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
WORD32 ih264d_fill_num_mem_rec(void *pv_api_ip, void *pv_api_op)
{

    ih264d_fill_mem_rec_ip_t *ps_mem_q_ip;
    ih264d_fill_mem_rec_op_t *ps_mem_q_op;
    WORD32 level;
    UWORD32 num_reorder_frames;
    UWORD32 num_ref_frames;
    UWORD32 num_extra_disp_bufs;
    UWORD32 u4_dpb_size_num_frames;
    iv_mem_rec_t *memTab;

    UWORD32 chroma_format, u4_share_disp_buf;
    UWORD32 u4_total_num_mbs;
    UWORD32 luma_width, luma_width_in_mbs;
    UWORD32 luma_height, luma_height_in_mbs;
    UWORD32 max_dpb_size;

    ps_mem_q_ip = (ih264d_fill_mem_rec_ip_t *)pv_api_ip;
    ps_mem_q_op = (ih264d_fill_mem_rec_op_t *)pv_api_op;

    if(ps_mem_q_ip->s_ivd_fill_mem_rec_ip_t.u4_size
                    > offsetof(ih264d_fill_mem_rec_ip_t, i4_level))
    {
        level = ps_mem_q_ip->i4_level;
    }
    else
    {
        level = H264_LEVEL_3_1;
    }

    if(ps_mem_q_ip->s_ivd_fill_mem_rec_ip_t.u4_size
                    > offsetof(ih264d_fill_mem_rec_ip_t, u4_num_reorder_frames))
    {
        num_reorder_frames = ps_mem_q_ip->u4_num_reorder_frames;
    }
    else
    {
        num_reorder_frames = H264_MAX_REF_PICS;
    }

    if(ps_mem_q_ip->s_ivd_fill_mem_rec_ip_t.u4_size
                    > offsetof(ih264d_fill_mem_rec_ip_t, u4_num_ref_frames))
    {
        num_ref_frames = ps_mem_q_ip->u4_num_ref_frames;
    }
    else
    {
        num_ref_frames = H264_MAX_REF_PICS;
    }

    if(ps_mem_q_ip->s_ivd_fill_mem_rec_ip_t.u4_size
                    > offsetof(ih264d_fill_mem_rec_ip_t, u4_num_extra_disp_buf))
    {
        num_extra_disp_bufs = ps_mem_q_ip->u4_num_extra_disp_buf;
    }
    else
    {
        num_extra_disp_bufs = 0;
    }

    if(ps_mem_q_ip->s_ivd_fill_mem_rec_ip_t.u4_size
                    > offsetof(ih264d_fill_mem_rec_ip_t, u4_share_disp_buf))
    {
#ifndef LOGO_EN
        u4_share_disp_buf = ps_mem_q_ip->u4_share_disp_buf;
#else
        u4_share_disp_buf = 0;
#endif
    }
    else
    {
        u4_share_disp_buf = 0;
    }

    if(ps_mem_q_ip->s_ivd_fill_mem_rec_ip_t.u4_size
                    > offsetof(ih264d_fill_mem_rec_ip_t, e_output_format))
    {
        chroma_format = ps_mem_q_ip->e_output_format;
    }
    else
    {
        chroma_format = -1;
    }

    if((chroma_format != IV_YUV_420P) && (chroma_format != IV_YUV_420SP_UV)
                    && (chroma_format != IV_YUV_420SP_VU))
    {
        u4_share_disp_buf = 0;
    }
    if(0 == u4_share_disp_buf)
        num_extra_disp_bufs = 0;

    {

        luma_height = ps_mem_q_ip->s_ivd_fill_mem_rec_ip_t.u4_max_frm_ht;
        luma_width = ps_mem_q_ip->s_ivd_fill_mem_rec_ip_t.u4_max_frm_wd;

        luma_height = ALIGN32(luma_height);
        luma_width = ALIGN16(luma_width);
        luma_width_in_mbs = luma_width >> 4;
        luma_height_in_mbs = luma_height >> 4;
        u4_total_num_mbs = (luma_height * luma_width) >> 8;
    }
    /*
     * If level is lesser than 31 and the resolution required is higher,
     * then make the level at least 31.
     */
    if(u4_total_num_mbs > MAX_MBS_LEVEL_30 && level < H264_LEVEL_3_1)
    {
        level = H264_LEVEL_3_1;
    }

    if((level < MIN_LEVEL_SUPPORTED) || (level > MAX_LEVEL_SUPPORTED))
    {
        ps_mem_q_op->s_ivd_fill_mem_rec_op_t.u4_error_code |=
                        ERROR_LEVEL_UNSUPPORTED;
        return (IV_FAIL);
    }

    if(num_ref_frames > H264_MAX_REF_PICS)
    {
        ps_mem_q_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= ERROR_NUM_REF;
        num_ref_frames = H264_MAX_REF_PICS;
    }

    if(num_reorder_frames > H264_MAX_REF_PICS)
    {
        ps_mem_q_op->s_ivd_fill_mem_rec_op_t.u4_error_code |= ERROR_NUM_REF;
        num_reorder_frames = H264_MAX_REF_PICS;
    }
    memTab = ps_mem_q_ip->s_ivd_fill_mem_rec_ip_t.pv_mem_rec_location;

    memTab[MEM_REC_IV_OBJ].u4_mem_size = sizeof(iv_obj_t);
    memTab[MEM_REC_IV_OBJ].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_IV_OBJ].e_mem_type = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    H264_DEC_DEBUG_PRINT("MEM_REC_IV_OBJ MEM Size = %d\n",
                         memTab[MEM_REC_IV_OBJ].u4_mem_size);

    memTab[MEM_REC_CODEC].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_CODEC].e_mem_type = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    memTab[MEM_REC_CODEC].u4_mem_size = sizeof(dec_struct_t);

    {
        UWORD32 mvinfo_size, mv_info_size_pad;
        UWORD32 MVbank, MVbank_pad;
        UWORD32 Ysize;
        UWORD32 UVsize;
        UWORD32 one_frm_size;

        UWORD32 extra_mem = 0;

        UWORD32 pad_len_h, pad_len_v;

        /*
         * For low_delay, use num_buf as 2 -
         *      num_buf = (num_buf_ref) + 1;
         * where num_buf_ref is 1.
         */
        UWORD32 num_buf;

        {
            UWORD32 num_bufs_app, num_bufs_level;

            num_bufs_app = num_ref_frames + num_reorder_frames + 1;

            if(num_bufs_app <= 1)
                num_bufs_app = 2;

            num_bufs_level = ih264d_get_dpb_size_new(level, (luma_width >> 4),
                                                     (luma_height >> 4));

            max_dpb_size = num_bufs_level;

            num_bufs_level = num_bufs_level * 2 + 1;

            num_buf = MIN(num_bufs_level, num_bufs_app);

            num_buf += num_extra_disp_bufs;

        }

        mvinfo_size = ((luma_width * (luma_height)) >> 4);

        mv_info_size_pad = ((luma_width * (PAD_MV_BANK_ROW)) >> 4);

        Ysize = ALIGN32((luma_width + (PAD_LEN_Y_H << 1)))
                        * (luma_height + (PAD_LEN_Y_V << 2));


        UVsize = Ysize >> 2;
        if(u4_share_disp_buf == 1)
        {
            /* In case of buffers getting shared between application and library
             there is no need of reference memtabs. Instead of setting the i4_size
             to zero, it is reduced to a small i4_size to ensure that changes
             in the code are minimal */

            if((chroma_format == IV_YUV_420P)
                            || (chroma_format == IV_YUV_420SP_UV)
                            || (chroma_format == IV_YUV_420SP_VU))
            {
                Ysize = 64;
            }
            if(chroma_format == IV_YUV_420SP_UV)
            {
                UVsize = 64;
            }
        }

        one_frm_size = (((Ysize + 127) >> 7) << 7)
                        + ((((UVsize << 1) + 127) >> 7) << 7);

        //Note that for ARM RVDS WS the sizeof(mv_pred_t) is 16

        /*Add memory for colocated MB*/
        MVbank = sizeof(mv_pred_t) * mvinfo_size;
        MVbank_pad = sizeof(mv_pred_t) * mv_info_size_pad;

        MVbank = (((MVbank + 127) >> 7) << 7);

        MVbank_pad = (((MVbank_pad + 127) >> 7) << 7);

        memTab[MEM_REC_MVBANK].u4_mem_alignment = (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_MVBANK].e_mem_type =
                        IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        memTab[MEM_REC_MVBANK].u4_mem_size = (MVbank + MVbank_pad)
                        * (MIN(max_dpb_size, num_ref_frames) + 1);


        memTab[MEM_REC_REF_PIC].u4_mem_alignment = (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_REF_PIC].e_mem_type =
                        IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        memTab[MEM_REC_REF_PIC].u4_mem_size = one_frm_size * num_buf;

    }

    memTab[MEM_REC_DEBLK_MB_INFO].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_DEBLK_MB_INFO].e_mem_type =
                    IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    memTab[MEM_REC_DEBLK_MB_INFO].u4_mem_size = (((((u4_total_num_mbs
                    + (luma_width >> 4)) * sizeof(deblk_mb_t)) + 127) >> 7) << 7);

    memTab[MEM_REC_NEIGHBOR_INFO].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_NEIGHBOR_INFO].e_mem_type =
                    IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    memTab[MEM_REC_NEIGHBOR_INFO].u4_mem_size = sizeof(mb_neigbour_params_t)
                    * ((luma_width + 16) >> 4) * 2 * 2;
    {
        WORD32 size;
        WORD32 num_entries;

        num_entries = MIN(MAX_FRAMES, num_ref_frames);
        num_entries = 2 * ((2 * num_entries) + 1);

        size = num_entries * sizeof(void *);
        size += PAD_MAP_IDX_POC * sizeof(void *);
        size *= u4_total_num_mbs;
        size += sizeof(dec_slice_struct_t) * u4_total_num_mbs;
        memTab[MEM_REC_SLICE_HDR].u4_mem_alignment = (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_SLICE_HDR].e_mem_type = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        memTab[MEM_REC_SLICE_HDR].u4_mem_size = size;
    }
    {

        UWORD32 u4_num_entries;

        u4_num_entries = u4_total_num_mbs;

        memTab[MEM_REC_MB_INFO].u4_mem_alignment = (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_MB_INFO].e_mem_type =
                        IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        memTab[MEM_REC_MB_INFO].u4_mem_size = sizeof(dec_mb_info_t)
                        * u4_num_entries;

        memTab[MEM_REC_PRED_INFO].u4_mem_alignment = (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_PRED_INFO].e_mem_type =
                        IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;

        memTab[MEM_REC_PRED_INFO].u4_mem_size = sizeof(pred_info_t) * 2*32;

        memTab[MEM_REC_COEFF_DATA].u4_mem_alignment = (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_COEFF_DATA].e_mem_type =
                        IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        memTab[MEM_REC_COEFF_DATA].u4_mem_size = MB_LUM_SIZE * sizeof(WORD16);
        /*For I16x16 MBs, 16 4x4 AC coeffs and 1 4x4 DC coeff TU blocks will be sent
        For all MBs along with 8 4x4 AC coeffs 2 2x2 DC coeff TU blocks will be sent
        So use 17 4x4 TU blocks for luma and 9 4x4 TU blocks for chroma */
        memTab[MEM_REC_COEFF_DATA].u4_mem_size += u4_num_entries
                        * (MAX(17 * sizeof(tu_sblk4x4_coeff_data_t),4 * sizeof(tu_blk8x8_coeff_data_t))
                                        + 9 * sizeof(tu_sblk4x4_coeff_data_t));
        //32 bytes for each mb to store u1_prev_intra4x4_pred_mode and u1_rem_intra4x4_pred_mode data
        memTab[MEM_REC_COEFF_DATA].u4_mem_size += u4_num_entries * 32;

    }

    memTab[MEM_REC_SPS].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_SPS].e_mem_type = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    memTab[MEM_REC_SPS].u4_mem_size = ((sizeof(dec_seq_params_t))
                    * MAX_NUM_SEQ_PARAMS);

    memTab[MEM_REC_PPS].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_PPS].e_mem_type = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    memTab[MEM_REC_PPS].u4_mem_size = (sizeof(dec_pic_params_t))
                    * MAX_NUM_PIC_PARAMS;

    {
        UWORD32 u4_mem_size;

        u4_mem_size = 0;
        u4_mem_size += (((sizeof(dec_err_status_t) + 127) >> 7) << 7);
        u4_mem_size += sizeof(sei);
        u4_mem_size += sizeof(dpb_commands_t);
        u4_mem_size += sizeof(dec_bit_stream_t);
        u4_mem_size += sizeof(dec_slice_params_t);
        u4_mem_size += MAX(sizeof(dec_seq_params_t), sizeof(dec_pic_params_t));

        memTab[MEM_REC_EXTRA_MEM].u4_mem_alignment = (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_EXTRA_MEM].e_mem_type =
                        IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        memTab[MEM_REC_EXTRA_MEM].u4_mem_size = u4_mem_size;
    }

    {

        UWORD32 u4_mem_size;

        u4_mem_size = 0;
        u4_mem_size += ((TOTAL_LIST_ENTRIES + PAD_MAP_IDX_POC) * sizeof(void *));
        u4_mem_size = ALIGN64(u4_mem_size);
        u4_mem_size += (sizeof(bin_ctxt_model_t) * NUM_CABAC_CTXTS);
        u4_mem_size = ALIGN64(u4_mem_size);
        u4_mem_size += sizeof(ctxt_inc_mb_info_t);
        u4_mem_size = ALIGN64(u4_mem_size);
        u4_mem_size += sizeof(UWORD32) * (MAX_REF_BUFS * MAX_REF_BUFS);
        u4_mem_size = ALIGN64(u4_mem_size);

        u4_mem_size += MAX_REF_BUF_SIZE * 2;
        u4_mem_size = ALIGN64(u4_mem_size);
        u4_mem_size += ((sizeof(WORD16)) * PRED_BUFFER_WIDTH
                        * PRED_BUFFER_HEIGHT * 2);
        u4_mem_size = ALIGN64(u4_mem_size);
        u4_mem_size += sizeof(UWORD8) * (MB_LUM_SIZE);
        u4_mem_size = ALIGN64(u4_mem_size);
        u4_mem_size += sizeof(parse_pmbarams_t) * luma_width_in_mbs; //Max recon mb group*/
        u4_mem_size = ALIGN64(u4_mem_size);
        u4_mem_size += (sizeof(parse_part_params_t) * luma_width_in_mbs) << 4; //Max recon mb group*/
        u4_mem_size = ALIGN64(u4_mem_size);

        u4_mem_size += 2 * MAX_REF_BUFS * sizeof(struct pic_buffer_t);
        u4_mem_size = ALIGN64(u4_mem_size);
        u4_mem_size += 2 * MAX_REF_BUFS * sizeof(struct pic_buffer_t);
        u4_mem_size = ALIGN64(u4_mem_size);
        u4_mem_size += (sizeof(UWORD32) * 3 * (MAX_REF_BUFS * MAX_REF_BUFS)) << 3;
        u4_mem_size = ALIGN64(u4_mem_size);

        u4_mem_size += sizeof(UWORD32) * 2 * 3
                        * ((MAX_FRAMES << 1) * (MAX_FRAMES << 1));
        u4_mem_size = ALIGN64(u4_mem_size);

        memTab[MEM_REC_INTERNAL_SCRATCH].u4_mem_alignment =
                        (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_INTERNAL_SCRATCH].e_mem_type =
                        IV_EXTERNAL_CACHEABLE_SCRATCH_MEM;
        memTab[MEM_REC_INTERNAL_SCRATCH].u4_mem_size = u4_mem_size;
    }

    {

        UWORD32 u4_mem_used;
        UWORD32 u4_numRows = MB_SIZE << 1;
        UWORD32 u4_blk_wd = ((luma_width_in_mbs << 4) >> 1) + 8;

        u4_mem_used = 0;
        u4_mem_used += ((luma_width_in_mbs * sizeof(deblkmb_neighbour_t)) << 1);
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += (sizeof(neighbouradd_t) << 2);
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += ((sizeof(ctxt_inc_mb_info_t))
                        * (((luma_width_in_mbs + 1) << 1) + 1));
        u4_mem_used = ALIGN64(u4_mem_used);

        u4_mem_used += (sizeof(mv_pred_t) * luma_width_in_mbs * 16);
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += (sizeof(mv_pred_t) * luma_width_in_mbs * 16);
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += (sizeof(mv_pred_t) * luma_width_in_mbs * 4
                        * MV_SCRATCH_BUFS);
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += sizeof(UWORD8) * u4_numRows * u4_blk_wd;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += sizeof(UWORD8) * u4_numRows * u4_blk_wd;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_numRows = BLK8x8SIZE << 1;

        u4_blk_wd = ((luma_width_in_mbs << 3) >> 1) + 8;

        u4_mem_used += sizeof(UWORD8) * u4_numRows * u4_blk_wd;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += sizeof(UWORD8) * u4_numRows * u4_blk_wd;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += sizeof(UWORD8) * u4_numRows * u4_blk_wd;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += sizeof(UWORD8) * u4_numRows * u4_blk_wd;
        u4_mem_used += 32;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += sizeof(UWORD8) * (luma_width + 16) * 2;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += sizeof(UWORD8) * (luma_width + 16) * 2;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += sizeof(UWORD8) * (luma_width + 16) * 2;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += sizeof(mb_neigbour_params_t) * (luma_width_in_mbs + 1)
                        * luma_height_in_mbs;
        u4_mem_used += luma_width;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += luma_width;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += luma_width;
        u4_mem_used = ALIGN64(u4_mem_used);

        u4_mem_used += ((MB_SIZE + 4) << 1) * PAD_LEN_Y_H;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += ((BLK8x8SIZE + 2) << 1) * PAD_LEN_UV_H;
        u4_mem_used = ALIGN64(u4_mem_used);
        u4_mem_used += ((BLK8x8SIZE + 2) << 1) * PAD_LEN_UV_H;
        u4_mem_used = ALIGN64(u4_mem_used);
        memTab[MEM_REC_INTERNAL_PERSIST].u4_mem_alignment =
                        (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_INTERNAL_PERSIST].e_mem_type =
                        IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        memTab[MEM_REC_INTERNAL_PERSIST].u4_mem_size = u4_mem_used;
    }

    memTab[MEM_REC_BITSBUF].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_BITSBUF].e_mem_type = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    memTab[MEM_REC_BITSBUF].u4_mem_size = MAX(256000, (luma_width * luma_height * 3 / 2));

    {

        UWORD32 u4_thread_struct_size = ithread_get_handle_size();

        memTab[MEM_REC_THREAD_HANDLE].u4_mem_alignment = (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_THREAD_HANDLE].e_mem_type =
                        IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        memTab[MEM_REC_THREAD_HANDLE].u4_mem_size = u4_thread_struct_size * 2;

    }

    memTab[MEM_REC_PARSE_MAP].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_PARSE_MAP].e_mem_type = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    memTab[MEM_REC_PARSE_MAP].u4_mem_size = u4_total_num_mbs;

    memTab[MEM_REC_PROC_MAP].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_PROC_MAP].e_mem_type = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    memTab[MEM_REC_PROC_MAP].u4_mem_size = u4_total_num_mbs;

    memTab[MEM_REC_SLICE_NUM_MAP].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_SLICE_NUM_MAP].e_mem_type =
                    IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    memTab[MEM_REC_SLICE_NUM_MAP].u4_mem_size = u4_total_num_mbs
                    * sizeof(UWORD16);

    memTab[MEM_REC_DPB_MGR].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_DPB_MGR].e_mem_type = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    memTab[MEM_REC_DPB_MGR].u4_mem_size = sizeof(dpb_manager_t);

    memTab[MEM_REC_BACKUP].u4_mem_alignment = (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_BACKUP].e_mem_type = IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
    memTab[MEM_REC_BACKUP].u4_mem_size = sizeof(iv_mem_rec_t) * MEM_REC_CNT;

    {

        UWORD32 u4_mem_size;

        u4_mem_size = sizeof(disp_mgr_t);
        u4_mem_size += sizeof(buf_mgr_t) + ithread_get_mutex_lock_size();
        u4_mem_size += sizeof(struct pic_buffer_t) * (H264_MAX_REF_PICS * 2);

        memTab[MEM_REC_PIC_BUF_MGR].u4_mem_alignment = (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_PIC_BUF_MGR].e_mem_type =
                        IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        memTab[MEM_REC_PIC_BUF_MGR].u4_mem_size = u4_mem_size;
    }

    {
        UWORD32 u4_mem_size;

        u4_mem_size  = sizeof(buf_mgr_t) + ithread_get_mutex_lock_size();
        u4_mem_size += sizeof(col_mv_buf_t) * (H264_MAX_REF_PICS * 2);
        u4_mem_size = ALIGN128(u4_mem_size);
        u4_mem_size += ((luma_width * luma_height) >> 4)
                        * (MIN(max_dpb_size, num_ref_frames) + 1);
        memTab[MEM_REC_MV_BUF_MGR].u4_mem_alignment = (128 * 8) / CHAR_BIT;
        memTab[MEM_REC_MV_BUF_MGR].e_mem_type =
                        IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;
        memTab[MEM_REC_MV_BUF_MGR].u4_mem_size = u4_mem_size;
    }

    memTab[MEM_REC_PRED_INFO_PKD].u4_mem_alignment =  (128 * 8) / CHAR_BIT;
    memTab[MEM_REC_PRED_INFO_PKD].e_mem_type =
                    IV_EXTERNAL_CACHEABLE_PERSISTENT_MEM;

    {
        UWORD32 u4_num_entries;
        u4_num_entries = u4_total_num_mbs;

        if(1 == num_ref_frames)
            u4_num_entries *= 16;
        else
            u4_num_entries *= 16 * 2;

        memTab[MEM_REC_PRED_INFO_PKD].u4_mem_size = sizeof(pred_info_pkd_t)
                        * u4_num_entries;
    }

    ps_mem_q_op->s_ivd_fill_mem_rec_op_t.u4_num_mem_rec_filled = MEM_REC_CNT;


    return IV_SUCCESS;
}
/*****************************************************************************/
/*                                                                           */
/*  Function Name :  ih264d_clr                                              */
/*                                                                           */
/*  Description   :  returns memory records to app                           */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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
WORD32 ih264d_clr(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{

    dec_struct_t * ps_dec;
    iv_retrieve_mem_rec_ip_t *dec_clr_ip;
    iv_retrieve_mem_rec_op_t *dec_clr_op;

    dec_clr_ip = (iv_retrieve_mem_rec_ip_t *)pv_api_ip;
    dec_clr_op = (iv_retrieve_mem_rec_op_t *)pv_api_op;
    ps_dec = (dec_struct_t *)(dec_hdl->pv_codec_handle);

    if(ps_dec->init_done != 1)
    {
        //return a proper Error Code
        return IV_FAIL;
    }

    if(ps_dec->pv_pic_buf_mgr)
        ih264_buf_mgr_free((buf_mgr_t *)ps_dec->pv_pic_buf_mgr);
    if(ps_dec->pv_mv_buf_mgr)
        ih264_buf_mgr_free((buf_mgr_t *)ps_dec->pv_mv_buf_mgr);

    memcpy(dec_clr_ip->pv_mem_rec_location, ps_dec->ps_mem_tab,
           MEM_REC_CNT * (sizeof(iv_mem_rec_t)));
    dec_clr_op->u4_num_mem_rec_filled = MEM_REC_CNT;

    H264_DEC_DEBUG_PRINT("The clear non-conceal num mem recs: %d\n",
                         dec_clr_op->u4_num_mem_rec_filled);

    return IV_SUCCESS;

}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_init                                              */
/*                                                                           */
/*  Description   : initializes decoder                                      */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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
WORD32 ih264d_init(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{
    ih264d_init_ip_t *ps_init_ip;
    ih264d_init_op_t *ps_init_op;
    WORD32 init_status = IV_SUCCESS;
    ps_init_ip = (ih264d_init_ip_t *)pv_api_ip;
    ps_init_op = (ih264d_init_op_t *)pv_api_op;

    init_status = ih264d_init_video_decoder(dec_hdl, ps_init_ip, ps_init_op);

    if(IV_SUCCESS != init_status)
    {
        return init_status;
    }

    return init_status;
}
/*****************************************************************************/
/*                                                                           */
/*  Function Name :  ih264d_map_error                                        */
/*                                                                           */
/*  Description   :  Maps error codes to IVD error groups                    */
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
UWORD32 ih264d_map_error(UWORD32 i4_err_status)
{
    UWORD32 temp = 0;

    switch(i4_err_status)
    {
        case ERROR_MEM_ALLOC_ISRAM_T:
        case ERROR_MEM_ALLOC_SDRAM_T:
        case ERROR_BUF_MGR:
        case ERROR_MB_GROUP_ASSGN_T:
        case ERROR_FRAME_LIMIT_OVER:
        case ERROR_ACTUAL_RESOLUTION_GREATER_THAN_INIT:
        case ERROR_PROFILE_NOT_SUPPORTED:
        case ERROR_INIT_NOT_DONE:
            temp = 1 << IVD_FATALERROR;
            H264_DEC_DEBUG_PRINT("\nFatal Error\n");
            break;

        case ERROR_DBP_MANAGER_T:
        case ERROR_GAPS_IN_FRM_NUM:
        case ERROR_UNKNOWN_NAL:
        case ERROR_INV_MB_SLC_GRP_T:
        case ERROR_MULTIPLE_SLC_GRP_T:
        case ERROR_UNKNOWN_LEVEL:
        case ERROR_UNAVAIL_PICBUF_T:
        case ERROR_UNAVAIL_MVBUF_T:
        case ERROR_UNAVAIL_DISPBUF_T:
        case ERROR_NUM_REF:
        case ERROR_REFIDX_ORDER_T:
        case ERROR_PIC0_NOT_FOUND_T:
        case ERROR_MB_TYPE:
        case ERROR_SUB_MB_TYPE:
        case ERROR_CBP:
        case ERROR_REF_IDX:
        case ERROR_NUM_MV:
        case ERROR_CHROMA_PRED_MODE:
        case ERROR_INTRAPRED:
        case ERROR_NEXT_MB_ADDRESS_T:
        case ERROR_MB_ADDRESS_T:
        case ERROR_PIC1_NOT_FOUND_T:
        case ERROR_CAVLC_NUM_COEFF_T:
        case ERROR_CAVLC_SCAN_POS_T:
        case ERROR_PRED_WEIGHT_TABLE_T:
        case ERROR_CORRUPTED_SLICE:
            temp = 1 << IVD_CORRUPTEDDATA;
            break;

        case ERROR_NOT_SUPP_RESOLUTION:
        case ERROR_FEATURE_UNAVAIL:
        case ERROR_ACTUAL_LEVEL_GREATER_THAN_INIT:
            temp = 1 << IVD_UNSUPPORTEDINPUT;
            break;

        case ERROR_INVALID_PIC_PARAM:
        case ERROR_INVALID_SEQ_PARAM:
        case ERROR_EGC_EXCEED_32_1_T:
        case ERROR_EGC_EXCEED_32_2_T:
        case ERROR_INV_RANGE_TEV_T:
        case ERROR_INV_SLC_TYPE_T:
        case ERROR_INV_POC_TYPE_T:
        case ERROR_INV_RANGE_QP_T:
        case ERROR_INV_SPS_PPS_T:
        case ERROR_INV_SLICE_HDR_T:
            temp = 1 << IVD_CORRUPTEDHEADER;
            break;

        case ERROR_EOB_FLUSHBITS_T:
        case ERROR_EOB_GETBITS_T:
        case ERROR_EOB_GETBIT_T:
        case ERROR_EOB_BYPASS_T:
        case ERROR_EOB_DECISION_T:
        case ERROR_EOB_TERMINATE_T:
        case ERROR_EOB_READCOEFF4X4CAB_T:
            temp = 1 << IVD_INSUFFICIENTDATA;
            break;
        case ERROR_DYNAMIC_RESOLUTION_NOT_SUPPORTED:
        case ERROR_DISP_WIDTH_RESET_TO_PIC_WIDTH:
            temp = 1 << IVD_UNSUPPORTEDPARAM | 1 << IVD_FATALERROR;
            break;

        case ERROR_DANGLING_FIELD_IN_PIC:
            temp = 1 << IVD_APPLIEDCONCEALMENT;
            break;

    }

    return temp;

}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :  ih264d_video_decode                                     */
/*                                                                           */
/*  Description   :  handle video decode API command                         */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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

WORD32 ih264d_video_decode(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{
    /* ! */

    dec_struct_t * ps_dec = (dec_struct_t *)(dec_hdl->pv_codec_handle);

    WORD32 i4_err_status = 0;
    UWORD8 *pu1_buf = NULL;
    WORD32 buflen;
    UWORD32 u4_max_ofst, u4_length_of_start_code = 0;

    UWORD32 bytes_consumed = 0;
    UWORD32 cur_slice_is_nonref = 0;
    UWORD32 u4_next_is_aud;
    UWORD32 u4_first_start_code_found = 0;
    WORD32 ret = 0,api_ret_value = IV_SUCCESS;
    WORD32 header_data_left = 0,frame_data_left = 0;
    UWORD8 *pu1_bitstrm_buf;
    ivd_video_decode_ip_t *ps_dec_ip;
    ivd_video_decode_op_t *ps_dec_op;

    ithread_set_name((void*)"Parse_thread");

    ps_dec_ip = (ivd_video_decode_ip_t *)pv_api_ip;
    ps_dec_op = (ivd_video_decode_op_t *)pv_api_op;

    {
        UWORD32 u4_size;
        u4_size = ps_dec_op->u4_size;
        memset(ps_dec_op, 0, sizeof(ivd_video_decode_op_t));
        ps_dec_op->u4_size = u4_size;
    }

    ps_dec->pv_dec_out = ps_dec_op;
    ps_dec->process_called = 1;
    if(ps_dec->init_done != 1)
    {
        return IV_FAIL;
    }

    /*Data memory barries instruction,so that bitstream write by the application is complete*/
    DATA_SYNC();

    if(0 == ps_dec->u1_flushfrm)
    {
        if(ps_dec_ip->pv_stream_buffer == NULL)
        {
            ps_dec_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
            ps_dec_op->u4_error_code |= IVD_DEC_FRM_BS_BUF_NULL;
            return IV_FAIL;
        }
        if(ps_dec_ip->u4_num_Bytes <= 0)
        {
            ps_dec_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
            ps_dec_op->u4_error_code |= IVD_DEC_NUMBYTES_INV;
            return IV_FAIL;

        }
    }
    ps_dec->u1_pic_decode_done = 0;

    ps_dec_op->u4_num_bytes_consumed = 0;

    ps_dec->ps_out_buffer = NULL;

    if(ps_dec_ip->u4_size
                    >= offsetof(ivd_video_decode_ip_t, s_out_buffer))
        ps_dec->ps_out_buffer = &ps_dec_ip->s_out_buffer;

    ps_dec->u4_fmt_conv_cur_row = 0;

    ps_dec->u4_output_present = 0;
    ps_dec->s_disp_op.u4_error_code = 1;
    ps_dec->u4_fmt_conv_num_rows = FMT_CONV_NUM_ROWS;
    ps_dec->u4_stop_threads = 0;
    if(0 == ps_dec->u4_share_disp_buf
                    && ps_dec->i4_decode_header == 0)
    {
        UWORD32 i;
        if(ps_dec->ps_out_buffer->u4_num_bufs == 0)
        {
            ps_dec_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
            ps_dec_op->u4_error_code |= IVD_DISP_FRM_ZERO_OP_BUFS;
            return IV_FAIL;
        }

        for(i = 0; i < ps_dec->ps_out_buffer->u4_num_bufs; i++)
        {
            if(ps_dec->ps_out_buffer->pu1_bufs[i] == NULL)
            {
                ps_dec_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                ps_dec_op->u4_error_code |= IVD_DISP_FRM_OP_BUF_NULL;
                return IV_FAIL;
            }

            if(ps_dec->ps_out_buffer->u4_min_out_buf_size[i] == 0)
            {
                ps_dec_op->u4_error_code |= 1 << IVD_UNSUPPORTEDPARAM;
                ps_dec_op->u4_error_code |=
                                IVD_DISP_FRM_ZERO_OP_BUF_SIZE;
                return IV_FAIL;
            }
        }
    }

    if(ps_dec->u4_total_frames_decoded >= NUM_FRAMES_LIMIT)
    {
        ps_dec_op->u4_error_code = ERROR_FRAME_LIMIT_OVER;
        return IV_FAIL;
    }

    /* ! */
    ps_dec->u4_ts = ps_dec_ip->u4_ts;

    ps_dec_op->u4_error_code = 0;
    ps_dec_op->e_pic_type = -1;
    ps_dec_op->u4_output_present = 0;
    ps_dec_op->u4_frame_decoded_flag = 0;

    ps_dec->i4_frametype = -1;
    ps_dec->i4_content_type = -1;
    /*
     * For field pictures, set the bottom and top picture decoded u4_flag correctly.
     */
    {
        if((TOP_FIELD_ONLY | BOT_FIELD_ONLY) == ps_dec->u1_top_bottom_decoded)
        {
            ps_dec->u1_top_bottom_decoded = 0;
        }
    }
    ps_dec->u4_slice_start_code_found = 0;

    /* In case the deocder is not in flush mode(in shared mode),
     then decoder has to pick up a buffer to write current frame.
     Check if a frame is available in such cases */

    if(ps_dec->u1_init_dec_flag == 1 && ps_dec->u4_share_disp_buf == 1
                    && ps_dec->u1_flushfrm == 0)
    {
        UWORD32 i;

        WORD32 disp_avail = 0, free_id;

        /* Check if at least one buffer is available with the codec */
        /* If not then return to application with error */
        for(i = 0; i < ps_dec->u1_pic_bufs; i++)
        {
            if(0 == ps_dec->u4_disp_buf_mapping[i]
                            || 1 == ps_dec->u4_disp_buf_to_be_freed[i])
            {
                disp_avail = 1;
                break;
            }

        }

        if(0 == disp_avail)
        {
            /* If something is queued for display wait for that buffer to be returned */

            ps_dec_op->u4_error_code = IVD_DEC_REF_BUF_NULL;
            ps_dec_op->u4_error_code |= (1 << IVD_UNSUPPORTEDPARAM);
            return (IV_FAIL);
        }

        while(1)
        {
            pic_buffer_t *ps_pic_buf;
            ps_pic_buf = (pic_buffer_t *)ih264_buf_mgr_get_next_free(
                            (buf_mgr_t *)ps_dec->pv_pic_buf_mgr, &free_id);

            if(ps_pic_buf == NULL)
            {
                UWORD32 i, display_queued = 0;

                /* check if any buffer was given for display which is not returned yet */
                for(i = 0; i < (MAX_DISP_BUFS_NEW); i++)
                {
                    if(0 != ps_dec->u4_disp_buf_mapping[i])
                    {
                        display_queued = 1;
                        break;
                    }
                }
                /* If some buffer is queued for display, then codec has to singal an error and wait
                 for that buffer to be returned.
                 If nothing is queued for display then codec has ownership of all display buffers
                 and it can reuse any of the existing buffers and continue decoding */

                if(1 == display_queued)
                {
                    /* If something is queued for display wait for that buffer to be returned */
                    ps_dec_op->u4_error_code = IVD_DEC_REF_BUF_NULL;
                    ps_dec_op->u4_error_code |= (1
                                    << IVD_UNSUPPORTEDPARAM);
                    return (IV_FAIL);
                }
            }
            else
            {
                /* If the buffer is with display, then mark it as in use and then look for a buffer again */
                if(1 == ps_dec->u4_disp_buf_mapping[free_id])
                {
                    ih264_buf_mgr_set_status(
                                    (buf_mgr_t *)ps_dec->pv_pic_buf_mgr,
                                    free_id,
                                    BUF_MGR_IO);
                }
                else
                {
                    /**
                     *  Found a free buffer for present call. Release it now.
                     *  Will be again obtained later.
                     */
                    ih264_buf_mgr_release((buf_mgr_t *)ps_dec->pv_pic_buf_mgr,
                                          free_id,
                                          BUF_MGR_IO);
                    break;
                }
            }
        }

    }

    if(ps_dec->u1_flushfrm && ps_dec->u1_init_dec_flag)
    {

        ih264d_get_next_display_field(ps_dec, ps_dec->ps_out_buffer,
                                      &(ps_dec->s_disp_op));
        if(0 == ps_dec->s_disp_op.u4_error_code)
        {
            ps_dec->u4_fmt_conv_cur_row = 0;
            ps_dec->u4_fmt_conv_num_rows = ps_dec->s_disp_frame_info.u4_y_ht;
            ih264d_format_convert(ps_dec, &(ps_dec->s_disp_op),
                                  ps_dec->u4_fmt_conv_cur_row,
                                  ps_dec->u4_fmt_conv_num_rows);
            ps_dec->u4_fmt_conv_cur_row += ps_dec->u4_fmt_conv_num_rows;
            ps_dec->u4_output_present = 1;

        }
        ih264d_release_display_field(ps_dec, &(ps_dec->s_disp_op));

        ps_dec_op->u4_pic_wd = (UWORD32)ps_dec->u2_disp_width;
        ps_dec_op->u4_pic_ht = (UWORD32)ps_dec->u2_disp_height;

        ps_dec_op->u4_new_seq = 0;

        ps_dec_op->u4_output_present = ps_dec->u4_output_present;
        ps_dec_op->u4_progressive_frame_flag =
                        ps_dec->s_disp_op.u4_progressive_frame_flag;
        ps_dec_op->e_output_format =
                        ps_dec->s_disp_op.e_output_format;
        ps_dec_op->s_disp_frm_buf = ps_dec->s_disp_op.s_disp_frm_buf;
        ps_dec_op->e4_fld_type = ps_dec->s_disp_op.e4_fld_type;
        ps_dec_op->u4_ts = ps_dec->s_disp_op.u4_ts;
        ps_dec_op->u4_disp_buf_id = ps_dec->s_disp_op.u4_disp_buf_id;

        /*In the case of flush ,since no frame is decoded set pic type as invalid*/
        ps_dec_op->u4_is_ref_flag = -1;
        ps_dec_op->e_pic_type = IV_NA_FRAME;
        ps_dec_op->u4_frame_decoded_flag = 0;

        if(0 == ps_dec->s_disp_op.u4_error_code)
        {
            return (IV_SUCCESS);
        }
        else
            return (IV_FAIL);

    }
    if(ps_dec->u1_res_changed == 1)
    {
        /*if resolution has changed and all buffers have been flushed, reset decoder*/
        ih264d_init_decoder(ps_dec);
    }

    ps_dec->u4_prev_nal_skipped = 0;

    ps_dec->u2_cur_mb_addr = 0;
    ps_dec->u2_total_mbs_coded = 0;
    ps_dec->u2_cur_slice_num = 0;
    ps_dec->cur_dec_mb_num = 0;
    ps_dec->cur_recon_mb_num = 0;
    ps_dec->u4_first_slice_in_pic = 2;
    ps_dec->u1_first_pb_nal_in_pic = 1;
    ps_dec->u1_slice_header_done = 0;
    ps_dec->u1_dangling_field = 0;

    ps_dec->u4_dec_thread_created = 0;
    ps_dec->u4_bs_deblk_thread_created = 0;
    ps_dec->u4_cur_bs_mb_num = 0;
    ps_dec->u4_start_recon_deblk  = 0;

    DEBUG_THREADS_PRINTF(" Starting process call\n");

    ps_dec->u4_pic_buf_got = 0;

    do
    {

        pu1_buf = (UWORD8*)ps_dec_ip->pv_stream_buffer
                        + ps_dec_op->u4_num_bytes_consumed;

        u4_max_ofst = ps_dec_ip->u4_num_Bytes
                        - ps_dec_op->u4_num_bytes_consumed;
        pu1_bitstrm_buf = ps_dec->ps_mem_tab[MEM_REC_BITSBUF].pv_base;

        u4_next_is_aud = 0;

        buflen = ih264d_find_start_code(pu1_buf, 0, u4_max_ofst,
                                               &u4_length_of_start_code,
                                               &u4_next_is_aud);

        if(buflen == -1)
            buflen = 0;
        /* Ignore bytes beyond the allocated size of intermediate buffer */
        /* Since 8 bytes are read ahead, ensure 8 bytes are free at the
        end of the buffer, which will be memset to 0 after emulation prevention */
        buflen = MIN(buflen, (WORD32)(ps_dec->ps_mem_tab[MEM_REC_BITSBUF].u4_mem_size - 8));

        bytes_consumed = buflen + u4_length_of_start_code;
        ps_dec_op->u4_num_bytes_consumed += bytes_consumed;

        if(buflen >= MAX_NAL_UNIT_SIZE)
        {

            ih264d_fill_output_struct_from_context(ps_dec, ps_dec_op);
            H264_DEC_DEBUG_PRINT(
                            "\nNal Size exceeded %d, Processing Stopped..\n",
                            MAX_NAL_UNIT_SIZE);
            ps_dec->i4_error_code = 1 << IVD_CORRUPTEDDATA;

            ps_dec_op->e_pic_type = -1;
            /*signal the decode thread*/
            ih264d_signal_decode_thread(ps_dec);
            /*signal end of frame decode for curren frame*/

            if(ps_dec->u4_pic_buf_got == 0)
            {
                if(ps_dec->i4_header_decoded == 3)
                {
                    ps_dec->u2_total_mbs_coded =
                                    ps_dec->ps_cur_sps->u2_max_mb_addr + 1;
                }

                /* close deblock thread if it is not closed yet*/
                if(ps_dec->u4_num_cores == 3)
                {
                    ih264d_signal_bs_deblk_thread(ps_dec);
                }
                return IV_FAIL;
            }
            else
            {
                ps_dec->u1_pic_decode_done = 1;
                continue;
            }
        }

        {
            UWORD8 u1_firstbyte, u1_nal_ref_idc;

            if(ps_dec->i4_app_skip_mode == IVD_SKIP_B)
            {
                u1_firstbyte = *(pu1_buf + u4_length_of_start_code);
                u1_nal_ref_idc = (UWORD8)(NAL_REF_IDC(u1_firstbyte));
                if(u1_nal_ref_idc == 0)
                {
                    /*skip non reference frames*/
                    cur_slice_is_nonref = 1;
                    continue;
                }
                else
                {
                    if(1 == cur_slice_is_nonref)
                    {
                        /*We have encountered a referenced frame,return to app*/
                        ps_dec_op->u4_num_bytes_consumed -=
                                        bytes_consumed;
                        ps_dec_op->e_pic_type = IV_B_FRAME;
                        ps_dec_op->u4_error_code =
                                        IVD_DEC_FRM_SKIPPED;
                        ps_dec_op->u4_error_code |= (1
                                        << IVD_UNSUPPORTEDPARAM);
                        ps_dec_op->u4_frame_decoded_flag = 0;
                        ps_dec_op->u4_size =
                                        sizeof(ivd_video_decode_op_t);
                        /*signal the decode thread*/
                        ih264d_signal_decode_thread(ps_dec);
                        /* close deblock thread if it is not closed yet*/
                        if(ps_dec->u4_num_cores == 3)
                        {
                            ih264d_signal_bs_deblk_thread(ps_dec);
                        }

                        return (IV_FAIL);
                    }
                }

            }

        }


        if(buflen)
        {
            memcpy(pu1_bitstrm_buf, pu1_buf + u4_length_of_start_code,
                   buflen);
            u4_first_start_code_found = 1;

        }
        else
        {
            /*start code not found*/

            if(u4_first_start_code_found == 0)
            {
                /*no start codes found in current process call*/

                ps_dec->i4_error_code = ERROR_START_CODE_NOT_FOUND;
                ps_dec_op->u4_error_code |= 1 << IVD_INSUFFICIENTDATA;

                if(ps_dec->u4_pic_buf_got == 0)
                {

                    ih264d_fill_output_struct_from_context(ps_dec,
                                                           ps_dec_op);

                    ps_dec_op->u4_error_code = ps_dec->i4_error_code;
                    ps_dec_op->u4_frame_decoded_flag = 0;

                    return (IV_FAIL);
                }
                else
                {
                    ps_dec->u1_pic_decode_done = 1;
                    continue;
                }
            }
            else
            {
                /* a start code has already been found earlier in the same process call*/
                frame_data_left = 0;
                header_data_left = 0;
                continue;
            }

        }

        ps_dec->u4_return_to_app = 0;
        ret = ih264d_parse_nal_unit(dec_hdl, ps_dec_op,
                              pu1_bitstrm_buf, buflen);
        if(ret != OK)
        {
            UWORD32 error =  ih264d_map_error(ret);
            ps_dec_op->u4_error_code = error | ret;
            api_ret_value = IV_FAIL;

            if((ret == IVD_RES_CHANGED)
                            || (ret == IVD_STREAM_WIDTH_HEIGHT_NOT_SUPPORTED)
                            || (ret == ERROR_UNAVAIL_PICBUF_T)
                            || (ret == ERROR_UNAVAIL_MVBUF_T)
                            || (ret == ERROR_INV_SPS_PPS_T))
            {
                ps_dec->u4_slice_start_code_found = 0;
                break;
            }

            if((ret == ERROR_INCOMPLETE_FRAME) || (ret == ERROR_DANGLING_FIELD_IN_PIC))
            {
                ps_dec_op->u4_num_bytes_consumed -= bytes_consumed;
                api_ret_value = IV_FAIL;
                break;
            }

            if(ret == ERROR_IN_LAST_SLICE_OF_PIC)
            {
                api_ret_value = IV_FAIL;
                break;
            }

        }

        if(ps_dec->u4_return_to_app)
        {
            /*We have encountered a referenced frame,return to app*/
            ps_dec_op->u4_num_bytes_consumed -= bytes_consumed;
            ps_dec_op->u4_error_code = IVD_DEC_FRM_SKIPPED;
            ps_dec_op->u4_error_code |= (1 << IVD_UNSUPPORTEDPARAM);
            ps_dec_op->u4_frame_decoded_flag = 0;
            ps_dec_op->u4_size = sizeof(ivd_video_decode_op_t);
            /*signal the decode thread*/
            ih264d_signal_decode_thread(ps_dec);
            /* close deblock thread if it is not closed yet*/
            if(ps_dec->u4_num_cores == 3)
            {
                ih264d_signal_bs_deblk_thread(ps_dec);
            }
            return (IV_FAIL);

        }



        header_data_left = ((ps_dec->i4_decode_header == 1)
                        && (ps_dec->i4_header_decoded != 3)
                        && (ps_dec_op->u4_num_bytes_consumed
                                        < ps_dec_ip->u4_num_Bytes));
        frame_data_left = (((ps_dec->i4_decode_header == 0)
                        && ((ps_dec->u1_pic_decode_done == 0)
                                        || (u4_next_is_aud == 1)))
                        && (ps_dec_op->u4_num_bytes_consumed
                                        < ps_dec_ip->u4_num_Bytes));
    }
    while(( header_data_left == 1)||(frame_data_left == 1));

    if((ps_dec->u4_slice_start_code_found == 1)
            && ps_dec->u2_total_mbs_coded < ps_dec->u2_frm_ht_in_mbs * ps_dec->u2_frm_wd_in_mbs)
    {
        // last slice - missing/corruption
        WORD32 num_mb_skipped;
        WORD32 prev_slice_err;
        pocstruct_t temp_poc;
        WORD32 ret1;
        WORD32 ht_in_mbs;
        ht_in_mbs = ps_dec->u2_pic_ht >> (4 + ps_dec->ps_cur_slice->u1_field_pic_flag);
        num_mb_skipped = (ht_in_mbs * ps_dec->u2_frm_wd_in_mbs)
                            - ps_dec->u2_total_mbs_coded;

        if(ps_dec->u4_first_slice_in_pic && (ps_dec->u4_pic_buf_got == 0))
            prev_slice_err = 1;
        else
            prev_slice_err = 2;

        if(ps_dec->u4_first_slice_in_pic && (ps_dec->u2_total_mbs_coded == 0))
            prev_slice_err = 1;

        ret1 = ih264d_mark_err_slice_skip(ps_dec, num_mb_skipped, ps_dec->u1_nal_unit_type == IDR_SLICE_NAL, ps_dec->ps_cur_slice->u2_frame_num,
                                   &temp_poc, prev_slice_err);

        if((ret1 == ERROR_UNAVAIL_PICBUF_T) || (ret1 == ERROR_UNAVAIL_MVBUF_T) ||
                       (ret1 == ERROR_INV_SPS_PPS_T))
        {
            ret = ret1;
        }
    }

    if((ret == IVD_RES_CHANGED)
                    || (ret == IVD_STREAM_WIDTH_HEIGHT_NOT_SUPPORTED)
                    || (ret == ERROR_UNAVAIL_PICBUF_T)
                    || (ret == ERROR_UNAVAIL_MVBUF_T)
                    || (ret == ERROR_INV_SPS_PPS_T))
    {

        /* signal the decode thread */
        ih264d_signal_decode_thread(ps_dec);
        /* close deblock thread if it is not closed yet */
        if(ps_dec->u4_num_cores == 3)
        {
            ih264d_signal_bs_deblk_thread(ps_dec);
        }
        /* dont consume bitstream for change in resolution case */
        if(ret == IVD_RES_CHANGED)
        {
            ps_dec_op->u4_num_bytes_consumed -= bytes_consumed;
        }
        return IV_FAIL;
    }


    if(ps_dec->u1_separate_parse)
    {
        /* If Format conversion is not complete,
         complete it here */
        if(ps_dec->u4_num_cores == 2)
        {

            /*do deblocking of all mbs*/
            if((ps_dec->u4_nmb_deblk == 0) &&(ps_dec->u4_start_recon_deblk == 1) && (ps_dec->ps_cur_sps->u1_mb_aff_flag == 0))
            {
                UWORD32 u4_num_mbs,u4_max_addr;
                tfr_ctxt_t s_tfr_ctxt;
                tfr_ctxt_t *ps_tfr_cxt = &s_tfr_ctxt;
                pad_mgr_t *ps_pad_mgr = &ps_dec->s_pad_mgr;

                /*BS is done for all mbs while parsing*/
                u4_max_addr = (ps_dec->u2_frm_wd_in_mbs * ps_dec->u2_frm_ht_in_mbs) - 1;
                ps_dec->u4_cur_bs_mb_num = u4_max_addr + 1;


                ih264d_init_deblk_tfr_ctxt(ps_dec, ps_pad_mgr, ps_tfr_cxt,
                                           ps_dec->u2_frm_wd_in_mbs, 0);


                u4_num_mbs = u4_max_addr
                                - ps_dec->u4_cur_deblk_mb_num + 1;

                DEBUG_PERF_PRINTF("mbs left for deblocking= %d \n",u4_num_mbs);

                if(u4_num_mbs != 0)
                    ih264d_check_mb_map_deblk(ps_dec, u4_num_mbs,
                                                   ps_tfr_cxt,1);

                ps_dec->u4_start_recon_deblk  = 0;

            }

        }

        /*signal the decode thread*/
        ih264d_signal_decode_thread(ps_dec);
        /* close deblock thread if it is not closed yet*/
        if(ps_dec->u4_num_cores == 3)
        {
            ih264d_signal_bs_deblk_thread(ps_dec);
        }
    }


    DATA_SYNC();


    if((ps_dec_op->u4_error_code & 0xff)
                    != ERROR_DYNAMIC_RESOLUTION_NOT_SUPPORTED)
    {
        ps_dec_op->u4_pic_wd = (UWORD32)ps_dec->u2_disp_width;
        ps_dec_op->u4_pic_ht = (UWORD32)ps_dec->u2_disp_height;
    }

//Report if header (sps and pps) has not been decoded yet
    if(ps_dec->i4_header_decoded != 3)
    {
        ps_dec_op->u4_error_code |= (1 << IVD_INSUFFICIENTDATA);

    }

    if(ps_dec->i4_decode_header == 1 && ps_dec->i4_header_decoded != 3)
    {
        ps_dec_op->u4_error_code |= (1 << IVD_INSUFFICIENTDATA);

    }
    if(ps_dec->u4_prev_nal_skipped)
    {
        /*We have encountered a referenced frame,return to app*/
        ps_dec_op->u4_error_code = IVD_DEC_FRM_SKIPPED;
        ps_dec_op->u4_error_code |= (1 << IVD_UNSUPPORTEDPARAM);
        ps_dec_op->u4_frame_decoded_flag = 0;
        ps_dec_op->u4_size = sizeof(ivd_video_decode_op_t);
        /* close deblock thread if it is not closed yet*/
        if(ps_dec->u4_num_cores == 3)
        {
            ih264d_signal_bs_deblk_thread(ps_dec);
        }
        return (IV_FAIL);

    }

    if((ps_dec->u4_slice_start_code_found == 1)
                    && (ERROR_DANGLING_FIELD_IN_PIC != i4_err_status))
    {
        /*
         * For field pictures, set the bottom and top picture decoded u4_flag correctly.
         */

        if(ps_dec->ps_cur_slice->u1_field_pic_flag)
        {
            if(1 == ps_dec->ps_cur_slice->u1_bottom_field_flag)
            {
                ps_dec->u1_top_bottom_decoded |= BOT_FIELD_ONLY;
            }
            else
            {
                ps_dec->u1_top_bottom_decoded |= TOP_FIELD_ONLY;
            }
        }

        /* if new frame in not found (if we are still getting slices from previous frame)
         * ih264d_deblock_display is not called. Such frames will not be added to reference /display
         */
        if (((ps_dec->ps_dec_err_status->u1_err_flag & REJECT_CUR_PIC) == 0)
                && (ps_dec->u4_pic_buf_got == 1))
        {
            /* Calling Function to deblock Picture and Display */
            ret = ih264d_deblock_display(ps_dec);
            if(ret != 0)
            {
                return IV_FAIL;
            }
        }


        /*set to complete ,as we dont support partial frame decode*/
        if(ps_dec->i4_header_decoded == 3)
        {
            ps_dec->u2_total_mbs_coded = ps_dec->ps_cur_sps->u2_max_mb_addr + 1;
        }

        /*Update the i4_frametype at the end of picture*/
        if(ps_dec->ps_cur_slice->u1_nal_unit_type == IDR_SLICE_NAL)
        {
            ps_dec->i4_frametype = IV_IDR_FRAME;
        }
        else if(ps_dec->i4_pic_type == B_SLICE)
        {
            ps_dec->i4_frametype = IV_B_FRAME;
        }
        else if(ps_dec->i4_pic_type == P_SLICE)
        {
            ps_dec->i4_frametype = IV_P_FRAME;
        }
        else if(ps_dec->i4_pic_type == I_SLICE)
        {
            ps_dec->i4_frametype = IV_I_FRAME;
        }
        else
        {
            H264_DEC_DEBUG_PRINT("Shouldn't come here\n");
        }

        //Update the content type
        ps_dec->i4_content_type = ps_dec->ps_cur_slice->u1_field_pic_flag;

        ps_dec->u4_total_frames_decoded = ps_dec->u4_total_frames_decoded + 2;
        ps_dec->u4_total_frames_decoded = ps_dec->u4_total_frames_decoded
                        - ps_dec->ps_cur_slice->u1_field_pic_flag;

    }

    /* close deblock thread if it is not closed yet*/
    if(ps_dec->u4_num_cores == 3)
    {
        ih264d_signal_bs_deblk_thread(ps_dec);
    }


    {
        /* In case the decoder is configured to run in low delay mode,
         * then get display buffer and then format convert.
         * Note in this mode, format conversion does not run paralelly in a thread and adds to the codec cycles
         */

        if((0 == ps_dec->u4_num_reorder_frames_at_init)
                        && ps_dec->u1_init_dec_flag)
        {

            ih264d_get_next_display_field(ps_dec, ps_dec->ps_out_buffer,
                                          &(ps_dec->s_disp_op));
            if(0 == ps_dec->s_disp_op.u4_error_code)
            {
                ps_dec->u4_fmt_conv_cur_row = 0;
                ps_dec->u4_output_present = 1;
            }
        }

        ih264d_fill_output_struct_from_context(ps_dec, ps_dec_op);

        /* If Format conversion is not complete,
         complete it here */
        if(ps_dec->u4_output_present &&
          (ps_dec->u4_fmt_conv_cur_row < ps_dec->s_disp_frame_info.u4_y_ht))
        {
            ps_dec->u4_fmt_conv_num_rows = ps_dec->s_disp_frame_info.u4_y_ht
                            - ps_dec->u4_fmt_conv_cur_row;
            ih264d_format_convert(ps_dec, &(ps_dec->s_disp_op),
                                  ps_dec->u4_fmt_conv_cur_row,
                                  ps_dec->u4_fmt_conv_num_rows);
            ps_dec->u4_fmt_conv_cur_row += ps_dec->u4_fmt_conv_num_rows;
        }

        ih264d_release_display_field(ps_dec, &(ps_dec->s_disp_op));
    }

    if(ps_dec->i4_decode_header == 1 && (ps_dec->i4_header_decoded & 1) == 1)
    {
        ps_dec_op->u4_progressive_frame_flag = 1;
        if((NULL != ps_dec->ps_cur_sps) && (1 == (ps_dec->ps_cur_sps->u1_is_valid)))
        {
            if((0 == ps_dec->ps_sps->u1_frame_mbs_only_flag)
                            && (0 == ps_dec->ps_sps->u1_mb_aff_flag))
                ps_dec_op->u4_progressive_frame_flag = 0;

        }
    }

    /*Data memory barrier instruction,so that yuv write by the library is complete*/
    DATA_SYNC();

    H264_DEC_DEBUG_PRINT("The num bytes consumed: %d\n",
                         ps_dec_op->u4_num_bytes_consumed);
    return api_ret_value;
}

WORD32 ih264d_get_version(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{
    char version_string[MAXVERSION_STRLEN + 1];
    UWORD32 version_string_len;

    ivd_ctl_getversioninfo_ip_t *ps_ip;
    ivd_ctl_getversioninfo_op_t *ps_op;

    ps_ip = (ivd_ctl_getversioninfo_ip_t *)pv_api_ip;
    ps_op = (ivd_ctl_getversioninfo_op_t *)pv_api_op;
    UNUSED(dec_hdl);
    ps_op->u4_error_code = IV_SUCCESS;

    VERSION(version_string, CODEC_NAME, CODEC_RELEASE_TYPE, CODEC_RELEASE_VER,
            CODEC_VENDOR);

    if((WORD32)ps_ip->u4_version_buffer_size <= 0)
    {
        ps_op->u4_error_code = IH264D_VERS_BUF_INSUFFICIENT;
        return (IV_FAIL);
    }

    version_string_len = strlen(version_string) + 1;

    if(ps_ip->u4_version_buffer_size >= version_string_len) //(WORD32)sizeof(sizeof(version_string)))
    {
        memcpy(ps_ip->pv_version_buffer, version_string, version_string_len);
        ps_op->u4_error_code = IV_SUCCESS;
    }
    else
    {
        ps_op->u4_error_code = IH264D_VERS_BUF_INSUFFICIENT;
        return IV_FAIL;
    }
    return (IV_SUCCESS);
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :   ih264d_get_display_frame                               */
/*                                                                           */
/*  Description   :                                                          */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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
WORD32 ih264d_get_display_frame(iv_obj_t *dec_hdl,
                                void *pv_api_ip,
                                void *pv_api_op)
{

    UNUSED(dec_hdl);
    UNUSED(pv_api_ip);
    UNUSED(pv_api_op);
    // This function is no longer needed, output is returned in the process()
    return IV_FAIL;
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :  ih264d_set_display_frame                                */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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
WORD32 ih264d_set_display_frame(iv_obj_t *dec_hdl,
                                void *pv_api_ip,
                                void *pv_api_op)
{

    ivd_set_display_frame_ip_t *dec_disp_ip;
    ivd_set_display_frame_op_t *dec_disp_op;

    UWORD32 i, num_mvbank_req;
    dec_struct_t * ps_dec = (dec_struct_t *)(dec_hdl->pv_codec_handle);

    dec_disp_ip = (ivd_set_display_frame_ip_t *)pv_api_ip;
    dec_disp_op = (ivd_set_display_frame_op_t *)pv_api_op;
    dec_disp_op->u4_error_code = 0;
    if((NULL != ps_dec->ps_cur_sps) && (1 == (ps_dec->ps_cur_sps->u1_is_valid)))
    {
        UWORD32 level, width_mbs, height_mbs;

        level = ps_dec->u4_level_at_init;
        width_mbs = ps_dec->u2_frm_wd_in_mbs;
        height_mbs = ps_dec->u2_frm_ht_in_mbs;

        if((ps_dec->ps_cur_sps->u1_vui_parameters_present_flag == 1)
                        && (ps_dec->ps_cur_sps->s_vui.u4_num_reorder_frames != 64))
        {
            num_mvbank_req = ps_dec->ps_cur_sps->s_vui.u4_num_reorder_frames + 2;
        }
        else
        {
            /*if VUI is not present assume maximum possible refrence frames for the level,
             * as max reorder frames*/
            num_mvbank_req = ih264d_get_dpb_size_new(level, width_mbs,
                                                     height_mbs);
        }

        num_mvbank_req += ps_dec->ps_cur_sps->u1_num_ref_frames + 1;
    }
    else
    {
        UWORD32 num_bufs_app, num_bufs_level;
        UWORD32 num_ref_frames, num_reorder_frames, luma_width;
        UWORD32 luma_height, level;

        num_ref_frames = ps_dec->u4_num_ref_frames_at_init;
        num_reorder_frames = ps_dec->u4_num_reorder_frames_at_init;
        level = ps_dec->u4_level_at_init;
        luma_width = ps_dec->u4_width_at_init;
        luma_height = ps_dec->u4_height_at_init;

        num_bufs_app = num_ref_frames + num_reorder_frames + 1;

        if(num_bufs_app <= 1)
            num_bufs_app = 2;

        num_bufs_level = ih264d_get_dpb_size_new(level, (luma_width >> 4),
                                                 (luma_height >> 4));

        num_bufs_level = num_bufs_level * 2 + 1;

        num_mvbank_req = MIN(num_bufs_level, num_bufs_app);

        num_mvbank_req += ps_dec->u4_num_extra_disp_bufs_at_init;

    }

    ps_dec->u4_num_disp_bufs = 0;
    if(ps_dec->u4_share_disp_buf)
    {
        UWORD32 u4_num_bufs = dec_disp_ip->num_disp_bufs;
        if(u4_num_bufs > MAX_DISP_BUFS_NEW)
            u4_num_bufs = MAX_DISP_BUFS_NEW;

        u4_num_bufs = MIN(u4_num_bufs, MAX_DISP_BUFS_NEW);
        u4_num_bufs = MIN(u4_num_bufs, num_mvbank_req);

        ps_dec->u4_num_disp_bufs = u4_num_bufs;
        for(i = 0; i < u4_num_bufs; i++)
        {
            ps_dec->disp_bufs[i].u4_num_bufs =
                            dec_disp_ip->s_disp_buffer[i].u4_num_bufs;

            ps_dec->disp_bufs[i].buf[0] =
                            dec_disp_ip->s_disp_buffer[i].pu1_bufs[0];
            ps_dec->disp_bufs[i].buf[1] =
                            dec_disp_ip->s_disp_buffer[i].pu1_bufs[1];
            ps_dec->disp_bufs[i].buf[2] =
                            dec_disp_ip->s_disp_buffer[i].pu1_bufs[2];

            ps_dec->disp_bufs[i].u4_bufsize[0] =
                            dec_disp_ip->s_disp_buffer[i].u4_min_out_buf_size[0];
            ps_dec->disp_bufs[i].u4_bufsize[1] =
                            dec_disp_ip->s_disp_buffer[i].u4_min_out_buf_size[1];
            ps_dec->disp_bufs[i].u4_bufsize[2] =
                            dec_disp_ip->s_disp_buffer[i].u4_min_out_buf_size[2];

        }
    }
    return IV_SUCCESS;

}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_set_flush_mode                                    */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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
WORD32 ih264d_set_flush_mode(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{

    dec_struct_t * ps_dec;
    ivd_ctl_flush_op_t *ps_ctl_op = (ivd_ctl_flush_op_t*)pv_api_op;
    ps_ctl_op->u4_error_code = 0;

    ps_dec = (dec_struct_t *)(dec_hdl->pv_codec_handle);
    UNUSED(pv_api_ip);
    /* ! */
    /* Signal flush frame control call */
    ps_dec->u1_flushfrm = 1;

    if(  ps_dec->u1_init_dec_flag == 1)
    {

    ih264d_release_pics_in_dpb((void *)ps_dec,
                               ps_dec->u1_pic_bufs);
    ih264d_release_display_bufs(ps_dec);
    }

    ps_ctl_op->u4_error_code =
                    ((ivd_ctl_flush_op_t*)ps_dec->pv_dec_out)->u4_error_code; //verify the value

    return IV_SUCCESS;

}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_get_status                                        */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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

WORD32 ih264d_get_status(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{

    UWORD32 i;
    dec_struct_t * ps_dec;
    UWORD32 pic_wd, pic_ht;
    ivd_ctl_getstatus_op_t *ps_ctl_op = (ivd_ctl_getstatus_op_t*)pv_api_op;
    UNUSED(pv_api_ip);
    ps_ctl_op->u4_error_code = 0;

    ps_dec = (dec_struct_t *)(dec_hdl->pv_codec_handle);

    pic_wd = ps_dec->u4_width_at_init;
    pic_ht = ps_dec->u4_height_at_init;

    if((NULL != ps_dec->ps_cur_sps) && (1 == (ps_dec->ps_cur_sps->u1_is_valid)))
    {
        ps_ctl_op->u4_pic_ht = ps_dec->u2_disp_height;
        ps_ctl_op->u4_pic_wd = ps_dec->u2_disp_width;

        if(0 == ps_dec->u4_share_disp_buf)
        {
            pic_wd = ps_dec->u2_disp_width;
            pic_ht = ps_dec->u2_disp_height;

        }
        else
        {
            pic_wd = ps_dec->u2_frm_wd_y;
            pic_ht = ps_dec->u2_frm_ht_y;
        }
    }
    else
    {
        ps_ctl_op->u4_pic_ht = pic_wd;
        ps_ctl_op->u4_pic_wd = pic_ht;

        if(1 == ps_dec->u4_share_disp_buf)
        {
            pic_wd += (PAD_LEN_Y_H << 1);
            pic_ht += (PAD_LEN_Y_V << 2);

        }

    }

    if(ps_dec->u4_app_disp_width > pic_wd)
        pic_wd = ps_dec->u4_app_disp_width;
    if(0 == ps_dec->u4_share_disp_buf)
        ps_ctl_op->u4_num_disp_bufs = 1;
    else
    {
        if((NULL != ps_dec->ps_cur_sps) && (1 == (ps_dec->ps_cur_sps->u1_is_valid)))
        {
            UWORD32 level, width_mbs, height_mbs;

            level = ps_dec->u4_level_at_init;
            width_mbs = ps_dec->u2_frm_wd_in_mbs;
            height_mbs = ps_dec->u2_frm_ht_in_mbs;

            if((ps_dec->ps_cur_sps->u1_vui_parameters_present_flag == 1)
                            && (ps_dec->ps_cur_sps->s_vui.u4_num_reorder_frames
                                            != 64))
            {
                ps_ctl_op->u4_num_disp_bufs =
                                ps_dec->ps_cur_sps->s_vui.u4_num_reorder_frames + 2;
            }
            else
            {
                /*if VUI is not present assume maximum possible refrence frames for the level,
                 * as max reorder frames*/
                ps_ctl_op->u4_num_disp_bufs = ih264d_get_dpb_size_new(
                                level, width_mbs, height_mbs);
            }

            ps_ctl_op->u4_num_disp_bufs +=
                            ps_dec->ps_cur_sps->u1_num_ref_frames + 1;
        }
        else
        {
            ps_ctl_op->u4_num_disp_bufs = ih264d_get_dpb_size_new(
                            ps_dec->u4_level_at_init,
                            (ps_dec->u4_width_at_init >> 4),
                            (ps_dec->u4_height_at_init >> 4));

            ps_ctl_op->u4_num_disp_bufs +=
                            ps_ctl_op->u4_num_disp_bufs;

            ps_ctl_op->u4_num_disp_bufs =
                            MIN(ps_ctl_op->u4_num_disp_bufs,
                                (ps_dec->u4_num_ref_frames_at_init
                                                + ps_dec->u4_num_reorder_frames_at_init));

        }

        ps_ctl_op->u4_num_disp_bufs = MAX(
                        ps_ctl_op->u4_num_disp_bufs, 6);
        ps_ctl_op->u4_num_disp_bufs = MIN(
                        ps_ctl_op->u4_num_disp_bufs, 32);
    }

    ps_ctl_op->u4_error_code = ps_dec->i4_error_code;

    ps_ctl_op->u4_frame_rate = 0; //make it proper
    ps_ctl_op->u4_bit_rate = 0; //make it proper
    ps_ctl_op->e_content_type = ps_dec->i4_content_type;
    ps_ctl_op->e_output_chroma_format = ps_dec->u1_chroma_format;
    ps_ctl_op->u4_min_num_in_bufs = MIN_IN_BUFS;

    if(ps_dec->u1_chroma_format == IV_YUV_420P)
    {
        ps_ctl_op->u4_min_num_out_bufs = MIN_OUT_BUFS_420;
    }
    else if(ps_dec->u1_chroma_format == IV_YUV_422ILE)
    {
        ps_ctl_op->u4_min_num_out_bufs = MIN_OUT_BUFS_422ILE;
    }
    else if(ps_dec->u1_chroma_format == IV_RGB_565)
    {
        ps_ctl_op->u4_min_num_out_bufs = MIN_OUT_BUFS_RGB565;
    }
    else if((ps_dec->u1_chroma_format == IV_YUV_420SP_UV)
                    || (ps_dec->u1_chroma_format == IV_YUV_420SP_VU))
    {
        ps_ctl_op->u4_min_num_out_bufs = MIN_OUT_BUFS_420SP;
    }

    else
    {
        //Invalid chroma format; Error code may be updated, verify in testing if needed
        ps_ctl_op->u4_error_code = ERROR_FEATURE_UNAVAIL;
        return IV_FAIL;
    }

    for(i = 0; i < ps_ctl_op->u4_min_num_in_bufs; i++)
    {
        ps_ctl_op->u4_min_in_buf_size[i] = MIN_IN_BUF_SIZE;
    }

    /*!*/
    if(ps_dec->u1_chroma_format == IV_YUV_420P)
    {
        ps_ctl_op->u4_min_out_buf_size[0] = (pic_wd * pic_ht);
        ps_ctl_op->u4_min_out_buf_size[1] = (pic_wd * pic_ht)
                        >> 2;
        ps_ctl_op->u4_min_out_buf_size[2] = (pic_wd * pic_ht)
                        >> 2;
    }
    else if(ps_dec->u1_chroma_format == IV_YUV_422ILE)
    {
        ps_ctl_op->u4_min_out_buf_size[0] = (pic_wd * pic_ht)
                        * 2;
        ps_ctl_op->u4_min_out_buf_size[1] =
                        ps_ctl_op->u4_min_out_buf_size[2] = 0;
    }
    else if(ps_dec->u1_chroma_format == IV_RGB_565)
    {
        ps_ctl_op->u4_min_out_buf_size[0] = (pic_wd * pic_ht)
                        * 2;
        ps_ctl_op->u4_min_out_buf_size[1] =
                        ps_ctl_op->u4_min_out_buf_size[2] = 0;
    }
    else if((ps_dec->u1_chroma_format == IV_YUV_420SP_UV)
                    || (ps_dec->u1_chroma_format == IV_YUV_420SP_VU))
    {
        ps_ctl_op->u4_min_out_buf_size[0] = (pic_wd * pic_ht);
        ps_ctl_op->u4_min_out_buf_size[1] = (pic_wd * pic_ht)
                        >> 1;
        ps_ctl_op->u4_min_out_buf_size[2] = 0;
    }

    ps_dec->u4_num_disp_bufs_requested = ps_ctl_op->u4_num_disp_bufs;
    return IV_SUCCESS;
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :    ih264d_get_buf_info                                   */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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
WORD32 ih264d_get_buf_info(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{

    dec_struct_t * ps_dec;
    UWORD8 i = 0; // Default for 420P format
    UWORD16 pic_wd, pic_ht;
    ivd_ctl_getbufinfo_op_t *ps_ctl_op =
                    (ivd_ctl_getbufinfo_op_t*)pv_api_op;
    UNUSED(pv_api_ip);
    ps_ctl_op->u4_error_code = 0;

    ps_dec = (dec_struct_t *)(dec_hdl->pv_codec_handle);

    ps_ctl_op->u4_min_num_in_bufs = MIN_IN_BUFS;
    if(ps_dec->u1_chroma_format == IV_YUV_420P)
        ps_ctl_op->u4_min_num_out_bufs = MIN_OUT_BUFS_420;
    else if(ps_dec->u1_chroma_format == IV_YUV_422ILE)
        ps_ctl_op->u4_min_num_out_bufs = MIN_OUT_BUFS_422ILE;
    else if(ps_dec->u1_chroma_format == IV_RGB_565)
        ps_ctl_op->u4_min_num_out_bufs = MIN_OUT_BUFS_RGB565;
    else if((ps_dec->u1_chroma_format == IV_YUV_420SP_UV)
                    || (ps_dec->u1_chroma_format == IV_YUV_420SP_VU))
        ps_ctl_op->u4_min_num_out_bufs = MIN_OUT_BUFS_420SP;

    else
    {
        //Invalid chroma format; Error code may be updated, verify in testing if needed
        return IV_FAIL;
    }

    ps_ctl_op->u4_num_disp_bufs = 1;

    for(i = 0; i < ps_ctl_op->u4_min_num_in_bufs; i++)
    {
        ps_ctl_op->u4_min_in_buf_size[i] = MIN_IN_BUF_SIZE;
    }

    pic_wd = ps_dec->u4_width_at_init;
    pic_ht = ps_dec->u4_height_at_init;

    if((NULL != ps_dec->ps_cur_sps) && (1 == (ps_dec->ps_cur_sps->u1_is_valid)))
    {

        if(0 == ps_dec->u4_share_disp_buf)
        {
            pic_wd = ps_dec->u2_disp_width;
            pic_ht = ps_dec->u2_disp_height;

        }
        else
        {
            pic_wd = ps_dec->u2_frm_wd_y;
            pic_ht = ps_dec->u2_frm_ht_y;
        }

    }
    else
    {
        if(1 == ps_dec->u4_share_disp_buf)
        {
            pic_wd += (PAD_LEN_Y_H << 1);
            pic_ht += (PAD_LEN_Y_V << 2);

        }
    }

    if((WORD32)ps_dec->u4_app_disp_width > pic_wd)
        pic_wd = ps_dec->u4_app_disp_width;

    if(0 == ps_dec->u4_share_disp_buf)
        ps_ctl_op->u4_num_disp_bufs = 1;
    else
    {
        if((NULL != ps_dec->ps_cur_sps) && (1 == (ps_dec->ps_cur_sps->u1_is_valid)))
        {
            UWORD32 level, width_mbs, height_mbs;

            level = ps_dec->u4_level_at_init;
            width_mbs = ps_dec->u2_frm_wd_in_mbs;
            height_mbs = ps_dec->u2_frm_ht_in_mbs;

            if((ps_dec->ps_cur_sps->u1_vui_parameters_present_flag == 1)
                            && (ps_dec->ps_cur_sps->s_vui.u4_num_reorder_frames
                                            != 64))
            {
                ps_ctl_op->u4_num_disp_bufs =
                                ps_dec->ps_cur_sps->s_vui.u4_num_reorder_frames + 2;
            }
            else
            {
                /*if VUI is not present assume maximum possible refrence frames for the level,
                 * as max reorder frames*/
                ps_ctl_op->u4_num_disp_bufs = ih264d_get_dpb_size_new(
                                level, width_mbs, height_mbs);
            }

            ps_ctl_op->u4_num_disp_bufs +=
                            ps_dec->ps_cur_sps->u1_num_ref_frames + 1;

        }
        else
        {
            ps_ctl_op->u4_num_disp_bufs = ih264d_get_dpb_size_new(
                            ps_dec->u4_level_at_init,
                            (ps_dec->u4_width_at_init >> 4),
                            (ps_dec->u4_height_at_init >> 4));

            ps_ctl_op->u4_num_disp_bufs +=
                            ps_ctl_op->u4_num_disp_bufs;

            ps_ctl_op->u4_num_disp_bufs =
                            MIN(ps_ctl_op->u4_num_disp_bufs,
                                (ps_dec->u4_num_ref_frames_at_init
                                                + ps_dec->u4_num_reorder_frames_at_init));

        }

        ps_ctl_op->u4_num_disp_bufs = MAX(
                        ps_ctl_op->u4_num_disp_bufs, 6);
        ps_ctl_op->u4_num_disp_bufs = MIN(
                        ps_ctl_op->u4_num_disp_bufs, 32);
    }

    /*!*/
    if(ps_dec->u1_chroma_format == IV_YUV_420P)
    {
        ps_ctl_op->u4_min_out_buf_size[0] = (pic_wd * pic_ht);
        ps_ctl_op->u4_min_out_buf_size[1] = (pic_wd * pic_ht)
                        >> 2;
        ps_ctl_op->u4_min_out_buf_size[2] = (pic_wd * pic_ht)
                        >> 2;
    }
    else if(ps_dec->u1_chroma_format == IV_YUV_422ILE)
    {
        ps_ctl_op->u4_min_out_buf_size[0] = (pic_wd * pic_ht)
                        * 2;
        ps_ctl_op->u4_min_out_buf_size[1] =
                        ps_ctl_op->u4_min_out_buf_size[2] = 0;
    }
    else if(ps_dec->u1_chroma_format == IV_RGB_565)
    {
        ps_ctl_op->u4_min_out_buf_size[0] = (pic_wd * pic_ht)
                        * 2;
        ps_ctl_op->u4_min_out_buf_size[1] =
                        ps_ctl_op->u4_min_out_buf_size[2] = 0;
    }
    else if((ps_dec->u1_chroma_format == IV_YUV_420SP_UV)
                    || (ps_dec->u1_chroma_format == IV_YUV_420SP_VU))
    {
        ps_ctl_op->u4_min_out_buf_size[0] = (pic_wd * pic_ht);
        ps_ctl_op->u4_min_out_buf_size[1] = (pic_wd * pic_ht)
                        >> 1;
        ps_ctl_op->u4_min_out_buf_size[2] = 0;
    }
    ps_dec->u4_num_disp_bufs_requested = ps_ctl_op->u4_num_disp_bufs;

    return IV_SUCCESS;
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_set_params                                        */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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
WORD32 ih264d_set_params(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{

    dec_struct_t * ps_dec;
    WORD32 ret = IV_SUCCESS;

    ivd_ctl_set_config_ip_t *ps_ctl_ip =
                    (ivd_ctl_set_config_ip_t *)pv_api_ip;
    ivd_ctl_set_config_op_t *ps_ctl_op =
                    (ivd_ctl_set_config_op_t *)pv_api_op;

    ps_dec = (dec_struct_t *)(dec_hdl->pv_codec_handle);

    ps_dec->u4_skip_frm_mask = 0;

    ps_ctl_op->u4_error_code = 0;

    ps_dec->i4_app_skip_mode = ps_ctl_ip->e_frm_skip_mode;

    /*Is it really supported test it when you so the corner testing using test app*/

    if(ps_ctl_ip->e_frm_skip_mode != IVD_SKIP_NONE)
    {

        if(ps_ctl_ip->e_frm_skip_mode == IVD_SKIP_P)
            ps_dec->u4_skip_frm_mask |= 1 << P_SLC_BIT;
        else if(ps_ctl_ip->e_frm_skip_mode == IVD_SKIP_B)
            ps_dec->u4_skip_frm_mask |= 1 << B_SLC_BIT;
        else if(ps_ctl_ip->e_frm_skip_mode == IVD_SKIP_PB)
        {
            ps_dec->u4_skip_frm_mask |= 1 << B_SLC_BIT;
            ps_dec->u4_skip_frm_mask |= 1 << P_SLC_BIT;
        }
        else if(ps_ctl_ip->e_frm_skip_mode == IVD_SKIP_I)
            ps_dec->u4_skip_frm_mask |= 1 << I_SLC_BIT;
        else
        {
            //dynamic parameter not supported
            //Put an appropriate error code to return the error..
            //when you do the error code tests and after that remove this comment
            ps_ctl_op->u4_error_code = (1 << IVD_UNSUPPORTEDPARAM);
            ret = IV_FAIL;
        }
    }

    if((0 != ps_dec->u4_app_disp_width)
                    && (ps_ctl_ip->u4_disp_wd
                                    != ps_dec->u4_app_disp_width))
    {
        ps_ctl_op->u4_error_code |= (1 << IVD_UNSUPPORTEDPARAM);
        ps_ctl_op->u4_error_code |= ERROR_DISP_WIDTH_INVALID;
        ret = IV_FAIL;
    }
    else
    {
        if((ps_ctl_ip->u4_disp_wd >= ps_dec->u2_pic_wd)/* && (ps_ctl_ip->u4_disp_wd <= ps_dec->u4_width_at_init) */)
        {
            ps_dec->u4_app_disp_width = ps_ctl_ip->u4_disp_wd;
        }
        else if((0 == ps_dec->i4_header_decoded) /*&& (ps_ctl_ip->u4_disp_wd <= ps_dec->u4_width_at_init)*/)
        {
            ps_dec->u4_app_disp_width = ps_ctl_ip->u4_disp_wd;
        }
        else if(ps_ctl_ip->u4_disp_wd == 0)
        {
            ps_dec->u4_app_disp_width = 0;
        }
        else
        {
            /*
             * Set the display width to zero. This will ensure that the wrong value we had stored (0xFFFFFFFF)
             * does not propogate.
             */
            ps_dec->u4_app_disp_width = 0;
            ps_ctl_op->u4_error_code |= (1 << IVD_UNSUPPORTEDPARAM);
            ps_ctl_op->u4_error_code |= ERROR_DISP_WIDTH_INVALID;
            ret = IV_FAIL;
        }
    }
    if(ps_ctl_ip->e_vid_dec_mode == IVD_DECODE_FRAME)
        ps_dec->i4_decode_header = 0;
    else if(ps_ctl_ip->e_vid_dec_mode == IVD_DECODE_HEADER)
        ps_dec->i4_decode_header = 1;
    else
    {
        ps_ctl_op->u4_error_code = (1 << IVD_UNSUPPORTEDPARAM);
        ps_dec->i4_decode_header = 1;
        ret = IV_FAIL;
    }

    return ret;

}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_set_default_params                                */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
/*  Outputs       :                                                          */
/*  Returns       : void                                                     */
/*                                                                           */
/*  Issues        : none                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         08 08 2011   100421          Copied from set_params               */
/*                                                                           */
/*****************************************************************************/
WORD32 ih264d_set_default_params(iv_obj_t *dec_hdl,
                                 void *pv_api_ip,
                                 void *pv_api_op)
{

    dec_struct_t * ps_dec;
    WORD32 ret = IV_SUCCESS;

    ivd_ctl_set_config_op_t *ps_ctl_op =
                    (ivd_ctl_set_config_op_t *)pv_api_op;
    ps_dec = (dec_struct_t *)(dec_hdl->pv_codec_handle);
    UNUSED(pv_api_ip);


    {
        ps_dec->u4_app_disp_width = 0;
        ps_dec->u4_skip_frm_mask = 0;
        ps_dec->i4_decode_header = 1;

        ps_ctl_op->u4_error_code = 0;
    }


    return ret;
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :  ih264d_reset                                            */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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
WORD32 ih264d_reset(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{
    dec_struct_t * ps_dec;
    ivd_ctl_reset_op_t *ps_ctl_op = (ivd_ctl_reset_op_t *)pv_api_op;
    UNUSED(pv_api_ip);
    ps_ctl_op->u4_error_code = 0;

    ps_dec = (dec_struct_t *)(dec_hdl->pv_codec_handle);
//CHECK
    if(ps_dec != NULL)
    {

        ih264d_init_decoder(ps_dec);

        /*
         memset(ps_dec->disp_bufs, 0, (MAX_DISP_BUFS_NEW) * sizeof(disp_buf_t));
         memset(ps_dec->u4_disp_buf_mapping, 0, (MAX_DISP_BUFS_NEW) * sizeof(UWORD32));
         memset(ps_dec->u4_disp_buf_to_be_freed, 0, (MAX_DISP_BUFS_NEW) * sizeof(UWORD32));
         */
    }
    else
    {
        H264_DEC_DEBUG_PRINT(
                        "\nReset called without Initializing the decoder\n");
        ps_ctl_op->u4_error_code = ERROR_INIT_NOT_DONE;
    }

    return IV_SUCCESS;
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name :  ih264d_ctl                                              */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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
WORD32 ih264d_ctl(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{
    ivd_ctl_set_config_ip_t *ps_ctl_ip;
    ivd_ctl_set_config_op_t *ps_ctl_op;
    WORD32 ret = IV_SUCCESS;
    UWORD32 subcommand;
    dec_struct_t *ps_dec = dec_hdl->pv_codec_handle;

    if(ps_dec->init_done != 1)
    {
        //Return proper Error Code
        return IV_FAIL;
    }
    ps_ctl_ip = (ivd_ctl_set_config_ip_t*)pv_api_ip;
    ps_ctl_op = (ivd_ctl_set_config_op_t*)pv_api_op;
    ps_ctl_op->u4_error_code = 0;
    subcommand = ps_ctl_ip->e_sub_cmd;

    switch(subcommand)
    {
        case IVD_CMD_CTL_GETPARAMS:
            ret = ih264d_get_status(dec_hdl, (void *)pv_api_ip,
                                    (void *)pv_api_op);
            break;
        case IVD_CMD_CTL_SETPARAMS:
            ret = ih264d_set_params(dec_hdl, (void *)pv_api_ip,
                                    (void *)pv_api_op);
            break;
        case IVD_CMD_CTL_RESET:
            ret = ih264d_reset(dec_hdl, (void *)pv_api_ip, (void *)pv_api_op);
            break;
        case IVD_CMD_CTL_SETDEFAULT:
            ret = ih264d_set_default_params(dec_hdl, (void *)pv_api_ip,
                                            (void *)pv_api_op);
            break;
        case IVD_CMD_CTL_FLUSH:
            ret = ih264d_set_flush_mode(dec_hdl, (void *)pv_api_ip,
                                        (void *)pv_api_op);
            break;
        case IVD_CMD_CTL_GETBUFINFO:
            ret = ih264d_get_buf_info(dec_hdl, (void *)pv_api_ip,
                                      (void *)pv_api_op);
            break;
        case IVD_CMD_CTL_GETVERSION:
            ret = ih264d_get_version(dec_hdl, (void *)pv_api_ip,
                                     (void *)pv_api_op);
            break;
        case IH264D_CMD_CTL_DEGRADE:
            ret = ih264d_set_degrade(dec_hdl, (void *)pv_api_ip,
                                     (void *)pv_api_op);
            break;

        case IH264D_CMD_CTL_SET_NUM_CORES:
            ret = ih264d_set_num_cores(dec_hdl, (void *)pv_api_ip,
                                       (void *)pv_api_op);
            break;
        case IH264D_CMD_CTL_GET_BUFFER_DIMENSIONS:
            ret = ih264d_get_frame_dimensions(dec_hdl, (void *)pv_api_ip,
                                              (void *)pv_api_op);
            break;
        case IH264D_CMD_CTL_SET_PROCESSOR:
            ret = ih264d_set_processor(dec_hdl, (void *)pv_api_ip,
                                       (void *)pv_api_op);
            break;
        default:
            H264_DEC_DEBUG_PRINT("\ndo nothing\n")
            ;
            break;
    }

    return ret;
}
/*****************************************************************************/
/*                                                                           */
/*  Function Name :   ih264d_rel_display_frame                               */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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
WORD32 ih264d_rel_display_frame(iv_obj_t *dec_hdl,
                                void *pv_api_ip,
                                void *pv_api_op)
{

    ivd_rel_display_frame_ip_t *ps_rel_ip;
    ivd_rel_display_frame_op_t *ps_rel_op;
    UWORD32 buf_released = 0;

    UWORD32 u4_ts = -1;
    dec_struct_t *ps_dec = dec_hdl->pv_codec_handle;

    ps_rel_ip = (ivd_rel_display_frame_ip_t *)pv_api_ip;
    ps_rel_op = (ivd_rel_display_frame_op_t *)pv_api_op;
    ps_rel_op->u4_error_code = 0;
    u4_ts = ps_rel_ip->u4_disp_buf_id;

    if(0 == ps_dec->u4_share_disp_buf)
    {
        ps_dec->u4_disp_buf_mapping[u4_ts] = 0;
        ps_dec->u4_disp_buf_to_be_freed[u4_ts] = 0;
        return IV_SUCCESS;
    }

    if(ps_dec->pv_pic_buf_mgr != NULL)
    {
        if(1 == ps_dec->u4_disp_buf_mapping[u4_ts])
        {
            ih264_buf_mgr_release((buf_mgr_t *)ps_dec->pv_pic_buf_mgr,
                                  ps_rel_ip->u4_disp_buf_id,
                                  BUF_MGR_IO);
            ps_dec->u4_disp_buf_mapping[u4_ts] = 0;
            buf_released = 1;
        }
    }

    if((1 == ps_dec->u4_share_disp_buf) && (0 == buf_released))
        ps_dec->u4_disp_buf_to_be_freed[u4_ts] = 1;

    return IV_SUCCESS;
}

/**
 *******************************************************************************
 *
 * @brief
 *  Sets degrade params
 *
 * @par Description:
 *  Sets degrade params.
 *  Refer to ih264d_ctl_degrade_ip_t definition for details
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

WORD32 ih264d_set_degrade(iv_obj_t *ps_codec_obj,
                          void *pv_api_ip,
                          void *pv_api_op)
{
    ih264d_ctl_degrade_ip_t *ps_ip;
    ih264d_ctl_degrade_op_t *ps_op;
    dec_struct_t *ps_codec = (dec_struct_t *)ps_codec_obj->pv_codec_handle;

    ps_ip = (ih264d_ctl_degrade_ip_t *)pv_api_ip;
    ps_op = (ih264d_ctl_degrade_op_t *)pv_api_op;

    ps_codec->i4_degrade_type = ps_ip->i4_degrade_type;
    ps_codec->i4_nondegrade_interval = ps_ip->i4_nondegrade_interval;
    ps_codec->i4_degrade_pics = ps_ip->i4_degrade_pics;

    ps_op->u4_error_code = 0;
    ps_codec->i4_degrade_pic_cnt = 0;

    return IV_SUCCESS;
}

WORD32 ih264d_get_frame_dimensions(iv_obj_t *dec_hdl,
                                   void *pv_api_ip,
                                   void *pv_api_op)
{
    ih264d_ctl_get_frame_dimensions_ip_t *ps_ip;
    ih264d_ctl_get_frame_dimensions_op_t *ps_op;
    dec_struct_t *ps_dec = dec_hdl->pv_codec_handle;
    UWORD32 disp_wd, disp_ht, buffer_wd, buffer_ht, x_offset, y_offset;

    ps_ip = (ih264d_ctl_get_frame_dimensions_ip_t *)pv_api_ip;

    ps_op = (ih264d_ctl_get_frame_dimensions_op_t *)pv_api_op;
    UNUSED(ps_ip);
    if((NULL != ps_dec->ps_cur_sps) && (1 == (ps_dec->ps_cur_sps->u1_is_valid)))
    {
        disp_wd = ps_dec->u2_disp_width;
        disp_ht = ps_dec->u2_disp_height;

        if(0 == ps_dec->u4_share_disp_buf)
        {
            buffer_wd = disp_wd;
            buffer_ht = disp_ht;
        }
        else
        {
            buffer_wd = ps_dec->u2_frm_wd_y;
            buffer_ht = ps_dec->u2_frm_ht_y;
        }
    }
    else
    {
        disp_wd = ps_dec->u4_width_at_init;
        disp_ht = ps_dec->u4_height_at_init;

        if(0 == ps_dec->u4_share_disp_buf)
        {
            buffer_wd = disp_wd;
            buffer_ht = disp_ht;
        }
        else
        {
            buffer_wd = ALIGN16(disp_wd) + (PAD_LEN_Y_H << 1);
            buffer_ht = ALIGN16(disp_ht) + (PAD_LEN_Y_V << 2);
        }
    }
    if(ps_dec->u4_app_disp_width > buffer_wd)
        buffer_wd = ps_dec->u4_app_disp_width;

    if(0 == ps_dec->u4_share_disp_buf)
    {
        x_offset = 0;
        y_offset = 0;
    }
    else
    {
        y_offset = (PAD_LEN_Y_V << 1);
        x_offset = PAD_LEN_Y_H;

        if((NULL != ps_dec->ps_sps) && (1 == (ps_dec->ps_sps->u1_is_valid))
                        && (0 != ps_dec->u2_crop_offset_y))
        {
            y_offset += ps_dec->u2_crop_offset_y / ps_dec->u2_frm_wd_y;
            x_offset += ps_dec->u2_crop_offset_y % ps_dec->u2_frm_wd_y;
        }
    }

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
    ps_op->u4_x_offset[1] = ps_op->u4_x_offset[2] =
                    (ps_op->u4_x_offset[0] >> 1);
    ps_op->u4_y_offset[1] = ps_op->u4_y_offset[2] =
                    (ps_op->u4_y_offset[0] >> 1);

    if((ps_dec->u1_chroma_format == IV_YUV_420SP_UV)
                    || (ps_dec->u1_chroma_format == IV_YUV_420SP_VU))
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

WORD32 ih264d_set_num_cores(iv_obj_t *dec_hdl, void *pv_api_ip, void *pv_api_op)
{
    ih264d_ctl_set_num_cores_ip_t *ps_ip;
    ih264d_ctl_set_num_cores_op_t *ps_op;
    dec_struct_t *ps_dec = dec_hdl->pv_codec_handle;

    ps_ip = (ih264d_ctl_set_num_cores_ip_t *)pv_api_ip;
    ps_op = (ih264d_ctl_set_num_cores_op_t *)pv_api_op;
    ps_op->u4_error_code = 0;
    ps_dec->u4_num_cores = ps_ip->u4_num_cores;
    if(ps_dec->u4_num_cores == 1)
    {
        ps_dec->u1_separate_parse = 0;
        ps_dec->pi4_ctxt_save_register_dec = ps_dec->pi4_ctxt_save_register;
    }
    else
    {
        ps_dec->u1_separate_parse = 1;
    }

    /*using only upto three threads currently*/
    if(ps_dec->u4_num_cores > 3)
        ps_dec->u4_num_cores = 3;

    return IV_SUCCESS;
}

void ih264d_fill_output_struct_from_context(dec_struct_t *ps_dec,
                                            ivd_video_decode_op_t *ps_dec_op)
{
    if((ps_dec_op->u4_error_code & 0xff)
                    != ERROR_DYNAMIC_RESOLUTION_NOT_SUPPORTED)
    {
        ps_dec_op->u4_pic_wd = (UWORD32)ps_dec->u2_disp_width;
        ps_dec_op->u4_pic_ht = (UWORD32)ps_dec->u2_disp_height;
    }
    ps_dec_op->e_pic_type = ps_dec->i4_frametype;

    ps_dec_op->u4_new_seq = 0;
    ps_dec_op->u4_output_present = ps_dec->u4_output_present;
    ps_dec_op->u4_progressive_frame_flag =
                    ps_dec->s_disp_op.u4_progressive_frame_flag;

    ps_dec_op->u4_is_ref_flag = 1;
    if(ps_dec_op->u4_frame_decoded_flag)
    {
        if(ps_dec->ps_cur_slice->u1_nal_ref_idc == 0)
            ps_dec_op->u4_is_ref_flag = 0;
    }

    ps_dec_op->e_output_format = ps_dec->s_disp_op.e_output_format;
    ps_dec_op->s_disp_frm_buf = ps_dec->s_disp_op.s_disp_frm_buf;
    ps_dec_op->e4_fld_type = ps_dec->s_disp_op.e4_fld_type;
    ps_dec_op->u4_ts = ps_dec->s_disp_op.u4_ts;
    ps_dec_op->u4_disp_buf_id = ps_dec->s_disp_op.u4_disp_buf_id;
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_api_function                                      */
/*                                                                           */
/*  Description   :                                                          */
/*                                                                           */
/*  Inputs        :iv_obj_t decoder handle                                   */
/*                :pv_api_ip pointer to input structure                      */
/*                :pv_api_op pointer to output structure                     */
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
IV_API_CALL_STATUS_T ih264d_api_function(iv_obj_t *dec_hdl,
                                              void *pv_api_ip,
                                              void *pv_api_op)
{
    UWORD32 command;
    UWORD32 *pu2_ptr_cmd;
    UWORD32 u4_api_ret;
    IV_API_CALL_STATUS_T e_status;
    e_status = api_check_struct_sanity(dec_hdl, pv_api_ip, pv_api_op);

    if(e_status != IV_SUCCESS)
    {
        UWORD32 *ptr_err;

        ptr_err = (UWORD32 *)pv_api_op;
        UNUSED(ptr_err);
        H264_DEC_DEBUG_PRINT("error code = %d\n", *(ptr_err + 1));
        return IV_FAIL;
    }

    pu2_ptr_cmd = (UWORD32 *)pv_api_ip;
    pu2_ptr_cmd++;

    command = *pu2_ptr_cmd;
//    H264_DEC_DEBUG_PRINT("inside lib = %d\n",command);
    switch(command)
    {

        case IV_CMD_GET_NUM_MEM_REC:
            u4_api_ret = ih264d_get_num_rec((void *)pv_api_ip,
                                            (void *)pv_api_op);

            break;
        case IV_CMD_FILL_NUM_MEM_REC:

            u4_api_ret = ih264d_fill_num_mem_rec((void *)pv_api_ip,
                                                 (void *)pv_api_op);
            break;
        case IV_CMD_INIT:
            u4_api_ret = ih264d_init(dec_hdl, (void *)pv_api_ip,
                                     (void *)pv_api_op);
            break;

        case IVD_CMD_VIDEO_DECODE:
            u4_api_ret = ih264d_video_decode(dec_hdl, (void *)pv_api_ip,
                                             (void *)pv_api_op);
            break;

        case IVD_CMD_GET_DISPLAY_FRAME:
            u4_api_ret = ih264d_get_display_frame(dec_hdl, (void *)pv_api_ip,
                                                  (void *)pv_api_op);

            break;

        case IVD_CMD_SET_DISPLAY_FRAME:
            u4_api_ret = ih264d_set_display_frame(dec_hdl, (void *)pv_api_ip,
                                                  (void *)pv_api_op);

            break;

        case IVD_CMD_REL_DISPLAY_FRAME:
            u4_api_ret = ih264d_rel_display_frame(dec_hdl, (void *)pv_api_ip,
                                                  (void *)pv_api_op);
            break;

        case IV_CMD_RETRIEVE_MEMREC:
            u4_api_ret = ih264d_clr(dec_hdl, (void *)pv_api_ip,
                                    (void *)pv_api_op);
            break;

        case IVD_CMD_VIDEO_CTL:
            u4_api_ret = ih264d_ctl(dec_hdl, (void *)pv_api_ip,
                                    (void *)pv_api_op);
            break;
        default:
            u4_api_ret = IV_FAIL;
            break;
    }

    return u4_api_ret;
}
