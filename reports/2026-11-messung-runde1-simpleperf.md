# simpleperf, harbor, stock driver, small underclock, 60fps patch, 20 s
Threads (share of all cycles): OSSched[core=1] 44.4 %, LatteThread 35.8 %, OSSched[core=2] 18.1 %.
Total 1.0e11 cycles / 20 s = 5.0e9 cycles/s. core1 thread ~2.2e9 cycles/s = ~90 % of the 2.48 GHz prime core -> main bottleneck.
LatteThread ~1.8e9 cycles/s = ~75-78 % of a 2.32 GHz mid core.

OSSched[core=1] by dso: JIT code (unknown) 79.3 %, [vdso] clock_gettime 6.6 %, libCemuAndroid 6.3 %, libc 3.9 %, kernel 3.0 %.
LatteThread by dso: libCemuAndroid 42.2 %, vulkan.adreno 29.2 %, kernel 12.7 %, libc 11.1 %, vdso 3.2 %.
LatteThread top symbols: kernel 7.7 %, LatteIndices_fastConvertU16_NEON 5.7 %, LatteCP_processCommandBuffer 5.1 %, LatteCP_readU32Deprc 3.9 %,
  adreno internal 3.9 %, clock_gettime 3.2 %, LatteTexture_updateTexturesForStage 3.1 %, vkCmdBindDescriptorSets 2.9 %, draw_execute_first 2.8 %,
  LatteTexture_CalculateTextureDataHash 2.7 %, __aarch64_cas4_acq 2.0 % (outline atomics), vkCmdBindPipeline 1.7 %, memcpy 1.4 %.
__rdtsc on ARM already reads cntvct_el0, so clock_gettime comes from std::chrono based timers (HighResolutionTimer / GetTickCount); callers not resolved (no unwind through vdso).
