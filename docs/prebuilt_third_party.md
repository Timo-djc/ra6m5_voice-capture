# Prebuilt Third-Party Libraries

This repo now supports a separate Keil project that links prebuilt `.lib` files for:

- `TensorFlow Lite Micro`
- `CMSIS-NN`
- `CMSIS-DSP`

## Files

- `tools/build_prebuilt_libs.py`
  Builds `prebuilt/lib/tflm.lib`, `prebuilt/lib/cmsis_nn.lib`, `prebuilt/lib/cmsis_dsp.lib`
- `tools/build_prebuilt_libs.ps1`
  Keil-friendly wrapper for the Python builder
- `tools/generate_prebuilt_project.py`
  Regenerates `A_i2s_capture_prebuilt.uvprojx` from the main project
- `A_i2s_capture_prebuilt.uvprojx`
  Project copy that links the prebuilt libraries instead of recompiling third-party sources

## First Use

1. Open `A_i2s_capture_prebuilt.uvprojx`
2. Build the target `Target 1 (Prebuilt Libs)`

The target runs `tools/build_prebuilt_libs.ps1` in `Before Make`.

## Notes

- The builder reuses the compile options and source list already captured in `Objects/A_i2s_capture_Target 1.dep`.
- If the builder reports that it cannot discover third-party sources, build the original `A_i2s_capture.uvprojx` target once to refresh `Objects/A_i2s_capture_Target 1.dep`, then rebuild the prebuilt target.
- If you add or remove TFLM/CMSIS source usage later, regenerate the project with:

```powershell
python .\tools\generate_prebuilt_project.py
```
