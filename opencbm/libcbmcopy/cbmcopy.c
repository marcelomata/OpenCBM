/*
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 *  Copyright 2001 Michael Klein <michael.klein@puffin.lb.shuttle.de>
 */

#ifdef SAVE_RCSID
static char *rcsid =
    "@(#) $Id: cbmcopy.c,v 1.1 2004-11-07 11:05:12 strik Exp $";
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opencbm.h"
#include "cbmcopy.h"

#include "cbmcopy_int.h"


static const unsigned char turboread1541[] = {
#include "turboread1541.inc"
};

static const unsigned char turboread1571[] = {
#include "turboread1571.inc"
};

static const unsigned char turboread1581[] = {
#include "turboread1581.inc"
};

static const unsigned char turbowrite1541[] = {
#include "turbowrite1541.inc"
};

static const unsigned char turbowrite1571[] = {
#include "turbowrite1571.inc"
};

static const unsigned char turbowrite1581[] = {
#include "turbowrite1581.inc"
};

extern transfer_funcs cbmcopy_s1_transfer,
                      cbmcopy_s2_transfer,
                      cbmcopy_pp_transfer;

static struct _transfers
{
    const transfer_funcs *trf;
    const char *name, *abbrev;
}
transfers[] =
{
    { &cbmcopy_s1_transfer, "serial1", "s1" },
    { &cbmcopy_s2_transfer, "serial2", "s2" },
    { &cbmcopy_pp_transfer, "parallel", "p%" },
    { NULL, NULL, NULL }
};


static int check_drive_type(CBM_FILE fd, unsigned char drive,
                            cbmcopy_settings *settings,
                            cbmcopy_message_cb msg_cb)
{
    const char *type_str;

    if( settings->drive_type == cbm_dt_unknown )
    {
        if(cbm_identify( fd, drive, &settings->drive_type, &type_str ))
        {
            msg_cb( sev_warning, "could not identify drive, using 1541 turbo" );
            settings->drive_type = cbm_dt_cbm1541;
        }
        else
        {
            msg_cb( sev_info, "identified a %s drive", type_str );
        }
    }
    return 0;
}


static int send_turbo(CBM_FILE fd, unsigned char drive, int write,
                      const cbmcopy_settings *settings,
                      const unsigned char *turbo, size_t turbo_size,
                      const unsigned char *start_cmd, size_t cmd_len,
                      cbmcopy_message_cb msg_cb)
{
    const transfer_funcs *trf;

    trf = transfers[settings->transfer_mode].trf;
    cbm_upload( fd, drive, 0x500, turbo, turbo_size );
    msg_cb( sev_debug, "uploading %d bytes turbo code", turbo_size );
    if(trf->upload_turbo(fd, drive, settings->drive_type, write) == 0)
    {
        cbm_exec_command( fd, drive, start_cmd, cmd_len );
        msg_cb( sev_debug, "initializing transfer code" );
        if(trf->start_turbo(fd, write) == 0)
        {
            msg_cb( sev_debug, "done" );
            return 0;
        }
        else
        {
            msg_cb( sev_fatal, "could not start turbo" );
        }
    }
    else
    {
        msg_cb( sev_fatal, "Unsupported transfer mode for this device" );
    }
    return -1;
}


static int cbmcopy_read(CBM_FILE fd,
                        cbmcopy_settings *settings,
                        unsigned char drive,
                        int track, int sector,
                        const unsigned char *cbmname,
                        int cbmname_len,
                        unsigned char **filedata,
                        int *filedata_size,
                        cbmcopy_message_cb msg_cb,
                        cbmcopy_status_cb status_cb)
{
    int rv;
    int i;
    int turbo_size;
    int error;
    unsigned char c;
    unsigned char *cptr;
    unsigned char buf[48];
    const char *turbo;
    const transfer_funcs *trf;
    int blocks_read;

    *filedata = NULL;
    *filedata_size = 0;

    msg_cb( sev_debug, "using transfer mode `%s�",
            transfers[settings->transfer_mode].name);
    trf = transfers[settings->transfer_mode].trf;

    if(check_drive_type( fd, drive, settings, msg_cb ))
    {
        return -1;
    }

    switch(settings->drive_type)
    {
        case cbm_dt_cbm1541:
            turbo = turboread1541;
            turbo_size = sizeof(turboread1541);
            break;
        case cbm_dt_cbm1570:
        case cbm_dt_cbm1571:
            cbm_exec_command( fd, drive, "U0>M1", 0 );
            turbo = turboread1571;
            turbo_size = sizeof(turboread1571);
            break;
        case cbm_dt_cbm1581:
            turbo = turboread1581;
            turbo_size = sizeof(turboread1581);
            break;
        default: /* unreachable */
            return -1;
    }

    if(cbmname)
    {
        /* start by file name */
        track = 0;
        sector = 0;
        cbm_open( fd, drive, 0, NULL, 0 );
        if(cbmname_len == 0) cbmname_len = strlen( cbmname );
        cbm_raw_write( fd, cbmname, cbmname_len );
        cbm_unlisten( fd );
    }
    else
    {
        /* start by track/sector */
        cbm_open( fd, drive, 0, "#", 1 );
    }
    rv = cbm_device_status( fd, drive, buf, sizeof(buf) );

    if(rv)
    {
        msg_cb( sev_fatal, "could not open file for reading: %s", buf );
        cbm_driver_close(fd);
        return rv;
    }

    blocks_read = 0;
    error = 0;

    if(track)
    {
        msg_cb( sev_debug, "start read at %d/%d", track, sector );
    }
    sprintf( buf, "U4:%c%c", (unsigned char)track, (unsigned char)sector );

    if(send_turbo(fd, drive, 0, settings,
                  turbo, turbo_size, buf, 5, msg_cb) == 0)
    {
        msg_cb( sev_debug, "start of copy" );
        status_cb( blocks_read );

        for(c = 0xff;
            c == 0xff && (error = trf->check_error(fd, 0)) == 0;
            /* nothing */ )
        {
            c = trf->read_byte( fd );
            i = (c == 0xff) ? 0xfe : c;
            *filedata_size += i;
            *filedata = realloc(*filedata, *filedata_size);
            if(*filedata)
            {
#ifdef CBMCOPY_DEBUG
                msg_cb( sev_debug, "receive block data (%d)", c );
#endif 
                for(cptr = (*filedata) + blocks_read * 254; i; i--)
                {
                    *(cptr++) = trf->read_byte( fd );
                }
                /* (drive is busy now) */
                
                status_cb( ++blocks_read );
            }
            else
            {
                /* FIXME */
            }
        }
        msg_cb( sev_debug, "done" );
        trf->exit_turbo( fd, 0 );
    }

    return rv;
}


char *cbmcopy_get_transfer_modes()
{
    const struct _transfers *t;
    int size;
    char *buf;
    char *dst;

    size = 1; /* for terminating '\0' */
    for(t = transfers; t->name; t++)
    {
        size += (strlen(t->name) + 1);
    }

    buf = malloc(size);

    if(buf)
    {
        dst = buf;
        for(t = transfers; t->name; t++)
        {
            strcpy(dst, t->name);
            dst += (strlen(t->name) + 1);
        }
        *dst = '\0';
    }

    return buf;
}


int cbmcopy_get_transfer_mode_index(const char *name)
{
    const struct _transfers *t;
    int i;
    int abbrev_len;
    int tm_len;

    if(NULL == name)
    {
        /* default transfer mode */
        return 0;
    }

    tm_len = strlen(name);
    for(i = 0, t = transfers; t->name; i++, t++)
    {
        if(strcmp(name, t->name) == 0)
        {
            /* full match */
            return i;
        }
        if(t->abbrev[strlen(t->abbrev)-1] == '%')
        {
            abbrev_len = strlen(t->abbrev) - 1;
            if(abbrev_len <= tm_len && strncmp(t->name, name, tm_len) == 0)
            {
                return i;
            }
        }
        else
        {
            if(strcmp(name, t->abbrev) == 0)
            {
                return i;
            }
        }
    }
    return -1;
}


cbmcopy_settings *cbmcopy_get_default_settings(void)
{
    cbmcopy_settings *settings;

    settings = malloc(sizeof(cbmcopy_settings));

    if(NULL != settings)
    {
        settings->drive_type    = cbm_dt_unknown; /* auto detect later on */
        settings->transfer_mode = 0;
    }
    return settings;
}



int cbmcopy_write_file(CBM_FILE fd,
                       cbmcopy_settings *settings,
                       int drivei,
                       const unsigned char *cbmname,
                       int cbmname_len,
                       const unsigned char *filedata,
                       int filedata_size,
                       cbmcopy_message_cb msg_cb,
                       cbmcopy_status_cb status_cb)
{
    int rv;
    int i;
    int turbo_size;
    unsigned char drive = (unsigned char) drivei; //! \todo Find better solution
    int error;
    unsigned char c;
    unsigned char buf[48];
    const char *turbo;
    const transfer_funcs *trf;
    int blocks_written;

    msg_cb( sev_debug, "using transfer mode `%s�",
            transfers[settings->transfer_mode].name);
    trf = transfers[settings->transfer_mode].trf;

    if(check_drive_type( fd, drive, settings, msg_cb ))
    {
        return -1;
    }

    switch(settings->drive_type)
    {
        case cbm_dt_cbm1541:
            turbo = turbowrite1541;
            turbo_size = sizeof(turbowrite1541);
            break;
        case cbm_dt_cbm1570:
        case cbm_dt_cbm1571:
            cbm_exec_command( fd, drive, "U0>M1", 0 );
            turbo = turbowrite1571;
            turbo_size = sizeof(turbowrite1571);
            break;
        case cbm_dt_cbm1581:
            turbo = turbowrite1581;
            turbo_size = sizeof(turbowrite1581);
            break;
        default: /* unreachable */
            return -1;
    }

    cbm_open( fd, drive, 1, NULL, 0 );
    if(cbmname_len == 0) cbmname_len = strlen( cbmname );
    cbm_raw_write( fd, cbmname, cbmname_len );
    cbm_unlisten( fd );
    rv = cbm_device_status( fd, drive, buf, sizeof(buf) );

    if(rv)
    {
        msg_cb( sev_fatal, "could not open file for writing: %s", buf );
        cbm_driver_close(fd);
        return rv;
    }

    blocks_written = 0;
    error = 0;

    if(send_turbo(fd, drive, 1, settings,
                  turbo, turbo_size, "U4:", 3, msg_cb) == 0)
    {
        msg_cb( sev_debug, "start of copy" );
        status_cb( blocks_written );
        for(i = 0;
            (i == 0) || (i < filedata_size && !error );
            i+=254)
        {
            if( filedata_size - i <= 254 )
            {
                c = filedata_size - i;
            }
            else
            {
                c = 255;
            }
#ifdef CBMCOPY_DEBUG
            msg_cb( sev_debug, "send byte count: %d", c );
#endif
            trf->write_byte( fd, c );

            if(c)
            {
#ifdef CBMCOPY_DEBUG
                msg_cb( sev_debug, "send block data" );
#endif 
                if( c == 0xff ) c = 0xfe;
                while(c)
                {
                    trf->write_byte( fd, *(filedata++) );
                    c--;
                }

                /* (drive is busy now) */
            }
            error = trf->check_error( fd, 1 );
            if(!error)
            {
                status_cb( ++blocks_written );
            }
        }
        msg_cb( sev_debug, "done" );

        trf->exit_turbo( fd, 1 );
    }
    return rv;
}


/* just a wrapper */
int cbmcopy_read_file_ts(CBM_FILE fd,
                         cbmcopy_settings *settings,
                         int drive,
                         int track, int sector,
                         unsigned char **filedata,
                         int *filedata_size,
                         cbmcopy_message_cb msg_cb,
                         cbmcopy_status_cb status_cb)
{
    return cbmcopy_read(fd, settings, (unsigned char) drive,
                        track, sector,
                        NULL, 0,
                        filedata, filedata_size,
                        msg_cb, status_cb);
}


/* just a wrapper */
int cbmcopy_read_file(CBM_FILE fd,
                      cbmcopy_settings *settings,
                      int drive,
                      const unsigned char *cbmname,
                      int cbmname_len,
                      unsigned char **filedata,
                      int *filedata_size,
                      cbmcopy_message_cb msg_cb,
                      cbmcopy_status_cb status_cb)
{
    return cbmcopy_read(fd, settings, (unsigned char) drive,
                        0, 0,
                        cbmname, cbmname_len,
                        filedata, filedata_size,
                        msg_cb, status_cb);
}