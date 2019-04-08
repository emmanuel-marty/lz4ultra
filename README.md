lz4ultra -- Optimal LZ4 packer with faster decompression
========================================================

lz4ultra is a command-line optimal compression utility that produces compressed files in the [lz4](https://github.com/lz4/lz4) format created by Yann Collet.

The tool creates compressed files that decompress approximately 1% faster than files produced by lz4 in optimal compression mode ("lz4hc"), smalllz4 and blz4, and around 0.5% faster than files produced by lz4x.

The compression ratio is identical or nearly identical to lz4 in the highest optimal compression mode.

lz4ultra works by performing the usual optimal compression (using a suffix array), and then applying a forward peephole optimization pass, that breaks ties (sequences of compression commands that result in an identical number of bytes added to the compressed data), in favor of outputting less commands.

In other words, it will go over the compressed data before it is flattened into the output bytestream, and possibly replace match->match and match->literal sequences by a literal->match or just literal sequences, if the output size is unchanged.

Everything else being equal, having less commands speeds up decompression as the decompressor is required to do less mode switches.

The peephole optimizer is located in src/shrink.c, as lz4ultra_optimize_command_count(). This pass is extremely fast and cache-friendly as it just goes over the compressed data once. The logic isn't tied to compressing with a suffix array, it can be adapted to any compressor.

lz4ultra is an offshoot of the [lzsa](https://github.com/emmanuel-marty/lzsa) compressor as we thought it would be interesting to the wider compression community, to users of LZ4 that compress data once and require the fastest decompression time without a trade-off in compression ratio, and also to retrocomputing developers as this will speed decompression up further on 8/16-bit systems.

The tool currently outputs 64Kb blocks with inter-block dependencies and no frame checksums. It could easily be augmented to produce blocks of any size supported by the LZ4 format, and to add the checksums.
