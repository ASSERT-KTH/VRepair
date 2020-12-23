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
#include <stdio.h>
#include <string.h>

#include "iv_datatypedef.h"
#include "iv.h"

#include "impeg2_buf_mgr.h"
#include "impeg2_disp_mgr.h"
#include "impeg2_defs.h"
#include "impeg2_platform_macros.h"
#include "impeg2_inter_pred.h"
#include "impeg2_idct.h"
#include "impeg2_globals.h"
#include "impeg2_mem_func.h"
#include "impeg2_format_conv.h"
#include "impeg2_macros.h"

#include "ivd.h"
#include "impeg2d.h"
#include "impeg2d_bitstream.h"
#include "impeg2d_structs.h"
#include "impeg2d_globals.h"
#include "impeg2d_vld_tables.h"
#include "impeg2d_vld.h"
#include "impeg2d_pic_proc.h"
#include "impeg2d_debug.h"

void impeg2d_init_function_ptr(void *pv_codec);
void impeg2d_format_convert(dec_state_t *ps_dec,
                            pic_buf_t *ps_src_pic,
                            iv_yuv_buf_t    *ps_disp_frm_buf,
                            UWORD32 u4_start_row, UWORD32 u4_num_rows)
{
    UWORD8  *pu1_src_y,*pu1_src_u,*pu1_src_v;
    UWORD8  *pu1_dst_y,*pu1_dst_u,*pu1_dst_v;



    if((NULL == ps_src_pic) || (NULL == ps_src_pic->pu1_y) || (0 == u4_num_rows))
            return;

    pu1_src_y   = ps_src_pic->pu1_y + (u4_start_row * ps_dec->u2_frame_width);
    pu1_src_u   = ps_src_pic->pu1_u + ((u4_start_row >> 1) * (ps_dec->u2_frame_width >> 1));
    pu1_src_v   = ps_src_pic->pu1_v + ((u4_start_row >> 1) *(ps_dec->u2_frame_width >> 1));

    pu1_dst_y  =  (UWORD8 *)ps_disp_frm_buf->pv_y_buf + (u4_start_row *  ps_dec->u4_frm_buf_stride);
    pu1_dst_u =   (UWORD8 *)ps_disp_frm_buf->pv_u_buf +((u4_start_row >> 1)*(ps_dec->u4_frm_buf_stride >> 1));
    pu1_dst_v =   (UWORD8 *)ps_disp_frm_buf->pv_v_buf +((u4_start_row >> 1)*(ps_dec->u4_frm_buf_stride >> 1));

    if (IV_YUV_420P == ps_dec->i4_chromaFormat)
    {
        ps_dec->pf_copy_yuv420p_buf(pu1_src_y, pu1_src_u, pu1_src_v, pu1_dst_y,
                                    pu1_dst_u, pu1_dst_v,
                                    ps_dec->u2_horizontal_size,
                                    u4_num_rows,
                                    ps_dec->u2_frame_width,
                                    (ps_dec->u2_frame_width >> 1),
                                    (ps_dec->u2_frame_width >> 1),
                                    ps_dec->u4_frm_buf_stride,
                                    (ps_dec->u4_frm_buf_stride >> 1),
                                    (ps_dec->u4_frm_buf_stride >> 1));
    }
    else if (IV_YUV_422ILE == ps_dec->i4_chromaFormat)
    {
        void    *pv_yuv422i;
        UWORD32 u2_height,u2_width,u2_stride_y,u2_stride_u,u2_stride_v;
        UWORD32 u2_stride_yuv422i;


        pv_yuv422i          = (UWORD8 *)ps_disp_frm_buf->pv_y_buf + ((ps_dec->u2_vertical_size)*(ps_dec->u4_frm_buf_stride));
        u2_height           = u4_num_rows;
        u2_width            = ps_dec->u2_horizontal_size;
        u2_stride_y         = ps_dec->u2_frame_width;
        u2_stride_u         = u2_stride_y >> 1;
        u2_stride_v         = u2_stride_u;
        u2_stride_yuv422i   = (0 == ps_dec->u4_frm_buf_stride) ? ps_dec->u2_horizontal_size : ps_dec->u4_frm_buf_stride;

        ps_dec->pf_fmt_conv_yuv420p_to_yuv422ile(pu1_src_y,
            pu1_src_u,
            pu1_src_v,
            pv_yuv422i,
            u2_width,
            u2_height,
            u2_stride_y,
            u2_stride_u,
            u2_stride_v,
            u2_stride_yuv422i);

    }
    else if((ps_dec->i4_chromaFormat == IV_YUV_420SP_UV) ||
            (ps_dec->i4_chromaFormat == IV_YUV_420SP_VU))
    {

        UWORD32 dest_inc_Y=0,dest_inc_UV=0;
        WORD32 convert_uv_only;

        pu1_dst_u =   (UWORD8 *)ps_disp_frm_buf->pv_u_buf +((u4_start_row >> 1)*(ps_dec->u4_frm_buf_stride));
        dest_inc_Y =    ps_dec->u4_frm_buf_stride;
        dest_inc_UV =   ((ps_dec->u4_frm_buf_stride + 1) >> 1) << 1;
        convert_uv_only = 0;

        if(1 == ps_dec->u4_share_disp_buf)
            convert_uv_only = 1;

        if(pu1_src_y == pu1_dst_y)
            convert_uv_only = 1;

        if(ps_dec->i4_chromaFormat == IV_YUV_420SP_UV)
        {
            ps_dec->pf_fmt_conv_yuv420p_to_yuv420sp_uv(pu1_src_y,
                pu1_src_u,
                pu1_src_v,
                pu1_dst_y,
                pu1_dst_u,
                u4_num_rows,
                ps_dec->u2_horizontal_size,
                ps_dec->u2_frame_width,
                ps_dec->u2_frame_width >> 1,
                ps_dec->u2_frame_width >> 1,
                dest_inc_Y,
                dest_inc_UV,
                convert_uv_only);
        }
        else
        {
            ps_dec->pf_fmt_conv_yuv420p_to_yuv420sp_vu(pu1_src_y,
                    pu1_src_u,
                    pu1_src_v,
                    pu1_dst_y,
                    pu1_dst_u,
                    u4_num_rows,
                    ps_dec->u2_horizontal_size,
                    ps_dec->u2_frame_width,
                    ps_dec->u2_frame_width >> 1,
                    ps_dec->u2_frame_width >> 1,
                    dest_inc_Y,
                    dest_inc_UV,
                    convert_uv_only);
        }



    }

}


/*******************************************************************************
*
*  Function Name   : impeg2d_get_frm_buf
*
*  Description     : Gets YUV component buffers for the frame
*
*  Arguments       :
*  frm_buf         : YUV buffer
*  frm             : Reference frame
*  width           : Width of the frame
*  Height          : Height of the frame
*
*  Values Returned : None
*******************************************************************************/
void impeg2d_get_frm_buf(yuv_buf_t *ps_frm_buf,UWORD8 *pu1_frm,UWORD32 u4_width,UWORD32 u4_height)
{
   UWORD32 u4_luma_size = u4_width * u4_height;
   UWORD32 u4_chroma_size = (u4_width * u4_height)>>2;

   ps_frm_buf->pu1_y = pu1_frm;
   ps_frm_buf->pu1_u = pu1_frm + u4_luma_size;
   ps_frm_buf->pu1_v = pu1_frm + u4_luma_size + u4_chroma_size;

}
/*******************************************************************************
*
*  Function Name   : impeg2d_get_bottom_field_buf
*
*  Description     : Gets YUV component buffers for bottom field of the frame
*
*  Arguments       :
*  frm_buf         : YUV buffer
*  frm             : Reference frame
*  width           : Width of the frame
*  Height          : Height of the frame
*
*  Values Returned : None
*******************************************************************************/
void impeg2d_get_bottom_field_buf(yuv_buf_t *ps_src_buf,yuv_buf_t *ps_dst_buf,
                      UWORD32 u4_width)
{
   ps_dst_buf->pu1_y = ps_src_buf->pu1_y + u4_width;
   ps_dst_buf->pu1_u = ps_src_buf->pu1_u + (u4_width>>1);
   ps_dst_buf->pu1_v = ps_src_buf->pu1_v + (u4_width>>1);

}
/*******************************************************************************
*  Function Name   : impeg2d_get_mb_addr_incr
*
*  Description     : Decodes the Macroblock address increment
*
*  Arguments       :
*  stream          : Bitstream
*
*  Values Returned : Macroblock address increment
*******************************************************************************/
UWORD16 impeg2d_get_mb_addr_incr(stream_t *ps_stream)
{
    UWORD16 u2_mb_addr_incr = 0;
    while (impeg2d_bit_stream_nxt(ps_stream,MB_ESCAPE_CODE_LEN) == MB_ESCAPE_CODE &&
            ps_stream->u4_offset < ps_stream->u4_max_offset)
    {
        impeg2d_bit_stream_flush(ps_stream,MB_ESCAPE_CODE_LEN);
        u2_mb_addr_incr += 33;
    }
    u2_mb_addr_incr += impeg2d_dec_vld_symbol(ps_stream,gai2_impeg2d_mb_addr_incr,MB_ADDR_INCR_LEN) +
        MB_ADDR_INCR_OFFSET;
    return(u2_mb_addr_incr);
}

/*******************************************************************************
*
*  Function Name   : impeg2d_init_video_state
*
*  Description     : Initializes the Video decoder state
*
*  Arguments       :
*  dec             : Decoder context
*  videoType       : MPEG_2_Video / MPEG_1_Video
*
*  Values Returned : None
*******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_init_video_state(dec_state_t *ps_dec, e_video_type_t e_video_type)
{
    /*-----------------------------------------------------------------------*/
    /* Bit Stream  that conforms to MPEG-1 <ISO/IEC 11172-2> standard        */
    /*-----------------------------------------------------------------------*/
    if(e_video_type == MPEG_1_VIDEO)
    {
        ps_dec->u2_is_mpeg2 = 0;

        /*-------------------------------------------------------------------*/
        /* force MPEG-1 parameters for proper decoder behavior               */
        /* see ISO/IEC 13818-2 section D.9.14                                */
        /*-------------------------------------------------------------------*/
        ps_dec->u2_progressive_sequence         = 1;
        ps_dec->u2_intra_dc_precision           = 0;
        ps_dec->u2_picture_structure            = FRAME_PICTURE;
        ps_dec->u2_frame_pred_frame_dct         = 1;
        ps_dec->u2_concealment_motion_vectors   = 0;
        ps_dec->u2_q_scale_type                 = 0;
        ps_dec->u2_intra_vlc_format             = 0;
        ps_dec->u2_alternate_scan               = 0;
        ps_dec->u2_repeat_first_field           = 0;
        ps_dec->u2_progressive_frame            = 1;
        ps_dec->u2_frame_rate_extension_n       = 0;
        ps_dec->u2_frame_rate_extension_d       = 0;
        ps_dec->u2_forw_f_code                  = 7;
        ps_dec->u2_back_f_code                  = 7;

        ps_dec->pf_vld_inv_quant                  = impeg2d_vld_inv_quant_mpeg1;
        /*-------------------------------------------------------------------*/
        /* Setting of parameters other than those mentioned in MPEG2 standard*/
        /* but used in decoding process.                                     */
        /*-------------------------------------------------------------------*/
    }
    /*-----------------------------------------------------------------------*/
    /* Bit Stream  that conforms to MPEG-2                                   */
    /*-----------------------------------------------------------------------*/
    else
    {
        ps_dec->u2_is_mpeg2                  = 1;
        ps_dec->u2_full_pel_forw_vector   = 0;
        ps_dec->u2_forw_f_code            = 7;
        ps_dec->u2_full_pel_back_vector   = 0;
        ps_dec->u2_back_f_code            = 7;
        ps_dec->pf_vld_inv_quant       = impeg2d_vld_inv_quant_mpeg2;


    }


    impeg2d_init_function_ptr(ps_dec);

    /* Set the frame Width and frame Height */
    ps_dec->u2_frame_height        = ALIGN16(ps_dec->u2_vertical_size);
    ps_dec->u2_frame_width         = ALIGN16(ps_dec->u2_horizontal_size);
    ps_dec->u2_num_horiz_mb         = (ps_dec->u2_horizontal_size + 15) >> 4;
   // dec->u4_frm_buf_stride    = dec->frameWidth;
    if (ps_dec->u2_frame_height > ps_dec->u2_create_max_height || ps_dec->u2_frame_width > ps_dec->u2_create_max_width)
    {
        return IMPEG2D_PIC_SIZE_NOT_SUPPORTED;
    }

    ps_dec->u2_num_flds_decoded = 0;

    /* Calculate the frame period */
    {
        UWORD32 numer;
        UWORD32 denom;
        numer = (UWORD32)gau2_impeg2_frm_rate_code[ps_dec->u2_frame_rate_code][1] *
                                (UWORD32)(ps_dec->u2_frame_rate_extension_d + 1);

        denom = (UWORD32)gau2_impeg2_frm_rate_code[ps_dec->u2_frame_rate_code][0] *
                                (UWORD32)(ps_dec->u2_frame_rate_extension_n + 1);
        ps_dec->u2_framePeriod =  (numer * 1000 * 100) / denom;
    }


   if(VERTICAL_SCAN == ps_dec->u2_alternate_scan)
   {
    ps_dec->pu1_inv_scan_matrix = (UWORD8 *)gau1_impeg2_inv_scan_vertical;
   }
   else
   {
    ps_dec->pu1_inv_scan_matrix = (UWORD8 *)gau1_impeg2_inv_scan_zig_zag;
   }
   return (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;
}
/*******************************************************************************
*
*  Function Name   : impeg2d_pre_pic_dec_proc
*
*  Description     : Does the processing neccessary before picture decoding
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_pre_pic_dec_proc(dec_state_t *ps_dec)
{
    WORD32 u4_get_disp;
    pic_buf_t *ps_disp_pic;
    IMPEG2D_ERROR_CODES_T e_error = (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;

    u4_get_disp = 0;
    ps_disp_pic = NULL;

    /* Field Picture */
    if(ps_dec->u2_picture_structure != FRAME_PICTURE)
    {
        ps_dec->u2_num_vert_mb       = (ps_dec->u2_vertical_size + 31) >> 5;

        if(ps_dec->u2_num_flds_decoded == 0)
        {
            pic_buf_t *ps_pic_buf;
            u4_get_disp = 1;

            ps_pic_buf = impeg2_buf_mgr_get_next_free(ps_dec->pv_pic_buf_mg, &ps_dec->i4_cur_buf_id);

            if (NULL == ps_pic_buf)
            {
                return IMPEG2D_NO_FREE_BUF_ERR;
            }

            impeg2_buf_mgr_set_status((buf_mgr_t *)ps_dec->pv_pic_buf_mg, ps_dec->i4_cur_buf_id, BUF_MGR_DISP);
            impeg2_buf_mgr_set_status((buf_mgr_t *)ps_dec->pv_pic_buf_mg, ps_dec->i4_cur_buf_id, BUF_MGR_REF);
            if(ps_dec->u4_deinterlace)
                impeg2_buf_mgr_set_status((buf_mgr_t *)ps_dec->pv_pic_buf_mg, ps_dec->i4_cur_buf_id, MPEG2_BUF_MGR_DEINT);

            ps_pic_buf->u4_ts = ps_dec->u4_inp_ts;
            ps_pic_buf->e_pic_type = ps_dec->e_pic_type;
            ps_dec->ps_cur_pic = ps_pic_buf;
            ps_dec->s_cur_frm_buf.pu1_y = ps_pic_buf->pu1_y;
            ps_dec->s_cur_frm_buf.pu1_u = ps_pic_buf->pu1_u;
            ps_dec->s_cur_frm_buf.pu1_v = ps_pic_buf->pu1_v;
        }

        if(ps_dec->u2_picture_structure == TOP_FIELD)
        {
            ps_dec->u2_fld_parity = TOP;
        }
        else
        {
            ps_dec->u2_fld_parity = BOTTOM;
        }
        ps_dec->u2_field_dct           = 0;
        ps_dec->u2_read_dct_type        = 0;
        ps_dec->u2_read_motion_type     = 1;
        ps_dec->u2_fld_pic             = 1;
        ps_dec->u2_frm_pic             = 0;
        ps_dec->ps_func_forw_or_back     = gas_impeg2d_func_fld_fw_or_bk;
        ps_dec->ps_func_bi_direct       = gas_impeg2d_func_fld_bi_direct;
   }
    /* Frame Picture */
    else
    {
        pic_buf_t *ps_pic_buf;


        ps_dec->u2_num_vert_mb       = (ps_dec->u2_vertical_size + 15) >> 4;
        u4_get_disp = 1;
        ps_pic_buf = impeg2_buf_mgr_get_next_free(ps_dec->pv_pic_buf_mg, &ps_dec->i4_cur_buf_id);

        if (NULL == ps_pic_buf)
        {
            return IMPEG2D_NO_FREE_BUF_ERR;
        }
        impeg2_buf_mgr_set_status((buf_mgr_t *)ps_dec->pv_pic_buf_mg, ps_dec->i4_cur_buf_id, BUF_MGR_DISP);
        impeg2_buf_mgr_set_status((buf_mgr_t *)ps_dec->pv_pic_buf_mg, ps_dec->i4_cur_buf_id, BUF_MGR_REF);
        if(ps_dec->u4_deinterlace)
            impeg2_buf_mgr_set_status((buf_mgr_t *)ps_dec->pv_pic_buf_mg, ps_dec->i4_cur_buf_id, MPEG2_BUF_MGR_DEINT);

        ps_pic_buf->u4_ts = ps_dec->u4_inp_ts;
        ps_pic_buf->e_pic_type = ps_dec->e_pic_type;
        ps_dec->ps_cur_pic = ps_pic_buf;
        ps_dec->s_cur_frm_buf.pu1_y = ps_pic_buf->pu1_y;
        ps_dec->s_cur_frm_buf.pu1_u = ps_pic_buf->pu1_u;
        ps_dec->s_cur_frm_buf.pu1_v = ps_pic_buf->pu1_v;


        if(ps_dec->u2_frame_pred_frame_dct == 0)
        {
            ps_dec->u2_read_dct_type    = 1;
            ps_dec->u2_read_motion_type = 1;
        }
        else
        {
            ps_dec->u2_read_dct_type    = 0;
            ps_dec->u2_read_motion_type = 0;
            ps_dec->u2_motion_type     = 2;
            ps_dec->u2_field_dct       = 0;
        }

        ps_dec->u2_fld_parity          = TOP;
        ps_dec->u2_fld_pic             = 0;
        ps_dec->u2_frm_pic             = 1;
        ps_dec->ps_func_forw_or_back     = gas_impeg2d_func_frm_fw_or_bk;
        ps_dec->ps_func_bi_direct       = gas_impeg2d_func_frm_bi_direct;
   }
    ps_dec->u2_def_dc_pred[Y_LUMA]   = 128 << ps_dec->u2_intra_dc_precision;
    ps_dec->u2_def_dc_pred[U_CHROMA]   = 128 << ps_dec->u2_intra_dc_precision;
    ps_dec->u2_def_dc_pred[V_CHROMA]   = 128 << ps_dec->u2_intra_dc_precision;
    ps_dec->u2_num_mbs_left  = ps_dec->u2_num_horiz_mb * ps_dec->u2_num_vert_mb;
    if(u4_get_disp)
    {
        if(ps_dec->u4_num_frames_decoded > 1)
        {
            ps_disp_pic = impeg2_disp_mgr_get(&ps_dec->s_disp_mgr, &ps_dec->i4_disp_buf_id);
        }
        ps_dec->ps_disp_pic = ps_disp_pic;
        if(ps_disp_pic)
        {
            if(1 == ps_dec->u4_share_disp_buf)
            {
                ps_dec->ps_disp_frm_buf->pv_y_buf  = ps_disp_pic->pu1_y;
                if(IV_YUV_420P == ps_dec->i4_chromaFormat)
                {
                    ps_dec->ps_disp_frm_buf->pv_u_buf  = ps_disp_pic->pu1_u;
                    ps_dec->ps_disp_frm_buf->pv_v_buf  = ps_disp_pic->pu1_v;
                }
                else
                {
                    UWORD8 *pu1_buf;

                    pu1_buf = ps_dec->as_disp_buffers[ps_disp_pic->i4_buf_id].pu1_bufs[1];
                    ps_dec->ps_disp_frm_buf->pv_u_buf  = pu1_buf;

                    pu1_buf = ps_dec->as_disp_buffers[ps_disp_pic->i4_buf_id].pu1_bufs[2];
                    ps_dec->ps_disp_frm_buf->pv_v_buf  = pu1_buf;
                }
            }
        }
    }


    switch(ps_dec->e_pic_type)
    {
    case I_PIC:
        {
            ps_dec->pf_decode_slice = impeg2d_dec_i_slice;
            break;
        }
    case D_PIC:
        {
            ps_dec->pf_decode_slice = impeg2d_dec_d_slice;
            break;
        }
    case P_PIC:
        {
            ps_dec->pf_decode_slice = impeg2d_dec_p_b_slice;
            ps_dec->pu2_mb_type       = gau2_impeg2d_p_mb_type;
            break;
        }
    case B_PIC:
        {
            ps_dec->pf_decode_slice = impeg2d_dec_p_b_slice;
            ps_dec->pu2_mb_type       = gau2_impeg2d_b_mb_type;
            break;
        }
    default:
        return IMPEG2D_INVALID_PIC_TYPE;
    }

    /*************************************************************************/
    /* Set the reference pictures                                            */
    /*************************************************************************/

    /* Error resilience: If forward and backward pictures are going to be NULL*/
    /* then assign both to the current                                        */
    /* if one of them NULL then we will assign the non null to the NULL one   */

    if(ps_dec->e_pic_type == P_PIC)
    {
        if (NULL == ps_dec->as_recent_fld[1][0].pu1_y)
        {
            ps_dec->as_recent_fld[1][0] = ps_dec->s_cur_frm_buf;
        }
        if (NULL == ps_dec->as_recent_fld[1][1].pu1_y)
        {
            impeg2d_get_bottom_field_buf(&ps_dec->s_cur_frm_buf, &ps_dec->as_recent_fld[1][1],
                ps_dec->u2_frame_width);
        }

        ps_dec->as_ref_buf[FORW][TOP]    = ps_dec->as_recent_fld[1][0];
        ps_dec->as_ref_buf[FORW][BOTTOM] = ps_dec->as_recent_fld[1][1];


    }
    else if(ps_dec->e_pic_type == B_PIC)
    {
        if((NULL == ps_dec->as_recent_fld[1][0].pu1_y) && (NULL == ps_dec->as_recent_fld[0][0].pu1_y))
        {
            // assign the current picture to both
            ps_dec->as_recent_fld[1][0] = ps_dec->s_cur_frm_buf;
            impeg2d_get_bottom_field_buf(&ps_dec->s_cur_frm_buf, &ps_dec->as_recent_fld[1][1],
                ps_dec->u2_frame_width);
            ps_dec->as_recent_fld[0][0] = ps_dec->s_cur_frm_buf;
            ps_dec->as_recent_fld[0][1] = ps_dec->as_recent_fld[1][1];
        }
        //Assign the non-null picture to the null picture
        else if ((NULL != ps_dec->as_recent_fld[1][0].pu1_y) && (NULL == ps_dec->as_recent_fld[0][0].pu1_y))
        {
            ps_dec->as_recent_fld[0][0] = ps_dec->as_recent_fld[1][0];
            ps_dec->as_recent_fld[0][1] = ps_dec->as_recent_fld[1][1];
        }
        else if ((NULL == ps_dec->as_recent_fld[1][0].pu1_y) && (NULL != ps_dec->as_recent_fld[0][0].pu1_y))
        {
            ps_dec->as_recent_fld[1][0] = ps_dec->as_recent_fld[0][0];
            ps_dec->as_recent_fld[1][1] = ps_dec->as_recent_fld[0][1];
        }

        /* Error resilience: If forward and backward pictures are going to be NULL*/
        /* then assign both to the current                                        */
        /* if one of them NULL then we will assign the non null to the NULL one   */

        if((NULL == ps_dec->as_recent_fld[0][1].pu1_y) && (NULL == ps_dec->as_recent_fld[1][1].pu1_y))
        {
            // assign the current picture to both
            ps_dec->as_recent_fld[1][0] = ps_dec->s_cur_frm_buf;
            impeg2d_get_bottom_field_buf(&ps_dec->s_cur_frm_buf, &ps_dec->as_recent_fld[1][1],
                                         ps_dec->u2_frame_width);
            ps_dec->as_recent_fld[0][0] = ps_dec->s_cur_frm_buf;
            ps_dec->as_recent_fld[0][1] = ps_dec->as_recent_fld[1][1];
        }
        //Assign the non-null picture to the null picture

        else if((NULL == ps_dec->as_recent_fld[0][1].pu1_y) && (NULL != ps_dec->as_recent_fld[1][1].pu1_y))
        {
            ps_dec->as_recent_fld[0][0] = ps_dec->as_recent_fld[1][0];
            ps_dec->as_recent_fld[0][1] = ps_dec->as_recent_fld[1][1];
        }

        else if((NULL == ps_dec->as_recent_fld[1][1].pu1_y) && (NULL != ps_dec->as_recent_fld[0][1].pu1_y))
        {
            ps_dec->as_recent_fld[1][0] = ps_dec->as_recent_fld[0][0];
            ps_dec->as_recent_fld[1][1] = ps_dec->as_recent_fld[0][1];
        }
        ps_dec->as_ref_buf[FORW][TOP]    = ps_dec->as_recent_fld[0][0];
        ps_dec->as_ref_buf[FORW][BOTTOM] = ps_dec->as_recent_fld[0][1];
        ps_dec->as_ref_buf[BACK][TOP]    = ps_dec->as_recent_fld[1][0];
        ps_dec->as_ref_buf[BACK][BOTTOM] = ps_dec->as_recent_fld[1][1];


    }

    return e_error;
}

/*******************************************************************************
*
*  Function Name   : impeg2d_post_pic_dec_proc
*
*  Description     : Performs processing that is needed at the end of picture
*                    decode
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
void impeg2d_post_pic_dec_proc(dec_state_t *ps_dec)
{

   WORD32 u4_update_pic_buf = 0;
    /*************************************************************************/
    /* Processing at the end of picture                                      */
    /*************************************************************************/
    if(ps_dec->u2_picture_structure != FRAME_PICTURE)
    {
        ps_dec->u2_num_vert_mb       = (ps_dec->u2_vertical_size + 31) >> 5;

        if(ps_dec->u2_num_flds_decoded == 1)
        {
            ps_dec->u2_num_flds_decoded = 0;
            u4_update_pic_buf = 1;
        }
        else
        {
            ps_dec->u2_num_flds_decoded = 1;
        }
    }
    else
    {
        u4_update_pic_buf = 1;
    }

    if(u4_update_pic_buf)
    {
        ps_dec->i4_frame_decoded = 1;
        if(ps_dec->e_pic_type != B_PIC)
        {
            /* In any sequence first two pictures have to be reference pictures */
            /* Adding of first picture in the sequence */
            if(ps_dec->aps_ref_pics[0] == NULL)
            {
                ps_dec->aps_ref_pics[0] = ps_dec->ps_cur_pic;
            }

            /* Adding of second picture in the sequence */
            else if(ps_dec->aps_ref_pics[1] == NULL)
            {
                ps_dec->aps_ref_pics[1] = ps_dec->ps_cur_pic;
                impeg2_disp_mgr_add(&ps_dec->s_disp_mgr, ps_dec->aps_ref_pics[0], ps_dec->aps_ref_pics[0]->i4_buf_id);
            }
            else
            {

                impeg2_disp_mgr_add(&ps_dec->s_disp_mgr, ps_dec->aps_ref_pics[1], ps_dec->aps_ref_pics[1]->i4_buf_id);
                impeg2_buf_mgr_release(ps_dec->pv_pic_buf_mg, ps_dec->aps_ref_pics[0]->i4_buf_id, BUF_MGR_REF);
                ps_dec->aps_ref_pics[0] = ps_dec->aps_ref_pics[1];
                ps_dec->aps_ref_pics[1] = ps_dec->ps_cur_pic;

            }
        }
        else
        {
            impeg2_disp_mgr_add(&ps_dec->s_disp_mgr, ps_dec->ps_cur_pic, ps_dec->ps_cur_pic->i4_buf_id);

            impeg2_buf_mgr_release(ps_dec->pv_pic_buf_mg, ps_dec->ps_cur_pic->i4_buf_id, BUF_MGR_REF);
        }

    }
    /*************************************************************************/
    /* Update the list of recent reference pictures                          */
    /*************************************************************************/
    if(ps_dec->e_pic_type != B_PIC)
    {
        switch(ps_dec->u2_picture_structure)
        {
        case FRAME_PICTURE:
            {
                ps_dec->as_recent_fld[0][0] = ps_dec->as_recent_fld[1][0];
                ps_dec->as_recent_fld[0][1] = ps_dec->as_recent_fld[1][1];

                ps_dec->as_recent_fld[1][0] = ps_dec->s_cur_frm_buf;
                impeg2d_get_bottom_field_buf(&ps_dec->s_cur_frm_buf, &ps_dec->as_recent_fld[1][1],
                ps_dec->u2_frame_width);
                break;
            }
        case TOP_FIELD:
            {
                ps_dec->as_recent_fld[0][0] = ps_dec->as_recent_fld[1][0];
                ps_dec->as_recent_fld[1][0] = ps_dec->s_cur_frm_buf;
                break;
            }
        case BOTTOM_FIELD:
            {
                ps_dec->as_recent_fld[0][1] = ps_dec->as_recent_fld[1][1];
                impeg2d_get_bottom_field_buf(&ps_dec->s_cur_frm_buf, &ps_dec->as_recent_fld[1][1],
                ps_dec->u2_frame_width);
                break;
            }
        }
    }
}
