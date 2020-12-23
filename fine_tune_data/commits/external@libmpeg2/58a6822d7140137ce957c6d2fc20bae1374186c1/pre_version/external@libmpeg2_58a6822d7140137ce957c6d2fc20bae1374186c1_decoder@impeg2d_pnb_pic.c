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
#include "impeg2d_vld_tables.h"
#include "impeg2d_vld.h"
#include "impeg2d_pic_proc.h"
#include "impeg2d_debug.h"
#include "impeg2d_mc.h"

#define BLK_SIZE 8
#define LUMA_BLK_SIZE (2 * (BLK_SIZE))
#define CHROMA_BLK_SIZE (BLK_SIZE)


/*******************************************************************************
*
*  Function Name   : impeg2d_dec_p_mb_params
*
*  Description     : Decodes the parameters for P
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
void impeg2d_dec_p_mb_params(dec_state_t *ps_dec)
{
    stream_t *ps_stream = &ps_dec->s_bit_stream;
    UWORD16 u2_mb_addr_incr;
    UWORD16 u2_total_len;
    UWORD16 u2_len;
    UWORD16 u2_mb_type;
    UWORD32 u4_next_word;
    const dec_mb_params_t *ps_dec_mb_params;
    if(impeg2d_bit_stream_nxt(ps_stream,1) == 1)
    {
        impeg2d_bit_stream_flush(ps_stream,1);

    }
    else
    {
        u2_mb_addr_incr = impeg2d_get_mb_addr_incr(ps_stream);
        if(0 == ps_dec->u2_first_mb)
        {
            /****************************************************************/
            /* If the 2nd member of a field picture pair is a P picture and */
            /* the first one was an I picture, there cannot be any skipped  */
            /* MBs in the second field picture                              */
            /****************************************************************/
            /*
            if((dec->picture_structure != FRAME_PICTURE) &&
                (dec->f->FieldFuncCall != 0) &&
                (dec->las->u1_last_coded_vop_type == I))
            {
                core0_err_handler((void *)(VOLParams),
                    ITTMPEG2_ERR_INVALID_MB_SKIP);
            }
            */
            /****************************************************************/
            /* In MPEG-2, the last MB of the row cannot be skipped and the  */
            /* MBAddrIncr cannot be such that it will take the current MB   */
            /* beyond the current row                                       */
            /* In MPEG-1, the slice could start and end anywhere and is not */
            /* restricted to a row like in MPEG-2. Hence this check should  */
            /* not be done for MPEG-1 streams.                              */
            /****************************************************************/
            if(ps_dec->u2_is_mpeg2 && ((ps_dec->u2_mb_x + u2_mb_addr_incr) > ps_dec->u2_num_horiz_mb) )
            {
                u2_mb_addr_incr    = ps_dec->u2_num_horiz_mb - ps_dec->u2_mb_x;
            }

            impeg2d_dec_skip_mbs(ps_dec, (UWORD16)(u2_mb_addr_incr - 1));
        }

    }
    u4_next_word = (UWORD16)impeg2d_bit_stream_nxt(ps_stream,16);
    /*-----------------------------------------------------------------------*/
    /* MB type                                                               */
    /*-----------------------------------------------------------------------*/
    {
        u2_mb_type   = ps_dec->pu2_mb_type[BITS((UWORD16)u4_next_word,15,10)];
        u2_len      = BITS(u2_mb_type,15,8);
        u2_total_len = u2_len;
        u4_next_word = (UWORD16)LSW((UWORD16)u4_next_word << u2_len);
    }
    /*-----------------------------------------------------------------------*/
    /* motion type                                                           */
    /*-----------------------------------------------------------------------*/
    {
        if((u2_mb_type & MB_FORW_OR_BACK) &&  ps_dec->u2_read_motion_type)
        {
            WORD32 i4_motion_type;
            ps_dec->u2_motion_type = BITS((UWORD16)u4_next_word,15,14);
            u2_total_len        += MB_MOTION_TYPE_LEN;
            u4_next_word        = (UWORD16)LSW((UWORD16)u4_next_word << MB_MOTION_TYPE_LEN);
            i4_motion_type     = ps_dec->u2_motion_type;

            if((i4_motion_type == 0) ||
                (i4_motion_type == 4) ||
                (i4_motion_type >  7))
            {
                //TODO : VANG Check for validity
                i4_motion_type = 1;
            }

        }
    }
    /*-----------------------------------------------------------------------*/
    /* dct type                                                              */
    /*-----------------------------------------------------------------------*/
    {
        if((u2_mb_type & MB_CODED) && ps_dec->u2_read_dct_type)
        {
            ps_dec->u2_field_dct = BIT((UWORD16)u4_next_word,15);
            u2_total_len += MB_DCT_TYPE_LEN;
            u4_next_word = (UWORD16)LSW((UWORD16)u4_next_word << MB_DCT_TYPE_LEN);
        }
    }
    /*-----------------------------------------------------------------------*/
    /* Quant scale code                                                      */
    /*-----------------------------------------------------------------------*/
    if(u2_mb_type & MB_QUANT)
    {
        UWORD16 u2_quant_scale_code;
        u2_quant_scale_code = BITS((UWORD16)u4_next_word,15,11);

        ps_dec->u1_quant_scale = (ps_dec->u2_q_scale_type) ?
            gau1_impeg2_non_linear_quant_scale[u2_quant_scale_code] : (u2_quant_scale_code << 1);
        u2_total_len += MB_QUANT_SCALE_CODE_LEN;
    }
    impeg2d_bit_stream_flush(ps_stream,u2_total_len);
    /*-----------------------------------------------------------------------*/
    /* Set the function pointers                                             */
    /*-----------------------------------------------------------------------*/
    ps_dec->u2_coded_mb    = (UWORD16)(u2_mb_type & MB_CODED);

    if(u2_mb_type & MB_FORW_OR_BACK)
    {

        UWORD16 refPic      = !(u2_mb_type & MB_MV_FORW);
        UWORD16 index       = (ps_dec->u2_motion_type);
        ps_dec->u2_prev_intra_mb    = 0;
        ps_dec->e_mb_pred         = (e_pred_direction_t)refPic;
        ps_dec_mb_params = &ps_dec->ps_func_forw_or_back[index];
        ps_dec->s_mb_type = ps_dec_mb_params->s_mb_type;
        ps_dec_mb_params->pf_func_mb_params(ps_dec);

    }
    else if(u2_mb_type & MB_TYPE_INTRA)
    {
        ps_dec->u2_prev_intra_mb    = 1;
        impeg2d_dec_intra_mb(ps_dec);

    }
    else
    {
        ps_dec->u2_prev_intra_mb    = 0;
        ps_dec->e_mb_pred = FORW;
        ps_dec->u2_motion_type = 0;
        impeg2d_dec_0mv_coded_mb(ps_dec);
    }

    /*-----------------------------------------------------------------------*/
    /* decode cbp                                                            */
    /*-----------------------------------------------------------------------*/
    if((u2_mb_type & MB_TYPE_INTRA))
    {
        ps_dec->u2_cbp  = 0x3f;
        ps_dec->u2_prev_intra_mb    = 1;
    }
    else
    {
        ps_dec->u2_prev_intra_mb  = 0;
        ps_dec->u2_def_dc_pred[Y_LUMA] = 128 << ps_dec->u2_intra_dc_precision;
        ps_dec->u2_def_dc_pred[U_CHROMA] = 128 << ps_dec->u2_intra_dc_precision;
        ps_dec->u2_def_dc_pred[V_CHROMA] = 128 << ps_dec->u2_intra_dc_precision;
        if((ps_dec->u2_coded_mb))
        {
            UWORD16 cbpValue;
            cbpValue  = gau2_impeg2d_cbp_code[impeg2d_bit_stream_nxt(ps_stream,MB_CBP_LEN)];
            ps_dec->u2_cbp  = cbpValue & 0xFF;
            impeg2d_bit_stream_flush(ps_stream,(cbpValue >> 8) & 0x0FF);
        }
        else
        {
            ps_dec->u2_cbp  = 0;
        }
    }
}


/*******************************************************************************
*
*  Function Name   : impeg2d_dec_pnb_mb_params
*
*  Description     : Decodes the parameters for P and B pictures
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
void impeg2d_dec_pnb_mb_params(dec_state_t *ps_dec)
{
    stream_t *ps_stream = &ps_dec->s_bit_stream;
    UWORD16 u2_mb_addr_incr;
    UWORD16 u2_total_len;
    UWORD16 u2_len;
    UWORD16 u2_mb_type;
    UWORD32 u4_next_word;
    const dec_mb_params_t *ps_dec_mb_params;
    if(impeg2d_bit_stream_nxt(ps_stream,1) == 1)
    {
        impeg2d_bit_stream_flush(ps_stream,1);

    }
    else
    {
        u2_mb_addr_incr = impeg2d_get_mb_addr_incr(ps_stream);

        if(ps_dec->u2_first_mb)
        {
            /****************************************************************/
            /* Section 6.3.17                                               */
            /* The first MB of a slice cannot be skipped                    */
            /* But the mb_addr_incr can be > 1, because at the beginning of */
            /* a slice, it indicates the offset from the last MB in the     */
            /* previous row. Hence for the first slice in a row, the        */
            /* mb_addr_incr needs to be 1.                                  */
            /****************************************************************/
            /* MB_x is set to zero whenever MB_y changes.                   */
            ps_dec->u2_mb_x = u2_mb_addr_incr - 1;
            /* For error resilience */
            ps_dec->u2_mb_x = MIN(ps_dec->u2_mb_x, (ps_dec->u2_num_horiz_mb - 1));

            /****************************************************************/
            /* mb_addr_incr is forced to 1 because in this decoder it is used */
            /* more as an indicator of the number of MBs skipped than the   */
            /* as defined by the standard (Section 6.3.17)                  */
            /****************************************************************/
            u2_mb_addr_incr = 1;
            ps_dec->u2_first_mb = 0;
        }
        else
        {
            /****************************************************************/
            /* In MPEG-2, the last MB of the row cannot be skipped and the  */
            /* mb_addr_incr cannot be such that it will take the current MB   */
            /* beyond the current row                                       */
            /* In MPEG-1, the slice could start and end anywhere and is not */
            /* restricted to a row like in MPEG-2. Hence this check should  */
            /* not be done for MPEG-1 streams.                              */
            /****************************************************************/
            if(ps_dec->u2_is_mpeg2 &&
                ((ps_dec->u2_mb_x + u2_mb_addr_incr) > ps_dec->u2_num_horiz_mb))
            {
                u2_mb_addr_incr    = ps_dec->u2_num_horiz_mb - ps_dec->u2_mb_x;
            }


            impeg2d_dec_skip_mbs(ps_dec, (UWORD16)(u2_mb_addr_incr - 1));
        }

    }
    u4_next_word = (UWORD16)impeg2d_bit_stream_nxt(ps_stream,16);
    /*-----------------------------------------------------------------------*/
    /* MB type                                                               */
    /*-----------------------------------------------------------------------*/
    {
        u2_mb_type   = ps_dec->pu2_mb_type[BITS((UWORD16)u4_next_word,15,10)];
        u2_len      = BITS(u2_mb_type,15,8);
        u2_total_len = u2_len;
        u4_next_word = (UWORD16)LSW((UWORD16)u4_next_word << u2_len);
    }
    /*-----------------------------------------------------------------------*/
    /* motion type                                                           */
    /*-----------------------------------------------------------------------*/
    {
        WORD32 i4_motion_type = ps_dec->u2_motion_type;

        if((u2_mb_type & MB_FORW_OR_BACK) &&  ps_dec->u2_read_motion_type)
        {
            ps_dec->u2_motion_type = BITS((UWORD16)u4_next_word,15,14);
            u2_total_len += MB_MOTION_TYPE_LEN;
            u4_next_word = (UWORD16)LSW((UWORD16)u4_next_word << MB_MOTION_TYPE_LEN);
            i4_motion_type     = ps_dec->u2_motion_type;

        }


        if ((u2_mb_type & MB_FORW_OR_BACK) &&
            ((i4_motion_type == 0) ||
            (i4_motion_type == 3) ||
            (i4_motion_type == 4) ||
            (i4_motion_type >= 7)))
        {
            //TODO: VANG Check for validity
            i4_motion_type = 1;
        }

    }
    /*-----------------------------------------------------------------------*/
    /* dct type                                                              */
    /*-----------------------------------------------------------------------*/
    {
        if((u2_mb_type & MB_CODED) && ps_dec->u2_read_dct_type)
        {
            ps_dec->u2_field_dct = BIT((UWORD16)u4_next_word,15);
            u2_total_len += MB_DCT_TYPE_LEN;
            u4_next_word = (UWORD16)LSW((UWORD16)u4_next_word << MB_DCT_TYPE_LEN);
        }
    }
    /*-----------------------------------------------------------------------*/
    /* Quant scale code                                                      */
    /*-----------------------------------------------------------------------*/
    if(u2_mb_type & MB_QUANT)
    {
        UWORD16 u2_quant_scale_code;
        u2_quant_scale_code = BITS((UWORD16)u4_next_word,15,11);

        ps_dec->u1_quant_scale = (ps_dec->u2_q_scale_type) ?
            gau1_impeg2_non_linear_quant_scale[u2_quant_scale_code] : (u2_quant_scale_code << 1);
        u2_total_len += MB_QUANT_SCALE_CODE_LEN;
    }
    impeg2d_bit_stream_flush(ps_stream,u2_total_len);
    /*-----------------------------------------------------------------------*/
    /* Set the function pointers                                             */
    /*-----------------------------------------------------------------------*/
    ps_dec->u2_coded_mb    = (UWORD16)(u2_mb_type & MB_CODED);

    if(u2_mb_type & MB_BIDRECT)
    {
        UWORD16 u2_index       = (ps_dec->u2_motion_type);

        ps_dec->u2_prev_intra_mb    = 0;
        ps_dec->e_mb_pred         = BIDIRECT;
        ps_dec_mb_params = &ps_dec->ps_func_bi_direct[u2_index];
        ps_dec->s_mb_type = ps_dec_mb_params->s_mb_type;
        ps_dec_mb_params->pf_func_mb_params(ps_dec);
    }
    else if(u2_mb_type & MB_FORW_OR_BACK)
    {

        UWORD16 u2_refPic      = !(u2_mb_type & MB_MV_FORW);
        UWORD16 u2_index       = (ps_dec->u2_motion_type);
        ps_dec->u2_prev_intra_mb    = 0;
        ps_dec->e_mb_pred         = (e_pred_direction_t)u2_refPic;
        ps_dec_mb_params = &ps_dec->ps_func_forw_or_back[u2_index];
        ps_dec->s_mb_type = ps_dec_mb_params->s_mb_type;
        ps_dec_mb_params->pf_func_mb_params(ps_dec);

    }
    else if(u2_mb_type & MB_TYPE_INTRA)
    {
        ps_dec->u2_prev_intra_mb    = 1;
        impeg2d_dec_intra_mb(ps_dec);

    }
    else
    {
        ps_dec->u2_prev_intra_mb =0;
        ps_dec->e_mb_pred = FORW;
        ps_dec->u2_motion_type = 0;
        impeg2d_dec_0mv_coded_mb(ps_dec);
    }

    /*-----------------------------------------------------------------------*/
    /* decode cbp                                                            */
    /*-----------------------------------------------------------------------*/
    if((u2_mb_type & MB_TYPE_INTRA))
    {
        ps_dec->u2_cbp  = 0x3f;
        ps_dec->u2_prev_intra_mb    = 1;
    }
    else
    {
        ps_dec->u2_prev_intra_mb  = 0;
        ps_dec->u2_def_dc_pred[Y_LUMA] = 128 << ps_dec->u2_intra_dc_precision;
        ps_dec->u2_def_dc_pred[U_CHROMA] = 128 << ps_dec->u2_intra_dc_precision;
        ps_dec->u2_def_dc_pred[V_CHROMA] = 128 << ps_dec->u2_intra_dc_precision;
        if((ps_dec->u2_coded_mb))
        {
            UWORD16 cbpValue;
            cbpValue  = gau2_impeg2d_cbp_code[impeg2d_bit_stream_nxt(ps_stream,MB_CBP_LEN)];
            ps_dec->u2_cbp  = cbpValue & 0xFF;
            impeg2d_bit_stream_flush(ps_stream,(cbpValue >> 8) & 0x0FF);
        }
        else
        {
            ps_dec->u2_cbp  = 0;
        }
    }
}

/*******************************************************************************
*  Function Name   : impeg2d_dec_p_b_slice
*
*  Description     : Decodes P and B slices
*
*  Arguments       :
*  dec             : Decoder state
*
*  Values Returned : None
*******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_dec_p_b_slice(dec_state_t *ps_dec)
{
    WORD16 *pi2_vld_out;
    UWORD32 i;
    yuv_buf_t *ps_cur_frm_buf      = &ps_dec->s_cur_frm_buf;

    UWORD32 u4_frm_offset          = 0;
    const dec_mb_params_t *ps_dec_mb_params;
    IMPEG2D_ERROR_CODES_T e_error   = (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;

    pi2_vld_out = ps_dec->ai2_vld_buf;
    memset(ps_dec->ai2_pred_mv,0,sizeof(ps_dec->ai2_pred_mv));

    ps_dec->u2_prev_intra_mb    = 0;
    ps_dec->u2_first_mb       = 1;

    ps_dec->u2_picture_width = ps_dec->u2_frame_width;

    if(ps_dec->u2_picture_structure != FRAME_PICTURE)
    {
        ps_dec->u2_picture_width <<= 1;
        if(ps_dec->u2_picture_structure == BOTTOM_FIELD)
        {
            u4_frm_offset = ps_dec->u2_frame_width;
        }
    }

    do
    {
        UWORD32 u4_x_offset, u4_y_offset;



        UWORD32 u4_x_dst_offset = 0;
        UWORD32 u4_y_dst_offset = 0;
        UWORD8  *pu1_out_p;
        UWORD8  *pu1_pred;
        WORD32 u4_pred_strd;

        IMPEG2D_TRACE_MB_START(ps_dec->u2_mb_x, ps_dec->u2_mb_y);


        if(ps_dec->e_pic_type == B_PIC)
            impeg2d_dec_pnb_mb_params(ps_dec);
        else
            impeg2d_dec_p_mb_params(ps_dec);

        IMPEG2D_TRACE_MB_START(ps_dec->u2_mb_x, ps_dec->u2_mb_y);

        u4_x_dst_offset = u4_frm_offset + (ps_dec->u2_mb_x << 4);
        u4_y_dst_offset = (ps_dec->u2_mb_y << 4) * ps_dec->u2_picture_width;
        pu1_out_p = ps_cur_frm_buf->pu1_y + u4_x_dst_offset + u4_y_dst_offset;
        if(ps_dec->u2_prev_intra_mb == 0)
        {
            UWORD32 offset_x, offset_y, stride;
            UWORD16 index = (ps_dec->u2_motion_type);
            /*only for non intra mb's*/
            if(ps_dec->e_mb_pred == BIDIRECT)
            {
                ps_dec_mb_params = &ps_dec->ps_func_bi_direct[index];
            }
            else
            {
                ps_dec_mb_params = &ps_dec->ps_func_forw_or_back[index];
            }

            stride = ps_dec->u2_picture_width;

            offset_x = u4_frm_offset + (ps_dec->u2_mb_x << 4);

            offset_y = (ps_dec->u2_mb_y << 4);

            ps_dec->s_dest_buf.pu1_y = ps_cur_frm_buf->pu1_y + offset_y * stride + offset_x;

            stride = stride >> 1;

            ps_dec->s_dest_buf.pu1_u = ps_cur_frm_buf->pu1_u + (offset_y >> 1) * stride
                            + (offset_x >> 1);

            ps_dec->s_dest_buf.pu1_v = ps_cur_frm_buf->pu1_v + (offset_y >> 1) * stride
                            + (offset_x >> 1);

            PROFILE_DISABLE_MC_IF0
            ps_dec_mb_params->pf_mc(ps_dec);

        }
        for(i = 0; i < NUM_LUMA_BLKS; ++i)
        {
            if((ps_dec->u2_cbp & (1 << (BLOCKS_IN_MB - 1 - i))) != 0)
            {
                e_error = ps_dec->pf_vld_inv_quant(ps_dec, pi2_vld_out, ps_dec->pu1_inv_scan_matrix,
                              ps_dec->u2_prev_intra_mb, Y_LUMA, 0);
                if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
                {
                    return e_error;
                }

                u4_x_offset = gai2_impeg2_blk_x_off[i];

                if(ps_dec->u2_field_dct == 0)
                    u4_y_offset = gai2_impeg2_blk_y_off_frm[i] ;
                else
                    u4_y_offset = gai2_impeg2_blk_y_off_fld[i] ;





                IMPEG2D_IDCT_INP_STATISTICS(pi2_vld_out, ps_dec->u4_non_zero_cols, ps_dec->u4_non_zero_rows);

                PROFILE_DISABLE_IDCT_IF0
                {
                    WORD32 idx;
                    if(1 == (ps_dec->u4_non_zero_cols | ps_dec->u4_non_zero_rows))
                        idx = 0;
                    else
                        idx = 1;

                    if(0 == ps_dec->u2_prev_intra_mb)
                    {
                        pu1_pred = pu1_out_p + u4_y_offset * ps_dec->u2_picture_width + u4_x_offset;
                        u4_pred_strd = ps_dec->u2_picture_width << ps_dec->u2_field_dct;
                    }
                    else
                    {
                        pu1_pred = (UWORD8 *)gau1_impeg2_zerobuf;
                        u4_pred_strd = 8;
                    }

                    ps_dec->pf_idct_recon[idx * 2 + ps_dec->i4_last_value_one](pi2_vld_out,
                                                            ps_dec->ai2_idct_stg1,
                                                            pu1_pred,
                                                            pu1_out_p + u4_y_offset * ps_dec->u2_picture_width + u4_x_offset,
                                                            8,
                                                            u4_pred_strd,
                                                            ps_dec->u2_picture_width << ps_dec->u2_field_dct,
                                                            ~ps_dec->u4_non_zero_cols, ~ps_dec->u4_non_zero_rows);
                }
            }

        }

        /* For U and V blocks, divide the x and y offsets by 2. */
        u4_x_dst_offset >>= 1;
        u4_y_dst_offset >>= 2;


        /* In case of chrominance blocks the DCT will be frame DCT */
        /* i = 0, U component and i = 1 is V componet */
        if((ps_dec->u2_cbp & 0x02) != 0)
        {
            pu1_out_p = ps_cur_frm_buf->pu1_u + u4_x_dst_offset + u4_y_dst_offset;
            e_error = ps_dec->pf_vld_inv_quant(ps_dec, pi2_vld_out, ps_dec->pu1_inv_scan_matrix,
                          ps_dec->u2_prev_intra_mb, U_CHROMA, 0);
            if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
            {
                return e_error;
            }


            IMPEG2D_IDCT_INP_STATISTICS(pi2_vld_out, ps_dec->u4_non_zero_cols, ps_dec->u4_non_zero_rows);

            PROFILE_DISABLE_IDCT_IF0
            {
                WORD32 idx;
                if(1 == (ps_dec->u4_non_zero_cols | ps_dec->u4_non_zero_rows))
                    idx = 0;
                else
                    idx = 1;

                if(0 == ps_dec->u2_prev_intra_mb)
                {
                    pu1_pred = pu1_out_p;
                    u4_pred_strd = ps_dec->u2_picture_width >> 1;
                }
                else
                {
                    pu1_pred = (UWORD8 *)gau1_impeg2_zerobuf;
                    u4_pred_strd = 8;
                }

                ps_dec->pf_idct_recon[idx * 2 + ps_dec->i4_last_value_one](pi2_vld_out,
                                                        ps_dec->ai2_idct_stg1,
                                                        pu1_pred,
                                                        pu1_out_p,
                                                        8,
                                                        u4_pred_strd,
                                                        ps_dec->u2_picture_width >> 1,
                                                        ~ps_dec->u4_non_zero_cols, ~ps_dec->u4_non_zero_rows);

            }

        }


        if((ps_dec->u2_cbp & 0x01) != 0)
        {
            pu1_out_p = ps_cur_frm_buf->pu1_v + u4_x_dst_offset + u4_y_dst_offset;
            e_error = ps_dec->pf_vld_inv_quant(ps_dec, pi2_vld_out, ps_dec->pu1_inv_scan_matrix,
                          ps_dec->u2_prev_intra_mb, V_CHROMA, 0);
            if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
            {
                return e_error;
            }


            IMPEG2D_IDCT_INP_STATISTICS(pi2_vld_out, ps_dec->u4_non_zero_cols, ps_dec->u4_non_zero_rows);

            PROFILE_DISABLE_IDCT_IF0
            {
                WORD32 idx;
                if(1 == (ps_dec->u4_non_zero_cols | ps_dec->u4_non_zero_rows))
                    idx = 0;
                else
                    idx = 1;
                if(0 == ps_dec->u2_prev_intra_mb)
                {
                    pu1_pred = pu1_out_p;
                    u4_pred_strd = ps_dec->u2_picture_width >> 1;
                }
                else
                {
                    pu1_pred = (UWORD8 *)gau1_impeg2_zerobuf;
                    u4_pred_strd = 8;
                }

                ps_dec->pf_idct_recon[idx * 2 + ps_dec->i4_last_value_one](pi2_vld_out,
                                                        ps_dec->ai2_idct_stg1,
                                                        pu1_pred,
                                                        pu1_out_p,
                                                        8,
                                                        u4_pred_strd,
                                                        ps_dec->u2_picture_width >> 1,
                                                        ~ps_dec->u4_non_zero_cols, ~ps_dec->u4_non_zero_rows);

            }
        }


        ps_dec->u2_num_mbs_left--;
        ps_dec->u2_first_mb = 0;
        ps_dec->u2_mb_x++;

        if(ps_dec->s_bit_stream.u4_offset > ps_dec->s_bit_stream.u4_max_offset)
        {
            return IMPEG2D_BITSTREAM_BUFF_EXCEEDED_ERR;
        }
        else if (ps_dec->u2_mb_x == ps_dec->u2_num_horiz_mb)
        {
            ps_dec->u2_mb_x = 0;
            ps_dec->u2_mb_y++;

        }
    }
    while(ps_dec->u2_num_mbs_left != 0 && impeg2d_bit_stream_nxt(&ps_dec->s_bit_stream,23) != 0x0);
    return e_error;
}
