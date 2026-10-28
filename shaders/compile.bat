@echo off
rem Recompiles shaders into src\generated\shaders_spv.h. Needs glslangValidator (Vulkan SDK).
rem Only needed if you edit the .vert/.frag files. Requires Python.
python "%~dp0..\tools\compile_shaders.py"
