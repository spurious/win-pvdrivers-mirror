/*
PV Drivers for Windows Xen HVM Domains

Copyright (c) 2014, James Harper
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of James Harper nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL JAMES HARPER BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "xenpci.h"

static BOOLEAN last_newline = TRUE;
static volatile LONG debug_print_lock = 0;

NTSTATUS
XenPci_DebugPrintV(PCHAR format, va_list args) {
  NTSTATUS status;
  KIRQL old_irql;
  CHAR buf[512]; /* truncate anything larger */
  ULONG i;
  ULONGLONG j;
  LARGE_INTEGER current_time;

  status = RtlStringCbVPrintfA(buf, ARRAY_SIZE(buf), format, args);  
  if (status != STATUS_SUCCESS)
    return status;
  DbgPrint("%s", buf);
  KeRaiseIrql(HIGH_LEVEL, &old_irql);
  /* make sure that each print gets to complete in its entirety */
  while(InterlockedCompareExchange(&debug_print_lock, 1, 0) == 1)
    KeStallExecutionProcessor(1);
  for (i = 0; i < strlen(buf); i++) {
    /* only write a timestamp if the last character was a newline */
    if (last_newline) {
      KeQuerySystemTime(&current_time);
      current_time.QuadPart /= 10000; /* convert to ms */
      for (j = 1000000000000000000L; j >= 1; j /= 10)
        if (current_time.QuadPart / j)
          break;
      for (; j >= 1; j /= 10) {
        #pragma warning(suppress:28138)
        WRITE_PORT_UCHAR(XEN_IOPORT_LOG, '0' + (UCHAR)((current_time.QuadPart / j) % 10));
      }
      #pragma warning(suppress:28138)
      WRITE_PORT_UCHAR(XEN_IOPORT_LOG, ':');
      #pragma warning(suppress:28138)
      WRITE_PORT_UCHAR(XEN_IOPORT_LOG, ' ');
    }
    #pragma warning(suppress:28138)
    WRITE_PORT_UCHAR(XEN_IOPORT_LOG, buf[i]);
    last_newline = (buf[i] == '\n');
  }
  /* release the lock */
  InterlockedExchange(&debug_print_lock, 0);
  KeLowerIrql(old_irql);
  return status;
}

NTSTATUS
XenPci_DebugPrint(PCHAR format, ...) {
  NTSTATUS status;
  va_list args;
  
  va_start(args, format);
  status = XenPci_DebugPrintV(format, args);
  va_end(args);
  return status;
}