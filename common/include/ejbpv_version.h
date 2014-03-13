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

#ifndef EJBPV_VERSION_H
#define EJBPV_VERSION_H

#define EXPAND(x) STRINGIFY(x)
#define STRINGIFY(x) #x

#define VER_FILETYPE                VFT_DRV
#define VER_FILESUBTYPE             VFT2_DRV_SYSTEM
#ifdef DEBUG
  #define VER_FILEDESCRIPTION_STR     EJBPV_DRIVER_DESCRIPTION 
#else
  #define VER_FILEDESCRIPTION_STR     EJBPV_DRIVER_DESCRIPTION " (Checked Build)"
#endif
#define VER_INTERNALNAME_STR        EJBPV_DRIVER_FILENAME 
#define VER_ORIGINALFILENAME_STR    EJBPV_DRIVER_FILENAME 

#ifdef VERSION_MAJOR
  #ifdef BUILD_NUMBER
    #define VER_FILEVERSION             VERSION_MAJOR,VERSION_MINOR,REVISION,BUILD_NUMBER
    #define VER_FILEVERSION_STR         "EJBPV " EXPAND(VERSION_MAJOR) "." EXPAND(VERSION_MINOR) "." EXPAND(REVISION) "." EXPAND(BUILD_NUMBER)
  #else
    #define VER_FILEVERSION             VERSION_MAJOR,VERSION_MINOR,REVISION,0
    #define VER_FILEVERSION_STR         "EJBPV " EXPAND(VERSION_MAJOR) "." EXPAND(VERSION_MINOR) "." EXPAND(REVISION)
  #endif
#else
  #define VER_FILEVERSION             0,0,0,0
  #define VER_FILEVERSION_STR         "EJBPV Unversioned"
#endif

#undef VER_PRODUCTVERSION
#define VER_PRODUCTVERSION          VER_FILEVERSION
#undef VER_PRODUCTVERSION_STR
#define VER_PRODUCTVERSION_STR      VER_FILEVERSION_STR
#define VER_LEGALCOPYRIGHT_STR      "Copyright (C) 2014 James Harper" 

#ifdef VER_COMPANYNAME_STR
#undef VER_COMPANYNAME_STR
#define VER_COMPANYNAME_STR         "James Harper"
#endif
#undef VER_PRODUCTNAME_STR
#define VER_PRODUCTNAME_STR         "PV Drivers for Windows"

#endif
