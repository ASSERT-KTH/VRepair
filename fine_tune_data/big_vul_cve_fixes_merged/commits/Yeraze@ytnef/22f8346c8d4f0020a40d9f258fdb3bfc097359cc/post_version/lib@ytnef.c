/*
*    Yerase's TNEF Stream Reader Library
*    Copyright (C) 2003  Randall E. Hand
*
*    This program is free software; you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation; either version 2 of the License, or
*    (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software
*    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*
*    You can contact me at randall.hand@gmail.com for questions or assistance
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include "ytnef.h"
#include "tnef-errors.h"
#include "mapi.h"
#include "mapidefs.h"
#include "mapitags.h"
#include "config.h"

#define RTF_PREBUF "{\\rtf1\\ansi\\mac\\deff0\\deftab720{\\fonttbl;}{\\f0\\fnil \\froman \\fswiss \\fmodern \\fscript \\fdecor MS Sans SerifSymbolArialTimes New RomanCourier{\\colortbl\\red0\\green0\\blue0\n\r\\par \\pard\\plain\\f0\\fs20\\b\\i\\u\\tab\\tx"
#define DEBUG(lvl, curlvl, msg) \
        if ((lvl) >= (curlvl)) \
            printf("DEBUG(%i/%i): %s\n", curlvl, lvl,  msg);
#define DEBUG1(lvl, curlvl, msg, var1) \
        if ((lvl) >= (curlvl)) { \
            printf("DEBUG(%i/%i):", curlvl, lvl); \
            printf(msg, var1); \
            printf("\n"); \
        }
#define DEBUG2(lvl, curlvl, msg, var1, var2) \
        if ((lvl) >= (curlvl)) { \
            printf("DEBUG(%i/%i):", curlvl, lvl); \
            printf(msg, var1, var2); \
            printf("\n"); \
        }
#define DEBUG3(lvl, curlvl, msg, var1, var2, var3) \
        if ((lvl) >= (curlvl)) { \
            printf("DEBUG(%i/%i):", curlvl, lvl); \
            printf(msg, var1, var2,var3); \
            printf("\n"); \
        }

#define MIN(x,y) (((x)<(y))?(x):(y))

#define ALLOCCHECK(x) { if(!x) { printf("Out of Memory at %s : %i\n", __FILE__, __LINE__); return(-1); } }
#define ALLOCCHECK_CHAR(x) { if(!x) { printf("Out of Memory at %s : %i\n", __FILE__, __LINE__); return(NULL); } }
#define SIZECHECK(x) { if ((((char *)d - (char *)data) + x) > size) {  printf("Corrupted file detected at %s : %i\n", __FILE__, __LINE__); return(-1); } }

int TNEFFillMapi(TNEFStruct *TNEF, BYTE *data, DWORD size, MAPIProps *p);
void SetFlip(void);

int TNEFDefaultHandler STD_ARGLIST;
int TNEFAttachmentFilename STD_ARGLIST;
int TNEFAttachmentSave STD_ARGLIST;
int TNEFDetailedPrint STD_ARGLIST;
int TNEFHexBreakdown STD_ARGLIST;
int TNEFBody STD_ARGLIST;
int TNEFRendData STD_ARGLIST;
int TNEFDateHandler STD_ARGLIST;
int TNEFPriority  STD_ARGLIST;
int TNEFVersion  STD_ARGLIST;
int TNEFMapiProperties STD_ARGLIST;
int TNEFIcon STD_ARGLIST;
int TNEFSubjectHandler STD_ARGLIST;
int TNEFFromHandler STD_ARGLIST;
int TNEFRecipTable STD_ARGLIST;
int TNEFAttachmentMAPI STD_ARGLIST;
int TNEFSentFor STD_ARGLIST;
int TNEFMessageClass STD_ARGLIST;
int TNEFMessageID STD_ARGLIST;
int TNEFParentID STD_ARGLIST;
int TNEFOriginalMsgClass STD_ARGLIST;
int TNEFCodePage STD_ARGLIST;


BYTE *TNEFFileContents = NULL;
DWORD TNEFFileContentsSize;
BYTE *TNEFFileIcon = NULL;
DWORD TNEFFileIconSize;

int IsCompressedRTF(variableLength *p);

TNEFHandler TNEFList[] = {
  {attNull,                    "Null",                        TNEFDefaultHandler},
  {attFrom,                    "From",                        TNEFFromHandler},
  {attSubject,                 "Subject",                     TNEFSubjectHandler},
  {attDateSent,                "Date Sent",                   TNEFDateHandler},
  {attDateRecd,                "Date Received",               TNEFDateHandler},
  {attMessageStatus,           "Message Status",              TNEFDefaultHandler},
  {attMessageClass,            "Message Class",               TNEFMessageClass},
  {attMessageID,               "Message ID",                  TNEFMessageID},
  {attParentID,                "Parent ID",                   TNEFParentID},
  {attConversationID,          "Conversation ID",             TNEFDefaultHandler},
  {attBody,                    "Body",                        TNEFBody},
  {attPriority,                "Priority",                    TNEFPriority},
  {attAttachData,              "Attach Data",                 TNEFAttachmentSave},
  {attAttachTitle,             "Attach Title",                TNEFAttachmentFilename},
  {attAttachMetaFile,          "Attach Meta-File",            TNEFIcon},
  {attAttachCreateDate,        "Attachment Create Date",      TNEFDateHandler},
  {attAttachModifyDate,        "Attachment Modify Date",      TNEFDateHandler},
  {attDateModified,            "Date Modified",               TNEFDateHandler},
  {attAttachTransportFilename, "Attachment Transport name",   TNEFDefaultHandler},
  {attAttachRenddata,          "Attachment Display info",     TNEFRendData},
  {attMAPIProps,               "MAPI Properties",             TNEFMapiProperties},
  {attRecipTable,              "Recip Table",                 TNEFRecipTable},
  {attAttachment,              "Attachment",                  TNEFAttachmentMAPI},
  {attTnefVersion,             "TNEF Version",                TNEFVersion},
  {attOemCodepage,             "OEM CodePage",                TNEFCodePage},
  {attOriginalMessageClass,    "Original Message Class",      TNEFOriginalMsgClass},
  {attOwner,                   "Owner",                       TNEFDefaultHandler},
  {attSentFor,                 "Sent For",                    TNEFSentFor},
  {attDelegate,                "Delegate",                    TNEFDefaultHandler},
  {attDateStart,               "Date Start",                  TNEFDateHandler},
  {attDateEnd,                 "Date End",                    TNEFDateHandler},
  {attAidOwner,                "Aid Owner",                   TNEFDefaultHandler},
  {attRequestRes,              "Request Response",            TNEFDefaultHandler}
};


WORD SwapWord(BYTE *p, int size) {
  union BYTES2WORD
  {
      WORD word;
      BYTE bytes[sizeof(WORD)];
  };
  
  union BYTES2WORD converter;  
  converter.word = 0;
  int i = 0;
  int correct = size > sizeof(WORD) ? sizeof(WORD) : size;

#ifdef WORDS_BIGENDIAN
  for (i = 0; i < correct; ++i)
  {
      converter.bytes[i] = p[correct - i];
  }
#else
  for (i = 0; i < correct; ++i)
  {
      converter.bytes[i] = p[i];
  }
#endif
  
  return converter.word;
}

DWORD SwapDWord(BYTE *p, int size) {
  union BYTES2DWORD
  {
      DWORD dword;
      BYTE  bytes[sizeof(DWORD)];
  };
  
  union BYTES2DWORD converter;
  converter.dword = 0;
  int i = 0;  
  int correct = size > sizeof(DWORD) ? sizeof(DWORD) : size;
  
#ifdef WORDS_BIGENDIAN
  for (i = 0; i < correct; ++i)
  {
      converter.bytes[i] = p[correct - i];
  }
#else
  for (i = 0; i < correct; ++i)
  {
      converter.bytes[i] = p[i];
  }
#endif
  
  return converter.dword;
}



DDWORD SwapDDWord(BYTE *p, int size) {
  union BYTES2DDWORD
  {
      DDWORD ddword;
      BYTE   bytes[sizeof(DDWORD)];
  };
  
  union BYTES2DDWORD converter;
  converter.ddword = 0;
  int i = 0;  
  int correct = size > sizeof(DDWORD) ? sizeof(DDWORD) : size;
  
#ifdef WORDS_BIGENDIAN
  for (i = 0; i < correct; ++i)
  {
      converter.bytes[i] = p[correct - i];
  }
#else
  for (i = 0; i < correct; ++i)
  {
      converter.bytes[i] = p[i];
  }
#endif
  
  return converter.ddword;
}

/* convert 16-bit unicode to UTF8 unicode */
char *to_utf8(size_t len, char *buf) {
  int i, j = 0;
  /* worst case length */
  if (len > 10000) {	// deal with this by adding an arbitrary limit
     printf("suspecting a corrupt file in UTF8 conversion\n");
     exit(-1);
  }
  char *utf8 = malloc(3 * len / 2 + 1);

  for (i = 0; i < len - 1; i += 2) {
    unsigned int c = SwapWord((BYTE *)buf + i, 2);
    if (c <= 0x007f) {
      utf8[j++] = 0x00 | ((c & 0x007f) >> 0);
    } else if (c < 0x07ff) {
      utf8[j++] = 0xc0 | ((c & 0x07c0) >> 6);
      utf8[j++] = 0x80 | ((c & 0x003f) >> 0);
    } else {
      utf8[j++] = 0xe0 | ((c & 0xf000) >> 12);
      utf8[j++] = 0x80 | ((c & 0x0fc0) >> 6);
      utf8[j++] = 0x80 | ((c & 0x003f) >> 0);
    }
  }

  /* just in case the original was not null terminated */
  utf8[j++] = '\0';

  return utf8;
}


// -----------------------------------------------------------------------------
int TNEFDefaultHandler STD_ARGLIST {
  if (TNEF->Debug >= 1)
    printf("%s: [%i] %s\n", TNEFList[id].name, size, data);
  return 0;
}

// -----------------------------------------------------------------------------
int TNEFCodePage STD_ARGLIST {
  TNEF->CodePage.size = size;
  TNEF->CodePage.data = calloc(size, sizeof(BYTE));
  ALLOCCHECK(TNEF->CodePage.data);
  memcpy(TNEF->CodePage.data, data, size);
  return 0;
}

// -----------------------------------------------------------------------------
int TNEFParentID STD_ARGLIST {
  memcpy(TNEF->parentID, data, MIN(size, sizeof(TNEF->parentID)));
  return 0;
}
// -----------------------------------------------------------------------------
int TNEFMessageID STD_ARGLIST {
  memcpy(TNEF->messageID, data, MIN(size, sizeof(TNEF->messageID)));
  return 0;
}
// -----------------------------------------------------------------------------
int TNEFBody STD_ARGLIST {
  TNEF->body.size = size;
  TNEF->body.data = calloc(size, sizeof(BYTE));
  ALLOCCHECK(TNEF->body.data);
  memcpy(TNEF->body.data, data, size);
  return 0;
}
// -----------------------------------------------------------------------------
int TNEFOriginalMsgClass STD_ARGLIST {
  TNEF->OriginalMessageClass.size = size;
  TNEF->OriginalMessageClass.data = calloc(size, sizeof(BYTE));
  ALLOCCHECK(TNEF->OriginalMessageClass.data);
  memcpy(TNEF->OriginalMessageClass.data, data, size);
  return 0;
}
// -----------------------------------------------------------------------------
int TNEFMessageClass STD_ARGLIST {
  memcpy(TNEF->messageClass, data, MIN(size, sizeof(TNEF->messageClass)));
  return 0;
}
// -----------------------------------------------------------------------------
int TNEFFromHandler STD_ARGLIST {
  TNEF->from.data = calloc(size, sizeof(BYTE));
  ALLOCCHECK(TNEF->from.data);
  TNEF->from.size = size;
  memcpy(TNEF->from.data, data, size);
  return 0;
}
// -----------------------------------------------------------------------------
int TNEFSubjectHandler STD_ARGLIST {
  if (TNEF->subject.data)
    free(TNEF->subject.data);

  TNEF->subject.data = calloc(size, sizeof(BYTE));
  ALLOCCHECK(TNEF->subject.data);
  TNEF->subject.size = size;
  memcpy(TNEF->subject.data, data, size);
  return 0;
}

// -----------------------------------------------------------------------------
int TNEFRendData STD_ARGLIST {
  Attachment *p;
  // Find the last attachment.
  p = &(TNEF->starting_attach);
  while (p->next != NULL) p = p->next;

  // Add a new one
  p->next = calloc(1, sizeof(Attachment));
  ALLOCCHECK(p->next);
  p = p->next;

  TNEFInitAttachment(p);

 int correct = (size >= sizeof(renddata)) ? sizeof(renddata) : size;
  memcpy(&(p->RenderData), data, correct);
  return 0;
}

// -----------------------------------------------------------------------------
int TNEFVersion STD_ARGLIST {
  WORD major;
  WORD minor;
  minor = SwapWord((BYTE*)data, size);
  major = SwapWord((BYTE*)data + 2, size - 2);

  snprintf(TNEF->version, sizeof(TNEF->version), "TNEF%i.%i", major, minor);
  return 0;
}

// -----------------------------------------------------------------------------
int TNEFIcon STD_ARGLIST {
  Attachment *p;
  // Find the last attachment.
  p = &(TNEF->starting_attach);
  while (p->next != NULL) p = p->next;

  p->IconData.size = size;
  p->IconData.data = calloc(size, sizeof(BYTE));
  ALLOCCHECK(p->IconData.data);
  memcpy(p->IconData.data, data, size);
  return 0;
}

// -----------------------------------------------------------------------------
int TNEFRecipTable STD_ARGLIST {
  DWORD count;
  BYTE *d;
  int current_row;
  int propcount;
  int current_prop;

  d = (BYTE*)data;
  count = SwapDWord((BYTE*)d, 4);
  d += 4;
//    printf("Recipient Table containing %u rows\n", count);

  return 0;

  for (current_row = 0; current_row < count; current_row++) {
    propcount = SwapDWord((BYTE*)d, 4);
    if (TNEF->Debug >= 1)
      printf("> Row %i contains %i properties\n", current_row, propcount);
    d += 4;
    for (current_prop = 0; current_prop < propcount; current_prop++) {


    }
  }
  return 0;
}
// -----------------------------------------------------------------------------
int TNEFAttachmentMAPI STD_ARGLIST {
  Attachment *p;
  // Find the last attachment.
  //
  p = &(TNEF->starting_attach);
  while (p->next != NULL) p = p->next;
  return TNEFFillMapi(TNEF, (BYTE*)data, size, &(p->MAPI));
}
// -----------------------------------------------------------------------------
int TNEFMapiProperties STD_ARGLIST {
  if (TNEFFillMapi(TNEF, (BYTE*)data, size, &(TNEF->MapiProperties)) < 0) {
    printf("ERROR Parsing MAPI block\n");
    return -1;
  };
  if (TNEF->Debug >= 3) {
    MAPIPrint(&(TNEF->MapiProperties));
  }
  return 0;
}

int TNEFFillMapi(TNEFStruct *TNEF, BYTE *data, DWORD size, MAPIProps *p) {
  int i, j;
  DWORD num;
  BYTE *d;
  MAPIProperty *mp;
  DWORD type;
  DWORD length;
  variableLength *vl;

  WORD temp_word;
  DWORD temp_dword;
  DDWORD temp_ddword;
  int count = -1;
  int offset;

  d = data;
  p->count = SwapDWord((BYTE*)data, 4);
  d += 4;
  p->properties = calloc(p->count, sizeof(MAPIProperty));
  ALLOCCHECK(p->properties);
  mp = p->properties;

  for (i = 0; i < p->count; i++) {
    if (count == -1) {
      mp->id = SwapDWord((BYTE*)d, 4);
      d += 4;
      mp->custom = 0;
      mp->count = 1;
      mp->namedproperty = 0;
      length = -1;
      if (PROP_ID(mp->id) >= 0x8000) {
        // Read the GUID
        SIZECHECK(16);
        memcpy(&(mp->guid[0]), d, 16);
        d += 16;

        SIZECHECK(4);
        length = SwapDWord((BYTE*)d, 4);
        d += sizeof(DWORD);
        if (length > 0) {
          mp->namedproperty = length;
          mp->propnames = calloc(length, sizeof(variableLength));
          ALLOCCHECK(mp->propnames);
          while (length > 0) {
            SIZECHECK(4);
            type = SwapDWord((BYTE*)d, 4);
            mp->propnames[length - 1].data = calloc(type, sizeof(BYTE));
            ALLOCCHECK(mp->propnames[length - 1].data);
            mp->propnames[length - 1].size = type;
            d += 4;
            for (j = 0; j < (type >> 1); j++) {
              SIZECHECK(j*2);
              mp->propnames[length - 1].data[j] = d[j * 2];
            }
            d += type + ((type % 4) ? (4 - type % 4) : 0);
            length--;
          }
        } else {
          // READ the type
          SIZECHECK(sizeof(DWORD));
          type = SwapDWord((BYTE*)d, sizeof(DWORD));
          d += sizeof(DWORD);
          mp->id = PROP_TAG(PROP_TYPE(mp->id), type);
        }
        mp->custom = 1;
      }

      DEBUG2(TNEF->Debug, 3, "Type id = %04x, Prop id = %04x", PROP_TYPE(mp->id),
             PROP_ID(mp->id));
      if (PROP_TYPE(mp->id) & MV_FLAG) {
        mp->id = PROP_TAG(PROP_TYPE(mp->id) - MV_FLAG, PROP_ID(mp->id));
        SIZECHECK(4);
        mp->count = SwapDWord((BYTE*)d, 4);
        d += 4;
        count = 0;
      }
      mp->data = calloc(mp->count, sizeof(variableLength));
      ALLOCCHECK(mp->data);
      vl = mp->data;
    } else {
      i--;
      count++;
      vl = &(mp->data[count]);
    }

    switch (PROP_TYPE(mp->id)) {
      case PT_BINARY:
      case PT_OBJECT:
      case PT_STRING8:
      case PT_UNICODE:
        // First number of objects (assume 1 for now)
        if (count == -1) {
          SIZECHECK(4);
          vl->size = SwapDWord((BYTE*)d, 4);
          d += 4;
        }
        // now size of object
        SIZECHECK(4);
        vl->size = SwapDWord((BYTE*)d, 4);
        d += 4;

        // now actual object
        if (vl->size != 0) {    
         SIZECHECK(vl->size);
         if (PROP_TYPE(mp->id) == PT_UNICODE) {
                vl->data =(BYTE*) to_utf8(vl->size, (char*)d);
            } else {
              vl->data = calloc(vl->size, sizeof(BYTE));
              ALLOCCHECK(vl->data);
              memcpy(vl->data, d, vl->size);
            }
        } else {
          vl->data = NULL;
        }

        // Make sure to read in a multiple of 4
        num = vl->size;
        offset = ((num % 4) ? (4 - num % 4) : 0);
        d += num + ((num % 4) ? (4 - num % 4) : 0);
        break;

      case PT_I2:
        // Read in 2 bytes, but proceed by 4 bytes
        vl->size = 2;
        vl->data = calloc(vl->size, sizeof(WORD));
        ALLOCCHECK(vl->data);
        SIZECHECK(sizeof(WORD))
        temp_word = SwapWord((BYTE*)d, sizeof(WORD));
        memcpy(vl->data, &temp_word, vl->size);
        d += 4;
        break;
      case PT_BOOLEAN:
      case PT_LONG:
      case PT_R4:
      case PT_CURRENCY:
      case PT_APPTIME:
      case PT_ERROR:
        vl->size = 4;
        vl->data = calloc(vl->size, sizeof(BYTE));
        ALLOCCHECK(vl->data);
        SIZECHECK(4);
        temp_dword = SwapDWord((BYTE*)d, 4);
        memcpy(vl->data, &temp_dword, vl->size);
        d += 4;
        break;
      case PT_DOUBLE:
      case PT_I8:
      case PT_SYSTIME:
        vl->size = 8;
        vl->data = calloc(vl->size, sizeof(BYTE));
        ALLOCCHECK(vl->data);
        SIZECHECK(8);
        temp_ddword = SwapDDWord(d, 8);
        memcpy(vl->data, &temp_ddword, vl->size);
        d += 8;
        break;
      case PT_CLSID:
        vl->size = 16;
        vl->data = calloc(vl->size, sizeof(BYTE));
        ALLOCCHECK(vl->data);
        SIZECHECK(vl->size);
        memcpy(vl->data, d, vl->size);
        d+=16;
        break;
      default:
        printf("Bad file\n");
        exit(-1);
    }

    switch (PROP_ID(mp->id)) {
      case PR_SUBJECT:
      case PR_SUBJECT_IPM:
      case PR_ORIGINAL_SUBJECT:
      case PR_NORMALIZED_SUBJECT:
      case PR_CONVERSATION_TOPIC:
        DEBUG(TNEF->Debug, 3, "Got a Subject");
        if (TNEF->subject.size == 0) {
          int i;
          DEBUG(TNEF->Debug, 3, "Assigning a Subject");
          TNEF->subject.data = calloc(size, sizeof(BYTE));
          ALLOCCHECK(TNEF->subject.data);
          TNEF->subject.size = vl->size;
          memcpy(TNEF->subject.data, vl->data, vl->size);
          //  Unfortunately, we have to normalize out some invalid
          //  characters, or else the file won't write
          for (i = 0; i != TNEF->subject.size; i++) {
            switch (TNEF->subject.data[i]) {
              case '\\':
              case '/':
              case '\0':
                TNEF->subject.data[i] = '_';
                break;
            }
          }
        }
        break;
    }

    if (count == (mp->count - 1)) {
      count = -1;
    }
    if (count == -1) {
      mp++;
    }

  }
  if ((d - data) < size) {
    if (TNEF->Debug >= 1)  {
      printf("ERROR DURING MAPI READ\n");
      printf("Read %td bytes, Expected %u bytes\n", (d - data), size);
      printf("%td bytes missing\n", size - (d - data));
    }
  } else if ((d - data) > size) {
    if (TNEF->Debug >= 1)  {
      printf("ERROR DURING MAPI READ\n");
      printf("Read %td bytes, Expected %u bytes\n", (d - data), size);
      printf("%li bytes extra\n", (d - data) - size);
    }
  }
  return 0;
}
// -----------------------------------------------------------------------------
int TNEFSentFor STD_ARGLIST {
  WORD name_length, addr_length;
  BYTE *d;

  d = (BYTE*)data;

  while ((d - (BYTE*)data) < size) {
    SIZECHECK(sizeof(WORD));
    name_length = SwapWord((BYTE*)d, sizeof(WORD));
    d += sizeof(WORD);
    if (TNEF->Debug >= 1)
      printf("Sent For : %s", d);
    d += name_length;

    SIZECHECK(sizeof(WORD));
    addr_length = SwapWord((BYTE*)d, sizeof(WORD));
    d += sizeof(WORD);
    if (TNEF->Debug >= 1)
      printf("<%s>\n", d);
    d += addr_length;
  }
  return 0;
}
// -----------------------------------------------------------------------------
int TNEFDateHandler STD_ARGLIST {
  dtr *Date;
  Attachment *p;
  WORD * tmp_src, *tmp_dst;
  int i;

  p = &(TNEF->starting_attach);
  switch (TNEFList[id].id) {
    case attDateSent: Date = &(TNEF->dateSent); break;
    case attDateRecd: Date = &(TNEF->dateReceived); break;
    case attDateModified: Date = &(TNEF->dateModified); break;
    case attDateStart: Date = &(TNEF->DateStart); break;
    case attDateEnd:  Date = &(TNEF->DateEnd); break;
    case attAttachCreateDate:
      while (p->next != NULL) p = p->next;
      Date = &(p->CreateDate);
      break;
    case attAttachModifyDate:
      while (p->next != NULL) p = p->next;
      Date = &(p->ModifyDate);
      break;
    default:
      if (TNEF->Debug >= 1)
        printf("MISSING CASE\n");
      return YTNEF_UNKNOWN_PROPERTY;
  }

  tmp_src = (WORD *)data;
  tmp_dst = (WORD *)Date;
  for (i = 0; i < sizeof(dtr) / sizeof(WORD); i++) {
    *tmp_dst++ = SwapWord((BYTE *)tmp_src++, sizeof(WORD));
  }
  return 0;
}

void TNEFPrintDate(dtr Date) {
  char days[7][15] = {"Sunday", "Monday", "Tuesday",
                      "Wednesday", "Thursday", "Friday", "Saturday"
                     };
  char months[12][15] = {"January", "February", "March", "April", "May",
                         "June", "July", "August", "September", "October", "November",
                         "December"
                        };

  if (Date.wDayOfWeek < 7)
    printf("%s ", days[Date.wDayOfWeek]);

  if ((Date.wMonth < 13) && (Date.wMonth > 0))
    printf("%s ", months[Date.wMonth - 1]);

  printf("%hu, %hu ", Date.wDay, Date.wYear);

  if (Date.wHour > 12)
    printf("%i:%02hu:%02hu pm", (Date.wHour - 12),
           Date.wMinute, Date.wSecond);
  else if (Date.wHour == 12)
    printf("%hu:%02hu:%02hu pm", (Date.wHour),
           Date.wMinute, Date.wSecond);
  else
    printf("%hu:%02hu:%02hu am", Date.wHour,
           Date.wMinute, Date.wSecond);
}
// -----------------------------------------------------------------------------
int TNEFHexBreakdown STD_ARGLIST {
  int i;
  if (TNEF->Debug == 0)
    return 0;

  printf("%s: [%i bytes] \n", TNEFList[id].name, size);

  for (i = 0; i < size; i++) {
    printf("%02x ", data[i]);
    if ((i + 1) % 16 == 0) printf("\n");
  }
  printf("\n");
  return 0;
}

// -----------------------------------------------------------------------------
int TNEFDetailedPrint STD_ARGLIST {
  int i;
  if (TNEF->Debug == 0)
    return 0;

  printf("%s: [%i bytes] \n", TNEFList[id].name, size);

  for (i = 0; i < size; i++) {
    printf("%c", data[i]);
  }
  printf("\n");
  return 0;
}

// -----------------------------------------------------------------------------
int TNEFAttachmentFilename STD_ARGLIST {
  Attachment *p;
  p = &(TNEF->starting_attach);
  while (p->next != NULL) p = p->next;

  p->Title.size = size;
  p->Title.data = calloc(size, sizeof(BYTE));
  ALLOCCHECK(p->Title.data);
  memcpy(p->Title.data, data, size);

  return 0;
}

// -----------------------------------------------------------------------------
int TNEFAttachmentSave STD_ARGLIST {
  Attachment *p;
  p = &(TNEF->starting_attach);
  while (p->next != NULL) p = p->next;

  p->FileData.data = calloc(sizeof(char), size);
  ALLOCCHECK(p->FileData.data);
  p->FileData.size = size;

  memcpy(p->FileData.data, data, size);

  return 0;
}

// -----------------------------------------------------------------------------
int TNEFPriority STD_ARGLIST {
  DWORD value;

  value = SwapDWord((BYTE*)data, size);
  switch (value) {
    case 3:
      sprintf((TNEF->priority), "high");
      break;
    case 2:
      sprintf((TNEF->priority), "normal");
      break;
    case 1:
      sprintf((TNEF->priority), "low");
      break;
    default:
      sprintf((TNEF->priority), "N/A");
      break;
  }
  return 0;
}

// -----------------------------------------------------------------------------
int TNEFCheckForSignature(DWORD sig) {
  DWORD signature = 0x223E9F78;

  sig = SwapDWord((BYTE *)&sig, sizeof(DWORD));

  if (signature == sig) {
    return 0;
  } else {
    return YTNEF_NOT_TNEF_STREAM;
  }
}

// -----------------------------------------------------------------------------
int TNEFGetKey(TNEFStruct *TNEF, WORD *key) {
  if (TNEF->IO.ReadProc(&(TNEF->IO), sizeof(WORD), 1, key) < 1) {
    if (TNEF->Debug >= 1)
      printf("Error reading Key\n");
    return YTNEF_ERROR_READING_DATA;
  }
  *key = SwapWord((BYTE *)key, sizeof(WORD));

  DEBUG1(TNEF->Debug, 2, "Key = 0x%X", *key);
  DEBUG1(TNEF->Debug, 2, "Key = %i", *key);
  return 0;
}

// -----------------------------------------------------------------------------
int TNEFGetHeader(TNEFStruct *TNEF, DWORD *type, DWORD *size) {
  BYTE component;

  DEBUG(TNEF->Debug, 2, "About to read Component");
  if (TNEF->IO.ReadProc(&(TNEF->IO), sizeof(BYTE), 1, &component) < 1) {
    return YTNEF_ERROR_READING_DATA;
  }


  DEBUG(TNEF->Debug, 2, "About to read type");
  if (TNEF->IO.ReadProc(&(TNEF->IO), sizeof(DWORD), 1, type)  < 1) {
    if (TNEF->Debug >= 1)
      printf("ERROR: Error reading type\n");
    return YTNEF_ERROR_READING_DATA;
  }
  DEBUG1(TNEF->Debug, 2, "Type = 0x%X", *type);
  DEBUG1(TNEF->Debug, 2, "Type = %u", *type);


  DEBUG(TNEF->Debug, 2, "About to read size");
  if (TNEF->IO.ReadProc(&(TNEF->IO), sizeof(DWORD), 1, size) < 1) {
    if (TNEF->Debug >= 1)
      printf("ERROR: Error reading size\n");
    return YTNEF_ERROR_READING_DATA;
  }


  DEBUG1(TNEF->Debug, 2, "Size = %u", *size);

  *type = SwapDWord((BYTE *)type, sizeof(DWORD));
  *size = SwapDWord((BYTE *)size, sizeof(DWORD));

  return 0;
}

// -----------------------------------------------------------------------------
int TNEFRawRead(TNEFStruct *TNEF, BYTE *data, DWORD size, WORD *checksum) {
  WORD temp;
  int i;

  if (TNEF->IO.ReadProc(&TNEF->IO, sizeof(BYTE), size, data) < size) {
    if (TNEF->Debug >= 1)
      printf("ERROR: Error reading data\n");
    return YTNEF_ERROR_READING_DATA;
  }


  if (checksum != NULL) {
    *checksum = 0;
    for (i = 0; i < size; i++) {
      temp = data[i];
      *checksum = (*checksum + temp);
    }
  }
  return 0;
}

#define INITVARLENGTH(x) (x).data = NULL; (x).size = 0;
#define INITDTR(x) (x).wYear=0; (x).wMonth=0; (x).wDay=0; \
                   (x).wHour=0; (x).wMinute=0; (x).wSecond=0; \
                   (x).wDayOfWeek=0;
#define INITSTR(x) memset((x), 0, sizeof(x));
void TNEFInitMapi(MAPIProps *p) {
  p->count = 0;
  p->properties = NULL;
}

void TNEFInitAttachment(Attachment *p) {
  INITDTR(p->Date);
  INITVARLENGTH(p->Title);
  INITVARLENGTH(p->MetaFile);
  INITDTR(p->CreateDate);
  INITDTR(p->ModifyDate);
  INITVARLENGTH(p->TransportFilename);
  INITVARLENGTH(p->FileData);
  INITVARLENGTH(p->IconData);
  memset(&(p->RenderData), 0, sizeof(renddata));
  TNEFInitMapi(&(p->MAPI));
  p->next = NULL;
}

void TNEFInitialize(TNEFStruct *TNEF) {
  INITSTR(TNEF->version);
  INITVARLENGTH(TNEF->from);
  INITVARLENGTH(TNEF->subject);
  INITDTR(TNEF->dateSent);
  INITDTR(TNEF->dateReceived);

  INITSTR(TNEF->messageStatus);
  INITSTR(TNEF->messageClass);
  INITSTR(TNEF->messageID);
  INITSTR(TNEF->parentID);
  INITSTR(TNEF->conversationID);
  INITVARLENGTH(TNEF->body);
  INITSTR(TNEF->priority);
  TNEFInitAttachment(&(TNEF->starting_attach));
  INITDTR(TNEF->dateModified);
  TNEFInitMapi(&(TNEF->MapiProperties));
  INITVARLENGTH(TNEF->CodePage);
  INITVARLENGTH(TNEF->OriginalMessageClass);
  INITVARLENGTH(TNEF->Owner);
  INITVARLENGTH(TNEF->SentFor);
  INITVARLENGTH(TNEF->Delegate);
  INITDTR(TNEF->DateStart);
  INITDTR(TNEF->DateEnd);
  INITVARLENGTH(TNEF->AidOwner);
  TNEF->RequestRes = 0;
  TNEF->IO.data = NULL;
  TNEF->IO.InitProc = NULL;
  TNEF->IO.ReadProc = NULL;
  TNEF->IO.CloseProc = NULL;
}
#undef INITVARLENGTH
#undef INITDTR
#undef INITSTR

#define FREEVARLENGTH(x) if ((x).size > 0) { \
                            free((x).data); (x).size =0; }
void TNEFFree(TNEFStruct *TNEF) {
  Attachment *p, *store;

  FREEVARLENGTH(TNEF->from);
  FREEVARLENGTH(TNEF->subject);
  FREEVARLENGTH(TNEF->body);
  FREEVARLENGTH(TNEF->CodePage);
  FREEVARLENGTH(TNEF->OriginalMessageClass);
  FREEVARLENGTH(TNEF->Owner);
  FREEVARLENGTH(TNEF->SentFor);
  FREEVARLENGTH(TNEF->Delegate);
  FREEVARLENGTH(TNEF->AidOwner);
  TNEFFreeMapiProps(&(TNEF->MapiProperties));

  p = TNEF->starting_attach.next;
  while (p != NULL) {
    TNEFFreeAttachment(p);
    store = p->next;
    free(p);
    p = store;
  }
}

void TNEFFreeAttachment(Attachment *p) {
  FREEVARLENGTH(p->Title);
  FREEVARLENGTH(p->MetaFile);
  FREEVARLENGTH(p->TransportFilename);
  FREEVARLENGTH(p->FileData);
  FREEVARLENGTH(p->IconData);
  TNEFFreeMapiProps(&(p->MAPI));
}

void TNEFFreeMapiProps(MAPIProps *p) {
  int i, j;
  for (i = 0; i < p->count; i++) {
    for (j = 0; j < p->properties[i].count; j++) {
      FREEVARLENGTH(p->properties[i].data[j]);
    }
    free(p->properties[i].data);
    for (j = 0; j < p->properties[i].namedproperty; j++) {
      FREEVARLENGTH(p->properties[i].propnames[j]);
    }
    free(p->properties[i].propnames);
  }
  free(p->properties);
  p->count = 0;
}
#undef FREEVARLENGTH

// Procedures to handle File IO
int TNEFFile_Open(TNEFIOStruct *IO) {
  TNEFFileInfo *finfo;
  finfo = (TNEFFileInfo *)IO->data;

  DEBUG1(finfo->Debug, 3, "Opening %s", finfo->filename);
  if ((finfo->fptr = fopen(finfo->filename, "rb")) == NULL) {
    return -1;
  } else {
    return 0;
  }
}

int TNEFFile_Read(TNEFIOStruct *IO, int size, int count, void *dest) {
  TNEFFileInfo *finfo;
  finfo = (TNEFFileInfo *)IO->data;

  DEBUG2(finfo->Debug, 3, "Reading %i blocks of %i size", count, size);
  if (finfo->fptr != NULL) {
    return fread((BYTE *)dest, size, count, finfo->fptr);
  } else {
    return -1;
  }
}

int TNEFFile_Close(TNEFIOStruct *IO) {
  TNEFFileInfo *finfo;
  finfo = (TNEFFileInfo *)IO->data;

  DEBUG1(finfo->Debug, 3, "Closing file %s", finfo->filename);
  if (finfo->fptr != NULL) {
    fclose(finfo->fptr);
    finfo->fptr = NULL;
  }
  return 0;
}

int TNEFParseFile(char *filename, TNEFStruct *TNEF) {
  TNEFFileInfo finfo;

  if (TNEF->Debug >= 1)
    printf("Attempting to parse %s...\n", filename);


  finfo.filename = filename;
  finfo.fptr = NULL;
  finfo.Debug = TNEF->Debug;
  TNEF->IO.data = (void *)&finfo;
  TNEF->IO.InitProc = TNEFFile_Open;
  TNEF->IO.ReadProc = TNEFFile_Read;
  TNEF->IO.CloseProc = TNEFFile_Close;
  return TNEFParse(TNEF);
}
//-------------------------------------------------------------
// Procedures to handle Memory IO
int TNEFMemory_Open(TNEFIOStruct *IO) {
  TNEFMemInfo *minfo;
  minfo = (TNEFMemInfo *)IO->data;

  minfo->ptr = minfo->dataStart;
  return 0;
}

int TNEFMemory_Read(TNEFIOStruct *IO, int size, int count, void *dest) {
  TNEFMemInfo *minfo;
  int length;
  long max;
  minfo = (TNEFMemInfo *)IO->data;

  length = count * size;
  max = (minfo->dataStart + minfo->size) - (minfo->ptr);
  if (length > max) {
    return -1;
  }

  DEBUG1(minfo->Debug, 3, "Copying %i bytes", length);

  memcpy(dest, minfo->ptr, length);
  minfo->ptr += length;
  return count;
}

int TNEFMemory_Close(TNEFIOStruct *IO) {
  // Do nothing, really...
  return 0;
}

int TNEFParseMemory(BYTE *memory, long size, TNEFStruct *TNEF) {
  TNEFMemInfo minfo;

  DEBUG(TNEF->Debug, 1, "Attempting to parse memory block...\n");

  minfo.dataStart = memory;
  minfo.ptr = memory;
  minfo.size = size;
  minfo.Debug = TNEF->Debug;
  TNEF->IO.data = (void *)&minfo;
  TNEF->IO.InitProc = TNEFMemory_Open;
  TNEF->IO.ReadProc = TNEFMemory_Read;
  TNEF->IO.CloseProc = TNEFMemory_Close;
  return TNEFParse(TNEF);
}


int TNEFParse(TNEFStruct *TNEF) {
  WORD key;
  DWORD type;
  DWORD size;
  DWORD signature;
  BYTE *data;
  WORD checksum, header_checksum;
  int i;

  if (TNEF->IO.ReadProc == NULL) {
    printf("ERROR: Setup incorrectly: No ReadProc\n");
    return YTNEF_INCORRECT_SETUP;
  }

  if (TNEF->IO.InitProc != NULL) {
    DEBUG(TNEF->Debug, 2, "About to initialize");
    if (TNEF->IO.InitProc(&TNEF->IO) != 0) {
      return YTNEF_CANNOT_INIT_DATA;
    }
    DEBUG(TNEF->Debug, 2, "Initialization finished");
  }

  DEBUG(TNEF->Debug, 2, "Reading Signature");
  if (TNEF->IO.ReadProc(&TNEF->IO, sizeof(DWORD), 1, &signature) < 1) {
    printf("ERROR: Error reading signature\n");
    if (TNEF->IO.CloseProc != NULL) {
      TNEF->IO.CloseProc(&TNEF->IO);
    }
    return YTNEF_ERROR_READING_DATA;
  }

  DEBUG(TNEF->Debug, 2, "Checking Signature");
  if (TNEFCheckForSignature(signature) < 0) {
    printf("ERROR: Signature does not match. Not TNEF.\n");
    if (TNEF->IO.CloseProc != NULL) {
      TNEF->IO.CloseProc(&TNEF->IO);
    }
    return YTNEF_NOT_TNEF_STREAM;
  }

  DEBUG(TNEF->Debug, 2, "Reading Key.");

  if (TNEFGetKey(TNEF, &key) < 0) {
    printf("ERROR: Unable to retrieve key.\n");
    if (TNEF->IO.CloseProc != NULL) {
      TNEF->IO.CloseProc(&TNEF->IO);
    }
    return YTNEF_NO_KEY;
  }

  DEBUG(TNEF->Debug, 2, "Starting Full Processing.");

  while (TNEFGetHeader(TNEF, &type, &size) == 0) {
    DEBUG2(TNEF->Debug, 2, "Header says type=0x%X, size=%u", type, size);
    DEBUG2(TNEF->Debug, 2, "Header says type=%u, size=%u", type, size);
    if(size == 0) {
      printf("ERROR: Field with size of 0\n");
      return YTNEF_ERROR_READING_DATA;
    }
    data = calloc(size, sizeof(BYTE));
    ALLOCCHECK(data);
    if (TNEFRawRead(TNEF, data, size, &header_checksum) < 0) {
      printf("ERROR: Unable to read data.\n");
      if (TNEF->IO.CloseProc != NULL) {
        TNEF->IO.CloseProc(&TNEF->IO);
      }
      free(data);
      return YTNEF_ERROR_READING_DATA;
    }
    if (TNEFRawRead(TNEF, (BYTE *)&checksum, 2, NULL) < 0) {
      printf("ERROR: Unable to read checksum.\n");
      if (TNEF->IO.CloseProc != NULL) {
        TNEF->IO.CloseProc(&TNEF->IO);
      }
      free(data);
      return YTNEF_ERROR_READING_DATA;
    }
    checksum = SwapWord((BYTE *)&checksum, sizeof(WORD));
    if (checksum != header_checksum) {
      printf("ERROR: Checksum mismatch. Data corruption?:\n");
      if (TNEF->IO.CloseProc != NULL) {
        TNEF->IO.CloseProc(&TNEF->IO);
      }
      free(data);
      return YTNEF_BAD_CHECKSUM;
    }
    for (i = 0; i < (sizeof(TNEFList) / sizeof(TNEFHandler)); i++) {
      if (TNEFList[i].id == type) {
        if (TNEFList[i].handler != NULL) {
          if (TNEFList[i].handler(TNEF, i, (char*)data, size) < 0) {
            free(data);
            if (TNEF->IO.CloseProc != NULL) {
              TNEF->IO.CloseProc(&TNEF->IO);
            }
            return YTNEF_ERROR_IN_HANDLER;
          } else {
            //  Found our handler and processed it.  now time to get out
            break;
          }
        } else {
          DEBUG2(TNEF->Debug, 1, "No handler for %s: %u bytes",
                 TNEFList[i].name, size);
        }
      }
    }

    free(data);
  }

  if (TNEF->IO.CloseProc != NULL) {
    TNEF->IO.CloseProc(&TNEF->IO);
  }
  return 0;

}

// ----------------------------------------------------------------------------

variableLength *MAPIFindUserProp(MAPIProps *p, unsigned int ID) {
  int i;
  if (p != NULL) {
    for (i = 0; i < p->count; i++) {
      if ((p->properties[i].id == ID) && (p->properties[i].custom == 1)) {
        return (p->properties[i].data);
      }
    }
  }
  return MAPI_UNDEFINED;
}

variableLength *MAPIFindProperty(MAPIProps *p, unsigned int ID) {
  int i;
  if (p != NULL) {
    for (i = 0; i < p->count; i++) {
      if ((p->properties[i].id == ID) && (p->properties[i].custom == 0)) {
        return (p->properties[i].data);
      }
    }
  }
  return MAPI_UNDEFINED;
}

int MAPISysTimetoDTR(BYTE *data, dtr *thedate) {
  DDWORD ddword_tmp;
  int startingdate = 0;
  int tmp_date;
  int days_in_year = 365;
  unsigned int months[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  ddword_tmp = *((DDWORD *)data);
  ddword_tmp = ddword_tmp / 10; // micro-s
  ddword_tmp /= 1000; // ms
  ddword_tmp /= 1000; // s

  thedate->wSecond = (ddword_tmp % 60);

  ddword_tmp /= 60; // seconds to minutes
  thedate->wMinute = (ddword_tmp % 60);

  ddword_tmp /= 60; //minutes to hours
  thedate->wHour = (ddword_tmp % 24);

  ddword_tmp /= 24; // Hours to days

  // Now calculate the year based on # of days
  thedate->wYear = 1601;
  startingdate = 1;
  while (ddword_tmp >= days_in_year) {
    ddword_tmp -= days_in_year;
    thedate->wYear++;
    days_in_year = 365;
    startingdate++;
    if ((thedate->wYear % 4) == 0) {
      if ((thedate->wYear % 100) == 0) {
        // if the year is 1700,1800,1900, etc, then it is only
        // a leap year if exactly divisible by 400, not 4.
        if ((thedate->wYear % 400) == 0) {
          startingdate++;
          days_in_year = 366;
        }
      }  else {
        startingdate++;
        days_in_year = 366;
      }
    }
    startingdate %= 7;
  }

  // the remaining number is the day # in this year
  // So now calculate the Month, & Day of month
  if ((thedate->wYear % 4) == 0) {
    // 29 days in february in a leap year
    months[1] = 29;
  }

  tmp_date = (int)ddword_tmp;
  thedate->wDayOfWeek = (tmp_date + startingdate) % 7;
  thedate->wMonth = 0;

  while (tmp_date > months[thedate->wMonth]) {
    tmp_date -= months[thedate->wMonth];
    thedate->wMonth++;
  }
  thedate->wMonth++;
  thedate->wDay = tmp_date + 1;
  return 0;
}

void MAPIPrint(MAPIProps *p) {
  int j, i, index, h, x;
  DDWORD *ddword_ptr;
  DDWORD ddword_tmp;
  dtr thedate;
  MAPIProperty *mapi;
  variableLength *mapidata;
  variableLength vlTemp;
  int found;

  for (j = 0; j < p->count; j++) {
    mapi = &(p->properties[j]);
    printf("   #%i: Type: [", j);
    switch (PROP_TYPE(mapi->id)) {
      case PT_UNSPECIFIED:
        printf("  NONE   "); break;
      case PT_NULL:
        printf("  NULL   "); break;
      case PT_I2:
        printf("   I2    "); break;
      case PT_LONG:
        printf("  LONG   "); break;
      case PT_R4:
        printf("   R4    "); break;
      case PT_DOUBLE:
        printf(" DOUBLE  "); break;
      case PT_CURRENCY:
        printf("CURRENCY "); break;
      case PT_APPTIME:
        printf("APP TIME "); break;
      case PT_ERROR:
        printf("  ERROR  "); break;
      case PT_BOOLEAN:
        printf(" BOOLEAN "); break;
      case PT_OBJECT:
        printf(" OBJECT  "); break;
      case PT_I8:
        printf("   I8    "); break;
      case PT_STRING8:
        printf(" STRING8 "); break;
      case PT_UNICODE:
        printf(" UNICODE "); break;
      case PT_SYSTIME:
        printf("SYS TIME "); break;
      case PT_CLSID:
        printf("OLE GUID "); break;
      case PT_BINARY:
        printf(" BINARY  "); break;
      default:
        printf("<%x>", PROP_TYPE(mapi->id)); break;
    }

    printf("]  Code: [");
    if (mapi->custom == 1) {
      printf("UD:x%04x", PROP_ID(mapi->id));
    } else {
      found = 0;
      for (index = 0; index < sizeof(MPList) / sizeof(MAPIPropertyTagList); index++) {
        if ((MPList[index].id == PROP_ID(mapi->id)) && (found == 0)) {
          printf("%s", MPList[index].name);
          found = 1;
        }
      }
      if (found == 0) {
        printf("0x%04x", PROP_ID(mapi->id));
      }
    }
    printf("]\n");
    if (mapi->namedproperty > 0) {
      for (i = 0; i < mapi->namedproperty; i++) {
        printf("    Name: %s\n", mapi->propnames[i].data);
      }
    }
    for (i = 0; i < mapi->count; i++) {
      mapidata = &(mapi->data[i]);
      if (mapi->count > 1) {
        printf("    [%i/%u] ", i, mapi->count);
      } else {
        printf("    ");
      }
      printf("Size: %i", mapidata->size);
      switch (PROP_TYPE(mapi->id)) {
        case PT_SYSTIME:
          MAPISysTimetoDTR(mapidata->data, &thedate);
          printf("    Value: ");
          ddword_tmp = *((DDWORD *)mapidata->data);
          TNEFPrintDate(thedate);
          printf(" [HEX: ");
          for (x = 0; x < sizeof(ddword_tmp); x++) {
            printf(" %02x", (BYTE)mapidata->data[x]);
          }
          printf("] (%llu)\n", ddword_tmp);
          break;
        case PT_LONG:
          printf("    Value: %i\n", *((int*)mapidata->data));
          break;
        case PT_I2:
          printf("    Value: %hi\n", *((short int*)mapidata->data));
          break;
        case PT_BOOLEAN:
          if (mapi->data->data[0] != 0) {
            printf("    Value: True\n");
          } else {
            printf("    Value: False\n");
          }
          break;
        case PT_OBJECT:
          printf("\n");
          break;
        case PT_BINARY:
          if (IsCompressedRTF(mapidata) == 1) {
            printf("    Detected Compressed RTF. ");
            printf("Decompressed text follows\n");
            printf("-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-\n");
            if ((vlTemp.data = (BYTE*)DecompressRTF(mapidata, &(vlTemp.size))) != NULL) {
              printf("%s\n", vlTemp.data);
              free(vlTemp.data);
            }
            printf("-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-\n");
          } else {
            printf("    Value: [");
            for (h = 0; h < mapidata->size; h++) {
              if (isprint(mapidata->data[h])) {
                printf("%c", mapidata->data[h]);
              } else {
                printf(".");
              }

            }
            printf("]\n");
          }
          break;
        case PT_STRING8:
          printf("    Value: [%s]\n", mapidata->data);
          if (strlen((char*)mapidata->data) != mapidata->size - 1) {
            printf("Detected Hidden data: [");
            for (h = 0; h < mapidata->size; h++) {
              if (isprint(mapidata->data[h])) {
                printf("%c", mapidata->data[h]);
              } else {
                printf(".");
              }

            }
            printf("]\n");
          }
          break;
        case PT_CLSID:
          printf("    Value: ");
          printf("[HEX: ");
          for(x=0; x< 16; x++) {
            printf(" %02x", (BYTE)mapidata->data[x]);
          }
          printf("]\n");
          break;
        default:
          printf("    Value: [%s]\n", mapidata->data);
      }
    }
  }
}


int IsCompressedRTF(variableLength *p) {
  unsigned int in;
  BYTE *src;
  ULONG magic;

  if (p->size < 4)
    return 0;

  src = p->data;
  in = 0;

  in += 4;
  in += 4;
  magic = SwapDWord((BYTE*)src + in, 4);

  if (magic == 0x414c454d) {
    return 1;
  } else if (magic == 0x75465a4c) {
    return 1;
  } else {
    return 0;
  }
}

BYTE *DecompressRTF(variableLength *p, int *size) {
  BYTE *dst; // destination for uncompressed bytes
  BYTE *src;
  unsigned int in;
  unsigned int out;
  variableLength comp_Prebuf;
  ULONG compressedSize, uncompressedSize, magic;

  comp_Prebuf.size = strlen(RTF_PREBUF);
  comp_Prebuf.data = calloc(comp_Prebuf.size+1, 1);
  ALLOCCHECK_CHAR(comp_Prebuf.data);
  memcpy(comp_Prebuf.data, RTF_PREBUF, comp_Prebuf.size);

  src = p->data;
  in = 0;

  if (p->size < 20) {
    printf("File too small\n");
    return(NULL);
  }
  compressedSize = (ULONG)SwapDWord((BYTE*)src + in, 4);
  in += 4;
  uncompressedSize = (ULONG)SwapDWord((BYTE*)src + in, 4);
  in += 4;
  magic = SwapDWord((BYTE*)src + in, 4);
  in += 4;
  in += 4;

  // check size excluding the size field itself
  if (compressedSize != p->size - 4) {
    printf(" Size Mismatch: %u != %i\n", compressedSize, p->size - 4);
    free(comp_Prebuf.data);
    return NULL;
  }

  // process the data
  if (magic == 0x414c454d) {
    // magic number that identifies the stream as a uncompressed stream
    dst = calloc(uncompressedSize, 1);
    ALLOCCHECK_CHAR(dst);
    memcpy(dst, src + 4, uncompressedSize);
  } else if (magic == 0x75465a4c) {
    // magic number that identifies the stream as a compressed stream
    int flagCount = 0;
    int flags = 0;
    // Prevent overflow on 32 Bit Systems
    if (comp_Prebuf.size >= INT_MAX - uncompressedSize) {
       printf("Corrupted file\n");
       exit(-1);
    }
    dst = calloc(comp_Prebuf.size + uncompressedSize, 1);
    ALLOCCHECK_CHAR(dst);
    memcpy(dst, comp_Prebuf.data, comp_Prebuf.size);
    out = comp_Prebuf.size;
    while ((out < (comp_Prebuf.size + uncompressedSize)) && (in < p->size)) {
      // each flag byte flags 8 literals/references, 1 per bit
      flags = (flagCount++ % 8 == 0) ? src[in++] : flags >> 1;
      if ((flags & 1) == 1) { // each flag bit is 1 for reference, 0 for literal
        unsigned int offset = src[in++];
        unsigned int length = src[in++];
        unsigned int end;
        offset = (offset << 4) | (length >> 4); // the offset relative to block start
        length = (length & 0xF) + 2; // the number of bytes to copy
        // the decompression buffer is supposed to wrap around back
        // to the beginning when the end is reached. we save the
        // need for such a buffer by pointing straight into the data
        // buffer, and simulating this behaviour by modifying the
        // pointers appropriately.
        offset = (out / 4096) * 4096 + offset;
        if (offset >= out) // take from previous block
          offset -= 4096;
        // note: can't use System.arraycopy, because the referenced
        // bytes can cross through the current out position.
        end = offset + length;
        while ((offset < end) && (out < (comp_Prebuf.size + uncompressedSize))
             && (offset < (comp_Prebuf.size + uncompressedSize)))
          dst[out++] = dst[offset++];
      } else { // literal
        if ((out >= (comp_Prebuf.size + uncompressedSize)) ||
            (in >= p->size)) {
          printf("Corrupted stream\n");
          exit(-1);
        }
        dst[out++] = src[in++];
      }
    }
    // copy it back without the prebuffered data
    src = dst;
    dst = calloc(uncompressedSize, 1);
    ALLOCCHECK_CHAR(dst);
    memcpy(dst, src + comp_Prebuf.size, uncompressedSize);
    free(src);
    *size = uncompressedSize;
    free(comp_Prebuf.data);
    return dst;
  } else { // unknown magic number
    printf("Unknown compression type (magic number %x)\n", magic);
  }
  free(comp_Prebuf.data);
  return NULL;
}
