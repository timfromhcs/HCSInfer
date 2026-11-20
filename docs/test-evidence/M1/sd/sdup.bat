call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d <REPO>
cmake -S external/stable-diffusion.cpp -B build/sd-upstream-ggml -G Ninja -DSD_VULKAN=ON -DSD_USE_UPSTREAM_GGML=ON -DSD_GGML_SOURCE_DIR=<REPO>/external/llama.cpp/ggml -DSD_BUILD_EXAMPLES=ON -DSD_BUILD_SHARED_LIBS=OFF -DSD_SERVER_BUILD_FRONTEND=OFF -DCMAKE_BUILD_TYPE=Release > docs\test-evidence\M1\sd\sd-upstream-configure.log 2>&1
echo configure exit %ERRORLEVEL% >> docs\test-evidence\M1\sd\sd-upstream-configure.log
