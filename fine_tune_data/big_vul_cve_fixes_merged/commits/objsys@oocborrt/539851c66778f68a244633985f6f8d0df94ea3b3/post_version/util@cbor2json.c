/* CBOR-to-JSON translation utility */

#include "rtjsonsrc/osrtjson.h"
#include "rtcborsrc/osrtcbor.h"
#include "rtxsrc/rtxCharStr.h"
#include "rtxsrc/rtxContext.h"
#include "rtxsrc/rtxFile.h"
#include "rtxsrc/rtxHexDump.h"

#include <stdio.h>

#ifndef _NO_INT64_SUPPORT
#define OSUINTTYPE OSUINT64
#define OSINTTYPE OSINT64
#define rtCborDecUInt rtCborDecUInt64
#define rtCborDecInt  rtCborDecInt64
#else
#define OSUINTTYPE OSUINT32
#define OSINTTYPE OSINT32
#define rtCborDecUInt rtCborDecUInt32
#define rtCborDecInt  rtCborDecInt32
#endif

static int cborTagNotSupp (OSCTXT* pctxt, OSOCTET tag)
{
   char numbuf[10];
   char errtext[80];

   rtxUIntToCharStr (tag, numbuf, sizeof(numbuf), 0);
   rtxStrJoin (errtext, sizeof(errtext), "CBOR tag ", numbuf, 0, 0, 0);
   rtxErrAddStrParm (pctxt, errtext);

   return RTERR_NOTSUPP;
}

static int cborElemNameToJson (OSCTXT* pCborCtxt, OSCTXT* pJsonCtxt)
{
   char* pElemName = 0;
   OSOCTET ub;
   int ret;

   /* Read byte from stream */
   ret = rtxReadBytes (pCborCtxt, &ub, 1);
   if (0 != ret) return LOG_RTERR (pCborCtxt, ret);

   /* Decode element name (note: only string type is currently supported) */
   ret = rtCborDecDynUTF8Str (pCborCtxt, ub, &pElemName);
   if (0 != ret) return LOG_RTERR (pCborCtxt, ret);

   /* Encode map element name as string */
   ret = rtJsonEncStringValue (pJsonCtxt, (const OSUTF8CHAR*)pElemName);
   rtxMemFreePtr (pCborCtxt, pElemName);
   if (0 != ret) return LOG_RTERR (pJsonCtxt, ret);

   OSRTSAFEPUTCHAR (pJsonCtxt, ':');

   return 0;
}

static int cbor2json (OSCTXT* pCborCtxt, OSCTXT* pJsonCtxt)
{
   int ret = 0;
   OSOCTET tag, ub;

   /* Read byte from stream */
   ret = rtxReadBytes (pCborCtxt, &ub, 1);
   if (0 != ret) return LOG_RTERR (pCborCtxt, ret);
   tag = ub >> 5;

   /* Switch on tag value */
   switch (tag) {
   case OSRTCBOR_UINT: {
      OSUINTTYPE value;
      ret = rtCborDecUInt (pCborCtxt, ub, &value);
      if (0 != ret) return LOG_RTERR (pCborCtxt, ret);

      /* Encode JSON */
#ifndef _NO_INT64_SUPPORT
      ret = rtJsonEncUInt64Value (pJsonCtxt, value);
#else
      ret = rtJsonEncUIntValue (pJsonCtxt, value);
#endif
      if (0 != ret) return LOG_RTERR (pJsonCtxt, ret);
      break;
   }
   case OSRTCBOR_NEGINT: {
      OSINTTYPE value;
      ret = rtCborDecInt (pCborCtxt, ub, &value);
      if (0 != ret) return LOG_RTERR (pCborCtxt, ret);

      /* Encode JSON */
#ifndef _NO_INT64_SUPPORT
      ret = rtJsonEncInt64Value (pJsonCtxt, value);
#else
      ret = rtJsonEncIntValue (pJsonCtxt, value);
#endif
      if (0 != ret) return LOG_RTERR (pJsonCtxt, ret);
      break;
   }
   case OSRTCBOR_BYTESTR: {
      OSDynOctStr64 byteStr;
      ret = rtCborDecDynByteStr (pCborCtxt, ub, &byteStr);
      if (0 != ret) return LOG_RTERR (pCborCtxt, ret);

      /* Encode JSON */
      ret = rtJsonEncHexStr (pJsonCtxt, byteStr.numocts, byteStr.data);
      rtxMemFreePtr (pCborCtxt, byteStr.data);
      if (0 != ret) return LOG_RTERR (pJsonCtxt, ret);

      break;
   }
   case OSRTCBOR_UTF8STR: {
      OSUTF8CHAR* utf8str;
      ret = rtCborDecDynUTF8Str (pCborCtxt, ub, (char**)&utf8str);
      if (0 != ret) return LOG_RTERR (pCborCtxt, ret);

      ret = rtJsonEncStringValue (pJsonCtxt, utf8str);
      rtxMemFreePtr (pCborCtxt, utf8str);
      if (0 != ret) return LOG_RTERR (pJsonCtxt, ret);

      break;
   }
   case OSRTCBOR_ARRAY: 
   case OSRTCBOR_MAP: {
      OSOCTET len = ub & 0x1F;
      char startChar = (tag == OSRTCBOR_ARRAY) ? '[' : '{';
      char endChar = (tag == OSRTCBOR_ARRAY) ? ']' : '}';

      OSRTSAFEPUTCHAR (pJsonCtxt, startChar);

      if (len == OSRTCBOR_INDEF) {
         OSBOOL first = TRUE;
         for (;;) {
            if (OSRTCBOR_MATCHEOC (pCborCtxt)) {
               pCborCtxt->buffer.byteIndex++;
               break;
            }

            if (!first) 
               OSRTSAFEPUTCHAR (pJsonCtxt, ',');
            else
               first = FALSE;

            /* If map, decode object name */
            if (tag == OSRTCBOR_MAP) {
               ret = cborElemNameToJson (pCborCtxt, pJsonCtxt);
            }

            /* Make recursive call */
            if (0 == ret)
               ret = cbor2json (pCborCtxt, pJsonCtxt);
            if (0 != ret) {
               OSCTXT* pctxt = 
                  (rtxErrGetErrorCnt(pJsonCtxt) > 0) ? pJsonCtxt : pCborCtxt;
               return LOG_RTERR (pctxt, ret);
            }
         }
      }
      else { /* definite length */
         OSSIZE nitems;

         /* Decode tag and number of items */
         ret = rtCborDecSize (pCborCtxt, len, &nitems);
         if (0 == ret) {
            OSSIZE i;

            /* Loop to decode array items */
            for (i = 0; i < nitems; i++) {
               if (0 != i) OSRTSAFEPUTCHAR (pJsonCtxt, ',');

               /* If map, decode object name */
               if (tag == OSRTCBOR_MAP) {
                  ret = cborElemNameToJson (pCborCtxt, pJsonCtxt);
               }

               /* Make recursive call */
               if (0 == ret)
                  ret = cbor2json (pCborCtxt, pJsonCtxt);
               if (0 != ret) {
                  OSCTXT* pctxt = 
                  (rtxErrGetErrorCnt(pJsonCtxt) > 0) ? pJsonCtxt : pCborCtxt;
                  return LOG_RTERR (pctxt, ret);
               }
            }
         }
      }
      OSRTSAFEPUTCHAR (pJsonCtxt, endChar);
      break;
   }

   case OSRTCBOR_FLOAT:
      if (tag == OSRTCBOR_FALSEENC || tag == OSRTCBOR_TRUEENC) {
         OSBOOL boolval = (ub == OSRTCBOR_TRUEENC) ? TRUE : FALSE;
         ret = rtJsonEncBoolValue (pJsonCtxt, boolval);
         if (0 != ret) return LOG_RTERR (pJsonCtxt, ret);
      }
      else if (tag == OSRTCBOR_FLT16ENC ||
               tag == OSRTCBOR_FLT32ENC ||
               tag == OSRTCBOR_FLT64ENC) {
         OSDOUBLE fltval;
         ret = rtCborDecFloat (pCborCtxt, ub, &fltval);
         if (0 != ret) return LOG_RTERR (pCborCtxt, ret);

         /* Encode JSON */
         ret = rtJsonEncDoubleValue (pJsonCtxt, fltval, 0);
         if (0 != ret) return LOG_RTERR (pJsonCtxt, ret);
      }
      else {
         ret = cborTagNotSupp (pCborCtxt, tag);
      }
      break;

   default:
      ret = cborTagNotSupp (pCborCtxt, tag);
   }

   return ret;
}

int main (int argc, char** argv)
{
   OSCTXT      jsonCtxt, cborCtxt;
   OSOCTET*    pMsgBuf = 0;
   size_t      msglen;
   OSBOOL      verbose = FALSE;
   const char* filename = "message.cbor";
   const char* outfname = "message.json";
   int         ret;

   /* Process command line arguments */
   if (argc > 1) {
      int i;
      for (i = 1; i < argc; i++) {
         if (!strcmp (argv[i], "-v")) verbose = TRUE;
         else if (!strcmp (argv[i], "-i")) filename = argv[++i];
         else if (!strcmp (argv[i], "-o")) outfname = argv[++i];
         else {
            printf ("usage: cbor2json [-v] [-i <filename>] [-o filename]\n");
            printf ("   -v  verbose mode: print trace info\n");
            printf ("   -i <filename>  read CBOR msg from <filename>\n");
            printf ("   -o <filename>  write JSON data to <filename>\n");
            return 1;
         }
      }
   }

   /* Initialize context structures */
   ret = rtxInitContext (&jsonCtxt);
   if (ret != 0) {
      rtxErrPrint (&jsonCtxt);
      return ret;
   }
   rtxErrInit();
   /* rtxSetDiag (&jsonCtxt, verbose); */

   ret = rtxInitContext (&cborCtxt);
   if (ret != 0) {
      rtxErrPrint (&cborCtxt);
      return ret;
   }
   /* rtxSetDiag (&cborCtxt, verbose); */

   /* Create file input stream */
#if 0
   /* Streaming not supported in open source version
   ret = rtxStreamFileCreateReader (&jsonCtxt, filename);
   */
#else
   /* Read input file into memory buffer */
   ret = rtxFileReadBinary (&cborCtxt, filename, &pMsgBuf, &msglen);
   if (0 == ret) {
      ret = rtxInitContextBuffer (&cborCtxt, pMsgBuf, msglen);
   }
#endif
   if (0 != ret) {
      rtxErrPrint (&jsonCtxt);
      rtxFreeContext (&jsonCtxt);
      rtxFreeContext (&cborCtxt);
      return ret;
   }

   /* Init JSON output buffer */
   ret = rtxInitContextBuffer (&jsonCtxt, 0, 0);
   if (0 != ret) {
      rtxErrPrint (&jsonCtxt);
      rtxFreeContext (&jsonCtxt);
      rtxFreeContext (&cborCtxt);
      return ret;
   }

   /* Invoke the translation function */
   ret = cbor2json (&cborCtxt, &jsonCtxt);

   if (0 == ret && cborCtxt.level != 0) 
      ret = LOG_RTERR (&cborCtxt, RTERR_UNBAL);

   if (0 == ret && 0 != outfname) {
      /* Write encoded JSON data to output file */
      OSRTSAFEPUTCHAR (&jsonCtxt, '\0');  /* null terminate buffer */
      int fileret = rtxFileWriteText 
         (outfname, (const char*)jsonCtxt.buffer.data);

      if (0 != fileret) {
         printf ("unable to write message data to '%s', status = %d\n", 
                 outfname, fileret);
      }
   }

   if (0 != ret) {
      rtxErrPrint (&jsonCtxt);
      rtxErrPrint (&cborCtxt);
   }

   rtxFreeContext (&jsonCtxt);
   rtxFreeContext (&cborCtxt);

   return ret;
}
