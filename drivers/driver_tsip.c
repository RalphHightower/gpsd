/*
 * Handle the Trimble TSIP packet format
 * by Rob Janssen, PE1CHL.
 * Acutime Gold support by Igor Socec <igorsocec@gmail.com>
 * Trimble RES multi-constellation support by Nuno Goncalves <nunojpg@gmail.com>
 *
 * Week counters are not limited to 10 bits. It's unknown what
 * the firmware is doing to disambiguate them, if anything; it might just
 * be adding a fixed offset based on a hidden epoch value, in which case
 * unhappy things will occur on the next rollover.
 *
 * TSIPv1n RES270 Resolution SMTx support added by:
 *     Gary E. Miller <gem@rellim.com>
 *
 * This file is Copyright by the GPSD project
 * SPDX-License-Identifier: BSD-2-clause
 */

#include "../include/gpsd_config.h"   // must be before all includes

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>           // For llabs()
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../include/compiler.h"   // for FALLTHROUGH
#include "../include/gpsd.h"
#include "../include/bits.h"
#include "../include/strfuncs.h"
#include "../include/timespec.h"

#ifdef TSIP_ENABLE
// RES SMT 360 has 32 max channels, use 64 for next gen
#define TSIP_CHANNELS  64

/* defines for Set or Request I/O Options (0x35)
 * SMT 360 default: IO1_DP|IO1_LLA, IO2_ENU, 0, IO4_DBHZ */
// byte 1 Position
#define IO1_ECEF 1
#define IO1_LLA 2
#define IO1_MSL 4
#define IO1_DP 0x10
// IO1_8F20 not in SMT 360
#define IO1_8F20 0x20
// byte 2 Velocity
#define IO2_VECEF 1
#define IO2_ENU 2
// byte 3 Timing
#define IO3_UTC 1
// byte 4 Aux/Reserved
#define IO4_RAW 1
#define IO4_DBHZ 8

#define SEMI_2_DEG      (180.0 / 2147483647L)    // 2^-31 semicircle to deg

// Start TSIPv1 values and flags

/* Data Bits v1
 * Used in x91-00 */
static struct vlist_t vdbits1[] = {
    {3, "8 bits"},
    {0, NULL},
};

/* Error Code Flags
 * Used in xa3-21 */
static struct vlist_t verr_codes1[] = {
    {1, "Parameter Error"},
    {2, "Length Error"},
    {3, "Invalid Parket Format"},
    {4, "Invalid Checksum"},
    {5, "Bad TNL/User mode"},
    {6, "Invalid Packet ID"},
    {7, "Invalid Subacket ID"},
    {8, "Update in Progress"},
    {9, "Internal Error (div by 0)"},
    {10, "Internal Error (failed queuing)"},
    {0, NULL},
};

/* Fix Type v1
 * Used in xa1-11 */
static struct vlist_t vfix_type1[] = {
    {0, "No Fix"},
    {1, "1D"},
    {2, "3D"},
    {0, NULL},
};

/* GNSS Decoding Status to string
 * Used in xa3-11 */
static struct vlist_t vgnss_decode_status1[] = {
    {0, "Doing Fixes"},
    {1, "No GPS time"},
    {2, "PDOP too high"},
    {3, "0 usable sats"},
    {4, "1 usable sat"},
    {5, "2 usable sats"},
    {6, "3 usable sats"},
    {0xff, "GPS Time Fix (OD mode)"},
    {0, NULL},
};

/* Major Alarm Flags v1
 * Used in xa3-00 */
static struct flist_t vmajor_alarms1[] = {
    {1, 1, "Not tracking sats"},
    {2, 2, "PPS bad"},
    {4, 4, "PPS not generated"},
    {0x80, 0x80, "Spoofing/Multipath"},
    {0x100, 0x100, "Jamming"},
    {0, 0, NULL},
};

/* Minor Alarm Flags v1
 * Used in xa3-00 */
static struct flist_t vminor_alarms1[] = {
    {1, 1, "Ant Open"},
    {2, 2, "Ant Short"},
    {4, 4, "Leap Pending"},
    {8, 8, "Almanac Incomplete"},
    {0x10, 0x10, "Survey in Progress"},
    {0x20, 0x20, "GPS Almanac Incomplete"},
    {0x20, 0x20, "GLO Almanac Incomplete"},
    {0x40, 0x40, "BDS Almanac Incomplete"},
    {0x80, 0x80, "GAL Almanac Incomplete"},
    {0x100, 0x100, "Leap Second Insertion"},
    {0x200, 0x200, "Leap Second Deletion"},
    {0, 0, NULL},
};

/* Parity v1
 * Used in x91-00 */
static struct vlist_t vparity1[] = {
    {0, "None bits"},
    {1, "Odd"},
    {2, "Even"},
    {255, "Ignore"},
    {0, NULL},
};

/* Port Name v1
 * Used in x91-00 */
static struct vlist_t vport_name1[] = {
    {0, "Port A"},
    {1, "Port B"},
    {255, "Current Port"},
    {0, NULL},
};

/* Port Type v1
 * Used in x91-00 */
static struct vlist_t vport_type1[] = {
    {0, "UART"},
    {0, NULL},
};

/* Position Mask
 * Used in xa1-11 */
static struct flist_t vpos_mask1[] = {
    {0, 1, "Real Time Position"},
    {1, 1, "Surveyed Position"},
    {0, 2, "LLA Position"},
    {2, 2, "XYZ ECEF"},
    {0, 4, "HAE"},
    {4, 4, "MSL"},
    {0, 8, "Velocity ENU"},
    {8, 8, "Velocity ECEF"},
    {0, 0, NULL},
};

/* PPS Mask v1
 * Used in x91-03 */
static struct vlist_t vpps_mask1[] = {
    {0, "Off"},
    {1, "On"},
    {2, "Fix Based"},
    {3, "When Valid"},
    {4, "Off"},
    {5, "On/Negative"},
    {6, "Fix Based/Negative"},
    {7, "When Valid/Negative"},
    {0, NULL},
};

/* Protocol v1
 * Used in x91-00 */
static struct vlist_t vprotocol1[] = {
    {2, "TSIP"},
    {4, "NMEA"},
    {255, "Ignore"},
    {0, NULL},
};

/* Receiver Mode v1
 * Used in xa3-11 */
static struct vlist_t vrec_mode1[] = {
    {0, "2D"},
    {1, "(3D) Time Only"},
    {3, "Automatic"},
    {6, "Overdetermined"},
    {0, NULL},
};

/* Reset Type, Reset Cause
 * Used in x92-00, x92-01 */
static struct vlist_t vreset_type1[] = {
    {1, "No Reset"},           // x92-01 only
    {1, "Cold Reset"},
    {2, "Hot Reset"},
    {3, "Warm Reset"},
    {4, "Factory Reset"},
    {5, "System Reset"},
    {6, "Power Cycle"},        // x92-01 only
    {7, "Watchdog"},           // x92-01 only
    {8, "Hardfault"},          // x92-01 only
    {0, NULL},
};

/* Satellite Flags v1
 * Used in xa2-00 */
static struct flist_t vsflags1[] = {
    {1, 1, "Acquired"},
    {2, 2, "Used in Position"},
    {4, 4, "Used in PPS"},
    // Bits 8 - 15 "Satellite Status, otherwise undocumented.
    {0, 0, NULL},
};

/* Speed v1
 * Used in x91-00 */
static struct vlist_t vspeed1[] = {
    {11, "115200"},
    {12, "230400"},
    {13, "460800"},
    {14, "1821600"},
    {255, "Ignore"},
    {0, NULL},
};

/* Self-Survey Mask v1
 * Used in x91-04 */
static struct flist_t vss_mask1[] = {
    {1, 1, "SS restarted"},
    {0, 2, "SS Disabled"},
    {2, 2, "SS Enabled"},
    {0, 8, "Don't save position"},
    {8, 8, "Save position"},
    {0, 0, NULL},
};

/* Stop Bits v1
 * Used in x91-00 */
static struct vlist_t vstop1[] = {
    {0, "1 bit"},
    {1, "2 bit"},
    {255, "Ignore"},
    {0, NULL},
};

/* SV Type v1
 * Used in xa2-00 */
static struct vlist_t vsv_type1[] = {
    {1, "GPS L1C"},
    {2, "GPS L2"},
    {3, "GPS L5"},
    {5, "GLO G1"},
    {6, "GLO G2"},
    {9, "SBAS"},
    {13, "BDS B1"},
    {14, "BDS B2i"},
    {15, "BDS B2a"},
    {17, "GAL E1"},
    {18, "GAL E5a"},
    {19, "GAL E5b"},
    {20, "GAL E6"},
    {22, "QZSS L1"},
    {23, "QZSS L2C"},
    {24, "QZSS L5"},
    {26, "IRNSS L5"},
    {0, NULL},
};

/* SV Types v1
 * Used in x91-01 */
static struct flist_t vsv_types1[] = {
    {1, 1, "GPS L1C"},
    {2, 2, "GPS L2"},
    {4, 3, "GPS L5"},
    {0x20, 0x20, "GLO G1"},
    {0x40, 0x40, "GLO G2"},
    {0x100, 0x100, "SBAS"},
    {0x1000, 0x1000, "BDS B1"},
    {0x2000, 0x2000, "BDS B2i"},
    {0x4000, 0x4000, "BDS B2a"},
    {0x10000, 0x10000, "GAL E1"},
    {0x20000, 0x20000, "GAL E5a"},
    {0x40000, 0x40000, "GAL E5b"},
    {0x80000, 0x80000, "GAL E6"},
    {0x100000, 0x100000, "QZSS L1"},
    {0x200000, 0x200000, "QZSS L2C"},
    {0x400000, 0x400000, "QZSS L5"},
    {0x1000000, 0x1000000, "IRNSS L5"},
    {0, 0, NULL},
};

/* Time Base v1
 * Used in x91-03, xa1-00 */
static struct vlist_t vtime_base1[] = {
    {0, "GPS"},
    {1, "GLO"},
    {2, "BDS"},
    {3, "GAL"},
    {4, "GPS/UTC"},
    {6, "GLO/UTC"},
    {6, "BDS/UTC"},
    {7, "GAL/UTC"},
    {0, NULL},
};

/* Time Flags v1
 * Used in xa1-00 */
static struct flist_t vtime_flags1[] = {
    {0, 1, "UTC Invalid"},
    {1, 1, "UTC Valid"},
    {0, 2, "Time Invalid"},
    {2, 2, "Time Valid"},
    {0, 0, NULL},
};

// End TSIPv1 values and flags

// Start TSIP values and flags

/* Error Code Flags
 * Used in x46 */
static struct flist_t verr_codes[] = {
    {1, 1, "No Bat"},
    {0x10, 0x30, "Ant Open"},
    {0x30, 0x30, "Ant Short"},
    {0, 0, NULL},
};

/* GNSS Decoding Status to string
 * Used in x46, x8f-ac */
static struct vlist_t vgnss_decode_status[] = {
    {0, "Doing Fixes"},
    {1, "No GPS time"},
    {2, "Needs Init"},                      // ACE II, LassenSQ
    {3, "PDOP too high"},
    {8, "0 usable sats"},
    {9, "1 usable sat"},
    {10, "2 usable sats"},
    {11, "3 usable sats"},
    {12, "chosen sat unusable"},
    {16, "TRAIM rejected"},                 // Thunderbolt E
    {0xbb, "GPS Time Fix (OD mode)"},       // Acutime 360
    {0, NULL},
};

/* Disciplining Activity
 * Used in x46, x8f-ac */
static struct vlist_t vdisc_act[] = {
    {0, "Phase Locking"},
    {1, "OSC Wrm-up"},
    {2, "Freq lokgin"},
    {3, "Placing PPS"},
    {4, "Init Loop FIlter"},
    {5, "Comp OCXO"},
    {6, "Inactive"},
    {7, "Not used"},
    {8, "REcovery Mode"},
    {0, NULL},
};

/* PPS indication
 * Used in x46, x8f-ac */
static struct vlist_t vpps_ind[] = {
    {0, "PPS Good"},
    {1, "PPS Ungood"},
    {0, NULL},
};

/* PPS Reference
 * Used in x46, x8f-ac */
static struct vlist_t vpps_ref[] = {
    {0, "GNSS"},
    {1, "Externa;"},
    {0xff, "None;"},
    {0, NULL},
};

/* Packet Broadcast Mask
 * Used in x8f-a3 */
static struct flist_t vpbm_mask0[] = {
    {1, 1, "x8f-ab"},
    {4, 4, "x8f-ac"},
    {0x40, 0x40, "Automatic"},
    {0, 0, NULL},
};

/* Receiver Mode
 * Used in xbb, x8f-ac */
static struct vlist_t vrec_mode[] = {
    {0, "Autonomous (2D/3D)"},
    {1, "Time Only (1-SV)"},    // Accutime 2000, Tbolt
    {3, "2D"},                  // Accutime 2000, Tbolt
    {4, "3D"},                  // Accutime 2000, Tbolt
    {5, "DGPS"},                // Accutime 2000, Tbolt
    {6, "2D Clock hold"},       // Accutime 2000, Tbolt
    {7, "Overdetermined"},      // Stationary Timing, surveyed
    {0, NULL},
};

/* Save Status
 * Used in x91-02 */
static struct flist_t vsave_status1[] = {
    {0, 1, "Save failed"},
    {1, 1, "Save OK"},
    {0, 0, NULL},
};

/* Self-Survey Enable
 * Used in x8f-a9 */
static struct vlist_t vss_enable[] = {
    {0, "SS Disabled"},
    {1, "SS Eabled"},
    {0, NULL},
};

/* Self-Survey Save
 * Used in x8f-a9 */
static struct vlist_t vss_save[] = {
    {0, "Don't Save"},
    {1, "Save at end"},
    {0, NULL},
};

/* Status 1
 * Used in x4b */
static struct flist_t vstat1[] = {
    {2, 2, "RTC invalid"},
    {8, 8, "No Almanac"},
    {0, 0, NULL},
};

/* Status 2
 * Used in x4b */
static struct flist_t vstat2[] = {
    {1, 1, "Superpackets"},      // x8f-20 (LFwEI)
    {2, 2, "Superpackets 2"},    // x8f-1b, x8f-ac
    {0, 0, NULL},
};

/* SV Bad
 * Used in x5d */
static struct vlist_t vsv_bad[] = {
    {0, "OK"},
    {1, "Bad Parity"},
    {2, "Bad Health"},
    {0, NULL},
};

/* SV Type
 * Used in x5d */
static struct vlist_t vsv_type[] = {
    {0, "GPS"},
    {1, "GLO"},
    {2, "BDS"},
    {3, "GAL"},
    {6, "QZSS"},
    {0, NULL},
};

/* SV Used Flags
 * Used in x5d */
static struct flist_t vsv_used_flags[] = {
    {1, 1, "Used in Timing"},
    {2, 2, "Used in Position"},
    {0, 0, NULL},
};

/* x4c Dynamics Code
 * Used in x4c */
static struct vlist_t vx4c_dyncode[] = {
    {1, "Land"},             // < 120 knots
    {2, "Sea"},              // < 50 knots
    {3, "Air"},              // > 800 knots
    {0,NULL},
};

/* x55 auxiliary
 * Used in x55 */
static struct flist_t vx55_aux[] = {
    {0, 1, "x5a Off"},
    {1, 1, "x5a On"},
    {0, 0, NULL},
};

/* x55 Position
 * Used in x55 */
static struct flist_t vx55_pos[] = {
    {1, 1, "ECEF On"},
    {2, 2, "LLA On"},
    {0, 4, "HAE"},
    {4, 4, "MSL"},
    {0, 0x10, "Single Precision"},
    {0x10, 0x104, "Double Position"},
    {0, 0, NULL},
};

/* x55 Timing
 * Used in x55 */
static struct flist_t vx55_timing[] = {
    {1, 1, "Use x8e-a2"},
    {0, 0, NULL},
};

/* x55 Velocity
 * Used in x55 */
static struct flist_t vx55_vel[] = {
    {1, 1, "ECEF On"},
    {2, 2, "ENU On"},
    {0, 0, NULL},
};

/* x57 Source of Info
 * Used in x57 */
static struct flist_t vx57_info[] = {
    {0, 1, "Old Fix"},
    {1, 1, "New Fix"},
    {0, 0, NULL},
};

/* x57 Fix Mode
 * Used in x6c, x57, yet another decode of the same data... */
static struct vlist_t vx57_fmode[] = {
    {0, "No Fix"},
    {1, "Time"},             // Time only 1SV/2D
    {3, "2D Fix"},
    {4, "3D Fix"},
    {5, "OD Fix"},
    {0,NULL},
};

/* x5c Acquisition Flag
 * Used in x5c */
static struct vlist_t vx5c_acq[] = {
    {0, "Never"},
    {1, "Yes"},
    {2, "Search"},
    {0,NULL},
};

/* x5c Ephemeris Flag
 * Used in x5c */
static struct vlist_t vx5c_eflag[] = {
    {0, "none"},
    {1, "Decoded"},
    {3, "Decoded/Healthy"},
    {19, "Used"},
    {51, "Used/DGPS"},
    {0,NULL},
};

/* x82 Mode Timing
 * Used in x82 */
static struct vlist_t vx82_mode[] = {
    {0, "Man DGPS Off"},      // No DPGS ever
    {1, "Man DGPS OOn"},      // Only DPGS ever
    {2, "Auto DGPS Off"},     // DGPS unavailable
    {3, "Auto DGPS On"},      // DGPS available, and in use
    {4, NULL},
};

/* x8f-20 Fix Flags
 * Used in x8f-20 */
static struct flist_t vx8f_20_fflags[] = {
    {0, 1, "Fix Yes"},
    {2, 2, "DGPS"},
    {0, 4, "3D"},
    {4, 4, "2D"},
    {8, 8, "Alt Holdt"},
    {0x10, 0x10, "Filtered"},
    {0, 0, NULL},
};

/* Fis Dimension, Fix Mode
 * Used in x6c, x6d */
static struct flist_t vfix[] = {
    // Accutime calls 0 "Auto"
    {0, 7, "No Fix"},       // not in ResSMT360
    // in x6d, Thunderbolt E calls 1 "1D Time Fix", not an OD Fix
    {1, 7, "1D/OD Fix"},
    // Accutime calls 3 "2D Clock Hold"
    {3, 7, "2D Fix"},
    {4, 7, "3D Fix"},
    {5, 7, "OD Fix"},       // in Thunderbolt E, x6d, others
    {6, 7, "DGPS"},         // in Accutime
    {0, 8, "Auto"},
    {8, 8, "Manual"},       // aka surveyed
    {0, 0, NULL},
};

/* Timing Flags
 * Used in x8f-ab */
static struct flist_t vtiming[] = {
    {0, 1, "GPS time"},
    {1, 1, "UTC time"},
    {0, 2, "GPS PPS"},
    {1, 2, "UTC PPS"},
    {4, 4, "Time not set"},
    {8, 8,  "no UTC info"},
    {0x10, 0x10, "time from user"},
    {0, 0, NULL},
};

/* Critical Alarm Flags
 * Used in x8f-ac */
static struct flist_t vcrit_alarms[] = {
    {1, 1, "ROM error"},               // Thunderbolt
    {2, 2, "RAM error"},               // Thunderbolt
    {4, 4, "FPGA error"},              // Thunderbolt
    {8, 8, "Power error"},             // Thunderbolt
    {0x10, 0x10, "OSC error"},         // Thunderbolt
    {0, 0, NULL},
};

/* Minor Alarm Flags
 * Used in x8f-ac */
static struct flist_t vminor_alarms[] = {
    {1, 1, "OSC warning"},                   // Thunderbolt
    {2, 2, "Ant Open"},
    {4, 4, "Ant Short"},
    {8, 8, "Not tracking Sats"},
    {0x10, 0x10, "Osc unlocked"},            // Thunderbolt
    {0x20, 0x20, "Survey in progress"},
    {0x40, 0x40, "No stored Position"},
    {0x80, 0x80, "Leap Sec Pending"},
    {0x100, 0x100, "Test Mode"},
    {0x200, 0x200, "Position questionable"},
    {0x400, 0x400, "EEROM corrupt"},         // Thunderbolt
    {0x800, 0x800, "Almanac Incomplete"},
    {0x1000, 0x1000, "PPS generated"},
    {0, 0, NULL},
};

/* convert TSIP SV Type to satellite_t.gnssid and satellite_t.svid
 * return gnssid directly, svid indirectly through pointer */
static unsigned char tsip_gnssid(unsigned svtype, short prn,
                                 unsigned char *svid)
{
    // initialized to shut up clang
    unsigned char gnssid = 0;

    *svid = 0;

    switch (svtype) {
    case 0:
        if (0 < prn && 33 > prn) {
            gnssid = GNSSID_GPS;
            *svid = prn;
        } else if (32 < prn && 55 > prn) {
            // RES SMT 360 and ICM SMT 360 put SBAS in 33-54
            gnssid = GNSSID_SBAS;
            *svid = prn + 87;
        } else if (64 < prn && 97 > prn) {
            // RES SMT 360 and ICM SMT 360 put GLONASS in 65-96
            gnssid = GNSSID_GLO;
            *svid = prn - 64;
        } else if (96 < prn && 134 > prn) {
            // RES SMT 360 and ICM SMT 360 put Galileo in 97-133
            gnssid = GNSSID_GAL;
            *svid = prn - 96;
        } else if (119 < prn && 139 > prn) {
            // Copernicus (II) put SBAS in 120-138
            gnssid = GNSSID_SBAS;
            *svid = prn + 87;
        } else if (183 == prn) {
            gnssid = GNSSID_QZSS;
            *svid = 1;
        } else if (192 <= prn && 193 >= prn) {
            gnssid = GNSSID_QZSS;
            *svid = prn - 190;
        } else if (200 == prn) {
            gnssid = GNSSID_QZSS;
            *svid = 4;
        } else if (200 < prn && 238 > prn) {
            // BeidDou in 201-237
            gnssid = GNSSID_BD;
            *svid = prn - 200;
        }
        // else: huh?
        break;
    case 1:
        gnssid = GNSSID_GLO;  // GLONASS
        *svid = prn - 64;
        break;
    case 2:
        gnssid = GNSSID_BD;  // BeiDou
        *svid = prn - 200;
        break;
    case 3:
        gnssid = GNSSID_GAL;  // Galileo
        *svid = prn - 96;
        break;
    case 5:
        gnssid = GNSSID_QZSS;  // QZSS
        switch (prn) {
        case 183:
            *svid = 1;
            break;
        case 192:
            *svid = 2;
            break;
        case 193:
            *svid = 3;
            break;
        case 200:
            *svid = 4;
            break;
        default:
            *svid = prn;
            break;
        }
        break;
    case 4:
        FALLTHROUGH
    case 6:
        FALLTHROUGH
    case 7:
        FALLTHROUGH
    default:
        *svid = 0;
        gnssid = 0;
        break;
    }
    return gnssid;
}

/* tsip1_checksum()
 * compute TSIP version 1 checksum
 *
 * Return: checksum
 */
static char tsip1_checksum(const char *buf, size_t len)
{
    char checksum = 0;
    size_t index;

    for(index = 0; index < len; index++) {
        checksum ^= buf[index];
    }
    return checksum;
}

/* tsip_write1() - send old style TSIP message, improved tsip_write()
 * buf - the packet
 * len - length of buf
 *
 * Adds leading DLE, and the trailing DLE, ETX
 *
 * Return: 0 == OK
 *         -1 == write fail
 */
static ssize_t tsip_write1(struct gps_device_t *session,
                           char *buf, size_t len)
{
    char *ep, *cp;
    char obuf[100];
    size_t olen = len;

    if (session->context->readonly) {
        return 0;
    }
    if ((NULL == buf) ||
        0 == len ||
        (sizeof(session->msgbuf) / 2) < len) {
        // could over run, do not chance it
        return -1;
    }
    session->msgbuf[0] = '\x10';
    ep = session->msgbuf + 1;
    for (cp = buf; olen-- > 0; cp++) {
        if ('\x10' == *cp) {
            *ep++ = '\x10';
        }
        *ep++ = *cp;
    }
    *ep++ = '\x10';
    *ep++ = '\x03';
    session->msgbuflen = (size_t)(ep - session->msgbuf);
    // Don't bore the user with the header (DLE) or trailer (DLE, STX).
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP: tsip_write1(0x%s)\n",
             gps_hexdump(obuf, sizeof(obuf),
                         (unsigned char *)&session->msgbuf[1], len));
    if (gpsd_write(session, session->msgbuf, session->msgbuflen) !=
        (ssize_t) session->msgbuflen)
        return -1;

    return 0;
}

/* tsip_detect()
 *
 * see if it looks like a TSIP device (speaking 9600O81) is listening and
 * return 1 if found, 0 if not
 */
static bool tsip_detect(struct gps_device_t *session)
{
    bool ret = false;
    int myfd;
    speed_t old_baudrate;
    char old_parity;
    unsigned int old_stopbits;
    bool override = true;

    if ((speed_t)0 == session->context->fixed_port_speed &&
        '\0' == session->context->fixed_port_framing[0]) {
        // Only try 9600 8O1 is no speed or framing override
        old_baudrate = session->gpsdata.dev.baudrate;
        old_parity = session->gpsdata.dev.parity;
        old_stopbits = session->gpsdata.dev.stopbits;
        gpsd_set_speed(session, 9600, 'O', 1);
        override = false;
    }

    /* request firmware revision and look for a valid response
     * send 0x1f, expext 0x45.  TSIPv1 does not have this, but it
     * will respond with a TSIPv1 error message, so all good. */
    if (0 == tsip_write1(session, "\x1f", 1)) {
        unsigned int n;
        struct timespec to;

        myfd = session->gpsdata.gps_fd;

        // FIXME: this holds the main loop from running...
        for (n = 0; n < 3; n++) {
            // wait 100 milli second
            to.tv_sec = 0;
            to.tv_nsec = 100000000;
            if (!nanowait(myfd, &to)) {
                break;
            }
            if (0 <= packet_get1(session)) {
                if (TSIP_PACKET == session->lexer.type) {
                    GPSD_LOG(LOG_RAW, &session->context->errout,
                             "TSIP: tsip_detect found\n");
                    ret = true;
                    break;
                }
            }
        }
    }

    if (!ret &&
        !override) {
        // return serial port to original settings
        gpsd_set_speed(session, old_baudrate, old_parity, old_stopbits);
    }

    return ret;
}

// configure generic Trimble TSIP device to a known state
static void configuration_packets_generic(struct gps_device_t *session)
{
        char buf[100];

        GPSD_LOG(LOG_PROG, &session->context->errout,
                 "TSIP: configuration_packets_generic()\n");

        // Set basic configuration, using Set or Request I/O Options (0x35).
        // Position: enable: Double Precision, LLA, disable: ECEF
        buf[0] = 0x35;
        // Time: enable: 0x42, 0x43, 0x4a, disable: 0x83, 0x84, 0x56
        buf[1] = IO1_8F20|IO1_DP|IO1_LLA;
        // Velocity: enable: ENU, disable ECEF
        buf[2] = IO2_ENU;
        buf[3] = 0x00;
        buf[4] = IO4_DBHZ;    // Aux: enable: 0x5A, dBHz
        (void)tsip_write1(session, buf, 5);

        // Request Software Version (0x1f), returns 0x45
        (void)tsip_write1(session, "\x1f", 1);

        // Current Time Request (0x21), returns 0x41
        (void)tsip_write1(session, "\x21", 1);

        /* Set Operating Parameters (0x2c)
         * not present in:
         *   Lassen SQ (2002)
         *   Lassen iQ (2005)
         *   RES SMT 360 */
        /* dynamics code: enabled: 1=land
         *   disabled: 2=sea, 3=air, 4=static
         *   default is land */
        buf[0] = 0x2c;
        buf[1] = 0x01;
        // elevation mask, 10 degrees is a common default, TSIP default is 15
        putbef32(buf, 2, (float)10.0 * DEG_2_RAD);
        // signal level mask, default is 2.0 AMU. 5.0 to 6.0 for high accuracy
        putbef32(buf, 6, (float)06.0);
        // PDOP mask default is 12. 5.0 to 6.0 for high accuracy
        putbef32(buf, 10, (float)8.0);
        // PDOP switch, default is 8.0
        putbef32(buf, 14, (float)6.0);
        (void)tsip_write1(session, buf, 18);

        /* Set Position Fix Mode (0x22)
         * 0=auto 2D/3D, 1=time only, 3=2D, 4=3D, 10=Overdetermined clock */
        (void)tsip_write1(session, "\x22\x00", 2);

        /* Request GPS System Message (0x48)
         * not supported on model RES SMT 360 */
        (void)tsip_write1(session, "\x28", 1);

        /* Last Position and Velocity Request (0x37)
         * returns 0x57 and (0x42, 0x4a, 0x83, or 0x84) and (0x43 or 0x56)  */
        (void)tsip_write1(session, "\x37", 1);

        // 0x8e-15 request output datum
        (void)tsip_write1(session, "\x8e\x15", 2);

        /* Primary Receiver Configuration Parameters Request (0xbb-00)
         * returns  Primary Receiver Configuration Block (0xbb-00) */
        (void)tsip_write1(session, "\xbb\x00", 2);
}

// configure Acutime Gold to a known state
static void configuration_packets_acutime_gold(struct gps_device_t *session)
{
        char buf[100];

        GPSD_LOG(LOG_PROG, &session->context->errout,
                 "TSIP: configuration_packets_acutime_gold()\n");

        /* Request Firmware Version (0x1c-01)
         * returns Firmware component version information (0x1x-81) */
        (void)tsip_write1(session, "\x1c\x01", 2);

        buf[0] = 0x8e;          // Set Self-Survey Parameters (0x8e-a9)
        buf[1] = 0xa9;          // Subcode
        buf[2] = 0x01;          // Self-Survey Enable = enable
        buf[3] = 0x01;          // Position Save Flag = save position
        putbe32(buf, 4, 2000);  // Self-Survey Length = 2000 fixes, default 2000
        // Horizontal Uncertainty, 1-100, 1=best, 100=worst, default 100
        putbef32(buf, 8, 100);
        // Verical Uncertainty, 1-100, 1=best, 100=worst, default 100
        putbef32(buf, 12, 100);
        (void)tsip_write1(session, buf, 16);

        /* Set PPS Output Option (0x8e-4e)
         * 0x4e Subcode
         * 2 == PPS driver switch (PPS is always output) */
        (void)tsip_write1(session, "\x8e\x4e\x02", 3);

        buf[0] = 0xbb;  // Set Primary Receiver Configuration (0xbb-00)
        buf[1] = 0x00;  // 00 =  Subcode
        buf[2] = 0x07;  // Receiver mode, 7 = Force Overdetermined clock
        buf[3] = 0xff;  // Not enabled = unchanged, must be 0xff on RES SMT 360
        buf[4] = 0x01;  // Dynamics code = default must be 0xff on RES SMT 360
        buf[5] = 0x01;  // Solution Mode = default must be 0xff on RES SMT 360
        // Elevation Mask = 10 deg
        putbef32((char *)buf, 6, (float)10.0 * DEG_2_RAD);
        // AMU Mask. 0 to 55. default is 4.0
        putbef32((char *)buf, 10, (float)4.0);
        // PDOP Mask = 8.0, default = 6
        putbef32((char *)buf, 14, (float)8.0);
        // PDOP Switch = 6.0, ignored in RES SMT 360
        putbef32((char *)buf, 18, (float)6.0);
        buf[22] = 0xff;  // must be 0xff
        buf[23] = 0x0;   // Anti-Jam Mode, 0=Off, 1=On
        putbe16(buf, 24, 0xffff);  // Reserved.  Must be 0xffff
        /* Measurement Rate and Position Fix Rate = default
         * must be 0xffff on res smt 360 */
        putbe16(buf, 26, 0x0000);
        /* 27 is Constellation on RES SMT 360.
         * 1 = GPS, 2=GLONASS, 8=BeiDou, 0x10=Galileo, 5=QZSS */
        putbe32(buf, 28, 0xffffffff);   // Reserved
        putbe32(buf, 32, 0xffffffff);   // Reserved
        putbe32(buf, 36, 0xffffffff);   // Reserved
        putbe32(buf, 40, 0xffffffff);   // Reserved
        (void)tsip_write1(session, buf, 44);

        buf[0] = 0x8e;   // Set Packet Broadcast Mask (0x8e-a5)
        buf[1] = 0xa5;   // Subcode a5
        /* Packets bit field = default + Primary timing,
         *  Supplemental timing 32e1
         *  1=0x8f-ab, 4=0x8f-ac, 0x40=Automatic Output Packets */
        putbe16(buf, 2, 0x32e1);
        buf[4] = 0x00;   // not used
        buf[5] = 0x00;   // not used
        (void)tsip_write1(session, buf, 6);
}

// configure RES 360, Resolution SMTx, and similar to a known state
static void configuration_packets_res360(struct gps_device_t *session)
{
    char buf[100];

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP: configuration_packets_res360()\n");

    // should already have versions 0x8f-81 and 0x8f-83.

    // Request Self Survey Parameters (0x8e-a9)
    (void)tsip_write1(session, "\x8e\xa9", 2);

    if (session->context->passive) {
        // request I/O Options (0x55)
        (void)tsip_write1(session, "\x35", 1);

        // request Receiver Configuration (0xbb)
        (void)tsip_write1(session, "\xbb\x00", 2);

        // Request Packet Broadcast Mask (0x8e-a5)
        (void)tsip_write1(session, "\x8e\xa5", 2);

    } else {
        // PPS Output Option (0x8e-4e) is default on

        buf[0] = 0x8e;  // Set Packet Broadcast Mask (0x8e-a5)
        buf[1] = 0xa5;  // a5 = Subcode
        /* Packets bit field = default + Auto output packets
         *  1=0x8f-ab, 4=0x8f-ac, 0x40=Automatic Output Packets */
        buf[2] = 0;        // reserved
        buf[3] = 0x45;
        buf[4] = 0;        // reserved
        buf[5] = 0;        // reserved
        (void)tsip_write1(session, buf, 6);

        /* IO Options defaults:
         *   Lassen iQ:       02 02 00 00
         *   RES SMT 360:     12 02 00 08
         *   Resolution SMTx: 12 02 00 08
         */
        buf[0] = 0x35;  // set I/O Options
        // position and velocity only sent during self-survey.
        // Position
        buf[1] =  IO1_DP|IO1_LLA|IO1_ECEF;
        // Velocity
        buf[2] = IO2_VECEF|IO2_ENU;
        // Timing
        buf[3] = 0x01;          // Use 0x8e-a2
        // Auxiliary
        buf[4] = 0x08;         // Packet 0x5a off, dBHz
        (void)tsip_write1(session, buf, 5);

#ifdef __UNUSED__
// #if 1
        // Restart Self-Survey (0x8e-a6)
        // which gives us 2,000 normal fixes, before going quiet again.
        (void)tsip_write1(session, "\x8e\xa6\x00", 3);
#endif // __UNUSED__
    }
}

/* send the next TSIPv1 query
 * Return: void
 */
static void tsipv1_query(struct gps_device_t *session)
{
    char snd_buf[24];         // send buffer

    // advance to next queue item.
    session->queue++;
    // allow it to repeat every x1000 packets
    session->queue &= 0x0ffff;

    if (0 != (session->queue % 4)) {
        // once every 4 messages
        return;
    }
    switch (session->queue / 4) {
    case 1:
        // x90-00, query protocol version
        snd_buf[0] = 0x90;             // id
        snd_buf[1] = 0x00;             // sub id
        putbe16(snd_buf, 2, 2);        // length
        snd_buf[4] = 0;                // mode: query
        snd_buf[5] = tsip1_checksum(snd_buf, 5);   // checksum
        (void)tsip_write1(session, snd_buf, 6);
        break;
    case 2:
        // x90-01, query GNSS config version
        snd_buf[0] = 0x90;             // id
        snd_buf[1] = 0x01;             // sub id
        putbe16(snd_buf, 2, 2);        // length
        snd_buf[4] = 0;                // mode: query
        snd_buf[5] = tsip1_checksum(snd_buf, 5);   // checksum
        (void)tsip_write1(session, snd_buf, 6);
        break;
    case 3:
        // x91-00, Port config
        snd_buf[0] = 0x91;             // id
        snd_buf[1] = 0x00;             // sub id
        putbe16(snd_buf, 2, 3);        // length
        snd_buf[4] = 0;                // mode: query
        snd_buf[5] = 0;                // current port
        snd_buf[6] = tsip1_checksum(snd_buf, 6);   // checksum
        (void)tsip_write1(session, snd_buf, 7);
        break;
    case 4:
        // x81-01, GNSS config
        snd_buf[0] = 0x91;             // id
        snd_buf[1] = 0x01;             // sub id
        putbe16(snd_buf, 2, 2);        // length
        snd_buf[4] = 0;                // mode: query
        snd_buf[5] = tsip1_checksum(snd_buf, 5);   // checksum
        (void)tsip_write1(session, snd_buf, 6);
        break;
    case 5:
        // x91-03, query timing config
        snd_buf[0] = 0x91;             // id
        snd_buf[1] = 0x03;             // sub id
        putbe16(snd_buf, 2, 2);        // length
        snd_buf[4] = 0;                // mode: query
        snd_buf[5] = tsip1_checksum(snd_buf, 5);   // checksum
        (void)tsip_write1(session, snd_buf, 6);
        break;
    case 6:
        // x91-04, self survey config
        snd_buf[0] = 0x91;             // id
        snd_buf[1] = 0x04;             // sub id
        putbe16(snd_buf, 2, 2);        // length
        snd_buf[4] = 0;                // mode: query
        snd_buf[5] = tsip1_checksum(snd_buf, 5);   // checksum
        (void)tsip_write1(session, snd_buf, 6);
        break;
    case 7:
        if (session->context->passive) {
            // x91-05, query current periodic messages
            snd_buf[0] = 0x91;             // id
            snd_buf[1] = 0x05;             // sub id
            putbe16(snd_buf, 2, 3);        // length
            snd_buf[4] = 0;                // mode: query
            snd_buf[5] = 0xff;             // port: current port
            snd_buf[6] = tsip1_checksum(snd_buf, 6);   // checksum
            (void)tsip_write1(session, snd_buf, 7);
        } else {
            /* request periodic  messages, x91-05
             * little harm at 115.2 kbps, this also responses as a query */
            snd_buf[0] = 0x91;             // id
            snd_buf[1] = 0x05;             // sub id
            putbe16(snd_buf, 2, 19);       // length
            snd_buf[4] = 0x01;             // mode: set
            snd_buf[5] = 0xff;             // port: current port
            // 0xaaaaa, everything preiodic
            putbe32(snd_buf, 6, 0xaaaaa);
            putbe32(snd_buf, 10, 0);       // reserved
            putbe32(snd_buf, 14, 0);       // reserved
            putbe32(snd_buf, 18, 0);       // reserved
            snd_buf[22] = tsip1_checksum(snd_buf, 22);   // checksum
            (void)tsip_write1(session, snd_buf, 23);
        }
        break;
    case 8:
        // x93-00, production info
        snd_buf[0] = 0x93;             // id
        snd_buf[1] = 0x00;             // sub id
        putbe16(snd_buf, 2, 2);        // length
        snd_buf[4] = 0;                // mode: query
        snd_buf[5] = tsip1_checksum(snd_buf, 5);   // checksum
        (void)tsip_write1(session, snd_buf, 6);
        break;
    default:
        // nothing to do
        break;
    }
}

/* tsipv1_svtype()
 * convert TSIPv1 SV Type to satellite_t.gnssid and satellite_t.sigid
 * PRN is already GNSS specific (1-99)
 * return gnssid directly, sigid indirectly through pointer
 *
 * Return: gnssid
 *         0xff on error
 */
static unsigned char tsipv1_svtype(unsigned svtype, unsigned char *sigid)
{

    unsigned char gnssid;

    switch (svtype) {
    case 1:  // GPS L1C
       gnssid =  GNSSID_GPS;
       *sigid = 0;
       break;
    case 2:  // GPS L2.  CL or CM?
       gnssid =  GNSSID_GPS;
       *sigid = 3;         // or, maybe 4
       break;
    case 3:  // GPS L5.  I or Q?
       gnssid =  GNSSID_GPS;
       *sigid = 6;         // or maybe 7
       break;
    case 5:  // GLONASS G1
       gnssid =  GNSSID_GLO;
       *sigid = 0;
       break;
    case 6:  // GLONASS G2
       gnssid =  GNSSID_GLO;
       *sigid = 2;
       break;
    case 9:  // SBAS, assume L1
       gnssid =  GNSSID_SBAS;
       *sigid = 0;
       break;
    case 13:  // Beidou B1, D1 or D2?
       gnssid =  GNSSID_BD;
       *sigid = 0;   // or maybe 1
       break;
    case 14:  // Beidou B2i
       gnssid =  GNSSID_BD;
       *sigid = 2;
       break;
    case 15:  // Beidou B2a
       gnssid =  GNSSID_BD;
       *sigid = 3;
       break;
    case 17:  // Galileo E1, C or B?
       gnssid =  GNSSID_GAL;
       *sigid = 0;    // or maybe 1
       break;
    case 18:  // Galileo E5a, aI or aQ?
       gnssid =  GNSSID_GAL;
       *sigid = 3;    // or maybe 4?
       break;
    case 19:  // Galileo E5b, bI or bQ?
       gnssid =  GNSSID_GAL;
       *sigid = 5;    // or maybe 6
       break;
    case 20:  // Galileo E6
       gnssid =  GNSSID_GAL;
       *sigid = 8;     // no idea
       break;
    case 22:  // QZSS L1
       gnssid =  GNSSID_QZSS;
       *sigid = 0;
       break;
    case 23:  // QZSS L2C
       gnssid =  GNSSID_QZSS;
       *sigid = 4;    // or maybe 5
       break;
    case 24:  // QZSS L5
       gnssid =  GNSSID_QZSS;
       *sigid = 8;     // no idea
       break;
    case 26:  // IRNSS L5
       gnssid =  GNSSID_IRNSS;
       *sigid = 8;     // no idea
       break;
    case 4:  // Reserved
        FALLTHROUGH
    case 7:  // Reserved
        FALLTHROUGH
    case 8:  // Reserved
        FALLTHROUGH
    case 10:  // Reservced
        FALLTHROUGH
    case 11:  // Reservced
        FALLTHROUGH
    case 12:  // Reservced
        FALLTHROUGH
    case 16:  // Reserved
        FALLTHROUGH
    case 21:  // Reserved
        FALLTHROUGH
    case 25:  // Reserved
        FALLTHROUGH
    default:
        *sigid = 0xff;
        return 0xff;
    }
    return gnssid;
}

// decode Packet x13
static gps_mask_t decode_x13(struct gps_device_t *session, const char *buf,
                             int len)
{
    gps_mask_t mask = 0;
    unsigned u1 = getub(buf, 0);         // Packet ID of non-parsable packet
    unsigned u2 = 0;

    if (2 <= len) {
        u2 = getub(buf, 1);     // Data byte 0 of non-parsable packet
    }
    GPSD_LOG(LOG_WARN, &session->context->errout,
             "TSIP x13: Report Packet: request x%02x %02x "
             "cannot be parsed\n",
             u1, u2);
    // ignore the rest of the bad data
    if (0x8e == (int)u1 &&
        0x23 == (int)u2) {
        // no Compact Super Packet 0x8e-23
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIP x8e-23: not available, use LFwEI (0x8f-20)\n");

        /* Request LFwEI Super Packet instead
         * SMT 360 does not support 0x8e-20 either */
        (void)tsip_write1(session, "\x8e\x20\x01", 3);
    }
    return mask;
}
// decode Superpacket x1c-81
static gps_mask_t decode_x1c_81(struct gps_device_t *session, const char *buf,
                                int len)
{
    gps_mask_t mask = 0;
    char buf2[BUFSIZ];

    // byte 1, reserved
    unsigned maj = getub(buf, 2);        // Major version
    unsigned min = getub(buf, 3);        // Minor version
    unsigned bnum = getub(buf, 4);       // Build number
    unsigned bmon = getub(buf, 5);       // Build Month
    unsigned bday = getub(buf, 6);       // Build Day
    unsigned byr = getbeu16(buf, 7);     // Build Year
    unsigned plen = getub(buf, 9);       // Length of product name

    // check for valid module name length
    if (40 < plen) {
        plen = 40;
    }
    // check for valid module name length, again
    if ((unsigned)(len - 10) < plen) {
        plen = len - 10;
    }
    // Product name in ASCII
    memcpy(buf2, &buf[10], plen);
    buf2[plen] = '\0';

    (void)snprintf(session->subtype, sizeof(session->subtype),
                   "fw %u.%u %u %02u/%02u/%04u %.40s",
                   min, maj, bnum, bmon, bday, byr, buf2);
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x1c-81: Firmware version: %s\n",
             session->subtype);

    mask |= DEVICEID_SET;
    if ('\0' == session->subtype1[0]) {
        // request actual subtype1 from 0x1c-83
        (void)tsip_write1(session, "\x1c\x03", 2);
    }
    return mask;
}

// decode Superpacket x1c-83
static gps_mask_t decode_x1c_83(struct gps_device_t *session, const char *buf,
                                int len)
{
    gps_mask_t mask = 0;
    char buf2[BUFSIZ];
    unsigned long ul1 = getbeu32(buf, 1);  // Serial number
    unsigned bday = getub(buf, 5);         // Build day
    unsigned bmon = getub(buf, 6);         // Build month
    unsigned byr = getbeu16(buf, 7);       // Build year
    unsigned u4 = getub(buf, 9);           // Build hour
    unsigned u5 = getub(buf, 12);          // Length of Hardware ID

    // Hardware Code
    session->driver.tsip.hardware_code = getbeu16(buf, 10);

    // check for valid module name length
    // copernicus ii is 27 long
    if (40 < u5) {
        u5 = 40;
    }
    // check for valid module name length, again
    if ((unsigned)(len - 13) < u5) {
        u5 = len - 13;
    }
    memcpy(buf2, &buf[13], u5);
    buf2[u5] = '\0';

    (void)snprintf(session->gpsdata.dev.sernum,
                   sizeof(session->gpsdata.dev.sernum),
                   "%lx", ul1);
    (void)snprintf(session->subtype1, sizeof(session->subtype1),
                   "hw %02u/%02u/%04u %02u %04u %.40s",
                   bmon, bday, byr, u4,
                   session->driver.tsip.hardware_code,
                   buf2);
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x1c-83: Hardware vers %s Sernum %s\n",
             session->subtype1,
             session->gpsdata.dev.sernum);

    mask |= DEVICEID_SET;
    session->driver.tsip.subtype =
        session->driver.tsip.hardware_code;

    // Detecting device by Hardware Code
    switch (session->driver.tsip.hardware_code) {
    case 3001:            // Acutime Gold
        configuration_packets_acutime_gold(session);
        break;

    // RES look-alikes
    case 3002:            // TSIP_REST
        FALLTHROUGH
    case 3009:            // TSIP_RESSMT, Model 66266
        FALLTHROUGH
    case 3017:            // Resolution SMTx,  Model 99889
        FALLTHROUGH
    case 3023:            // RES SMT 360
        FALLTHROUGH
    case 3026:            // ICM SMT 360
        FALLTHROUGH
    case 3031:            // RES360 17x22
        FALLTHROUGH
    case 3100:            // TSIP_RES720
        configuration_packets_res360(session);
        break;

    // Unknown
    default:
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIP x1c-83: Unknown hw code %x\n",
                 session->driver.tsip.hardware_code);
        FALLTHROUGH
    case 1001:            // Lassen iQ
        FALLTHROUGH
    case 1002:            // Copernicus
        FALLTHROUGH
    case 1003:            // Copernicus II
        FALLTHROUGH
    case 3007:            // Thunderbolt E
        FALLTHROUGH
    case 3032:            // Acutime 360
        configuration_packets_generic(session);
        break;
    }
    return mask;
}

// decode Superpackets x1c-XX
static gps_mask_t decode_x1c(struct gps_device_t *session, const char *buf,
                             int len, int *pbad_len)
{
    gps_mask_t mask = 0;
    int bad_len = 0;
    unsigned u1 = getub(buf, 0);

    // decode by sub-code
    switch (u1) {
    case 0x81:
        /* Firmware component version information (0x1c-81)
         * polled by 0x1c-01
         * Present in:
         *   Copernicus II (2009)
         */
        if (10 > len) {
            bad_len = 10;
            break;
        }
        mask = decode_x1c_81(session, buf, len);
        break;

    case 0x83:
        /* Hardware component version information (0x1c-83)
         * polled by 0x1c-03
         * Present in:
         *   Resolution SMTx
         * Not Present in:
         *   LassenSQ (2002)
         *   Copernicus II (2009)
         */
        if (13 > len) {
            bad_len = 13;
            break;
        }
        mask = decode_x1c_83(session, buf, len);
        break;
    default:
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIP x1c-%02x: Unhandled subpacket\n", u1);
        break;
    }
    *pbad_len = bad_len;
    // request x8f-42 Stored Production Parameters
    (void)tsip_write1(session, "\x8e\x42", 2);
    return mask;
}

/* decode GPS Time, Packet x41
 * This is "current" time, not the time of a fix */
static gps_mask_t decode_x41(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;
    char ts_buf[TIMESPEC_LEN];
    double ftow = getbef32(buf, 0);                // gpstime
    unsigned week = getbes16(buf, 4);              // week, yes, signed!
    double f2 = getbef32(buf, 6);                  // leap seconds, fractional!

    if (0.0 <= ftow &&
        10.0 < f2) {
        session->context->leap_seconds = (int)round(f2);
        session->context->valid |= LEAP_SECOND_VALID;
        DTOTS(&ts_tow, ftow);
        session->newdata.time =
            gpsd_gpstime_resolv(session, week, ts_tow);
        mask |= TIME_SET | NTPTIME_IS | CLEAR_IS;
        /* Note: this is not the time of current fix. So we do a clear
         * so the previous fix data does not get attached to this time.
         * Do not use in tsip.last_tow */
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x41: GPS Time: tow %.3f week %u ls %.1f %s\n",
             ftow, week, f2,
             timespec_str(&session->newdata.time, ts_buf, sizeof(ts_buf)));
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: %s",
             ctime(&session->newdata.time.tv_sec));
    return mask;
}

// decode Packet x42
static gps_mask_t decode_x42(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;
    double ecefx = getbef32(buf, 0);                  // X
    double ecefy = getbef32(buf, 4);                  // Y
    double ecefz = getbef32(buf, 8);                  // Z
    double ftow = getbef32(buf, 12);           // time-of-fix

    session->newdata.ecef.x = ecefx;
    session->newdata.ecef.y = ecefy;
    session->newdata.ecef.z = ecefz;
    DTOTS(&ts_tow, ftow);
    session->newdata.time = gpsd_gpstime_resolv(session,
                                                session->context->gps_week,
                                                ts_tow);
    mask = ECEF_SET | TIME_SET | NTPTIME_IS;
    if (!TS_EQ(&ts_tow, &session->driver.tsip.last_tow)) {
        mask |= CLEAR_IS;
        session->driver.tsip.last_tow = ts_tow;
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x42: SP-XYZ: %f %f %f ftow %f\n",
             session->newdata.ecef.x,
             session->newdata.ecef.y,
             session->newdata.ecef.z,
             ftow);
    return mask;
}

// Decode Protocol Version: x43
static gps_mask_t decode_x43(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;
    double vx = getbef32(buf, 0);               // X velocity, m/s
    double vy = getbef32(buf, 4);               // Y velocity, m/s
    double vz = getbef32(buf, 8);               // Z velocity, m/s
    double bias_rate = getbef32(buf, 12);       // bias rate, m/s
    double ftow = getbef32(buf, 16);            // time-of-fix

    session->newdata.ecef.vx = vx;
    session->newdata.ecef.vy = vy;
    session->newdata.ecef.vz = vz;

    // short circuit to gpsdata. Convert m/s to ns/s
    session->gpsdata.fix.clockdrift = 1e9 * bias_rate / CLIGHT;

    DTOTS(&ts_tow, ftow);
    session->newdata.time = gpsd_gpstime_resolv(session,
                                                session->context->gps_week,
                                                ts_tow);
    mask = VECEF_SET | TIME_SET | NTPTIME_IS;
    if (!TS_EQ(&ts_tow, &session->driver.tsip.last_tow)) {
        mask |= CLEAR_IS;
        session->driver.tsip.last_tow = ts_tow;
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x43: Vel XYZ: %f %f %f %f ftow %f\n",
             session->newdata.ecef.vx,
             session->newdata.ecef.vy,
             session->newdata.ecef.vz,
             bias_rate, ftow);
    return mask;
}

// Decode x45
static gps_mask_t decode_x45(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    unsigned nmaj = getub(buf, 0);
    unsigned nmin = getub(buf, 1);
    unsigned nmon = getub(buf, 2);
    unsigned nday = getub(buf, 3);
    unsigned nyr = getub(buf, 4) + 1900;
    unsigned fmaj = getub(buf, 5);
    unsigned fmin = getub(buf, 6);
    unsigned fmon = getub(buf, 7);
    unsigned fday = getub(buf, 8);
    unsigned fyr = getub(buf, 9) + 2000;

    /* ACE calls these "NAV processor firmware" and
     * "SIG processor firmware".
     * RES SMT 360 calls these "application" and "GPS core".
     */
    (void)snprintf(session->subtype, sizeof(session->subtype),
                   "sw %u.%u %02u/%02u/%04u hw %u.%u %02u/%02u/%04u",
                   nmaj, nmin, nmon, nday, nyr,
                   fmaj, fmin, fmon, fday, fyr);
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x45: Software version: %s\n", session->subtype);
    mask |= DEVICEID_SET;

    // request I/O Options (0x55)
    (void)tsip_write1(session, "\x35", 1);

    /* request actual subtype using x1c-01, returns x1c-81
     * which in turn requests 0x1c-83
     * then requests x8f-42 */
    (void)tsip_write1(session, "\x1c\x01", 2);
    return mask;
}

// Decode Health of Receiver, x46
static gps_mask_t decode_x46(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    char buf2[80];
    // Status code, see vgnss_decode_status
    unsigned status = getub(buf, 0);
    unsigned ec = getub(buf, 1);      // error codes

    switch (status) {
    case 0:         //  "Doing Fixes"
        // could be 2D or 3D.  So check the last setting.
        if (MODE_2D >= session->oldfix.mode) {
            session->newdata.mode = MODE_2D;  // At least 2D
        } else {
            session->newdata.mode = MODE_3D;
        }
        break;
    case 9:          // "1 usable sat"
        session->newdata.mode = MODE_2D;
        break;
    case 10:         // "2 usable sats"
        session->newdata.mode = MODE_2D;
        break;
    case 11:         // "3 usable sats"
        session->newdata.mode = MODE_2D;
        break;
    case 1:          // "No GPS time"
        FALLTHROUGH
    case 2:          // "Needs Init"
        FALLTHROUGH
    case 3:          // "PDOP too high"
        FALLTHROUGH
    case 8:          // "0 usable sats"
        FALLTHROUGH
    case 12:         // "chosen sat unusable"
        FALLTHROUGH
    case 16:         // "TRAIM rejected"
        session->newdata.mode = MODE_NO_FIX;
        break;
    case 0xbb:       // "GPS Time Fix (OD mode)"
        // Always on after survey, so no info here.
        break;
    }
    if (MODE_NOT_SEEN != session->newdata.mode) {
        mask |= MODE_SET;
    }

    /* Error codes, model dependent
     * 0x01 -- no battery, always set on RES SMT 360
     * 0x10 -- antenna is open
     * 0x30 -- antenna is shorted
     */
    switch (ec & 0x30) {
    case 0x10:
        session->newdata.ant_stat = ANT_OPEN;
        break;
    case 0x30:
        session->newdata.ant_stat = ANT_SHORT;
        break;
    default:
        session->newdata.ant_stat = ANT_OK;
        break;
    }

    if (STATUS_UNK != session->newdata.status) {
        mask |= STATUS_SET;
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x46: Receiver Health: mode %d status %d  gds:x%x "
             "ec:x%x\n",
            session->newdata.mode,
            session->newdata.status,
            status, ec);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: gds:%s ec:%s\n",
             val2str(status, vgnss_decode_status),
             flags2str(ec, verr_codes, buf2, sizeof(buf2)));
    return mask;
}

// Decode x47
static gps_mask_t decode_x47(struct gps_device_t *session, const char *buf,
                             int len, int *pbad_len)
{
    gps_mask_t mask = 0;
    char buf2[BUFSIZ];
    int i;

    // satellite count, RES SMT 360 doc says 12 max
    int count = getub(buf, 0);

    // Status code, see vgnss_decode_status
    gpsd_zero_satellites(&session->gpsdata);

    if ((5 * count + 1) > len) {
        *pbad_len = 5 * count + 1;
        return mask;
    }
    *pbad_len = 0;
    buf2[0] = '\0';
    for (i = 0; i < count; i++) {
        unsigned j;
        int PRN = getub(buf, 5 * i + 1);
        double snr = getbef32(buf, 5 * i + 2);

        if (0 > snr) {
            snr = 0.0;
        }
        for (j = 0; j < TSIP_CHANNELS; j++) {
            if (session->gpsdata.skyview[j].PRN == PRN) {
                session->gpsdata.skyview[j].ss = snr;
                break;
            }
        }
        str_appendf(buf2, sizeof(buf2), " %u=%.1f", PRN, snr);
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x47: Signal Levels: (%d):%s\n", count, buf2);
    mask |= SATELLITE_SET;
    return mask;
}

// Decode x4a
static gps_mask_t decode_x4a(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;
    char ts_buf[TIMESPEC_LEN];
    double lat = getbef32(buf, 0) * RAD_2_DEG;   // lat
    double lon = getbef32(buf, 4) * RAD_2_DEG;   // lon
    double alt = getbef32(buf, 8);               // alt
    double clock_bias = getbef32(buf, 12);       // clock bias, m/s
    double ftow = getbef32(buf, 16);             // time-of-fix

    session->newdata.latitude = lat;
    session->newdata.longitude = lon;
    // depending on GPS config, could be either WGS84 or MSL
    if (0 == session->driver.tsip.alt_is_msl) {
        session->newdata.altHAE = alt;
    } else {
        session->newdata.altMSL = alt;
    }
    // short circuit to gpsdata. COnvert m/s to ns
    session->gpsdata.fix.clockbias = 1e9 * clock_bias / CLIGHT;

    if (0 != (session->context->valid & GPS_TIME_VALID)) {
        DTOTS(&ts_tow, ftow);
        session->newdata.time =
            gpsd_gpstime_resolv(session, session->context->gps_week,
                                ts_tow);
        mask |= TIME_SET | NTPTIME_IS;
        if (!TS_EQ(&ts_tow, &session->driver.tsip.last_tow)) {
            mask |= CLEAR_IS;
            session->driver.tsip.last_tow = ts_tow;
        }
    }
    // this seems to be often first in cycle
    // REPORT_IS here breaks reports in read-only mode
    mask |= LATLON_SET | ALTITUDE_SET;
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x4a: SP-LLA: time=%s lat=%.2f lon=%.2f "
             "alt=%.2f cbias %.2f\n",
             timespec_str(&session->newdata.time, ts_buf, sizeof(ts_buf)),
             session->newdata.latitude,
             session->newdata.longitude, alt, clock_bias);
    return mask;
}

// Decode x4b
static gps_mask_t decode_x4b(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    char buf2[80];
    char buf3[80];
    unsigned u1 = getub(buf, 0);  // Machine ID
    /* Status 1
     * bit 1 -- No RTC at power up
     * bit 3 -- almanac not complete and current */
    unsigned u2 = getub(buf, 1);     // status 1
    unsigned u3 = getub(buf, 2);     // Status 2/Superpacket Support

    session->driver.tsip.machine_id = u1;  // Machine ID

    if ('\0' == session->subtype[0]) {
        const char *name;
        // better than nothing
        switch (session->driver.tsip.machine_id) {
        case 1:
            // should use better name from superpacket
            name = " SMT 360";
            /* request actual subtype from 0x1c-81
             * which in turn requests 0x1c-83 */
            (void)tsip_write1(session, "\x1c\x01", 2);
            break;
        case 0x32:
            name = " Acutime 360";
            break;
        case 0x5a:
            name = " Lassen iQ";
            /* request actual subtype from 0x1c-81
             * which in turn requests 0x1c-83.
             * Only later firmware Lassen iQ supports this */
            (void)tsip_write1(session, "\x1c\x01", 2);
            break;
        case 0x61:
            name = " Acutime 2000";
            break;
        case 0x62:
            name = " ACE UTC";
            break;
        case 0x96:
            // Also Copernicus II
            name = " Copernicus, Thunderbolt E";
            /* so request actual subtype from 0x1c-81
             * which in turn requests 0x1c-83 */
            (void)tsip_write1(session, "\x1c\x01", 2);
            break;
        case 0:
            // Resolution SMTx
            FALLTHROUGH
        default:
             name = "";
        }
        (void)snprintf(session->subtype, sizeof(session->subtype),
                       "Machine ID x%x(%s)",
                       session->driver.tsip.machine_id, name);
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x4b: Machine ID: %02x %02x %02x\n",
             session->driver.tsip.machine_id,
             u2, u3);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: stat1:%s stat2:%s\n",
             flags2str(u2, vstat1, buf2, sizeof(buf2)),
             flags2str(u3, vstat2, buf3, sizeof(buf3)));

    if (u3 != session->driver.tsip.superpkt) {
        session->driver.tsip.superpkt = u3;
        GPSD_LOG(LOG_PROG, &session->context->errout,
                 "TSIP: Switching to Super Packet mode %d\n", u3);
        switch (u3){
        default:
            FALLTHROUGH
        case 0:
            // old Trimble, no superpackets
            break;
        case 1:
            // 1 == superpacket is acutime 360, support 0x8f-20

            /* set I/O Options for Super Packet output
             * Position: 8F20, ECEF, DP */
            buf2[0] = 0x35;
            buf2[1] = IO1_8F20|IO1_DP|IO1_ECEF;
            buf2[2] = 0x00;          // Velocity: none (via SP)
            buf2[3] = 0x00;          // Time: GPS
            buf2[4] = IO4_DBHZ;      // Aux: dBHz
            (void)tsip_write1(session, buf2, 5);
            break;
        case 2:
            /* 2 == SMT 360, or Resolution SMTx
             * no 0x8f-20, or x8f-23.
             * request x8f-a5 */
            (void)tsip_write1(session, "\x8e\xa5", 2);
            break;
        }
    }

    return mask;
}

// Decode x4c
static gps_mask_t decode_x4c(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    unsigned u1 = getub(buf, 0);                  // Dynamics Code
    double f1 = getbef32(buf, 1) * RAD_2_DEG;     // Elevation Mask
    double f2 = getbef32(buf, 5);                 // Signal Level Mask
    double f3 = getbef32(buf, 9);                 // PDOP Mask
    double f4 = getbef32(buf, 13);                // PDOP Switch

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x4c: OP: Dyn x%02x El %f Sig %f PDOP %f %f\n",
             u1, f1, f2, f3, f4);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: Dynamics:%s\n",
             val2str(u1, vx4c_dyncode));
    return mask;
}

/* Decode  Bias and Bias Rate Report (0x54)
 * Present in:
 *   pre-2000 models
 *   Acutime 360
 *   ICM SMT 360  (undocumented)
 *   RES SMT 360  (undocumented)
 * Not Present in:
 *   Copernicus II (2009)
 *   Resolution SMTx
 */
static gps_mask_t decode_x54(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;
    double clock_bias = getbef32(buf, 0);         // clock Bias, m
    double clock_rate = getbef32(buf, 4);         // clock Bias rate, m/s
    double ftow = getbef32(buf, 8);               // tow

    DTOTS(&ts_tow, ftow);
    session->newdata.time =
        gpsd_gpstime_resolv(session, session->context->gps_week, ts_tow);
    if (!TS_EQ(&ts_tow, &session->driver.tsip.last_tow)) {
        mask |= CLEAR_IS;
        session->driver.tsip.last_tow = ts_tow;
    }
    // short circuit to gpsdata. Convert m to ns
    session->gpsdata.fix.clockbias = 1e9 * clock_bias / CLIGHT;
    session->gpsdata.fix.clockdrift = 1e9 * clock_rate / CLIGHT;

    mask |= TIME_SET | NTPTIME_IS;

    GPSD_LOG(LOG_PROG, &session->context->errout,
            "TSIP x54: BBRR: Bias %f brate %f tow %f\n",
            clock_bias, clock_rate, ftow);
    return mask;
}

// Decode Protocol Version: x55
static gps_mask_t decode_x55(struct gps_device_t *session, const char *buf,
                             time_t now)
{
    gps_mask_t mask = 0;
    char buf2[80];
    char buf3[80];
    char buf4[80];
    char buf5[80];

    unsigned u1 = getub(buf, 0);     // Position
    unsigned u2 = getub(buf, 1);     // Velocity
    /* Timing
     * bit 0 - reserved use 0x8e-a2 ?
     */
    unsigned u3 = getub(buf, 2);
    /* Aux
     * bit 0 - packet 0x5a (raw data)
     * bit 3 -- Output dbHz
     */
    unsigned u4 = getub(buf, 3);

    // decode HAE/MSL from Position byte
    if (IO1_MSL == (IO1_MSL & u1)) {
        session->driver.tsip.alt_is_msl = 1;
    } else {
        session->driver.tsip.alt_is_msl = 0;
    }

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x55: IO Options: %02x %02x %02x %02x\n",
             u1, u2, u3, u4);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: pos:%s vel:%s timing:%s aux:%s\n",
             flags2str(u1, vx55_pos, buf2, sizeof(buf2)),
             flags2str(u2, vx55_vel, buf3, sizeof(buf3)),
             flags2str(u3, vx55_timing, buf4, sizeof(buf4)),
             flags2str(u4, vx55_aux, buf5, sizeof(buf5)));
    if ((u1 & 0x20) != (uint8_t) 0) {
        /* Try to get Super Packets
         * Turn off 0x8f-20 LFwEI Super Packet */
        (void)tsip_write1(session, "\x8e\x20\x00", 3);

        // Turn on Compact Super Packet 0x8f-23
        (void)tsip_write1(session, "\x8e\x23\x01", 3);
        session->driver.tsip.req_compact = now;
    }
    return mask;
}

// Decode Velocity Fix, Easst-North-Up, packet x56
static gps_mask_t decode_x56(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;

    float f1 = getbef32(buf, 0);        // East velocity
    float f2 = getbef32(buf, 4);        // North velocity
    float f3 = getbef32(buf, 8);        // Up velocity
    float cbias = getbef32(buf, 12);    // clock bias rate, m/s
    float ftow = getbef32(buf, 16);     // time-of-fix

    // Could be GPS, or UTC...
    DTOTS(&ts_tow, ftow);
    session->newdata.time = gpsd_gpstime_resolv(session,
                                                session->context->gps_week,
                                                ts_tow);
    session->newdata.NED.velN = f2;
    session->newdata.NED.velE = f1;
    session->newdata.NED.velD = -f3;
    // short circuit to gpsdata. Convert m to ns
    session->gpsdata.fix.clockdrift = 1e9 * cbias / CLIGHT;

    mask |= VNED_SET | TIME_SET | NTPTIME_IS;
    if (!TS_EQ(&ts_tow, &session->driver.tsip.last_tow)) {
        mask |= CLEAR_IS;
        session->driver.tsip.last_tow = ts_tow;
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x56: Vel ENU: %f %f %f cbias %f ftow %f\n",
             f1, f2, f3, cbias, ftow);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: %s",
             ctime(&session->newdata.time.tv_sec));
    return mask;
}

// Decode x57
static gps_mask_t decode_x57(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;
    char buf2[80];
    unsigned u1 = getub(buf, 0);                     // Source of information
    unsigned u2 = getub(buf, 1);                     // Mfg. diagnostic
    double ftow = getbef32(buf, 2);                  // gps_time
    unsigned week = getbeu16(buf, 6);                // tsip.gps_week

    if (0x01 == u1) {
        // good current fix
        DTOTS(&ts_tow, ftow);
        (void)gpsd_gpstime_resolv(session, week, ts_tow);
        mask |= TIME_SET | NTPTIME_IS;
        if (!TS_EQ(&ts_tow, &session->driver.tsip.last_tow)) {
            mask |= CLEAR_IS;
            session->driver.tsip.last_tow = ts_tow;
        }
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x57: Fix info: %02x %02x %u %f\n",
             u1, u2, week, ftow);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: info:%s fmode:%s\n",
             flags2str(u1, vx57_info, buf2, sizeof(buf2)),
             val2str(u1, vx57_fmode));
    return mask;
}

// Decode x5a
static gps_mask_t decode_x5a(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    // Useless without the pseudorange...
    unsigned u1 = getub(buf, 0);             // PRN 1-237
    float f1 = getbef32(buf, 1);             // sample length
    float f2 = getbef32(buf, 5);             // Signal Level, dbHz
    float f3 = getbef32(buf, 9);             // Code phase, 1/16th chip
    float f4 = getbef32(buf, 13);            // Doppler, Hz @ L1
    double d1 = getbed64(buf, 17);           // Time of Measurement

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x5a: Raw Measurement Data: PRN %d len %f SNR %f chip %f "
             "doppler %f tom %f\n",
             u1, f1, f2, f3, f4, d1);
    return mask;
}

// Decode Satellite Tracking Status, packet x5c
static gps_mask_t decode_x5c(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;
    int i;
    // Useless without the pseudorange...
    int PRN = getub(buf, 0);                     // PRN 1-32
    // slot unused in Lassen IQ
    unsigned u2 = getub(buf, 1);                 // slot:chan
    unsigned acq = getub(buf, 2);                // Acquisition flag
    unsigned eflag = getub(buf, 3);              // Ephemeris flag
    double snr = getbef32(buf, 4);               // Signal level
    // time of skyview, not current time, nor time of fix
    double ftow = getbef32(buf, 8);

    double el = getbef32(buf, 12) * RAD_2_DEG;   // Elevation
    double az = getbef32(buf, 16) * RAD_2_DEG;   // Azimuth

    // Old Meassurement flag, unused in Lassen IQ
    unsigned omf = getub(buf, 20);

    DTOTS(&session->gpsdata.skyview_time, ftow);
    /* Channel number, bits 0-2 reserved/unused as of 1999.
     * Seems to always start series at zero and increment to last one.
     * No way to know how many there will be.
     * Save current channel to check for last 0x5c message
     */
    i = (int)(u2 >> 3);     // channel number, starting at 0
    if (0 == i) {
        // start of new cycle, save last count
        session->gpsdata.satellites_visible =
            session->driver.tsip.last_chan_seen;
    }
    session->driver.tsip.last_chan_seen = i;

    if (i < TSIP_CHANNELS) {
        session->gpsdata.skyview[i].PRN = PRN;
        session->gpsdata.skyview[i].svid = PRN;
        session->gpsdata.skyview[i].gnssid = GNSSID_GPS;
        session->gpsdata.skyview[i].ss = snr;
        session->gpsdata.skyview[i].elevation = el;
        session->gpsdata.skyview[i].azimuth = az;
        session->gpsdata.skyview[i].gnssid = tsip_gnssid(0, PRN,
            &session->gpsdata.skyview[i].svid);
        if (2 == (2 & eflag)) {
            session->gpsdata.skyview[i].health = SAT_HEALTH_OK;
        } else if (1 == eflag) {
            session->gpsdata.skyview[i].health = SAT_HEALTH_BAD;
        } // else, unknown

        if (0x10 == (0x10 & eflag)) {
            session->gpsdata.skyview[i].used = true;
            if (51 == eflag) {
                session->newdata.status = STATUS_DGPS;
                mask |= STATUS_SET;
            }
        } else {
            session->gpsdata.skyview[i].used = false;
        }
        /* when polled by 0x3c, all the skyview times will be the same
         * in one cluster */
        if (0.0 < ftow) {
            DTOTS(&ts_tow, ftow);
            session->gpsdata.skyview_time =
                gpsd_gpstime_resolv(session, session->context->gps_week,
                                    ts_tow);
            /* do not save in session->driver.tsip.last_tow
             * as this is skyview time, not fix time */
        }
        if ((i + 1) >= session->gpsdata.satellites_visible) {
            /* Last of the series?
             * This will cause extra SKY if this set has more
             * sats than the last set */
            mask |= SATELLITE_SET;
            session->gpsdata.satellites_visible = i + 1;
        }
        /* If this series has fewer than last series there will
         * be no SKY, unless the cycle ender pushes the SKY */
        GPSD_LOG(LOG_PROG, &session->context->errout,
                 "TSIP x5c: Satellite Tracking Status: Ch %2d PRN %3d "
                 "es %d Acq %d Eph %2d SNR %4.1f LMT %.04f El %.1f Az %.1f "
                 "omf %u hlth %u\n",
                 i, PRN, u2 & 7, acq, eflag, snr, ftow, el, az, omf,
                session->gpsdata.skyview[i].health);
        GPSD_LOG(LOG_IO, &session->context->errout,
                 "TSIP: acq:%s eflag:%s\n",
                 val2str(acq, vx5c_acq),
                 val2str(eflag, vx5c_eflag));
    } else {
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIP x5c: Satellite Tracking Status: Too many chans %d\n", i);
    }
    return mask;
}

// Decode x5d
static gps_mask_t decode_x5d(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;
    char buf2[BUFSIZ];
    unsigned char u1 = getub(buf, 0);           // PRN
    int i = getub(buf, 1);                      // chan
    unsigned u3 = getub(buf, 2);                // Acquisition flag
    unsigned u4 = getub(buf, 3);                // used?
    double f1 = getbef32(buf, 4);               // SNR
    // This can be one second behind the TPV on RES SMT 360
    double ftow = getbef32(buf, 8);             // time of Last measurement
    double d1 = getbef32(buf, 12) * RAD_2_DEG;  // Elevation
    double d2 = getbef32(buf, 16) * RAD_2_DEG;  // Azimuth
    unsigned u5 = getub(buf, 20);               // old measurement flag
    unsigned u6 = getub(buf, 21);               // integer msec flag
    unsigned u7 = getub(buf, 22);               // bad data flag
    unsigned u8 = getub(buf, 23);               // data collection flag
    unsigned u9 = getub(buf, 24);               // Used flags
    unsigned u10 = getub(buf, 25);              // SV Type

    /* Channel number, bits 0-2 reserved/unused as of 1999.
     * Seems to always start series at zero and increment to last one.
     * No way to know how many there will be.
     * Save current channel to check for last 0x5d message
     */
    if (0 == i) {
        // start of new cycle, save last count
        session->gpsdata.satellites_visible =
            session->driver.tsip.last_chan_seen;
    }
    session->driver.tsip.last_chan_seen = i;

    if (TSIP_CHANNELS > i) {
        session->gpsdata.skyview[i].PRN = u1;
        session->gpsdata.skyview[i].ss = f1;
        session->gpsdata.skyview[i].elevation = d1;
        session->gpsdata.skyview[i].azimuth = d2;
        session->gpsdata.skyview[i].used = (bool)u4;
        session->gpsdata.skyview[i].gnssid = tsip_gnssid(u10, u1,
            &session->gpsdata.skyview[i].svid);
        if (0 == u7) {
            session->gpsdata.skyview[i].health = SAT_HEALTH_OK;
        } else {
            session->gpsdata.skyview[i].health = SAT_HEALTH_BAD;
        }

        /* when polled by 0x3c, all the skyview times will be the same
         * in one cluster */
        if (0.0 < ftow) {
            DTOTS(&ts_tow, ftow);
            session->gpsdata.skyview_time =
                gpsd_gpstime_resolv(session, session->context->gps_week,
                                    ts_tow);
            /* do not save in session->driver.tsip.last_tow
             * as this is skyview time, not fix time */
        }
        if (++i >= session->gpsdata.satellites_visible) {
            /* Last of the series?
             * This will cause extra SKY if this set has more
             * sats than the last set */
            mask |= SATELLITE_SET;
            session->gpsdata.satellites_visible = i;
        }
        /* If this series has fewer than last series there will
         * be no SKY, unless the cycle ender pushes the SKY */
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
            "TSIP x5d: Satellite Tracking Status: Ch %2d Con %d PRN %3d "
            "Acq %d Use %d SNR %4.1f LMT %.04f El %4.1f Az %5.1f Old %d "
            "Int %d Bad %d Col %d TPF %d SVT %d\n",
            i, u10, u1, u3, u4, f1, ftow, d1, d2, u5, u6, u7, u8, u9, u10);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: bad:%s uflags:%s scons:%s\n",
             val2str(u7, vsv_bad),
             flags2str(u9, vsv_used_flags, buf2, sizeof(buf2)),
             val2str(u10, vsv_type));
    return mask;
}

// Decode x6c
static gps_mask_t decode_x6c(struct gps_device_t *session, const char *buf,
                             int len, int *pbad_len)
{
    gps_mask_t mask = 0;
    char buf2[80];
    int i, count;
    unsigned fixdm = getub(buf, 0);          // fix dimension, mode
    double pdop = getbef32(buf, 1);
    double hdop = getbef32(buf, 5);
    double vdop = getbef32(buf, 9);
    // RES SMT 360 and ICM SMT 360 always report tdop == 1
    double tdop = getbef32(buf, 13);

    if (IN(0.01, pdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.pdop = pdop;
        mask |= DOP_SET;
    }
    if (IN(0.01, hdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.hdop = hdop;
        mask |= DOP_SET;
    }
    if (IN(0.01, vdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.vdop = vdop;
        mask |= DOP_SET;
    }
    if (IN(0.01, tdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.tdop = tdop;
        mask |= DOP_SET;
    }

    count = getub(buf, 17);

    if ((18 + count) > len) {
        *pbad_len = 18 + count;
        return mask;
    }
    *pbad_len = 0;

    /*
     * This looks right, but it sets a spurious mode value when
     * the satellite constellation looks good to the chip but no
     * actual fix has yet been acquired.  We should set the mode
     * field (which controls gpsd's fix reporting) only from sentences
     * that convey actual fix information, like 0x8f-20, but some
     * TSIP do not support 0x8f-20, and 0x6c may be all we got.
     */
    switch (fixdm & 7) {       // dimension
    case 1:       // clock fix (surveyed in)
        FALLTHROUGH
    case 5:       // Overdetermined clock fix
        session->newdata.status = STATUS_TIME;
        session->newdata.mode = MODE_3D;
        break;
    case 3:
        session->newdata.mode = MODE_2D;
        break;
    case 4:
        session->newdata.mode = MODE_3D;
        break;
    case 6:
        // Accutime
        session->newdata.status = STATUS_DGPS;
        session->newdata.mode = MODE_3D;
        break;
    case 0:
        // Sometimes this is No Fix, sometimes Auto....
        FALLTHROUGH
    case 2:
        FALLTHROUGH
    case 7:
        FALLTHROUGH
    default:
        session->newdata.mode = MODE_NO_FIX;
        break;
    }
    if (8 == (fixdm & 8)) {      // fix mode
        // Manual (Surveyed in)
        if (count) {
            session->newdata.status = STATUS_TIME;
        } else {
            // no saats, must be DR
            session->newdata.status = STATUS_DR;
        }
    }
    if (STATUS_UNK < session->newdata.status) {
        mask |= STATUS_SET;
    }
    mask |= MODE_SET;

    session->gpsdata.satellites_used = count;

    memset(session->driver.tsip.sats_used, 0,
            sizeof(session->driver.tsip.sats_used));
    buf2[0] = '\0';
    for (i = 0; i < count; i++) {
        // negative PRN means sat unhealthy why use an unhealthy sat??
        session->driver.tsip.sats_used[i] = getsb(buf, 18 + i);
        if (LOG_PROG <= session->context->errout.debug) {
            str_appendf(buf2, sizeof(buf2),
                           " %d", session->driver.tsip.sats_used[i]);
        }
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x6c: AIVSS: mode %d status %d used %d "
             "pdop %.1f hdop %.1f vdop %.1f tdop %.1f Used %s fixdm x%x\n",
             session->newdata.mode,
             session->newdata.status,
             session->gpsdata.satellites_used,
             pdop, hdop, vdop, tdop, buf2, fixdm);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: fixd:%s\n",
             flags2str(fixdm, vfix, buf2, sizeof(buf2)));
    mask |= USED_IS;
    return mask;
}

// decode All-in-view Satellite Selection, x6d
static gps_mask_t decode_x6d(struct gps_device_t *session, const char *buf,
                             int len, int *pbad_len)
{
    gps_mask_t mask = 0;
    int i;
    char buf2[BUFSIZ];

    unsigned fix_dim = getub(buf, 0);     // nsvs/dimension
    int count = (int)((fix_dim >> 4) & 0x0f);
    double pdop = getbef32(buf, 1);
    double hdop = getbef32(buf, 5);
    double vdop = getbef32(buf, 9);
    double tdop = getbef32(buf, 13);

    if ((17 + count) > len) {
        *pbad_len = 17 + count;
        return 0;
    }
    *pbad_len = 0;

    session->gpsdata.satellites_used = count;
    if (IN(0.01, pdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.pdop = pdop;
        mask |= DOP_SET;
    }
    if (IN(0.01, hdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.hdop = hdop;
        mask |= DOP_SET;
    }
    if (IN(0.01, vdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.vdop = vdop;
        mask |= DOP_SET;
    }
    if (IN(0.01, tdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.tdop = tdop;
        mask |= DOP_SET;
    }

    /*
     * This looks right, but it sets a spurious mode value when
     * the satellite constellation looks good to the chip but no
     * actual fix has yet been acquired.  We should set the mode
     * field (which controls gpsd's fix reporting) only from sentences
     * that convey actual fix information, like 0x8f-20, but some
     * TSIP do not support 0x8f-20, and 0x6c may be all we got.
     */
    switch (fix_dim & 7) {   // dimension
    case 1:
        // clock fix (surveyed in), not in Lassen IQ
        FALLTHROUGH
    case 5:       // Overdetermined clock fix, not in Lassen IQ
        session->newdata.status = STATUS_TIME;
        session->newdata.mode = MODE_3D;
        break;
    case 3:
        // Copernicus ii can output this for OD mode.
        session->newdata.mode = MODE_2D;
        break;
    case 4:
        // SMTx can output this for OD mode.
        session->newdata.mode = MODE_3D;
        break;
    case 6:
        // Accutime, not in Lassen IQ
        session->newdata.status = STATUS_DGPS;
        session->newdata.mode = MODE_3D;
        break;
    case 2:              // not in Lassen IQ
        FALLTHROUGH
    case 7:              // not in Lassen IQ
        FALLTHROUGH
    default:             // huh?
        session->newdata.mode = MODE_NO_FIX;
        break;
    }
    if (0 >= count &&
        0 != isfinite(session->oldfix.longitude)) {
        // use oldfix, as this may be the 1st message in an epoch.

        // reports a fix even ith no sats!
        session->newdata.status = STATUS_DR;
    }
    if (STATUS_UNK < session->newdata.status) {
        mask |= STATUS_SET;
    }
    mask |= MODE_SET;

    memset(session->driver.tsip.sats_used, 0,
           sizeof(session->driver.tsip.sats_used));
    buf2[0] = '\0';
    for (i = 0; i < count; i++) {
        // negative PRN means sat unhealthy why use an unhealthy sat??

        session->driver.tsip.sats_used[i] = getsb(buf, 17 + i);
        if (LOG_PROG <= session->context->errout.debug ) {
            str_appendf(buf2, sizeof(buf2),
                           " %u", session->driver.tsip.sats_used[i]);
        }
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x6d: AIVSS: fix_dim=x%x status=%d mode=%d used=%d "
             "pdop=%.1f hdop=%.1f vdop=%.1f tdop=%.1f used >%s<\n",
             fix_dim,
             session->newdata.status,
             session->newdata.mode,
             session->gpsdata.satellites_used,
             pdop, hdop, vdop, tdop, buf2);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: fix::%s\n",
             flags2str(fix_dim, vfix, buf2, sizeof(buf2)));
    mask |= USED_IS;

    return mask;
}

// decode packet x82
static gps_mask_t decode_x82(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    // differential position fix mode
    unsigned mode = getub(buf, 0);
    if (1 == (mode & 1)) {
        /* mode 1 (manual DGPS), output fixes only w/ SGPS,
         * or
         * mode 3 (auto DGPS) and have DGPS */
        session->newdata.status = STATUS_DGPS;
        mask |= STATUS_SET;
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x82: DPFM: mode %d status=%d\n",
             mode, session->newdata.status);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: mode:%s\n",
             val2str(mode, vx82_mode));
    return mask;
}

// decode packet x83
static gps_mask_t decode_x83(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;
    // differential position fix mode
    double ecefx = getbed64(buf, 0);            // X, m
    double ecefy = getbed64(buf, 8);            // Y, m
    double ecefz = getbed64(buf, 16);           // Z, m
    double clock_bias = getbed64(buf, 24);      // clock bias, m
    double ftow = getbef32(buf, 32);            // time-of-fix, s

    session->newdata.ecef.x = ecefx;
    session->newdata.ecef.y = ecefy;
    session->newdata.ecef.z = ecefz;
    // short circuit to gpsdata. Convert m to ns
    session->gpsdata.fix.clockbias = 1e9 * clock_bias / CLIGHT;

    DTOTS(&ts_tow, ftow);
    session->newdata.time = gpsd_gpstime_resolv(session,
                                                session->context->gps_week,
                                                ts_tow);
    /* No fix mode info!! That comes later in 0x6d.
     * This message only sent when there is 2D or 3D fix.
     * This is a problem as gpsd will send a report with no mode.
     * Steal mode from last fix.
     * The last fix is likely lastfix, not oldfix, as this is likely
     * a new time and starts a new cycle! */
    session->newdata.status = session->lastfix.status;
    if (MODE_2D > session->oldfix.mode) {
        session->newdata.mode = MODE_2D;  // At least 2D
    } else {
        session->newdata.mode = session->lastfix.mode;
    }
    mask |= STATUS_SET | MODE_SET | ECEF_SET | TIME_SET | NTPTIME_IS;
    if (!TS_EQ(&ts_tow, &session->driver.tsip.last_tow)) {
        // New time, so new fix.
        mask |= CLEAR_IS;
        session->driver.tsip.last_tow = ts_tow;
    }
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x83: DP-XYZ: %f %f %f cbias %f tow %f mode %u\n",
             session->newdata.ecef.x,
             session->newdata.ecef.y,
             session->newdata.ecef.z,
             clock_bias, ftow,
             session->newdata.mode);
    return mask;
}

// decode packet x84
static gps_mask_t decode_x84(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;
    char ts_buf[TIMESPEC_LEN];
    double lat = getbed64(buf, 0) * RAD_2_DEG;   // lat, radians
    double lon = getbed64(buf, 8) * RAD_2_DEG;   // lon, radians
    // depending on GPS config, could be either WGS84 or MSL
    double d1 = getbed64(buf, 16);               // altitude, m
    double cbias = getbed64(buf, 16);            // clock bias, meters
    double ftow = getbef32(buf, 32);             // time-of-fix, s

    session->newdata.latitude = lat;
    session->newdata.longitude =lon;
    if (0 == session->driver.tsip.alt_is_msl) {
        session->newdata.altHAE = d1;
    } else {
        session->newdata.altMSL = d1;
    }
    mask |= ALTITUDE_SET;

    // short circuit to gpsdata. Convert m to ns
    session->gpsdata.fix.clockbias = 1e9 * cbias / CLIGHT;

    if (0 != (session->context->valid & GPS_TIME_VALID)) {
        // fingers crossed receiver set to UTC, not GPS.
        DTOTS(&ts_tow, ftow);
        session->newdata.time =
            gpsd_gpstime_resolv(session, session->context->gps_week,
                                ts_tow);
        mask |= TIME_SET | NTPTIME_IS;
        if (!TS_EQ(&ts_tow, &session->driver.tsip.last_tow)) {
            mask |= CLEAR_IS;
            session->driver.tsip.last_tow = ts_tow;
        }
    }
    mask |= LATLON_SET;
    /* No fix mode info!! That comes later in 0x6d.
     * Message sent when there is 2D or 3D fix.
     * This is a problem as gpsd will send a report with no mode.
     * This message only sent on 2D or 3D fix.
     * Steal mode from last fix. */
    session->newdata.status = session->oldfix.status;
    session->newdata.mode = session->oldfix.mode;
    mask |= STATUS_SET | MODE_SET;

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x84: DP-LLA: time=%s lat=%.2f lon=%.2f alt=%.2f %s "
             "cbias %.2f\n",
             timespec_str(&session->newdata.time, ts_buf, sizeof(ts_buf)),
             session->newdata.latitude,
             session->newdata.longitude, d1,
             session->driver.tsip.alt_is_msl ? "MSL" : "HAE", cbias);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: mode:%s status:%s\n",
             val2str(session->newdata.mode, vmode_str),
             val2str(session->newdata.status, vstatus_str));
    return mask;
}

/* decode Superpacket x8f-15
*/
static gps_mask_t decode_x8f_15(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    int s1 = getbes16(buf, 1);                 // Datum Index
    double d1 = getbed64(buf, 3);              // DX
    double d2 = getbed64(buf, 11);             // DY
    double d3 = getbed64(buf, 19);             // DZ
    double d4 = getbed64(buf, 27);             // A-axis
    double d5 = getbed64(buf, 35);             // Eccentricity Squared

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x8f-15: Current Datum: %d %f %f %f %f %f\n",
             s1, d1, d2, d3, d4, d5);
    return mask;
}

/* decode Last Fix with Extra Information, Superpacket x8f-20
 */
static gps_mask_t decode_x8f_20(struct gps_device_t *session, const char *buf,
                                const int length)
{
    gps_mask_t mask = 0;
    double d1, d2, d3, d4;
    timespec_t ts_tow;
    char buf2[80];
    char buf3[160];
    char ts_buf[TIMESPEC_LEN];
    int i;

    int s1 = getbes16(buf, 2);                // east velocity
    int s2 = getbes16(buf, 4);                // north velocity
    int s3 = getbes16(buf, 6);                // up velocity
    unsigned long tow = getbeu32(buf, 8);     // time in ms
    long lat = getbes32(buf, 12);             // latitude
    unsigned long lon = getbeu32(buf, 16);    // longitude
    // Lassen iQ, and copernicus (ii) doc says this is always altHAE
    long alt = getbes32(buf, 20);             // altitude
    unsigned u1 = getub(buf, 24);             // velocity scaling
    unsigned datum = getub(buf, 26);          // Datum + 1
    unsigned fflags = getub(buf, 27);         // fix flags
    int numSV = getub(buf, 28);               // num svs
    unsigned ls = getub(buf, 29);             // utc offset (leap seconds)
    unsigned week = getbeu16(buf, 30);        // tsip.gps_week

    // PRN/IODE data follows

    if (0 != (u1 & 0x01)) {     // check velocity scaling
        d4 = 0.02;
    } else {
        d4 = 0.005;
    }

    // 0x8000 is over-range
    if ((int)0x8000 != s2) {
        d2 = (double)s2 * d4;   // north velocity m/s
        session->newdata.NED.velN = d2;
    }
    if ((int16_t)0x8000 != s1) {
        d1 = (double)s1 * d4;   // east velocity m/s
        session->newdata.NED.velE = d1;
    }
    if ((int16_t)0x8000 != s3) {
        d3 = (double)s3 * d4;       // up velocity m/s
        session->newdata.NED.velD = -d3;
    }

    session->newdata.latitude = (double)lat * SEMI_2_DEG;
    session->newdata.longitude = (double)lon * SEMI_2_DEG;

    if (180.0 < session->newdata.longitude) {
        session->newdata.longitude -= 360.0;
    }
    // Lassen iQ doc says this is always altHAE in mm
    session->newdata.altHAE = (double)alt * 1e-3;
    mask |= ALTITUDE_SET;

    session->newdata.status = STATUS_UNK;
    session->newdata.mode = MODE_NO_FIX;
    if ((uint8_t)0 == (fflags & 0x01)) {          // Fix Available
        session->newdata.status = STATUS_GPS;
        if ((uint8_t)0 != (fflags & 0x02)) {      // DGPS Corrected
            session->newdata.status = STATUS_DGPS;
        }
        if ((uint8_t)0 != (fflags & 0x04)) {      // Fix Dimension
            session->newdata.mode = MODE_2D;
        } else {
            session->newdata.mode = MODE_3D;
        }
    }
    session->gpsdata.satellites_used = numSV;
    if (10 < ls) {
        session->context->leap_seconds = ls;
        session->context->valid |= LEAP_SECOND_VALID;
        /* check for week rollover
         * Trimble uses 15 bit weeks, but can guess the epoch wrong
         * Can not be in gpsd_gpstime_resolv() because that
         * may see BUILD_LEAPSECONDS instead of leap_seconds
         * from receiver.
         */
        if (17 < ls &&
            1930 > week) {
            // leap second 18 added in gps week 1930
            week += 1024;
            if (1930 > week) {
                // and again?
                week += 1024;
            }
        }
    }
    MSTOTS(&ts_tow, tow);
    session->newdata.time = gpsd_gpstime_resolv(session, week,
                                                ts_tow);
    mask |= TIME_SET | NTPTIME_IS | LATLON_SET |
            STATUS_SET | MODE_SET | VNED_SET;
    if (!TS_EQ(&ts_tow, &session->driver.tsip.last_tow)) {
        mask |= CLEAR_IS;
        session->driver.tsip.last_tow = ts_tow;
    }

    memset(session->driver.tsip.sats_used, 0,
           sizeof(session->driver.tsip.sats_used));
    buf3[0] = '\0';
    if (MAXCHANNELS < numSV) {
        // should not happen, pacify Coverity 493012
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIP x8f-20: MAXCHANNELS < numSV (%d)\n", numSV);
        return 0;
    }
    for (i = 0; i < numSV; i++) {
        if (length < (33 + (i * 2))) {
            // too short
            break;
        }
        // bits 0 to 5, junk in 5 to 7
        int PRN = getub(buf, 32 + (i * 2)) & 0x1f;
        int IODE = getub(buf, 33 + (i * 2));

        session->driver.tsip.sats_used[i] = PRN;
        if (LOG_PROG <= session->context->errout.debug) {
            str_appendf(buf3, sizeof(buf3),
                           " %d (%d)", session->driver.tsip.sats_used[i], IODE);
        }
    }

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x8f-20: LFwEI: %d %d %d tow %lu %ld "
             " %lu %lu %x fflags %x numSV %u ls %u week %d datum %u used:%s\n",
             s1, s2, s3, tow, lat, lon, alt, u1, fflags, numSV,
             ls, week, datum, buf3);
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x8f-20: LFwEI: time=%s lat=%.2f lon=%.2f "
             "altHAE=%.2f mode=%d status=%d\n",
             timespec_str(&session->newdata.time, ts_buf,
                          sizeof(ts_buf)),
             session->newdata.latitude, session->newdata.longitude,
             session->newdata.altHAE,
             session->newdata.mode, session->newdata.status);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: flags:%s\n",
             flags2str(fflags, vx8f_20_fflags, buf2, sizeof(buf2)));
    return mask;
}

/* decode Packet Broadcast Mask: Superpacket x8f-23
 */
static gps_mask_t decode_x8f_23(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    double d1, d2, d3, d5;
    timespec_t ts_tow;
    char ts_buf[TIMESPEC_LEN];

    unsigned long tow = getbeu32(buf, 1);    // time in ms
    unsigned week = getbeu16(buf, 5);        // tsip.gps_week
    unsigned u1 = getub(buf, 7);             // utc offset
    unsigned u2 = getub(buf, 8);             // fix flags
    long lat = getbes32(buf, 9);             // latitude
    unsigned long lon = getbeu32(buf, 13);   // longitude
    // Copernicus (ii) doc says this is always altHAE in mm
    long alt = getbes32(buf, 17);            // altitude
    // set xNED here
    int s2 = getbes16(buf, 21);              // east velocity
    int s3 = getbes16(buf, 23);              // north velocity
    int s4 = getbes16(buf, 25);              // up velocity

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x8f-23: CSP: tow %lu week %u %u %u %ld %lu %ld "
             " %d %d %d\n",
             tow, week, u1, u2, lat, lon, alt, s2, s3, s4);
    if (10 < (int)u1) {
        session->context->leap_seconds = (int)u1;
        session->context->valid |= LEAP_SECOND_VALID;
    }
    MSTOTS(&ts_tow, tow);
    session->newdata.time =
        gpsd_gpstime_resolv(session, week, ts_tow);
    session->newdata.status = STATUS_UNK;
    session->newdata.mode = MODE_NO_FIX;
    if ((u2 & 0x01) == (uint8_t)0) {          // Fix Available
        session->newdata.status = STATUS_GPS;
        if ((u2 & 0x02) != (uint8_t)0) {      // DGPS Corrected
            session->newdata.status = STATUS_DGPS;
        }
        if ((u2 & 0x04) != (uint8_t)0) {       // Fix Dimension
            session->newdata.mode = MODE_2D;
        } else {
            session->newdata.mode = MODE_3D;
        }
    }
    session->newdata.latitude = (double)lat * SEMI_2_DEG;
    session->newdata.longitude = (double)lon * SEMI_2_DEG;
    if (180.0 < session->newdata.longitude) {
        session->newdata.longitude -= 360.0;
    }
    // Copernicus (ii) doc says this is always altHAE in mm
    session->newdata.altHAE = (double)alt * 1e-3;
    mask |= ALTITUDE_SET;
    if ((u2 & 0x20) != (uint8_t)0) {     // check velocity scaling
        d5 = 0.02;
    } else {
        d5 = 0.005;
    }
    d1 = (double)s2 * d5;       // east velocity m/s
    d2 = (double)s3 * d5;       // north velocity m/s
    d3 = (double)s4 * d5;       // up velocity m/s
    session->newdata.NED.velN = d2;
    session->newdata.NED.velE = d1;
    session->newdata.NED.velD = -d3;

    mask |= TIME_SET | NTPTIME_IS | LATLON_SET |
            STATUS_SET | MODE_SET | VNED_SET;
    if (!TS_EQ(&ts_tow, &session->driver.tsip.last_tow)) {
        mask |= CLEAR_IS;
        session->driver.tsip.last_tow = ts_tow;
    }
    session->driver.tsip.req_compact = 0;
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x8f-23: SP-CSP: time %s lat %.2f lon %.2f "
             "altHAE %.2f mode %d status %d\n",
             timespec_str(&session->newdata.time, ts_buf,
                          sizeof(ts_buf)),
             session->newdata.latitude, session->newdata.longitude,
             session->newdata.altHAE,
             session->newdata.mode, session->newdata.status);
    return mask;
}

/* decode Superpacket x8f-42
 */
static gps_mask_t decode_x8f_42(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;

    unsigned u1 = getub(buf, 1);                 // Production Options Prefix
    unsigned u2 = getub(buf, 2);                 // Production Number Extension
    unsigned u3 = getbeu16(buf, 3);              // Case Sernum Prefix
    unsigned long ul1 = getbeu32(buf, 5);        // Case Sernum
    unsigned long ul2 = getbeu32(buf, 9);        // Production Number
    unsigned long ul3 = getbeu32(buf, 13);       // Resevered
    unsigned u4 = getbeu16(buf, 15);             // Machine ID
    unsigned u5 = getbeu16(buf, 17);             // Reserved

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x8f-42: SPP: Prod x%x-%x Sernum %x-%lx "
             "Prod %lx  Res %lx ID %x Res %x\n",
             u1, u2, u3, ul1, ul2, ul3, u4, u5);
    return mask;
}

/* decode Packet Broadcast Mask: Superpacket x8f-ad
 */
static gps_mask_t decode_x8f_a5(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    uint16_t mask0, mask1;
    char buf2[40];

    mask0 = getbeu16(buf, 1);    // Mask 0
    mask1 = getbeu16(buf, 3);    // Mask 1, reserved in ResSMT 360
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x8f-a5: PBM: mask0 x%04x mask1 x%04x\n",
             mask0, mask1);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: mask0::%s\n",
             flags2str(mask0, vpbm_mask0, buf2, sizeof(buf2)));

    return mask;
}

/* decode Superpacket x8f-a6
 */
static gps_mask_t decode_x8f_a6(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    unsigned u1 = getub(buf, 1);          // Command
    unsigned u2 = getub(buf, 2);          // Status

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x8f-a6: SSC: command x%x status x%x\n",
             u1, u2);
    return mask;
}

/* decode Superpacket x8f-a7
 * Individual Satellite Solutions
 *
 * Present in:
 *   Thunderbolt
 * Not present in:
 *   THunderbolt E
 *
 */
static gps_mask_t decode_x8f_a7(struct gps_device_t *session, const char *buf,
                                const int length)
{
    gps_mask_t mask = 0;

    // we assume the receiver not in some crazy mode, and is GPS time
    unsigned long tow = getbeu32(buf, 2);         // gpstime in seconds
    unsigned fmt = buf[1];                        // format, 0 Float, 1 Int

    if (0 == fmt) {
        // floating point mode
        double clock_bias = getbef32(buf, 6);    // clock bias (combined). s
        // clock bias rate (combined), s/s
        double clock_rate = getbef32(buf, 10);

        // short circuit to gpsdata
        session->gpsdata.fix.clockbias = clock_bias / 1e9;
        session->gpsdata.fix.clockdrift = clock_rate / 1e9;

        // FIXME: decode the individual biases
        GPSD_LOG(LOG_PROG, &session->context->errout,
                 "TSIP x8f-a7: tow %llu fmt %u bias %e "
                 "bias rate %e len %d\n",
                 (long long unsigned)tow, fmt, clock_bias, clock_rate, length);
    } else if (1 == fmt) {
        // integer mode
        int clock_bias = getbes16(buf, 6);   // Clock Bias (combined) 0.1ns
        int clock_rate = getbes16(buf, 8);   // Clock Bias rate (combined) ps/s

        // short circuit to gpsdata
        session->gpsdata.fix.clockbias = clock_bias / 10;
        session->gpsdata.fix.clockdrift = clock_rate / 1000;

        // FIXME: decode the individual biases
        GPSD_LOG(LOG_PROG, &session->context->errout,
                 "TSIP x8f-a7: tow %llu mode %u bias %ld "
                 "bias rate %ld len %d\n",
                 (long long unsigned)tow, fmt,
                 session->gpsdata.fix.clockbias,
                 session->gpsdata.fix.clockdrift, length);
    } else {
        // unknown mode
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIP x8f-a7: tow %llu fmt %u. Unnown mode len %d\n",
                 (long long unsigned)tow, fmt, length);
    }
    // FIME, loop over the individual sat data
    return mask;
}

/* decode Superpacket x8f-a9
 */
static gps_mask_t decode_x8f_a9(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;

    unsigned u1 = getub(buf, 1);              // Self Survey Enable
    unsigned u2 = getub(buf, 2);              // Position Save Flag
    unsigned long u3 = getbeu32(buf, 3);      // Self Survey Length
    unsigned long u4 = getbeu32(buf, 7);      // Reserved

    GPSD_LOG(LOG_WARN, &session->context->errout,
             "TSIP x8f-a9 SSP: sse %u psf %u length %ld rex x%lx \n",
             u1, u2, u3, u4);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: sse:%s sssave:%s\n",
             val2str(u1, vss_enable),
             val2str(u2, vss_save));
    return mask;
}

/* decode Superpacket x8f-ab
 * Oddly, no flag to say if the time is valid...
 */
static gps_mask_t decode_x8f_ab(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    unsigned long tow;                 // time of week in milli seconds
    timespec_t ts_tow;
    unsigned short week;
    char ts_buf[TIMESPEC_LEN];
    unsigned time_flag;
    char buf2[BUFSIZ];

    // we assume the receiver not in some crazy mode, and is GPS time
    tow = getbeu32(buf, 1);             // gpstime in seconds
    ts_tow.tv_sec = tow;
    ts_tow.tv_nsec = 0;
    week = getbeu16(buf, 5);            // week
    // leap seconds
    session->context->leap_seconds = (int)getbes16(buf, 7);
    time_flag = buf[9];                // Time Flag
    /* ignore the broken down time, use the GNSS time.
     * Hope it is not BeiDou time */

    if (1 == (time_flag & 1)) {
        // time is UTC, have leap seconds.
        session->context->valid |= LEAP_SECOND_VALID;
    } else {
        // time is GPS
        if (0 == (time_flag & 8)) {
            // have leap seconds.
            session->context->valid |= LEAP_SECOND_VALID;
        }
    }
    if (0 == (time_flag & 0x14)) {
        // time it good, not in test mode
        session->newdata.time = gpsd_gpstime_resolv(session, week,
                                                    ts_tow);
        mask |= TIME_SET | NTPTIME_IS;
    } else {
        // time is bad
    }

    if (!TS_EQ(&ts_tow, &session->driver.tsip.last_tow)) {
        mask |= CLEAR_IS;
        session->driver.tsip.last_tow = ts_tow;
    }

    /* since we compute time from weeks and tow, we ignore the
     * supplied H:M:S M/D/Y */
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x8f-ab: SP-TTS: tow %lu wk %u ls %d flag x%x "
             "time %s mask %s\n",
             tow, week, session->context->leap_seconds, time_flag,
             timespec_str(&session->newdata.time, ts_buf,
                          sizeof(ts_buf)),
             gps_maskdump(mask));
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: tf:%s\n",
             flags2str(time_flag, vtiming, buf2, sizeof(buf2)));

    return mask;
}

/* decode Supplemental Timing Packet (0x8f-ac)
 * present in:
 *   ThunderboltE
 *   ICM SMT 360
 *   RES SMT 360
 *   Resolution SMTx
 * Not Present in:
 *   pre-2000 models
 *   Lassen iQ
 *   Copernicus II (2009)
 */
static gps_mask_t decode_x8f_ac(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    char buf2[BUFSIZ];
    char buf3[BUFSIZ];

    // byte 0 is Subpacket ID
    unsigned rec_mode = getub(buf, 1);         // Receiver Mode
    // Disciplining Mode, ICM SMT 360 only
    unsigned disc_mode = getub(buf, 2);
    // Self-Survey Progress
    unsigned survey_prog = getub(buf, 3);
    // ignore 4-7, Holdover Duration, reserved on Resolution SMTx
    // ignore 8-9, Critical Alarms, reserved on Resolution SMTx
    unsigned crit_alarm = getbeu16(buf, 8);
    // Minor Alarms
    unsigned minor_alarm = getbeu16(buf, 10);
    unsigned decode_stat = getub(buf, 12);        // GNSS Decoding Status
    // Disciplining Activity, ICM SMT 360 Only
    unsigned disc_act = getub(buf, 13);
    // PPS indication, RES SMT 360 Only
    unsigned pps_ind = getub(buf, 14);
    unsigned pps_ref = getub(buf, 15);            // PPS reference
    /* PPS Offset in ns
     * save as (long)pico seconds
     * can't really use it as it is not referenced to any PPS */
    double fqErr = getbef32(buf, 16);          // PPS Offset. positive is slow.
    // Clock Offset (bias) ns. same as ppb
    double clk_off = getbef32(buf, 20);
    // ignore 24-27, DAC Value (ICM SMT 360 Only)
    double dac_v = getbef32(buf, 28);          // DAC Voltage
    // 32-35, Temperature degrees C
    session->newdata.temp = getbef32(buf, 32);
    session->newdata.latitude = getbed64(buf, 36) * RAD_2_DEG;
    session->newdata.longitude = getbed64(buf, 44) * RAD_2_DEG;
    // SMT 360 doc says this is always altHAE in meters
    session->newdata.altHAE = getbed64(buf, 52);
    // ignore 60-63, always zero, PPS Quanization error, ns ?
    // ignore 64-67, reserved

    switch (minor_alarm & 6) {
    case 2:
        session->newdata.ant_stat = ANT_OPEN;
        break;
    case 4:
        session->newdata.ant_stat = ANT_SHORT;
        break;
    default:
        session->newdata.ant_stat = ANT_OK;
        break;
    }

    session->gpsdata.qErr = (long)(fqErr * 1000);
    // short circuit to gpsdata.
    session->gpsdata.fix.clockbias = clk_off;

    // PPS indication
    if (3026 == session->driver.tsip.hardware_code) {
        // only ICM SMT 360 has disciplining activity
        // disc_act = 10;
    }
    // We don;t know enough to set status, probably TIME_TIME

    // Decode Fix modes
    switch (rec_mode & 7) {
    case 0:     // Auto
        /*
        * According to the Thunderbolt Manual, the
        * first byte of the supplemental timing packet
        * simply indicates the configuration of the
        * device, not the actual lock, so we need to
        * look at the decode status.
        */
        switch (decode_stat) {
        case 0:   // "Doing Fixes"
            session->newdata.mode = MODE_3D;
            break;
        case 0x0B: // "Only 3 usable sats"
            session->newdata.mode = MODE_2D;
            break;
        case 0x1:   // "Don't have GPS time"
            FALLTHROUGH
        case 0x3:   // "PDOP is too high"
            FALLTHROUGH
        case 0x8:   // "No usable sats"
            FALLTHROUGH
        case 0x9:   // "Only 1 usable sat"
            FALLTHROUGH
        case 0x0A:  // "Only 2 usable sats
            FALLTHROUGH
        case 0x0C:  // "The chosen sat is unusable"
            FALLTHROUGH
        case 0x10:  // TRAIM rejected the fix
            FALLTHROUGH
        default:
            session->newdata.mode = MODE_NO_FIX;
            break;
        }
        break;
    case 6:             // Clock Hold 2D
        /* Not present:
         *   SMT 360
         *   Acutime 360
         */
        FALLTHROUGH
    case 3:             // forced 2D Position Fix
        // Does this mean STATUS_TIME?
        session->newdata.mode = MODE_2D;
        break;
    case 1:             // Single Satellite Time
        /* Present in:
         *   Acutime 360
         */
        FALLTHROUGH
    case 7:             // overdetermined clock
        /* Present in:
         *   Acutiome 360
         *   ResSMT360
         *   Resolution SMTx
         */
        /*
        * According to the Thunderbolt Manual, the
        * first byte of the supplemental timing packet
        * simply indicates the configuration of the
        * device, not the actual lock, so we need to
        * look at the decode status.
        */
        session->newdata.status = STATUS_TIME;
        switch (decode_stat) {
        case 0:   // "Doing Fixes"
            session->newdata.mode = MODE_3D;
            break;
        case 0x9:   // "Only 1 usable sat"
            FALLTHROUGH
        case 0x0A:  // "Only 2 usable sats
            FALLTHROUGH
        case 0x0B: // "Only 3 usable sats"
            session->newdata.mode = MODE_2D;
            break;
        case 0x1:   // "Don't have GPS time"
            FALLTHROUGH
        case 0x3:   // "PDOP is too high"
            FALLTHROUGH
        case 0x8:   // "No usable sats"
            FALLTHROUGH
        case 0x0C:  // "The chosen sat is unusable"
            FALLTHROUGH
        case 0x10:  // TRAIM rejected the fix
            FALLTHROUGH
        default:
            session->newdata.mode = MODE_NO_FIX;
            break;
        }
        break;
    case 4:             // forced 3D position Fix
        session->newdata.mode = MODE_3D;
        break;
    default:
        session->newdata.mode = MODE_NO_FIX;
        break;
    }
    if (0 != (0x208 & minor_alarm) &&
        7 == (rec_mode & 7)) {
        // OD, No sats or position questionable, must be Dead reckoning
        session->newdata.mode = MODE_3D;
        session->newdata.status = STATUS_DR;
    }
    if (STATUS_UNK != session->newdata.status) {
        mask |= STATUS_SET;
    }

    mask |= LATLON_SET | ALTITUDE_SET | MODE_SET;
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP x8f-ac: SP-TPS: lat=%.2f lon=%.2f altHAE=%.2f "
             "mode %d status %d  temp %.1f disc %u pps_ind %u pps_ref %u "
             "fqErr %.4f clko %f DACV %f rm x%x dm %u "
             "sp %u ca %x ma x%x gds x%x\n",
             session->newdata.latitude,
             session->newdata.longitude,
             session->newdata.altHAE,
             session->newdata.mode,
             session->newdata.status,
             session->newdata.temp,
             disc_act, pps_ind, pps_ref, fqErr, clk_off, dac_v,  rec_mode,
             disc_mode, survey_prog, crit_alarm,
             minor_alarm, decode_stat);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: mode:%s status:%s rm:%s gds:%s ca:%s ma:%s disc_act %s "
             "pps_ing %s pps_ref %s\n",
             val2str(session->newdata.mode, vmode_str),
             val2str(session->newdata.status, vstatus_str),
             val2str(rec_mode, vrec_mode),
             val2str(decode_stat, vgnss_decode_status),
             flags2str(crit_alarm, vcrit_alarms, buf2,
                       sizeof(buf2)),
             flags2str(minor_alarm, vminor_alarms, buf3,
                       sizeof(buf3)),
             val2str(disc_act, vdisc_act),
             val2str(pps_ind, vpps_ind),
             val2str(pps_ref, vpps_ref));
    return mask;
}

// decode Superpackets x8f-XX
static gps_mask_t decode_x8f(struct gps_device_t *session, const char *buf,
                             int len, int *pbad_len, time_t now)
{
    gps_mask_t mask = 0;
    int bad_len = 0;
    unsigned u1 = getub(buf, 0);

    switch (u1) {           // sub-code ID
    case 0x15:
        /* Current Datum Values
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (43 > len) {
            bad_len = 43;
            break;
        }
        mask = decode_x8f_15(session, buf);
        break;

    case 0x20:
        /* Last Fix with Extra Information (binary fixed point) 0x8f-20
         * Only output when fix is available.
         * CSK sez "why does my Lassen SQ output oversize packets?"
         * Present in:
         *   pre-2000 models
         *   ACE II
         *   Copernicus, Copernicus II (64-bytes)
         * Not present in:
         *   ICM SMT 360
         *   RES SMT 360
         */
        if (56 != (len) &&
            64 != (len)) {
            bad_len = 56;
            break;
        }
        mask = decode_x8f_20(session, buf, len);
        break;
    case 0x23:
        /* Compact Super Packet (0x8f-23)
         * Present in:
         *   Copernicus, Copernicus II
         * Not present in:
         *   pre-2000 models
         *   Lassen iQ
         *   ICM SMT 360
         *   RES SMT 360
         */
        // CSK sez "i don't trust this to not be oversized either."
        if (29 > len) {
            bad_len = 29;
            break;
        }
        mask = decode_x8f_23(session, buf);
        break;

    case 0x42:
        /* Stored production parameters
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Resolution SMTx (2013)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        if (19 > len) {
            bad_len = 19;
            break;
        }
        mask = decode_x8f_42(session, buf);
        break;
    case 0xa5:
        /* Packet Broadcast Mask (0x8f-a5) polled by 0x8e-a5
         *
         * Present in:
         *   ICM SMT 360
         *   RES SMT 360
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *
         * Defaults:
         *   RES SMT 360: 05, 00
         *   Resolution SMTx: 05 00
         */
        if (5 > len) {
            bad_len = 5;
            break;
        }
        mask = decode_x8f_a5(session, buf);
        break;

    case 0xa6:
        /* Self-Survey Command (0x8f-a6) polled by 0x8e-a6
         *
         * Present in:
         *   ICM SMT 360
         *   RES SMT 360
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        if (3 > len) {
            bad_len = 3;
            break;
        }
        mask = decode_x8f_a6(session, buf);
        break;

    case 0xa7:
        /* Thunderbolt Individual Satellite Solutions
         * partial decode
         */
        if (10 > len) {
            bad_len = 10;
            break;
        }
        mask = decode_x8f_a7(session, buf, len);
        break;
    case 0xa9:
        /* Self Survey Parameters
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Resolution SMTx
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        if (11 > len) {
            bad_len = 11;
            break;
        }
        mask = decode_x8f_a9(session, buf);
        break;
    case 0xab:
        /* Thunderbolt Timing Superpacket
         * Present in:
         *   Resolution SMTx
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        if (17 > len) {
            bad_len = 17;
            break;
        }
        session->driver.tsip.last_41 = now; // keep timestamp for request
        mask = decode_x8f_ab(session, buf);
        break;

    case 0xac:
        /* Supplemental Timing Packet (0x8f-ac)
         * present in:
         *   ThunderboltE
         *   ICM SMT 360
         *   RES SMT 360
         *   Resolution SMTx
         * Not Present in:
         *   pre-2000 models
         *   Lassen iQ
         *   Copernicus II (2009)
         */
        if (68 > len) {
            bad_len = 68;
            break;
        }
        mask = decode_x8f_ac(session, buf);
        break;

    case 0x02:
        /* UTC Information
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x21:
        /* Request accuracy information
         * Present in:
         *   Copernicus II (2009)
         * Not Present in:
         *   pre-2000 models
         */
        FALLTHROUGH
    case 0x2a:
        /* Request Fix and Channel Tracking info, Type 1
         * Present in:
         *   Copernicus II (2009)
         * Not Present in:
         *   pre-2000 models
         */
        FALLTHROUGH
    case 0x2b:
        /* Request Fix and Channel Tracking info, Type 2
         * Present in:
         *   Copernicus II (2009)
         * Not Present in:
         *   pre-2000 models
         */
        FALLTHROUGH
    case 0x41:
        /* Stored manufacturing operating parameters x8f-41
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x4a:
        /* PPS characteristics
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         * Not Present in:
         *   pre-2000 models
         */
        FALLTHROUGH
    case 0x4e:
        /* PPS Output options
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x4f:
        /* Set PPS Width
         * Present in:
         *   Copernicus II (2009)
         * Not Present in:
         *   pre-2000 models
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x60:
        /* DR Calibration and Status Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x62:
        /* GPS/DR Position/Velocity Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x64:
        /* Firmware Version and Configuration Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x6b:
        /* Last Gyroscope Readings Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x6d:
        /* Last Odometer Readings Report x8f-6d
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x6f:
        /* Firmware Version Name Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x70:
        /* Beacon Channel Status Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x71:
        /* DGPS Station Database Reports
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x73:
        /* Beacon Channel Control Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x74:
        /* Clear Beacon Database Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x75:
        /* FFT Start Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x76:
        /* FFT Stop Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x77:
        /* FFT Reports
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x78:
        /* RTCM Reports
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x79:
        /* Beacon Station Attributes Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x7a:
        /* Beacon Station Attributes Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x7b:
        /* DGPS Receiver RAM Configuration Block Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x7c:
        /* DGPS Receiver Configuration Block Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x7e:
        /* Satellite Line-of-Sight (LOS) Message
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x7f:
        /* DGPS Receiver ROM Configuration Block Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x80:
        /* DGPS Service Provider System Information Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x81:
        /* Decoder Station Information Report and Selection Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x82:
        /* Decoder Diagnostic Information Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x84:
        /* Satellite FFT Control Acknowledgment x8f-84
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x85:
        /* DGPS Source Tracking Status Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x86:
        /* Clear Satellite Database Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x87:
        /* Network Statistics Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x88:
        /* Diagnostic Output Options Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x89:
        /* DGPS Source Control Report /Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x8a:
        /* Service Provider Information Report and Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x8b:
        /* Service Provider Activation Information Report & Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x8e:
        /* Service Provider Data Load Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x8f:
        /* Receiver Identity Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x90:
        /* Guidance Status Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x91:
        /* Guidance Configuration Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x92:
        /* Lightbar Configuration Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x94:
        /* Guidance Operation Acknowledgment
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x95:
        /* Button Box Configuration Type Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x96:
        /* Point Manipulation Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x97:
        /* Utility Information Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x98:
        /* Individual Button Configuration Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x9a:
        /* Differential Correction Information Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0xa0:
        /* DAC value
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0xa2:
        /* UTC/GPS timing
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0xa3:
        /* Oscillator disciplining command
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0xa8:
        /* Oscillator disciplining parameters
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    default:
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIP x8f-%02x: Unhandled TSIP superpacket\n", u1);
    }
    *pbad_len = bad_len;

    return mask;
}

// Decode Protocol Versi/on: x90-00
static gps_mask_t decode_x90_00(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;

    unsigned u1 = getub(buf, 4);              // NMEA Major version
    unsigned u2 = getub(buf, 5);              // NMEA Minor version
    unsigned u3 = getub(buf, 6);              // TSIP version
    unsigned u4 = getub(buf, 7);              // Trimble NMEA version
    unsigned long u6 = getbeu32(buf, 8);      // reserved
    unsigned u7 = getub(buf, 12);             // reserved
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 x90-00: NMEA %u.%u TSIP %u TNMEA %u "
             "res x%04lx x%02x \n",
             u1, u2, u3, u4, u6, u7);
    return mask;
}

/* Receiver Version Information, x90-01
 * Received in response to TSIPv1 probe
 */
static gps_mask_t decode_x90_01(struct gps_device_t *session, const char *buf,
                                int len)
{
    gps_mask_t mask = 0;
    char buf2[BUFSIZ];

    unsigned u1 = getub(buf, 4);               // Major version
    unsigned u2 = getub(buf, 5);               // Minor version
    unsigned u3 = getub(buf, 6);               // Build number
    unsigned u4 = getub(buf, 7);               // Build month
    unsigned u5 = getub(buf, 8);               // Build day
    unsigned u6 = getbeu16(buf, 9);            // Build year
    unsigned u7 = getbeu16(buf, 11);           // Hardware ID
    unsigned u8 = getub(buf, 13);              // Product Name length

    session->driver.tsip.hardware_code = u7;
    // check for valid module name length
    // RES720 is 27 long
    // check for valid module name length, again
    if (40 < u8) {
        u8 = 40;
    }
    if ((int)u8 > (len - 13)) {
        u8 = len - 13;
    }
    memcpy(buf2, &buf[14], u8);
    buf2[u8] = '\0';
    (void)snprintf(session->subtype, sizeof(session->subtype),
                   "fw %u.%u %u %02u/%02u/%04u %.40s",
                   u1, u2, u3, u6, u5, u4, buf2);
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 x90-01: Version %u.%u Build %u %u/%u/%u hwid %u, "
             "%.*s[%u]\n",
             u1, u2, u3, u6, u5, u4, u7, u8, buf2, u8);
    mask |= DEVICEID_SET;
    return mask;
}

// Decode, Port Configuration: x91-00
static gps_mask_t decode_x91_00(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;

    unsigned u1 = getub(buf, 4);               // port
    unsigned u2 = getub(buf, 5);               // port type
    unsigned u3 = getub(buf, 6);               // protocol
    unsigned u4 = getub(buf, 7);               // baud rate
    unsigned u5 = getub(buf, 8);               // data bits
    unsigned u6 = getub(buf, 9);               // parity
    unsigned u7 = getub(buf, 10);              // stop bits
    unsigned long u8 = getbeu32(buf, 11);      // reserved
    unsigned long u9 = getbeu32(buf, 12);      // reserved

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 x91-00: port %u type %u proto %u baud %u bits %u "
             "parity %u stop %u res x%04lx %04lx\n",
             u1, u2, u3, u4, u5, u6, u7, u8, u9);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: port:%s type:%s, proto:%s speed:%s bits:%s %s %s\n",
             val2str(u1, vport_name1),
             val2str(u2, vport_type1),
             val2str(u3, vprotocol1),
             val2str(u4, vspeed1),
             val2str(u5, vdbits1),
             val2str(u6, vparity1),
             val2str(u6, vstop1));
    return mask;
}

// Decode GNSS COnfiguration: x91-01
static gps_mask_t decode_x91_01(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    char buf2[BUFSIZ];

    /* constellations, 0 to 26, mashup of constellation and signal
     * ignore if 0xffffffff */
    unsigned long cons = getbeu32(buf, 4);     // constellations
    double d1 = getbef32(buf, 8);              // elevation mask
    double d2 = getbef32(buf, 12);             // signal mask
    double d3 = getbef32(buf, 16);             // PDOP mask
    // anti-jamming, always enabled in RES 720
    unsigned u2 = getub(buf, 20);
    unsigned u3 = getub(buf, 21);              // fix rate
    double d4 = getbef32(buf, 22);             // Antenna CAble delay, seconds
    unsigned long u4 = getbeu32(buf, 26);      // reserved

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 x91-01 cons x%lx el %f signal %f PDOP %f jam %u "
             "frate %u delay %f res x%04lx\n",
             cons, d1, d2, d3, u2, u3, d4, u4);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: cons %s\n",
             flags2str(cons, vsv_types1, buf2, sizeof(buf2)));
    return mask;
}

// Decode NVS Configuration, x91-02
static gps_mask_t decode_x91_02(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    char buf2[20];

    unsigned u1 = getub(buf, 6);               // status
    unsigned long u2 = getbeu32(buf, 7);            // reserved

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 x91-02: status %u res x%04lx\n",
             u1, u2);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: Status:%s\n",
             flags2str(u1, vsave_status1, buf2, sizeof(buf2)));
    return mask;
}

// Decode Timing Configuration: x91-03
static gps_mask_t decode_x91_03(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;

    unsigned tbase = getub(buf, 4);               // time base
    unsigned pbase = getub(buf, 5);               // PPS base
    unsigned pmask = getub(buf, 6);               // PPS mask
    unsigned res = getbeu16(buf, 7);              // reserved
    unsigned pwidth = getbeu16(buf, 9);           // PPS width
    double  poffset = getbed64(buf, 11);          // PPS offset, in seconds

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 x91-03: time base %u PPS base %u mask %u res x%04x "
             "width %u offset %f\n",
             tbase, pbase, pmask, res, pwidth, poffset);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: time base:%s pps base:%s pps mask:%s\n",
             val2str(tbase, vtime_base1),
             val2str(pbase, vtime_base1),
             val2str(pmask, vpps_mask1));
    return mask;
}

// Decode Self Survey Copnfiguration: x91-04
static gps_mask_t decode_x91_04(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    char buf2[BUFSIZ];

    unsigned u1 = getub(buf, 4);               // self-survey mask
    unsigned long u2 = getbeu32(buf, 5);       // self-survey length, # fixes
    unsigned u3 = getbeu16(buf, 9);            // horz uncertainty, meters
    unsigned u4 = getbeu16(buf, 11);           // vert uncertainty, meters

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 x91-04: mask x%x length %lu eph %u epv %u\n",
             u1, u2, u3, u4);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1:ssmask %s\n",
             flags2str(u1, vss_mask1, buf2, sizeof(buf2)));
    return mask;
}

// Decode Receiver COnfiguration: x91-05
static gps_mask_t decode_x91_05(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;

    unsigned port = getub(buf, 4);              // port
    unsigned long otype = getbeu32(buf, 5);     // type of output
    unsigned long res1 = getbeu32(buf, 9);      // reserved
    unsigned long res2 = getbeu32(buf, 13);     // reserved
    unsigned long res3 = getbeu32(buf, 17);     // reserved

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 x91-05: port %u type x%04lx res x%04lx x%04lx x%04lx\n",
             port, otype, res1, res2, res3);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: port %s xa1-00: %lu xa1-03: %lu xa1-11: %lu "
             "xa1-00: %lu  xa3-00: %lu  xa3-11: %lu\n",
             val2str(port, vport_name1),
             otype & 3, (otype >> 2) & 3, (otype >> 4) & 3,
            (otype >> 6) & 3, (otype >> 8) & 3, (otype >> 10) & 3);
    return mask;
}

// Decode Receiver Reset: x92-01
static gps_mask_t decode_x92_01(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    unsigned u1 = getub(buf, 6);               // reset cause

    GPSD_LOG(LOG_WARN, &session->context->errout,
             "TSIPv1 x92-01: cause %u\n", u1);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: cause:%s\n",
             val2str(u1, vreset_type1));
    return mask;
}

// Decode Production Information: x93-00
static gps_mask_t decode_x93_00(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    char buf2[BUFSIZ];
    char buf3[BUFSIZ];

    unsigned u1 = getub(buf, 4);                // reserved. always 0xff
    unsigned long u2 = getbeu32(buf, 5);        // serial number
    unsigned long long u3 = getbeu64(buf, 9);   // extended serial number
    unsigned long long u4 = getbeu64(buf, 17);  // extended serial number
    unsigned u5 = getub(buf, 25);               // build day
    unsigned u6 = getub(buf, 26);               // build month
    unsigned u7 = getbeu16(buf, 27);            // build year
    unsigned u8 = getub(buf, 29);               // build hour
    unsigned u9 = getbeu16(buf, 30);            // machine id
    // getbeu64(buf, 32);             // hardware ID string
    // getbeu64(buf, 40);             // hardware ID string
    // getbeu64(buf, 48);             // product ID string
    // getbeu64(buf, 56);             // product ID string
    unsigned long u10 = getbeu32(buf, 64);       // premium options
    unsigned long u11 = getbeu32(buf, 78);       // reserved
    // ignore 77 Osc search range, and 78–81 Osc offset, always 0xff

    (void)snprintf(session->subtype1, sizeof(session->subtype1),
                   "hw %u %02u/%02u/%04u",
                   u9, u5, u6, u7);
    // The sernum I get does not match the printed one on the device...
    // extended sernum seems to be zeros...
    (void)snprintf(session->gpsdata.dev.sernum,
                   sizeof(session->gpsdata.dev.sernum),
                   "%lx", u2);
    GPSD_LOG(LOG_WARN, &session->context->errout,
             "TSIPv1 x93-00: res %u ser %s x%llx-%llx Build %u/%u/%u %u "
             "machine %u hardware %s product %s "
             "options x%04lx res x%04lx\n",
             u1, session->gpsdata.dev.sernum,
             u3, u4, u7, u6, u5, u8, u9,
             gpsd_packetdump(buf2, sizeof(buf2),
                            (const unsigned char *)&buf[32], 16),
             gpsd_packetdump(buf3, sizeof(buf3),
                            (const unsigned char *)&buf[48], 16),
             u10, u11);
    mask |= DEVICEID_SET;
    return mask;
}

// Decode xa0-00
static gps_mask_t decode_xa0_00(struct gps_device_t *session, const char *buf,
                                int len)
{
    gps_mask_t mask = 0;
    unsigned u1, u2, u3;

    switch (len) {
    case 3:
        u1 = getub(buf, 6);               // command
        GPSD_LOG(LOG_PROG, &session->context->errout,
                 "TSIPv1 xa0-00: command %u\n", u1);
        break;
    case 8:
        // ACK/NAK
        u1 = getub(buf, 6);               // command
        u2 = getub(buf, 7);               // status
        u3 = getbeu16(buf, 8);            // frame
        GPSD_LOG(LOG_PROG, &session->context->errout,
                 "TSIPv1 xa0-00: command %u status %u frame %u\n",
                 u1, u2, u3);
        break;
    default:
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIPv1 xa0-00: bad length %d\n", len);
        break;
    }
    return mask;
}

// Decode xa1-00
static gps_mask_t decode_xa1_00(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    char buf2[BUFSIZ];
    unsigned u1, u2, u3;
    int s1;
    double d1, d2, d3;
    struct tm date = {0};

    unsigned tow = getbeu32(buf, 4);
    unsigned week = getbeu16(buf, 8);

    session->context->gps_week = week;

    date.tm_hour = getub(buf, 10);               // hours 0 - 23
    date.tm_min = getub(buf, 11);                // minutes 0 -59
    date.tm_sec = getub(buf, 12);                // seconds 0 - 60
    date.tm_mon = getub(buf, 13) - 1;            // month 1 - 12
    date.tm_mday = getub(buf, 14);               // day of month 1 - 31
    date.tm_year = getbeu16(buf, 15) - 1900;     // year

    u1 = getub(buf, 17);                // time base
    u2 = getub(buf, 18);                // PPS base
    u3 = getub(buf, 19);                // flags
    s1 = getbes16(buf, 20);             // UTC Offset
    d1 = getbef32(buf, 22);             // PPS Quantization Error
    d2 = getbef32(buf, 26);             // Bias
    d3 = getbef32(buf, 30);             // Bias Rate

    // convert seconds to pico seconds
    session->gpsdata.qErr = (long)(d1 * 10e12);
    // fix.time is w/o leap seconds...
    session->newdata.time.tv_sec = mkgmtime(&date) - s1;
    session->newdata.time.tv_nsec = 0;

    session->context->leap_seconds = s1;
    session->context->valid |= LEAP_SECOND_VALID;
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 xa1-00: tow %u week %u %02u:%02u:%02u %4u/%02u/%02u "
             "tbase %u/%u tflags x%x UTC offset %d qErr %f Bias %f/%f\n",
             tow, week, date.tm_hour, date.tm_min, date.tm_sec,
             date.tm_year + 1900, date.tm_mon, date.tm_mday,
             u1, u2, u3, s1, d1, d2, d3);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: tbase:%s pbase:%s tflags:%s\n",
             val2str(u1, vtime_base1),
             val2str(u2, vtime_base1),
             flags2str(u3, vtime_flags1, buf2, sizeof(buf2)));

    if (2 == (u3 & 2)) {
        // flags say we have good time
        // if we have good time, can we guess at fix mode?
        mask |= TIME_SET;
        if (1 == (u3 & 1)) {
            // good UTC
            mask |= NTPTIME_IS;
        }
    }
    if (0 == session->driver.tsip.hardware_code) {
        // Query Receiver Version Information
        (void)tsip_write1(session, "\x90\x01\x00\x02\x00\x93", 6);
    }
    mask |= CLEAR_IS;  // ssems to always be first. Time to clear.
    return mask;
}

// Decode packet xa1-02
static gps_mask_t decode_xa1_02(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;

    double d1 = getbef32(buf, 6);               // DAC voltage
    unsigned u1 = getbeu16(buf, 10);            // DAC value
    unsigned u2 = getub(buf, 12);               // holdover status
    unsigned u3 = getbeu32(buf, 13);            // holdover time

    session->newdata.temp = getbef32(buf, 17);  // Temperature, degrees C
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 xa1-02: DAC voltage %f value %u Holdover status %u "
             "time %u temp %f\n",
             d1, u1, u2, u3, session->newdata.temp);
    return mask;
}

// Decode packet Position Information, xa1-11
static gps_mask_t decode_xa1_11(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    char buf2[BUFSIZ];

    unsigned pmask = getub(buf, 4);            // position mask
    unsigned ftype = getub(buf, 5);            // fix type
    double d1 = getbed64(buf, 6);              // latitude or X
    double d2  = getbed64(buf, 14);            // longitude or Y
    double d3  = getbed64(buf, 22);            // altitude or Z
    double d4  = getbef32(buf, 30);            // velocity X or E
    double d5  = getbef32(buf, 34);            // velocity Y or N
    double d6  = getbef32(buf, 38);            // velocity Z or U

    double pdop = getbef32(buf, 42);  // PDOP, surveyed/current

    if (IN(0.01, pdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.pdop = pdop;
        mask |= DOP_SET;
    }
    session->newdata.eph = getbef32(buf, 46);  // eph, 0 - 100, unknown units
    session->newdata.epv = getbef32(buf, 50);  // epv, 0 - 100, unknown units
    mask |= DOP_SET;
    // position mask bit 0 does not tell us if we are in OD mode
    if (0 == (pmask & 2)) {
        // LLA
        session->newdata.latitude = d1;
        session->newdata.longitude = d2;
        if (0 == (pmask & 4)) {
            // HAE
            session->newdata.altHAE = d3;
        } else {
            // MSL
            session->newdata.altMSL = d3;
        }
        mask |= LATLON_SET | ALTITUDE_SET;
    } else {
        // XYZ ECEF
        session->newdata.ecef.x = d1;
        session->newdata.ecef.y = d2;
        session->newdata.ecef.z = d3;
        mask |= ECEF_SET;
    }
    if (0 == (pmask & 1)) {
        // valid velocity
        if (0 == (pmask & 8)) {
            // Velocity ENU
            session->newdata.NED.velN = d5;
            session->newdata.NED.velE = d4;
            session->newdata.NED.velD = -d6;
            mask |= VNED_SET;
        } else {
            // Velocity ECEF
            session->newdata.ecef.vx = d4;
            session->newdata.ecef.vy = d5;
            session->newdata.ecef.vz = d6;
            mask |= VECEF_SET;
        }
    }
    switch (ftype) {
    default:
        FALLTHROUGH
    case 0:
        session->newdata.mode = MODE_NO_FIX;
        break;
    case 1:
        session->newdata.mode = MODE_2D;
        break;
    case 2:
        session->newdata.mode = MODE_3D;
    }
    // status NOT set
    mask |= MODE_SET | DOP_SET | HERR_SET | VERR_SET;
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 xa1-11: mode %d status %d pmask %u fixt %u "
             "Pos %f %f %f Vel %f %f %f PDOP %f eph %f epv %f\n",
             session->newdata.mode,
             session->newdata.status,
             pmask, ftype, d1, d2, d3, d4, d5, d6,
             pdop,
             session->newdata.eph,
             session->newdata.epv);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: mode:%s status:%s pmask:%s fixt %s\n",
             val2str(session->newdata.mode, vmode_str),
             val2str(session->newdata.status, vstatus_str),
             flags2str(pmask, vpos_mask1, buf2, sizeof(buf2)),
             val2str(ftype, vfix_type1));
    return mask;
}

// decode packet xa2-00
static gps_mask_t decode_xa2_00(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    timespec_t ts_tow;
    unsigned char gnssid, sigid;
    char buf2[BUFSIZ];

    unsigned u1 = getub(buf, 4);               // message number, 1 to X

    // SV type, 0 to 26, mashup of constellation and signal
    unsigned u2 = getub(buf, 5);
    unsigned prn = getub(buf, 6);              // PRN (svid) 1 to 32 (99)
    double az = getbef32(buf, 7);              // azimuth, degrees
    double el = getbef32(buf, 11);             // elevation, degrees
    double d3 = getbef32(buf, 15);             // signal level, db-Hz
    unsigned u4 = getbeu32(buf, 19);           // Flags
    // TOW of measurement, not current TOW!
    unsigned tow = getbeu32(buf, 23);          // TOW, seconds

    if (1 == u1) {
        // message number starts at 1, no way to know last number
        gpsd_zero_satellites(&session->gpsdata);
        // start of new cycle, save last count
        session->gpsdata.satellites_visible =
            session->driver.tsip.last_chan_seen;
    }
    session->driver.tsip.last_chan_seen = u1;
    session->driver.tsip.last_a200 = tow;
    ts_tow.tv_sec = tow;
    ts_tow.tv_nsec = 0;
    session->gpsdata.skyview_time =
            gpsd_gpstime_resolv(session, session->context->gps_week,
                                ts_tow);

    // convert svtype to gnssid and svid
    gnssid = tsipv1_svtype(u2, &sigid);
    session->gpsdata.skyview[u1 - 1].gnssid = gnssid;
    session->gpsdata.skyview[u1 - 1].svid = prn;
    session->gpsdata.skyview[u1 - 1].sigid = sigid;
    // "real" NMEA 4.0 (not 4.10 ir 4.11) PRN
    session->gpsdata.skyview[u1 - 1].PRN = ubx2_to_prn(gnssid, prn);
    if (0 >= session->gpsdata.skyview[u1 - 1].PRN) {
        // bad PRN??
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIPv1 xa2-00(%u): Bad PRN: gnssid %u, prn %u PRN %d\n",
                 u1, gnssid, prn, session->gpsdata.skyview[u1 - 1].PRN);
    }
    if (0 != (1 & u4)) {
        if (90.0 >= fabs(el)) {
            session->gpsdata.skyview[u1 - 1].elevation = el;
        }
        if (360.0 > az &&
            0.0 <= az) {
            session->gpsdata.skyview[u1 - 1].azimuth = az;
        }
    }
    session->gpsdata.skyview[u1 - 1].ss = d3;
    if (0 != (6 & u4)) {
        session->gpsdata.skyview[u1 - 1].used = true;
    }

    if ((int)u1 >= session->gpsdata.satellites_visible) {
        /* Last of the series? Assume same number of sats as
         * last cycle.
         * This will cause extra SKY if this set has more
         * sats than the last set.  Will cause drop outs when
         * number of sats decreases. */
        if (10 < llabs(session->driver.tsip.last_a311 -
                       session->driver.tsip.last_a200)) {
            // no xa3-11 in 10 seconds, so push out now
            mask |= SATELLITE_SET;
            session->driver.tsip.last_a200 = 0;
        }
    }
    /* If this series has fewer than last series there will
     * be no SKY, unless the cycle ender pushes the SKY */
    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 xa2-00: num %u type %u (gnss %u sigid %u) PRN %u "
             "az %f el %f snr %f sflags x%0x4 tow %u\n",
             u1, u2, gnssid, sigid, prn, az, el, d3, u4, tow);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: svtype:%s flags:%s\n",
             val2str(u2, vsv_type1),
             flags2str(u4, vsflags1, buf2, sizeof(buf2)));
    return mask;
}


// decode System Alarms,  packet xa3-00
static gps_mask_t decode_xa3_00(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    char buf2[BUFSIZ];
    char buf3[BUFSIZ];

    unsigned minor_alarm = getbeu32(buf, 4);            // Minor Alarms
    unsigned res1 = getbeu32(buf, 8);                   // reserved
    unsigned major_alarm = getbeu32(buf, 12);           // Major Alarms
    unsigned res2 = getbeu32(buf, 16);                  // reserved

    if (1 & minor_alarm) {
        session->newdata.ant_stat = ANT_OPEN;
    } else if (2 & minor_alarm) {
        session->newdata.ant_stat = ANT_SHORT;
    } else {
        session->newdata.ant_stat = ANT_OK;
    }

    if (1 == (major_alarm & 1)) {
        // not tracking sats, assume surveyed-in
        session->newdata.status = STATUS_DR;
    } else {
        session->newdata.status = STATUS_GPS;
    }
    if (0x80 == (major_alarm & 0x80)) {
        // jamming
        session->newdata.jam = 255;
    } else if (0x40 == (major_alarm & 0x40)) {
        // spoofing/multipath
        session->newdata.jam = 128;
    }
    mask |= STATUS_SET;

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 xa3-00: Minor x%04x res x%04x Major x%04x "
             "res x%04u status %d\n",
             minor_alarm, res1, major_alarm, res2,
             session->newdata.status);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: minor:%s mojor:%s status:%s\n",
             flags2str(minor_alarm, vminor_alarms1, buf2, sizeof(buf2)),
             flags2str(major_alarm, vmajor_alarms1, buf3, sizeof(buf3)),
             val2str(session->newdata.status, vstatus_str));
    return mask;
}

// decode packet xa3-11
static gps_mask_t decode_xa3_11(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;

    unsigned rec_mode = getub(buf, 4);                // receiver mode
    unsigned rec_status = getub(buf, 5);              // status
    unsigned ssp = getub(buf, 6);              // self survey progress 0 - 100

    double pdop = getbef32(buf, 7);     // PDOP
    double hdop = getbef32(buf, 11);    // HDOP
    double vdop = getbef32(buf, 15);    // VDOP
    double tdop = getbef32(buf, 19);    // TDOP

    session->newdata.temp = getbef32(buf, 23);        // Temperature, degrees C

    if (IN(0.01, pdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.pdop = pdop;
        mask |= DOP_SET;
    }
    if (IN(0.01, hdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.hdop = hdop;
        mask |= DOP_SET;
    }
    if (IN(0.01, vdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.vdop = vdop;
        mask |= DOP_SET;
    }
    if (IN(0.01, tdop, 89.99)) {
        // why not to newdata?
        session->gpsdata.dop.tdop = tdop;
        mask |= DOP_SET;
    }

    // don't have tow, so use the one from xa2-00, if any
    session->driver.tsip.last_a311 = session->driver.tsip.last_a200;

    if (0 < session->driver.tsip.last_a200) {
        session->driver.tsip.last_a200 = 0;
        // TSIPv1 seem to be sent in numerical order, so this
        // is after xa2-00 and the sats.  Push out any lingering sats.
        mask |= SATELLITE_SET;
    }
    mask |= REPORT_IS;
    switch (rec_status) {
    case 0:         // 2D
        session->newdata.mode = MODE_2D;
        mask |= MODE_SET;
        break;
    case 1:         // 3D (time only)
        session->newdata.mode = MODE_3D;
        mask |= MODE_SET;
        break;
    case 3:         // Automatic (?)
        break;
    case 4:         // OD clock
        session->newdata.status = STATUS_TIME;
        mask |= STATUS_SET;
        break;
    default:        // Huh?
        break;
    }

    switch (rec_status) {
    case 0:         // doing position fixes
        FALLTHROUGH
    case 4:         // using 1 sat
        FALLTHROUGH
    case 5:         // using 2 sat
        FALLTHROUGH
    case 6:         // using 3 sat
        session->newdata.status = STATUS_GPS;
        mask |= STATUS_SET;
        break;
    case 1:         // no GPS time
        FALLTHROUGH
    case 2:         // PDOP too high
        FALLTHROUGH
    case 3:         // no sats
        session->newdata.status = STATUS_UNK;
        mask |= STATUS_SET;
        break;
    case 255:
        session->newdata.mode = MODE_3D;
        session->newdata.status = STATUS_TIME;
        mask |= STATUS_SET | MODE_SET;
        break;
    default:
        // huh?
        break;
    }

    if (10.0 < pdop) {
        session->newdata.status = STATUS_DR;
        mask |= STATUS_SET;
    }

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIPv1 xa3-11: mode %d status %d rm %u stat %u survey %u "
             "PDOP %f HDOP %f VDOP %f TDOP %f temp %f\n",
             session->newdata.mode,
             session->newdata.status,
             rec_mode, rec_status, ssp,
             pdop, hdop, vdop, tdop,
             session->newdata.temp);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: mode:%s status:%s rm:%s stat:%s\n",
             val2str(session->newdata.mode, vmode_str),
             val2str(session->newdata.status, vstatus_str),
             val2str(rec_mode, vrec_mode1),
             val2str(rec_status, vgnss_decode_status1));

    return mask;
}

// decode packet xa3-21
static gps_mask_t decode_xa3_21(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;

    unsigned u1 = getub(buf, 4);            // reference packet id
    unsigned u2 = getub(buf, 5);            // reference sub packet id
    unsigned u3 = getub(buf, 6);            // error code

    GPSD_LOG(LOG_WARN, &session->context->errout,
             "TSIPv1 xa3-21: id x%02x-%02x error: %u\n",
             u1, u2, u3);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIPv1: ec:%s\n",
             val2str(u3, verr_codes1));
    return mask;
}

// Decode xbb
static gps_mask_t decode_xbb(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;
    unsigned u1 = getub(buf, 0);        // Subcode, always zero?
    unsigned u2 = getub(buf, 1);        // Operating Dimension (Receiver Mode)
    unsigned u3 = getub(buf, 2);        // DGPS Mode (not in Acutime Gold)
    unsigned u4 = getub(buf, 3);        // Dynamics Code
    double f1 = getbef32(buf, 5);       // Elevation Mask
    double f2 = getbef32(buf, 9);       // AMU Mask
    double f3 = getbef32(buf, 13);      // DOP Mask
    double f4 = getbef32(buf, 17);      // DOP Switch
    unsigned u5 = getub(buf, 21);       // DGPS Age Limit (not in Acutime Gold)
    /* Constellation
     * bit 0 - GPS
     * bit 1 - GLONASS
     * bit 2 - reserved
     * bit 3 - BeiDou
     * bit 4 - Galileo
     * bit 5 - QZSS
     * bit 6 - reserved
     * bit 7 - reserved
     */
    // RES SMT 360 defaults to Mode 7, Constellation 3
    unsigned u6 = getub(buf, 27);

    GPSD_LOG(LOG_PROG, &session->context->errout,
             "TSIP xbb: Navigation Configuration: %u %u %u %u %f %f %f "
             "%f %u x%x\n",
             u1, u2, u3, u4, f1, f2, f3, f4, u5, u6);
    GPSD_LOG(LOG_IO, &session->context->errout,
             "TSIP: rm %s\n",
             val2str(u1, vrec_mode));
    return mask;
}

// decode packet xd0-00
static gps_mask_t decode_xd0_00(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;

    unsigned u1 = getub(buf, 6);               // debug output type
    GPSD_LOG(LOG_WARN, &session->context->errout,
             "TSIPv1 xd0-00: debug %u\n", u1);

    return mask;
}

// decode packet xd0-01
static gps_mask_t decode_xd0_01(struct gps_device_t *session, const char *buf)
{
    gps_mask_t mask = 0;

    unsigned u1 = getub(buf, 6);               // debug type
    unsigned u2 = getub(buf, 7);               // debug level

    GPSD_LOG(LOG_WARN, &session->context->errout,
             "TSIPv1 xd0-01: debug type %u level %u\n", u1, u2);

    return mask;
}

/* parse TSIP v1 packages.
* Currently only in RES720 devices, from 2020 onward.
* buf: raw data, with DLE stuffing removed
* len:  length of data in buf
*
* return: mask
*/
static gps_mask_t tsipv1_parse(struct gps_device_t *session, unsigned id,
                                const char *buf, int len)
{
    gps_mask_t mask = 0;
    unsigned sub_id, length, mode;
    unsigned u1;
    bool bad_len = false;
    unsigned char chksum;

    if (4 > len) {
        // should never happen
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIPv1 0x%02x: runt, got len %u\n",
                 id, len);
        return mask;
    }
    /* Note: bug starts at sub id, offset 2 of the wire packet.
     * So subtract 2 from the offsets in the Trimble doc. */
    sub_id = getub(buf, 0);
    length = getbeu16(buf, 1);  // expected length
    mode = getub(buf, 3);

    if ((length + 3) != (unsigned)len) {
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIPv1 x%02x-%02x: Bad Length, "
                 "length got %d expected %u mode %u\n",
                 id, sub_id, len, length + 3, mode);
        return mask;
    }

    // checksum is id, sub id, length, mode, data, not including trailer
    // length is mode + data + checksum
    chksum = id;
    for (u1 = 0; u1 < (length + 3); u1++ ) {
        chksum ^= buf[u1];
    }
    if (0 != chksum) {
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIPv1 x%02x-%02x: Bad Checksum "
                 "length %d/%u mode %u\n",
                 id, sub_id, len, length + 3, mode);
        return mask;
    }

    GPSD_LOG(LOG_DATA, &session->context->errout,
             "TSIPv1 x%02x-%02x: length %d/%u mode %u\n",
             id, sub_id, len, length + 3, mode);

    if (2 != mode) {
        /* Don't decode queries (mode 0) or set (mode 1).
         * Why would we even see one? */
        return mask;
    }
    // FIXME: check len/length
    switch ((id << 8) | sub_id) {
    case 0x9000:
        // Protocol Version, x90-00
        if (11 > length) {
            bad_len = true;
            break;
        }
        mask = decode_x90_00(session, buf);
        break;
    case 0x9001:
        /* Receiver Version Information, x90-01
         * Received in response to the TSIPv1 probe */
        if (11 > length) {
            bad_len = true;
            break;
        }
        mask = decode_x90_01(session, buf, len);
        break;
    case 0x9100:
        // Port Configuration, x91-00
        if (17 > length) {
            bad_len = true;
            break;
        }
        mask = decode_x91_00(session, buf);
        break;
    case 0x9101:
        // GNSS Configuration, x91-01
        if (28 > length) {
            bad_len = true;
            break;
        }
        mask = decode_x91_01(session, buf);
        break;
    case 0x9102:
        // NVS Configuration, x91-02
        if (8 > length) {
            bad_len = true;
            break;
        }
        mask = decode_x91_02(session, buf);
        break;
    case 0x9103:
        // Timing Configuration, x91-03
        if (19 > length) {
            bad_len = true;
            break;
        }
        mask = decode_x91_03(session, buf);
        break;
    case 0x9104:
        // Self-Survey Configuration, x91-04
        if (11 > length) {
            bad_len = true;
            break;
        }
        mask = decode_x91_04(session, buf);
        break;
    case 0x9105:
        //  Receiver Configuration, xx91-05
        if (19 > length) {
            bad_len = true;
            break;
        }
        mask = decode_x91_05(session, buf);
        break;
    case 0x9201:
        // Reset Cause, x92-01
        if (3 > length) {
            bad_len = true;
            break;
        }
        mask = decode_x92_01(session, buf);
        break;
    case 0x9300:
        // Production Information, x93-00
        if (78 > length) {
            bad_len = true;
            break;
        }
        mask = decode_x93_00(session, buf);
        break;
    case 0xa000:
        // Firmware Upload, xa0-00
        // could be length 3, or 8, different data...
        if (3 != length &&
            8 != length) {
            bad_len = true;
            break;
        }
        mask = decode_xa0_00(session, buf, length);
        break;
    case 0xa100:
        // Timing Information. xa1-00
        // the only message on by default
        if (32 > length) {
            bad_len = true;
            break;
        }
        mask = decode_xa1_00(session, buf);
        break;

    case 0xa102:
        // Frequency Information, xa1-02
        if (17 > length) {
            bad_len = true;
            break;
        }
        mask = decode_xa1_02(session, buf);
        break;

    case 0xa111:
        // Position Information, xa1-11
        if (52 > length) {
            bad_len = true;
            break;
        }
        mask = decode_xa1_11(session, buf);
        break;
    case 0xa200:
        // Satellite Information, xa2-00
        if (25 > length) {
            bad_len = true;
            break;
        }
        mask = decode_xa2_00(session, buf);
        break;

    case 0xa300:
        // System Alarms, xa3-00
        if (18 > length) {
            bad_len = true;
            break;
        }
        mask = decode_xa3_00(session, buf);
        break;
    case 0xa311:
        /* Receiver Status, xa3-11
         * RES 720
         */
        if (29 > length) {
            bad_len = true;
            break;
        }
        // usually the last message, except for A2-00 (sats)
        mask = decode_xa3_11(session, buf);
        break;
    case 0xa321:
        /* Error Report xa3-21
         * expect errors for x1c-03 and x35-32 from TSIP probes
         */
        if (5 > length) {
            bad_len = true;
            break;
        }
        mask = decode_xa3_21(session, buf);
        break;
    case 0xd000:
        // Debug Output type packet, xd0-00
        if (3 > length) {
            bad_len = true;
            break;
        }
        mask = decode_xd0_00(session, buf);
        break;
    case 0xd001:
        // Trimble Debug config packet, xd0-01
        if (4 > length) {
            bad_len = true;
            break;
        }
        mask = decode_xd0_01(session, buf);
        break;
    case 0xd040:
        // Trimble Raw GNSS Debug Output packet. xd0-40
        // length can be zero, contents undefined
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIPv1 xd0-40: raw GNSS data\n");
        break;
    case 0xd041:
        // Trimble Raw GNSS Debug Output packet. xd0-41
        // length can be zero, contents undefined
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIPv1 xd0-41: raw GNSS data\n");
        break;

    // undecoded:
    case 0x9200:
        // Receiver Reset, send only, x92-00
        FALLTHROUGH
    case 0xa400:
        // AGNSS, send only, xa4-00
        FALLTHROUGH
    default:
        // Huh?
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIPv1 x%02x-%02x: unknown packet id/su-id\n",
                 id, sub_id);
        break;
    }
    if (bad_len) {
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIPv1 0x%02x-%02x: runt, got length %u\n",
                 id, sub_id, length);
        mask = 0;
    }
    // get next item off queue
    tsipv1_query(session);

    return mask;
}

/* This is the meat of parsing all the TSIP packets, except v1
 *
 * Return: mask
 */
static gps_mask_t tsip_parse_input(struct gps_device_t *session)
{
    int i, len;
    gps_mask_t mask = 0;
    unsigned int id;
    time_t now;
    char buf[BUFSIZ];
    char buf2[BUFSIZ];
    int bad_len = 0;

    if (TSIP_PACKET != session->lexer.type) {
        // this should not happen
        GPSD_LOG(LOG_INF, &session->context->errout,
                 "TSIP: tsip_analyze packet type %d\n",
                 session->lexer.type);
        return 0;
    }

    if (4 > session->lexer.outbuflen ||
        0x10 != session->lexer.outbuffer[0]) {
        // packet too short, or does not start with DLE
        GPSD_LOG(LOG_INF, &session->context->errout,
                 "TSIP: tsip_analyze packet bad packet\n");
        return 0;
    }

    /* get receive time, first
     * using system time breaks regressions!
     * so use latest from receiver */
    if (0 != session->lastfix.time.tv_sec) {
        now = session->lastfix.time.tv_sec;
    } else if (0 != session->oldfix.time.tv_sec) {
        now = session->oldfix.time.tv_sec;
    } else {
        now = 0;
    }

    // put data part of message in buf

    memset(buf, 0, sizeof(buf));
    len = 0;
    for (i = 2; i < (int)session->lexer.outbuflen; i++) {
        if (0x10 == session->lexer.outbuffer[i] &&
            0x03 == session->lexer.outbuffer[++i]) {
                // DLE, STX.  end of packet, we know the length
                break;
        }
        buf[len++] = session->lexer.outbuffer[i];
    }

    id = (unsigned)session->lexer.outbuffer[1];
#ifdef __UNUSED__      // debug code
    GPSD_LOG(LOG_SHOUT, &session->context->errout,
             "TSIP x%02x: length %d: %s\n",
             id, len, gps_hexdump(buf2, sizeof(buf2),
             (char *)session->lexer.outbuffer, session->lexer.outbuflen));
#endif  // __UNUSED__
    GPSD_LOG(LOG_DATA, &session->context->errout,
             "TSIP x%02x: length %d: %s\n",
             id, len, gps_hexdump(buf2, sizeof(buf2),
                                  (unsigned char *)buf, len));

    // session->cycle_end_reliable = true;
    switch (id) {
    case 0x13:
        /* Packet Received
         * Present in:
         *   pre-2000 models
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Resolution SMTx
         * Not present in:
         *   Copernicus II
         */
        if (1 > len) {
            bad_len = 1;
            break;
        }
        mask = decode_x13(session, buf, len);
        break;

    case 0x1c:        // Hardware/Software Version Information
        /* Present in:
         *   Acutime Gold
         *   Lassen iQ (2005) fw 1.16+
         *   Copernicus (2006)
         *   Copernicus II (2009)
         *   Thunderbolt E (2012)
         *   RES SMT 360 (2018)
         *   ICM SMT 360 (2018)
         *   RES360 17x22 (2018)
         *   Acutime 360
         * Not Present in:
         *   pre-2000 models
         *   ACE II (1999)
         *   ACE III (2000)
         *   Lassen SQ (2002)
         *   Lassen iQ (2005) pre fw 1.16
         */
        mask = decode_x1c(session, buf, len, &bad_len);
        break;
    case 0x41:
        /* GPS Time (0x41).  polled by 0x21
         * Note: this is not the time of current fix
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Resolution SMTx
         */
        if (10 > len) {
            bad_len = 10;
            break;
        }
        session->driver.tsip.last_41 = now;     // keep timestamp for request
        mask = decode_x41(session, buf);
        break;
    case 0x42:
        /* Single-Precision Position Fix, XYZ ECEF
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (16 > len) {
            bad_len = 16;
            break;
        }
        mask = decode_x42(session, buf);
        break;
    case 0x43:
        /* Velocity Fix, XYZ ECEF
         * Present in:
         *   pre-2000 models
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   Copernicus II (2009)
         */
        if (20 > len) {
            bad_len = 20;
            break;
        }
        mask = decode_x43(session, buf);
        break;
    case 0x45:
        /* Software Version Information (0x45)
         * Present in:
         *   pre-2000 models
         *   ACE II (1999)
         *   ACE III (2000)
         *   Lassen SQ (2002)
         *   Lassen iQ (2005)
         *   Copernicus II (2009)
         *   ICM SMT 360
         *   RES SMT 360
         * Not present in:
         *   RES 720
         */
        if (10 > len) {
            bad_len = 10;
            break;
        }
        mask = decode_x45(session, buf);
        break;
    case 0x46:
        /* Health of Receiver (0x46).  Poll with 0x26
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018) (deprecated, used x8f-ab or x8f-1c)
         *   Resolution SMTx
         *   all models?
         */
        if (2 > len) {
            bad_len = 2;
            break;
        }
        session->driver.tsip.last_46 = now;
        mask = decode_x46(session, buf);
        break;
    case 0x47:
        /* Signal Levels for all Satellites
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (1 > len) {
            bad_len = 1;
            break;
        }
        mask = decode_x47(session, buf, len, &bad_len);
        break;
    case 0x48:
        /* GPS System Message
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        buf[len] = '\0';
        GPSD_LOG(LOG_PROG, &session->context->errout,
                 "TSIP x48: GPS System Message: %s\n", buf);
        break;
    case 0x4a:
        /* Single-Precision Position LLA
         * Only sent when valid
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (20 > len) {
            bad_len = 20;
            break;
        }
        mask = decode_x4a(session, buf);
        break;
    case 0x4b:
        /* Machine/Code ID and Additional Status (0x4b)
         * polled by 0x25 (soft reset) or 0x26 (request health).
         * Sent with 0x46 (receiver health).
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Resolution SMTx
         * Deprecated in:
         *   Resolution SMTx
         * Not in:
         *   Thunderbolt (2003)
         */
        if (3 > len) {
            bad_len = 3;
            break;
        }
        mask = decode_x4b(session, buf);
        break;
    case 0x4c:
        /* Operating Parameters Report (0x4c).  Polled by 0x2c
         * Present in:
         *   pre-2000 models
         *   Lassen iQ, but not documented
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (17 > len) {
            bad_len = 17;
            break;
        }
        mask = decode_x4c(session, buf);
        break;
    case 0x54:
        /* Bias and Bias Rate Report (0x54)
         * Present in:
         *   pre-2000 models
         *   Acutime 360
         *   ICM SMT 360  (undocumented)
         *   RES SMT 360  (undocumented)
         * Not Present in:
         *   Copernicus II (2009)
         *   Resolution SMTx
         */
        if (12 > len) {
            bad_len = 12;
            break;
        }
         mask = decode_x54(session, buf);
        break;
    case 0x55:
        /* IO Options (0x55), polled by 0x35
         * Present in:
         *   pre-2000 models
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Resolution SMTx
         *   all TSIP?
         *
         * Defaults:
         *   Lassen iQ:       02 02 00 00
         *   RES SMT 360:     12 02 00 08
         *   Resolution SMTx: 12 02 00 08
         */
        if (4 > len) {
            bad_len = 4;
            break;
        }
        mask = decode_x55(session, buf, now);
        break;
    case 0x56:
        /* Velocity Fix, East-North-Up (ENU)
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (20 > len) {
            bad_len = 20;
            break;
        }
        mask = decode_x56(session, buf);
        break;
    case 0x57:
        /* Information About Last Computed Fix
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (8 > len) {
            bad_len = 8;
            break;
        }
        mask = decode_x57(session, buf);
        break;
    case 0x5a:
        /* Raw Measurement Data
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (25 > len) {
            bad_len = 25;
            break;
        }
        mask = decode_x5a(session, buf);
        break;
    case 0x5c:
        /* Satellite Tracking Status (0x5c) polled by 0x3c
         *
         * GPS only, no WAAS reported here or used in fix
         * Present in:
         *   pre-2000 models
         *   Copernicus, Copernicus II
         *   Thunderbold E
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (24 > len) {
            bad_len = 24;
            break;
        }
        mask = decode_x5c(session, buf);
        break;

     case 0x5d:
        /* GNSS Satellite Tracking Status (multi-GNSS operation) (0x5d)
         * polled by 0x3c
         *
         * GNSS only, no WAAS reported here or used in fix
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus, Copernicus II
         *   Thunderbold E
         */
        if (26 > len) {
            bad_len = 26;
            break;
        }
        mask = decode_x5d(session, buf);
        break;
    case 0x6c:
        /* Satellite Selection List (0x6c) polled by 0x24
         * Eeerily similar to 0x6d, the difference is where the sat count is.
         *
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   Lassen SQ (2002)
         *   Lassen iQ (2005) */
        if (18 > len) {
            bad_len = 18;
            break;
        }
        // why same as 6d?
        session->driver.tsip.last_6d = now;     // keep timestamp for request
        mask = decode_x6c(session, buf, len, &bad_len);
        break;
    case 0x6d:
        /* All-In-View Satellite Selection (0x6d) polled by 0x24
         * Sent after every fix
         *
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   Lassen SQ
         *   Lassen iQ
         * Deprecated in:
         *   Resolution SMTx
         * Not present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (17 > len) {
            bad_len = 17;
            break;
        }
        session->driver.tsip.last_6d = now;     // keep timestamp for request
        mask = decode_x6d(session, buf, len, &bad_len);
        break;
    case 0x82:
        /* Differential Position Fix Mode (0x82) poll with 0x62-ff
         * Sent after every position fix in Auto GPS/DGPS,
         * so potential cycle ender
         *
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   Lassen SQ
         *   Lassen iQ, deprecated use 0xbb instead
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (1 > len) {
            bad_len = 1;
            break;
        }
        mask = decode_x82(session, buf);
        break;
    case 0x83:
        /* Double-Precision XYZ Position Fix and Bias Information
         * Only sent when valid
         * Present in:
         *   pre-2000 models
         *   LasenSQ (2002)
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (36 > len) {
            bad_len = 36;
            break;
        }
        mask = decode_x83(session, buf);
        break;
    case 0x84:
        /* Double-Precision LLA Position Fix and Bias Information
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   LassenSQ  (2002)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (36 > len) {
            bad_len = 36;
            break;
        }
        mask = decode_x84(session, buf);
        break;
    case 0x8f:
        /* Super Packet.
         * Present in:
         *   pre-2000 models
         *   ACE II
         *   ACE III
         *   Copernicus II (2009)
         *   ICM SMT 360
         *   RES SMT 360
         *   Resolution SMTx
         */
        mask = decode_x8f(session, buf, len, &bad_len, now);
        break;
// Start of TSIP V1
    case 0x90:
        /* Version Information, TSIP v1
         * Present in:
         *   RES720
         */
        FALLTHROUGH
    case 0x91:
        /* Receiver Configuration, TSIP v1
         * Present in:
         *   RES720
         */
        FALLTHROUGH
    case 0x92:
        /* Resets, TSIP v1
         * Present in:
         *   RES720
         */
        FALLTHROUGH
    case 0x93:
        /* Production & Manufacturing, TSIP v1
         * Present in:
         *   RES720
         */
        FALLTHROUGH
    case 0xa0:
        /* Firmware Upload, TSIP v1
         * Present in:
         *   RES720
         */
        FALLTHROUGH
    case 0xa1:
        /* PVT, TSIP v1
         * Present in:
         *   RES720
         */
        FALLTHROUGH
    case 0xa2:
        /* GNSS Information, TSIP v1
         * Present in:
         *   RES720
         */
        FALLTHROUGH
    case 0xa3:
        /* Alarms % Status, TSIP v1
         * Present in:
         *   RES720
         */
        FALLTHROUGH
    case 0xa4:
        /* AGNSS, TSIP v1
         * Present in:
         *   RES720
         */
        FALLTHROUGH
    case 0xa5:
        /* Miscellaneous, TSIP v1
         * Present in:
         *   RES720
         */
        FALLTHROUGH
    case 0xd0:
        /* Debug & Logging, TSIP v1
         * Present in:
         *   RES720
         */
        return tsipv1_parse(session, id, buf, len);
// end of TSIP V1
    case 0xbb:
        /* Navigation Configuration
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        if (40 != len &&
            43 != len) {
            // see packet.c for explamation
            bad_len = 40;
            break;
        }
        mask = decode_xbb(session, buf);
        break;

    case 0x1a:
        /* TSIP RTCM Wrapper Command
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x2e:
        /* Request GPS Time
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x32:
        /* Request Unit Position
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x38:
        /* Request SV System data
         * Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x40:
        /* Almanac Data for Single Satellite Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x44:
        /* Non-Overdetermined Satellite Selection Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x49:
        /* Almanac Health Page
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x4d:
        /* Oscillator Offset
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x4e:
        /* Response to set GPS time
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x4f:
        /* UTC Parameters Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x53:
        /* Analog-to-Digital Readings Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x58:
        /* Satellite System Data/Acknowledge from Receiver
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x59:
        /* Status of Satellite Disable or Ignore Health
         * aka Satellite Attribute Database Status Report
         * Present in:
         *   pre-2000 models
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x5b:
        /* Satellite Ephemeris Status
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x5e:
        /* Additional Fix Status Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x5f:
        /* Severe Failure Notification
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x60:
        /* Differential GPS Pseudorange Corrections Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x61:
        /* Differential GPS Delta Pseudorange Corrections Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x6a:
        /* Differential Corrections Used in the Fix Repor
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x6e:
        /* Synchronized Measurements
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x6f:
        /* Synchronized Measurements Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x70:
        /* Filter Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x76:
        /* Overdetermined Mode Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x78:
        /* Maximum PRC Age Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x7a:
        /* NMEA settings
         * Not Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x7b:
        /* NMEA interval and message mask response
         * Present in:
         *   pre-2000 models
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x7d:
        /* Position Fix Rate Configuration Reports
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x85:
        /* Differential Correction Status Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x87:
        /* Reference Station Parameters Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x89:
        /* Receiver acquisition sensitivity mode
         * Present in:
         *   Copernicus II (2009)
         * Not Present in:
         *   pre-2000 models
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0x88:
        /* Mobile Differential Parameters Report
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x8b:
        /* QA/QC Reports
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0x8d:
        /* Average Position Reports
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0xb0:
        /* PPS and Event Report Packets
         * Present in:
         *   pre-2000 models
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         *   Copernicus II (2009)
         */
        FALLTHROUGH
    case 0xbc:
        /* Receiver port configuration
         * Present in:
         *   pre-2000 models
         *   Copernicus II (2009)
         * Not Present in:
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         */
        FALLTHROUGH
    case 0xc1:
        /* Bit Mask for GPIOs in Standby Mode
         * Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         */
        FALLTHROUGH
    case 0xc2:
        /* SBAS SV Mask
         * Present in:
         *   Copernicus II (2009)
         *   ICM SMT 360 (2018)
         *   RES SMT 360 (2018)
         * Not Present in:
         *   pre-2000 models
         */
        FALLTHROUGH
    default:
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIP x%02x: Unhandled packet type\n", id);
        break;
    }

#ifdef __UNUSED__
// #if 1
    // full reset
    (void)tsip_write1(session, "\x1e\x46", 2);
#endif

    if (bad_len) {
        GPSD_LOG(LOG_WARNING, &session->context->errout,
                 "TSIP x%02x: wrong len %d s/b >= %d \n", id, len, bad_len);
    } else {
        GPSD_LOG(LOG_IO, &session->context->errout,
                 "TSIP x%02x: mask %s\n", id, gps_maskdump(mask));
    }
    /* See if it is time to send some request packets for reports that.
     * The receiver won't send at fixed intervals.
     * Use llabs() as time sometimes goes backwards. */

    if (5 < llabs(now - session->driver.tsip.last_41)) {
        /* Request Current Time returns 0x41.
         * Easiest way to get GPS weeks and current leap seconds */
        (void)tsip_write1(session, "\x21", 1);
        session->driver.tsip.last_41 = now;
    }

    if (5 < llabs(now - session->driver.tsip.last_6d)) {
        /* Request GPS Receiver Position Fix Mode
         * Returns 0x44, 0x6c, or 0x6d
         * We need one of those to get PDOP, HDOP, etc.
         * At least on RexSMT360. */
        (void)tsip_write1(session, "\x24", 1);
        session->driver.tsip.last_6d = now;
#ifdef __UNUSED__
// #if 1
        // request Receiver Configuration (0xbb)
        (void)tsip_write1(session, "\xbb\x00", 2);

        // request Packet Broadcast Mask
        (void)tsip_write1(session, "\x8e\xa5", 2);
#endif // UNUSED
    }

    if (1 > session->driver.tsip.superpkt &&
        60 < llabs(now - session->driver.tsip.last_48)) {
        /* Request GPS System Message
         * Returns 0x48.
         * not supported on:
         *  Lassen SQ (2002)
         *  Lassen iQ (2005)
         *  ICM SMT 360
         *  RES SMT 360
         *  and post 2005
         * SuperPackets replaced 0x28 */
        (void)tsip_write1(session, "\x28", 1);
        session->driver.tsip.last_48 = now;
    }

    if (5 < llabs(now - session->driver.tsip.last_5c)) {
        /* Request Current Satellite Tracking Status
         * Returns: 0x5c or 0x5d
         *  5c from GPS only devices
         *  5d from multi-gnss devices */
        // 00 == All satellites
        (void)tsip_write1(session, "\x3c\x00", 2);
        session->driver.tsip.last_5c = now;
    }

    if (5 < llabs(now - session->driver.tsip.last_46)) {
        /* Request Health of Receiver
         * Returns 0x46 and 0x4b. */
        (void)tsip_write1(session, "\x26", 1);
        session->driver.tsip.last_46 = now;
    }
    if ((0 < session->driver.tsip.req_compact) &&
        (5 < llabs(now - session->driver.tsip.req_compact))) {
        /* Compact Superpacket requested but no response
         * Not in:
         * ICM SMT 360
         * RES SMT 360
          */
        session->driver.tsip.req_compact = 0;
        GPSD_LOG(LOG_WARN, &session->context->errout,
                 "TSIP x8f-23: No Compact Super Packet, "
                 "try LFwEI (0x8f-20)\n");

        // Request LFwEI Super Packet 0x8f-20, enabled
        (void)tsip_write1(session, "\x8e\x20\x01", 3);
    }

    return mask;
}

static void tsip_init_query(struct gps_device_t *session)
{
    // Use 0x1C-03 to Request Hardware Version Information (0x1C-83)
    (void)tsip_write1(session, "\x1c\x03", 2);
    /*
     * After HW information packet is received, a
     * decision is made how to configure the device.
     */
}

static void tsip_event_hook(struct gps_device_t *session, event_t event)
{
    GPSD_LOG(LOG_SPIN, &session->context->errout,
             "TSIP: event_hook event %d ro %d\n",
             event, session->context->readonly);

    if (session->context->readonly ||
        session->context->passive) {
        return;
    }
    switch (event) {
    case EVENT_IDENTIFIED:
        FALLTHROUGH
    case EVENT_REACTIVATE:
        /* reactivate style needs to depend on model
         * So send Request Software Version (0x1f), which returns 0x45.
         * Once we have the x45, we can decide how to configure */
        (void)tsip_write1(session, "\x1f", 1);
        break;
    case EVENT_CONFIGURE:
        // this seems to get called on every packet...
        if (0 == session->lexer.counter) {
            /* but the above if() makes it never execute
             * formerely tried to force 801 here, but luckily it
             * never fired as some Trimble are 8N1 */
        }
        break;
    case EVENT_DEACTIVATE:
        // used to revert serial port parms here.  No need for that.
        FALLTHROUGH
    default:
        break;
    }
}

static bool tsip_speed_switch(struct gps_device_t *session,
                              speed_t speed, char parity, int stopbits)
{
    char buf[100];

    switch (parity) {
    case 'E':
    case 2:
        parity = (char)2;
        break;
    case 'O':
    case 1:
        parity = (char)1;
        break;
    case 'N':
    case 0:
    default:
        parity = (char)0;
        break;
    }

    buf[0] = 0xbc;          // Set Port Configuration (0xbc)
    buf[1] = 0xff;          // current port
    // input dev.baudrate
    buf[2] = (round(log((double)speed / 300) / GPS_LN2)) + 2;
    buf[3] = buf[2];        // output baudrate
    buf[4] = 3;             // character width (8 bits)
    buf[5] = parity;        // parity (normally odd)
    buf[6] = stopbits - 1;  // stop bits (normally 1 stopbit)
    buf[7] = 0;             // flow control (none)
    buf[8] = 0x02;          // input protocol (TSIP)
    buf[9] = 0x02;          // output protocol (TSIP)
    buf[10] = 0;            // reserved
    (void)tsip_write1(session, buf, 11);

    return true;            // it would be nice to error-check this
}

static void tsip_mode(struct gps_device_t *session, int mode)
{
    if (MODE_NMEA == mode) {
        char buf[16];

        /* send NMEA Interval and Message Mask Command (0x7a)
         * First turn on the NMEA messages we want */
        buf[0] = 0x7a;
        buf[1] = 0x00;  //  subcode 0
        buf[2] = 0x01;  //  1-second fix interval
        buf[3] = 0x00;  //  Reserved
        buf[4] = 0x00;  //  Reserved
        buf[5] = 0x01;  //  1=GST, Reserved
        /* 1=GGA, 2=GGL, 4=VTG, 8=GSV,
         * 0x10=GSA, 0x20=ZDA, 0x40=Reserved, 0x80=RMC  */
        buf[6] = 0x19;

        (void)tsip_write1(session, buf, 7);

        // Now switch to NMEA mode
        memset(buf, 0, sizeof(buf));

        buf[0] = 0x8c;     // Set Port Configuration (0xbc)
        buf[1] = 0xff;     // current port
        buf[2] = 0x06;     // 4800 bps input.  4800, really?
        buf[3] = buf[2];   // output SAME AS INPUT
        buf[4] = 0x03;     // 8 data bits
        buf[5] = 0x00;     // No parity
        buf[6] = 0x00;     // 1 stop bit
        buf[7] = 0x00;     // No flow control
        buf[8] = 0x02;     // Input protocol TSIP
        buf[9] = 0x04;     // Output protocol NMEA
        buf[10] = 0x00;    // Reserved

        (void)tsip_write1(session, buf, 11);

    } else if (MODE_BINARY == mode) {
        /* The speed switcher also puts us back in TSIP, so call it
         * with the default 9600 8O1. */
        // FIXME: Should preserve the current speed.
        // (void)tsip_speed_switch(session, 9600, 'O', 1);
        // FIXME: should config TSIP binary!
        ;

    } else {
        GPSD_LOG(LOG_ERROR, &session->context->errout,
                 "TSIP: unknown mode %i requested\n", mode);
    }
}

// this is everything we export
// *INDENT-OFF*
const struct gps_type_t driver_tsip =
{
    .type_name      = "Trimble TSIP",     // full name of type
    .packet_type    = TSIP_PACKET,        // associated lexer packet type
    .flags          = DRIVER_STICKY,      // remember this
    .trigger        = NULL,               // no trigger
    .channels       = TSIP_CHANNELS,      // consumer-grade GPS
    .probe_detect   = tsip_detect,        // probe for 9600O81 device
    .get_packet     = packet_get1,        // use the generic packet getter
    .parse_packet   = tsip_parse_input,   // parse message packets
    .rtcm_writer    = NULL,               // doesn't accept DGPS corrections
    .init_query     = tsip_init_query,    // non-perturbing initial query
    .event_hook     = tsip_event_hook,    // fire on various lifetime events
    .speed_switcher = tsip_speed_switch,  // change baud rate
    .mode_switcher  = tsip_mode,          // there is a mode switcher
    .rate_switcher  = NULL,               // no rate switcher
    .min_cycle.tv_sec  = 1,               // not relevant, no rate switch
    .min_cycle.tv_nsec = 0,               // not relevant, no rate switch
    .control_send   = tsip_write1,        // how to send commands
    .time_offset     = NULL,
};
// *INDENT-ON*

#endif  // TSIP_ENABLE

// vim: set expandtab shiftwidth=4
