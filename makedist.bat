@echo off
IF NOT EXIST set_ddk_path.bat ECHO >set_ddk_path.bat SET DDK_PATH=C:\WinDDK\7600.16385.0
IF NOT EXIST set_ddk_path_2k.bat ECHO >set_ddk_path_2k.bat SET DDK_PATH_2K=C:\WinDDK\6001.18002

SET BUILD_NUMBER=1
SET VERSION_MAJOR=1
SET VERSION_MINOR=0
for /f %%i in ('hg identify --num -r .') do set REVISION=%%i

hg status | findstr "^M" && GOTO TEST_BUILD
SET EJBPV_VERSION=%VERSION_MAJOR%.%VERSION_MINOR%.%REVISION%
GOTO DONE_VERSION

:TEST_BUILD
IF EXIST build_number.bat CALL build_number.bat
SET EJBPV_VERSION=%VERSION_MAJOR%.%VERSION_MINOR%.%REVISION%.%BUILD_NUMBER%

:DONE_VERSION

SET /A NEW_BUILD_NUMBER=%BUILD_NUMBER%+1
ECHO >build_number.bat SET BUILD_NUMBER=%NEW_BUILD_NUMBER%

ECHO BUILDING %EJBPV_VERSION%

CALL set_ddk_path.bat
CALL set_ddk_path_2K.bat

SET PV_DIR=%CD%

SET SIGNTOOL=%DDK_PATH%\bin\x86\signtool.exe
IF NOT EXIST %SIGNTOOL% SET SIGNTOOL=%DDK_PATH%\bin\selfsign\signtool.exe

SET CERT_FILENAME=
SET CERT_PASSWORD=
SET CERT_CROSS_CERT_FILENAME=
SET CERT_PUBLIC_FILENAME=
IF NOT EXIST SIGN_CONFIG.BAT GOTO DONT_SIGN
CALL SIGN_CONFIG.BAT
SET CERT_CROSS_CERT_FLAG=
SET CERT_PASSWORD_FLAG=
IF DEFINED CERT_CROSS_CERT_FILENAME SET CERT_CROSS_CERT_FLAG=/ac %CERT_CROSS_CERT_FILENAME%
IF DEFINED CERT_PASSWORD SET CERT_PASSWORD_FLAG=-p %CERT_PASSWORD%
IF EXIST %CERT_FILENAME% GOTO :DONT_SIGN
"%DDK_PATH%"\bin\x86\makecert -r -pe -ss PrivateCertStore -n "CN=EJBPV Test Cert" %CERT_PUBLIC_FILENAME%
certutil -exportpfx -user -privatekey %CERT_PASSWORD_FLAG% PrivateCertStore "EJBPV Test Cert" "%CERT_FILENAME%
:DONT_SIGN

mkdir symbols\%EJBPV_VERSION%

cmd /C "%DDK_PATH_2K%\bin\setenv.bat %DDK_PATH_2K%\ chk W2K && CD /D "%PV_DIR%" && build -cZg ~xenvbd_storport ~xenusb ~copyconfig ~waitnopendinginstallevents && call sign.bat && call archive.bat && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ chk WXP && CD /D "%PV_DIR%" && build -cZg ~xenvbd_storport && call sign.bat && call archive.bat && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ chk WNET && CD /D "%PV_DIR%" && build -cZg && call sign.bat && call archive.bat && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ chk x64 WNET && CD /D "%PV_DIR%" && build -cZg && call sign.bat && call archive.bat && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ chk WLH && CD /D "%PV_DIR%" && build -cZg && call sign.bat && call archive.bat && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ chk x64 WLH && CD /D "%PV_DIR%" && build -cZg && call sign.bat && call archive.bat && call wix.bat"

cmd /C "%DDK_PATH_2K%\bin\setenv.bat %DDK_PATH_2K%\ fre W2K && CD /D "%PV_DIR%" && build -cZg ~xenvbd_storport ~xenusb ~copyconfig ~waitnopendinginstallevents && call sign.bat && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ fre WXP && CD /D "%PV_DIR%" && build -cZg ~xenvbd_storport && call sign.bat && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ fre WNET && CD /D "%PV_DIR%" && build -cZg && call sign.bat && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ fre x64 WNET && CD /D "%PV_DIR%" && build -cZg && call sign.bat && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ fre WLH && CD /D "%PV_DIR%" && build -cZg && call sign.bat && call wix.bat"

cmd /C "%DDK_PATH%\bin\setenv.bat %DDK_PATH%\ fre x64 WLH && CD /D "%PV_DIR%" && build -cZg && call sign.bat && call wix.bat"

hg status | findstr "^M" && GOTO DONT_TAG
IF NOT EXIST TAG_HG GOTO DONT_TAG
IF EXIST BUILD*.WRN GOTO DONT_TAG
IF EXIST BUILD*.ERR GOTO DONT_TAG

hg tag %EJBPV_VERSION%

:DONT_TAG
