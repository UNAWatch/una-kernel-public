/*
 * gcc simulator build only: the generated Makefile compiles every .c file
 * under simulator/, but nothing outside the TouchGFX project. The MSVS
 * project lists ThirdParty/Nayuki/qrcodegen.c itself and skips this file.
 */
#include "../../../ThirdParty/Nayuki/qrcodegen.c"
