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
/*  File Name         : mpeg2dec_api_utils.c                                 */
/*                                                                           */
/*                                                                           */
/*  Description       : This file defines the API interface for MPEG2 Decoder*/
/*                                                                           */
/*  List of Functions : <List the functions defined in this file>            */
/*                                                                           */
/*  Issues / Problems : None                                                 */
/*                                                                           */
/*  Revision History  :                                                      */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         17 09 2007  Rajendra C Y       Creation                           */
/*                                                                           */
/*****************************************************************************/

/*****************************************************************************/
/* File Includes                                                             */
/*****************************************************************************/
/* System include files */

#include <stddef.h>
#include <stdio.h>
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
#include "impeg2d_dec_hdr.h"

void impeg2d_next_start_code(dec_state_t *ps_dec);
void impeg2d_next_code(dec_state_t *ps_dec, UWORD32 u4_start_code_val);

/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_dec_hdr                                      */
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
void impeg2d_dec_hdr(void *pv_dec,impeg2d_video_decode_ip_t *ps_ip,
                 impeg2d_video_decode_op_t *ps_op)
{

    UWORD32 u4_bits_read;
    dec_state_t *ps_dec;
    UWORD32 u4_size = ps_ip->s_ivd_video_decode_ip_t.u4_num_Bytes;

    ps_dec = (dec_state_t *)pv_dec;
    ps_op->s_ivd_video_decode_op_t.u4_error_code = 0;
    if (u4_size > MAX_BITSTREAM_BUFFER_SIZE)
    {
        u4_size = MAX_BITSTREAM_BUFFER_SIZE;
    }

    memcpy(ps_dec->pu1_input_buffer, ps_ip->s_ivd_video_decode_ip_t.pv_stream_buffer, u4_size);

    impeg2d_bit_stream_init(&(ps_dec->s_bit_stream), ps_dec->pu1_input_buffer,
        u4_size);

    {
        {
            IMPEG2D_ERROR_CODES_T e_error;
            e_error = impeg2d_process_video_header(ps_dec);
            if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
            {
                ps_op->s_ivd_video_decode_op_t.u4_error_code    = e_error;

                u4_bits_read     = impeg2d_bit_stream_num_bits_read(&ps_dec->s_bit_stream);

                ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = u4_bits_read>> 3;
                if(ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed > ps_ip->s_ivd_video_decode_ip_t.u4_num_Bytes)
                {
                    ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = ps_ip->s_ivd_video_decode_ip_t.u4_num_Bytes;
                }
                if(ps_op->s_ivd_video_decode_op_t.u4_error_code == 0)
                    ps_op->s_ivd_video_decode_op_t.u4_error_code = e_error;

                if (IMPEG2D_UNSUPPORTED_DIMENSIONS == e_error)
                {
                    ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = 0;
                    ps_dec->u2_header_done = 0;

                    ps_op->s_ivd_video_decode_op_t.u4_pic_ht = ps_dec->u2_reinit_max_height;
                    ps_op->s_ivd_video_decode_op_t.u4_pic_wd = ps_dec->u2_reinit_max_width;
                }
                impeg2d_next_code(ps_dec, SEQUENCE_HEADER_CODE);
                return;
            }
        }
        ps_op->s_ivd_video_decode_op_t.u4_pic_ht = ps_dec->u2_vertical_size;
        ps_op->s_ivd_video_decode_op_t.u4_pic_wd = ps_dec->u2_horizontal_size;

        ps_op->s_ivd_video_decode_op_t.e_pic_type            = IV_NA_FRAME;
        ps_op->s_ivd_video_decode_op_t.u4_error_code        = IV_SUCCESS;

        u4_bits_read     = impeg2d_bit_stream_num_bits_read(&ps_dec->s_bit_stream);
        ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = u4_bits_read>> 3;
        if(ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed > ps_ip->s_ivd_video_decode_ip_t.u4_num_Bytes)
        {
            ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = ps_ip->s_ivd_video_decode_ip_t.u4_num_Bytes;
        }
        ps_op->s_ivd_video_decode_op_t.u4_frame_decoded_flag = 0;
        /* MOD */
        ps_dec->u2_header_done = 1;

    }
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_dec_frm                                         */
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
void impeg2d_dec_frm(void *pv_dec,impeg2d_video_decode_ip_t *ps_ip,
                 impeg2d_video_decode_op_t *ps_op)
{


    stream_t *ps_stream;
    UWORD32 u4_size = ps_ip->s_ivd_video_decode_ip_t.u4_num_Bytes;

    dec_state_t *ps_dec;

    ps_dec = (dec_state_t *)pv_dec;
    ps_op->s_ivd_video_decode_op_t.u4_error_code = 0;
    ps_dec->i4_bytes_consumed = 0;
    ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = 0;

    IMPEG2D_FRM_NUM_SET();
    if (u4_size > MAX_BITSTREAM_BUFFER_SIZE)
    {
        u4_size = MAX_BITSTREAM_BUFFER_SIZE;
    }

    memcpy(ps_dec->pu1_input_buffer, ps_ip->s_ivd_video_decode_ip_t.pv_stream_buffer, u4_size);

    ps_dec->pu1_inp_bits_buf = ps_dec->pu1_input_buffer;

    ps_dec->u4_num_inp_bytes = u4_size;
    ps_stream  = &ps_dec->s_bit_stream;

    impeg2d_bit_stream_init(ps_stream, ps_dec->pu1_input_buffer, u4_size);

    /* @ */ /* Updating the bufferID */

    ps_dec->u4_xdmBufID     = ps_ip->s_ivd_video_decode_ip_t.u4_ts;

    {
        IMPEG2D_ERROR_CODES_T e_error;
        /* Process the Bitstream */
        e_error = impeg2d_process_video_bit_stream(ps_dec);
        if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
        {
            ps_op->s_ivd_video_decode_op_t.u4_error_code    = e_error;

            if ((IMPEG2D_ERROR_CODES_T) IVD_RES_CHANGED == e_error)
            {
                ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = ps_dec->i4_bytes_consumed;
                ps_dec->u2_header_done = 0;
            }
            else if (IMPEG2D_UNSUPPORTED_DIMENSIONS == e_error)
            {
                ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = 0;
                ps_dec->u2_header_done = 0;

                ps_op->s_ivd_video_decode_op_t.u4_pic_ht = ps_dec->u2_reinit_max_height;
                ps_op->s_ivd_video_decode_op_t.u4_pic_wd = ps_dec->u2_reinit_max_width;
            }
            else
            {
                if(ps_dec->i4_num_cores > 1 && 0 != ps_dec->i4_bytes_consumed)
                {
                    /* If the number of bytes consumed has been updated by
                     * get_slice_pos function, then use that. Else, the bytes consumed is
                     * calculated from the offset. The bytes consumed for multi-thread runs
                     * is updated only into ps_dec->i4_bytes_consumed if the get_slice_pos
                     * function has been called. If that function has not run, then we have
                     * encountered an error but still have to consume the bytes in header
                     * decode, etc.
                     */
                    ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = ps_dec->i4_bytes_consumed;
                }
                else
                {
                    ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = (ps_dec->s_bit_stream.u4_offset + 7) >> 3;
                    ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed -= ((size_t)ps_dec->s_bit_stream.pv_bs_buf & 3);
                }

                if(ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed
                                > ps_ip->s_ivd_video_decode_ip_t.u4_num_Bytes)
                {
                    ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed =
                                    ps_ip->s_ivd_video_decode_ip_t.u4_num_Bytes;
                }

                impeg2d_next_start_code(ps_dec);
            }

            if(ps_op->s_ivd_video_decode_op_t.u4_error_code == 0)
            {
                ps_op->s_ivd_video_decode_op_t.u4_error_code = e_error;
            }

            return;
        }
    }
    /**************************************************************************/
    /* Remove the bytes left till next start code is encountered              */
    /**************************************************************************/
    ps_op->s_ivd_video_decode_op_t.u4_error_code  = IV_SUCCESS;

    if(ps_dec->i4_num_cores > 1 && 0 != ps_dec->i4_bytes_consumed)
    {
        /* If the number of bytes consumed has been updated by
         * get_slice_pos function, then use that. Else, the bytes consumed is
         * calculated from the offset. The bytes consumed for multi-thread runs
         * is updated only into ps_dec->i4_bytes_consumed if the get_slice_pos
         * function has been called. If that function has not run, then we have
         * encountered an error but still have to consume the bytes in header
         * decode, etc.
         */
        ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = ps_dec->i4_bytes_consumed;
    }
    else
    {
        ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = (ps_dec->s_bit_stream.u4_offset + 7) >> 3;
        ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed -= ((size_t)ps_dec->s_bit_stream.pv_bs_buf & 3);
    }
    if(ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed > ps_ip->s_ivd_video_decode_ip_t.u4_num_Bytes)
    {
        ps_op->s_ivd_video_decode_op_t.u4_num_bytes_consumed = ps_ip->s_ivd_video_decode_ip_t.u4_num_Bytes;
    }
    ps_op->s_ivd_video_decode_op_t.u4_pic_ht = ps_dec->u2_vertical_size;
    ps_op->s_ivd_video_decode_op_t.u4_pic_wd = ps_dec->u2_horizontal_size;

        switch(ps_dec->e_pic_type)
        {
        case I_PIC :
            ps_op->s_ivd_video_decode_op_t.e_pic_type = IV_I_FRAME;
            break;

        case P_PIC:
            ps_op->s_ivd_video_decode_op_t.e_pic_type = IV_P_FRAME;
            break;

        case B_PIC:
            ps_op->s_ivd_video_decode_op_t.e_pic_type = IV_B_FRAME;
            break;

        case D_PIC:
            ps_op->s_ivd_video_decode_op_t.e_pic_type = IV_I_FRAME;
            break;

        default :
            ps_op->s_ivd_video_decode_op_t.e_pic_type = IV_FRAMETYPE_DEFAULT;
            break;
        }

        ps_op->s_ivd_video_decode_op_t.u4_frame_decoded_flag = ps_dec->i4_frame_decoded;
        ps_op->s_ivd_video_decode_op_t.u4_new_seq = 0;
        ps_op->s_ivd_video_decode_op_t.u4_error_code = ps_dec->u4_error_code;


}
