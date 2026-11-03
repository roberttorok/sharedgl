#ifndef _ICD_H_
#define _ICD_H_

#ifndef SGL_INTERNAL
#ifdef _WIN32
#define SGL_INTERNAL __cdecl /* glimpl.c is built with -mrtd so that GL entry points are stdcall */
#else
#define SGL_INTERNAL
#endif
#endif


#define ICD_SET_MAX_DIMENSIONS_DEFINITION(mw, mh, rw, rh) \
    void icd_set_max_dimensions(int width, int height) \
    { \
        mw = width; \
        mh = height; \
        rw = 16; \
        rh = 16; \
    }

#define ICD_RESIZE_DEFINITION(rw, rh) \
    void icd_resize(int width, int height) \
    { \
        rw = width; \
        rh = height; \
    } \

void SGL_INTERNAL icd_set_max_dimensions(int width, int height);
void SGL_INTERNAL icd_resize(int width, int height);

#endif