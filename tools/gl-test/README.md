# g15-gl-triangle

A GLES 3.0 test client for the G15 Mesa driver. Each `--mode` draws a known
scene (triangles, clears, reloads, depth, stencil, mip levels, overdraw,
stress grids, external textures, ...) into an offscreen target, reads it back
and checks it against a CPU oracle (`render-ref.h`) or against llvmpipe.

    make
    ASAHI_G15_GRAPHICS=1 LD_LIBRARY_PATH=$G15_MESA/lib ./g15-gl-triangle --mode const
    ./g15-gl-triangle --driver software --mode const     # llvmpipe reference

`--help` lists the modes and options. Each run prints one `G15_GL_*` result
line (PASS/FAIL) and exits nonzero on failure.
