#include "Cafe/HW/Latte/Core/LatteFrameTimeLog.h"
#include "config/ActiveSettings.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

static bool s_initialized = false;
static FileStream* s_logFile = nullptr;
static uint32 s_frameIndex = 0;
static uint64 s_lastFrameTime = 0;
static uint64 s_shaderCreateTimeUs = 0;

void LatteFrameTimeLog_OnSwap(bool newFrame, uint64 shaderCreateTimeUs)
{
	if (!s_initialized)
	{
		s_initialized = true;
		std::error_code ec;
		if (fs::exists(ActiveSettings::GetUserDataPath("frametimes.enable"), ec))
			s_logFile = FileStream::createFile2(ActiveSettings::GetUserDataPath("frametimes.csv"));
		if (s_logFile)
			s_logFile->writeString("frame,timestamp_us,frametime_us,shadercreate_us\n");
	}
	if (!s_logFile)
		return;
	s_shaderCreateTimeUs += shaderCreateTimeUs;
	if (!newFrame)
		return;
	uint64 now = HighResolutionTimer::ticksToMicroseconds(HighResolutionTimer::now().getTick());
	uint64 frameTime = s_lastFrameTime ? (now - s_lastFrameTime) : 0;
	s_lastFrameTime = now;
	s_logFile->writeString(fmt::format("{},{},{},{}\n", s_frameIndex, now, frameTime, s_shaderCreateTimeUs).c_str());
	s_shaderCreateTimeUs = 0;
	s_frameIndex++;
	if ((s_frameIndex % 120) == 0)
		s_logFile->Flush();
}
