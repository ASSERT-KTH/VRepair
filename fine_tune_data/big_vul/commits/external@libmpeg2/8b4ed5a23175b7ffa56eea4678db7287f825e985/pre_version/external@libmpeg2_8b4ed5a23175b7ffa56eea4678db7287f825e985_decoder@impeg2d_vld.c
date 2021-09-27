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


/*******************************************************************************
* Function name : impeg2d_dec_vld_symbol
*
* Description   : Performs decoding of VLD symbol. It performs decoding by
*                 processing 1 bit at a time
*
* Arguments     :
* stream        : Bitstream
* ai2_code_table     : Table used for decoding
* maxLen        : Maximum Length of the decoded symbol in bits
*
* Value Returned: Decoded symbol
*******************************************************************************/
WORD16 impeg2d_dec_vld_symbol(stream_t *ps_stream,const WORD16 ai2_code_table[][2],  UWORD16 u2_max_len)
{
  UWORD16 u2_data;
  WORD16  u2_end = 0;
  UWORD16 u2_org_max_len = u2_max_len;
  UWORD16 u2_i_bit;

  /* Get the maximum number of bits needed to decode a symbol */
  u2_data = impeg2d_bit_stream_nxt(ps_stream,u2_max_len);
  do
  {
    u2_max_len--;
    /* Read one bit at a time from the variable to decode the huffman code */
    u2_i_bit = (UWORD8)((u2_data >> u2_max_len) & 0x1);

    /* Get the next node pointer or the symbol from the tree */
    u2_end = ai2_code_table[u2_end][u2_i_bit];
  }while(u2_end > 0);

  /* Flush the appropriate number of bits from the ps_stream */
  impeg2d_bit_stream_flush(ps_stream,(UWORD8)(u2_org_max_len - u2_max_len));
  return(u2_end);
}
/*******************************************************************************
* Function name : impeg2d_fast_dec_vld_symbol
*
* Description   : Performs decoding of VLD symbol. It performs decoding by
*                 processing n bits at a time
*
* Arguments     :
* stream        : Bitstream
* ai2_code_table     : Code table containing huffman value
* indexTable    : Index table containing index
* maxLen        : Maximum Length of the decoded symbol in bits
*
* Value Returned: Decoded symbol
*******************************************************************************/
WORD16 impeg2d_fast_dec_vld_symbol(stream_t *ps_stream,
                     const WORD16  ai2_code_table[][2],
                     const UWORD16 au2_indexTable[][2],
                     UWORD16 u2_max_len)
{
    UWORD16 u2_cur_code;
    UWORD16 u2_num_bits;
    UWORD16 u2_vld_offset;
    UWORD16 u2_start_len;
    WORD16  u2_value;
    UWORD16 u2_len;
    UWORD16 u2_huffCode;

    u2_start_len  = au2_indexTable[0][0];
    u2_vld_offset = 0;
    u2_huffCode  = impeg2d_bit_stream_nxt(ps_stream,u2_max_len);
    do
    {
        u2_cur_code = u2_huffCode >> (u2_max_len - u2_start_len);
        u2_num_bits = ai2_code_table[u2_cur_code + u2_vld_offset][0];
        if(u2_num_bits == 0)
        {
            u2_huffCode  &= ((1 << (u2_max_len - u2_start_len)) - 1);
            u2_max_len    -= u2_start_len;
            u2_start_len   = au2_indexTable[ai2_code_table[u2_cur_code + u2_vld_offset][1]][0];
            u2_vld_offset  = au2_indexTable[ai2_code_table[u2_cur_code + u2_vld_offset][1]][1];
        }
        else
        {
            u2_value = ai2_code_table[u2_cur_code + u2_vld_offset][1];
            u2_len   = u2_num_bits;
        }
    }while(u2_num_bits == 0);
    impeg2d_bit_stream_flush(ps_stream,u2_len);
    return(u2_value);
}
/******************************************************************************
*
*  Function Name   : impeg2d_dec_ac_coeff_zero
*
*  Description     : Decodes using Table B.14
*
*  Arguments       : Pointer to VideoObjectLayerStructure
*
*  Values Returned : Decoded value
*
*  Revision History:
*
*         28 02 2002  AR        Creation
*******************************************************************************/
UWORD16 impeg2d_dec_ac_coeff_zero(stream_t *ps_stream, UWORD16* pu2_sym_len, UWORD16* pu2_sym_val)
{
    UWORD16 u2_offset,u2_decoded_value;
    UWORD8  u1_shift;
    UWORD32 u4_bits_read;

    u4_bits_read = (UWORD16)impeg2d_bit_stream_nxt(ps_stream,MPEG2_AC_COEFF_MAX_LEN);

    if ((UWORD16)u4_bits_read >= 0x0800)
    {
        u2_offset = (UWORD16)u4_bits_read >> 11;
    }
    else if ((UWORD16)u4_bits_read >= 0x40)
    {
        u2_offset = 31 + ((UWORD16)u4_bits_read >> 6);
    }
    else if ((UWORD16)u4_bits_read >= 0x20)
    {
        u2_offset = 64;
    }
    else
    {
        u2_offset      = 63;
        u4_bits_read    = (UWORD16)u4_bits_read - 0x10;
    }
    /*-----------------------------------------------------------------------
     * The table gOffset contains both the offset for the group to which the
     * Vld code belongs in the Ac Coeff Table and the no of bits with which
     * the BitsRead should be shifted
     *-----------------------------------------------------------------------*/
    u2_offset = gau2_impeg2d_offset_zero[u2_offset];
    u1_shift  = u2_offset & 0xF;

    /*-----------------------------------------------------------------------
     * Depending upon the vld code, we index exactly to that particular
     * Vld codes value in the Ac Coeff Table.
     * (Offset >> 4)       gives the offset for the group in the AcCoeffTable.
     * (BitsRead >> shift) gives the offset within its group
     *-----------------------------------------------------------------------*/
     u2_offset = (u2_offset >> 4) + ((UWORD16)u4_bits_read >> u1_shift);
    /*-----------------------------------------------------------------------
     * DecodedValue has the Run, Level and the number of bits used by Vld code
     *-----------------------------------------------------------------------*/
    u2_decoded_value = gau2_impeg2d_dct_coeff_zero[u2_offset];
    if(u2_decoded_value == END_OF_BLOCK)
    {
        *pu2_sym_len = 2;
        *pu2_sym_val = EOB_CODE_VALUE;
    }
    else if(u2_decoded_value == ESCAPE_CODE)
    {
        *pu2_sym_len     = u2_decoded_value & 0x1F;
        *pu2_sym_val = ESC_CODE_VALUE;
    }
    else
    {
        *pu2_sym_len = u2_decoded_value & 0x1F;
        *pu2_sym_val = u2_decoded_value >> 5;
    }
    return(u2_decoded_value);
}

/******************************************************************************
*
*  Function Name   : impeg2d_dec_ac_coeff_one
*
*  Description     : Decodes using Table B.15
*
*  Arguments       : Pointer to VideoObjectLayerStructure
*
*  Values Returned : Decoded value
*
*  Revision History:
*
*         28 02 2002  AR        Creation
*******************************************************************************/
UWORD16 impeg2d_dec_ac_coeff_one(stream_t *ps_stream, UWORD16* pu2_sym_len, UWORD16* pu2_sym_val)
{
    UWORD16 u2_offset, u2_decoded_value;
    UWORD8  u1_shift;
    UWORD32 u4_bits_read;


    u4_bits_read = (UWORD16)impeg2d_bit_stream_nxt(ps_stream,MPEG2_AC_COEFF_MAX_LEN);

    if ((UWORD16)u4_bits_read >= 0x8000)
    {
        /* If the MSB of the vld code is 1 */
        if (((UWORD16)u4_bits_read >> 12) == 0xF)
            u2_offset = ((UWORD16)u4_bits_read >> 8) & 0xF;
        else
            u2_offset = (UWORD16)u4_bits_read >> 11;
        u2_offset += gau2_impeg2d_offset_one[0];
    }
    else if ((UWORD16)u4_bits_read >= 0x400)
    {
        u2_offset =(UWORD16) u4_bits_read >> 10;
        u2_offset = gau2_impeg2d_offset_one[u2_offset];
        u1_shift = u2_offset & 0xF;
        u2_offset = (u2_offset >> 4) + ((UWORD16)u4_bits_read >> u1_shift);
    }
    else if ((UWORD16)u4_bits_read >= 0x20)
    {
        u2_offset = ((UWORD16)u4_bits_read >> 5) + 31;
        u2_offset = gau2_impeg2d_offset_one[u2_offset];
        u1_shift = u2_offset & 0xF;
        u2_offset = (u2_offset >> 4) + ((UWORD16)u4_bits_read >> u1_shift);
    }
    else
    {
        u2_offset = gau2_impeg2d_offset_one[63] + ((UWORD16)u4_bits_read & 0xF);
    }
    /*-----------------------------------------------------------------------
    * DecodedValue has the Run, Level and the number of bits used by Vld code
    *-----------------------------------------------------------------------*/
    u2_decoded_value = gau2_impeg2d_dct_coeff_one[u2_offset];

    if(u2_decoded_value == END_OF_BLOCK)
    {
        *pu2_sym_len = 4;
        *pu2_sym_val = EOB_CODE_VALUE;
    }
    else if(u2_decoded_value == ESCAPE_CODE)
    {
        *pu2_sym_len     = u2_decoded_value & 0x1F;
        *pu2_sym_val = ESC_CODE_VALUE;
    }
    else
    {
        *pu2_sym_len = u2_decoded_value & 0x1F;
        *pu2_sym_val = u2_decoded_value >> 5;
    }

    return(u2_decoded_value);
}

/******************************************************************************
 *
 *  Function Name   : impeg2d_vld_inv_quant_mpeg1
 *
 *  Description     : Performs VLD operation for MPEG1/2
 *
 *  Arguments       :
 *  state           : VLCD state parameter
 *  regs            : Registers of VLCD
 *
 *  Values Returned : None
 ******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_vld_inv_quant_mpeg1(
                             void  *pv_dec,           /* Decoder State */
                             WORD16       *pi2_out_addr,       /*!< Address where decoded symbols will be stored */
                             const UWORD8 *pu1_scan,          /*!< Scan table to be used */
                             UWORD16      u2_intra_flag,      /*!< Intra Macroblock or not */
                             UWORD16      u2_colr_comp,      /*!< 0 - Luma,1 - U comp, 2 - V comp */
                             UWORD16      u2_d_picture        /*!< D Picture or not */
                             )
{
    UWORD8  *pu1_weighting_matrix;
    dec_state_t *ps_dec    = (dec_state_t *) pv_dec;
    IMPEG2D_ERROR_CODES_T e_error   = (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;

    WORD16  pi2_coeffs[NUM_COEFFS];
    UWORD8  pu1_pos[NUM_COEFFS];
    WORD32  i4_num_coeffs;

    /* Perform VLD on the stream to get the coefficients and their positions */
    e_error = impeg2d_vld_decode(ps_dec, pi2_coeffs, pu1_scan, pu1_pos, u2_intra_flag,
                                 u2_colr_comp, u2_d_picture, ps_dec->u2_intra_vlc_format,
                                 ps_dec->u2_is_mpeg2, &i4_num_coeffs);
    if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
    {
        return e_error;
    }

    /* For YUV420 format,Select the weighting matrix according to Table 7.5 */
    pu1_weighting_matrix = (u2_intra_flag == 1) ? ps_dec->au1_intra_quant_matrix:
                    ps_dec->au1_inter_quant_matrix;

    IMPEG2D_IQNT_INP_STATISTICS(pi2_out_addr, ps_dec->u4_non_zero_cols, ps_dec->u4_non_zero_rows);
    /* Inverse Quantize the Output of VLD */
    PROFILE_DISABLE_INVQUANT_IF0

    {
        /* Clear output matrix */
        PROFILE_DISABLE_MEMSET_RESBUF_IF0
        if (1 != (ps_dec->u4_non_zero_cols | ps_dec->u4_non_zero_rows))
        {
            ps_dec->pf_memset_16bit_8x8_linear_block (pi2_out_addr);
        }

        impeg2d_inv_quant_mpeg1(pi2_out_addr, pu1_weighting_matrix,
                                  ps_dec->u1_quant_scale, u2_intra_flag,
                                  i4_num_coeffs, pi2_coeffs, pu1_pos,
                                  pu1_scan, &ps_dec->u2_def_dc_pred[u2_colr_comp],
                                  ps_dec->u2_intra_dc_precision);

        if (0 != pi2_out_addr[0])
        {
            /* The first coeff might've become non-zero due to intra_dc_decision
             * value. So, check here after inverse quantization.
             */
            ps_dec->u4_non_zero_cols  |= 0x1;
            ps_dec->u4_non_zero_rows  |= 0x1;
        }
    }

    return e_error;
}

/******************************************************************************
  *
  *  Function Name   : impeg2d_vld_inv_quant_mpeg2
  *
  *  Description     : Performs VLD operation for MPEG1/2
  *
  *  Arguments       :
  *  state           : VLCD state parameter
  *  regs            : Registers of VLCD
  *
  *  Values Returned : None
  ******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_vld_inv_quant_mpeg2(
                             void  *pv_dec,           /* Decoder State */
                             WORD16       *pi2_out_addr,       /*!< Address where decoded symbols will be stored */
                             const UWORD8 *pu1_scan,          /*!< Scan table to be used */
                             UWORD16      u2_intra_flag,      /*!< Intra Macroblock or not */
                             UWORD16      u2_colr_comp,      /*!< 0 - Luma,1 - U comp, 2 - V comp */
                             UWORD16      u2_d_picture        /*!< D Picture or not */
                             )
{
    UWORD8  *pu1_weighting_matrix;
    WORD32 u4_sum_is_even;
    dec_state_t *ps_dec = (dec_state_t *)pv_dec;
    IMPEG2D_ERROR_CODES_T e_error = (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;

    WORD16  pi2_coeffs[NUM_COEFFS];
    UWORD8  pi4_pos[NUM_COEFFS];
    WORD32  i4_num_coeffs;

    /* Perform VLD on the stream to get the coefficients and their positions */
    e_error = impeg2d_vld_decode(ps_dec, pi2_coeffs, pu1_scan, pi4_pos, u2_intra_flag,
                                 u2_colr_comp, u2_d_picture, ps_dec->u2_intra_vlc_format,
                                 ps_dec->u2_is_mpeg2, &i4_num_coeffs);
    if ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE != e_error)
    {
        return e_error;
    }

    /* For YUV420 format,Select the weighting matrix according to Table 7.5 */
    pu1_weighting_matrix = (u2_intra_flag == 1) ? ps_dec->au1_intra_quant_matrix:
                    ps_dec->au1_inter_quant_matrix;

    /*mismatch control for mpeg2*/
    /* Check if the block has only one non-zero coeff which is DC  */
    ps_dec->i4_last_value_one = 0;

    IMPEG2D_IQNT_INP_STATISTICS(pi2_out_addr, ps_dec->u4_non_zero_cols, ps_dec->u4_non_zero_rows);

    /* Inverse Quantize the Output of VLD */
    PROFILE_DISABLE_INVQUANT_IF0

    {
        /* Clear output matrix */
        PROFILE_DISABLE_MEMSET_RESBUF_IF0
        if (1 != (ps_dec->u4_non_zero_cols | ps_dec->u4_non_zero_rows))
        {
            ps_dec->pf_memset_16bit_8x8_linear_block (pi2_out_addr);
        }

        u4_sum_is_even  = impeg2d_inv_quant_mpeg2(pi2_out_addr, pu1_weighting_matrix,
                                                 ps_dec->u1_quant_scale, u2_intra_flag,
                                                 i4_num_coeffs, pi2_coeffs,
                                                 pi4_pos, pu1_scan,
                                                 &ps_dec->u2_def_dc_pred[u2_colr_comp],
                                                 ps_dec->u2_intra_dc_precision);

        if (0 != pi2_out_addr[0])
        {
            /* The first coeff might've become non-zero due to intra_dc_decision
             * value. So, check here after inverse quantization.
             */
            ps_dec->u4_non_zero_cols  |= 0x1;
            ps_dec->u4_non_zero_rows  |= 0x1;
        }

        if (1 == (ps_dec->u4_non_zero_cols | ps_dec->u4_non_zero_rows))
        {
            ps_dec->i4_last_value_one = 1 - (pi2_out_addr[0] & 1);
        }
        else
        {
            /*toggle last bit if sum is even ,else retain it as it is*/
            pi2_out_addr[63]        ^= (u4_sum_is_even & 1);

            if (0 != pi2_out_addr[63])
            {
                ps_dec->u4_non_zero_cols  |= 0x80;
                ps_dec->u4_non_zero_rows  |= 0x80;
            }
        }
    }

    return e_error;
}


/******************************************************************************
*
*  Function Name   : impeg2d_vld_decode
*
*  Description     : Performs VLD operation for MPEG1/2
*
*  Arguments       :
*  state           : VLCD state parameter
*  regs            : Registers of VLCD
*
*  Values Returned : None
******************************************************************************/
IMPEG2D_ERROR_CODES_T impeg2d_vld_decode(
    dec_state_t *ps_dec,
    WORD16      *pi2_outAddr,       /*!< Address where decoded symbols will be stored */
    const UWORD8 *pu1_scan,         /*!< Scan table to be used */
    UWORD8      *pu1_pos,       /*!< Scan table to be used */
    UWORD16     u2_intra_flag,      /*!< Intra Macroblock or not */
    UWORD16     u2_chroma_flag,     /*!< Chroma Block or not */
    UWORD16     u2_d_picture,       /*!< D Picture or not */
    UWORD16     u2_intra_vlc_format, /*!< Intra VLC format */
    UWORD16     u2_mpeg2,          /*!< MPEG-2 or not */
    WORD32      *pi4_num_coeffs /*!< Returns the number of coeffs in block */
    )
{

    UWORD32 u4_sym_len;

    UWORD32 u4_decoded_value;
    UWORD32 u4_level_first_byte;
    WORD32  u4_level;
    UWORD32 u4_run, u4_numCoeffs;
    UWORD32 u4_buf;
    UWORD32 u4_buf_nxt;
    UWORD32 u4_offset;
    UWORD32 *pu4_buf_aligned;
    UWORD32 u4_bits;
    stream_t *ps_stream = &ps_dec->s_bit_stream;
    WORD32  u4_pos;
    UWORD32 u4_nz_cols;
    UWORD32 u4_nz_rows;

    *pi4_num_coeffs = 0;

    ps_dec->u4_non_zero_cols = 0;
    ps_dec->u4_non_zero_rows = 0;
    u4_nz_cols = ps_dec->u4_non_zero_cols;
    u4_nz_rows = ps_dec->u4_non_zero_rows;

    GET_TEMP_STREAM_DATA(u4_buf,u4_buf_nxt,u4_offset,pu4_buf_aligned,ps_stream)
    /**************************************************************************/
    /* Decode the DC coefficient in case of Intra block                       */
    /**************************************************************************/
    if(u2_intra_flag)
    {
        WORD32 dc_size;
        WORD32 dc_diff;
        WORD32 maxLen;
        WORD32 idx;


        maxLen = MPEG2_DCT_DC_SIZE_LEN;
        idx = 0;
        if(u2_chroma_flag != 0)
        {
            maxLen += 1;
            idx++;
        }


        {
            WORD16  end = 0;
            UWORD32 maxLen_tmp = maxLen;
            UWORD16 m_iBit;


            /* Get the maximum number of bits needed to decode a symbol */
            IBITS_NXT(u4_buf,u4_buf_nxt,u4_offset,u4_bits,maxLen)
            do
            {
                maxLen_tmp--;
                /* Read one bit at a time from the variable to decode the huffman code */
                m_iBit = (UWORD8)((u4_bits >> maxLen_tmp) & 0x1);

                /* Get the next node pointer or the symbol from the tree */
                end = gai2_impeg2d_dct_dc_size[idx][end][m_iBit];
            }while(end > 0);
            dc_size = end + MPEG2_DCT_DC_SIZE_OFFSET;

            /* Flush the appropriate number of bits from the stream */
            FLUSH_BITS(u4_offset,u4_buf,u4_buf_nxt,(maxLen - maxLen_tmp),pu4_buf_aligned)

        }



        if (dc_size != 0)
        {
            UWORD32 u4_bits;

            IBITS_GET(u4_buf,u4_buf_nxt,u4_offset,u4_bits,pu4_buf_aligned, dc_size)
            dc_diff = u4_bits;

            if ((dc_diff & (1 << (dc_size - 1))) == 0) //v Probably the prediction algo?
                dc_diff -= (1 << dc_size) - 1;
        }
        else
        {
            dc_diff = 0;
        }


        pi2_outAddr[*pi4_num_coeffs]    = dc_diff;
        /* This indicates the position of the coefficient. Since this is the DC
         * coefficient, we put the position as 0.
         */
        pu1_pos[*pi4_num_coeffs]    = pu1_scan[0];
        (*pi4_num_coeffs)++;

        if (0 != dc_diff)
        {
            u4_nz_cols |= 0x01;
            u4_nz_rows |= 0x01;
        }

        u4_numCoeffs = 1;
    }
    /**************************************************************************/
    /* Decoding of first AC coefficient in case of non Intra block            */
    /**************************************************************************/
    else
    {
        /* First symbol can be 1s */
        UWORD32 u4_bits;

        IBITS_NXT(u4_buf,u4_buf_nxt,u4_offset,u4_bits,1)

        if(u4_bits == 1)
        {

            FLUSH_BITS(u4_offset,u4_buf,u4_buf_nxt,1, pu4_buf_aligned)
            IBITS_GET(u4_buf,u4_buf_nxt,u4_offset,u4_bits,pu4_buf_aligned, 1)
            if(u4_bits == 1)
            {
                pi2_outAddr[*pi4_num_coeffs] = -1;
            }
            else
            {
                pi2_outAddr[*pi4_num_coeffs] = 1;
            }

            /* This indicates the position of the coefficient. Since this is the DC
             * coefficient, we put the position as 0.
             */
            pu1_pos[*pi4_num_coeffs]    = pu1_scan[0];
            (*pi4_num_coeffs)++;
            u4_numCoeffs = 1;

            u4_nz_cols |= 0x01;
            u4_nz_rows |= 0x01;
        }
        else
        {
            u4_numCoeffs = 0;
        }
    }
    if (1 == u2_d_picture)
    {
        PUT_TEMP_STREAM_DATA(u4_buf, u4_buf_nxt, u4_offset, pu4_buf_aligned, ps_stream)
        ps_dec->u4_non_zero_cols  = u4_nz_cols;
        ps_dec->u4_non_zero_rows  = u4_nz_rows;
        return ((IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE);
    }



        if (1 == u2_intra_vlc_format && u2_intra_flag)
        {

            while(1)
            {
                //Putting the impeg2d_dec_ac_coeff_one function inline.

                UWORD32 lead_zeros;
                WORD16 DecodedValue;

                u4_sym_len = 17;
                IBITS_NXT(u4_buf,u4_buf_nxt,u4_offset,u4_bits,u4_sym_len)

                DecodedValue = gau2_impeg2d_tab_one_1_9[u4_bits >> 8];
                u4_sym_len = (DecodedValue & 0xf);
                u4_level = DecodedValue >> 9;
                /* One table lookup */
                if(0 != u4_level)
                {
                    u4_run = ((DecodedValue >> 4) & 0x1f);
                    u4_numCoeffs       += u4_run;
                    u4_pos             = pu1_scan[u4_numCoeffs++ & 63];
                    pu1_pos[*pi4_num_coeffs]    = u4_pos;

                    FLUSH_BITS(u4_offset,u4_buf,u4_buf_nxt,u4_sym_len,pu4_buf_aligned)
                    pi2_outAddr[*pi4_num_coeffs]    = u4_level;

                    (*pi4_num_coeffs)++;
                }
                else
                {
                    if (DecodedValue == END_OF_BLOCK_ONE)
                    {
                        u4_sym_len = 4;

                        break;
                    }
                    else
                    {
                        /*Second table lookup*/
                        lead_zeros = CLZ(u4_bits) - 20;/* -16 since we are dealing with WORD32 */
                        if (0 != lead_zeros)
                        {

                            u4_bits         = (u4_bits >> (6 - lead_zeros)) & 0x001F;

                            /* Flush the number of bits */
                            if (1 == lead_zeros)
                            {
                                u4_sym_len         = ((u4_bits & 0x18) >> 3) == 2 ? 11:10;
                            }
                            else
                            {
                                u4_sym_len         = 11 + lead_zeros;
                            }
                            /* flushing */
                            FLUSH_BITS(u4_offset,u4_buf,u4_buf_nxt,u4_sym_len,pu4_buf_aligned)

                            /* Calculate the address */
                            u4_bits         = ((lead_zeros - 1) << 5) + u4_bits;

                            DecodedValue    = gau2_impeg2d_tab_one_10_16[u4_bits];

                            u4_run = BITS(DecodedValue, 8,4);
                            u4_level = ((WORD16) DecodedValue) >> 9;

                            u4_numCoeffs       += u4_run;
                            u4_pos             = pu1_scan[u4_numCoeffs++ & 63];
                            pu1_pos[*pi4_num_coeffs]    = u4_pos;
                            pi2_outAddr[*pi4_num_coeffs]    = u4_level;
                            (*pi4_num_coeffs)++;
                        }
                        /*********************************************************************/
                        /* MPEG2 Escape Code                                                 */
                        /*********************************************************************/
                        else if(u2_mpeg2 == 1)
                        {
                            u4_sym_len         = 6;
                            FLUSH_BITS(u4_offset,u4_buf,u4_buf_nxt,u4_sym_len,pu4_buf_aligned)
                                IBITS_GET(u4_buf,u4_buf_nxt,u4_offset,u4_bits,pu4_buf_aligned,18)
                                u4_decoded_value    = u4_bits;
                            u4_run             = (u4_decoded_value >> 12);
                            u4_level           = (u4_decoded_value & 0x0FFF);

                            if (u4_level)
                                u4_level = (u4_level - ((u4_level & 0x0800) << 1));

                            u4_numCoeffs       += u4_run;
                            u4_pos             = pu1_scan[u4_numCoeffs++ & 63];
                            pu1_pos[*pi4_num_coeffs]    = u4_pos;
                            pi2_outAddr[*pi4_num_coeffs]    = u4_level;
                            (*pi4_num_coeffs)++;
                        }
                        /*********************************************************************/
                        /* MPEG1 Escape Code                                                 */
                        /*********************************************************************/
                        else
                        {
                            /*-----------------------------------------------------------
                            * MPEG-1 Stream
                            *
                            * <See D.9.3 of MPEG-2> Run-level escape syntax
                            * Run-level values that cannot be coded with a VLC are coded
                            * by the escape code '0000 01' followed by
                            * either a 14-bit FLC (127 <= level <= 127),
                            * or a 22-bit FLC (255 <= level <= 255).
                            * This is described in Annex B,B.5f of MPEG-1.standard
                            *-----------------------------------------------------------*/

                            /*-----------------------------------------------------------
                            * First 6 bits are the value of the Run. Next is First 8 bits
                            * of Level. These bits decide whether it is 14 bit FLC or
                            * 22-bit FLC.
                            *
                            * If( first 8 bits of Level == '1000000' or '00000000')
                            *      then its is 22-bit FLC.
                            * else
                            *      it is 14-bit FLC.
                            *-----------------------------------------------------------*/
                            u4_sym_len         = 6;
                            FLUSH_BITS(u4_offset,u4_buf,u4_buf_nxt,u4_sym_len,pu4_buf_aligned)
                                IBITS_GET(u4_buf,u4_buf_nxt,u4_offset,u4_bits,pu4_buf_aligned,14)
                                u4_decoded_value     = u4_bits;
                            u4_run              = (u4_decoded_value >> 8);
                            u4_level_first_byte = (u4_decoded_value & 0x0FF);
                            if(u4_level_first_byte & 0x7F)
                            {
                                /*-------------------------------------------------------
                                * First 8 bits of level are neither 1000000 nor 00000000
                                * Hence 14-bit FLC (Last 8 bits are used to get level)
                                *
                                *  Level = (msb of Level_First_Byte is 1)?
                                *          Level_First_Byte - 256 : Level_First_Byte
                                *-------------------------------------------------------*/
                                u4_level = (u4_level_first_byte -
                                    ((u4_level_first_byte & 0x80) << 1));
                            }
                            else
                            {
                                /*-------------------------------------------------------
                                * Next 8 bits are either 1000000 or 00000000
                                * Hence 22-bit FLC (Last 16 bits are used to get level)
                                *
                                *  Level = (msb of Level_First_Byte is 1)?
                                *          Level_Second_Byte - 256 : Level_Second_Byte
                                *-------------------------------------------------------*/
                                IBITS_GET(u4_buf,u4_buf_nxt,u4_offset,u4_bits,pu4_buf_aligned,8)
                                    u4_level = u4_bits;
                                u4_level = (u4_level - (u4_level_first_byte << 1));
                            }
                            u4_numCoeffs += u4_run;

                            u4_pos = pu1_scan[u4_numCoeffs++ & 63];

                            pu1_pos[*pi4_num_coeffs]    = u4_pos;
                            pi2_outAddr[*pi4_num_coeffs]    = u4_level;
                            (*pi4_num_coeffs)++;
                        }
                    }
                }

                u4_nz_cols |= 1 << (u4_pos & 0x7);
                u4_nz_rows |= 1 << (u4_pos >> 0x3);


            }
            IBITS_GET(u4_buf,u4_buf_nxt,u4_offset,u4_bits,pu4_buf_aligned,u4_sym_len)
            if (u4_numCoeffs > 64)
            {
                return IMPEG2D_MB_TEX_DECODE_ERR;
            }
        }
        else
        {
            // Inline
            while(1)
            {

                UWORD32 lead_zeros;
                UWORD16 DecodedValue;

                u4_sym_len = 17;
                IBITS_NXT(u4_buf, u4_buf_nxt, u4_offset, u4_bits, u4_sym_len)


                DecodedValue = gau2_impeg2d_tab_zero_1_9[u4_bits >> 8];
                u4_sym_len = BITS(DecodedValue, 3, 0);
                u4_level = ((WORD16) DecodedValue) >> 9;

                if (0 != u4_level)
                {
                    u4_run = BITS(DecodedValue, 8,4);

                    u4_numCoeffs       += u4_run;

                    u4_pos                 = pu1_scan[u4_numCoeffs++ & 63];
                    pu1_pos[*pi4_num_coeffs]    = u4_pos;

                    FLUSH_BITS(u4_offset,u4_buf,u4_buf_nxt,u4_sym_len,pu4_buf_aligned)
                    pi2_outAddr[*pi4_num_coeffs]    = u4_level;
                    (*pi4_num_coeffs)++;
                }
                else
                {
                    if(DecodedValue == END_OF_BLOCK_ZERO)
                    {
                        u4_sym_len = 2;

                        break;
                    }
                    else
                    {
                        lead_zeros = CLZ(u4_bits) - 20;/* -15 since we are dealing with WORD32 */
                        /*Second table lookup*/
                        if (0 != lead_zeros)
                        {
                            u4_bits         = (u4_bits >> (6 - lead_zeros)) & 0x001F;

                            /* Flush the number of bits */
                            u4_sym_len         = 11 + lead_zeros;

                            /* Calculate the address */
                            u4_bits         = ((lead_zeros - 1) << 5) + u4_bits;

                            DecodedValue    = gau2_impeg2d_tab_zero_10_16[u4_bits];

                            u4_run = BITS(DecodedValue, 8,4);
                            u4_level = ((WORD16) DecodedValue) >> 9;

                            u4_numCoeffs       += u4_run;

                            u4_pos                 = pu1_scan[u4_numCoeffs++ & 63];
                            pu1_pos[*pi4_num_coeffs]    = u4_pos;
                            if (1 == lead_zeros)
                                u4_sym_len--;
                            /* flushing */
                            FLUSH_BITS(u4_offset,u4_buf,u4_buf_nxt,u4_sym_len,pu4_buf_aligned)
                            pi2_outAddr[*pi4_num_coeffs]    = u4_level;

                            (*pi4_num_coeffs)++;
                        }
                        /*Escape Sequence*/
                        else if(u2_mpeg2 == 1)
                        {
                            u4_sym_len         = 6;
                            FLUSH_BITS(u4_offset,u4_buf,u4_buf_nxt,u4_sym_len,pu4_buf_aligned)
                            IBITS_GET(u4_buf,u4_buf_nxt,u4_offset,u4_bits,pu4_buf_aligned,18)
                            u4_decoded_value    = u4_bits;
                            u4_run             = (u4_decoded_value >> 12);
                            u4_level           = (u4_decoded_value & 0x0FFF);

                            if (u4_level)
                                u4_level = (u4_level - ((u4_level & 0x0800) << 1));

                            u4_numCoeffs           += u4_run;

                            u4_pos                 = pu1_scan[u4_numCoeffs++ & 63];
                            pu1_pos[*pi4_num_coeffs]    = u4_pos;
                            pi2_outAddr[*pi4_num_coeffs]    = u4_level;

                            (*pi4_num_coeffs)++;
                        }
                        /*********************************************************************/
                        /* MPEG1 Escape Code                                                 */
                        /*********************************************************************/
                        else
                        {
                            /*-----------------------------------------------------------
                            * MPEG-1 Stream
                            *
                            * <See D.9.3 of MPEG-2> Run-level escape syntax
                            * Run-level values that cannot be coded with a VLC are coded
                            * by the escape code '0000 01' followed by
                            * either a 14-bit FLC (127 <= level <= 127),
                            * or a 22-bit FLC (255 <= level <= 255).
                            * This is described in Annex B,B.5f of MPEG-1.standard
                            *-----------------------------------------------------------*/

                            /*-----------------------------------------------------------
                            * First 6 bits are the value of the Run. Next is First 8 bits
                            * of Level. These bits decide whether it is 14 bit FLC or
                            * 22-bit FLC.
                            *
                            * If( first 8 bits of Level == '1000000' or '00000000')
                            *      then its is 22-bit FLC.
                            * else
                            *      it is 14-bit FLC.
                            *-----------------------------------------------------------*/
                            u4_sym_len             = 6;
                            FLUSH_BITS(u4_offset,u4_buf,u4_buf_nxt,u4_sym_len,pu4_buf_aligned)
                            IBITS_GET(u4_buf,u4_buf_nxt,u4_offset,u4_bits,pu4_buf_aligned,14)
                            u4_decoded_value        = u4_bits;
                            u4_run                 = (u4_decoded_value >> 8);
                            u4_level_first_byte    = (u4_decoded_value & 0x0FF);
                            if(u4_level_first_byte & 0x7F)
                            {
                                /*-------------------------------------------------------
                                * First 8 bits of level are neither 1000000 nor 00000000
                                * Hence 14-bit FLC (Last 8 bits are used to get level)
                                *
                                *  Level = (msb of Level_First_Byte is 1)?
                                *          Level_First_Byte - 256 : Level_First_Byte
                                *-------------------------------------------------------*/
                                u4_level = (u4_level_first_byte -
                                    ((u4_level_first_byte & 0x80) << 1));
                            }
                            else
                            {
                                /*-------------------------------------------------------
                                * Next 8 bits are either 1000000 or 00000000
                                * Hence 22-bit FLC (Last 16 bits are used to get level)
                                *
                                *  Level = (msb of Level_First_Byte is 1)?
                                *          Level_Second_Byte - 256 : Level_Second_Byte
                                *-------------------------------------------------------*/
                                IBITS_GET(u4_buf,u4_buf_nxt,u4_offset,u4_bits,pu4_buf_aligned,8)
                                u4_level = u4_bits;
                                u4_level = (u4_level - (u4_level_first_byte << 1));
                            }
                            u4_numCoeffs           += u4_run;

                            u4_pos                 = pu1_scan[u4_numCoeffs++ & 63];
                            pu1_pos[*pi4_num_coeffs]    = u4_pos;
                            pi2_outAddr[*pi4_num_coeffs]    = u4_level;

                            (*pi4_num_coeffs)++;
                        }
                    }
                }

                u4_nz_cols |= 1 << (u4_pos & 0x7);
                u4_nz_rows |= 1 << (u4_pos >> 0x3);
            }
            if (u4_numCoeffs > 64)
            {
                return IMPEG2D_MB_TEX_DECODE_ERR;
            }

            IBITS_GET(u4_buf,u4_buf_nxt,u4_offset,u4_bits,pu4_buf_aligned,u4_sym_len)

        }

        PUT_TEMP_STREAM_DATA(u4_buf, u4_buf_nxt, u4_offset, pu4_buf_aligned, ps_stream)

        ps_dec->u4_non_zero_cols  = u4_nz_cols;
        ps_dec->u4_non_zero_rows  = u4_nz_rows;

            return (IMPEG2D_ERROR_CODES_T)IVD_ERROR_NONE;
}



/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_inv_quant_mpeg1                                   */
/*                                                                           */
/*  Description   : Inverse quantizes the output of VLD                      */
/*                                                                           */
/*  Inputs        :                                                          */
/*  blk,              - Block to be inverse quantized                        */
/*  weighting_matrix  - Matrix to be used in inverse quant                   */
/*  intra_dc_precision- Precision reqd to scale intra DC value               */
/*  quant_scale       - Quanization scale for inverse quant                  */
/*  intra_flag        - Intra or Not                                         */
/*                                                                           */
/*  Globals       : None                                                     */
/*                                                                           */
/*  Processing    : Implements the inverse quantize equation                 */
/*                                                                           */
/*  Outputs       : Inverse quantized values in the block                    */
/*                                                                           */
/*  Returns       : None                                                     */
/*                                                                           */
/*  Issues        : None                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes                              */
/*         05 09 2005   Harish M        First Version                        */
/*                                                                           */
/*****************************************************************************/
UWORD8 impeg2d_inv_quant_mpeg1(WORD16 *pi2_blk,
                              UWORD8 *pu1_weighting_matrix,
                              UWORD8 u1_quant_scale,
                              WORD32 u4_intra_flag,
                              WORD32 i4_num_coeffs,
                              WORD16 *pi2_coeffs,
                              UWORD8 *pu1_pos,
                              const UWORD8 *pu1_scan,
                              UWORD16 *pu2_def_dc_pred,
                              UWORD16 u2_intra_dc_precision)
{
    UWORD16 i4_pos;

    WORD32  i4_iter;

    /* Inverse Quantize the predicted DC value for intra MB*/
    if(u4_intra_flag == 1)
    {
        /**************************************************************************/
        /* Decode the DC coefficient in case of Intra block and also update       */
        /* DC predictor value of the corresponding color component                */
        /**************************************************************************/
        {
            pi2_coeffs[0]   += *pu2_def_dc_pred;
            *pu2_def_dc_pred      = pi2_coeffs[0];
            pi2_coeffs[0]   <<= (3 - u2_intra_dc_precision);
            pi2_coeffs[0]   = CLIP_S12(pi2_coeffs[0]);
        }

        pi2_blk[pu1_scan[0]]  = pi2_coeffs[0];
    }
    /************************************************************************/
    /* Inverse quantization of other DCT coefficients                       */
    /************************************************************************/
    for(i4_iter = u4_intra_flag; i4_iter < i4_num_coeffs; i4_iter++)
    {

        WORD16 sign;
        WORD32 temp, temp1;

        /* Position is the inverse scan of the index stored */
        i4_pos      = pu1_pos[i4_iter];
        pi2_blk[i4_pos] = pi2_coeffs[i4_iter];

        sign = SIGN(pi2_blk[i4_pos]);
        temp = ABS(pi2_blk[i4_pos] << 1);

        /* pi2_coeffs has only non-zero elements. So no need to check
         * if the coeff is non-zero.
         */
        temp = temp + (1 * !u4_intra_flag);

        temp = temp * pu1_weighting_matrix[i4_pos] * u1_quant_scale;

        temp = temp >> 5;

        temp1 = temp | 1;

        temp1 = (temp1 > temp) ? (temp1 - temp) : (temp - temp1);

        temp = temp - temp1;

        if(temp < 0)
        {
            temp = 0;
        }

        temp = temp * sign;

        temp = CLIP_S12(temp);

        pi2_blk[i4_pos] = temp;
    }

    /*return value is used in the case of mpeg2 for mismatch control*/
    return  (0);
} /* End of inv_quant() */



/*****************************************************************************/
/*                                                                           */
/*  Function Name : impeg2d_inv_quant_mpeg2                                   */
/*                                                                           */
/*  Description   : Inverse quantizes the output of VLD                      */
/*                                                                           */
/*  Inputs        :                                                          */
/*  blk,              - Block to be inverse quantized                        */
/*  weighting_matrix  - Matrix to be used in inverse quant                   */
/*  intra_dc_precision- Precision reqd to scale intra DC value               */
/*  quant_scale       - Quanization scale for inverse quant                  */
/*  intra_flag        - Intra or Not                                         */
/*                                                                           */
/*  Globals       : None                                                     */
/*                                                                           */
/*  Processing    : Implements the inverse quantize equation                 */
/*                                                                           */
/*  Outputs       : Inverse quantized values in the block                    */
/*                                                                           */
/*  Returns       : None                                                     */
/*                                                                           */
/*  Issues        : None                                                     */
/*                                                                           */
/*  Revision History:                                                        */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes                              */
/*         05 09 2005   Harish M        First Version                        */
/*                                                                           */
/*****************************************************************************/
UWORD8 impeg2d_inv_quant_mpeg2(WORD16 *pi2_blk,
                              UWORD8 *pu1_weighting_matrix,
                              UWORD8 u1_quant_scale,
                              WORD32 u4_intra_flag,
                              WORD32 i4_num_coeffs,
                              WORD16 *pi2_coeffs,
                              UWORD8 *pu1_pos,
                              const UWORD8 *pu1_scan,
                              UWORD16 *pu2_def_dc_pred,
                              UWORD16 u2_intra_dc_precision)
{

    WORD32  i4_pos;
    /* Used for Mismatch control */
    UWORD32 sum;

    WORD32  i4_iter;

    sum = 0;

    /* Inverse Quantize the predicted DC value for intra MB*/
    if(u4_intra_flag == 1)
    {
        /**************************************************************************/
        /* Decode the DC coefficient in case of Intra block and also update       */
        /* DC predictor value of the corresponding color component                */
        /**************************************************************************/
        {
            pi2_coeffs[0]   += *pu2_def_dc_pred;
            *pu2_def_dc_pred      = pi2_coeffs[0];
            pi2_coeffs[0]   <<= (3 - u2_intra_dc_precision);
            pi2_coeffs[0]   = CLIP_S12(pi2_coeffs[0]);
        }

        pi2_blk[pu1_scan[0]]  = pi2_coeffs[0];
        sum = pi2_blk[0];
    }

    /************************************************************************/
    /* Inverse quantization of other DCT coefficients                       */
    /************************************************************************/
    for(i4_iter = u4_intra_flag; i4_iter < i4_num_coeffs; i4_iter++)
    {
        WORD16 sign;
        WORD32 temp;
        /* Position is the inverse scan of the index stored */
        i4_pos      = pu1_pos[i4_iter];
        pi2_blk[i4_pos] = pi2_coeffs[i4_iter];

        sign = SIGN(pi2_blk[i4_pos]);
        temp = ABS(pi2_blk[i4_pos] << 1);
        temp = temp + (1 * !u4_intra_flag);
        temp = temp * pu1_weighting_matrix[i4_pos] * u1_quant_scale;

        temp = temp >> 5;

        temp = temp * sign;

        temp = CLIP_S12(temp);

        pi2_blk[i4_pos] = temp;

        sum += temp;
    }
    return (sum ^ 1);
} /* End of inv_quant() */
