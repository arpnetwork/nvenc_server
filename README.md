# nvenc-server

nvenc-server is a H.264 encoding server by using Nvidia GPU.

## Prerequisites

   * `gcc` 4.7 or later
   * GNU `make` 3.81 or later
   * `cmake` 2.8.7 or later
   * `git`
   * `libnvidia-encode`
   * [`nanomsg`](https://github.com/nanomsg/nanomsg) 1.0 or later
   * [`ffmpeg`](http://ffmpeg.org) 3.2 or later (with h264_nvenc encoder enabled)

## Building

   ```bash
   $ git clone https://github.com/arpnetwork/nvenc-server.git
   $ cd nvenc-server
   $ mkdir build
   $ cd build
   $ cmake ..
   $ cmake --bulid .
   ```
