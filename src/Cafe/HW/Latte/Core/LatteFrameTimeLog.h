#pragma once

// writes one line per presented frame to frametimes.csv if frametimes.enable exists in the user data folder
void LatteFrameTimeLog_OnSwap(bool newFrame, uint64 shaderCreateTimeUs);
