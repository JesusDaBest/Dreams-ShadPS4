# Sources for the embedded optional volume shaders

The source delta includes `native_volume_spv.h` because it was compiled into the supplied EXE.
The corresponding emulator-authored GLSL is retained here. This optional renderer is disabled
by default and was disabled during the reported shape-fix test.

The header arrays were built with Vulkan SDK 1.4.350.0 using:

```text
glslangValidator -V --target-env vulkan1.1 -o enumerate_live.spv enumerate_live.comp
glslangValidator -V --target-env vulkan1.1 -o prepare_draws.spv prepare_draws.comp
```

Interpret each SPIR-V file as little-endian 32-bit words. The arrays are named `Enumerate`
and `PrepareDraws`, respectively, in namespace `Vulkan::HostPasses::NativeVolumeSpv`.
The included header is ready to compile; rebuilding these arrays is optional.
