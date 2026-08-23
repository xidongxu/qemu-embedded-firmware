/*
 * mpsx_dev.h - pjmedia-audiodev factory for the QEMU mpsx audio/mic devices.
 *
 * The factory is registered at runtime (from pj_phone.c) via the public
 * pjmedia_aud_register_factory() API, so no upstream pjproject source file
 * needs to be modified for the mps2-an505 board.
 */
#ifndef MPSX_DEV_H
#define MPSX_DEV_H

#include <pjmedia/audiodev.h>

/* Create the mpsx audio/mic pjmedia-audiodev factory. */
pjmedia_aud_dev_factory* pjmedia_mpsx_audio_factory(pj_pool_factory *pf);

#endif /* MPSX_DEV_H */
