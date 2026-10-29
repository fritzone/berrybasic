/* Empty <math.h> shim for the freestanding build. stb_image_write.h #includes
   <math.h> unconditionally but references no floating-point functions in the
   PNG/BMP/TGA paths we use, so nothing needs to be declared here. */
#ifndef STB_SHIM_MATH_H
#define STB_SHIM_MATH_H
#endif
