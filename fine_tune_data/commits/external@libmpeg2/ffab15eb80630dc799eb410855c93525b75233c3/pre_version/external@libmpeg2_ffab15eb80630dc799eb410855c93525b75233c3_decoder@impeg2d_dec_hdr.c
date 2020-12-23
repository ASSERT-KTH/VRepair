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
#include <string.h>

#include "iv_datatypedef.h"
#include "iv.h"
#include "ivd.h"
#include "impeg2_macros.h"
#include "impeg2_buf_mgr.h"
#include "impeg2_disp_mgr.h"
#include "impeg2_defs.h"
#include "impeg2_inter_pred.h"
#include "impeg2_idct.h"
#include "impeg2_format_conv.h"
#include "impeg2_mem_func.h"
#include "impeg2_platform_macros.h"
#include "ithread.h"
#include "impeg2_job_queue.h"

#include "impeg2d.h"
#include "impeg2d_bitstream.h"
#include "impeg2d_api.h"
#include "impeg2d_structs.h"
#include "impeg2_globals.h"
#include "impeg2d_pic_proc.h"



/******************************************************************************
*  Function Name   : impeg2d_next_start_code
*
*  Description     : Peek for next_start_code from the stream_t.
*
*  Arguments       :
*  dec             : Decoder Context
*
*  Values Returned : None
******************************************************************************/
void impeg2d_next_start_code(dec_state_t *ps_dec)
{
    stream_t *ps_stream;
    ps_stream = &ps_dec->s_bit_stream;
    impeg2d_bit_stream_flush_to_byte_boundary(ps_stream);

    while ((impeg2d_bit_stream_nxt(ps_stream,START_CODE_PREFIX_LEN) != START_CODE_PREFIX)
        && (ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset))
    {
        impeg2d_bit_stream_get(ps_stream,8);
    }
    return;
}
/******************************************************************************
*  Function Name   : impeg2d_next_code
*
*  Description     : Peek for next_start_code from the stream_t.
*
*  Arguments       :
*  dec             : Decoder Context
*
*  Values Returned : None
******************************************************************************/
void impeg2d_next_code(dec_state_t *ps_dec, UWORD32 u4_start_code_val)
{
    stream_t *ps_stream;
    ps_stream = &ps_dec->s_bit_stream;
    impeg2d_bit_stream_flush_to_byte_boundary(ps_stream);

    while ((impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN) != u4_start_code_val)
        && (ps_dec->s_bit_stream.u4_offset <= ps_dec->s_bit_stream.u4_max_offset))
    {

        if (impeg2d_bit_stream_get(ps_stream,8) != 0)
        {
            /* Ignore stuffing bit errors. */
        }

    }
    return;
}
/******************************************************************************
*  Function Name   : impeg2d_peek_next_start_code
*
*  Description     : Peek for next_start_code from the stream_t.
*
*  Arguments       :
*  dec             : Decoder Context
*
*  Values Returned : None
******************************************************************************/
void impeg2d_peek_next_start_code(dec_state_t *ps_dec)
{
    stream_t *ps_stream;
    ps_stream = &ps_dec->s_bit_stream;
    impeg2d_bit_stream_flush_to_byte_boundary(ps_stream);

    while ((impeg2d_bit_stream_nxt(ps_stream,START_CODE_PREFIX_LEN) != START_CODE_PREFIX)
        && (ps_dec->s_bit_stream.u4_offset <= ps_dec->s_bit_stream.u4_max_offset))
    {
        impeg2d_bit_stream_get(ps_stream,8);
    }
    return;
}
/******************************************************************************
*
*  Function Name   : impeg2d_dec_seq_hdr
*
*  Description     : Decodes Sequence header information
*
*  Arguments       :
*  dec             : Decoder Context
*
*  Values Returned : None
******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_dec_seq_hdr(dec_state_t *ps_dec)
{
    stream_t *ps_stream;
    ps_stream = &ps_dec->s_bit_stream;
    UWORD16 u2_height;
    UWORD16 u2_width;

    if (impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN) != SEQUENCE_HEADER_CODE)
    {
        impeg2d_bit_stream_flush(ps_stream,START_CODE_LEN);
        return IMPEG2D_FRM_HDR_START_CODE_NOT_FOUND;

    }
    impeg2d_bit_stream_flush(ps_stream,START_CODE_LEN);

    u2_width    = impeg2d_bit_stream_get(ps_stream,12);
    u2_height   = impeg2d_bit_stream_get(ps_stream,12);

    if ((u2_width != ps_dec->u2_horizontal_size)
                    || (u2_height != ps_dec->u2_vertical_size))
    {
        if (0 == ps_dec->u2_header_done)
        {
            /* This is the first time we are reading the resolution */
            ps_dec->u2_horizontal_size = u2_width;
            ps_dec->u2_vertical_size = u2_height;
            if (0 == ps_dec->u4_frm_buf_stride)
            {
                ps_dec->u4_frm_buf_stride  = (UWORD32) ALIGN16(u2_width);
            }
        }
        else
        {
            if((u2_width > ps_dec->u2_create_max_width)
                            || (u2_height > ps_dec->u2_create_max_height))
            {
                IMPEG2D_ERROR_CODES_T e_error = IMPEG2D_UNSUPPORTED_DIMENSIONS;

                ps_dec->u2_reinit_max_height   = u2_height;
                ps_dec->u2_reinit_max_width    = u2_width;

                return e_error;
            }
            else
            {
                /* The resolution has changed */
                return (IMPEG2D_ERROR_CODES_T)IVD_RES_CHANGED;
            }
        }
    }

    if((ps_dec->u2_horizontal_size > ps_dec->u2_create_max_width)
                    || (ps_dec->u2_vertical_size > ps_dec->u2_create_max_height))
    {
        IMPEG2D_ERROR_CODES_T e_error = IMPEG2D_UNSUPPORTED_DIMENSIONS;
        return SET_IVD_FATAL_ERROR(e_error);
    }


    /*------------------------------------------------------------------------*/
    /* Flush the following as they are not being used                         */
    /* aspect_ratio_info (4 bits)                                             */
    /*------------------------------------------------------------------------*/
    ps_dec->u2_aspect_ratio_info = impeg2d_bit_stream_get(ps_stream,4);

    /*------------------------------------------------------------------------*/
    /* Frame rate code(4 bits)                                                */
    /*------------------------------------------------------------------------*/
    ps_dec->u2_frame_rate_code = impeg2d_bit_stream_get(ps_stream,4);
    /*------------------------------------------------------------------------*/
    /* Flush the following as they are not being used                         */
    /* bit_rate_value (18 bits)                                               */
    /*------------------------------------------------------------------------*/
    impeg2d_bit_stream_flush(ps_stream,18);
    GET_MARKER_BIT(ps_dec,ps_stream);
    /*------------------------------------------------------------------------*/
    /* Flush the following as they are not being used                         */
    /* vbv_buffer_size_value(10 bits), constrained_parameter_flag (1 bit)     */
    /*------------------------------------------------------------------------*/
    impeg2d_bit_stream_flush(ps_stream,11);

    /*------------------------------------------------------------------------*/
    /* Quantization matrix for the intra blocks                               */
    /*------------------------------------------------------------------------*/
    if(impeg2d_bit_stream_get_bit(ps_stream) == 1)
    {
        UWORD16 i;
        for(i = 0; i < NUM_PELS_IN_BLOCK; i++)
        {
            ps_dec->au1_intra_quant_matrix[gau1_impeg2_inv_scan_zig_zag[i]] =  (UWORD8)impeg2d_bit_stream_get(ps_stream,8);
        }

    }
    else
    {
        memcpy(ps_dec->au1_intra_quant_matrix,gau1_impeg2_intra_quant_matrix_default,
                NUM_PELS_IN_BLOCK);
    }

    /*------------------------------------------------------------------------*/
    /* Quantization matrix for the inter blocks                               */
    /*------------------------------------------------------------------------*/
    if(impeg2d_bit_stream_get_bit(ps_stream) == 1)
    {
        UWORD16 i;
        for(i = 0; i < NUM_PELS_IN_BLOCK; i++)
        {
            ps_dec->au1_inter_quant_matrix[gau1_impeg2_inv_scan_zig_zag[i]] =   (UWORD8)impeg2d_bit_stream_get(ps_stream,8);
        }
    }
    else
    {
        memcpy(ps_dec->au1_inter_quant_matrix,gau1_impeg2_inter_quant_matrix_default,
            NUM_PELS_IN_BLOCK);
    }
    impeg2d_next_start_code(ps_dec);

    return (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;
}

/******************************************************************************
*
*  Function Name   : impeg2d_dec_seq_ext
*
*  Description     : Gets additional sequence data.
*
*  Arguments       :
*  dec             : Decoder Context
*
*  Values Returned : None
******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_dec_seq_ext(dec_state_t *ps_dec)
{
    stream_t *ps_stream;

    ps_stream = &ps_dec->s_bit_stream;

    if (impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN) != EXTENSION_START_CODE)
    {
        impeg2d_bit_stream_flush(ps_stream,START_CODE_LEN);
        return IMPEG2D_FRM_HDR_START_CODE_NOT_FOUND;

    }
    /* Flush the extension start code */
    impeg2d_bit_stream_flush(ps_stream,START_CODE_LEN);

    /* Flush extension start code identifier */
    impeg2d_bit_stream_flush(ps_stream,4);

    /*----------------------------------------------------------------------*/
    /* Profile and Level information                                        */
    /*----------------------------------------------------------------------*/
    {
        UWORD32   u4_esc_bit, u4_profile, u4_level;

        /* Read the profile and level information */
        /* check_profile_and_level: Table 8-1     */
        /* [7:7] 1 Escape bit                     */
        /* [6:4] 3 Profile identification         */
        /* [3:0] 4 Level identification           */

        u4_esc_bit   = impeg2d_bit_stream_get_bit(ps_stream);
        u4_profile   = impeg2d_bit_stream_get(ps_stream,3);
        u4_level     = impeg2d_bit_stream_get(ps_stream,4);
        UNUSED(u4_profile);
        UNUSED(u4_level);
        /*
        if( escBit == 1                   ||
            profile < MPEG2_MAIN_PROFILE  ||
            level < MPEG2_MAIN_LEVEL)
            */
        if (1 == u4_esc_bit)
        {
            return IMPEG2D_PROF_LEVEL_NOT_SUPPORTED;
        }
    }

    ps_dec->u2_progressive_sequence = impeg2d_bit_stream_get_bit(ps_stream);

    /* Read the chrominance format */
    if(impeg2d_bit_stream_get(ps_stream,2) != 0x1)
        return IMPEG2D_CHROMA_FMT_NOT_SUP;

    /* Read the 2 most significant bits from horizontal_size */
    ps_dec->u2_horizontal_size    += (impeg2d_bit_stream_get(ps_stream,2) << 12);

    /* Read the 2 most significant bits from vertical_size */
    ps_dec->u2_vertical_size      += (impeg2d_bit_stream_get(ps_stream,2) << 12);

    /*-----------------------------------------------------------------------*/
    /* Flush the following as they are not used now                          */
    /* bit_rate_extension          12                                        */
    /* marker_bit                   1                                        */
    /* vbv_buffer_size_extension    8                                        */
    /* low_delay                    1                                        */
    /*-----------------------------------------------------------------------*/
    impeg2d_bit_stream_flush(ps_stream,12);
    GET_MARKER_BIT(ps_dec,ps_stream);
    impeg2d_bit_stream_flush(ps_stream,9);
    /*-----------------------------------------------------------------------*/
    /* frame_rate_extension_n       2                                        */
    /* frame_rate_extension_d       5                                        */
    /*-----------------------------------------------------------------------*/
    ps_dec->u2_frame_rate_extension_n = impeg2d_bit_stream_get(ps_stream,2);
    ps_dec->u2_frame_rate_extension_d = impeg2d_bit_stream_get(ps_stream,5);

    return (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;
}

/*******************************************************************************
*
*  Function Name   : impeg2d_dec_seq_disp_ext
*
*  Description     : This function is eqvt to sequence_display_extension() of
*                    standard. It flushes data present as it is not being used
*
*  Arguments       :
*  dec             : Decoder Context
*
*  Values Returned : None
******************************************************************************/
void impeg2d_dec_seq_disp_ext(dec_state_t *ps_dec)
{
    stream_t *ps_stream;
    ps_stream = &ps_dec->s_bit_stream;

    /*
    sequence_display_extension()
    {
        extension_start_code_identifier 4
        video_format                    3
        colour_description              1
        if (colour_description)
        {
            colour_primaries            8
            transfer_characteristics    8
            matrix_coefficients         8
        }
        display_horizontal_size         14
        marker_bit                      1
        display_vertical_size           14
        next_start_code()
    }
    */

    impeg2d_bit_stream_get(ps_stream,7);
    if (impeg2d_bit_stream_get_bit(ps_stream) == 1)
    {
        impeg2d_bit_stream_get(ps_stream,24);
    }

    /* display_horizontal_size and display_vertical_size */
    ps_dec->u2_display_horizontal_size = impeg2d_bit_stream_get(ps_stream,14);;
    GET_MARKER_BIT(ps_dec,ps_stream);
    ps_dec->u2_display_vertical_size   = impeg2d_bit_stream_get(ps_stream,14);

    impeg2d_next_start_code(ps_dec);
}


/*******************************************************************************
*
*  Function Name   : impeg2d_dec_seq_scale_ext
*
*  Description     : This function is eqvt to sequence_scalable_extension() of
*                    standard.
*
*  Arguments       : Decoder context
*
*  Values Returned : None
*******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_dec_seq_scale_ext(dec_state_t *ps_dec)
{
    UNUSED(ps_dec);
    return IMPEG2D_SCALABILITIY_NOT_SUPPORTED;
}

/*******************************************************************************
*
*  Function Name   : impeg2d_dec_quant_matrix_ext
*
*  Description     : Gets Intra and NonIntra quantizer matrix from the stream.
*
*  Arguments       : Decoder context
*
*  Values Returned : None
*******************************************************************************/
void impeg2d_dec_quant_matrix_ext(dec_state_t *ps_dec)
{
    stream_t *ps_stream;

    ps_stream = &ps_dec->s_bit_stream;
    /* Flush extension_start_code_identifier */
    impeg2d_bit_stream_flush(ps_stream,4);

    /*------------------------------------------------------------------------*/
    /* Quantization matrix for the intra blocks                               */
    /*------------------------------------------------------------------------*/
    if(impeg2d_bit_stream_get(ps_stream,1) == 1)
    {
        UWORD16 i;
        for(i = 0; i < NUM_PELS_IN_BLOCK; i++)
        {
            ps_dec->au1_intra_quant_matrix[gau1_impeg2_inv_scan_zig_zag[i]] =  (UWORD8)impeg2d_bit_stream_get(ps_stream,8);
        }

    }


    /*------------------------------------------------------------------------*/
    /* Quantization matrix for the inter blocks                               */
    /*------------------------------------------------------------------------*/
    if(impeg2d_bit_stream_get(ps_stream,1) == 1)
    {
        UWORD16 i;
        for(i = 0; i < NUM_PELS_IN_BLOCK; i++)
        {
            ps_dec->au1_inter_quant_matrix[gau1_impeg2_inv_scan_zig_zag[i]] =   (UWORD8)impeg2d_bit_stream_get(ps_stream,8);
        }
    }

    /* Note : chroma intra quantizer matrix and chroma non
    intra quantizer matrix are not needed for 4:2:0 format */
    impeg2d_next_start_code(ps_dec);
}
/*******************************************************************************
*
*  Function Name   : impeg2d_dec_pic_disp_ext
*
*  Description     : This function is eqvt to picture_display_extension() of
*                    standard.The parameters are not used by decoder
*
*  Arguments       : Pointer to dec_state_t
*
*  Values Returned : Decoder context
*
*  Values Returned : None
*******************************************************************************/
void impeg2d_dec_pic_disp_ext(dec_state_t *ps_dec)
{
    WORD16 i2_number_of_frame_centre_offsets ;
    stream_t *ps_stream;

    ps_stream = &ps_dec->s_bit_stream;
    impeg2d_bit_stream_flush(ps_stream,4);

    if (ps_dec->u2_progressive_sequence)
    {
        i2_number_of_frame_centre_offsets = (ps_dec->u2_repeat_first_field) ?
            2 + ps_dec->u2_top_field_first : 1;
    }
    else
    {
        i2_number_of_frame_centre_offsets =
            (ps_dec->u2_picture_structure != FRAME_PICTURE) ?
            1 : 2 + ps_dec->u2_repeat_first_field;
    }
    while(i2_number_of_frame_centre_offsets--)
    {
        /* frame_centre_horizontal_offset */
        impeg2d_bit_stream_get(ps_stream,16);
        GET_MARKER_BIT(ps_dec,ps_stream);
        /* frame_centre_vertical_offset */
        impeg2d_bit_stream_get(ps_stream,16);
        GET_MARKER_BIT(ps_dec,ps_stream);
    }
    impeg2d_next_start_code(ps_dec);
}

/*******************************************************************************
*
*  Function Name   : impeg2d_dec_itu_t_ext
*
*  Description     : This function is eqvt to ITU-T_extension() of
*                    standard.The parameters are not used by decoder
*
*  Arguments       : Decoder context
*
*  Values Returned : None
*******************************************************************************/
void impeg2d_dec_itu_t_ext(dec_state_t *ps_dec)
{
  impeg2d_bit_stream_flush(&ps_dec->s_bit_stream,EXT_ID_LEN);
  impeg2d_next_start_code(ps_dec);
}

/*******************************************************************************
*  Function Name   : impeg2d_dec_copyright_ext
*
*  Description     : This function is eqvt to copyright_extension() of
*                    standard. The parameters are not used by decoder
*
*  Arguments       : Decoder context
*
*  Values Returned : None
*******************************************************************************/


void impeg2d_dec_copyright_ext(dec_state_t *ps_dec)
{
    UWORD32 u4_bits_to_flush;

    u4_bits_to_flush = COPYRIGHT_EXTENSION_LEN;

    while(u4_bits_to_flush >= 32 )
    {
        impeg2d_bit_stream_flush(&ps_dec->s_bit_stream,32);
        u4_bits_to_flush = u4_bits_to_flush - 32;
    }

    if(u4_bits_to_flush > 0)
    {
        impeg2d_bit_stream_flush(&ps_dec->s_bit_stream,u4_bits_to_flush);
    }


  impeg2d_next_start_code(ps_dec);
}
/*******************************************************************************
*  Function Name   : impeg2d_dec_cam_param_ext
*
*  Description     : This function is eqvt to camera_parameters_extension() of
*                    standard. The parameters are not used by decoder
*
*  Arguments       : Decoder context
*
*  Values Returned : None
*******************************************************************************/


void impeg2d_dec_cam_param_ext(dec_state_t *ps_dec)
{

    UWORD32 u4_bits_to_flush;

    u4_bits_to_flush = CAMERA_PARAMETER_EXTENSION_LEN;

    while(u4_bits_to_flush >= 32 )
    {
        impeg2d_bit_stream_flush(&ps_dec->s_bit_stream,32);
        u4_bits_to_flush = u4_bits_to_flush - 32;
    }

    if(u4_bits_to_flush > 0)
    {
        impeg2d_bit_stream_flush(&ps_dec->s_bit_stream,u4_bits_to_flush);
    }

  impeg2d_next_start_code(ps_dec);
}

/*******************************************************************************
*
*  Function Name   : impeg2d_dec_grp_of_pic_hdr
*
*  Description     : Gets information at the GOP level.
*
*  Arguments       : Decoder context
*
*  Values Returned : None
*******************************************************************************/


void impeg2d_dec_grp_of_pic_hdr(dec_state_t *ps_dec)
{

    UWORD32 u4_bits_to_flush;

    u4_bits_to_flush = GROUP_OF_PICTURE_LEN;

    while(u4_bits_to_flush >= 32 )
    {
        impeg2d_bit_stream_flush(&ps_dec->s_bit_stream,32);
        u4_bits_to_flush = u4_bits_to_flush - 32;
    }

    if(u4_bits_to_flush > 0)
    {
        impeg2d_bit_stream_flush(&ps_dec->s_bit_stream,u4_bits_to_flush);
    }

}


/*******************************************************************************
*
*  Function Name   : impeg2d_dec_pic_hdr
*
*  Description     : Gets the picture header information.
*
*  Arguments       : Decoder context
*
*  Values Returned : None
*******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_dec_pic_hdr(dec_state_t *ps_dec)
{
    stream_t *ps_stream;
    ps_stream = &ps_dec->s_bit_stream;

    impeg2d_bit_stream_flush(ps_stream,START_CODE_LEN);
    /* Flush temporal reference */
    impeg2d_bit_stream_get(ps_stream,10);

    /* Picture type */
    ps_dec->e_pic_type = (e_pic_type_t)impeg2d_bit_stream_get(ps_stream,3);
    if((ps_dec->e_pic_type < I_PIC) || (ps_dec->e_pic_type > D_PIC))
    {
        impeg2d_next_code(ps_dec, PICTURE_START_CODE);
        return IMPEG2D_INVALID_PIC_TYPE;
    }

    /* Flush vbv_delay */
    impeg2d_bit_stream_get(ps_stream,16);

    if(ps_dec->e_pic_type == P_PIC || ps_dec->e_pic_type == B_PIC)
    {
        ps_dec->u2_full_pel_forw_vector = impeg2d_bit_stream_get_bit(ps_stream);
        ps_dec->u2_forw_f_code          = impeg2d_bit_stream_get(ps_stream,3);
    }
    if(ps_dec->e_pic_type == B_PIC)
    {
        ps_dec->u2_full_pel_back_vector = impeg2d_bit_stream_get_bit(ps_stream);
        ps_dec->u2_back_f_code          = impeg2d_bit_stream_get(ps_stream,3);
    }

    if(ps_dec->u2_is_mpeg2 == 0)
    {
        ps_dec->au2_f_code[0][0] = ps_dec->au2_f_code[0][1] = ps_dec->u2_forw_f_code;
        ps_dec->au2_f_code[1][0] = ps_dec->au2_f_code[1][1] = ps_dec->u2_back_f_code;
    }

    /*-----------------------------------------------------------------------*/
    /*  Flush the extra bit value                                            */
    /*                                                                       */
    /*  while(impeg2d_bit_stream_nxt() == '1')                                  */
    /*  {                                                                    */
    /*      extra_bit_picture         1                                      */
    /*      extra_information_picture 8                                      */
    /*  }                                                                    */
    /*  extra_bit_picture             1                                      */
    /*-----------------------------------------------------------------------*/
    while (impeg2d_bit_stream_nxt(ps_stream,1) == 1)
    {
        impeg2d_bit_stream_get(ps_stream,9);
    }
    impeg2d_bit_stream_get_bit(ps_stream);
    impeg2d_next_start_code(ps_dec);

    return (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;
}


/*******************************************************************************
*
*  Function Name   : impeg2d_dec_pic_coding_ext
*
*  Description     : Reads more picture level parameters
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
void impeg2d_dec_pic_coding_ext(dec_state_t *ps_dec)
{
    stream_t *ps_stream;

    ps_stream = &ps_dec->s_bit_stream;
    impeg2d_bit_stream_flush(ps_stream,START_CODE_LEN);
    /* extension code identifier */
    impeg2d_bit_stream_get(ps_stream,4);

    ps_dec->au2_f_code[0][0]             = impeg2d_bit_stream_get(ps_stream,4);
    ps_dec->au2_f_code[0][1]             = impeg2d_bit_stream_get(ps_stream,4);
    ps_dec->au2_f_code[1][0]             = impeg2d_bit_stream_get(ps_stream,4);
    ps_dec->au2_f_code[1][1]             = impeg2d_bit_stream_get(ps_stream,4);
    ps_dec->u2_intra_dc_precision        = impeg2d_bit_stream_get(ps_stream,2);
    ps_dec->u2_picture_structure            = impeg2d_bit_stream_get(ps_stream,2);
    ps_dec->u2_top_field_first              = impeg2d_bit_stream_get_bit(ps_stream);
    ps_dec->u2_frame_pred_frame_dct         = impeg2d_bit_stream_get_bit(ps_stream);
    ps_dec->u2_concealment_motion_vectors   = impeg2d_bit_stream_get_bit(ps_stream);
    ps_dec->u2_q_scale_type                 = impeg2d_bit_stream_get_bit(ps_stream);
    ps_dec->u2_intra_vlc_format             = impeg2d_bit_stream_get_bit(ps_stream);
    ps_dec->u2_alternate_scan               = impeg2d_bit_stream_get_bit(ps_stream);
    ps_dec->u2_repeat_first_field           = impeg2d_bit_stream_get_bit(ps_stream);
    /* Flush chroma_420_type */
    impeg2d_bit_stream_get_bit(ps_stream);

    ps_dec->u2_progressive_frame            = impeg2d_bit_stream_get_bit(ps_stream);
    if (impeg2d_bit_stream_get_bit(ps_stream))
    {
        /* Flush v_axis, field_sequence, burst_amplitude, sub_carrier_phase */
        impeg2d_bit_stream_flush(ps_stream,20);
    }
    impeg2d_next_start_code(ps_dec);


    if(VERTICAL_SCAN == ps_dec->u2_alternate_scan)
    {
        ps_dec->pu1_inv_scan_matrix = (UWORD8 *)gau1_impeg2_inv_scan_vertical;
    }
    else
    {
        ps_dec->pu1_inv_scan_matrix = (UWORD8 *)gau1_impeg2_inv_scan_zig_zag;
    }
}

/*******************************************************************************
*
*  Function Name   : impeg2d_dec_slice
*
*  Description     : Reads Slice level parameters and calls functions that
*                    decode individual MBs of slice
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_dec_slice(dec_state_t *ps_dec)
{
    stream_t *ps_stream;
    UWORD32 u4_slice_vertical_position;
    UWORD32 u4_slice_vertical_position_extension;
    IMPEG2D_ERROR_CODES_T e_error;

    ps_stream = &ps_dec->s_bit_stream;

    /*------------------------------------------------------------------------*/
    /* All the profiles supported require restricted slice structure. Hence   */
    /* there is no need to store slice_vertical_position. Note that max       */
    /* height supported does not exceed 2800 and scalablity is not supported  */
    /*------------------------------------------------------------------------*/

    /* Remove the slice start code */
    impeg2d_bit_stream_flush(ps_stream,START_CODE_PREFIX_LEN);
    u4_slice_vertical_position = impeg2d_bit_stream_get(ps_stream, 8);
    if(u4_slice_vertical_position > 2800)
    {
        u4_slice_vertical_position_extension = impeg2d_bit_stream_get(ps_stream, 3);
        u4_slice_vertical_position += (u4_slice_vertical_position_extension << 7);
    }

    if((u4_slice_vertical_position > ps_dec->u2_num_vert_mb) ||
       (u4_slice_vertical_position == 0))
    {
        return IMPEG2D_INVALID_VERT_SIZE;
    }

    // change the mb_y to point to slice_vertical_position
    u4_slice_vertical_position--;
    if (ps_dec->u2_mb_y != u4_slice_vertical_position)
    {
        ps_dec->u2_mb_y    = u4_slice_vertical_position;
        ps_dec->u2_mb_x    = 0;
    }
    ps_dec->u2_first_mb = 1;

    /*------------------------------------------------------------------------*/
    /* Quant scale code decoding                                              */
    /*------------------------------------------------------------------------*/
    {
        UWORD16 u2_quant_scale_code;
        u2_quant_scale_code = impeg2d_bit_stream_get(ps_stream,5);
        ps_dec->u1_quant_scale = (ps_dec->u2_q_scale_type) ?
            gau1_impeg2_non_linear_quant_scale[u2_quant_scale_code] : (u2_quant_scale_code << 1);
    }

    if (impeg2d_bit_stream_nxt(ps_stream,1) == 1)
    {
        impeg2d_bit_stream_flush(ps_stream,9);
        /* Flush extra bit information */
        while (impeg2d_bit_stream_nxt(ps_stream,1) == 1)
        {
            impeg2d_bit_stream_flush(ps_stream,9);
        }
    }
    impeg2d_bit_stream_get_bit(ps_stream);

    /* Reset the DC predictors to reset values given in Table 7.2 at the start*/
    /* of slice data */
    ps_dec->u2_def_dc_pred[Y_LUMA]   = 128 << ps_dec->u2_intra_dc_precision;
    ps_dec->u2_def_dc_pred[U_CHROMA]   = 128 << ps_dec->u2_intra_dc_precision;
    ps_dec->u2_def_dc_pred[V_CHROMA]   = 128 << ps_dec->u2_intra_dc_precision;
    /*------------------------------------------------------------------------*/
    /* dec->DecMBsinSlice() implements the following psuedo code from standard*/
    /* do                                                                     */
    /* {                                                                      */
    /*      macroblock()                                                      */
    /* } while (impeg2d_bit_stream_nxt() != '000 0000 0000 0000 0000 0000')      */
    /*------------------------------------------------------------------------*/

    e_error = ps_dec->pf_decode_slice(ps_dec);
    if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
    {
        return e_error;
    }

    /* Check for the MBy index instead of number of MBs left, because the
     * number of MBs left in case of multi-thread decode is the number of MBs
     * in that row only
     */
    if(ps_dec->u2_mb_y < ps_dec->u2_num_vert_mb)
        impeg2d_next_start_code(ps_dec);

    return (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;
}

void impeg2d_dec_pic_data_thread(dec_state_t *ps_dec)
{
    WORD32 i4_continue_decode;

    WORD32 i4_cur_row, temp;
    UWORD32 u4_bits_read;
    WORD32 i4_dequeue_job;
    IMPEG2D_ERROR_CODES_T e_error;

    i4_cur_row = ps_dec->u2_mb_y + 1;

    i4_continue_decode = 1;

    i4_dequeue_job = 1;
    do
    {
        if(i4_cur_row > ps_dec->u2_num_vert_mb)
        {
            i4_continue_decode = 0;
            break;
        }

        {
            if((ps_dec->i4_num_cores> 1) && (i4_dequeue_job))
            {
                job_t s_job;
                IV_API_CALL_STATUS_T e_ret;
                UWORD8 *pu1_buf;

                e_ret = impeg2_jobq_dequeue(ps_dec->pv_jobq, &s_job, sizeof(s_job), 1, 1);
                if(e_ret != IV_SUCCESS)
                    break;

                if(CMD_PROCESS == s_job.i4_cmd)
                {
                    pu1_buf = ps_dec->pu1_inp_bits_buf + s_job.i4_bistream_ofst;
                    impeg2d_bit_stream_init(&(ps_dec->s_bit_stream), pu1_buf,
                            (ps_dec->u4_num_inp_bytes - s_job.i4_bistream_ofst) + 8);
                    i4_cur_row      = s_job.i2_start_mb_y;
                    ps_dec->i4_start_mb_y = s_job.i2_start_mb_y;
                    ps_dec->i4_end_mb_y = s_job.i2_end_mb_y;
                    ps_dec->u2_mb_x = 0;
                    ps_dec->u2_mb_y = ps_dec->i4_start_mb_y;
                    ps_dec->u2_num_mbs_left = (ps_dec->i4_end_mb_y - ps_dec->i4_start_mb_y) * ps_dec->u2_num_horiz_mb;

                }
                else
                {
                    WORD32 start_row;
                    WORD32 num_rows;
                    start_row = s_job.i2_start_mb_y << 4;
                    num_rows = MIN((s_job.i2_end_mb_y << 4), ps_dec->u2_vertical_size);
                    num_rows -= start_row;
                    impeg2d_format_convert(ps_dec, ps_dec->ps_disp_pic,
                                        ps_dec->ps_disp_frm_buf,
                                        start_row, num_rows);
                    break;

                }

            }
            e_error = impeg2d_dec_slice(ps_dec);

            if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
            {
                impeg2d_next_start_code(ps_dec);
            }
        }

        /* Detecting next slice start code */
        while(1)
        {
            // skip (dec->u4_num_cores-1) rows
            u4_bits_read = impeg2d_bit_stream_nxt(&ps_dec->s_bit_stream,START_CODE_LEN);
            temp = u4_bits_read & 0xFF;
            i4_continue_decode = (((u4_bits_read >> 8) == 0x01) && (temp) && (temp <= 0xAF));

            if(i4_continue_decode)
            {
                /* If the slice is from the same row, then continue decoding without dequeue */
                if((temp - 1) == i4_cur_row)
                {
                    i4_dequeue_job = 0;
                    break;
                }

                if(temp < ps_dec->i4_end_mb_y)
                {
                    i4_cur_row = ps_dec->u2_mb_y;
                }
                else
                {
                    i4_dequeue_job = 1;
                }
                break;

            }
            else
                break;
        }

    }while(i4_continue_decode);
    if(ps_dec->i4_num_cores > 1)
    {
        while(1)
        {
            job_t s_job;
            IV_API_CALL_STATUS_T e_ret;

            e_ret = impeg2_jobq_dequeue(ps_dec->pv_jobq, &s_job, sizeof(s_job), 1, 1);
            if(e_ret != IV_SUCCESS)
                break;
            if(CMD_FMTCONV == s_job.i4_cmd)
            {
                WORD32 start_row;
                WORD32 num_rows;
                start_row = s_job.i2_start_mb_y << 4;
                num_rows = MIN((s_job.i2_end_mb_y << 4), ps_dec->u2_vertical_size);
                num_rows -= start_row;
                impeg2d_format_convert(ps_dec, ps_dec->ps_disp_pic,
                                    ps_dec->ps_disp_frm_buf,
                                    start_row, num_rows);
            }
        }
    }
    else
    {
        if((NULL != ps_dec->ps_disp_pic) && ((0 == ps_dec->u4_share_disp_buf) || (IV_YUV_420P != ps_dec->i4_chromaFormat)))
            impeg2d_format_convert(ps_dec, ps_dec->ps_disp_pic,
                            ps_dec->ps_disp_frm_buf,
                            0, ps_dec->u2_vertical_size);
    }
}

static WORD32 impeg2d_init_thread_dec_ctxt(dec_state_t *ps_dec,
                                           dec_state_t *ps_dec_thd,
                                           WORD32 i4_min_mb_y)
{
    UNUSED(i4_min_mb_y);
    ps_dec_thd->i4_start_mb_y = 0;
    ps_dec_thd->i4_end_mb_y = ps_dec->u2_num_vert_mb;
    ps_dec_thd->u2_mb_x = 0;
    ps_dec_thd->u2_mb_y = 0;
    ps_dec_thd->u2_is_mpeg2 = ps_dec->u2_is_mpeg2;
    ps_dec_thd->u2_frame_width = ps_dec->u2_frame_width;
    ps_dec_thd->u2_frame_height = ps_dec->u2_frame_height;
    ps_dec_thd->u2_picture_width = ps_dec->u2_picture_width;
    ps_dec_thd->u2_horizontal_size = ps_dec->u2_horizontal_size;
    ps_dec_thd->u2_vertical_size = ps_dec->u2_vertical_size;
    ps_dec_thd->u2_create_max_width = ps_dec->u2_create_max_width;
    ps_dec_thd->u2_create_max_height = ps_dec->u2_create_max_height;
    ps_dec_thd->u2_header_done = ps_dec->u2_header_done;
    ps_dec_thd->u2_decode_header = ps_dec->u2_decode_header;

    ps_dec_thd->u2_num_horiz_mb = ps_dec->u2_num_horiz_mb;
    ps_dec_thd->u2_num_vert_mb = ps_dec->u2_num_vert_mb;
    ps_dec_thd->u2_num_flds_decoded = ps_dec->u2_num_flds_decoded;

    ps_dec_thd->u4_frm_buf_stride = ps_dec->u4_frm_buf_stride;

    ps_dec_thd->u2_field_dct = ps_dec->u2_field_dct;
    ps_dec_thd->u2_read_dct_type = ps_dec->u2_read_dct_type;

    ps_dec_thd->u2_read_motion_type = ps_dec->u2_read_motion_type;
    ps_dec_thd->u2_motion_type = ps_dec->u2_motion_type;

    ps_dec_thd->pu2_mb_type = ps_dec->pu2_mb_type;
    ps_dec_thd->u2_fld_pic = ps_dec->u2_fld_pic;
    ps_dec_thd->u2_frm_pic = ps_dec->u2_frm_pic;

    ps_dec_thd->u2_fld_parity = ps_dec->u2_fld_parity;

    ps_dec_thd->au2_fcode_data[0] = ps_dec->au2_fcode_data[0];
    ps_dec_thd->au2_fcode_data[1] = ps_dec->au2_fcode_data[1];

    ps_dec_thd->u1_quant_scale = ps_dec->u1_quant_scale;

    ps_dec_thd->u2_num_mbs_left = ps_dec->u2_num_mbs_left;
    ps_dec_thd->u2_first_mb = ps_dec->u2_first_mb;
    ps_dec_thd->u2_num_skipped_mbs = ps_dec->u2_num_skipped_mbs;

    memcpy(&ps_dec_thd->s_cur_frm_buf, &ps_dec->s_cur_frm_buf, sizeof(yuv_buf_t));
    memcpy(&ps_dec_thd->as_recent_fld[0][0], &ps_dec->as_recent_fld[0][0], sizeof(yuv_buf_t));
    memcpy(&ps_dec_thd->as_recent_fld[0][1], &ps_dec->as_recent_fld[0][1], sizeof(yuv_buf_t));
    memcpy(&ps_dec_thd->as_recent_fld[1][0], &ps_dec->as_recent_fld[1][0], sizeof(yuv_buf_t));
    memcpy(&ps_dec_thd->as_recent_fld[1][1], &ps_dec->as_recent_fld[1][1], sizeof(yuv_buf_t));
    memcpy(&ps_dec_thd->as_ref_buf, &ps_dec->as_ref_buf, sizeof(yuv_buf_t) * 2 * 2);


    ps_dec_thd->pf_decode_slice = ps_dec->pf_decode_slice;

    ps_dec_thd->pf_vld_inv_quant = ps_dec->pf_vld_inv_quant;

    memcpy(ps_dec_thd->pf_idct_recon, ps_dec->pf_idct_recon, sizeof(ps_dec->pf_idct_recon));

    memcpy(ps_dec_thd->pf_mc, ps_dec->pf_mc, sizeof(ps_dec->pf_mc));
    ps_dec_thd->pf_interpolate = ps_dec->pf_interpolate;
    ps_dec_thd->pf_copy_mb = ps_dec->pf_copy_mb;
    ps_dec_thd->pf_fullx_halfy_8x8              =  ps_dec->pf_fullx_halfy_8x8;
    ps_dec_thd->pf_halfx_fully_8x8              =  ps_dec->pf_halfx_fully_8x8;
    ps_dec_thd->pf_halfx_halfy_8x8              =  ps_dec->pf_halfx_halfy_8x8;
    ps_dec_thd->pf_fullx_fully_8x8              =  ps_dec->pf_fullx_fully_8x8;

    ps_dec_thd->pf_memset_8bit_8x8_block        =  ps_dec->pf_memset_8bit_8x8_block;
    ps_dec_thd->pf_memset_16bit_8x8_linear_block        =  ps_dec->pf_memset_16bit_8x8_linear_block;
    ps_dec_thd->pf_copy_yuv420p_buf             =   ps_dec->pf_copy_yuv420p_buf;
    ps_dec_thd->pf_fmt_conv_yuv420p_to_yuv422ile    =   ps_dec->pf_fmt_conv_yuv420p_to_yuv422ile;
    ps_dec_thd->pf_fmt_conv_yuv420p_to_yuv420sp_uv  =   ps_dec->pf_fmt_conv_yuv420p_to_yuv420sp_uv;
    ps_dec_thd->pf_fmt_conv_yuv420p_to_yuv420sp_vu  =   ps_dec->pf_fmt_conv_yuv420p_to_yuv420sp_vu;


    memcpy(ps_dec_thd->au1_intra_quant_matrix, ps_dec->au1_intra_quant_matrix, NUM_PELS_IN_BLOCK * sizeof(UWORD8));
    memcpy(ps_dec_thd->au1_inter_quant_matrix, ps_dec->au1_inter_quant_matrix, NUM_PELS_IN_BLOCK * sizeof(UWORD8));
    ps_dec_thd->pu1_inv_scan_matrix = ps_dec->pu1_inv_scan_matrix;


    ps_dec_thd->u2_progressive_sequence = ps_dec->u2_progressive_sequence;
    ps_dec_thd->e_pic_type =  ps_dec->e_pic_type;
    ps_dec_thd->u2_full_pel_forw_vector = ps_dec->u2_full_pel_forw_vector;
    ps_dec_thd->u2_forw_f_code =   ps_dec->u2_forw_f_code;
    ps_dec_thd->u2_full_pel_back_vector = ps_dec->u2_full_pel_back_vector;
    ps_dec_thd->u2_back_f_code = ps_dec->u2_back_f_code;

    memcpy(ps_dec_thd->ai2_mv, ps_dec->ai2_mv, (2*2*2)*sizeof(WORD16));
    memcpy(ps_dec_thd->au2_f_code, ps_dec->au2_f_code, (2*2)*sizeof(UWORD16));
    ps_dec_thd->u2_intra_dc_precision = ps_dec->u2_intra_dc_precision;
    ps_dec_thd->u2_picture_structure = ps_dec->u2_picture_structure;
    ps_dec_thd->u2_top_field_first = ps_dec->u2_top_field_first;
    ps_dec_thd->u2_frame_pred_frame_dct = ps_dec->u2_frame_pred_frame_dct;
    ps_dec_thd->u2_concealment_motion_vectors = ps_dec->u2_concealment_motion_vectors;
    ps_dec_thd->u2_q_scale_type =  ps_dec->u2_q_scale_type;
    ps_dec_thd->u2_intra_vlc_format = ps_dec->u2_intra_vlc_format;
    ps_dec_thd->u2_alternate_scan = ps_dec->u2_alternate_scan;
    ps_dec_thd->u2_repeat_first_field = ps_dec->u2_repeat_first_field;
    ps_dec_thd->u2_progressive_frame = ps_dec->u2_progressive_frame;
    ps_dec_thd->pu1_inp_bits_buf = ps_dec->pu1_inp_bits_buf;
    ps_dec_thd->u4_num_inp_bytes = ps_dec->u4_num_inp_bytes;
    ps_dec_thd->pv_jobq = ps_dec->pv_jobq;
    ps_dec_thd->pv_jobq_buf = ps_dec->pv_jobq_buf;
    ps_dec_thd->i4_jobq_buf_size = ps_dec->i4_jobq_buf_size;


    ps_dec_thd->u2_frame_rate_code = ps_dec->u2_frame_rate_code;
    ps_dec_thd->u2_frame_rate_extension_n = ps_dec->u2_frame_rate_extension_n;
    ps_dec_thd->u2_frame_rate_extension_d = ps_dec->u2_frame_rate_extension_d;
    ps_dec_thd->u2_framePeriod =   ps_dec->u2_framePeriod;
    ps_dec_thd->u2_display_horizontal_size = ps_dec->u2_display_horizontal_size;
    ps_dec_thd->u2_display_vertical_size = ps_dec->u2_display_vertical_size;
    ps_dec_thd->u2_aspect_ratio_info = ps_dec->u2_aspect_ratio_info;

    ps_dec_thd->ps_func_bi_direct = ps_dec->ps_func_bi_direct;
    ps_dec_thd->ps_func_forw_or_back = ps_dec->ps_func_forw_or_back;

    return 0;

}


WORD32 impeg2d_get_slice_pos(dec_state_multi_core_t *ps_dec_state_multi_core)
{
    WORD32 u4_bits;
    WORD32 i4_row;


    dec_state_t *ps_dec = ps_dec_state_multi_core->ps_dec_state[0];
    WORD32 i4_prev_row;
    stream_t s_bitstrm;
    WORD32 i4_start_row;
    WORD32 i4_slice_bistream_ofst;
    WORD32 i;
    s_bitstrm = ps_dec->s_bit_stream;
    i4_prev_row = -1;

    ps_dec_state_multi_core->ps_dec_state[0]->i4_start_mb_y = 0;
    ps_dec_state_multi_core->ps_dec_state[1]->i4_start_mb_y = -1;
    ps_dec_state_multi_core->ps_dec_state[2]->i4_start_mb_y = -1;
    ps_dec_state_multi_core->ps_dec_state[3]->i4_start_mb_y = -1;

    ps_dec_state_multi_core->ps_dec_state[0]->i4_end_mb_y = ps_dec->u2_num_vert_mb;
    ps_dec_state_multi_core->ps_dec_state[1]->i4_end_mb_y = -1;
    ps_dec_state_multi_core->ps_dec_state[2]->i4_end_mb_y = -1;
    ps_dec_state_multi_core->ps_dec_state[3]->i4_end_mb_y = -1;

    if(ps_dec->i4_num_cores == 1)
        return 0;
    /* Reset the jobq to start of the jobq buffer */
    impeg2_jobq_reset((jobq_t *)ps_dec->pv_jobq);

    i4_start_row = -1;
    i4_slice_bistream_ofst = 0;
    while(1)
    {
        WORD32 i4_is_slice;

        if(s_bitstrm.u4_offset + START_CODE_LEN >= s_bitstrm.u4_max_offset)
        {
            break;
        }
        u4_bits = impeg2d_bit_stream_nxt(&s_bitstrm,START_CODE_LEN);

        i4_row = u4_bits & 0xFF;

        /* Detect end of frame */
        i4_is_slice = (((u4_bits >> 8) == 0x01) && (i4_row) && (i4_row <= ps_dec->u2_num_vert_mb));
        if(!i4_is_slice)
            break;

        i4_row -= 1;


        if(i4_prev_row != i4_row)
        {
            /* Create a job for previous slice row */
            if(i4_start_row != -1)
            {
                job_t s_job;
                IV_API_CALL_STATUS_T ret;
                s_job.i2_start_mb_y = i4_start_row;
                s_job.i2_end_mb_y = i4_row;
                s_job.i4_cmd = CMD_PROCESS;
                s_job.i4_bistream_ofst = i4_slice_bistream_ofst;
                ret = impeg2_jobq_queue(ps_dec->pv_jobq, &s_job, sizeof(s_job), 1, 0);
                if(ret != IV_SUCCESS)
                    return ret;

            }
            /* Store current slice's bitstream offset */
            i4_slice_bistream_ofst = s_bitstrm.u4_offset >> 3;
            i4_slice_bistream_ofst -= (size_t)s_bitstrm.pv_bs_buf & 3;
            i4_prev_row = i4_row;

            /* Store current slice's row position */
            i4_start_row = i4_row;

        }


        impeg2d_bit_stream_flush(&s_bitstrm, START_CODE_LEN);

        // flush bytes till next start code
        /* Flush the bytes till a  start code is encountered  */
        while(impeg2d_bit_stream_nxt(&s_bitstrm, 24) != START_CODE_PREFIX)
        {
            impeg2d_bit_stream_get(&s_bitstrm, 8);

            if(s_bitstrm.u4_offset >= s_bitstrm.u4_max_offset)
            {
                break;
            }
        }
    }

    /* Create job for the last slice row */
    {
        job_t s_job;
        IV_API_CALL_STATUS_T e_ret;
        s_job.i2_start_mb_y = i4_start_row;
        s_job.i2_end_mb_y = ps_dec->u2_num_vert_mb;
        s_job.i4_cmd = CMD_PROCESS;
        s_job.i4_bistream_ofst = i4_slice_bistream_ofst;
        e_ret = impeg2_jobq_queue(ps_dec->pv_jobq, &s_job, sizeof(s_job), 1, 0);
        if(e_ret != IV_SUCCESS)
            return e_ret;

    }
    if((NULL != ps_dec->ps_disp_pic) && ((0 == ps_dec->u4_share_disp_buf) || (IV_YUV_420P != ps_dec->i4_chromaFormat)))
    {
        for(i = 0; i < ps_dec->u2_vertical_size; i+=64)
        {
            job_t s_job;
            IV_API_CALL_STATUS_T ret;
            s_job.i2_start_mb_y = i;
            s_job.i2_start_mb_y >>= 4;
            s_job.i2_end_mb_y = (i + 64);
            s_job.i2_end_mb_y >>= 4;
            s_job.i4_cmd = CMD_FMTCONV;
            s_job.i4_bistream_ofst = 0;
            ret = impeg2_jobq_queue(ps_dec->pv_jobq, &s_job, sizeof(s_job), 1, 0);
            if(ret != IV_SUCCESS)
                return ret;

        }
    }

    impeg2_jobq_terminate(ps_dec->pv_jobq);
    ps_dec->i4_bytes_consumed = s_bitstrm.u4_offset >> 3;
    ps_dec->i4_bytes_consumed -= ((size_t)s_bitstrm.pv_bs_buf & 3);

    return 0;
}

/*******************************************************************************
*
*  Function Name   : impeg2d_dec_pic_data
*
*  Description     : It intializes several parameters and decodes a Picture
*                    till any slice is left.
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/

void impeg2d_dec_pic_data(dec_state_t *ps_dec)
{

    WORD32 i;
    dec_state_multi_core_t *ps_dec_state_multi_core;

    UWORD32  u4_error_code;

    dec_state_t *ps_dec_thd;
    WORD32 i4_status;
    WORD32 i4_min_mb_y;


    /* Resetting the MB address and MB coordinates at the start of the Frame */
    ps_dec->u2_mb_x = ps_dec->u2_mb_y = 0;
    u4_error_code = 0;

    ps_dec_state_multi_core = ps_dec->ps_dec_state_multi_core;
    impeg2d_get_slice_pos(ps_dec_state_multi_core);

    i4_min_mb_y = 1;
    for(i=0; i < ps_dec->i4_num_cores - 1; i++)
    {
        // initialize decoder context for thread
        // launch dec->u4_num_cores-1 threads

        ps_dec_thd = ps_dec_state_multi_core->ps_dec_state[i+1];

        ps_dec_thd->ps_disp_pic = ps_dec->ps_disp_pic;
        ps_dec_thd->ps_disp_frm_buf = ps_dec->ps_disp_frm_buf;

        i4_status = impeg2d_init_thread_dec_ctxt(ps_dec, ps_dec_thd, i4_min_mb_y);
        //impeg2d_dec_pic_data_thread(ps_dec_thd);

        if(i4_status == 0)
        {
            ithread_create(ps_dec_thd->pv_codec_thread_handle, NULL, (void *)impeg2d_dec_pic_data_thread, ps_dec_thd);
            ps_dec_state_multi_core->au4_thread_launched[i + 1] = 1;
            i4_min_mb_y = ps_dec_thd->u2_mb_y + 1;
        }
        else
        {
            ps_dec_state_multi_core->au4_thread_launched[i + 1] = 0;
            break;
        }
    }

    impeg2d_dec_pic_data_thread(ps_dec);

    // wait for threads to complete
    for(i=0; i < (ps_dec->i4_num_cores - 1); i++)
    {
        if(ps_dec_state_multi_core->au4_thread_launched[i + 1] == 1)
        {
            ps_dec_thd = ps_dec_state_multi_core->ps_dec_state[i+1];
            ithread_join(ps_dec_thd->pv_codec_thread_handle, NULL);
        }
    }

    ps_dec->u4_error_code = u4_error_code;

}
/*******************************************************************************
*
*  Function Name   : impeg2d_flush_ext_and_user_data
*
*  Description     : Flushes the extension and user data present in the
*                    stream_t
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
void impeg2d_flush_ext_and_user_data(dec_state_t *ps_dec)
{
    UWORD32 u4_start_code;
    stream_t *ps_stream;

    ps_stream    = &ps_dec->s_bit_stream;
    u4_start_code = impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN);

    while(u4_start_code == EXTENSION_START_CODE || u4_start_code == USER_DATA_START_CODE)
    {
        impeg2d_bit_stream_flush(ps_stream,START_CODE_LEN);
        while(impeg2d_bit_stream_nxt(ps_stream,START_CODE_PREFIX_LEN) != START_CODE_PREFIX)
        {
            impeg2d_bit_stream_flush(ps_stream,8);
        }
        u4_start_code = impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN);
    }
}
/*******************************************************************************
*
*  Function Name   : impeg2d_dec_user_data
*
*  Description     : Flushes the user data present in the stream_t
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
void impeg2d_dec_user_data(dec_state_t *ps_dec)
{
    UWORD32 u4_start_code;
    stream_t *ps_stream;

    ps_stream    = &ps_dec->s_bit_stream;
    u4_start_code = impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN);

    while(u4_start_code == USER_DATA_START_CODE)
    {
        impeg2d_bit_stream_flush(ps_stream,START_CODE_LEN);
        while(impeg2d_bit_stream_nxt(ps_stream,START_CODE_PREFIX_LEN) != START_CODE_PREFIX)
        {
            impeg2d_bit_stream_flush(ps_stream,8);
        }
        u4_start_code = impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN);
    }
}
/*******************************************************************************
*  Function Name   : impeg2d_dec_seq_ext_data
*
*  Description     : Decodes the extension data following Sequence
*                    Extension. It flushes any user data if present
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_dec_seq_ext_data(dec_state_t *ps_dec)
{
    stream_t   *ps_stream;
    UWORD32     u4_start_code;
    IMPEG2D_ERROR_CODES_T e_error;

    e_error = (IMPEG2D_ERROR_CODES_T) IVD_ERROR_NONE;

    ps_stream      = &ps_dec->s_bit_stream;
    u4_start_code = impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN);
    while( (u4_start_code == EXTENSION_START_CODE ||
            u4_start_code == USER_DATA_START_CODE) &&
            (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE == e_error)
    {
        if(u4_start_code == USER_DATA_START_CODE)
        {
            impeg2d_dec_user_data(ps_dec);
        }
        else
        {
            impeg2d_bit_stream_flush(ps_stream,START_CODE_LEN);
            u4_start_code   = impeg2d_bit_stream_nxt(ps_stream,EXT_ID_LEN);
            switch(u4_start_code)
            {
            case SEQ_DISPLAY_EXT_ID:
                impeg2d_dec_seq_disp_ext(ps_dec);
                break;
            case SEQ_SCALABLE_EXT_ID:
                e_error = IMPEG2D_SCALABILITIY_NOT_SUPPORTED;
                break;
            default:
                /* In case its a reserved extension code */
                impeg2d_bit_stream_flush(ps_stream,EXT_ID_LEN);
                impeg2d_peek_next_start_code(ps_dec);
                break;
            }
        }
        u4_start_code = impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN);
    }
    return e_error;
}
/*******************************************************************************
*  Function Name   : impeg2d_dec_pic_ext_data
*
*  Description     : Decodes the extension data following Picture Coding
*                    Extension. It flushes any user data if present
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_dec_pic_ext_data(dec_state_t *ps_dec)
{
    stream_t   *ps_stream;
    UWORD32     u4_start_code;
    IMPEG2D_ERROR_CODES_T e_error;

    e_error = (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;

    ps_stream      = &ps_dec->s_bit_stream;
    u4_start_code   = impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN);
    while ( (u4_start_code == EXTENSION_START_CODE ||
            u4_start_code == USER_DATA_START_CODE) &&
            (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE == e_error)
    {
        if(u4_start_code == USER_DATA_START_CODE)
        {
            impeg2d_dec_user_data(ps_dec);
        }
        else
        {
            impeg2d_bit_stream_flush(ps_stream,START_CODE_LEN);
            u4_start_code   = impeg2d_bit_stream_nxt(ps_stream,EXT_ID_LEN);
            switch(u4_start_code)
            {
            case QUANT_MATRIX_EXT_ID:
                impeg2d_dec_quant_matrix_ext(ps_dec);
                break;
            case COPYRIGHT_EXT_ID:
                impeg2d_dec_copyright_ext(ps_dec);
                break;
            case PIC_DISPLAY_EXT_ID:
                impeg2d_dec_pic_disp_ext(ps_dec);
                break;
            case CAMERA_PARAM_EXT_ID:
                impeg2d_dec_cam_param_ext(ps_dec);
                break;
            case ITU_T_EXT_ID:
                impeg2d_dec_itu_t_ext(ps_dec);
                break;
            case PIC_SPATIAL_SCALABLE_EXT_ID:
            case PIC_TEMPORAL_SCALABLE_EXT_ID:
                e_error = IMPEG2D_SCALABLITY_NOT_SUP;
                break;
            default:
                /* In case its a reserved extension code */
                impeg2d_bit_stream_flush(ps_stream,EXT_ID_LEN);
                impeg2d_next_start_code(ps_dec);
                break;
            }
        }
        u4_start_code = impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN);
    }
    return e_error;
}

/*******************************************************************************
*
*  Function Name   : impeg2d_process_video_header
*
*  Description     : Processes video sequence header information
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_process_video_header(dec_state_t *ps_dec)
{
    stream_t *ps_stream;
    ps_stream = &ps_dec->s_bit_stream;
    IMPEG2D_ERROR_CODES_T e_error;

    impeg2d_next_code(ps_dec, SEQUENCE_HEADER_CODE);
    if(ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset)
    {
        e_error = impeg2d_dec_seq_hdr(ps_dec);
        if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
        {
            return e_error;
        }
    }
    else
    {
      return IMPEG2D_BITSTREAM_BUFF_EXCEEDED_ERR;
    }
    if (impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN) == EXTENSION_START_CODE)
    {
        /* MPEG2 Decoder */
        if(ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset)
        {
            e_error = impeg2d_dec_seq_ext(ps_dec);
            if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
            {
                return e_error;
            }
        }
        else
        {
          return IMPEG2D_BITSTREAM_BUFF_EXCEEDED_ERR;
        }
        if(ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset)
        {
            e_error = impeg2d_dec_seq_ext_data(ps_dec);
            if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
            {
                return e_error;
            }
        }
        return impeg2d_init_video_state(ps_dec,MPEG_2_VIDEO);
    }
    else
    {
         /* MPEG1 Decoder */
        if(ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset)
        {
            impeg2d_flush_ext_and_user_data(ps_dec);
        }
        return impeg2d_init_video_state(ps_dec,MPEG_1_VIDEO);
    }
}
/*******************************************************************************
*
*  Function Name   : impeg2d_process_video_bit_stream
*
*  Description     : Processes video sequence header information
*
*  Arguments       :
*  dec             : Decoder context
*
*  Values Returned : None
*******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_process_video_bit_stream(dec_state_t *ps_dec)
{
    stream_t *ps_stream;
    UWORD32 u4_next_bits, u4_start_code_found;
    IMPEG2D_ERROR_CODES_T e_error;

    ps_stream = &ps_dec->s_bit_stream;
    impeg2d_next_start_code(ps_dec);
    /* If the stream is MPEG-2 compliant stream */
    u4_start_code_found = 0;

    if(ps_dec->u2_is_mpeg2)
    {
        /* MPEG2 decoding starts */
        while((u4_start_code_found == 0) && (ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset))
        {
            u4_next_bits = impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN);

            if(u4_next_bits == SEQUENCE_HEADER_CODE)
            {
                if(ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset)
                {
                    e_error = impeg2d_dec_seq_hdr(ps_dec);
                    if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
                    {
                        return e_error;
                    }

                    u4_start_code_found = 0;

                }
                else
                {
                    return IMPEG2D_BITSTREAM_BUFF_EXCEEDED_ERR;
                }


                if(ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset)
                {
                    IMPEG2D_ERROR_CODES_T e_error;
                    e_error = impeg2d_dec_seq_ext(ps_dec);
                    if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
                    {
                        return e_error;
                    }
                    u4_start_code_found = 0;

                }
                else
                {
                    return IMPEG2D_BITSTREAM_BUFF_EXCEEDED_ERR;
                }
            }
            else if((u4_next_bits == USER_DATA_START_CODE) || (u4_next_bits == EXTENSION_START_CODE))
            {
                if(ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset)
                {
                    impeg2d_dec_seq_ext_data(ps_dec);
                    u4_start_code_found = 0;

                }

            }
            else if((ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset)
                    && (u4_next_bits == GOP_START_CODE))
            {
                impeg2d_dec_grp_of_pic_hdr(ps_dec);
                impeg2d_dec_user_data(ps_dec);
                u4_start_code_found = 0;

            }
            else if((ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset)
                    && (u4_next_bits == PICTURE_START_CODE))
            {

                e_error = impeg2d_dec_pic_hdr(ps_dec);
                if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
                {
                    return e_error;
                }
                impeg2d_dec_pic_coding_ext(ps_dec);
                e_error = impeg2d_dec_pic_ext_data(ps_dec);
                if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
                {
                    return e_error;
                }
                impeg2d_pre_pic_dec_proc(ps_dec);
                impeg2d_dec_pic_data(ps_dec);
                impeg2d_post_pic_dec_proc(ps_dec);
                u4_start_code_found = 1;
            }
            else

            {
                FLUSH_BITS(ps_dec->s_bit_stream.u4_offset, ps_dec->s_bit_stream.u4_buf, ps_dec->s_bit_stream.u4_buf_nxt, 8, ps_dec->s_bit_stream.pu4_buf_aligned);

            }
            if(u4_start_code_found == 0)
            {
                impeg2d_next_start_code(ps_dec);
            }
        }
        if((u4_start_code_found == 0) && (ps_dec->s_bit_stream.u4_offset > ps_dec->s_bit_stream.u4_max_offset))
        {
            return IMPEG2D_FRM_HDR_START_CODE_NOT_FOUND;
        }

    }
        /* If the stream is MPEG-1 compliant stream */
    else
    {
        while((u4_start_code_found == 0) && (ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset))
        {
            u4_next_bits = impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN);

            if(impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN) == SEQUENCE_HEADER_CODE)
            {
                if(ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset)
                {
                    e_error = impeg2d_dec_seq_hdr(ps_dec);
                    if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
                    {
                        return e_error;
                    }

                    u4_start_code_found = 0;
                }
                else
                {
                    return IMPEG2D_BITSTREAM_BUFF_EXCEEDED_ERR;
                }
            }
            else if((ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset) && (u4_next_bits == EXTENSION_START_CODE || u4_next_bits == USER_DATA_START_CODE))
            {
                impeg2d_flush_ext_and_user_data(ps_dec);
                u4_start_code_found = 0;
            }


            else if ((impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN) == GOP_START_CODE)
                    && (ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset))
            {
                impeg2d_dec_grp_of_pic_hdr(ps_dec);
                impeg2d_flush_ext_and_user_data(ps_dec);
                u4_start_code_found = 0;
            }
            else if ((impeg2d_bit_stream_nxt(ps_stream,START_CODE_LEN) == PICTURE_START_CODE)
                    && (ps_dec->s_bit_stream.u4_offset < ps_dec->s_bit_stream.u4_max_offset))
            {

                e_error = impeg2d_dec_pic_hdr(ps_dec);
                if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
                {
                    return e_error;
                }
                impeg2d_flush_ext_and_user_data(ps_dec);
                impeg2d_pre_pic_dec_proc(ps_dec);
                impeg2d_dec_pic_data(ps_dec);
                impeg2d_post_pic_dec_proc(ps_dec);
                u4_start_code_found = 1;
            }
            else
            {
                FLUSH_BITS(ps_dec->s_bit_stream.u4_offset, ps_dec->s_bit_stream.u4_buf, ps_dec->s_bit_stream.u4_buf_nxt, 8, ps_dec->s_bit_stream.pu4_buf_aligned);
            }
            impeg2d_next_start_code(ps_dec);

        }
        if((u4_start_code_found == 0) && (ps_dec->s_bit_stream.u4_offset > ps_dec->s_bit_stream.u4_max_offset))
        {
           return IMPEG2D_FRM_HDR_START_CODE_NOT_FOUND;
        }
    }

    return (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;
}
