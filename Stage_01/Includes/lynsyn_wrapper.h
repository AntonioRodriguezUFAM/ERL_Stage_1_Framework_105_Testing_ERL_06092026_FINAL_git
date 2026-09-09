// lynsyn_wrapper.h
/*
Alternative Ways to Call lynsyn_reset
If the extern "C" fix doesn?t resolve the issue, or you want alternative approaches to call lynsyn_reset, consider these options:

Wrap in a C++ Class:
Create a C++ wrapper class for lynsyn functions to encapsulate the C interface:

*/
// lynsyn_wrapper.h
#ifndef LYNSYN_WRAPPER_H
#define LYNSYN_WRAPPER_H

#include "lynsyn.h"

class LynsynWrapper {
public:
    static int reset() {
        return lynsyn_reset();
    }
    static bool init() {
        return lynsyn_init();
    }
    // Add other functions as needed
};

#endif