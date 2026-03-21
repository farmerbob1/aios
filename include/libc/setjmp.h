#ifndef _AIOS_SETJMP_H
#define _AIOS_SETJMP_H

/* jmp_buf: EBX, ESI, EDI, EBP, ESP, EIP (6 x 4 = 24 bytes) */
typedef unsigned int jmp_buf[6];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif
