lz4ultra -- Optimal LZ4 packer with faster decompression
========================================================

lz4ultra is a command-line optimal compression utility that produces compressed files in the [lz4](https://github.com/lz4/lz4) format created by Yann Collet.

The tool creates optimally compressed files, like lz4 in optimal compression mode ("lz4hc"), smallLZ4, blz4 and lz4x. The files decompress slightly faster.

lz4ultra beats lz4 1.9.1 --12 --favor-decSpeed in both size and the number of tokens produced. With enwik9 (1,000,000,000 bytes):

                                      Compr.size    Tokens
    lz4 1.9.1 --12 --favor-decSpeed   376,408,347   92,105,212
    lz4ultra 1.1.0                    371,687,509   85,910,002

The produced files are meant to be decompressed with the lz4 tool and library. While lz4ultra includes a decompressor, it is mostly meant to verify the output of the compressor and isn't as optimized as Yann Collet's lz4 proper.

lz4ultra works by performing the usual optimal compression (using a suffix array), and then applying a forward peephole optimization pass, that breaks ties (sequences of compression commands that result in an identical number of bytes added to the compressed data), in favor of outputting less commands.

In other words, it will go over the compressed data before it is flattened into the output bytestream, and possibly replace match->match and match->literal sequences by a literal->match or just literal sequences, if the output size is unchanged.

Everything else being equal, having less commands speeds up decompression as the decompressor is required to do less mode switches.

The peephole optimizer is located in src/shrink.c, as lz4ultra_optimize_command_count(). This pass is extremely fast and cache-friendly as it just goes over the compressed data once. The logic isn't tied to compressing with a suffix array, it can be adapted to any compressor.

lz4ultra is an offshoot of the [lzsa](https://github.com/emmanuel-marty/lzsa) compressor as we thought it would be interesting to the wider compression community, to users of LZ4 that compress data once and require the fastest decompression time without a trade-off in compression ratio, and also to retrocomputing developers as this will speed decompression up further on 8/16-bit systems.

The tool defaults to 4 Mb blocks with inter-block dependencies but can be configured to output all of the LZ4 block sizes (64 Kb to 4 Mb) and to compress independent blocks, using command-line switches.

lz4ultra is developed by Emmanuel Marty with the help of spke.
