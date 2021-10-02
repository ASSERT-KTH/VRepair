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
/*  File Name         : ih264d_sei.c                                                */
/*                                                                           */
/*  Description       : This file contains routines to parse SEI NAL's       */
/*                                                                           */
/*  List of Functions : <List the functions defined in this file>            */
/*                                                                           */
/*  Issues / Problems : None                                                 */
/*                                                                           */
/*  Revision History  :                                                      */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         25 05 2005   NS              Draft                                */
/*                                                                           */
/*****************************************************************************/

#include "ih264_typedefs.h"
#include "ih264_macros.h"
#include "ih264_platform_macros.h"
#include "ih264d_sei.h"
#include "ih264d_bitstrm.h"
#include "ih264d_structs.h"
#include "ih264d_error_handler.h"
#include "ih264d_vui.h"
#include "ih264d_parse_cavlc.h"
#include "ih264d_defs.h"

/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_parse_buffering_period                                   */
/*                                                                           */
/*  Description   : This function parses SEI message buffering_period        */
/*  Inputs        : ps_buf_prd pointer to struct buf_period_t                  */
/*                  ps_bitstrm    Bitstream                                */
/*  Globals       : None                                                     */
/*  Processing    : Parses SEI payload buffering period.                     */
/*  Outputs       : None                                                     */
/*  Returns       : None                                                     */
/*                                                                           */
/*  Issues        : Not implemented fully                                    */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         06 05 2002   NS              Draft                                */
/*                                                                           */
/*****************************************************************************/

WORD32 ih264d_parse_buffering_period(buf_period_t *ps_buf_prd,
                                     dec_bit_stream_t *ps_bitstrm,
                                     dec_struct_t *ps_dec)
{
    UWORD8 u1_seq_parameter_set_id;
    dec_seq_params_t *ps_seq;
    UWORD8 u1_nal_hrd_present, u1_vcl_hrd_present;
    UWORD32 i;
    UWORD32 *pu4_bitstrm_ofst = &ps_bitstrm->u4_ofst;
    UWORD32 *pu4_bitstrm_buf = ps_bitstrm->pu4_buffer;
    UNUSED(ps_buf_prd);
    u1_seq_parameter_set_id = ih264d_uev(pu4_bitstrm_ofst,
                                         pu4_bitstrm_buf);
    if(u1_seq_parameter_set_id >= MAX_NUM_SEQ_PARAMS)
        return ERROR_INVALID_SEQ_PARAM;
    ps_seq = &ps_dec->ps_sps[u1_seq_parameter_set_id];
    if(TRUE != ps_seq->u1_is_valid)
        return (-1);

    ps_dec->ps_sei->u1_seq_param_set_id = u1_seq_parameter_set_id;
    ps_dec->ps_cur_sps = ps_seq;
    if(FALSE == ps_seq->u1_is_valid)
        return ERROR_INVALID_SEQ_PARAM;
    if(1 == ps_seq->u1_vui_parameters_present_flag)
    {
        u1_nal_hrd_present = ps_seq->s_vui.u1_nal_hrd_params_present;
        if(u1_nal_hrd_present)
        {
            for(i = 0; i < ps_seq->s_vui.s_nal_hrd.u4_cpb_cnt; i++)
            {
                ih264d_get_bits_h264(
                                ps_bitstrm,
                                ps_seq->s_vui.s_nal_hrd.u1_initial_cpb_removal_delay);
                ih264d_get_bits_h264(
                                ps_bitstrm,
                                ps_seq->s_vui.s_nal_hrd.u1_initial_cpb_removal_delay);
            }
        }

        u1_vcl_hrd_present = ps_seq->s_vui.u1_vcl_hrd_params_present;
        if(u1_vcl_hrd_present)
        {
            for(i = 0; i < ps_seq->s_vui.s_vcl_hrd.u4_cpb_cnt; i++)
            {
                ih264d_get_bits_h264(
                                ps_bitstrm,
                                ps_seq->s_vui.s_vcl_hrd.u1_initial_cpb_removal_delay);
                ih264d_get_bits_h264(
                                ps_bitstrm,
                                ps_seq->s_vui.s_vcl_hrd.u1_initial_cpb_removal_delay);
            }
        }
    }
    return OK;
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_parse_pic_timing                                         */
/*                                                                           */
/*  Description   : This function parses SEI message pic_timing              */
/*  Inputs        : ps_bitstrm    Bitstream                                */
/*                  ps_dec          Poniter decoder context                  */
/*                  ui4_payload_size pay load i4_size                           */
/*  Globals       : None                                                     */
/*  Processing    : Parses SEI payload picture timing                        */
/*  Outputs       : None                                                     */
/*  Returns       : None                                                     */
/*                                                                           */
/*  Issues        : Not implemented fully                                    */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         06 05 2002   NS              Draft                                */
/*                                                                           */
/*****************************************************************************/
WORD32 ih264d_parse_pic_timing(dec_bit_stream_t *ps_bitstrm,
                               dec_struct_t *ps_dec,
                               UWORD32 ui4_payload_size)
{
    sei *ps_sei;
    vui_t *ps_vu4;
    UWORD8 u1_cpb_dpb_present;
    UWORD8 u1_pic_struct_present_flag;
    UWORD32 u4_start_offset, u4_bits_consumed;
    UWORD8 u1_cpb_removal_delay_length, u1_dpb_output_delay_length;

    ps_sei = (sei *)ps_dec->ps_sei;
    ps_vu4 = &ps_dec->ps_cur_sps->s_vui;

    u1_cpb_dpb_present = ps_vu4->u1_vcl_hrd_params_present
                    + ps_vu4->u1_nal_hrd_params_present;

    if(ps_vu4->u1_vcl_hrd_params_present)
    {
        u1_cpb_removal_delay_length =
                        ps_vu4->s_vcl_hrd.u1_cpb_removal_delay_length;
        u1_dpb_output_delay_length =
                        ps_vu4->s_vcl_hrd.u1_dpb_output_delay_length;
    }
    else if(ps_vu4->u1_nal_hrd_params_present)
    {
        u1_cpb_removal_delay_length =
                        ps_vu4->s_nal_hrd.u1_cpb_removal_delay_length;
        u1_dpb_output_delay_length =
                        ps_vu4->s_nal_hrd.u1_dpb_output_delay_length;
    }
    else
    {
        u1_cpb_removal_delay_length = 24;
        u1_dpb_output_delay_length = 24;

    }

    u4_start_offset = ps_bitstrm->u4_ofst;
    if(u1_cpb_dpb_present)
    {
        ih264d_get_bits_h264(ps_bitstrm, u1_cpb_removal_delay_length);
        ih264d_get_bits_h264(ps_bitstrm, u1_dpb_output_delay_length);
    }

    u1_pic_struct_present_flag = ps_vu4->u1_pic_struct_present_flag;
    if(u1_pic_struct_present_flag)
    {
        ps_sei->u1_pic_struct = ih264d_get_bits_h264(ps_bitstrm, 4);
        ps_dec->u1_pic_struct_copy = ps_sei->u1_pic_struct;
        ps_sei->u1_is_valid = 1;
    }
    u4_bits_consumed = ps_bitstrm->u4_ofst - u4_start_offset;
    ih264d_flush_bits_h264(ps_bitstrm,
                           (ui4_payload_size << 3) - u4_bits_consumed);

    return (0);
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_parse_recovery_point                                     */
/*                                                                           */
/*  Description   : This function parses SEI message recovery point          */
/*  Inputs        : ps_bitstrm    Bitstream                                */
/*                  ps_dec          Poniter decoder context                  */
/*                  ui4_payload_size pay load i4_size                           */
/*  Globals       : None                                                     */
/*  Processing    : Parses SEI payload picture timing                        */
/*  Outputs       : None                                                     */
/*  Returns       : None                                                     */
/*                                                                           */
/*  Issues        : Not implemented fully                                    */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         06 05 2002   NS              Draft                                */
/*                                                                           */
/*****************************************************************************/
WORD32 ih264d_parse_recovery_point(dec_bit_stream_t *ps_bitstrm,
                                   dec_struct_t *ps_dec,
                                   UWORD32 ui4_payload_size)
{
    sei *ps_sei = ps_dec->ps_sei;
    dec_err_status_t *ps_err = ps_dec->ps_dec_err_status;
    UWORD32 *pu4_bitstrm_ofst = &ps_bitstrm->u4_ofst;
    UWORD32 *pu4_bitstrm_buf = ps_bitstrm->pu4_buffer;
    UNUSED(ui4_payload_size);
    ps_sei->u2_recovery_frame_cnt = ih264d_uev(pu4_bitstrm_ofst,
                                               pu4_bitstrm_buf);
    ps_err->u4_frm_sei_sync = ps_err->u4_cur_frm
                    + ps_sei->u2_recovery_frame_cnt;
    ps_sei->u1_exact_match_flag = ih264d_get_bit_h264(ps_bitstrm);
    ps_sei->u1_broken_link_flag = ih264d_get_bit_h264(ps_bitstrm);
    ps_sei->u1_changing_slice_grp_idc = ih264d_get_bits_h264(ps_bitstrm, 2);

    return (0);
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_parse_sei_payload                                        */
/*                                                                           */
/*  Description   : This function parses SEI pay loads. Currently it's       */
/*                  implemented partially.                                   */
/*  Inputs        : ps_bitstrm    Bitstream                                */
/*                  ui4_payload_type  SEI payload type                       */
/*                  ui4_payload_size  SEI payload i4_size                       */
/*  Globals       : None                                                     */
/*  Processing    : Parses SEI payloads units and stores the info            */
/*  Outputs       : None                                                     */
/*  Returns       : None                                                     */
/*                                                                           */
/*  Issues        : Not implemented fully                                    */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         06 05 2002   NS              Draft                                */
/*                                                                           */
/*****************************************************************************/

WORD32 ih264d_parse_sei_payload(dec_bit_stream_t *ps_bitstrm,
                                UWORD32 ui4_payload_type,
                                UWORD32 ui4_payload_size,
                                dec_struct_t *ps_dec)
{
    sei *ps_sei;
    WORD32 i4_status = 0;
    ps_sei = (sei *)ps_dec->ps_sei;
    switch(ui4_payload_type)
    {
        case SEI_BUF_PERIOD:

            i4_status = ih264d_parse_buffering_period(&ps_sei->s_buf_period,
                                                      ps_bitstrm, ps_dec);
            /*if(i4_status != OK)
                return i4_status;*/
            break;
        case SEI_PIC_TIMING:
            if(NULL == ps_dec->ps_cur_sps)
                ih264d_flush_bits_h264(ps_bitstrm, (ui4_payload_size << 3));
            else
                ih264d_parse_pic_timing(ps_bitstrm, ps_dec,
                                        ui4_payload_size);
            break;
        case SEI_RECOVERY_PT:
            ih264d_parse_recovery_point(ps_bitstrm, ps_dec,
                                        ui4_payload_size);
            break;
        default:
            ih264d_flush_bits_h264(ps_bitstrm, (ui4_payload_size << 3));
            break;
    }
    return (i4_status);
}

/*****************************************************************************/
/*                                                                           */
/*  Function Name : ih264d_parse_sei_message                                        */
/*                                                                           */
/*  Description   : This function is parses and decode SEI. Currently it's   */
/*                  not implemented fully.                                   */
/*  Inputs        : ps_dec    Decoder parameters                       */
/*                  ps_bitstrm    Bitstream                                */
/*  Globals       : None                                                     */
/*  Processing    : Parses SEI NAL units and stores the info                 */
/*  Outputs       : None                                                     */
/*  Returns       : None                                                     */
/*                                                                           */
/*  Issues        : Not implemented fully                                    */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         06 05 2002   NS              Draft                                */
/*                                                                           */
/*****************************************************************************/

WORD32 ih264d_parse_sei_message(dec_struct_t *ps_dec,
                                dec_bit_stream_t *ps_bitstrm)
{
    UWORD32 ui4_payload_type, ui4_payload_size;
    UWORD32 u4_bits;
    WORD32 i4_status = 0;

    do
    {
        ui4_payload_type = 0;

        u4_bits = ih264d_get_bits_h264(ps_bitstrm, 8);
        while(0xff == u4_bits && !EXCEED_OFFSET(ps_bitstrm))
        {
            u4_bits = ih264d_get_bits_h264(ps_bitstrm, 8);
            ui4_payload_type += 255;
        }
        ui4_payload_type += u4_bits;

        ui4_payload_size = 0;
        u4_bits = ih264d_get_bits_h264(ps_bitstrm, 8);
        while(0xff == u4_bits && !EXCEED_OFFSET(ps_bitstrm))
        {
            u4_bits = ih264d_get_bits_h264(ps_bitstrm, 8);
            ui4_payload_size += 255;
        }
        ui4_payload_size += u4_bits;

        i4_status = ih264d_parse_sei_payload(ps_bitstrm, ui4_payload_type,
                                             ui4_payload_size, ps_dec);
        if(i4_status == -1)
        {
            i4_status = 0;
            break;
        }

        if(i4_status != OK)
            return i4_status;

        if(ih264d_check_byte_aligned(ps_bitstrm) == 0)
        {
            u4_bits = ih264d_get_bit_h264(ps_bitstrm);
            if(0 == u4_bits)
            {
                H264_DEC_DEBUG_PRINT("\nError in parsing SEI message");
            }
            while(0 == ih264d_check_byte_aligned(ps_bitstrm)
                            && !EXCEED_OFFSET(ps_bitstrm))
            {
                u4_bits = ih264d_get_bit_h264(ps_bitstrm);
                if(u4_bits)
                {
                    H264_DEC_DEBUG_PRINT("\nError in parsing SEI message");
                }
            }
        }
    }
    while(ps_bitstrm->u4_ofst < ps_bitstrm->u4_max_ofst);
    return (i4_status);
}

