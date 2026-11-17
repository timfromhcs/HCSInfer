call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d D:\HCSInfer
cmake --build build/sd-upstream-ggml --parallel -- -k 0 > docs\test-evidence\M1\sd\sd-upstream-build.log 2>&1
echo build exit %ERRORLEVEL% >> docs\test-evidence\M1\sd\sd-upstream-build.log
