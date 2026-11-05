#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "Cafe/HW/MMU/MMU.h"
#include "config/ActiveSettings.h"
#include <algorithm>
#include <cstdio>

void mic_updateOnAXFrame();

namespace snd_core
{
	uint32 __AXDefaultMixerSelect = AX_MIXER_SELECT_BOTH;

	uint16 __AXTVAuxReturnVolume[AX_AUX_BUS_COUNT];

	void AXSetDefaultMixerSelect(uint32 mixerSelect)
	{
		__AXDefaultMixerSelect = mixerSelect;
	}

	uint32 AXGetDefaultMixerSelect()
	{
		return __AXDefaultMixerSelect;
	}

	void AXMix_Init()
	{
		AXSetDefaultMixerSelect(AX_MIXER_SELECT_PPC);
	}

	void AXMix_DepopVoice(AXVPBInternal_t* internalShadowCopy)
	{
		// todo
	}

#define handleAdpcmDecodeLoop() \
	if (internalShadowCopy->internalOffsets.loopFlag != 0) \
	{	\
		scale = _swapEndianU16(internalShadowCopy->adpcmLoop.loopScale); \
		delta = 1 << (scale & 0xF); \
		coefIndex = (scale >> 4) & 7; \
		coefA = (sint32)(sint16)_swapEndianU16(internalShadowCopy->adpcmData.coef[coefIndex * 2 + 0]); \
		coefB = (sint32)(sint16)_swapEndianU16(internalShadowCopy->adpcmData.coef[coefIndex * 2 + 1]); \
		playbackNibbleOffset = vpbLoopOffset; \
	} \
	else \
	{ \
		/* no loop */ \
		internalShadowCopy->playbackState = 0; \
		memset(outputWriter, 0, sampleCount * sizeof(sint16)); \
		break; \
	}

	uint32 _AX_fixAdpcmEndOffset(uint32 adpcmOffset)
	{
		// How to Survive uses an end offset of 0x40000 which is not a valid adpcm sample offset and thus can never be reached (the ppc side decoder jumps from 0x3FFFF to 0x40002)
		// todo - investigate if the DSP decoder routines can handle invalid ADPCM end offsets

		// for now we use this workaround to force a valid address
		if ((adpcmOffset & 0xF) <= 1)
			return adpcmOffset + 2;
		return adpcmOffset;
	}

	void AX_readADPCMSamples(AXVPBInternal_t* internalShadowCopy, sint16* output, sint32 sampleCount)
	{
		if (internalShadowCopy->playbackState == 0)
		{
			memset(output, 0, sampleCount * sizeof(sint16));
			return;
		}

		AXVPB* vpb = __AXVPBArrayPtr + (sint32)internalShadowCopy->index;

		uint8* sampleBase = (uint8*)memory_getPointerFromVirtualOffset(_swapEndianU32(vpb->offsets.samples));

		uint32 vpbLoopOffset = _swapEndianU32(*(uint32*)&vpb->offsets.loopOffset);
		uint32 vpbEndOffset = _swapEndianU32(*(uint32*)&vpb->offsets.endOffset);
		
		vpbEndOffset = _AX_fixAdpcmEndOffset(vpbEndOffset);

		uint32 internalCurrentOffset = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.currentOffsetPtrHigh);
		uint32 internalLoopOffset = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.loopOffsetPtrHigh);

		sint32 scale = (sint32)_swapEndianU16(internalShadowCopy->adpcmData.scale);
		sint32 delta = 1 << (scale & 0xF);
		sint32 coefIndex = (scale >> 4) & 7;

		sint32 coefA = (sint32)(sint16)_swapEndianU16(internalShadowCopy->adpcmData.coef[coefIndex * 2 + 0]);
		sint32 coefB = (sint32)(sint16)_swapEndianU16(internalShadowCopy->adpcmData.coef[coefIndex * 2 + 1]);

		sint32 hist0 = (sint32)_swapEndianS16(internalShadowCopy->adpcmData.yn1);
		sint32 hist1 = (sint32)_swapEndianS16(internalShadowCopy->adpcmData.yn2);

		uint32 playbackNibbleOffset = vpbLoopOffset + (internalCurrentOffset - internalLoopOffset);
		uint32 playbackNibbleOffsetStart = playbackNibbleOffset;

		sint16* outputWriter = output;
		// decode samples from current partial ADPCM block
		while (sampleCount && (playbackNibbleOffset & 0xF))
		{
			sint8 nibble = (sint8)sampleBase[(sint32)playbackNibbleOffset >> 1];
			nibble <<= (4 * ((playbackNibbleOffset & 1)));
			nibble &= 0xF0;
			sint32 v = (sint32)nibble;
			v <<= (11 - 4);
			v *= delta;
			v += (hist0 * coefA + hist1 * coefB);
			v = (v + 0x400) >> 11;
			v = std::clamp(v, -32768, 32767);

			hist1 = hist0;
			hist0 = v;
			*outputWriter = v;
			outputWriter++;

			// check for loop / end offset
			if (playbackNibbleOffset == vpbEndOffset)
			{
				handleAdpcmDecodeLoop();
			}
			else
			{
				playbackNibbleOffset++;
			}
			sampleCount--;
		}
		// optimized code to decode whole blocks
		if (!(playbackNibbleOffset <= vpbEndOffset && ((uint64)playbackNibbleOffset + (uint64)(sampleCount * 16 / 14)) >= (uint64)vpbEndOffset))
		{
			while (sampleCount >= 14) // 14 samples per 16 byte block
			{
				// decode header
				sint8* sampleInputData = (sint8*)sampleBase + ((sint32)playbackNibbleOffset >> 1);
				scale = (uint8)sampleInputData[0];
				delta = 1 << (scale & 0xF);
				coefIndex = (scale >> 4) & 7;
				coefA = (sint32)(sint16)_swapEndianU16(internalShadowCopy->adpcmData.coef[coefIndex * 2 + 0]);
				coefB = (sint32)(sint16)_swapEndianU16(internalShadowCopy->adpcmData.coef[coefIndex * 2 + 1]);
				playbackNibbleOffset += 2;
				// decode samples
				sampleInputData++;
				for (sint32 i = 0; i < 7; i++)
				{
					// n0
					sint8 nibble = *sampleInputData;
					sampleInputData++;
					sint32 v;
					// upper nibble
					v = (sint32)(sint8)(nibble & 0xF0);
					v <<= (11 - 4);
					v *= delta;
					v += (hist0 * coefA + hist1 * coefB);
					v = (v + 0x400) >> 11;

					v = std::min(v, 32767);
					v = std::max(v, -32768);

					hist1 = hist0;
					hist0 = v;
					*outputWriter = v;
					outputWriter++;
					// lower nibble
					v = (sint32)(sint8)(nibble << 4);
					v <<= (11 - 4);
					v *= delta;
					v += (hist0 * coefA + hist1 * coefB);
					v = (v + 0x400) >> 11;

					v = std::min(v, 32767);
					v = std::max(v, -32768);

					hist1 = hist0;
					hist0 = v;

					*outputWriter = v;
					outputWriter++;
				}
				playbackNibbleOffset += 7 * 2;
				sampleCount -= 7 * 2;
			}
		}
		// decode remaining samples
		while (sampleCount)
		{
			if ((playbackNibbleOffset & 0xF) == 0)
			{
				scale = sampleBase[(sint32)playbackNibbleOffset >> 1];
				delta = 1 << (scale & 0xF);
				coefIndex = (scale >> 4) & 7;
				coefA = (sint32)(sint16)_swapEndianU16(internalShadowCopy->adpcmData.coef[coefIndex * 2 + 0]);
				coefB = (sint32)(sint16)_swapEndianU16(internalShadowCopy->adpcmData.coef[coefIndex * 2 + 1]);
				playbackNibbleOffset += 2;
			}
			sint8 nibble = (sint8)sampleBase[(sint32)playbackNibbleOffset >> 1];
			nibble <<= (4 * ((playbackNibbleOffset & 1)));
			nibble &= 0xF0;
			sint32 v = (sint32)nibble;
			v <<= (11 - 4);
			v *= delta;
			v += (hist0 * coefA + hist1 * coefB);
			v = (v + 0x400) >> 11;

			v = std::min(v, 32767);
			v = std::max(v, -32768);

			hist1 = hist0;
			hist0 = v;
			*outputWriter = v;
			outputWriter++;

			// check for loop / end offset
			if (playbackNibbleOffset == vpbEndOffset)
			{
				handleAdpcmDecodeLoop();
			}
			else
			{
				playbackNibbleOffset++;
			}
			sampleCount--;
		}

		// write updated values
		internalShadowCopy->adpcmData.scale = _swapEndianU16(scale);
		internalShadowCopy->adpcmData.yn1 = (uint16)_swapEndianS16(hist0);
		internalShadowCopy->adpcmData.yn2 = (uint16)_swapEndianS16(hist1);

		uint32 newInternalCurrentOffset = internalCurrentOffset + (playbackNibbleOffset - playbackNibbleOffsetStart);
		*(uint32*)&internalShadowCopy->internalOffsets.currentOffsetPtrHigh = _swapEndianU32(newInternalCurrentOffset);

	}

	void AX_DecodeSamplesADPCM_NoSrc(AXVPBInternal_t* internalShadowCopy, float* output, sint32 sampleCount)
	{
		sint16 sampleBuffer[1024];
		cemu_assert(sampleCount <= (sizeof(sampleBuffer) / sizeof(sampleBuffer[0])));
		AX_readADPCMSamples(internalShadowCopy, sampleBuffer, sampleCount);
		for (sint32 i = 0; i < sampleCount; i++)
		{
			// decode next sample
			sint32 s = sampleBuffer[i];
			s <<= 8;
			*output = (float)s;
			output++;
		}
	}

	void AX_DecodeSamplesADPCM_Linear(AXVPBInternal_t* internalShadowCopy, float* output, sint32 sampleCount)
	{
		uint32 currentFracPos = (uint32)_swapEndianU16(internalShadowCopy->src.currentFrac);
		uint32 ratio = _swapEndianU32(*(uint32*)&internalShadowCopy->src.ratioHigh);

		sint16 historySamples[4];
		historySamples[0] = _swapEndianS16(internalShadowCopy->src.historySamples[0]);
		historySamples[1] = _swapEndianS16(internalShadowCopy->src.historySamples[1]);
		historySamples[2] = _swapEndianS16(internalShadowCopy->src.historySamples[2]);
		historySamples[3] = _swapEndianS16(internalShadowCopy->src.historySamples[3]);

		sint32 historyIndex = 0;

		sint32 numberOfDecodedAdpcmSamples = (sint32)((currentFracPos + ratio * sampleCount) >> 16);
		sint16 adpcmSampleBuffer[4096];
		if (numberOfDecodedAdpcmSamples >= 4096)
		{
			memset(output, 0, sizeof(float)*sampleCount);
			cemuLog_log(LogType::Force, "Too many ADPCM samples to decode. ratio = {:08x}", ratio);
			return;
		}
		AX_readADPCMSamples(internalShadowCopy, adpcmSampleBuffer, numberOfDecodedAdpcmSamples);

		sint32 readSampleCount = 0;

		if (ratio == 0x10000 && currentFracPos == 0)
		{
			// optimized path when accessing only fully aligned samples
			for (sint32 i = 0; i < sampleCount; i++)
			{
				cemu_assert_debug(readSampleCount < numberOfDecodedAdpcmSamples);
				// get next sample
				sint16 s = adpcmSampleBuffer[readSampleCount];
				readSampleCount++;
				historyIndex = (historyIndex + 1) & 3;
				historySamples[historyIndex] = s;
				// use previous sample
				sint32 previousSample = historySamples[(historyIndex + 3) & 3];
				sint32 p0 = previousSample << 8;
				*output = (float)p0;
				output++;
			}
		}
		else
		{
			for (sint32 i = 0; i < sampleCount; i++)
			{
				// move playback pos
				currentFracPos += ratio;
				// get more samples if needed
				while (currentFracPos >= 0x10000)
				{
					cemu_assert_debug(readSampleCount < numberOfDecodedAdpcmSamples);
					sint16 s = adpcmSampleBuffer[readSampleCount];
					readSampleCount++;
					currentFracPos -= 0x10000;
					historyIndex = (historyIndex + 1) & 3;
					historySamples[historyIndex] = s;
				}
				// linear interpolation of current sample
				sint32 previousSample = historySamples[(historyIndex + 3) & 3];
				sint32 nextSample = historySamples[historyIndex];

				sint32 p0 = (sint32)previousSample * (sint32)(0x10000 - currentFracPos);
				sint32 p1 = (sint32)nextSample * (sint32)(currentFracPos);

				p0 >>= 7;
				p1 >>= 7;

				sint32 interpolatedSample = p0 + p1;
				interpolatedSample >>= 1;

				*output = (float)interpolatedSample;
				output++;
			}
		}
		cemu_assert_debug(readSampleCount == numberOfDecodedAdpcmSamples);
		// set variables
		internalShadowCopy->src.currentFrac = _swapEndianU16((uint16)(currentFracPos));
		internalShadowCopy->src.historySamples[0] = _swapEndianS16(historySamples[(historyIndex + 0) & 3]);
		internalShadowCopy->src.historySamples[1] = _swapEndianS16(historySamples[(historyIndex + 1) & 3]);
		internalShadowCopy->src.historySamples[2] = _swapEndianS16(historySamples[(historyIndex + 2) & 3]);
		internalShadowCopy->src.historySamples[3] = _swapEndianS16(historySamples[(historyIndex + 3) & 3]);
	}

	void AX_DecodeSamplesADPCM_Tap(AXVPBInternal_t* internalShadowCopy, float* output, sint32 sampleCount)
	{
		// todo - implement this
		AX_DecodeSamplesADPCM_Linear(internalShadowCopy, output, sampleCount);
	}

	void AX_DecodeSamplesPCM8_Linear(AXVPBInternal_t* internalShadowCopy, float* output, sint32 sampleCount)
	{
		// get variables
		uint32 currentFracPos = (uint32)_swapEndianU16(internalShadowCopy->src.currentFrac);
		uint32 ratio = _swapEndianU32(*(uint32*)&internalShadowCopy->src.ratioHigh);

		uint32 endOffsetPtr = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.endOffsetPtrHigh);
		uint32 currentOffsetPtr = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.currentOffsetPtrHigh);
		uint32 loopOffsetPtr = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.loopOffsetPtrHigh);
		uint32 ptrHighExtension = _swapEndianU16(internalShadowCopy->internalOffsets.ptrHighExtension);

		uint8* endOffsetAddr = memory_base + (endOffsetPtr | (ptrHighExtension << 29));
		uint8* currentOffsetAddr = memory_base + (currentOffsetPtr | (ptrHighExtension << 29));

		sint16 historySamples[4];
		historySamples[0] = _swapEndianS16(internalShadowCopy->src.historySamples[0]);
		historySamples[1] = _swapEndianS16(internalShadowCopy->src.historySamples[1]);
		historySamples[2] = _swapEndianS16(internalShadowCopy->src.historySamples[2]);
		historySamples[3] = _swapEndianS16(internalShadowCopy->src.historySamples[3]);

		sint32 historyIndex = 0;

		for (sint32 i = 0; i<sampleCount; i++)
		{
			currentFracPos += ratio;
			while (currentFracPos >= 0x10000)
			{
				// read next sample
				historyIndex = (historyIndex + 1) & 3;
				if (internalShadowCopy->playbackState)
				{
					sint32 s = (sint32)(sint8)*currentOffsetAddr;
					s <<= 8;
					historySamples[historyIndex] = s;
					if (currentOffsetAddr == endOffsetAddr)
					{
						if (internalShadowCopy->internalOffsets.loopFlag)
						{
							// loop
							currentOffsetAddr = memory_base + (loopOffsetPtr | (ptrHighExtension << 29));
						}
						else
						{
							// stop playing
							internalShadowCopy->playbackState = 0;
						}
					}
					else
					{
						currentOffsetAddr++;
					}
				}
				else
				{
					// voice not playing, read sample as 0
					historySamples[historyIndex] = 0;
				}
				currentFracPos -= 0x10000;
			}
			// interpolate sample
			sint32 previousSample = historySamples[(historyIndex + 3) & 3];
			sint32 nextSample = historySamples[historyIndex];
			sint32 p0 = (sint32)previousSample * (sint32)(0x10000 - currentFracPos);
			sint32 p1 = (sint32)nextSample * (sint32)(currentFracPos);
			p0 >>= 7;
			p1 >>= 7;
			sint32 interpolatedSample = p0 + p1;
			interpolatedSample >>= 1;
			*output = (float)interpolatedSample;
			output++;
		}
		// set variables
		internalShadowCopy->src.currentFrac = _swapEndianU16((uint16)(currentFracPos));
		internalShadowCopy->src.historySamples[0] = _swapEndianS16(historySamples[(historyIndex + 0) & 3]);
		internalShadowCopy->src.historySamples[1] = _swapEndianS16(historySamples[(historyIndex + 1) & 3]);
		internalShadowCopy->src.historySamples[2] = _swapEndianS16(historySamples[(historyIndex + 2) & 3]);
		internalShadowCopy->src.historySamples[3] = _swapEndianS16(historySamples[(historyIndex + 3) & 3]);
		// store current offset
		currentOffsetPtr = (uint32)((uint8*)currentOffsetAddr - memory_base);
		currentOffsetPtr &= 0x1FFFFFFF; // is this correct?
		*(uint32*)&internalShadowCopy->internalOffsets.currentOffsetPtrHigh = _swapEndianU32(currentOffsetPtr);
	}

	void AX_DecodeSamplesPCM8_Tap(AXVPBInternal_t* internalShadowCopy, float* output, sint32 sampleCount)
	{
		// todo - implement this
		AX_DecodeSamplesPCM8_Linear(internalShadowCopy, output, sampleCount);
	}

	void AX_DecodeSamplesPCM8_NoSrc(AXVPBInternal_t* internalShadowCopy, float* output, sint32 sampleCount)
	{
		// get variables
		uint32 currentFracPos = (uint32)_swapEndianU16(internalShadowCopy->src.currentFrac);
		uint32 ratio = _swapEndianU32(*(uint32*)&internalShadowCopy->src.ratioHigh);

		uint32 endOffsetPtr = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.endOffsetPtrHigh);
		uint32 currentOffsetPtr = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.currentOffsetPtrHigh);
		uint32 ptrHighExtension = _swapEndianU16(internalShadowCopy->internalOffsets.ptrHighExtension);

		uint8* endOffsetAddr = memory_base + (endOffsetPtr | (ptrHighExtension << 29));
		uint8* currentOffsetAddr = memory_base + (currentOffsetPtr | (ptrHighExtension << 29));

		sint16 historySamples[4];
		historySamples[0] = _swapEndianS16(internalShadowCopy->src.historySamples[0]);
		historySamples[1] = _swapEndianS16(internalShadowCopy->src.historySamples[1]);
		historySamples[2] = _swapEndianS16(internalShadowCopy->src.historySamples[2]);
		historySamples[3] = _swapEndianS16(internalShadowCopy->src.historySamples[3]);

		cemu_assert_debug(false); // todo
	}

	void AX_DecodeSamplesPCM16_Linear(AXVPBInternal_t* internalShadowCopy, float* output, sint32 sampleCount)
	{
		uint32 currentFracPos = (uint32)_swapEndianU16(internalShadowCopy->src.currentFrac);
		uint32 ratio = _swapEndianU32(*(uint32*)&internalShadowCopy->src.ratioHigh);

		uint32 endOffsetPtr = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.endOffsetPtrHigh);
		uint32 currentOffsetPtr = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.currentOffsetPtrHigh);
		uint32 loopOffsetPtr = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.loopOffsetPtrHigh);
		uint32 ptrHighExtension = _swapEndianU16(internalShadowCopy->internalOffsets.ptrHighExtension);

		uint16* endOffsetAddr = (uint16*)(memory_base + ((endOffsetPtr * 2) | (ptrHighExtension << 29)));
		uint16* currentOffsetAddr = (uint16*)(memory_base + ((currentOffsetPtr * 2) | (ptrHighExtension << 29)));

		uint16* loopOffsetAddrDebug = (uint16*)(memory_base + ((loopOffsetPtr * 2) | (ptrHighExtension << 29)));

		sint16 historySamples[4];
		historySamples[0] = _swapEndianS16(internalShadowCopy->src.historySamples[0]);
		historySamples[1] = _swapEndianS16(internalShadowCopy->src.historySamples[1]);
		historySamples[2] = _swapEndianS16(internalShadowCopy->src.historySamples[2]);
		historySamples[3] = _swapEndianS16(internalShadowCopy->src.historySamples[3]);

		sint32 historyIndex = 0;

		for (sint32 i = 0; i < sampleCount; i++)
		{
			currentFracPos += ratio;
			while (currentFracPos >= 0x10000)
			{
				// read next sample
				historyIndex = (historyIndex + 1) & 3;
				if (internalShadowCopy->playbackState)
				{
					sint32 s = _swapEndianS16(*currentOffsetAddr);
					historySamples[historyIndex] = s;
					if (currentOffsetAddr == endOffsetAddr)
					{
						if (internalShadowCopy->internalOffsets.loopFlag)
						{
							// loop
							currentOffsetAddr = (uint16*)(memory_base + ((loopOffsetPtr * 2) | (ptrHighExtension << 29)));
						}
						else
						{
							// stop playing
							internalShadowCopy->playbackState = 0;
						}
					}
					else
					{
						currentOffsetAddr++; // increment pointer only if not at end offset
					}
				}
				else
				{
					// voice not playing -> sample is silent
					historySamples[historyIndex] = 0;
				}
				currentFracPos -= 0x10000;
			}
			// interpolate sample
			sint32 previousSample = historySamples[(historyIndex + 3) & 3];
			sint32 nextSample = historySamples[historyIndex];

			sint32 p0 = (sint32)previousSample * (sint32)(0x10000 - currentFracPos);
			sint32 p1 = (sint32)nextSample * (sint32)(currentFracPos);
			p0 >>= 7;
			p1 >>= 7;
			sint32 interpolatedSample = p0 + p1;
			interpolatedSample >>= 1;

			*output = (float)interpolatedSample;
			output++;
		}

		// set variables
		internalShadowCopy->src.currentFrac = _swapEndianU16((uint16)(currentFracPos));
		internalShadowCopy->src.historySamples[0] = _swapEndianS16(historySamples[(historyIndex + 0) & 3]);
		internalShadowCopy->src.historySamples[1] = _swapEndianS16(historySamples[(historyIndex + 1) & 3]);
		internalShadowCopy->src.historySamples[2] = _swapEndianS16(historySamples[(historyIndex + 2) & 3]);
		internalShadowCopy->src.historySamples[3] = _swapEndianS16(historySamples[(historyIndex + 3) & 3]);
		// store current offset
		currentOffsetPtr = (uint32)((uint8*)currentOffsetAddr - memory_base);
		currentOffsetPtr &= 0x1FFFFFFF;
		currentOffsetPtr >>= 1;
		*(uint32*)&internalShadowCopy->internalOffsets.currentOffsetPtrHigh = _swapEndianU32(currentOffsetPtr);
	}

	void AX_DecodeSamplesPCM16_Tap(AXVPBInternal_t* internalShadowCopy, float* output, sint32 sampleCount)
	{
		// todo - implement this
		AX_DecodeSamplesPCM16_Linear(internalShadowCopy, output, sampleCount);
	}

	void AX_DecodeSamplesPCM16_NoSrc(AXVPBInternal_t* internalShadowCopy, float* output, sint32 sampleCount)
	{
		// get variables
		uint32 currentFracPos = (uint32)_swapEndianU16(internalShadowCopy->src.currentFrac);
		uint32 ratio = _swapEndianU32(*(uint32*)&internalShadowCopy->src.ratioHigh);

		uint32 endOffsetPtr = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.endOffsetPtrHigh);
		uint32 currentOffsetPtr = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.currentOffsetPtrHigh);
		uint32 loopOffsetPtr = _swapEndianU32(*(uint32*)&internalShadowCopy->internalOffsets.loopOffsetPtrHigh);
		uint32 ptrHighExtension = _swapEndianU16(internalShadowCopy->internalOffsets.ptrHighExtension);

		uint16* endOffsetAddr = (uint16*)(memory_base + (endOffsetPtr * 2 | (ptrHighExtension << 29)));
		uint16* currentOffsetAddr = (uint16*)(memory_base + (currentOffsetPtr * 2 | (ptrHighExtension << 29)));

		if (internalShadowCopy->playbackState == 0)
		{
			memset(output, 0, sizeof(float)*sampleCount);
			return;
		}

		for (sint32 i = 0; i < sampleCount; i++)
		{
			sint32 s = _swapEndianS16(*currentOffsetAddr);
			s <<= 8;
			output[i] = (float)s;
			if (currentOffsetAddr == endOffsetAddr)
			{
				if (internalShadowCopy->internalOffsets.loopFlag)
				{
					currentOffsetAddr = (uint16*)(memory_base + (loopOffsetPtr * 2 | (ptrHighExtension << 29)));
				}
				else
				{
					internalShadowCopy->playbackState = 0;
					for (; i < sampleCount; i++)
					{
						output[i] = 0.0f;
					}
					break;
				}
			}
			else
				currentOffsetAddr++;
		}
		// store current offset
		currentOffsetPtr = (uint32)((uint8*)currentOffsetAddr - memory_base);
		currentOffsetPtr &= 0x1FFFFFFF;
		currentOffsetPtr >>= 1;

		*(uint32*)&internalShadowCopy->internalOffsets.currentOffsetPtrHigh = _swapEndianU32(currentOffsetPtr);
	}

	void AXVoiceMix_DecodeSamples(AXVPBInternal_t* internalShadowCopy, float* output, sint32 sampleCount)
	{
		uint32 srcFilterMode = _swapEndianU16(internalShadowCopy->srcFilterMode);
		uint16 format = _swapEndianU16(internalShadowCopy->internalOffsets.format);

		if (srcFilterMode == AX_FILTER_MODE_TAP)
		{
			if (format == AX_FORMAT_ADPCM)
				AX_DecodeSamplesADPCM_Tap(internalShadowCopy, output, sampleCount);
			else if (format == AX_FORMAT_PCM16)
				AX_DecodeSamplesPCM16_Tap(internalShadowCopy, output, sampleCount);
			else if (format == AX_FORMAT_PCM8)
				AX_DecodeSamplesPCM8_Tap(internalShadowCopy, output, sampleCount);
			else
				cemu_assert_debug(false);
		}
		else if (srcFilterMode == AX_FILTER_MODE_LINEAR)
		{
			if (format == AX_FORMAT_ADPCM)
				AX_DecodeSamplesADPCM_Linear(internalShadowCopy, output, sampleCount);
			else if (format == AX_FORMAT_PCM16)
				AX_DecodeSamplesPCM16_Linear(internalShadowCopy, output, sampleCount);
			else if (format == AX_FORMAT_PCM8)
				AX_DecodeSamplesPCM8_Linear(internalShadowCopy, output, sampleCount);
			else
				cemu_assert_debug(false);
		}
		else if (srcFilterMode == AX_FILTER_MODE_NONE)
		{
			if (format == AX_FORMAT_ADPCM)
				AX_DecodeSamplesADPCM_NoSrc(internalShadowCopy, output, sampleCount);
			else if (format == AX_FORMAT_PCM16)
				AX_DecodeSamplesPCM16_NoSrc(internalShadowCopy, output, sampleCount);
			else if (format == AX_FORMAT_PCM8)
				AX_DecodeSamplesPCM8_NoSrc(internalShadowCopy, output, sampleCount);
			else
				cemu_assert_debug(false);
		}
	}

	sint32 AXVoiceMix_MergeInto(float* inputSamples, float* outputSamples, sint32 sampleCount, AXCHMIX_DEPR* mix, sint16 deltaI)
	{
		float vol = (float)_swapEndianU16(mix->vol) / (float)0x8000;
		if (deltaI != 0)
		{
			float delta = (float)deltaI / (float)0x8000;
			for (sint32 i = 0; i < sampleCount; i++)
			{
				vol += delta;
				outputSamples[i] += inputSamples[i] * vol;
			}
		}
		else
		{
			// optimized version for delta == 0.0
			for (sint32 i = 0; i < sampleCount; i++)
			{
				outputSamples[i] += inputSamples[i] * vol;
			}
		}
		uint16 volI = (uint16)(vol * 32768.0f);
		mix->vol = _swapEndianU16(volI);
		return volI;
	}

	float __AXMixBufferTV[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT * AX_BUS_COUNT];
	float __AXMixBufferDRC[2 * AX_SAMPLES_MAX * AX_DRC_CHANNEL_COUNT * AX_BUS_COUNT];

	void AXVoiceMix_ApplyADSR(AXVPBInternal_t* internalShadowCopy, float* sampleData, sint32 sampleCount)
	{
		uint16 volume = internalShadowCopy->veVolume;
		sint16 volumeDelta = (sint16)internalShadowCopy->veDelta;
		if (volume == 0x8000 && volumeDelta == 0)
			return;
		float volumeScaler = (float)volume / 32768.0f;
		if (volumeDelta == 0)
		{
			// without delta
			for (sint32 i = 0; i < sampleCount; i++)
				sampleData[i] *= volumeScaler;
			return;
		}
		// with delta
		double volumeScalerDelta = (double)volumeDelta / 32768.0;
		volumeScalerDelta = volumeScalerDelta + volumeScalerDelta;
		for (sint32 i = 0; i < sampleCount; i++)
		{
			volumeScaler += (float)volumeScalerDelta;
			sampleData[i] *= volumeScaler;
		}
		if (volumeDelta != 0)
		{
			volume = (uint16)(volumeScaler * 32768.0);
			internalShadowCopy->veVolume = volume;
		}
	}

	void AXVoiceMix_ApplyBiquad(AXVPBInternal_t* internalShadowCopy, float* sampleData, sint32 sampleCount)
	{
		if (internalShadowCopy->biquad.on == AX_BIQUAD_OFF)
			return;
#ifdef CEMU_DEBUG_ASSERT
		if (internalShadowCopy->biquad.on != 0x0200)
		{
			cemuLog_logDebug(LogType::Force, "AX_ApplyBiquad() with incorrect biquad.on value 0x{:04x}", _swapEndianU16(internalShadowCopy->biquad.on));
		}
#endif

		float a1 = (float)(sint16)_swapEndianS16(internalShadowCopy->biquad.a1) / 16384.0f;
		float a2 = (float)(sint16)_swapEndianS16(internalShadowCopy->biquad.a2) / 16384.0f;
		float b0 = (float)(sint16)_swapEndianS16(internalShadowCopy->biquad.b0) / 16384.0f;
		float b1 = (float)(sint16)_swapEndianS16(internalShadowCopy->biquad.b1) / 16384.0f;
		float b2 = (float)(sint16)_swapEndianS16(internalShadowCopy->biquad.b2) / 16384.0f;

		float yn1 = (float)_swapEndianS16(internalShadowCopy->biquad.yn1);
		float yn2 = (float)_swapEndianS16(internalShadowCopy->biquad.yn2);
		float xn1 = (float)_swapEndianS16(internalShadowCopy->biquad.xn1);
		float xn2 = (float)_swapEndianS16(internalShadowCopy->biquad.xn2);
		if (internalShadowCopy->biquad.b1 != 0)
		{
			for (sint32 i = 0; i < sampleCount; i++)
			{
				float inputSample = sampleData[i] / 256.0f;
				float temp = b0 * inputSample + b1 * xn1 + b2 * xn2 + a1 * yn1 + a2 * yn2;
				sampleData[i] = temp * 256.0f;
				temp = std::min(32767.0f, temp);
				temp = std::max(-32768.0f, temp);
				yn2 = yn1;
				xn2 = xn1;
				yn1 = temp;
				xn1 = inputSample;
			}
		}
		else
		{
			// optimized variant where voiceInternal->biquad.b1 is hardcoded as zero (used heavily in BotW and Splatoon)
			for (sint32 i = 0; i < sampleCount; i++)
			{
				float inputSample = sampleData[i] / 256.0f;
				float temp = b0 * inputSample + b2 * xn2 + a1 * yn1 + a2 * yn2;
				sampleData[i] = temp * 256.0f;
				temp = std::min(32767.0f, temp);
				temp = std::max(-32768.0f, temp);
				yn2 = yn1;
				xn2 = xn1;
				yn1 = temp;
				xn1 = inputSample;
			}
		}

		internalShadowCopy->biquad.yn1 = _swapEndianU16((sint16)(yn1));
		internalShadowCopy->biquad.yn2 = _swapEndianU16((sint16)(yn2));
		internalShadowCopy->biquad.xn1 = _swapEndianU16((sint16)(xn1));
		internalShadowCopy->biquad.xn2 = _swapEndianU16((sint16)(xn2));
	}

	// Thor: AX one-pole low-pass is y[n] = (a0*x[n] + b0*y[n-1]) >> 15 with a0 + b0 = 0x7FFF (see AXComputeLpfCoefs,
	// matches Dolphin's DSP HLE). Cemu used to SUBTRACT the feedback term, which turns the filter into a
	// near-silent high-pass for low cutoffs. BO2 runs its distance/occlusion filtering through this, so gunshots
	// and zombies came out muffled or missing. yn1 is kept in s16 sample units (sampleData is s16 * 256).
	bool g_axLpfLegacy = false; // runtime A/B: <userdata>/lpf_legacy present => old behaviour
	uint32 g_axLpfStatVoices = 0, g_axLpfStatOn = 0, g_axBiquadStatOn = 0;
	uint64 g_axLpfStatA0Sum = 0;

	void AXVoiceMix_ApplyLowPass(AXVPBInternal_t* internalShadowCopy, float* sampleData, sint32 sampleCount)
	{
		g_axLpfStatVoices++;
		if (internalShadowCopy->biquad.on != AX_BIQUAD_OFF)
			g_axBiquadStatOn++;
		if (internalShadowCopy->lpf.on == _swapEndianU16(AX_LPF_OFF))
			return;
		g_axLpfStatOn++;
		g_axLpfStatA0Sum += (uint16)_swapEndianU16(internalShadowCopy->lpf.a0);
		if (g_axLpfLegacy)
		{
			float a0 = (float)_swapEndianS16(internalShadowCopy->lpf.a0) / 32767.0f;
			float b0 = (float)_swapEndianS16(internalShadowCopy->lpf.b0) / 32767.0f;
			float prevSample = (float)_swapEndianS16((sint16)internalShadowCopy->lpf.yn1) * 256.0f / 32767.0f;
			for (sint32 i = 0; i < sampleCount; i++)
			{
				sampleData[i] = a0 * sampleData[i] - b0 * prevSample;
				prevSample = sampleData[i];
			}
			internalShadowCopy->lpf.yn1 = (uint16)_swapEndianS16((sint16)std::clamp(prevSample / 256.0f * 32767.0f, -32768.0f, 32767.0f));
			return;
		}
		const float a0 = (float)(uint16)_swapEndianU16(internalShadowCopy->lpf.a0) / 32768.0f;
		const float b0 = (float)(uint16)_swapEndianU16(internalShadowCopy->lpf.b0) / 32768.0f;
		float y = (float)(sint16)_swapEndianU16(internalShadowCopy->lpf.yn1) * 256.0f;
		for (sint32 i = 0; i < sampleCount; i++)
		{
			y = a0 * sampleData[i] + b0 * y;
			sampleData[i] = y;
		}
		internalShadowCopy->lpf.yn1 = _swapEndianU16((uint16)(sint16)std::clamp(y / 256.0f, -32768.0f, 32767.0f));
	}

	// mix audio generated from voice into main bus and aux buses
	void AXVoiceMix_MixIntoBuses(AXVPBInternal_t* internalShadowCopy, float* sampleData, sint32 sampleCount, sint32 samplesPerFrame)
	{
		// TV mixing
		for (sint32 busIndex = 0; busIndex < AX_BUS_COUNT; busIndex++)
		{
			for (sint32 channel = 0; channel < 6; channel++)
			{
				uint32 channelMixMask = (_swapEndianU16(internalShadowCopy->deviceMixMaskTV[busIndex]) >> (channel * 2)) & 3;
				if (channelMixMask == 0)
				{
					internalShadowCopy->reserved1E8[busIndex*AX_TV_CHANNEL_COUNT + channel] = 0;
					continue;
				}
				AXCHMIX_DEPR* mix = internalShadowCopy->deviceMixTV + channel * 4 + busIndex;
				float* output = __AXMixBufferTV + (busIndex * 6 + channel) * samplesPerFrame;
				AXVoiceMix_MergeInto(sampleData, output, sampleCount, mix, _swapEndianS16(mix->delta));
				internalShadowCopy->reserved1E8[busIndex*AX_TV_CHANNEL_COUNT + channel] = mix->vol;
			}
		}
		// DRC0 mixing
		for (sint32 busIndex = 0; busIndex < AX_BUS_COUNT; busIndex++)
		{
			for (sint32 channel = 0; channel < AX_DRC_CHANNEL_COUNT; channel++)
			{
				uint32 channelMixMask = (_swapEndianU16(internalShadowCopy->deviceMixMaskDRC[busIndex]) >> (channel * 2)) & 3;

				if (channelMixMask == 0)
				{
					//internalShadowCopy->reserved1E8[busIndex*AX_DRC_CHANNEL_COUNT + channel] = 0;
					continue;
				}
				AXCHMIX_DEPR* mix = internalShadowCopy->deviceMixDRC + channel * 4 + busIndex;
				float* output = __AXMixBufferDRC + (busIndex * AX_DRC_CHANNEL_COUNT + channel) * samplesPerFrame;
				AXVoiceMix_MergeInto(sampleData, output, sampleCount, mix, _swapEndianS16(mix->delta));
			}
		}

		// DRC1 mixing + RMT mixing
		// todo
	}

	// ---- Thor voice tracer (<userdata>/trace_voices present): one log line per voice lifetime -------------
	bool g_axVoiceTrace = false;
	float g_axDrcFoldGain = 1.0f; // <userdata>/drc_fold_gain (float, 0 = off)
	double g_axDrcFoldEnergy = 0; uint64 g_axDrcFoldSamples = 0;
	uint16 g_axDrcAuxReturnVolume[3] = { 0x8000, 0x8000, 0x8000 };
	uint16 g_axDrcDeviceVolume = 0x8000;
	FILE* g_axDrcDumpFile = nullptr; uint32 g_axDrcDumpLeft = 0, g_axDrcDumpWritten = 0;
	sint32 g_axDrcFoldMode = 1; // <userdata>/drc_fold_mode: 0 off, 1 DRC sends -> TV final mix (default), 2/3 mix-level experiments, 4 DRC final output fold
	// Thor: BO2 mutes its own DRC final mix, so the GamePad-routed voices (player gunfire body, nearby zombies) are
	// captured here as a stereo downmix of the DRC aux sends and added after the TV final-mix callback (ax_ist.cpp).
	float g_axDrcSide[4][AX_SAMPLES_MAX]; sint32 g_axDrcSideN = 0;
	double g_axTvAuxInE = 0;
	double g_axDrcAuxInE[3] = {}, g_axDrcAuxOutE[3] = {}; bool g_axDrcAuxHasCb[3] = {}; uint32 g_axDrcDryUsed[3] = {}; uint64 g_axDrcMeterSamples = 0;
	struct VoiceTrace
	{
		uint64 firstFrame{}, lastFrame{};
		uint32 frames{};
		double rawE{}, outE{}; float outMax{};
		uint32 startAddr{}; uint16 fmt{}; uint32 src{};
		uint16 ve{}, veMax{}; uint32 tvMain{}, tvAux{}, drc{}, drcAux{}; uint16 lpfA0{}; uint16 lpfOn{};
	};
	static VoiceTrace s_vt[256];
	static uint64 s_vtFrame = 0;
	static void AXVoiceTrace_Emit(sint32 idx, VoiceTrace& t)
	{
		if (t.frames == 0) return;
		cemuLog_log(LogType::Force, "VT v{:02} ago{:.2f} dur{}ms fmt{} src{:05x} addr{:08x} ve{:04x}/{:04x} tvMain{:05} tvAux{:05} drc{:05} drcAux{:05} lpf{}:{:04x} raw{:.0f} out{:.0f} outMax{:.0f}",
			idx, (double)(s_vtFrame - t.firstFrame) * 0.003, t.frames * 3, t.fmt, t.src, t.startAddr, t.ve, t.veMax, t.tvMain, t.tvAux, t.drc, t.drcAux, t.lpfOn, t.lpfA0,
			sqrt(t.rawE / t.frames), sqrt(t.outE / t.frames), t.outMax);
		t = VoiceTrace{};
	}
	void AXVoiceTrace_Account(AXVPBInternal_t* v, float* data, size_t n, float rawRms, uint64 frame)
	{
		sint32 idx = (sint32)(v - __AXVPBInternalVoiceShadowCopyArrayPtr);
		if (idx < 0 || idx >= 256) return;
		s_vtFrame = frame;
		VoiceTrace& t = s_vt[idx];
		uint32 addr = v->internalOffsets.GetCurrentOffset32();
		if (t.frames && t.lastFrame + 1 != frame)
			AXVoiceTrace_Emit(idx, t);
		double e = 0; for (size_t i = 0; i < n; i++) e += (double)data[i] * data[i];
		float outRms = (float)sqrt(e / (double)n) / 256.0f;
		if (t.frames == 0)
		{
			t.firstFrame = frame; t.startAddr = addr; t.fmt = _swapEndianU16(v->internalOffsets.format); t.src = v->src.GetSrcRatio32();
			t.ve = (uint16)v->veVolume;
		}
		t.lastFrame = frame; t.frames++;
		t.rawE += (double)rawRms * rawRms; t.outE += (double)outRms * outRms; t.outMax = std::max(t.outMax, outRms);
		t.veMax = std::max<uint16>(t.veMax, (uint16)v->veVolume);
		uint32 main = 0, aux = 0, drc = 0;
		for (int c = 0; c < 6; c++)
		{
			main = std::max<uint32>(main, _swapEndianU16(v->deviceMixTV[c * 4 + 0].vol));
			for (int b = 1; b < 4; b++) aux = std::max<uint32>(aux, _swapEndianU16(v->deviceMixTV[c * 4 + b].vol));
		}
		for (int c = 0; c < AX_DRC_CHANNEL_COUNT; c++) { drc = std::max<uint32>(drc, _swapEndianU16(v->deviceMixDRC[c * 4 + 0].vol)); for (int b = 1; b < 4; b++) t.drcAux = std::max<uint32>(t.drcAux, _swapEndianU16(v->deviceMixDRC[c * 4 + b].vol)); }
		t.tvMain = std::max(t.tvMain, main); t.tvAux = std::max(t.tvAux, aux); t.drc = std::max(t.drc, drc);
		t.lpfOn = _swapEndianU16(v->lpf.on); if (t.lpfOn) t.lpfA0 = _swapEndianU16(v->lpf.a0);
	}
	void AXVoiceTrace_Flush()
	{
		for (sint32 i = 0; i < 256; i++)
			if (s_vt[i].frames && s_vt[i].lastFrame + 1 < s_vtFrame) AXVoiceTrace_Emit(i, s_vt[i]);
	}

	void AXMix_ProcessVoices(AXVPBInternal_t* firstVoice)
	{
		if (firstVoice == nullptr)
			return;
		size_t sampleCount = AXGetInputSamplesPerFrame();
		AXVPBInternal_t* internalVoice = firstVoice;
		cemu_assert_debug(sndGeneric.initParam.frameLength == 0);
		float tmpSampleBuffer[AX_SAMPLES_MAX];
		static uint64 s_traceFrame = 0;
		s_traceFrame++;
		while (internalVoice)
		{
			AXVoiceMix_DecodeSamples(internalVoice, tmpSampleBuffer, sampleCount);
			float rawRms = 0.0f;
			if (g_axVoiceTrace)
			{
				double e = 0; for (size_t i = 0; i < sampleCount; i++) e += (double)tmpSampleBuffer[i] * tmpSampleBuffer[i];
				rawRms = (float)sqrt(e / (double)sampleCount) / 256.0f;
			}
			AXVoiceMix_ApplyADSR(internalVoice, tmpSampleBuffer, sampleCount);
			AXVoiceMix_ApplyBiquad(internalVoice, tmpSampleBuffer, sampleCount);
			AXVoiceMix_ApplyLowPass(internalVoice, tmpSampleBuffer, sampleCount);
			if (g_axVoiceTrace)
				AXVoiceTrace_Account(internalVoice, tmpSampleBuffer, sampleCount, rawRms, s_traceFrame);
			AXVoiceMix_MixIntoBuses(internalVoice, tmpSampleBuffer, sampleCount, sampleCount);
			// next
			internalVoice = internalVoice->nextToProcess.GetPtr();
		}
		if (g_axVoiceTrace)
			AXVoiceTrace_Flush();
	}

	void AXMix_MergeBusSamples(float* input, sint32* output, sint32 sampleCount, uint16& volume, sint16 delta)
	{
		float volumeF = (float)volume / 32768.0f;
		float deltaF = (float)delta / 32768.0f;

		if (delta)
		{
			for (sint32 i = 0; i < sampleCount; i++)
			{
				float s = *input;
				input++;
				s *= volumeF;
				volumeF += deltaF;
				*output = _swapEndianS32(_swapEndianS32(*output) + (sint32)s);
				output++;
			}
			volume = (uint16)(volumeF * 32768.0f);
		}
		else
		{
			// no delta
			for (sint32 i = 0; i < sampleCount; i++)
			{
				float s = *input;
				input++;
				s *= volumeF;
				*output = _swapEndianS32(_swapEndianS32(*output) + (sint32)s);
				output++;
			}
		}
	}

	void AXAuxMix_StoreAuxSamples(float* input, sint32be* output, sint32 sampleCount)
	{
		// Not 100% sure why but we need to temporarily right shift the aux samples by 8 to get the sample range the games expect for the AUX callback
		// without this, Color Splash will apply it's effects incorrectly
		// Its probably because AUX mixing always goes through the DSP which uses 16bit arithmetic?

		// no delta
		for (sint32 i = 0; i < sampleCount; i++)
		{
			float s = *input;
			input++;
			*output = ((sint32)s) >> 8;
			output++;
		}
	}

	void AXAuxMix_MixProcessedAuxSamplesIntoOutput(sint32be* input, float* output, sint32 sampleCount, uint16* volumePtr, sint16 delta)
	{
		uint16 volume = *volumePtr;
		float volumeF = (float)volume / 32768.0f;
		float deltaF = (float)delta / 32768.0f;

		cemu_assert_debug(delta == 0); // todo

		for (sint32 i = 0; i < sampleCount; i++)
		{
			float s = (float)(((sint32)*input)<<8);
			input++;
			s *= volumeF;
			*output += s;
			output++;
		}
		*volumePtr = volume;
	}

	uint16 __AXMasterVolume = 0x8000;
	uint16 __AXDRCMasterVolume = 0x8000;

	// mix into __AXTVOutputBuffer
	void AXMix_mergeTVBuses()
	{
		size_t sampleCount = AXGetInputSamplesPerFrame();

		// debug - Erase main bus and only output AUX
		if (ActiveSettings::AudioOutputOnlyAux())
		{
			memset(__AXMixBufferTV, 0, sizeof(float) * sampleCount * 6);
		}
		// Mix aux into TV main bus
		for (sint32 auxBus = 0; auxBus < AX_AUX_BUS_COUNT; auxBus++)
		{
			sint32be* auxOutput = AXAux_GetOutputBuffer(AX_DEV_TV, 0, auxBus);
			if (auxOutput == nullptr)
				continue;
			// AUX return from output buffer
			uint16 auxReturnVolume = __AXTVAuxReturnVolume[auxBus];
			sint16 auxReturnDelta = 0;
			AXAuxMix_MixProcessedAuxSamplesIntoOutput(auxOutput, __AXMixBufferTV, sampleCount * AX_TV_CHANNEL_COUNT, &auxReturnVolume, auxReturnDelta);
		}
		// mix TV main bus into output
		float* input = __AXMixBufferTV;
		uint16 masterVolume = __AXMasterVolume;
		sint32* output = __AXTVOutputBuffer.GetPtr();
		cemu_assert_debug(masterVolume == 0x8000); // todo -> Calculate delta between old master volume and new volume
		sint16 delta = 0;
		uint16 volVar;
		for (uint16 c = 0; c < AX_TV_CHANNEL_COUNT; c++)
		{
			volVar = _swapEndianU16(masterVolume);
			AXMix_MergeBusSamples(input, output, sampleCount, masterVolume, delta);
			output += sampleCount;
			input += sampleCount;
		}
	}

	// mix into __AXDRCOutputBuffer
	void AXMix_mergeDRC0Buses()
	{
		sint32* output = __AXDRCOutputBuffer.GetPtr();
		uint16 masterVolume = __AXDRCMasterVolume;		
		size_t sampleCount = AXGetInputSamplesPerFrame();

		// todo - drc0 AUX

		// mix DRC0 main bus into output
		float* input = __AXMixBufferDRC;
		cemu_assert_debug(masterVolume == 0x8000); // todo -> Calculate delta between old master volume and new volume
		sint16 delta = 0;
		for (uint16 c = 0; c < AX_DRC_CHANNEL_COUNT; c++)
		{
			AXMix_MergeBusSamples(input, output, sampleCount, masterVolume, delta);
			output += sampleCount;
			input += sampleCount;
		}
	}

	void AXMix_process(AXVPBInternal_t* internalShadowCopyHead)
	{
		memset(__AXMixBufferTV, 0, sizeof(__AXMixBufferTV));
		memset(__AXMixBufferDRC, 0, sizeof(__AXMixBufferDRC));

		AXMix_ProcessVoices(internalShadowCopyHead);
		{ // Thor diag: TV bus per-channel energy straight from the voice mixer (main + aux A/B/C), before game final mix
			static double s_be[4][6] = {}; static uint32 s_bn = 0;
			const size_t sc = AXGetInputSamplesPerFrame();
			for (int bus = 0; bus < 4; bus++) for (int c = 0; c < 6; c++) { const float* q = __AXMixBufferTV + (bus * 6 + c) * sc; double e = 0; for (size_t i = 0; i < sc; i++) e += (double)q[i] * q[i]; s_be[bus][c] += e; }
			if (++s_bn % 3333 == 0)
			{
				const double n = (double)sc * 3333.0 * 65536.0;
				for (int bus = 0; bus < 4; bus++)
					cemuLog_log(LogType::Force, "Thor TV voice bus{} rms L={:.0f} R={:.0f} SL={:.0f} SR={:.0f} C={:.0f} LFE={:.0f}", bus,
						sqrt(s_be[bus][0] / n), sqrt(s_be[bus][1] / n), sqrt(s_be[bus][2] / n), sqrt(s_be[bus][3] / n), sqrt(s_be[bus][4] / n), sqrt(s_be[bus][5] / n));
				memset(s_be, 0, sizeof(s_be));
			}
		}
		AXAux_Process(); // apply AUX effects to previous frame
		AXIst_HandleFrameCallbacks();

		size_t sampleCount = AXGetInputSamplesPerFrame();
		// TV aux store
		for (sint32 auxBus = 0; auxBus < AX_AUX_BUS_COUNT; auxBus++)
		{
			sint32be* auxInput = AXAux_GetInputBuffer(AX_DEV_TV, 0, auxBus);
			if (auxInput == nullptr)
				continue;
			float* tvInput = __AXMixBufferTV + (1 + auxBus) * (sampleCount * AX_TV_CHANNEL_COUNT);
			AXAuxMix_StoreAuxSamples(tvInput, auxInput, sampleCount * AX_TV_CHANNEL_COUNT);
		}

		// DRC aux store/return (Thor: was a todo; BO2 sends the GamePad player's own sounds through DRC aux).
		// Meters + fallback: if a bus has no callback or its callback returns silence, its dry send is folded instead.
		{
			const size_t busLen = sampleCount * AX_DRC_CHANNEL_COUNT;
			static float s_outEma[AX_AUX_BUS_COUNT] = {};
			for (sint32 auxBus = 0; auxBus < AX_AUX_BUS_COUNT; auxBus++)
			{
				float* drcBus = __AXMixBufferDRC + (1 + auxBus) * busLen;
				double inE = 0; for (size_t i = 0; i < busLen; i++) inE += (double)drcBus[i] * drcBus[i];
				g_axDrcAuxInE[auxBus] += inE / 65536.0;
				sint32be* auxInput = AXAux_GetInputBuffer(AX_DEV_DRC, 0, auxBus);
				g_axDrcAuxHasCb[auxBus] = auxInput != nullptr;
				if (auxInput)
					AXAuxMix_StoreAuxSamples(drcBus, auxInput, (sint32)busLen);
				double outE = 0;
				if (sint32be* auxOutput = AXAux_GetOutputBuffer(AX_DEV_DRC, 0, auxBus))
				{
					for (size_t i = 0; i < busLen; i++) { double v = (double)(((sint32)auxOutput[i]) << 8); outE += v * v; }
					uint16 auxReturnVolume = g_axDrcAuxReturnVolume[auxBus];
					if (g_axDrcFoldMode != 2)
						AXAuxMix_MixProcessedAuxSamplesIntoOutput(auxOutput, __AXMixBufferDRC, (sint32)busLen, &auxReturnVolume, 0);
				}
				g_axDrcAuxOutE[auxBus] += outE / 65536.0;
				s_outEma[auxBus] = s_outEma[auxBus] * 0.99f + (float)(outE / (double)busLen) * 0.01f;
				const bool useDry = g_axDrcFoldMode == 2 || (g_axDrcFoldMode == 3 && s_outEma[auxBus] < 1.0f);
				if (useDry && inE > 0.0)
				{
					for (size_t i = 0; i < busLen; i++) __AXMixBufferDRC[i] += drcBus[i];
					g_axDrcDryUsed[auxBus]++;
				}
			}
			g_axDrcMeterSamples += busLen;
			// DRC aux sends (s16 units): front L/R in [0][1], rear SL/SR in [2][3] when the TV is 5.1 (rear cue applied
			// at output), otherwise the old stereo downmix. Non-finite samples (one blown-up voice) are dropped.
			{
				extern bool g_axGameSurround;
				const bool surround = g_axGameSurround && __AXMode[AX_DEV_TV] == AX_MODE_6CH;
				static double s_chE[4] = {}; static uint32 s_chFrames = 0;
				for (size_t i = 0; i < sampleCount; i++)
				{
					float c4[4] = {};
					for (sint32 auxBus = 0; auxBus < AX_AUX_BUS_COUNT; auxBus++)
					{
						const float* b = __AXMixBufferDRC + (1 + auxBus) * busLen;
						for (int c = 0; c < 4; c++) c4[c] += b[sampleCount * c + i];
					}
					for (int c = 0; c < 4; c++) { c4[c] /= 256.0f; if (!std::isfinite(c4[c]) || fabsf(c4[c]) > 1.0e6f) c4[c] = 0.0f; s_chE[c] += (double)c4[c] * c4[c]; }
					// BO2 only ever fills DRC slots 2/3 (slots 0/1 measured exactly 0 in-match, any facing), so they are a fixed
					// layout, NOT rear. Always fold them to the FRONT, else gunshot body + nearest zombies get the rear muffle.
					(void)surround;
					{
						g_axDrcSide[0][i] = c4[0] + 0.7071f * c4[2]; g_axDrcSide[1][i] = c4[1] + 0.7071f * c4[3];
						g_axDrcSide[2][i] = 0.0f; g_axDrcSide[3][i] = 0.0f;
					}
				}
				if (++s_chFrames % 3333 == 0)
				{
					double n = (double)sampleCount * 3333.0;
					cemuLog_log(LogType::Force, "AX DRC sends per channel (drcMode {}) rms L={:.0f} R={:.0f} SL={:.0f} SR={:.0f}", __AXMode[AX_DEV_DRC],
						sqrt(s_chE[0] / n), sqrt(s_chE[1] / n), sqrt(s_chE[2] / n), sqrt(s_chE[3] / n));
					for (auto& e : s_chE) e = 0;
				}
			}
			g_axDrcSideN = (sint32)sampleCount;
			// TV aux send level (L/R, s16 units) for matching the game's TV final-mix gain
			{
				const size_t tvBusLen = sampleCount * AX_TV_CHANNEL_COUNT;
				double e = 0;
				for (size_t i = 0; i < sampleCount; i++)
				{
					float l = 0, r = 0;
					for (sint32 auxBus = 0; auxBus < AX_AUX_BUS_COUNT; auxBus++)
					{
						const float* b = __AXMixBufferTV + (1 + auxBus) * tvBusLen;
						l += b[i]; r += b[sampleCount + i];
						if (__AXMode[AX_DEV_TV] == AX_MODE_6CH) { l += b[sampleCount * 2 + i]; r += b[sampleCount * 3 + i]; }
					}
					l /= 256.0f; r /= 256.0f;
					if (std::isfinite(l) && std::isfinite(r) && fabsf(l) < 1.0e6f && fabsf(r) < 1.0e6f) e += (double)l * l + (double)r * r;
				}
				g_axTvAuxInE += e;
			}
		}
		// Thor: no GamePad speaker. Games (BO2) route the GamePad player's own sounds - gunshots, nearby zombies -
		// to the DRC. When no pad audio device is open, fold the DRC main bus into the TV main bus so it is heard.
		{
			extern bool AXOut_IsPadAudioActive();
			if (!AXOut_IsPadAudioActive() && g_axDrcFoldGain > 0.0f && g_axDrcFoldMode >= 2)
			{
				float* drc = __AXMixBufferDRC;
				float* tvL = __AXMixBufferTV;
				float* tvR = __AXMixBufferTV + sampleCount;
				const float g = g_axDrcFoldGain * ((float)g_axDrcDeviceVolume / 32768.0f) * ((float)__AXDRCMasterVolume / 32768.0f), gs = g * 0.7071f;
				double e = 0;
				sint16 dumpBuf[2 * AX_SAMPLES_MAX];
				for (size_t i = 0; i < sampleCount; i++)
				{
					float l = drc[i], r = drc[sampleCount + i], sl = drc[sampleCount * 2 + i], sr = drc[sampleCount * 3 + i];
					float addL = g * l + gs * sl, addR = g * r + gs * sr;
					tvL[i] += addL;
					tvR[i] += addR;
					e += (double)addL * addL + (double)addR * addR;
					dumpBuf[i * 2 + 0] = (sint16)std::clamp(addL / 256.0f, -32768.0f, 32767.0f);
					dumpBuf[i * 2 + 1] = (sint16)std::clamp(addR / 256.0f, -32768.0f, 32767.0f);
				}
				g_axDrcFoldEnergy += e / 65536.0;
				g_axDrcFoldSamples += sampleCount * 2;

			}
		}

		// merge main and aux buses
		AXMix_mergeTVBuses();
		AXMix_mergeDRC0Buses();

		AXAux_incrementBufferIndex();
		// update microphone
		mic_updateOnAXFrame();
	}

}
