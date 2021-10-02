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
/*  File Name         : impeg2d_bitstream.c                                  */
/*                                                                           */
/*  Description       : This file contains all the necessary examples to     */
/*                      establish a consistent use of Ittiam C coding        */
/*                      standards (based on Indian Hill C Standards)         */
/*                                                                           */
/*  List of Functions : <List the functions defined in this file>            */
/*                                                                           */
/*  Issues / Problems : None                                                 */
/*                                                                           */
/*  Revision History  :                                                      */
/*                                                                           */
/*         DD MM YYYY   Author(s)       Changes (Describe the changes made)  */
/*         10 01 2005   Ittiam          Draft                                */
/*                                                                           */
/*****************************************************************************/
#include <stdlib.h>

#include "iv_datatypedef.h"
#include "impeg2_defs.h"
#include "impeg2_platform_macros.h"
#include "impeg2_macros.h"
#include "impeg2d_bitstream.h"

#define BIT(val,bit)      (UWORD16)(((val) >> (bit)) & 0x1)
/******************************************************************************
*
*  Function Name    : impeg2d_bit_stream_init
*
*  Description      : This is a Bitstream initialising function.
*  Arguments        :
*  stream           : Pointer to the Bitstream.
*  byteBuf          : Address of the buffer
*  size             : Size of the buffer in bytes
*
*  Values Returned  : None
*******************************************************************************/
void impeg2d_bit_stream_init(stream_t *ps_stream,
                             UWORD8 *pu1_byte_buf,
                             UWORD32 u4_max_offset)
{
    UWORD8      *pu1_byte_buff;
    UWORD32     *pu4_word_buf;
    size_t     u4_byte_addr;
    UWORD32     u4_temp1,u4_temp2;

    /* Set parameters of the stream structure.Associate the structure with
       the file */
    ps_stream->pv_bs_buf           = pu1_byte_buf;
    ps_stream->u4_offset              = 0;

    /* Take care of unaligned address and create
       nearest greater aligned address */
    pu1_byte_buff               = (UWORD8 *)pu1_byte_buf;
    u4_byte_addr                = (size_t)pu1_byte_buff;

    if((u4_byte_addr & 3) == 1)
    {
        u4_temp1                = ((UWORD32)(*pu1_byte_buff++)) << 8;
        u4_temp1                += ((UWORD32)(*pu1_byte_buff++)) << 16;
        u4_temp1                += ((UWORD32)(*pu1_byte_buff++)) << 24;

        pu4_word_buf            = (UWORD32 *)pu1_byte_buff;

        ps_stream->u4_offset          = 8;
    }
    else if((u4_byte_addr & 3) == 2)
    {
        u4_temp1                = ((UWORD32)(*pu1_byte_buff++)) << 16;
        u4_temp1                += ((UWORD32)(*pu1_byte_buff++)) << 24;

        pu4_word_buf            = (UWORD32 *)pu1_byte_buff;

        ps_stream->u4_offset          = 16;
    }
    else if((u4_byte_addr & 3) == 3)
    {
        u4_temp1                = (((UWORD32)(*pu1_byte_buff++)) << 24);

        pu4_word_buf            = (UWORD32 *)pu1_byte_buff;

        ps_stream->u4_offset          = 24;
    }
    else
    {
        pu4_word_buf            = (UWORD32 *)pu1_byte_buff;

        u4_temp1                = *pu4_word_buf++;
        ps_stream->u4_offset          = 0;
    }

    /* convert the endian ness from Little endian to Big endian so that bits
       are in proper order from MSB to LSB */
    CONV_LE_TO_BE(u4_temp2,u4_temp1)

    /* Read One more word for buf nxt */
    u4_temp1                    = *pu4_word_buf++;
    ps_stream->u4_buf              = u4_temp2;

    CONV_LE_TO_BE(u4_temp2,u4_temp1)

    ps_stream->u4_buf_nxt          = u4_temp2;

    ps_stream->pu4_buf_aligned      = pu4_word_buf;


    ps_stream->u4_max_offset        = (u4_max_offset << 3) + ps_stream->u4_offset;

    return;
}



/******************************************************************************
*
*  Function Name    : impeg2d_bit_stream_get_bit
*
*  Description      : This is a Bitstream processing function. It reads the
*                     bit currently pointed by the bit pointer in the buffer and
*                     advances the pointer by one.
*  Arguments        :
*  stream           : Pointer to the Bitstream.
*
*  Values Returned  : The bit read(0/1)
*******************************************************************************/
INLINE UWORD8 impeg2d_bit_stream_get_bit(stream_t *ps_stream)
{
    UWORD32     u4_bit,u4_offset,u4_temp;
    UWORD32     u4_curr_bit;

    u4_offset               = ps_stream->u4_offset;
    u4_curr_bit             = u4_offset & 0x1F;
    u4_bit                  = ps_stream->u4_buf;

    /* Move the current bit read from the current word to the
       least significant bit positions of 'c'.*/
    u4_bit                  >>= BITS_IN_INT - u4_curr_bit - 1;

    u4_offset++;

    /* If the last bit of the last word of the buffer has been read update
       the currrent buf with next, and read next buf from bit stream buffer */
    if (u4_curr_bit == 31)
    {
        ps_stream->u4_buf      = ps_stream->u4_buf_nxt;
        u4_temp             = *(ps_stream->pu4_buf_aligned)++;

        CONV_LE_TO_BE(ps_stream->u4_buf_nxt,u4_temp)
    }
    ps_stream->u4_offset          = u4_offset;

    return (u4_bit & 0x1);
}
/******************************************************************************
*
*  Function Name    : impeg2d_bit_stream_flush
*
*  Description      : This is a Bitstream processing function. It
*                     advances the bit and byte pointers appropriately
*
*  Arguments        :
*  ctxt             : Pointer to the Bitstream.
*  numBits          : No of bits to be read
*
*  Values Returned  : None
*******************************************************************************/
INLINE void impeg2d_bit_stream_flush(void* pv_ctxt, UWORD32 u4_no_of_bits)
{
    stream_t *ps_stream = (stream_t *)pv_ctxt;

    FLUSH_BITS(ps_stream->u4_offset,ps_stream->u4_buf,ps_stream->u4_buf_nxt,u4_no_of_bits,ps_stream->pu4_buf_aligned)
    return;
}
/******************************************************************************
*
*  Function Name    : impeg2d_bit_stream_flush_to_byte_boundary
*
*  Description      : This is a Bitstream processing function.It advances
*                     the bit and byte pointers to next byte boundary
*
*  Arguments        :
*  stream           : Pointer to the Bitstream.
*  NoOfBits         : No of bits to be read
*
*  Values Returned  : The bits read (upto 32 bits maximum) starting from the
*                     least significant bit and going towards most significant
*                     bit in the order of their occurence.
*******************************************************************************/
INLINE void impeg2d_bit_stream_flush_to_byte_boundary(void* pv_ctxt)
{
    UWORD8 u1_bit_offset;
    stream_t *ps_stream = (stream_t *)pv_ctxt;

    u1_bit_offset = (ps_stream->u4_offset) & 0x7;


    /* if it is not byte aligned make it byte aligned*/
    if(u1_bit_offset != 0)
    {
        impeg2d_bit_stream_flush(ps_stream,(8 - u1_bit_offset));
    }



}


/******************************************************************************
*
*  Function Name    : ibits_next
*
*  Description      : This is a Bitstream processing function.It gets the
*                     specified number of bits from the buffer without
*                     altering the current pointers. It is used mainly to
*                     check for some specific pattern of bits like start
*                     code. This is equivalent to next_bits() function
*                     defined in MPEG-4 Visual Standard Definition of functions
*
*  Arguments        :
*  ctxt             : Pointer to the Bitstream.
*  numBits          : No of bits to be read
*
*  Values Returned  : The bits read (upto 32 bits maximum) starting from the
*                     least significant bit and going towards most significant
*                     bit in the order of their occurence.
*******************************************************************************/
INLINE UWORD32 impeg2d_bit_stream_nxt( stream_t  *ps_stream, WORD32 i4_no_of_bits)
{
    UWORD32     u4_bits,u4_offset,u4_temp;
    UWORD8      u4_bit_ptr;

    ASSERT(i4_no_of_bits > 0);

    u4_offset               = ps_stream->u4_offset;
    u4_bit_ptr              = u4_offset & 0x1F;
    u4_bits                 = ps_stream->u4_buf << u4_bit_ptr;

    u4_bit_ptr              += i4_no_of_bits;
    if(32 < u4_bit_ptr)
    {
        /*  Read bits from the next word if necessary */
        u4_temp             = ps_stream->u4_buf_nxt;
        u4_bit_ptr          &= (BITS_IN_INT - 1);

        u4_temp             = (u4_temp >> (BITS_IN_INT - u4_bit_ptr));

        /* u4_temp consists of bits,if any that had to be read from the next word
           of the buffer.The bits read from both the words are concatenated and
           moved to the least significant positions of 'u4_bits'*/
        u4_bits = (u4_bits >> (32 - i4_no_of_bits)) | u4_temp;
    }
    else
    {
        u4_bits = (u4_bits >> (32 - i4_no_of_bits));
    }

    return (u4_bits);
}
/******************************************************************************
*
*  Function Name    : impeg2d_bit_stream_get
*
*  Description      : This is a Bitstream processing function. It reads a
*                     specified number of bits from the current bit
*                     position and advances the bit and byte pointers
*                     appropriately
*  Arguments        :
*  ctxt             : Pointer to the Bitstream.
*  numBits          : No of bits to be read
*
*  Values Returned  : The bits read (upto 32 bits maximum) starting from the
*                     least significant bit and going towards most significant
*                     bit in the order of their occurence.
*******************************************************************************/

INLINE UWORD32 impeg2d_bit_stream_get(void* pv_ctxt, UWORD32 u4_num_bits)
{
    UWORD32 u4_next_bits = impeg2d_bit_stream_nxt(pv_ctxt, u4_num_bits);
    impeg2d_bit_stream_flush(pv_ctxt, u4_num_bits);
    return(u4_next_bits);
}



/******************************************************************************
*
*  Function Name    : impeg2d_bit_stream_num_bits_read
*
*  Description      : This is a Bitstream processing function. It reads a
*                     specified number of bits from the current bit
*                     position and advances the bit and byte pointers
*                     appropriately
*  Arguments        :
*  ctxt             : Pointer to the Bitstream.
*  numBits          : No of bits to be read
*
*  Values Returned  : The bits read (upto 16 bits maximum) starting from the
*                     least significant bit and going towards most significant
*                     bit in the order of their occurence.
*******************************************************************************/
INLINE UWORD32 impeg2d_bit_stream_num_bits_read(void* pv_ctxt)
{
    stream_t *u4_no_of_bitsstream = (stream_t *)pv_ctxt;
    size_t     u4_temp;
    UWORD32     u4_bits_read;
    u4_temp         = (size_t)(u4_no_of_bitsstream->pv_bs_buf);
    u4_temp         &= 0x3;
    u4_bits_read         = (u4_no_of_bitsstream->u4_offset - (u4_temp << 3));

    return(u4_bits_read);

}


