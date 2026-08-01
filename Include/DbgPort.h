#ifndef _DBG_PORT_H_
#define _DBG_PORT_H_

#include <Uefi.h>
#include <Library/IoLib.h>



//
// debugcon (0x402) на реальном железе физически не существует Ч запись
// туда уходит в никуда без ошибок, но и результата не даЄт. ѕараллельно
// копим текст в буфер; SegaMain.c периодически сбрасывает его в файл на
// том же томе, откуда запущено приложение (сама пам€ть буфера определена
// в SegaMain.c, здесь только extern).
//
#define DBG_LOG_BUFFER_SIZE (64 * 1024)
extern CHAR8 gDbgLogBuffer[DBG_LOG_BUFFER_SIZE];
extern UINTN gDbgLogPos;

STATIC VOID DbgLogAppend(CHAR8 C) {
    if (gDbgLogPos < DBG_LOG_BUFFER_SIZE - 1) {
        gDbgLogBuffer[gDbgLogPos++] = C;
    }
    // Ѕуфер переполнен Ч просто прекращаем писать. ѕроще один раз
    // сбросить в файл, чем городить сложный wrap-around.
}

STATIC VOID DbgPortChar(CHAR8 C) {
    IoWrite8(0x402, (UINT8)C);   // оставл€ем Ч бесплатно и работает под QEMU
    DbgLogAppend(C);             // и параллельно копим дл€ файла
}


//STATIC VOID DbgPortChar(CHAR8 C) {
//    IoWrite8(0x402, (UINT8)C);
//}

STATIC VOID DbgStr(CONST CHAR8* S) {
    while (S && *S) DbgPortChar(*S++);
}

STATIC VOID DbgHex64(UINT64 V) {
    CHAR8 Buf[19];
    CHAR8 Hex[] = "0123456789ABCDEF";
    Buf[0] = '0';
    Buf[1] = 'x';
    for (INT32 i = 17; i >= 2; i--) {
        Buf[i] = Hex[V & 0xF];
        V >>= 4;
    }
    Buf[18] = '\0';
    DbgStr(Buf);
}

STATIC VOID DbgHex32(UINT32 V) {
    DbgHex64((UINT64)V);
}

STATIC VOID DbgDec(UINT64 V) {
    CHAR8 Buf[22];
    UINTN Pos = 20;
    Buf[21] = '\0';
    if (V == 0) { DbgPortChar('0'); return; }
    while (V > 0 && Pos > 0) {
        Buf[Pos--] = (CHAR8)('0' + (V % 10));
        V /= 10;
    }
    DbgStr(&Buf[Pos + 1]);
}

STATIC VOID DbgStatus(EFI_STATUS Status) {
    DbgHex64((UINT64)Status);
}

STATIC VOID DbgCrLf(VOID) {
    DbgPortChar('\r');
    DbgPortChar('\n');
}

#define DBG(s)          do { DbgStr("[SEG] " s); DbgCrLf(); } while(0)
#define DBG_RAW(s)      DbgStr(s)
#define DBG_HEX(lbl,v)  do { DbgStr("[SEG] " lbl ": "); DbgHex64((UINT64)(v)); DbgCrLf(); } while(0)
#define DBG_DEC(lbl,v)  do { DbgStr("[SEG] " lbl ": "); DbgDec((UINT64)(v)); DbgCrLf(); } while(0)

#define DBG_STATUS_TAG(lbl,st) do { DbgStr("[SEG] " lbl " status: "); DbgStatus(st); DbgCrLf(); } while(0)

#endif /* _DBG_PORT_H_ */
