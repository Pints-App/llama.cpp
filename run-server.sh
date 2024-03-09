#!/bin/bash
cd ..
apt-get -y install libssl-dev
wget https://github.com/Kitware/CMake/releases/download/v3.29.0-rc1/cmake-3.29.0-rc1.tar.gz
tar -xzvf cmake-3.29.0-rc1.tar.gz
cd cmake-3.29.0-rc1 && ./bootstrap && make -j$(nproc) && make install
cd ../llama.cpp/models
wget https://huggingface.co/ikawrakow/mixtral-instruct-8x7b-quantized-gguf/resolve/main/mixtral-instruct-8x7b-q2k.gguf
mkdir ../build && cmake -S .. -B ../build -DLLAMA_CUBLAS=ON
cmake --build ../build --config Release
../build/bin/server -m mixtral-instruct-8x7b-q2k.gguf --port 8081 -c 512 --host 172.17.0.5 -ngl 32 --log-format text
