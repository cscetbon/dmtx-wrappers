/*
 * libdmtx - Data Matrix Encoding/Decoding Library
 *
 * Copyright (C) 2011 Mike Laughton
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Contact: mike@dragonflylogic.com
 */

/* $Id$ */

/**
 * this file deals with encoding logic (scheme rules)
 *
 * In the context of this file:
 *
 * A "word" refers to a full codeword byte to be appended to the encoded output.
 *
 * A "value" refers to any scheme value being appended to the output stream,
 * regardless of how many bytes are used to represent it. Examples:
 *
 *   ASCII:                   1 value  in  1 word
 *   ASCII (digits):          2 values in  1 word
 *   C40/Text/X12:            3 values in  2 words
 *   C40/Text/X12 (unlatch):  1 values in  1 word
 *   EDIFACT:                 4 values in  3 words
 *   Base 256:                1 value  in  1 word
 *
 *   - Shifts count as values, so outputChainValueCount will reflect these.
 *
 *   - Latches and unlatches are also counted as values, but always in the
 *     scheme being exited.
 *
 *   - Base256 header bytes are not included as values.
 *
 * A "chunk" refers to the minimum grouping of values in a schema that must be
 * encoded together.
 *
 *   ASCII:                   1 value  (1 word)  in 1 chunk
 *   ASCII (digits):          2 values (1 word)  in 1 chunk (optional)
 *   C40/Text/X12:            3 values (2 words) in 1 chunk
 *   C40/Text/X12 (unlatch):  1 value  (1 word)  in 1 chunk
 *   EDIFACT:                 1 value  (1 word*) in 1 chunk
 *   Base 256:                1 value  (1 word)  in 1 chunk
 *
 *   * EDIFACT writes 6 bits at a time, but progress is tracked to the next byte
 *     boundary. If unlatch value finishes mid-byte, the remaining bits before
 *     the next boundary are all set to zero.
 *
 * XXX maybe reorder the functions list in the file and break them up:
 *
 * Each scheme implements 3 equivalent functions:
 *   - EncodeNextChunk[Scheme]
 *   - EncodeValue[Scheme]
 *   - CompleteIfDone[Scheme]
 *
 * XXX what about renaming EncodeValue[Scheme] to AppendValue[Scheme]? That
 * shows that the stream is being affected
 *
 * The master function EncodeNextChunk() (no Scheme in the name) knows which
 * scheme-specific implementations to call based on the stream's current
 * encodation scheme.
 *
 * It's important that EncodeNextChunk[Scheme] not call CompleteIfDone[Scheme]
 * directly because some parts of the logic might want to encode a stream
 * without allowing the padding and other extra logic that can occur when an
 * end-of-symbol condition is triggered.
 */

#include "dmtx.h"
#include "dmtxstatic.h"

/* XXX is there a way to handle muliple values of s? */
#define CHKSCHEME(s) { if(stream->currentScheme != (s)) { StreamMarkFatal(stream, 1); return; } }

/* CHKERR should follow any call that might alter stream status */
#define CHKERR { if(stream->status != DmtxStatusEncoding) { return; } }

/* CHKSIZE should follows typical calls to FindSymbolSize()  */
#define CHKSIZE { if(sizeIdx == DmtxUndefined) { StreamMarkInvalid(stream, 1); return; } }

/**
 *
 *
 */
static DmtxPassFail
EncodeSingleScheme2(DmtxEncodeStream *stream, DmtxScheme targetScheme, int requestedSizeIdx)
{
   if(stream->currentScheme != DmtxSchemeAscii)
   {
      StreamMarkFatal(stream, 1);
      return DmtxFail;
   }

   while(stream->status == DmtxStatusEncoding)
      EncodeNextChunk(stream, targetScheme, requestedSizeIdx);

   if(stream->status != DmtxStatusComplete || StreamInputHasNext(stream))
      return DmtxFail; /* throw out an error too? */

   return DmtxPass;
}

/**
 * This function distributes work to the equivalent scheme-specific
 * implementation.
 *
 * Each of these functions will encode the next symbol input word, and in some
 * cases this requires additional input words to be encoded as well.
 */
static void
EncodeNextChunk(DmtxEncodeStream *stream, DmtxScheme targetScheme, int requestedSizeIdx)
{
   /* Change to target scheme if necessary */
   if(stream->currentScheme != targetScheme)
   {
      EncodeChangeScheme(stream, targetScheme, DmtxUnlatchExplicit); CHKERR;
      CHKSCHEME(targetScheme);
   }

   switch(stream->currentScheme)
   {
      case DmtxSchemeAscii:
         EncodeNextChunkAscii(stream); CHKERR;
         CompleteIfDoneAscii(stream, requestedSizeIdx); CHKERR;
         break;
      case DmtxSchemeC40:
      case DmtxSchemeText:
      case DmtxSchemeX12:
         EncodeNextChunkCTX(stream, requestedSizeIdx); CHKERR;
         CompleteIfDoneCTX(stream, requestedSizeIdx); CHKERR;
         break;
      case DmtxSchemeEdifact:
         EncodeNextChunkEdifact(stream); CHKERR;
         CompleteIfDoneEdifact(stream, requestedSizeIdx); CHKERR;
         break;
      case DmtxSchemeBase256:
         EncodeNextChunkBase256(stream); CHKERR;
         CompleteIfDoneBase256(stream, requestedSizeIdx); CHKERR;
         break;
      default:
         StreamMarkFatal(stream, 1 /* unknown */);
         break;
   }
}

/**
 *
 *
 */
static void
EncodeChangeScheme(DmtxEncodeStream *stream, DmtxScheme targetScheme, int unlatchType)
{
   /* Nothing to do */
   if(stream->currentScheme == targetScheme)
      return;

   /* Every latch must go through ASCII */
   switch(stream->currentScheme)
   {
      case DmtxSchemeC40:
      case DmtxSchemeText:
      case DmtxSchemeX12:
         if(unlatchType == DmtxUnlatchExplicit)
         {
            EncodeUnlatchCTX(stream); CHKERR;
         }
         break;
      case DmtxSchemeEdifact:
         if(unlatchType == DmtxUnlatchExplicit)
         {
            EncodeValueEdifact(stream, DmtxValueEdifactUnlatch); CHKERR;
         }
         break;
      default:
         /* Nothing to do for ASCII or Base 256 */
         assert(stream->currentScheme == DmtxSchemeAscii ||
               stream->currentScheme == DmtxSchemeBase256);
         break;
   }
   stream->currentScheme = DmtxSchemeAscii;

   /* Anything other than ASCII (the default) requires a latch */
   switch(targetScheme)
   {
      case DmtxSchemeC40:
         EncodeValueAscii(stream, DmtxValueC40Latch); CHKERR;
         break;
      case DmtxSchemeText:
         EncodeValueAscii(stream, DmtxValueTextLatch); CHKERR;
         break;
      case DmtxSchemeX12:
         EncodeValueAscii(stream, DmtxValueX12Latch); CHKERR;
         break;
      case DmtxSchemeEdifact:
         EncodeValueAscii(stream, DmtxValueEdifactLatch); CHKERR;
         break;
      case DmtxSchemeBase256:
         EncodeValueAscii(stream, DmtxValueBase256Latch); CHKERR;
         break;
      default:
         /* Nothing to do for ASCII */
         CHKSCHEME(DmtxSchemeAscii);
         break;
   }
   stream->currentScheme = targetScheme;

   /* Reset new chain length to zero */
   stream->outputChainWordCount = 0;
   stream->outputChainValueCount = 0;

   /* Insert header byte if just latched to Base256 */
   if(targetScheme == DmtxSchemeBase256)
   {
      UpdateBase256ChainHeader(stream, DmtxUndefined); CHKERR;
   }
}

/**
 * this code is separated from EncodeNextChunkAscii() because it needs to be called directly elsewhere
 *
 */
static void
EncodeValueAscii(DmtxEncodeStream *stream, DmtxByte value)
{
   CHKSCHEME(DmtxSchemeAscii);

   StreamOutputChainAppend(stream, value); CHKERR;
   stream->outputChainValueCount++;
}

/**
 *
 *
 */
static void
EncodeNextChunkAscii(DmtxEncodeStream *stream)
{
   DmtxBoolean v1set;
   DmtxByte v0, v1;

   if(StreamInputHasNext(stream))
   {
      v0 = StreamInputAdvanceNext(stream); CHKERR;

      if(StreamInputHasNext(stream))
      {
         v1 = StreamInputPeekNext(stream); CHKERR;
         v1set = DmtxTrue;
      }
      else
      {
         v1 = 0;
         v1set = DmtxFalse;
      }

      if(ISDIGIT(v0) && v1set && ISDIGIT(v1))
      {
         /* Two adjacent digit chars */
         StreamInputAdvanceNext(stream); CHKERR; /* Make the peek progress official */
         EncodeValueAscii(stream, 10 * (v0 - '0') + (v1 - '0') + 130); CHKERR;
      }
      else
      {
         if(v0 < 128)
         {
            /* Regular ASCII char */
            EncodeValueAscii(stream, v0 + 1); CHKERR;
         }
         else
         {
            /* Extended ASCII char */
            EncodeValueAscii(stream, DmtxValueAsciiUpperShift); CHKERR;
            EncodeValueAscii(stream, v0 - 127); CHKERR;
         }
      }
   }
}

/**
 *
 *
 */
static void
CompleteIfDoneAscii(DmtxEncodeStream *stream, int requestedSizeIdx)
{
   int sizeIdx;

   if(!StreamInputHasNext(stream))
   {
      sizeIdx = FindSymbolSize(stream->output.length, requestedSizeIdx); CHKSIZE;
      PadRemainingInAscii(stream, sizeIdx); CHKERR;
      StreamMarkComplete(stream, sizeIdx);
   }
}

/**
 *
 *
 */
static void
EncodeValuesCTX(DmtxEncodeStream *stream, DmtxByteList *valueList)
{
   int pairValue;
   DmtxByte cw0, cw1;

   if(stream->currentScheme != DmtxSchemeC40 &&
         stream->currentScheme != DmtxSchemeText &&
         stream->currentScheme != DmtxSchemeX12)
   {
      StreamMarkFatal(stream, 1);
      return;
   }

   if(valueList->length != 3)
   {
      StreamMarkFatal(stream, 1);
      return;
   }

   /* Build codewords from computed value */
   pairValue = (1600 * valueList->b[0]) + (40 * valueList->b[1]) + valueList->b[2] + 1;
   cw0 = pairValue / 256;
   cw1 = pairValue % 256;

   /* Append 2 codewords */
   StreamOutputChainAppend(stream, cw0); CHKERR;
   StreamOutputChainAppend(stream, cw1); CHKERR;

   /* Update count for 3 encoded values */
   stream->outputChainValueCount += 3;
}

/**
 *
 *
 */
static void
EncodeUnlatchCTX(DmtxEncodeStream *stream)
{
   if(stream->currentScheme != DmtxSchemeC40 &&
         stream->currentScheme != DmtxSchemeText &&
         stream->currentScheme != DmtxSchemeX12)
   {
      StreamMarkFatal(stream, 1);
      return;
   }

   /* Verify we are on byte boundary */
   if(stream->outputChainValueCount % 3 != 0)
   {
      StreamMarkInvalid(stream, 1 /* not on byte boundary */);
      return;
   }

   StreamOutputChainAppend(stream, DmtxValueCTXUnlatch); CHKERR;

   stream->outputChainValueCount++;
}

/**
 *
 *
 */
static void
EncodeNextChunkCTX(DmtxEncodeStream *stream, int requestedSizeIdx)
{
   DmtxPassFail passFail;
   DmtxByte inputValue;
   DmtxByte valueListStorage[4];
   DmtxByteList valueList = dmtxByteListBuild(valueListStorage, sizeof(valueListStorage));

   while(StreamInputHasNext(stream))
   {
      inputValue = StreamInputAdvanceNext(stream); CHKERR;
      /* XXX remember to account for upper shift (4 values each) */
      passFail = PushCTXValues(&valueList, inputValue, stream->currentScheme);

      /* remember to account for upper shift (4 values each) ... does this loop structure still work? */
      while(valueList.length >= 3)
      {
         EncodeValuesCTX(stream, &valueList); CHKERR;
/*       DmtxByteListRemoveFirst(valueList, 3); */
      }

      /* Finished on byte boundary -- done with current chunk */
      if(valueList.length == 0)
         break;
   }

   /*
    * Special case: If all input values have been consumed and 1 or 2 unwritten
    * C40/Text/X12 values remain, finish encoding the symbol in ASCII according
    * to the published end-of-symbol conditions.
    */
   if(!StreamInputHasNext(stream) && valueList.length > 0)
   {
      CompleteIfDonePartialCTX(stream, &valueList, requestedSizeIdx); CHKERR;
   }
}

/**
 * Complete C40/Text/X12 encoding if it matches a known end-of-symbol condition.
 *
 *   Term  Trip  Symbol  Codeword
 *   Cond  Size  Remain  Sequence
 *   ----  ----  ------  -----------------------
 *    (a)     3       2  Special case
 *            -       -  UNLATCH [PAD]
 */
static void
CompleteIfDoneCTX(DmtxEncodeStream *stream, int requestedSizeIdx)
{
   int sizeIdx;
   int symbolRemaining;

   sizeIdx = FindSymbolSize(stream->output.length, requestedSizeIdx); CHKSIZE;
   symbolRemaining = GetRemainingSymbolCapacity(stream->output.length, sizeIdx);

   if(!StreamInputHasNext(stream))
   {
      if(symbolRemaining == 0)
      {
         /* End of symbol condition (a) -- Perfect fit */
         StreamMarkComplete(stream, sizeIdx);
      }
      else
      {
         EncodeChangeScheme(stream, DmtxSchemeAscii, DmtxUnlatchExplicit); CHKERR;
      }
   }
}

/**
 * The remaining values can exist in 3 possible cases:
 *
 *   a) 1 C40/Text/X12 remaining == 1 data
 *   b) 2 C40/Text/X12 remaining == 1 shift + 1 data
 *   c) 2 C40/Text/X12 remaining == 1 data +  1 data
 *
 * To distinguish between cases (b) and (c), encode the final input value to
 * C40/Text/X12 in a temporary location and check the resulting length. If
 * it expands to multiple values it represents (b); otherwise it is (c). This
 * accounts for both shift and upper shift conditions.
 *
 * Note that in cases (a) and (c) the final C40/Text/X12 value encoded in the
 * previous chunk may have been a shift value, but this will be ignored by
 * the decoder due to the implicit shift to ASCII. <-- what if symbol is much
 * larger though?
 *
 *   Term    Value  Symbol  Codeword
 *   Cond    Count  Remain  Sequence
 *   ----  -------  ------  ------------------------
 *    (b)    C40 2       2  C40+C40+0
 *    (d)  ASCII 1       1  ASCII (implicit unlatch)
 *    (c)  ASCII 1       2  UNLATCH (continue ASCII)
 *               -       -  UNLATCH (continue ASCII)
 */
static void
CompleteIfDonePartialCTX(DmtxEncodeStream *stream, DmtxByteList *valueList, int requestedSizeIdx)
{
   int sizeIdx;
   int symbolRemaining;

   /* replace this later */
   if(stream->currentScheme != DmtxSchemeC40 &&
         stream->currentScheme != DmtxSchemeText &&
         stream->currentScheme != DmtxSchemeX12)
   {
      StreamMarkFatal(stream, 1);
      return;
   }

   /* Should have exactly one or two input values left */
   assert(valueList->length == 1 || valueList->length == 2);

   sizeIdx = FindSymbolSize(stream->output.length, requestedSizeIdx); CHKSIZE;
   symbolRemaining = GetRemainingSymbolCapacity(stream->output.length, sizeIdx);

   if(valueList->length == 2 && symbolRemaining == 2)
   {
      /* End of symbol condition (b) -- Use Shift1 to pad final list value */
      dmtxByteListPush(valueList, DmtxValueCTXShift1);
      EncodeValuesCTX(stream, valueList); CHKERR;
      StreamMarkComplete(stream, sizeIdx);
   }
   else
   {
      /*
       * Rollback progress of previously consumed input value(s) since ASCII
       * encoder will be used to finish the symbol. 2 rollbacks are needed if
       * valueList holds 2 data words (i.e., not shift or upper shifts).
       */
/*
      StreamInputAdvancePrev(stream); CHKERR;

      // temporary re-encode most recently consumed input value to C40/Text/X12
      passFail = PushCTXValues(&tmp, inputValue));
      if(valueList.length == 2 && tmp.length > 1)
      {
         StreamInputAdvancePrev(stream); CHKERR;
      }

      ascii = encodeTmpRemainingToAscii(stream);
      if(ascii.length == 1 && symbolRemaining == 1)
      {
         // End of symbol condition (d)
         changeScheme(stream, DmtxSchemeAscii, DmtxUnlatchImplicit); CHKERR;
         EncodeValueAscii(stream, ascii.b[0]); CHKERR;
         StreamMarkComplete(stream, sizeIdx);
      }
      else
      {
         // Continue in ASCII (c)
         changeScheme(stream, DmtxSchemeAscii, DmtxUnlatchExplicit); CHKERR;
      }
*/
   }
}

/**
 * @brief  Convert 3 input values into 2 codewords for triplet-based schemes
 * @param  outputWords
 * @param  inputWord
 * @param  encScheme
 * @return Codeword count
 */
static DmtxPassFail
PushCTXValues(DmtxByteList *valueList, int inputValue, int targetScheme)
{
   /* Handle extended ASCII with Upper Shift character */
   if(inputValue > 127)
   {
      if(targetScheme == DmtxSchemeX12)
      {
         return 0; /* XXX shouldn't this be an error? */
      }
      else
      {
         dmtxByteListPush(valueList, DmtxValueCTXShift2);
         dmtxByteListPush(valueList, 30);
         inputValue -= 128;
      }
   }

   /* Handle all other characters according to encodation scheme */
   if(targetScheme == DmtxSchemeX12)
   {
      if(inputValue == 13)
         dmtxByteListPush(valueList, 0);
      else if(inputValue == 42)
         dmtxByteListPush(valueList, 1);
      else if(inputValue == 62)
         dmtxByteListPush(valueList, 2);
      else if(inputValue == 32)
         dmtxByteListPush(valueList, 3);
      else if(inputValue >= 48 && inputValue <= 57)
         dmtxByteListPush(valueList, inputValue - 44);
      else if(inputValue >= 65 && inputValue <= 90)
         dmtxByteListPush(valueList, inputValue - 51);
   }
   else
   {
      /* targetScheme is C40 or Text */
      if(inputValue <= 31)
      {
         dmtxByteListPush(valueList, DmtxValueCTXShift1);
         dmtxByteListPush(valueList, inputValue);
      }
      else if(inputValue == 32)
      {
         dmtxByteListPush(valueList, 3);
      }
      else if(inputValue <= 47)
      {
         dmtxByteListPush(valueList, DmtxValueCTXShift2);
         dmtxByteListPush(valueList, inputValue - 33);
      }
      else if(inputValue <= 57)
      {
         dmtxByteListPush(valueList, inputValue - 44);
      }
      else if(inputValue <= 64)
      {
         dmtxByteListPush(valueList, DmtxValueCTXShift2);
         dmtxByteListPush(valueList, inputValue - 43);
      }
      else if(inputValue <= 90 && targetScheme == DmtxSchemeC40)
      {
         dmtxByteListPush(valueList, inputValue - 51);
      }
      else if(inputValue <= 90 && targetScheme == DmtxSchemeText)
      {
         dmtxByteListPush(valueList, DmtxValueCTXShift3);
         dmtxByteListPush(valueList, inputValue - 64);
      }
      else if(inputValue <= 95)
      {
         dmtxByteListPush(valueList, DmtxValueCTXShift2);
         dmtxByteListPush(valueList, inputValue - 69);
      }
      else if(inputValue == 96 && targetScheme == DmtxSchemeText)
      {
         dmtxByteListPush(valueList, DmtxValueCTXShift3);
         dmtxByteListPush(valueList, 0);
      }
      else if(inputValue <= 122 && targetScheme == DmtxSchemeText)
      {
         dmtxByteListPush(valueList, inputValue - 83);
      }
      else if(inputValue <= 127)
      {
         dmtxByteListPush(valueList, DmtxValueCTXShift3);
         dmtxByteListPush(valueList, inputValue - 96);
      }
   }

   return DmtxPass;
}

/**
 *
 *
 */
static void
EncodeValueEdifact(DmtxEncodeStream *stream, DmtxByte value)
{
   DmtxByte edifactValue, previousOutput;

   CHKSCHEME(DmtxSchemeEdifact);

   if(value < 31 || value > 94)
   {
      StreamMarkInvalid(stream, DmtxChannelUnsupportedChar);
      return;
   }

   edifactValue = (value & 0x3f) << 2;

   switch(stream->outputChainValueCount % 4)
   {
      case 0:
         StreamOutputChainAppend(stream, edifactValue); CHKERR;
         break;
      case 1:
         previousOutput = StreamOutputChainRemoveLast(stream); CHKERR;
         StreamOutputChainAppend(stream, previousOutput | (edifactValue >> 6)); CHKERR;
         StreamOutputChainAppend(stream, edifactValue << 2); CHKERR;
         break;
      case 2:
         previousOutput = StreamOutputChainRemoveLast(stream); CHKERR;
         StreamOutputChainAppend(stream, previousOutput | (edifactValue >> 4)); CHKERR;
         StreamOutputChainAppend(stream, edifactValue << 4); CHKERR;
         break;
      case 3:
         previousOutput = StreamOutputChainRemoveLast(stream); CHKERR;
         StreamOutputChainAppend(stream, previousOutput | (edifactValue >> 2)); CHKERR;
         break;
   }

   stream->outputChainValueCount++;
}

/**
 *
 *
 */
static void
EncodeNextChunkEdifact(DmtxEncodeStream *stream)
{
   DmtxByte value;

   if(StreamInputHasNext(stream))
   {
      value = StreamInputAdvanceNext(stream); CHKERR;
      EncodeValueEdifact(stream, value); CHKERR;
   }
}

/**
 * Complete EDIFACT encoding if it matches a known end-of-symbol condition.
 *
 *   Term  Clean  Symbol  ASCII   Codeword
 *   Cond  Bound  Remain  Remain  Sequence
 *   ----  -----  ------  ------  -----------
 *    (a)      Y       0       0  [none]
 *    (b)      Y       1       0  PAD
 *    (c)      Y       1       1  ASCII
 *    (d)      Y       2       0  PAD PAD
 *    (e)      Y       2       1  ASCII PAD
 *    (f)      Y       2       2  ASCII ASCII
 *             -       -       0  UNLATCH
 *
 * If not matching any of the above, continue without doing anything.
 */
static void
CompleteIfDoneEdifact(DmtxEncodeStream *stream, int requestedSizeIdx)
{
   int i;
   int sizeIdx;
   int symbolRemaining;
   DmtxBoolean cleanBoundary;
   DmtxPassFail passFail;
   DmtxByte outputTmpStorage[3];
   DmtxByteList outputTmp;

   /* Check if sitting on a clean byte boundary */
   cleanBoundary = (stream->outputChainValueCount % 4 == 0) ? DmtxTrue : DmtxFalse;

   /* Find symbol's remaining capacity based on current length */
   sizeIdx = FindSymbolSize(stream->output.length, requestedSizeIdx); CHKSIZE;
   symbolRemaining = GetRemainingSymbolCapacity(stream->output.length, sizeIdx); CHKERR;

   if(!StreamInputHasNext(stream))
   {
      /* Explicit unlatch required unless on clean boundary and full symbol */
      if(cleanBoundary == DmtxFalse || symbolRemaining > 0)
      {
         EncodeChangeScheme(stream, DmtxSchemeAscii, DmtxUnlatchExplicit); CHKERR;
         sizeIdx = FindSymbolSize(stream->output.length, requestedSizeIdx); CHKSIZE;
         PadRemainingInAscii(stream, sizeIdx); CHKERR;
      }

      StreamMarkComplete(stream, sizeIdx);
   }
   else
   {
      /*
       * Allow encoder to write up to 3 additional codewords to a temporary
       * stream. If it finishes in 1 or 2 it is a known end-of-symbol condition.
       */
      outputTmp = EncodeTmpRemainingInAscii(stream, outputTmpStorage,
            sizeof(outputTmpStorage), &passFail);

      if(passFail == DmtxFail || outputTmp.length > symbolRemaining)
         return; /* Doesn't fit, continue encoding */

      if(cleanBoundary && (outputTmp.length == 1 || outputTmp.length == 2))
      {
         EncodeChangeScheme(stream, DmtxSchemeAscii, DmtxUnlatchImplicit); CHKERR;

         for(i = 0; i < outputTmp.length; i++)
         {
            EncodeValueAscii(stream, outputTmp.b[i]); CHKERR;
         }
         /* Register input progress since we encoded outside normal stream */
         stream->inputNext = stream->input.length;

         /* Pad remaining (if necessary) */
         PadRemainingInAscii(stream, sizeIdx); CHKERR;

         StreamMarkComplete(stream, sizeIdx);
      }
   }
}

/**
 *
 *
 */
static void
UpdateBase256ChainHeader(DmtxEncodeStream *stream, int perfectSizeIdx)
{
   int headerIndex;
   int outputLength;
   int headerByteCount;
   int symbolDataWords;
   DmtxByte headerValue0;
   DmtxByte headerValue1;

   headerIndex = stream->output.length - stream->outputChainWordCount;
   outputLength = stream->outputChainValueCount;
   headerByteCount = stream->outputChainWordCount - stream->outputChainValueCount;

   /*
    * Adjust header to hold correct number of bytes (not worrying about the
    * values store there until below). Note: Header bytes are not considered
    * scheme "values" so we can insert or remove them without needing to update
    * outputChainValueCount.
    */

   if(headerByteCount == 0 && stream->outputChainWordCount == 0)
   {
      /* No output words written yet -- insert single header byte */
      StreamOutputChainAppend(stream, 0); CHKERR;
      headerByteCount++;
   }
   else if(headerByteCount == 1 && outputLength > 249)
   {
      /* Beyond 249 bytes requires a second header byte */
      StreamOutputChainInsertFirst(stream); CHKERR; /* XXX just a stub right now */
      headerByteCount++;
   }
   else if(headerByteCount == 2 && perfectSizeIdx != DmtxUndefined)
   {
      /* Encoding to exact end of symbol only requires single byte */
      StreamOutputChainRemoveFirst(stream); CHKERR; /* XXX just a stub right now */
      headerByteCount--;
   }

   /*
    * Encode header byte(s) with current length
    */

   if(headerByteCount == 1 && perfectSizeIdx != DmtxUndefined)
   {
      /* XXX replace magic value 0 with DmtxValueBase256EncodeToEnd or something? */
      headerValue0 = Randomize255State2(0, headerIndex + 1);

      /* Verify output length matches exact caapacity of perfectSizeIdx */
      symbolDataWords = dmtxGetSymbolAttribute(DmtxSymAttribSymbolDataWords, perfectSizeIdx);
      if(symbolDataWords != stream->output.length)
      {
         StreamMarkFatal(stream, 1);
         return;
      }

      StreamOutputSet(stream, headerIndex, headerValue0); CHKERR;
   }
   else if(headerByteCount == 1 && perfectSizeIdx == DmtxUndefined)
   {
      headerValue0 = Randomize255State2(outputLength, headerIndex + 1);
      StreamOutputSet(stream, headerIndex, headerValue0); CHKERR;
   }
   else if(headerByteCount == 2 && perfectSizeIdx == DmtxUndefined)
   {
      headerValue0 = Randomize255State2(outputLength/250 + 249, headerIndex + 1);
      StreamOutputSet(stream, headerIndex, headerValue0); CHKERR;

      headerValue1 = Randomize255State2(outputLength%250, headerIndex + 2);
      StreamOutputSet(stream, headerIndex + 1, headerValue1); CHKERR;
   }
   else
   {
      StreamMarkFatal(stream, 1);
   }
}

/**
 *
 *
 */
static void
EncodeValueBase256(DmtxEncodeStream *stream, DmtxByte value)
{
   CHKSCHEME(DmtxSchemeBase256);

   StreamOutputChainAppend(stream, Randomize255State2(value, stream->output.length + 1)); CHKERR;
   stream->outputChainValueCount++;

   UpdateBase256ChainHeader(stream, DmtxUndefined); CHKERR;
}

/**
 *
 *
 */
static void
EncodeNextChunkBase256(DmtxEncodeStream *stream)
{
   DmtxByte value;

   if(StreamInputHasNext(stream))
   {
      value = StreamInputAdvanceNext(stream); CHKERR;
      EncodeValueBase256(stream, value); CHKERR;
   }
}

/**
 * check remaining symbol capacity and remaining codewords
 * if the chain can finish perfectly at the end of symbol data words there is a
 * special one-byte length header value that can be used (i think ... read the
 * spec again before commiting to anything)
 */
static void
CompleteIfDoneBase256(DmtxEncodeStream *stream, int requestedSizeIdx)
{
   int sizeIdx;
   int headerByteCount, outputLength, symbolRemaining;

   if(!StreamInputHasNext(stream))
   {
      headerByteCount = stream->outputChainWordCount - stream->outputChainValueCount;
      assert(headerByteCount == 1 || headerByteCount == 2);

      /* Check for special case where every symbol word is used */
      if(headerByteCount == 2)
      {
         /* Find symbol size as if headerByteCount was only 1 */
         outputLength = stream->output.length - 1;
         sizeIdx = FindSymbolSize(outputLength, requestedSizeIdx); /* No CHKSIZE */
         if(sizeIdx != DmtxUndefined)
         {
            symbolRemaining = GetRemainingSymbolCapacity(outputLength, sizeIdx);

            if(symbolRemaining == 0)
            {
               /* Perfect fit -- complete encoding */
               UpdateBase256ChainHeader(stream, sizeIdx); CHKERR;
               StreamMarkComplete(stream, sizeIdx);
               return;
            }
         }
      }

      /* Normal case */
      sizeIdx = FindSymbolSize(stream->output.length, requestedSizeIdx); CHKSIZE;
      EncodeChangeScheme(stream, DmtxSchemeAscii, DmtxUnlatchImplicit);
      PadRemainingInAscii(stream, sizeIdx);
      StreamMarkComplete(stream, sizeIdx);
   }
}

/**
 * \brief  Randomize 253 state
 * \param  codewordValue
 * \param  codewordPosition
 * \return Randomized value
 */
static DmtxByte
Randomize253State2(DmtxByte cwValue, int cwPosition)
{
   int pseudoRandom, tmp;

   pseudoRandom = ((149 * cwPosition) % 253) + 1;
   tmp = cwValue + pseudoRandom;
   if(tmp > 254)
      tmp -= 254;

   assert(tmp >= 0 && tmp < 256);

   return (DmtxByte)tmp;
}

/**
 * \brief  Randomize 255 state
 * \param  value
 * \param  position
 * \return Randomized value
 */
static DmtxByte
Randomize255State2(DmtxByte value, int position)
{
   int pseudoRandom, tmp;

   pseudoRandom = ((149 * position) % 255) + 1;
   tmp = value + pseudoRandom;

   return (tmp <= 255) ? tmp : tmp - 256;
}

/**
 *
 *
 */
static int
GetRemainingSymbolCapacity(int outputLength, int sizeIdx)
{
   int capacity;
   int remaining;

   if(sizeIdx == DmtxUndefined)
   {
      remaining = DmtxUndefined;
   }
   else
   {
      capacity = dmtxGetSymbolAttribute(DmtxSymAttribSymbolDataWords, sizeIdx);
      remaining = capacity - outputLength;
   }

   return remaining;
}

/**
 * Can we just receive a length to pad here? I don't like receiving
 * requestedSizeIdx (or sizeIdx) this late in the game
 *
 */
static void
PadRemainingInAscii(DmtxEncodeStream *stream, int sizeIdx)
{
   int symbolRemaining;
   DmtxByte padValue;

   CHKSCHEME(DmtxSchemeAscii);
   CHKSIZE;

   symbolRemaining = GetRemainingSymbolCapacity(stream->output.length, sizeIdx);

   /* First pad character is not randomized */
   if(symbolRemaining > 0)
   {
      padValue = DmtxValueAsciiPad;
      StreamOutputChainAppend(stream, padValue); CHKERR;
      symbolRemaining--;
   }

   /* All remaining pad characters are randomized based on character position */
   while(symbolRemaining > 0)
   {
      padValue = Randomize253State2(DmtxValueAsciiPad, stream->output.length + 1);
      StreamOutputChainAppend(stream, padValue); CHKERR;
      symbolRemaining--;
   }
}

/**
 *
 *
 */
static DmtxByteList
EncodeTmpRemainingInAscii(DmtxEncodeStream *stream, DmtxByte *storage, int capacity, DmtxPassFail *passFail)
{
   DmtxEncodeStream streamAscii;

   /* Create temporary copy of stream that writes to storage */
   streamAscii = *stream;
   streamAscii.currentScheme = DmtxSchemeAscii;
   streamAscii.outputChainValueCount = 0;
   streamAscii.outputChainWordCount = 0;
   streamAscii.reason = DmtxUndefined;
   streamAscii.sizeIdx = DmtxUndefined;
   streamAscii.status = DmtxStatusEncoding;
   streamAscii.output = dmtxByteListBuild(storage, capacity);

   while(dmtxByteListHasCapacity(&(streamAscii.output)))
   {
      /* Do not call CHKERR here because we don't want to return */
      if(StreamInputHasNext(&streamAscii))
         EncodeNextChunkAscii(&streamAscii);
      else
         break;
   }

   /*
    * We stopped encoding before attempting to write beyond output boundary so
    * any stream errors are truly unexpected. The passFail status indicates
    * whether output.length can be trusted by the calling function.
    */

   *passFail = (streamAscii.status == DmtxStatusEncoding) ? DmtxPass : DmtxFail;

   return streamAscii.output;
}