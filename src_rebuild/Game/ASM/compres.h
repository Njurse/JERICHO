#ifndef COMPRES_H
#define COMPRES_H

extern char* decomp_asm(char* dest, char* src); // 0x0002D298
extern char* decomp_asm_n(char* dest, char* src, int srcLen);	// bounded: NULL if src runs out

#endif
