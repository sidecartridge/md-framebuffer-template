/* Host stand-in for FatFs's ff.h: the types and calls the firmware units
 * under test use. The test implements them over files in memory. */
#ifndef HOST_SHIM_FF_H
#define HOST_SHIM_FF_H

#include <stdint.h>

typedef unsigned int UINT;
typedef uint32_t FSIZE_t;
typedef unsigned char BYTE;
typedef enum { FR_OK = 0, FR_DISK_ERR, FR_NO_FILE = 4 } FRESULT;
#define FA_READ 0x01

typedef struct {
  const BYTE *data;
  FSIZE_t size;
  FSIZE_t pos;
} FIL;

FRESULT f_open(FIL *fp, const char *path, BYTE mode);
FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT f_close(FIL *fp);

#endif
