#!/bin/bash
cd ..
apt-get -y install libssl-dev
wget https://github.com/Kitware/CMake/releases/download/v3.29.0-rc1/cmake-3.29.0-rc1.tar.gz
tar -xzvf cmake-3.29.0-rc1.tar.gz
cd cmake-3.29.0-rc1 && ./bootstrap && make -j$(nproc) && make install
cd ../llama.cpp/models
wget https://huggingface.co/TheBloke/Mixtral-8x7B-v0.1-GGUF/resolve/main/mixtral-8x7b-v0.1.Q6_K.gguf
mkdir ../build && cmake -S .. -B ../build -DLLAMA_CUBLAS=ON
cmake --build ../build --config Release
../build/bin/server -m mixtral-8x7b-v0.1.Q6_K.gguf --port 8081 -c 2048 --host 172.17.0.2 -ngl 33 --log-format text
