lz4ultra -- Optimal LZ4 packer with faster decompression
========================================================

lz4ultra is a command-line optimal compression utility that produces compressed files in the [lz4](https://github.com/lz4/lz4) format created by Yann Collet.

The tool creates optimally compressed files, like lz4 in optimal compression mode ("lz4hc"), smallLZ4, blz4 and lz4x.

With enwik9 (1,000,000,000 bytes):

                                      Compr.size    Tokens      Decomp.time (μs, Core i7-6700)
    lz4 1.9.2 -12 (favor ratio)       372,443,347   95,698,349  505,804
    smalLZ4 1.5 -9                    371,680,328   93,172,985  348,018
    lz4ultra 1.3.0 (favor ratio)      371,680,323   93,165,899  347,936
    lz4 1.9.2 -12 --favor-decSpeed    377,175,400   92,080,802  457,141
    lz4ultra 1.3.0 --favor-decSpeed   376,118,079   88,521,993  296,972

The produced files are meant to be decompressed with the lz4 tool and library. While lz4ultra includes a decompressor, it is mostly meant to verify the output of the compressor and isn't as optimized as Yann Collet's lz4 proper.

The tool defaults to 4 Mb blocks with inter-block dependencies but can be configured to output all of the LZ4 block sizes (64 Kb to 4 Mb), to use the LZ4 8 Mb blocks legacy encoding, and to compress independent blocks, using command-line switches.

lz4ultra is developed by Emmanuel Marty with the help of spke.
