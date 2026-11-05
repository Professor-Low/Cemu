#include "Cafe/OS/libs/snd_core/ax.h"
#include "Cafe/OS/libs/snd_core/ax_internal.h"
#include "Cafe/HW/MMU/MMU.h"
#include "audio/IAudioAPI.h"
//#include "ax.h"
#include "config/CemuConfig.h"
#include "Cemu/Logging/CemuLogging.h"
#include <cmath>
#include <algorithm>
#include <mutex>
#include <cstdio>

namespace snd_core
{
	// Thor: <userdata>/dump_audio (content = seconds, default 20) records the final stereo TV mix to audio_dump.wav
	static FILE* s_dumpFile = nullptr;
	static uint32 s_dumpSamplesLeft = 0, s_dumpSamplesWritten = 0;
	static std::mutex s_dumpMutex;
	static void AXOut_WriteWavHeader(FILE* f, uint32 frames)
	{
		uint32 dataBytes = frames * 4, riff = 36 + dataBytes, rate = 48000, byteRate = 48000 * 4, fmtLen = 16;
		uint16 pcm = 1, ch = 2, align = 4, bits = 16;
		fseek(f, 0, SEEK_SET);
		fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmtLen, 4, 1, f);
		fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
		fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
	}
	// Thor: treat the pad stream as inactive when it is muted (Android opens it at PadVolume 0 by default)
	// Thor build: one physical speaker, so the DRC is always folded into the TV mix (drc_fold_gain 0 disables).
	bool AXOut_IsPadAudioActive() { return false; }
	void AXOut_StartAudioDump(const std::string& path, uint32 seconds)
	{
		std::lock_guard lock(s_dumpMutex);
		if (s_dumpFile) { AXOut_WriteWavHeader(s_dumpFile, s_dumpSamplesWritten); fclose(s_dumpFile); }
		s_dumpFile = fopen(path.c_str(), "wb");
		if (!s_dumpFile) return;
		AXOut_WriteWavHeader(s_dumpFile, 0);
		s_dumpSamplesLeft = seconds * 48000; s_dumpSamplesWritten = 0;
		{
			extern FILE* g_axDrcDumpFile; extern uint32 g_axDrcDumpLeft, g_axDrcDumpWritten;
			if (g_axDrcDumpFile) fclose(g_axDrcDumpFile);
			std::string p2 = path.substr(0, path.size() - 4) + "_drc32k.raw";
			g_axDrcDumpFile = fopen(p2.c_str(), "wb");
			g_axDrcDumpLeft = seconds * 32000; g_axDrcDumpWritten = 0;
		}
		cemuLog_log(LogType::Force, "Thor: audio dump started ({}s) -> {}", seconds, path);
	}
	static void AXOut_DumpFrame(const sint16* bigEndianStereo, sint32 numSamples)
	{
		std::lock_guard lock(s_dumpMutex);
		if (!s_dumpFile) return;
		sint16 tmp[2 * 1024];
		sint32 n = std::min<sint32>(numSamples, std::min<uint32>(1024, s_dumpSamplesLeft));
		for (sint32 i = 0; i < n * 2; i++) tmp[i] = _swapEndianS16(bigEndianStereo[i]);
		fwrite(tmp, 4, n, s_dumpFile);
		s_dumpSamplesLeft -= n; s_dumpSamplesWritten += n;
		if (s_dumpSamplesLeft == 0)
		{
			AXOut_WriteWavHeader(s_dumpFile, s_dumpSamplesWritten);
			fclose(s_dumpFile); s_dumpFile = nullptr;
			extern FILE* g_axDrcDumpFile;
			if (g_axDrcDumpFile) { fclose(g_axDrcDumpFile); g_axDrcDumpFile = nullptr; }
			cemuLog_log(LogType::Force, "Thor: audio dump finished ({} frames)", s_dumpSamplesWritten);
		}
	}

	uint32 numProcessedFrames = 0;

	void resetNumProcessedFrames()
	{
		numProcessedFrames = 0;
	}

	uint32 getNumProcessedFrames()
	{
		return numProcessedFrames;
	}

	sint32 __AXMode[AX_DEV_COUNT]; // audio mode (AX_MODE_*) per device


	bool AVMGetTVAudioMode(uint32be* tvAudioMode)
	{
		// 0 -> mono
		// 1,2 -> stereo
		// 3 -> surround
		// 4 -> unknown mode
		switch (GetConfig().tv_channels)
		{
		case kMono:
			*tvAudioMode = 0;
			break;
		case kSurround:
			*tvAudioMode = 3;
			break;
		default:
			*tvAudioMode = 2;
			break;
		}

		return true;
	}

	bool AVMGetDRCSystemAudioMode(uint32be* drcAudioMode)
	{
		*drcAudioMode = 1; // apparently the default is Stereo(?), MH3U exits if AXGetDeviceMode doesn't return 0 (DRCSystemAudioMode must return 1 to set DRC mode to 0)
		return true;
	}

	sint32 __AXOutTVOutputChannelCount;
	sint32 __AXOutDRCOutputChannelCount;

	void __AXSetTVMode(sint32 mode)
	{
		cemu_assert(mode == AX_MODE_STEREO || mode == AX_MODE_6CH || mode == AX_MODE_MONO);
		__AXMode[AX_DEV_TV] = mode;
	}

	void __AXSetDeviceMode(sint32 device, sint32 mode)
	{
		if (device == AX_DEV_TV)
			__AXMode[AX_DEV_TV] = mode;
		else if (device == AX_DEV_DRC)
			__AXMode[AX_DEV_DRC] = mode;
		else if (device == AX_DEV_RMT)
			__AXMode[AX_DEV_RMT] = mode;
		else
		{
			cemu_assert_debug(false);
		}
	}

	sint32 AXGetDeviceMode(sint32 device)
	{
		if (device == AX_DEV_TV || device == AX_DEV_DRC || device == AX_DEV_RMT)
			return __AXMode[device];
		cemu_assert_debug(false);
		return 0;
	}

	void _AXOutInitDeviceModes()
	{
		// TV mode
		uint32be tvAudioMode;
		AVMGetTVAudioMode(&tvAudioMode);
		if (tvAudioMode == 0)
		{
			// mono
			__AXSetTVMode(AX_MODE_MONO);
			__AXOutTVOutputChannelCount = 1;
		}
		else if (tvAudioMode == 1 || tvAudioMode == 2)
		{
			// stereo
			__AXSetTVMode(AX_MODE_STEREO);
			__AXOutTVOutputChannelCount = 2;
		}
		else if (tvAudioMode == 3)
		{
			// surround (6ch)
			__AXSetTVMode(AX_MODE_6CH);
			__AXOutTVOutputChannelCount = 6;
		}
		else
		{
			assert_dbg();
		}
		// DRC mode
		uint32be drcAudioMode;
		AVMGetDRCSystemAudioMode(&drcAudioMode);
		if (drcAudioMode == 0)
		{
			// mono
			__AXSetDeviceMode(1, AX_MODE_MONO);
			__AXOutDRCOutputChannelCount = 1;
		}
		else if (drcAudioMode == 2)
		{
			// surround
			__AXSetDeviceMode(1, AX_MODE_SURROUND);
			__AXOutDRCOutputChannelCount = 2; // output channel count still 2 for DRC 'surround'
		}
		else if (drcAudioMode == 1)
		{
			// stereo
			__AXSetDeviceMode(1, AX_MODE_STEREO);
			__AXOutDRCOutputChannelCount = 2;
		}
		else
		{
			assert_dbg();
		}
	}

	void AXOut_Init()
	{
		_AXOutInitDeviceModes();
	}

	extern SysAllocator<sint32, AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT> __AXTVBuffer48;
	extern SysAllocator<sint32, AX_SAMPLES_MAX* AX_DRC_CHANNEL_COUNT * 2> __AXDRCBuffer48;

	sint16 __buf_AXTVDMABuffers_0[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT];
	sint16 __buf_AXTVDMABuffers_1[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT];
	sint16 __buf_AXTVDMABuffers_2[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT];
	sint16* __AXTVDMABuffers[3] = {__buf_AXTVDMABuffers_0, __buf_AXTVDMABuffers_1, __buf_AXTVDMABuffers_2};

	#define AX_FRAMES_PER_GROUP		(4)

	sint16 tempTVChannelData[AX_SAMPLES_MAX * AX_TV_CHANNEL_COUNT * AX_FRAMES_PER_GROUP] = {};
	sint32 tempAudioBlockCounter = 0;


	sint16 __buf_AXDRCDMABuffers_0[AX_SAMPLES_MAX * 6];
	sint16 __buf_AXDRCDMABuffers_1[AX_SAMPLES_MAX * 6];
	sint16 __buf_AXDRCDMABuffers_2[AX_SAMPLES_MAX * 6];
	sint16* __AXDRCDMABuffers[3] = { __buf_AXDRCDMABuffers_0, __buf_AXDRCDMABuffers_1, __buf_AXDRCDMABuffers_2 };

	sint16 tempDRCChannelData[AX_SAMPLES_MAX * 6 * AX_FRAMES_PER_GROUP] = {};
	sint32 tempDRCAudioBlockCounter = 0;

	void AIInitDMA(sint16* sampleData, sint32 size)
	{
		sint32 sampleCount = size / sizeof(sint16); // sample count in total (summed up for all channels)

		if (sndGeneric.initParam.frameLength != 0)
		{
			cemu_assert(false);
		}

		std::shared_lock lock(g_audioMutex);

		const uint32 channels = g_tvAudio ? g_tvAudio->GetChannels() : AX_TV_CHANNEL_COUNT;
		sint16* outputChannel = tempTVChannelData + AX_SAMPLES_PER_3MS_48KHZ * tempAudioBlockCounter * channels;
		for (sint32 i = 0; i < sampleCount; ++i)
		{
			outputChannel[i] = _swapEndianS16(sampleData[i]);
		}

		tempAudioBlockCounter++;
		if (tempAudioBlockCounter == AX_FRAMES_PER_GROUP)
		{
			if(g_tvAudio)
				g_tvAudio->FeedBlock(tempTVChannelData);

			tempAudioBlockCounter = 0;
		}
	}

	sint32 AIGetSamplesPerChannel(uint32 device)
	{
		// TV and DRC output the same number of samples
		return AX_SAMPLES_PER_3MS_48KHZ;
	}

	sint32 AIGetChannelCount(uint32 device)
	{
		if (__AXMode[device] == AX_MODE_6CH)
			return 6;
		if (__AXMode[device] == AX_MODE_STEREO)
			return 2;
		// default to mono
		return 1;
	}

	sint16* AIGetCurrentDMABuffer(uint32 device)
	{
		if (device == AX_DEV_TV)
			return __AXTVDMABuffers[0];
		else if (device == AX_DEV_DRC)
			return __AXDRCDMABuffers[0];
		cemu_assert_debug(false);
		return nullptr;
	}

	void AXOut_SubmitTVFrame(sint32 frameIndex)
	{
		sint32 numSamples = AIGetSamplesPerChannel(AX_DEV_TV);
		if (__AXMode[AX_DEV_TV] == AX_MODE_6CH)
		{
			sint32* inputChannel0 = __AXTVBuffer48.GetPtr() + numSamples * 0;
			sint32* inputChannel1 = __AXTVBuffer48.GetPtr() + numSamples * 1;
			sint32* inputChannel2 = __AXTVBuffer48.GetPtr() + numSamples * 2;
			sint32* inputChannel3 = __AXTVBuffer48.GetPtr() + numSamples * 3;
			sint32* inputChannel4 = __AXTVBuffer48.GetPtr() + numSamples * 4;
			sint32* inputChannel5 = __AXTVBuffer48.GetPtr() + numSamples * 5;
			sint16* dmaOutputBuffer = AIGetCurrentDMABuffer(AX_DEV_TV);
			for (sint32 i = 0; i < numSamples; i++)
			{
				/*
				* DirectSound surround order
				LEFT				0
				RIGHT				1
				SUR_LEFT			2
				SUR_RIGHT			3
				CH_FC				4
				CH_LFE				5
				=>
				Front Left - FL			0
				Front Right - FR		1
				Front Center - FC		2
				Low Frequency - LF		3
				Back Left - BL			4
				Back Right - BR			5
				*/
				dmaOutputBuffer[0] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel0), -32768), 32767));
				dmaOutputBuffer[1] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel1), -32768), 32767));

				dmaOutputBuffer[4] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel2), -32768), 32767));
				dmaOutputBuffer[5] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel3), -32768), 32767));

				dmaOutputBuffer[2] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel4), -32768), 32767));
				dmaOutputBuffer[3] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel5), -32768), 32767));
				dmaOutputBuffer += 6;
				// next sample
				inputChannel0++;
				inputChannel1++;
				inputChannel2++;
				inputChannel3++;
				inputChannel4++;
				inputChannel5++;
			}
			AIInitDMA(__AXTVDMABuffers[frameIndex], numSamples * 6 * sizeof(sint16)); // 6ch output
		}
		else if (__AXMode[AX_DEV_TV] == AX_MODE_STEREO)
		{
			// Thor: games (e.g. BO2) still route voices to the surround/center buses even when the TV is stereo.
			// Real AX folds those into L/R; Cemu used to drop them (no gunshots / zombie audio). Fold them unless
			// a game-supplied remix matrix already produced the stereo output.
			extern bool g_axTVRemixApplied;
			sint32* in = __AXTVBuffer48.GetPtr();
			sint16* dmaOutputBuffer = __AXTVDMABuffers[frameIndex];
			const bool fold = !g_axTVRemixApplied;
			static uint32 s_diagFrames = 0;
			static double s_energy[6] = {};
			for (sint32 i = 0; i < numSamples; i++)
			{
				float fl = (float)_swapEndianS32(in[i]);
				float fr = (float)_swapEndianS32(in[numSamples + i]);
				if (fold)
				{
					float sl = (float)_swapEndianS32(in[numSamples * 2 + i]);
					float sr = (float)_swapEndianS32(in[numSamples * 3 + i]);
					float fc = (float)_swapEndianS32(in[numSamples * 4 + i]);
					float lfe = (float)_swapEndianS32(in[numSamples * 5 + i]);
					if (s_diagFrames < 0xFFFFFFF0)
					{
						s_energy[0] += fl * fl; s_energy[1] += fr * fr; s_energy[2] += sl * sl;
						s_energy[3] += sr * sr; s_energy[4] += fc * fc; s_energy[5] += lfe * lfe;
					}
					fl += 0.7071f * (fc + sl) + 0.5f * lfe;
					fr += 0.7071f * (fc + sr) + 0.5f * lfe;
				}
				dmaOutputBuffer[0] = _swapEndianS16((sint16)std::clamp((sint32)fl, -32768, 32767));
				dmaOutputBuffer[1] = _swapEndianS16((sint16)std::clamp((sint32)fr, -32768, 32767));
				dmaOutputBuffer += 2;
			}
			if (fold && ++s_diagFrames % 3333 == 0) // ~10s of 3ms frames
			{
				double n = (double)numSamples * 3333.0;
				cemuLog_log(LogType::Force, "AXOut stereo fold rms FL={:.0f} FR={:.0f} SL={:.0f} SR={:.0f} FC={:.0f} LFE={:.0f}",
					sqrt(s_energy[0] / n), sqrt(s_energy[1] / n), sqrt(s_energy[2] / n), sqrt(s_energy[3] / n), sqrt(s_energy[4] / n), sqrt(s_energy[5] / n));
				for (auto& e : s_energy) e = 0;
				extern double g_axDrcFoldEnergy; extern uint64 g_axDrcFoldSamples; extern float g_axDrcFoldGain;
				cemuLog_log(LogType::Force, "AX DRC->TV fold: padStream {} padVol {} gain {:.2f} drc rms {:.0f}", g_padAudio ? 1 : 0, g_padVolume.load(), g_axDrcFoldGain, g_axDrcFoldSamples ? sqrt(g_axDrcFoldEnergy * 65536.0 / g_axDrcFoldSamples) : 0.0);
				g_axDrcFoldEnergy = 0; g_axDrcFoldSamples = 0;
				extern double g_axDrcAuxInE[3], g_axDrcAuxOutE[3]; extern bool g_axDrcAuxHasCb[3]; extern uint32 g_axDrcDryUsed[3]; extern uint64 g_axDrcMeterSamples; extern sint32 g_axDrcFoldMode;
				{
					double n = g_axDrcMeterSamples ? (double)g_axDrcMeterSamples : 1.0;
					cemuLog_log(LogType::Force, "AX DRC aux mode {}: A cb{} in {:.0f} out {:.0f} dry{} | B cb{} in {:.0f} out {:.0f} dry{} | C cb{} in {:.0f} out {:.0f} dry{}", g_axDrcFoldMode,
						g_axDrcAuxHasCb[0], sqrt(g_axDrcAuxInE[0] * 65536.0 / n), sqrt(g_axDrcAuxOutE[0] * 65536.0 / n), g_axDrcDryUsed[0],
						g_axDrcAuxHasCb[1], sqrt(g_axDrcAuxInE[1] * 65536.0 / n), sqrt(g_axDrcAuxOutE[1] * 65536.0 / n), g_axDrcDryUsed[1],
						g_axDrcAuxHasCb[2], sqrt(g_axDrcAuxInE[2] * 65536.0 / n), sqrt(g_axDrcAuxOutE[2] * 65536.0 / n), g_axDrcDryUsed[2]);
					for (int i = 0; i < 3; i++) { g_axDrcAuxInE[i] = 0; g_axDrcAuxOutE[i] = 0; g_axDrcDryUsed[i] = 0; }
					g_axDrcMeterSamples = 0;
				}
				extern uint32 g_axLpfStatVoices, g_axLpfStatOn, g_axBiquadStatOn; extern uint64 g_axLpfStatA0Sum; extern bool g_axLpfLegacy;
				cemuLog_log(LogType::Force, "AX filters: voiceFrames {} lpfOn {} (avg a0 {}) biquadOn {} lpfMode {}", g_axLpfStatVoices, g_axLpfStatOn,
					g_axLpfStatOn ? (uint32)(g_axLpfStatA0Sum / g_axLpfStatOn) : 0, g_axBiquadStatOn, g_axLpfLegacy ? "legacy" : "fixed");
				g_axLpfStatVoices = g_axLpfStatOn = g_axBiquadStatOn = 0; g_axLpfStatA0Sum = 0;
			}
			AXOut_DumpFrame(__AXTVDMABuffers[frameIndex], numSamples);
			AIInitDMA(__AXTVDMABuffers[frameIndex], numSamples * 2 * sizeof(sint16)); // 2ch output
		}
		else if (__AXMode[AX_DEV_TV] == AX_MODE_MONO)
		{
			sint32* inputChannel0 = __AXTVBuffer48.GetPtr() + numSamples * 0;
			sint16* dmaOutputBuffer = __AXTVDMABuffers[frameIndex];
			for (sint32 i = 0; i < numSamples; i++)
			{
				dmaOutputBuffer[0] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel0), -32768), 32767));
				dmaOutputBuffer++;
				// next sample
				inputChannel0++;
			}
			AIInitDMA(__AXTVDMABuffers[frameIndex], numSamples * 1 * sizeof(sint16)); // 1ch (output as stereo)
		}
		else
			assert_dbg();
	}

	void AIInitDRCDMA(sint16* sampleData, sint32 size)
	{
		sint32 sampleCount = size / sizeof(sint16); // sample count in total (summed up for all channels)

		if (sndGeneric.initParam.frameLength != 0)
		{
			cemu_assert(false);
		}

		std::shared_lock lock(g_audioMutex);

		const uint32 channels = g_padAudio ? g_padAudio->GetChannels() : AX_DRC_CHANNEL_COUNT;
		sint16* outputChannel = tempDRCChannelData + AX_SAMPLES_PER_3MS_48KHZ * tempDRCAudioBlockCounter * channels;
		for (sint32 i = 0; i < sampleCount; ++i)
		{
			outputChannel[i] = _swapEndianS16(sampleData[i]);
		}

		tempDRCAudioBlockCounter++;
		if (tempDRCAudioBlockCounter == AX_FRAMES_PER_GROUP)
		{
			if (g_padAudio)
				g_padAudio->FeedBlock(tempDRCChannelData);

			tempDRCAudioBlockCounter = 0;
		}
	}

	void AXOut_SubmitDRCFrame(sint32 frameIndex)
	{
		sint32 numSamples = AIGetSamplesPerChannel(AX_DEV_DRC);
		if (__AXMode[AX_DEV_DRC] == AX_MODE_6CH)
		{
			sint32* inputChannel0 = __AXDRCBuffer48.GetPtr() + numSamples * 0;
			sint32* inputChannel1 = __AXDRCBuffer48.GetPtr() + numSamples * 1;
			sint32* inputChannel2 = __AXDRCBuffer48.GetPtr() + numSamples * 2;
			sint32* inputChannel3 = __AXDRCBuffer48.GetPtr() + numSamples * 3;
			sint16* dmaOutputBuffer = AIGetCurrentDMABuffer(AX_DEV_DRC);
			for (sint32 i = 0; i < numSamples; i++)
			{
				dmaOutputBuffer[0] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel0), -32768), 32767));
				dmaOutputBuffer[1] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel1), -32768), 32767));

				dmaOutputBuffer[4] = 0;
				dmaOutputBuffer[5] = 0;

				dmaOutputBuffer[2] = 0;
				dmaOutputBuffer[3] = 0;
				dmaOutputBuffer += 6;
				// next sample
				inputChannel0++;
				inputChannel1++;
				inputChannel2++;
				inputChannel3++;
			}
			AIInitDRCDMA(__AXDRCDMABuffers[frameIndex], numSamples * 6 * sizeof(sint16)); // 6ch output
		}
		else if (__AXMode[AX_DEV_DRC] == AX_MODE_STEREO)
		{
			sint32* inputChannel0 = __AXDRCBuffer48.GetPtr() + numSamples * 0;
			sint32* inputChannel1 = __AXDRCBuffer48.GetPtr() + numSamples * 1;
			sint16* dmaOutputBuffer = __AXDRCDMABuffers[frameIndex];
			for (sint32 i = 0; i < numSamples; i++)
			{
				dmaOutputBuffer[0] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel0), -32768), 32767));
				dmaOutputBuffer[1] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel1), -32768), 32767));
				dmaOutputBuffer += 2;
				// next sample
				inputChannel0++;
				inputChannel1++;
			}

			AIInitDRCDMA(__AXDRCDMABuffers[frameIndex], numSamples * 2 * sizeof(sint16)); // 2ch output
		}
		else if (__AXMode[AX_DEV_DRC] == AX_MODE_MONO)
		{
			sint32* inputChannel0 = __AXDRCBuffer48.GetPtr() + numSamples * 0;
			sint16* dmaOutputBuffer = __AXDRCDMABuffers[frameIndex];
			for (sint32 i = 0; i < numSamples; i++)
			{
				// write mono input as stereo output
				dmaOutputBuffer[1] = dmaOutputBuffer[0] = _swapEndianS16((sint16)std::min(std::max(_swapEndianS32(*inputChannel0), -32768), 32767));
				dmaOutputBuffer += 2;
				// next sample
				inputChannel0++;
			}
			AIInitDRCDMA(__AXDRCDMABuffers[frameIndex], numSamples * 2 * sizeof(sint16)); // 1ch (output as stereo)
		}
		else
			assert_dbg();
	}

	/* AX output */

	uint32 numQueuedFramesSndGeneric = 0;

	void AXOut_init()
	{

		numQueuedFramesSndGeneric = 0;

		std::unique_lock lock(g_audioMutex);
		if (!g_tvAudio)
		{
			try
			{
				g_tvAudio = IAudioAPI::CreateDeviceFromConfig(IAudioAPI::AudioType::TV, 48000, snd_core::AX_SAMPLES_PER_3MS_48KHZ * AX_FRAMES_PER_GROUP, 16);
			}
			catch (std::runtime_error& ex)
			{
				cemuLog_log(LogType::Force, "can't initialize tv audio: {}", ex.what());
			}
		}

		g_padVolume = GetConfig().pad_volume;
		if (!g_padAudio)
		{
			try
			{
				g_padAudio = IAudioAPI::CreateDeviceFromConfig(IAudioAPI::AudioType::Gamepad, 48000, snd_core::AX_SAMPLES_PER_3MS_48KHZ * AX_FRAMES_PER_GROUP, 16);
			}
			catch (std::runtime_error& ex)
			{
				cemuLog_log(LogType::Force, "can't initialize pad audio: {}", ex.what());
			}
		}
	}

	void AXOut_reset()
	{
		std::unique_lock lock(g_audioMutex);
		if (g_tvAudio)
		{
			g_tvAudio->Stop();
			g_tvAudio.reset();
		}
		if (g_padAudio)
		{
			g_padAudio->Stop();
			g_padAudio.reset();
		}
		if (g_portalAudio)
		{
			g_portalAudio->Stop();
			g_portalAudio.reset();
		}
	}

	void AXOut_updateDevicePlayState(bool isPlaying)
	{
		std::shared_lock lock(g_audioMutex);
		if (g_tvAudio)
		{
			if (isPlaying)
				g_tvAudio->Play();
			else
				g_tvAudio->Stop();
		}

		if (g_padAudio)
		{
			if (isPlaying)
				g_padAudio->Play();
			else
				g_padAudio->Stop();
		}

		if (g_portalAudio)
		{
			if (isPlaying)
				g_portalAudio->Play();
			else
				g_portalAudio->Stop();
		}
	}

	// called periodically to check for AX updates
	void AXOut_update()
	{
		constexpr static auto kTimeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(((IAudioAPI::kBlockCount * 3) / 4) * (AX_FRAMES_PER_GROUP * 3)));
		constexpr static auto kWaitDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds(3));
		constexpr static auto kWaitDurationFast = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::microseconds(2900));
		constexpr static auto kWaitDurationMinimum = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::microseconds(1700));

		// if we haven't buffered any blocks, we will wait less time than usual
		bool additional_blocks_required = false;
		{
			const std::shared_lock lock(g_audioMutex, std::try_to_lock);
			if (lock)
				additional_blocks_required = (g_tvAudio && g_tvAudio->NeedAdditionalBlocks()) || (g_padAudio && g_padAudio->NeedAdditionalBlocks());
		}

		const auto wait_duration = additional_blocks_required ? kWaitDurationFast : kWaitDuration;

		// s_ax_interval_timer increases by the wait period
		// it can lag behind by multiple periods (up to kTimeout) if there is minor stutter in the CPU thread
		// s_last_check is always set to the timestamp at the time of firing
		// it's used to enforce the minimum wait delay (we want to avoid calling AX update in quick succession because other threads may need to do work first) 

		static auto s_ax_interval_timer = now_cached() - kWaitDuration;
		static auto s_last_check = now_cached();

		const auto now = now_cached();
		const auto diff = (now - s_ax_interval_timer);

		if (diff < wait_duration)
			return;

		// handle minimum wait time (1.7MS)
		if ((now - s_last_check) < kWaitDurationMinimum)
			return;
		s_last_check = now;

		// if we're too far behind, skip forward
		if (diff >= kTimeout)
			s_ax_interval_timer = (now - wait_duration);
		else
			s_ax_interval_timer += wait_duration;


		if (snd_core::isInitialized())
		{
			if (numQueuedFramesSndGeneric == snd_core::getNumProcessedFrames())
			{
				AXOut_updateDevicePlayState(true);
				snd_core::AXIst_QueueFrame();
				numQueuedFramesSndGeneric++;
			}
		}
	}

}
