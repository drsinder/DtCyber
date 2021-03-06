/*--------------------------------------------------------------------------
**
**  Copyright (c) 2003-2010, Tom Hunter, Paul Koning
**	          (c) 2017       Steven Zoppi 22-Oct-2017
**                           Added Ascii and ANSI support
**                           Added subdirectory support
**
**  Name: lp3000.c
**
**  Description:
**      Perform emulation of CDC 3000 series printers/controllers.
**
**      This combines the 501 and 512 printers, and the 3152 and 3555
**      controllers, because they all look pretty similar.
**
**      501 vs. 512 is selected by which Init function is called from
**      init.c via the device table; 3555 is the default but 3152/3256/3659
**      emulation can be selected by supplying a name string of "3152".
**
**  This program is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License version 3 as
**  published by the Free Software Foundation.
**
**  This program is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License version 3 for more details.
**
**  You should have received a copy of the GNU General Public License
**  version 3 along with this program in file "license-gpl-3.0.txt".
**  If not, see <http://www.gnu.org/licenses/gpl-3.0.txt>.
**
**--------------------------------------------------------------------------
*/

#define DEBUG 0

/*
**  -------------
**  Include Files
**  -------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <process.h>
#endif

#include <errno.h>
#include "const.h"
#include "types.h"
#include "proto.h"
#include "dcc6681.h"

/*
**  -----------------
**  Private Constants
**  -----------------
*/

// General

// Flags stored in the context field:
#define Lp3000Type501           00001
#define Lp3000Type512           00002
#define Lp3000Type3152          00010
#define Lp3000Type3555          00020
#define Lp3555FillImageMem      00100
#define Lp3000IntReady          00200   // Same code as int status bit
#define Lp3000IntEnd            00400   // ditto
#define Lp3000IntReadyEna       02000
#define Lp3000IntEndEna         04000

/*
**      Function codes
*/

// Codes common to 3152/3256/3659 and 3555
#define FcPrintRelease          00000
#define FcPrintSingle           00001
#define FcPrintDouble           00002
#define FcPrintLastLine         00003
#define FcPrintEject            00004
#define FcPrintAutoEject        00005
#define FcPrintNoSpace          00006

// Codes for 3152/3256/3659
#define Fc3152ClearFormat       00010
#define Fc3152PostVFU1          00011
#define Fc3152PostVFU2          00012
#define Fc3152PostVFU3          00013
#define Fc3152PostVFU4          00014
#define Fc3152PostVFU5          00015
#define Fc3152PostVFU6          00016
#define Fc3152SelectPreprint    00020
#define Fc3152PreVFU1           00021
#define Fc3152PreVFU2           00022
#define Fc3152PreVFU3           00023
#define Fc3152PreVFU4           00024
#define Fc3152PreVFU5           00025
#define Fc3152PreVFU6           00026
#define Fc3152SelIntReady       00030
#define Fc3152RelIntReady       00031
#define Fc3152SelIntEnd         00032
#define Fc3152RelIntEnd         00033
#define Fc3152SelIntError       00034
#define Fc3152RelIntError       00035
#define Fc3152Release2          00040

// Codes for 3555
#define Fc3555CondClearFormat   00007
#define Fc3555Sel8Lpi           00010
#define Fc3555Sel6Lpi           00011
#define Fc3555FillMemory        00012
#define Fc3555SelExtArray       00013
#define Fc3555ClearExtArray     00014
#define Fc3555SelIntReady       00020
#define Fc3555RelIntReady       00021
#define Fc3555SelIntEnd         00022
#define Fc3555RelIntEnd         00023
#define Fc3555SelIntError       00024
#define Fc3555RelIntError       00025
#define Fc3555ReloadMemEnable   00026
#define Fc3555ClearFormat       00030
#define Fc3555PostVFU1          00031
#define Fc3555PostVFU2          00032
#define Fc3555PostVFU3          00033
#define Fc3555PostVFU4          00034
#define Fc3555PostVFU5          00035
#define Fc3555PostVFU6          00036
#define Fc3555PostVFU7          00037
#define Fc3555PostVFU8          00040
#define Fc3555PostVFU9          00041
#define Fc3555PostVFU10         00042
#define Fc3555PostVFU11         00043
#define Fc3555PostVFU12         00044
#define Fc3555SelectPreprint    00050
#define Fc3555PreVFU1           00051
#define Fc3555PreVFU2           00052
#define Fc3555PreVFU3           00053
#define Fc3555PreVFU4           00054
#define Fc3555PreVFU5           00055
#define Fc3555PreVFU6           00056
#define Fc3555PreVFU7           00057
#define Fc3555PreVFU8           00060
#define Fc3555PreVFU9           00061
#define Fc3555PreVFU10          00062
#define Fc3555PreVFU11          00063
#define Fc3555PreVFU12          00064
#define Fc3555MaintStatus       00065
#define Fc3555ClearMaint        00066

//	Lines Per Page
#define LpInchesPerPage			00011		//	Standard 11 x 14 forms

/*
**      Status reply
**
**  3152/3256/3659 vs. 3555 have different status codes for the
**  most part, but the few we care about are common:
**
*/
#define StPrintReady            00001
#define StPrintIntReady         00200
#define StPrintIntEnd           00400

/*
**  -----------------------
**  Private Macro Functions
**  -----------------------
*/

/*
**  -----------------------------------------
**  Private Typedef and Structure Definitions
**  -----------------------------------------
*/

typedef struct lpContext
    {
    
    int flags;
    bool printed;
    bool keepInt;

    /*
    **	(SZoppi) 22-Oct-2017
    **		These extensions are to properly simulate the behavior of the
    **		line printers.  Line space handling (pre and post) messes with
    **		translation to proper PDF format (the new purpose for this).
    */
    u8 extspaceopt;			//	Spacing option (<>0 = PostPrintMode)
    u8 extlpi;				//	Lines Per Inch 6 or 8 (usually)
    u8 extlpp;				//	Number of Lines per Printed Page
                            //		at 8 LPI * 11" = 88 LPP
                            //		at 6 LPI * 11" = 66 LPP
    u8 extcurline;			//	current page position
    
                            //  Todo: Verify that the use of this doesn't
                            //        overflow over a long operating window
    bool extuseANSI;        //  use ANSI/ASA Carriage-control characters
    bool extsuppress;		//	suppress next post-print spacing op
    bool extpostprint;		//	all spacing occurs in post-print mode
    char extPath[_MAX_PATH + 1];    //  preserve the device folder path
    
    } LpContext;


/*
**  ---------------------------
**  Private Function Prototypes
**  ---------------------------
*/
static void lp3000Init(u8 unitNo, u8 eqNo, u8 channelNo, int flags, bool useANSI, char *devicePath);
static FcStatus lp3000Func(PpWord funcCode);
static void lp3000Io(void);
static void lp3000Activate(void);
static void lp3000Disconnect(void);
static void lp3000DebugData(void);
static char *lp3000Func2String(PpWord funcCode);

/*
**  ----------------
**  Public Variables
**  ----------------
*/

/*
**  -----------------
**  Private Variables
**  -----------------
*/
#if DEBUG
static FILE *lp3000Log = NULL;
#define MaxLine 1000
static PpWord lineData[MaxLine];
static int linePos = 0;
#endif

/*
**--------------------------------------------------------------------------
**
**  Public Functions
**
**--------------------------------------------------------------------------
*/

/*--------------------------------------------------------------------------
**  Purpose:        Initialise 501 line printer.
**
**  Parameters:     Name        Description.
**                  eqNo        equipment number
**                  unitNo      unit number
**                  channelNo   channel number the device is attached to
**                  deviceParams    Comma Delimited Parameters
**                      <devicePath> Path to Output Files
**                      <deviceType> "3152" to get 3152 controller
**                                   null or "3555" for 3555
**                      <deviceMode> "ansi" or "ascii" for output type
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
void lp501Init(u8 eqNo, u8 unitNo, u8 channelNo, char *deviceParams)
    {
    int flags = Lp3000Type501;
    bool useANSI;
    char *deviceType;
    char *devicePath;
    char *deviceMode;

#if DEBUG
    if (lp3000Log == NULL)
        {
        lp3000Log = fopen("lp3000log.txt", "wt");
        }
#endif

    (void)eqNo;
    (void)deviceParams;

    /*
    **  When we are called, "deviceParams" is a space terminated string
    **  at the end of the INI entry.
    **
    **  Tokenizing the remainder of the string as comma-delimited
    **  parameters gives us the configuration data that we need.
    **
    **  The format of the remainder of the line is:
    **
    **      <DeviceType> (NULL(="3555")|"3555"|"3512")
    **      <OutputMode> ("ASCII"|"ANSI")
    **
    */
    devicePath = strtok(deviceParams, ",");         //  Get the Path (subdirectory)
    deviceType = strtok(NULL, ",");                 //  "3555" | "3152"
    deviceMode = strlwr(strtok(NULL, ","));         //  pick up "ansi" or "ascii" flag

    if ((deviceMode) != NULL)
    {
        useANSI = FALSE;
        if (strcmp(deviceMode, "ansi") == 0)
        {
            useANSI = TRUE;
        }
        else if (strcmp(deviceMode, "ascii") == 0)
        {
            useANSI = FALSE;
        }
        else
        {
            useANSI = FALSE;
        }
    }

    if (deviceType == NULL ||
        strcmp(deviceType, "3555") == 0)
        {
        flags |= Lp3000Type3555;
        }
    else if (strcmp(deviceType, "3152") == 0)
        {
        flags |= Lp3000Type3152;
        }
    else
        {
        fprintf(stderr, "(lp3000 ) Unrecognized LP501 controller type %s\n", deviceType);
        exit(1);
        }

    lp3000Init(unitNo, eqNo, channelNo, flags, useANSI, devicePath);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Initialise 512 line printer.
**
**  Parameters:     Name        Description.
**                  eqNo        equipment number
**                  unitNo      unit number
**                  channelNo   channel number the device is attached to
**                  deviceParams    Comma Delimited Parameters
**                      <devicePath> Path to Output Files
**                      <deviceType> "3152" to get 3152 controller
**                                   null or "3555" for 3555
**                      <deviceMode> "ansi" or "ascii" for output type
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
void lp512Init(u8 eqNo, u8 unitNo, u8 channelNo, char *deviceParams)
    {
    int flags = Lp3000Type512;
    bool useANSI;
    char *deviceType;
    char *devicePath;
    char *deviceMode;

#if DEBUG
    if (lp3000Log == NULL)
        {
        lp3000Log = fopen("lp3000log.txt", "wt");
        }
#endif

    (void)eqNo;
    (void)deviceParams;

    /*
    **  When we are called, "deviceParams" is a space terminated string
    **  at the end of the INI entry.
    **
    **  Tokenizing the remainder of the string as comma-delimited
    **  parameters gives us the configuration data that we need.
    **
    **  The format of the remainder of the line is:
    **
    **      <DeviceType> (NULL(="3555")|"3555"|"3512")
    **      <OutputMode> ("ASCII"|"ANSI")
    **
    */
    devicePath = strtok(deviceParams, ",");     //  Get the Path (subdirectory)
    deviceType = strtok(NULL, ",");             //  "3555" | "3152"
    deviceMode = strlwr(strtok(NULL, ","));     //  pick up "ansi" or "ascii" flag

    if ((deviceMode) != NULL)
    {
        useANSI = FALSE;
        if (strcmp(deviceMode, "ansi") == 0)
        {
            useANSI = TRUE;
        }
        else if (strcmp(deviceMode, "ascii") == 0)
        {
            useANSI = FALSE;
        }
        else
        {
            useANSI = FALSE;
        }
    }

    if (deviceType == NULL ||
        strcmp(deviceType, "3555") == 0)
        {
        flags |= Lp3000Type3555;
        }
    else if (strcmp(deviceType, "3152") == 0)
        {
        flags |= Lp3000Type3152;
        }
    else
        {
        fprintf(stderr, "(lp3000 ) Unrecognized LP512 controller type %s\n", deviceType);
        exit(1);
        }

    lp3000Init(unitNo, eqNo, channelNo, flags, useANSI, devicePath);
    }

/*
**--------------------------------------------------------------------------
**
**  Private Functions
**
**--------------------------------------------------------------------------
*/
/*--------------------------------------------------------------------------
**  Purpose:        Initialise 3000 class line printer.
**
**  Parameters:     Name        Description.
**                  unitNo      unit number
**                  eqNo        equipment number
**                  channelNo   channel number the device is attached to
**                  flags       Printer type flags
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
static void lp3000Init(u8 unitNo, u8 eqNo, u8 channelNo, int flags, bool useANSI, char *devicePath)
    {
    DevSlot *up;
    char fname[_MAX_PATH];
    LpContext *lc;

    up = dcc6681Attach(channelNo, eqNo, unitNo, DtLp5xx);

    up->activate = lp3000Activate;
    up->disconnect = lp3000Disconnect;
    up->func = lp3000Func;
    up->io = lp3000Io;

    /*
    **  Only one printer unit is possible per equipment.
    */
    if (up->context[0] != NULL)
        {
        fprintf(stderr, "(lp3000 ) Only one LP5xx unit is possible per equipment\n");
        exit(1);
        }

    lc = (LpContext *)calloc(1, sizeof(LpContext));
    if (lc == NULL)
        {
        fprintf(stderr, "(lp3000 ) Failed to allocate LP5xx context block\n");
        exit(1);
        }

    up->context[0] = (void *)lc;
    lc->flags = flags;

    /*
    **	SZoppi: Extensions
    */
    lc->extspaceopt = FcPrintSingle;    //  Default Single Space
    lc->extlpi = 6;						//  Default Page Length
    lc->extlpp = LpInchesPerPage * lc->extlpi;
    lc->extcurline = 1;					//	1 = Initialized
    lc->extsuppress = FALSE;            //  Clear Suppress Print
    lc->extpostprint = TRUE;			//  Post-Print Mode by Default
    lc->extuseANSI = useANSI;           //  Indicate how to handle Carriage Control

    /*
    **  Remember the device Path for future fopen calls
    */
    if (devicePath == NULL)
    {
        lc->extPath[0] = '\0';
    }
    else
    {
        strcpy_s(lc->extPath, sizeof(lc->extPath), devicePath);
        if (lc->extPath[0] != '\0')
        {
            strcat_s(lc->extPath, sizeof(lc->extPath), "/");
        }
    }

    /*
    **  Open the device file.
    */
    sprintf_s(fname, sizeof(fname), "%sLP5xx_C%02o_E%o", lc->extPath, channelNo, eqNo);
    up->fcb[0] = fopen(fname, "w");
    if (up->fcb[0] == NULL)
        {
        fprintf(stderr, "(lp3000 ) Failed to open %s\n", fname);
        exit(1);
        }

    /*
    **  Print a friendly message.
    */
    printf("(lp3000 ) LP%d/%d initialised on channel %o equipment %o filename '%s'\n",
        (flags & Lp3000Type3555) ? 3555 : 3152,
           (flags & Lp3000Type501) ? 501 : 512,
           channelNo, eqNo, fname);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Remove the paper (operator interface).
**
**  Parameters:     Name        Description.
**                  params      parameters
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
void lp3000RemovePaper(char *params)
    {
    DevSlot *dp;
    int numParam;
    int channelNo;
    int equipmentNo;
    int isuffix;
    time_t currentTime;
    struct tm t;
    char fname[_MAX_PATH];
    char fnameNew[_MAX_PATH];
    LpContext *lc;


    /*
    **  Operator wants to remove paper.
    */
    numParam = sscanf(params, "%o,%o", &channelNo, &equipmentNo);

    /*
    **  Check parameters.
    */
    if (numParam != 2)
        {
        printf("(lp3000 ) Not enough or invalid parameters\n");
        return;
        }

    if (channelNo < 0 || channelNo >= MaxChannels)
        {
        printf("(lp3000 ) Invalid channel no\n");
        return;
        }

    if (equipmentNo < 0 || equipmentNo >= MaxEquipment)
        {
        printf("(lp3000 ) Invalid equipment no\n");
        return;
        }

    /*
    **  Locate the device control block.
    */
    dp = dcc6681FindDevice((u8)channelNo, (u8)equipmentNo, DtLp5xx);
    if (dp == NULL)
        {
        return;
        }

    lc = (LpContext *)dp->context[0];
    sprintf_s(fname, sizeof(fname), "%sLP5xx_C%02o_E%o", lc->extPath, channelNo, equipmentNo);

    //  SZoppi: this can happen if something goes wrong in the open
    //          and the file fails to be properly re-opened.
    if (dp->fcb[0] == NULL)
    {
        printf("(lp3000 ) lp3000RemovePaper: FCB is Null on channel %o equipment %o\n",
            dp->channel->id,
            dp->eqNo);
        return;
    }
    else
    {

        /*
        **  Close the old device file.
        */
        fflush(dp->fcb[0]);

        if (ftell(dp->fcb[0]) == 0)
        {
            printf("(lp3000 ) No output has been written on channel %o and equipment %o\n", channelNo, equipmentNo);
            return;
        }

        fclose(dp->fcb[0]);
        dp->fcb[0] = NULL;

        /*
        **  Rename the device file to the format "LP5xx_yyyymmdd_hhmmss_nn".
        */

        for (isuffix = 0; isuffix < 100; isuffix++)
        {
            time(&currentTime);
            t = *localtime(&currentTime);
            sprintf_s(fnameNew, sizeof(fnameNew), "%sLP5xx_%04d%02d%02d_%02d%02d%02d_%02d.txt",
                lc->extPath,
                t.tm_year + 1900,
                t.tm_mon + 1,
                t.tm_mday,
                t.tm_hour,
                t.tm_min,
                t.tm_sec,
                isuffix);

            if (rename(fname, fnameNew) == 0)
            {
                break;
            }
            printf("(lp3000 ) Could not rename '%s' to '%s' - %s (retrying)\n", fname, fnameNew, strerror(errno));
        }
    }
    /*
    **  Open the device file.
    */
    dp->fcb[0] = fopen(fname, "w");

    /*
    **  Check if the open succeeded.
    */
    if (dp->fcb[0] == NULL)
        {
        printf("Failed to open %s\n", fname);
        return;
        }

    printf("(lp3000 ) Paper removed from 5xx printer and available on '%s'\n", fnameNew);


#if defined(_WIN32)
	if (autoRemovePaper != 0 && strlen(printApp) > 5)
	{
		for (int i = 0; i < 256; i++)
		{
			if (fnameNew[i] == 0)
				break;
			if (fnameNew[i] == '/')
				fnameNew[i] = '\\';
		}
		intptr_t ret = _spawnl(_P_NOWAIT, printApp, printApp, fnameNew, printApp, NULL);
		printf("\n");
	}
#endif    
    
    }

/*--------------------------------------------------------------------------
**  Purpose:        Execute function code on 3000 line printer.
**
**  Parameters:     Name        Description.
**                  funcCode    function code
**
**  Returns:        FcStatus
**
**------------------------------------------------------------------------*/
static FcStatus lp3000Func(PpWord funcCode)
    {
    FILE *fcb;
    LpContext *lc;
    char lpdevid[16];               //  Used for automatically removing printouts at EOJ
    unsigned int channelid;
    unsigned int deviceid;

    //  SZoppi: this can happen if something goes wrong in the open
    //          and the file fails to be properly re-opened.
    if (active3000Device->fcb[0] == NULL)
    {
        printf("(lp3000 ) lp3000Func: FCB is Null on channel %o equipment %o\n",
            active3000Device->channel->id,
            active3000Device->eqNo);
        return(FcProcessed);
    }

    fcb = active3000Device->fcb[0];
    lc = (LpContext *)active3000Device->context[0];

    /*
    **  Note that we don't emulate the VFU, so all VFU control codes
    **  are implemented as NOPs.
    */

#if DEBUG
    fprintf(lp3000Log, "\n%06d PP:%02o CH:%02o f:%04o T:%-25s  >   ",
            traceSequenceNo,
            activePpu->id,
            activeDevice->channel->id,
            funcCode,
            lp3000Func2String(funcCode));
#endif

    //  Start with the common codes
    //  20171022: This section includes the "pre-print" carriage controls

    switch (funcCode)
        {
            case FcPrintNoSpace:
                lc->extsuppress = TRUE;
                return(FcProcessed);

            case FcPrintAutoEject:
                return(FcProcessed);

            case Fc6681MasterClear:
                /*
                **	SZoppi: Extensions
                */
                lc->extspaceopt = FcPrintSingle;    //  Default Single Space
                lc->extlpi = 6;						//  Default Page Length
                lc->extlpp = LpInchesPerPage * lc->extlpi;
                lc->extcurline = 1;					//	1 = Initialized
                lc->extsuppress = FALSE;            //  Clear Suppress Print
                lc->extpostprint = TRUE;			//  Post-Print Mode by Default
                if (lc->extuseANSI)
                {
                    fputc('1', fcb);
                }
                else
                {
                    fputc('\f', fcb);
                }
                return(FcProcessed);

            case FcPrintRelease:
                // clear all interrupt conditions
                lc->flags &= ~(StPrintIntReady | StPrintIntEnd);

                // Release is sent at end of job, so flush the print file
                if (lc->printed)
                    {
                    fflush(fcb);
                    channelid = (int) active3000Device->channel->id;
                    deviceid = (int) active3000Device->eqNo;
                    sprintf_s(lpdevid, sizeof(lpdevid), "%o,%o", channelid, deviceid);
                    lp3000RemovePaper (lpdevid);
                    lc->printed = FALSE;
                    }
                return(FcProcessed);

            case FcPrintSingle:
                lc->extspaceopt = FcPrintSingle;
                if (lc->extuseANSI)
                {
                    if (!lc->extpostprint)
                    {
                        //  if we are not post-print spacing,
                        //  then we space pre-print
                        fprintf(fcb, "\n ");
                        lc->extcurline++;
                    }
                }
                else
                {
                    fputc('\n', fcb);
                    lc->extcurline++;
                }


            #if DEBUG
                lp3000DebugData();
            #endif
                return(FcProcessed);

            case FcPrintLastLine:
                //  Treat last-line codes as a single blank line
                //  ToDo:   This will need to be reimplemented when
                //          we have the current line counters working.
                if (lc->extuseANSI)
                {
                    fprintf(fcb, "\n ");
                    lc->extcurline++;
                }
                else
                {
                    fputc('\n', fcb);
                    lc->extcurline++;
                }

            #if DEBUG
                lp3000DebugData();
            #endif
                return(FcProcessed);

            case FcPrintEject:
                // Turn eject into a formfeed character
                lc->extcurline = 1;

                if (lc->extuseANSI)
                {
                    fprintf(fcb, "\n1");
                }
                else 
                {
                    fputc('\f', fcb);
                }
            #if DEBUG
                lp3000DebugData();
            #endif

                return(FcProcessed);

            case FcPrintDouble:
                lc->extspaceopt = FcPrintDouble;

                if (lc->extuseANSI)
                {
                    if (!lc->extpostprint)
                    {
                        fprintf(fcb, "\n0");
                        lc->extcurline += 2;
                    }
                }
                else
                {
                    fprintf(fcb, "\n\n");
                    lc->extcurline += 2;
                }

            #if DEBUG
                lp3000DebugData();
            #endif
                return(FcProcessed);

            case Fc6681Output:
                if (lc->flags & Lp3555FillImageMem)
                    {
                    // Tweak the function code to tell I/O handler to toss this data
                    funcCode++;
                    // Now clear the flag
                    lc->flags &= ~Lp3555FillImageMem;
                    }
                // Initially clear interrupt status flags
                lc->flags &= ~(StPrintIntReady | StPrintIntEnd);

                // Update interrupt status to reflect what status
                // will be when this transfer is finished.
                // Ok, so that's cheating a bit...
                if (lc->flags & Lp3000IntReadyEna)
                    {
                    lc->flags |= Lp3000IntReady;
                    }
                if (lc->flags & Lp3000IntEndEna)
                    {
                    lc->flags |= Lp3000IntEnd;
                    }

                // Update interrupt summary flag in unit block
                dcc6681Interrupt((lc->flags & (Lp3000IntReady | Lp3000IntEnd)) != 0);
                active3000Device->fcode = funcCode;
                return(FcAccepted);

            case Fc6681DevStatusReq:
                active3000Device->fcode = funcCode;
                return(FcAccepted);
        }

    if (lc->flags & Lp3000Type3555)		// This is LP3555
        {
        switch (funcCode)
            {
                default:
                    printf("(lp3000 ) Unknown LP3555 function %04o\n", funcCode);
                    return(FcProcessed);

                case Fc3555CondClearFormat:
                    
                    return(FcProcessed);

                case Fc3555Sel8Lpi:
                    lc->extlpi = 8;
                    lc->extlpp = LpInchesPerPage * lc->extlpi;
                    return(FcProcessed);

                case Fc3555Sel6Lpi:
                    lc->extlpi = 6;
                    lc->extlpp = LpInchesPerPage * lc->extlpi;
                    return(FcProcessed);

                case Fc3555SelExtArray:
                case Fc3555ClearExtArray:
                case Fc3555SelIntError:
                case Fc3555RelIntError:
                case Fc3555ReloadMemEnable:
                    return(FcProcessed);

                case Fc3555ClearFormat:
                    /*
                    **	SZoppi: Extensions
                    */
                    lc->extspaceopt = FcPrintSingle;	//	Non-Zero = Print-Then-Space 
                                                        //  (Post-Print Spacing)
                    lc->extlpi = 6;						//  Default Page Length
                    lc->extlpp = LpInchesPerPage * lc->extlpi;
                    lc->extsuppress = FALSE;
                    lc->extspaceopt = FcPrintSingle;
                    lc->extpostprint = TRUE;
                    return(FcProcessed);

                case Fc3555PostVFU1:
                case Fc3555PostVFU2:
                case Fc3555PostVFU3:
                case Fc3555PostVFU4:
                case Fc3555PostVFU5:
                case Fc3555PostVFU6:
                case Fc3555PostVFU7:
                case Fc3555PostVFU8:
                case Fc3555PostVFU9:
                case Fc3555PostVFU10:
                case Fc3555PostVFU11:
                case Fc3555PostVFU12:
                    return(FcProcessed);

                case Fc3555SelectPreprint:
                    // Zero Function Code
                    lc->extpostprint = FALSE;
                    return(FcProcessed);

                case Fc3555PreVFU1:
                case Fc3555PreVFU2:
                case Fc3555PreVFU3:
                case Fc3555PreVFU4:
                case Fc3555PreVFU5:
                case Fc3555PreVFU6:
                case Fc3555PreVFU7:
                case Fc3555PreVFU8:
                case Fc3555PreVFU9:
                case Fc3555PreVFU10:
                case Fc3555PreVFU11:
                case Fc3555PreVFU12:
                case Fc3555MaintStatus:
                case Fc3555ClearMaint:
                    // All of the above are NOPs
                    return(FcProcessed);

                case Fc3555FillMemory:
                    // Remember that we saw this function, but this doesn't actually
                    // start any I/O yet.
                    lc->flags |= Lp3555FillImageMem;
                    return(FcProcessed);

                case Fc3555SelIntReady:
                    // Enable next int.  If an I/O was done since the last
                    // time an int enable was issued, don't clear the
                    // current int.  That's because things go very slowly
                    // otherwise; printer drivers typically issue the write,
                    // then enable the int shortly after.  We've already set
                    // "ready" by then, unlike physical printers.
                    lc->flags |= Lp3000IntReady | Lp3000IntReadyEna;
                    if (lc->keepInt)
                        {
                        lc->keepInt = FALSE;
                        }
                    else
                        {
                        lc->flags &= ~Lp3000IntReady;
                        }
                    // Update interrupt summary flag in unit block
                    dcc6681Interrupt((lc->flags & (Lp3000IntReady | Lp3000IntEnd)) != 0);
                    return(FcProcessed);

                case Fc3555RelIntReady:
                    lc->flags &= ~(Lp3000IntReadyEna | Lp3000IntReady);
                    // Update interrupt summary flag in unit block
                    dcc6681Interrupt((lc->flags & (Lp3000IntReady | Lp3000IntEnd)) != 0);
                    return(FcProcessed);

                case Fc3555SelIntEnd:
                    lc->flags |= Lp3000IntEnd | Lp3000IntEndEna;
                    if (lc->keepInt)
                        {
                        lc->keepInt = FALSE;
                        }
                    else
                        {
                        lc->flags &= ~Lp3000IntEnd;
                        }
                    // Update interrupt summary flag in unit block
                    dcc6681Interrupt((lc->flags & (Lp3000IntReady | Lp3000IntEnd)) != 0);
                    return(FcProcessed);

                case Fc3555RelIntEnd:
                    lc->flags &= ~(Lp3000IntEndEna | Lp3000IntEnd);
                    // Update interrupt summary flag in unit block
                    dcc6681Interrupt((lc->flags & (Lp3000IntReady | Lp3000IntEnd)) != 0);
                    return(FcProcessed);
            }
        }
    else	// This is LP3152
        {
        switch (funcCode)
            {
                default:
                    printf("(lp3000 ) Unknown LP3152 function %04o\n", funcCode);
                    return(FcProcessed);

                case Fc3152ClearFormat:
                    lc->extpostprint = TRUE;
                    return(FcProcessed);

                case Fc3152PostVFU1:
                case Fc3152PostVFU2:
                case Fc3152PostVFU3:
                case Fc3152PostVFU4:
                case Fc3152PostVFU5:
                case Fc3152PostVFU6:
                    return(FcProcessed);

                case Fc3152SelectPreprint:
                    lc->extpostprint = FALSE;
                    return(FcProcessed);

                case Fc3152PreVFU1:
                case Fc3152PreVFU2:
                case Fc3152PreVFU3:
                case Fc3152PreVFU4:
                case Fc3152PreVFU5:
                case Fc3152PreVFU6:
                case Fc3152SelIntError:
                case Fc3152RelIntError:
                case Fc3152Release2:
                    // All of the above are NOPs
                    return(FcProcessed);

                case Fc3152SelIntReady:
                    // Enable next int.  If an I/O was done since the last
                    // time an int enable was issued, don't clear the
                    // current int.  That's because things go very slowly
                    // otherwise; printer drivers typically issue the write,
                    // then enable the int shortly after.  We've already set
                    // "ready" by then, unlike physical printers.
                    lc->flags |= Lp3000IntReady | Lp3000IntReadyEna;
                    if (lc->keepInt)
                        {
                        lc->keepInt = FALSE;
                        }
                    else
                        {
                        lc->flags &= ~Lp3000IntReady;
                        }
                    // Update interrupt summary flag in unit block
                    dcc6681Interrupt((lc->flags & (Lp3000IntReady | Lp3000IntEnd)) != 0);
                    return(FcProcessed);

                case Fc3152RelIntReady:
                    lc->flags &= ~(Lp3000IntReadyEna | Lp3000IntReady);
                    // Update interrupt summary flag in unit block
                    dcc6681Interrupt((lc->flags & (Lp3000IntReady | Lp3000IntEnd)) != 0);
                    return(FcProcessed);

                case Fc3152SelIntEnd:
                    lc->flags |= Lp3000IntEnd | Lp3000IntEndEna;
                    if (lc->keepInt)
                        {
                        lc->keepInt = FALSE;
                        }
                    else
                        {
                        lc->flags &= ~Lp3000IntEnd;
                        }
                    // Update interrupt summary flag in unit block
                    dcc6681Interrupt((lc->flags & (Lp3000IntReady | Lp3000IntEnd)) != 0);
                    return(FcProcessed);

                case Fc3152RelIntEnd:
                    lc->flags &= ~(Lp3000IntEndEna | Lp3000IntEnd);
                    // Update interrupt summary flag in unit block
                    dcc6681Interrupt((lc->flags & (Lp3000IntReady | Lp3000IntEnd)) != 0);
                    return(FcProcessed);
            }
        }

    active3000Device->fcode = funcCode;
    return(FcAccepted);
    }

/*--------------------------------------------------------------------------
**  Purpose:        Perform I/O on 3000 line printer.
**
**  Parameters:     Name        Description.
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
static void lp3000Io(void)
    {
    FILE *fcb;
    LpContext *lc;

    //  SZoppi: this can happen if something goes wrong in the open
    //          and the file fails to be properly re-opened.
    if (active3000Device->fcb[0] == NULL)
    {
        printf("(lp3000 ) lp3000Io: FCB is Null on channel %o equipment %o\n",
            active3000Device->channel->id,
            active3000Device->eqNo);
        return;
    }

    fcb = active3000Device->fcb[0];
    lc = (LpContext *)active3000Device->context[0];

    /*
    **  Process printer I/O.
    */
    switch (active3000Device->fcode)
        {
            default:
                activeChannel->full = FALSE;
                break;

            case Fc6681Output:
                if (activeChannel->full)
                    {
                #if DEBUG
                    if (linePos < MaxLine)
                        {
                        lineData[linePos++] = activeChannel->data & Mask12;
                        }
                #endif

                    if (lc->flags & Lp3000Type501)
                        {
                        // 501 printer, output display code
                        fputc(bcdToAscii[(activeChannel->data >> 6) & Mask6], fcb);
                        fputc(bcdToAscii[activeChannel->data & Mask6], fcb);
                        }
                    else
                        {
                        // 512 printer, output ASCII
                        fputc(activeChannel->data & 0377, fcb);
                        }
                    activeChannel->full = FALSE;
                    lc->printed = TRUE;
                    lc->keepInt = TRUE;
                    }
                break;

            case Fc6681Output + 1:
                // Fill image memory, just ignore that data
                activeChannel->full = FALSE;
                break;

            case Fc6681DevStatusReq:
                // Indicate ready plus whatever interrupts are enabled
                activeChannel->data = StPrintReady |
                    (lc->flags &
                    (StPrintIntReady | StPrintIntEnd));
                activeChannel->full = TRUE;
                active3000Device->fcode = 0;
                break;
        }
    }

/*--------------------------------------------------------------------------
**  Purpose:        Handle channel activation.
**
**  Parameters:     Name        Description.
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
static void lp3000Activate(void)
    {
    }

/*--------------------------------------------------------------------------
**  Purpose:        Handle disconnecting of channel.
**
**  Parameters:     Name        Description.
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
static void lp3000Disconnect(void)
    {
    FILE *fcb = active3000Device->fcb[0];
    LpContext *lc = (LpContext *)active3000Device->context[0];

    //  SZoppi: this can happen if something goes wrong in the open
    //          and the file fails to be properly re-opened.
    if (active3000Device->fcb[0] == NULL)
    {
        printf("(lp3000 ) lp3000Disconnect: FCB is Null on channel %o equipment %o\n",
            active3000Device->channel->id,
            active3000Device->eqNo);
        return;
    }


    if (active3000Device->fcode == Fc6681Output)
        {
        if (lc->extsuppress)
        {
            //	20171022: There was no LineFeed Action expected so 
            //			  we use Carriage Return Only for NON-ASA
            //			  otherwise the default is a line-end.
            if (lc->extuseANSI)
            {
                fprintf(fcb, "\n+");
            }
            else
            {
                fputc('\r', fcb);
            }
            lc->extsuppress = FALSE;
        }
        else if (lc->extpostprint)
            {
            /*
            **	20171022: We perform post-print actions here if needed
            */
            switch (lc->extspaceopt)
                {
                    case FcPrintDouble:
                        if (lc->extuseANSI)
                        {
                            //  We simply terminate the existing line
                            fprintf(fcb, "\n0");
                        }
                        else
                        {
                            fprintf(fcb, "\n\n");
                        }
                    #if DEBUG
                        lp3000DebugData();
                    #endif
                        lc->extcurline += 2;
                        break;

                    case FcPrintSingle:
                    default:
                        //	At the end of every "Normal" line - 
                        //	we do a line end
                        if (lc->extuseANSI)
                        {
                            fprintf(fcb, "\n ");
                        }
                        else
                        {
                            fputc('\n', fcb);
                        }
                        lc->extcurline++;

                    #if DEBUG
                        lp3000DebugData();
                    #endif
                        break;
                }
            lc->extspaceopt = FcPrintSingle;
            }
        else
            {
            // TODO: This is an experimental flag to help me debug.
            //       Although it should never happen - we look for it.
            fputc('\t', fcb);
            }

    #if DEBUG
        lp3000DebugData();
    #endif
        active3000Device->fcode = 0;
        }
    }



/*--------------------------------------------------------------------------
**  Purpose:        Dump raw line data.
**
**  Parameters:     Name        Description.
**
**  Returns:        Nothing.
**
**------------------------------------------------------------------------*/
static void lp3000DebugData(void)
    {
#if DEBUG
    int i;

    if (linePos == 0)
        {
        return;
        }

    for (i = 0; i < linePos; i++)
        {
        if (i % 16 == 0)
            {
            fputc('\n', lp3000Log);
            }

        //        fprintf(lp3000Log, " %04o", lineData[i]);
        fprintf(lp3000Log, "%c%c", bcdToAscii[(lineData[i] >> 6) & Mask6], bcdToAscii[(lineData[i] >> 0) & Mask6]);
        }

    fputc('\n', lp3000Log);

    linePos = 0;
#endif
    }

/*--------------------------------------------------------------------------
**  Purpose:        Convert function code to string.
**
**  Parameters:     Name        Description.
**                  funcCode    function code
**
**  Returns:        String equivalent of function code.
**
**------------------------------------------------------------------------*/
static char *lp3000Func2String(PpWord funcCode)
    {
    static char buf[30];
#if DEBUG
    switch (funcCode)
        {
        //    case Fc669FormatUnit             : return "Fc669FormatUnit";
            default:
                break;
    }
#endif
    sprintf(buf, "(lp3000 ) Unknown Function: %04o", funcCode);
    return(buf);
    }

/*---------------------------  End Of File  ------------------------------*/
