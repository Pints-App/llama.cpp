#!/bin/bash
cmake -S .. -B ../build -DLLAMA_CUBLAS=OFF
cmake --build ../build --config Release
../build/bin/server -m mixtral-instruct-8x7b-q2k.gguf --port 8081 -t 1 -c 512 --host 172.17.0.2 --log-format text
