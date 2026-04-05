#ifndef TOYHOOK_ARCH_ARM64_H
#define TOYHOOK_ARCH_ARM64_H

/* --- General-purpose registers --- */

enum {
    X0  =  0, X1  =  1, X2  =  2,  X3  =  3,
    X4  =  4, X5  =  5, X6  =  6,  X7  =  7,
    X8  =  8, X9  =  9, X10 = 10, X11 = 11,
    X12 = 12, X13 = 13, X14 = 14, X15 = 15,
    X16 = 16, X17 = 17, X18 = 18, X19 = 19,
    X20 = 20, X21 = 21, X22 = 22, X23 = 23,
    X24 = 24, X25 = 25, X26 = 26, X27 = 27,
    X28 = 28, FP  = 29, LR  = 30, SP  = 31,
    XZR = 31
};

/* --- Condition codes (B.cond / CSEL / CCMP ...) --- */

enum {
    COND_EQ = 0,  COND_NE = 1,  COND_CS = 2,  COND_CC = 3,
    COND_MI = 4,  COND_PL = 5,  COND_VS = 6,  COND_VC = 7,
    COND_HI = 8,  COND_LS = 9,  COND_GE = 10, COND_LT = 11,
    COND_GT = 12, COND_LE = 13, COND_AL = 14, COND_NV = 15
};

#endif
