/**************************************************************
*
* Lattice Semiconductor Corp. Copyright 2008
*
* ispVME Embedded allows programming of Lattice's suite of FPGA
* devices on embedded systems through the JTAG port.  The software
* is distributed in source code form and is open to re - distribution
* and modification where applicable.
*
* ispVME Embedded C Source comprised with 3 modules:
* ispvm_ui.c is the module provides input and output support.
* ivm_core.c is the module interpret the VME file(s).
* hardware.c is the module access the JTAG port of the device(s).
*
* The optional module cable.c is for supporting Lattice's parallel
* port ispDOWNLOAD cable on DOS and Windows 95/98 O/S. It can be
* requested from Lattice's ispVMSupport.
*
***************************************************************/

/**************************************************************
*
* Revision History of ispvm_ui.c
*
* 3/6/07 ht Added functions vme_out_char(),vme_out_hex(),
*           vme_out_string() to provide output resources.
*           Consolidate all printf() calls into the added output
*           functions.
*
* 09/11/07 NN Added Global variables initialization
* 09/24/07 NN Added a switch allowing users to do calibration.
* Calibration will help to determine the system clock frequency
* and the count value for one micro-second delay of the target
* specific hardware.
* Removed Delay Percent support
* 11/15/07  NN moved the checking of the File CRC to the end of processing
* 08/28/08 NN Added Calculate checksum support.
***************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <debug_ispvme.h>
#include "vmopcode.h"
#include "common.h"

#define dbg_print(debug, fmt, arg...)  \
    if (debug == DEBUG_APP_ON || debug == DEBUG_ALL_ON) \
        { do{printf(fmt,##arg);} while(0); }

/***************************************************************
*
* File pointer to the VME file.
*
***************************************************************/

FILE *g_pVMEFile = NULL;

/***************************************************************
*
* Functions declared in this ispvm_ui.c module
*
***************************************************************/
unsigned char GetByte(void);
void vme_out_char(unsigned char charOut);
void vme_out_hex(unsigned char hexOut);
void vme_out_string(char *stringOut);
void ispVMMemManager(signed char cTarget, unsigned short usSize);
void ispVMFreeMem(void);
void error_handler(short a_siRetCode, char *pszMessage);
signed char ispVM(const char *a_pszFilename);

/***************************************************************
*
* Global variables.
*
***************************************************************/
unsigned short g_usPreviousSize = 0;
unsigned short g_usExpectedCRC = 0;

/***************************************************************
*
* External variables and functions declared in ivm_core.c module.
*
***************************************************************/
extern signed char ispVMCode();
extern void ispVMCalculateCRC32(unsigned char a_ucData);
extern void ispVMStart();
extern void ispVMEnd();
extern void ispVMStateMachine(signed char NextState);
extern void writePort(unsigned char a_ucPins, unsigned char a_ucValue);
extern void sclock();
extern unsigned char readPort();
extern unsigned short g_usCalculatedCRC;
extern unsigned short g_usDataType;
extern unsigned char *g_pucOutMaskData,
*g_pucInData,
*g_pucOutData,
*g_pucHIRData,
*g_pucTIRData,
*g_pucHDRData,
*g_pucTDRData,
*g_pucOutDMaskData,
*g_pucIntelBuffer;
extern unsigned char *g_pucHeapMemory;
extern unsigned short g_iHeapCounter;
extern unsigned short g_iHEAPSize;
extern unsigned short g_usIntelDataIndex;
extern unsigned short g_usIntelBufferSize;
extern LVDSPair *g_pLVDSList;
/* 08/28/08 NN Added Calculate checksum support. */
extern unsigned long g_usChecksum;
extern unsigned int g_uiChecksumIndex;
/***************************************************************
*
* External variables and functions declared in hardware.c module.
*
***************************************************************/
extern void calibration(void);
extern unsigned short g_usCpu_Frequency;

static int is_debug_on = -1;

/***************************************************************
*
* Supported VME versions.
*
***************************************************************/

const char *const g_szSupportedVersions[] = { "__VME2.0", "__VME3.0", "____12.0", "____12.1", 0 };

/***************************************************************
*
* GetByte
*
* Returns a byte to the caller. The returned byte depends on the
* g_usDataType register. If the HEAP_IN bit is set, then the byte
* is returned from the HEAP. If the LHEAP_IN bit is set, then
* the byte is returned from the intelligent buffer. Otherwise,
* the byte is returned directly from the VME file.
*
***************************************************************/

char* strlwr(char *str)
{
    char *orig = str;
    /* process the string */
    for (; *str != '\0'; str++)
        *str = tolower(*str);
    return orig;
}

unsigned char GetByte()
{
    unsigned char ucData = 0;

    if (g_usDataType & HEAP_IN) {

        /***************************************************************
        *
        * Get data from repeat buffer.
        *
        ***************************************************************/

        if (g_iHeapCounter > g_iHEAPSize) {

            /***************************************************************
            *
            * Data over-run.
            *
            ***************************************************************/

            return 0xFF;
        }

        ucData = g_pucHeapMemory[g_iHeapCounter++];
    }
    else if ( g_usDataType & LHEAP_IN ) {
    
        /***************************************************************
        *
        * Get data from intel buffer.
        *
        ***************************************************************/

        if (g_usIntelDataIndex >= g_usIntelBufferSize) {

            /***************************************************************
            *
            * Data over-run.
            *
            ***************************************************************/

            return 0xFF;
        }

        ucData = g_pucIntelBuffer[g_usIntelDataIndex++];
    }
    else {

        /***************************************************************
        *
        * Get data from file.
        *
        ***************************************************************/

        ucData = (unsigned char)fgetc(g_pVMEFile);
        

        if (feof(g_pVMEFile)) {

            /***************************************************************
            *
            * Reached EOF.
            *
            ***************************************************************/

            return 0xFF;
        }
        /***************************************************************
        *
        * Calculate the 32-bit CRC if the expected CRC exist.
        *
        ***************************************************************/
        if( g_usExpectedCRC != 0)
        {
            ispVMCalculateCRC32(ucData);
        }
    }

    return (ucData);
}

/***************************************************************
*
* vme_out_char
*
* Send a character out to the output resource if available.
* The monitor is the default output resource.
*
*
***************************************************************/
void vme_out_char(unsigned char charOut)
{
    dbg_print(is_debug_on, "%c", charOut);
}
/***************************************************************
*
* vme_out_hex
*
* Send a character out as in hex format to the output resource
* if available. The monitor is the default output resource.
*
*
***************************************************************/
void vme_out_hex(unsigned char hexOut)
{
    dbg_print(is_debug_on, "%.2X", hexOut);
}
/***************************************************************
*
* vme_out_string
*
* Send a text string out to the output resource if available.
* The monitor is the default output resource.
*
*
***************************************************************/
void vme_out_string(char *stringOut)
{
    if (is_debug_on == -1) {
        is_debug_on = firmware_upgrade_debug();
    }

    dbg_print(is_debug_on, stringOut);
}
/***************************************************************
*
* ispVMMemManager
*
* Allocate memory based on cTarget. The memory size is specified
* by usSize.
*
***************************************************************/

void ispVMMemManager(signed char cTarget, unsigned short usSize)
{
    switch (cTarget) {
    case XTDI:
    case TDI:
        if (g_pucInData != NULL) {
            if (g_usPreviousSize == usSize) { /*memory exist*/
                break;
            }
            else {
                free(g_pucInData);
                g_pucInData = NULL;
            }
        }
        g_pucInData = (unsigned char *)malloc(usSize / 8 + 2);
        g_usPreviousSize = usSize;
    case XTDO:
    case TDO:
        if (g_pucOutData != NULL) {
            if (g_usPreviousSize == usSize) { /*already exist*/
                break;
            }
            else {
                free(g_pucOutData);
                g_pucOutData = NULL;
            }
        }
        g_pucOutData = (unsigned char *)malloc(usSize / 8 + 2);
        g_usPreviousSize = usSize;
        break;
    case MASK:
        if (g_pucOutMaskData != NULL) {
            if (g_usPreviousSize == usSize) { /*already allocated*/
                break;
            }
            else {
                free(g_pucOutMaskData);
                g_pucOutMaskData = NULL;
            }
        }
        g_pucOutMaskData = (unsigned char *)malloc(usSize / 8 + 2);
        g_usPreviousSize = usSize;
        break;
    case HIR:
        if (g_pucHIRData != NULL) {
            free(g_pucHIRData);
            g_pucHIRData = NULL;
        }
        g_pucHIRData = (unsigned char *)malloc(usSize / 8 + 2);
        break;
    case TIR:
        if (g_pucTIRData != NULL) {
            free(g_pucTIRData);
            g_pucTIRData = NULL;
        }
        g_pucTIRData = (unsigned char *)malloc(usSize / 8 + 2);
        break;
    case HDR:
        if (g_pucHDRData != NULL) {
            free(g_pucHDRData);
            g_pucHDRData = NULL;
        }
        g_pucHDRData = (unsigned char *)malloc(usSize / 8 + 2);
        break;
    case TDR:
        if (g_pucTDRData != NULL) {
            free(g_pucTDRData);
            g_pucTDRData = NULL;
        }
        g_pucTDRData = (unsigned char *)malloc(usSize / 8 + 2);
        break;
    case HEAP:
        if (g_pucHeapMemory != NULL) {
            free(g_pucHeapMemory);
            g_pucHeapMemory = NULL;
        }
        g_pucHeapMemory = (unsigned char *)malloc(usSize + 2);
        break;
    case DMASK:
        if (g_pucOutDMaskData != NULL) {
            if (g_usPreviousSize == usSize) { /*already allocated*/
                break;
            }
            else {
                free(g_pucOutDMaskData);
                g_pucOutDMaskData = NULL;
            }
        }
        g_pucOutDMaskData = (unsigned char *)malloc(usSize / 8 + 2);
        g_usPreviousSize = usSize;
        break;
    case LHEAP:
        if (g_pucIntelBuffer != NULL) {
            free(g_pucIntelBuffer);
            g_pucIntelBuffer = NULL;
        }
        g_pucIntelBuffer = (unsigned char *)malloc(usSize + 2);
        break;
    case LVDS:
        if (g_pLVDSList != NULL) {
            free(g_pLVDSList);
            g_pLVDSList = NULL;
        }
        g_pLVDSList = (LVDSPair * )calloc(usSize, sizeof(LVDSPair));
        break;
    default:
        return;
    }
}

/***************************************************************
*
* ispVMFreeMem
*
* Free memory that were dynamically allocated.
*
***************************************************************/

void ispVMFreeMem()
{
    if (g_pucHeapMemory != NULL) {
        free(g_pucHeapMemory);
        g_pucHeapMemory = NULL;
    }

    if (g_pucOutMaskData != NULL) {
        free(g_pucOutMaskData);
        g_pucOutMaskData = NULL;
    }

    if (g_pucInData != NULL) {
        free(g_pucInData);
        g_pucInData = NULL;
    }

    if (g_pucOutData != NULL) {
        free(g_pucOutData);
        g_pucOutData = NULL;
    }

    if (g_pucHIRData != NULL) {
        free(g_pucHIRData);
        g_pucHIRData = NULL;
    }

    if (g_pucTIRData != NULL) {
        free(g_pucTIRData);
        g_pucTIRData = NULL;
    }

    if (g_pucHDRData != NULL) {
        free(g_pucHDRData);
        g_pucHDRData = NULL;
    }

    if (g_pucTDRData != NULL) {
        free(g_pucTDRData);
        g_pucTDRData = NULL;
    }

    if (g_pucOutDMaskData != NULL) {
        free(g_pucOutDMaskData);
        g_pucOutDMaskData = NULL;
    }

    if (g_pucIntelBuffer != NULL) {
        free(g_pucIntelBuffer);
        g_pucIntelBuffer = NULL;
    }

    if (g_pLVDSList != NULL) {
        free(g_pLVDSList);
        g_pLVDSList = NULL;
    }
}

/***************************************************************
*
* error_handler
*
* Reports the error message.
*
***************************************************************/

void error_handler(short a_siRetCode, char *pszMessage)
{
    const char *pszErrorMessage[] = { "pass",
        "verification fail",
        "can't find the file",
        "wrong file type",
        "file error",
        "option error",
        "crc verification error" };

    strcpy(pszMessage, pszErrorMessage[-a_siRetCode]);
}
/***************************************************************
*
* ispVM
*
* The entry point of the ispVM embedded. If the version and CRC
* are verified, then the VME will be processed.
*
***************************************************************/

signed char ispVM(const char *a_pszFilename)
{
    char szFileVersion[9] = { 0 };
    signed char cRetCode         = 0;
    signed char cIndex           = 0;
    signed char cVersionIndex    = 0;
    unsigned char ucReadByte     = 0;

    /***************************************************************
    *
    * Global variables initialization.
    *
    * 09/11/07 NN Added
    ***************************************************************/
    g_pucHeapMemory     = NULL;
    g_iHeapCounter      = 0;
    g_iHEAPSize         = 0;
    g_usIntelDataIndex  = 0;
    g_usIntelBufferSize = 0;
    g_usPreviousSize     = 0;

    /***************************************************************
    *
    * Open a file pointer to the VME file.
    *
    ***************************************************************/

        
    
    if ((g_pVMEFile = fopen(a_pszFilename, "rb")) == NULL) {
        return VME_FILE_READ_FAILURE;
    }
    g_usCalculatedCRC = 0;
    g_usExpectedCRC   = 0;
    ucReadByte = GetByte();
    //dbg_print(is_debug_on, "data: 0x%x.\n", ucReadByte);
    switch (ucReadByte) {
    case FILE_CRC:

        /***************************************************************
        *
        * Read and store the expected CRC to do the comparison at the end.
        * Only versions 3.0 and higher support CRC protection.
        *
        ***************************************************************/

        g_usExpectedCRC = (unsigned char)fgetc(g_pVMEFile);
        g_usExpectedCRC <<= 8;
        g_usExpectedCRC |= fgetc(g_pVMEFile);

        /***************************************************************
        *
        * Read and store the version of the VME file.
        *
        ***************************************************************/
            

        for (cIndex = 0; cIndex < 8; cIndex++) {
            szFileVersion[cIndex] = GetByte();
        }
        break;
    default:

        /***************************************************************
        *
        * Read and store the version of the VME file.  Must be version 2.0.
        *
        ***************************************************************/

        szFileVersion[0] = (signed char)ucReadByte;
        for (cIndex = 1; cIndex < 8; cIndex++) {
            szFileVersion[cIndex] = GetByte();
        }

        break;
    }

    /***************************************************************
    *
    * Compare the VME file version against the supported version.
    *
    ***************************************************************/
    /* check the file version: the value of 0x03-0x0a */
    for (cVersionIndex = 0; g_szSupportedVersions[cVersionIndex] != 0; cVersionIndex++) {
        for (cIndex = 0; cIndex < 8; cIndex++) {
            if (szFileVersion[cIndex] != g_szSupportedVersions[cVersionIndex][cIndex]) {
                cRetCode = VME_VERSION_FAILURE;
                break;
            }
            cRetCode = 0;
        }

        if (cRetCode == 0) {

            /***************************************************************
            *
            * Found matching version, break.
            *
            ***************************************************************/

            break;
        }
    }

    if (cRetCode < 0) {

        /***************************************************************
        *
        * VME file version failed to match the supported versions.
        *
        ***************************************************************/

        fclose(g_pVMEFile);
        g_pVMEFile = NULL;
        return VME_VERSION_FAILURE;
    }

    /***************************************************************
    *
    * Enable the JTAG port to communicate with the device.
    * Set the JTAG state machine to the Test-Logic/Reset State.
    *
    ***************************************************************/
    ispVMStart();
    

    /***************************************************************
    *
    * Process the VME file.
    *
    ***************************************************************/
    
    cRetCode = ispVMCode();
    
    /***************************************************************
    *
    * Set the JTAG State Machine to Test-Logic/Reset state then disable
    * the communication with the JTAG port.
    *
    ***************************************************************/
        
    ispVMEnd();

    fclose(g_pVMEFile);
    g_pVMEFile = NULL;

    ispVMFreeMem();

    /***************************************************************
    *
    * Compare the expected CRC versus the calculated CRC.
    *
    ***************************************************************/
    
    if (cRetCode == 0 && g_usExpectedCRC != 0 && (g_usExpectedCRC != g_usCalculatedCRC)) {
        printf("Expected CRC:   0x%.4X\n", g_usExpectedCRC);
        printf("Calculated CRC: 0x%.4X\n", g_usCalculatedCRC);
        return VME_CRC_FAILURE;
    }

    return (cRetCode);
}

/***************************************************************
*
* main
*
***************************************************************/
extern int g_file_fd;
int ispvme_main(int argc, char *argv[])
{
    unsigned short iCommandLineIndex  = 0;
    short siRetCode                   = 0;
    char szExtension[5] = { 0 };
    char szCommandLineArg[300] = { 0 };
    short sicalibrate                 = 0;

    /* 08/28/08 NN Added Calculate checksum support. */
    g_usChecksum = 0;
    g_uiChecksumIndex = 0;

    g_file_fd = open("/dev/firmware_cpld_ispvme0", O_RDWR);
    if (g_file_fd < 0) {
        printf("can't open device\r\n");
        return -1;
    }

    vme_out_string("                 Lattice Semiconductor Corp.\n");
    vme_out_string("\n             ispVME(tm) V");
    vme_out_string(VME_VERSION_NUMBER);
    vme_out_string(" Copyright 1998-2011.\n");
    vme_out_string("\nFor daisy chain programming of all in-system programmable devices\n\n");

    if (argc < 2) {
        vme_out_string("\nUsage: vme [option] vme_file [vme_file]\n");
        vme_out_string("Example: vme vme_file1.vme vme_file2.vme\n");
        vme_out_string("option   -c:   do the calibration.\n");
        vme_out_string("Example: vme -c\n");
        vme_out_string("Example: vme -c vme_file1.vme vme_file2.vme\n");
        vme_out_string("\n\n");
        close(g_file_fd);
        return -1;          /* Used by superiors to judge whether the upgrade is successful */
        //exit(1);
    }
    for (iCommandLineIndex = 1; iCommandLineIndex < argc; iCommandLineIndex++) {
        strcpy(szCommandLineArg, argv[iCommandLineIndex]);
        if (!strcmp(strlwr(szCommandLineArg), "-c") && (iCommandLineIndex == 1)) {
            sicalibrate = 1;
        } else if (!strcmp(strlwr(szCommandLineArg), "-c") && (iCommandLineIndex != 1)) {
            vme_out_string("Error: calibrate option -c must be the first argument\n\n");
            close(g_file_fd);
            return -1;      /* Used by superiors to judge whether the upgrade is successful */
            //exit(1);
        } else {
            strcpy(szExtension, &szCommandLineArg[strlen(szCommandLineArg) - 4]);
            strlwr(szExtension);
            if (strcmp(szExtension, ".vme")) {
                vme_out_string("Error: VME files must end with the extension *.vme\n\n");
                close(g_file_fd);
                return -1;      /* Used by superiors to judge whether the upgrade is successful */
                //exit(1);
            }
        }
    }
    siRetCode = 0;
    
    if (sicalibrate) {
        calibration();
    }
    for (iCommandLineIndex = 1; iCommandLineIndex < argc; iCommandLineIndex++) {   /* Process all VME files sequentially */
        strcpy(szCommandLineArg, argv[iCommandLineIndex]);
        if (!strcmp(strlwr(szCommandLineArg), "-c") && (iCommandLineIndex == 1)) {
            
        } else if (!strcmp(strlwr(szCommandLineArg), "-checksum")) {
          
        } else {
            vme_out_string("Processing virtual machine file (");
            vme_out_string(szCommandLineArg);
            vme_out_string(")......\n\n");
            siRetCode = ispVM(szCommandLineArg);
            if (siRetCode < 0) {
                break;
            }
        }
    }
    
    if (siRetCode < 0) {
        error_handler(siRetCode, szCommandLineArg);
        vme_out_string("Failed due to ");
        vme_out_string(szCommandLineArg);
        vme_out_string("\n\n");
        vme_out_string("+=======+\n");
        vme_out_string("| FAIL! |\n");
        vme_out_string("+=======+\n\n");
    } else {
        vme_out_string("+=======+\n");
        vme_out_string("| PASS! |\n");
        vme_out_string("+=======+\n\n");
        /* 08/28/08 NN Added Calculate checksum support. */
        if (g_usChecksum != 0) {
            g_usChecksum &= 0xFFFF;
            printf("Data Checksum: %.4X\n\n", (unsigned int)g_usChecksum);
            g_usChecksum = 0;
        }
    }
    close(g_file_fd);
    return siRetCode;       /* Used by superiors to judge whether the upgrade is successful */
    //exit(siRetCode);
}

/**
* function:CPLD online upgrade channel test program
* input parameters: None
* output parameters:None
* return: 0 means test successfully -1 means test failed
*/
int ispvme_test(void)
{
    int ret = 0;
    unsigned char index = 0;
    unsigned char indata = 0,outdata = 0;
    unsigned char datasize = 8;
    unsigned int tmpdata = 0;
    char curbit = 0;
    int nDevices = 0;

    g_file_fd = open("/dev/firmware_cpld_ispvme0", O_RDWR);
    if (g_file_fd < 0) {
        printf("can't open device\r\n");
        return -1;
    }

    /* go to Shift-IR */
    ispVMStart();
    ispVMStateMachine(IDLE);
    ispVMStateMachine(IRPAUSE);
    ispVMStateMachine(SHIFTIR);

    /* Send plenty of ones into the IR registers,that makes sure all devices are in BYPASS! */
    for (index = 0; index < 100; index++) {
        writePort(JTAG_TDI, 1);
        sclock();
    }

    /* exit Shift-IR,and go to Shift-DR */
    ispVMStateMachine(IRPAUSE);
    ispVMStateMachine(DRPAUSE);
    ispVMStateMachine(SHIFTDR);

    /* Send plenty of zeros into the DR registers to flush them */
    for (index = 0; index < 100; index++) {
        writePort(JTAG_TDI, 0);
        sclock();
    }

    /* now send ones until we receive one back,to find out number of device(s) */
    for(index = 0; index < 100; index++){
        writePort(JTAG_TDI, 1);
        sclock();
        curbit = readPort();
        if(1 == curbit){
            nDevices = index + 1;
            break;
        }
    }

    /*flush again */
    for (index = 0; index < 100; index++) {
        writePort(JTAG_TDI, 0);
        sclock();
    }

    /* test data probe */
    indata = 0x5a;
    for (index = 0; index < datasize + nDevices; index++) {
        tmpdata <<= 1;
        curbit = readPort();
        tmpdata |= curbit;
        writePort(JTAG_TDI, ((indata << index) & 0x80) ? 0x01:0x00);
        sclock();
    }
    outdata = (tmpdata & 0xFF);
    if(outdata != indata){
        ret = -1;
    }
    
    /* go to Test-Logic-Reset */
    ispVMStateMachine(IDLE);
    ispVMStart();
    
    /* close ispvme */
    close(g_file_fd);
    return ret;
}