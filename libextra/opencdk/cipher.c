/* cipher.c - Cipher filters
 *        Copyright (C) 2002, 2003, 2007 Timo Schulz
 *        Copyright (C) 1998, 1999, 2000, 2001 Free Software Foundation
 *
 * This file is part of OpenCDK.
 *
 * OpenCDK is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * OpenCDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#include <stdio.h>
#include <assert.h>
#include <sys/stat.h>

#include "opencdk.h"
#include "main.h"
#include "filters.h"


/* FIXME: We cannot use a static progress callback because the library
          might be MT. We need a progress cb per context. */


static off_t
fp_get_length (FILE *fp)
{
  struct stat statbuf;
    
  if (fstat( fileno (fp), &statbuf))
    return (off_t)-1;
  return statbuf.st_size;
}


static cdk_error_t
hash_encode (void *opaque, FILE *in, FILE *out)
{
  md_filter_t * mfx = opaque;
  byte buf[BUFSIZE];
  gcry_error_t err;
  int nread;
  
  if (!mfx)
    return CDK_Inv_Value;
  
  _cdk_log_debug( "hash filter: encode (algo=%d)\n", mfx->digest_algo );
  
  if (!mfx->md)
    {
      err = gcry_md_open (&mfx->md, mfx->digest_algo, 0);
      if (err)
	return map_gcry_error (err);
    }
  
  while (!feof (in))
    {
      nread = fread (buf, 1, BUFSIZE, in);
      if (!nread)
	break;
      gcry_md_write (mfx->md, buf, nread);
    }
  
  wipemem (buf, sizeof (buf));
  return 0;
}


cdk_error_t
_cdk_filter_hash (void *opaque, int ctl, FILE *in, FILE *out)
{
  if (ctl == STREAMCTL_READ)
    return hash_encode (opaque, in, out);
  else if (ctl == STREAMCTL_FREE)
    {
      md_filter_t *mfx = opaque;
      if (mfx) 
	{
	  _cdk_log_debug ("free hash filter\n");
	  gcry_md_close (mfx->md);
	  mfx->md = NULL;
	  return 0;
        }   
    }
    return CDK_Inv_Mode;
}


static cdk_error_t
write_header (cipher_filter_t *cfx, FILE *out)
{
  cdk_pkt_encrypted_t ed;
  cdk_packet_t pkt;
  cdk_dek_t dek = cfx->dek;
  byte temp[18];
  size_t blocksize;
  int use_mdc, nprefix;
  cdk_error_t rc = 0;
  gcry_error_t err;
  
  blocksize = gcry_cipher_get_algo_blklen( dek->algo );
  if (blocksize < 8 || blocksize > 16)
    return CDK_Inv_Algo;
  
  use_mdc = dek->use_mdc;
  if (blocksize != 8)
    use_mdc = 1; /* Enabled by default for all 128-bit block cipher */
  
  if (use_mdc && cfx->datalen)
    cfx->datalen += 22;
  
  cdk_pkt_alloc (&pkt, CDK_PKT_ENCRYPTED_MDC);
  ed = pkt->pkt.encrypted;
  if (!cfx->blkmode.on)
    {
      ed->len = cfx->datalen;
      ed->extralen = blocksize + 2;
    }
  else
    cfx->blkmode.nleft = BUFSIZE;
  
  if (use_mdc)
    {
      ed->mdc_method = GCRY_MD_SHA1;
      err = gcry_md_open (&cfx->mdc, GCRY_MD_SHA1, 0);
      if (err)
	return map_gcry_error (err);
    }
  
  pkt->old_ctb = cfx->dek->rfc1991 && !cfx->blkmode.on? 1 : 0;
  pkt->pkttype = use_mdc? CDK_PKT_ENCRYPTED_MDC : CDK_PKT_ENCRYPTED;
  rc = _cdk_pkt_write_fp (out, pkt);
  cdk_pkt_release (pkt);
  if (rc)
    return rc;
  
  nprefix = blocksize;
  gcry_randomize (temp, nprefix, GCRY_STRONG_RANDOM);
  temp[nprefix] = temp[nprefix - 2];
  temp[nprefix + 1] = temp[nprefix - 1];
  err = gcry_cipher_open (&cfx->hd, dek->algo, GCRY_CIPHER_MODE_CFB, 
			  use_mdc? 0 : GCRY_CIPHER_ENABLE_SYNC);
  if (err)
    return map_gcry_error (err);
  err = gcry_cipher_setiv (cfx->hd, NULL, 0);
  if (err)
    return map_gcry_error (err);
  err = gcry_cipher_setkey (cfx->hd, dek->key, dek->keylen);
  if (err)
    return map_gcry_error (err);
  if (cfx->mdc)
    gcry_md_write (cfx->mdc, temp, nprefix + 2);
  gcry_cipher_encrypt (cfx->hd, temp, nprefix + 2, NULL, 0);
  gcry_cipher_sync (cfx->hd);
  fwrite (temp, 1, nprefix+2, out);
  if (cfx->blkmode.on) 
    {
      cfx->blkmode.nleft -= (blocksize+2);
      if (use_mdc)
	cfx->blkmode.nleft -= (22+1); /* 22 mdc packet, 1 byte version */
    }
  return rc;
}


static cdk_error_t
write_mdc_packet (FILE *out, cipher_filter_t *cfx)
{
  byte pktdata[22];
  int dlen = gcry_md_get_algo_dlen (GCRY_MD_SHA1);

  if (!out || !cfx)
    return CDK_Inv_Value;  
  if (dlen != 20)
    return CDK_Inv_Algo;
  
  /* We must hash the prefix of the MDC packet here */
  pktdata[0] = 0xd3;
  pktdata[1] = 0x14;
  gcry_md_write (cfx->mdc, pktdata, 2);
  gcry_md_final (cfx->mdc);
  memcpy (pktdata + 2, gcry_md_read (cfx->mdc, GCRY_MD_SHA1), dlen);
  gcry_cipher_encrypt (cfx->hd, pktdata, dlen+2, NULL, 0);
  fwrite (pktdata, 1, dlen+2, out);
  wipemem (pktdata, sizeof (pktdata));
  return 0;
}
  

static __inline__ int
num2bits (size_t n)
{
  size_t i;
  
  if (!n)
    return -1;
  for (i = 0; n > 1; i++)
    n >>= 1;
  return i;
}


static __inline__ size_t
pow2 (size_t y)
{
  size_t x = 1, i;
  for (i = 0; i < y; i++)
    x <<= 1;
  return x; 
}


static cdk_error_t
write_partial_block (FILE *in, FILE *out, off_t *r_len,
                     cipher_filter_t *cfx )
{
    byte buf[BUFSIZE+1];
    size_t n;
    int nread;
    cdk_error_t rc;

    if( !out || !cfx )
        return CDK_Inv_Value;

    if( !cfx->blkmode.nleft ) {
        if( *r_len > BUFSIZE ) {
            fputc( (0xE0|DEF_BLOCKBITS), out );
            cfx->blkmode.nleft = BUFSIZE;
            (*r_len) -= BUFSIZE;
        }
        else if( *r_len > 512 ) {
            n = num2bits( *r_len );
            cfx->blkmode.nleft = pow2( n );
            fputc( (0xE0|n), out );
            (*r_len) -= cfx->blkmode.nleft ;
        }
        else {
            size_t pktlen = *r_len;
            if( pktlen < 192 )
                fputc( pktlen, out );
            else if( pktlen < 8384 ) {
                pktlen -= 192;
                fputc( (pktlen/256) + 192, out );
                fputc( (pktlen % 256), out );
            }
            cfx->blkmode.nleft = pktlen;
            (*r_len) -= pktlen;
        }
    }
    else
        (*r_len) -= cfx->blkmode.nleft;

    n = cfx->blkmode.nleft < DIM (buf)? cfx->blkmode.nleft : DIM (buf);
    nread = fread( buf, 1, n, in );
    if( !nread )
        return CDK_EOF;
    if( cfx->mdc )
        gcry_md_write( cfx->mdc, buf, nread );
    rc = gcry_cipher_encrypt (cfx->hd, buf, nread, NULL, 0);
    if( !rc ) {
        fwrite( buf, 1, nread, out );
        cfx->blkmode.nleft -= nread;
    }
    return rc;
}


static cdk_error_t
cipher_encode_file( void * opaque, FILE * in, FILE * out )
{
  cipher_filter_t * cfx = opaque;
  byte buf[BUFSIZE];
  off_t len, len2;
  int nread;
  cdk_error_t rc = 0;

  if (!cfx || !in || !out)
    return CDK_Inv_Value;

  len = len2 = fp_get_length (in);
  if (len == (off_t)-1)
    return CDK_File_Error;
  while (!feof (in)) 
    {
      if (cfx->blkmode.on)
	{
	  rc = write_partial_block (in, out, &len2, cfx);
	  if (rc)
	    break;
	  continue;
        }
      nread = fread (buf, 1, DIM (buf), in);
      if (!nread)
	break;
      if (cfx->mdc)
	gcry_md_write (cfx->mdc, buf, nread);
      gcry_cipher_encrypt (cfx->hd, buf, nread, NULL, 0);
      fwrite (buf, 1, nread, out);
    }
  wipemem (buf, sizeof (buf));
  if (!rc) 
    {
      if (cfx->mdc)
	rc = write_mdc_packet (out, cfx);
    }
  return rc;
}


static cdk_error_t
read_header (cipher_filter_t * cfx, FILE * in)
{
  cdk_dek_t dek;
  byte temp[32];
  int blocksize, nprefix;
  int i, c;
  gcry_error_t err;
  
  if (!cfx || !in)
    return CDK_Inv_Value;
  
  dek = cfx->dek;
  blocksize = gcry_cipher_get_algo_blklen (dek->algo);
  if (blocksize < 8 || blocksize > 16)
    return CDK_Inv_Algo;
  
  nprefix = blocksize;
  if (cfx->datalen > 0 && cfx->datalen < (nprefix + 2))
    return CDK_Inv_Value;
  if (cfx->mdc_method)
    {
      err = gcry_md_open (&cfx->mdc, cfx->mdc_method, 0);
      if (err)
	return map_gcry_error (err);
    }
  err = gcry_cipher_open (&cfx->hd, dek->algo, GCRY_CIPHER_MODE_CFB, 
			  cfx->mdc_method? 0 : GCRY_CIPHER_ENABLE_SYNC);
  if (err)
    return map_gcry_error (err);
  err = gcry_cipher_setiv (cfx->hd, NULL, 0);
  if (err)
    return map_gcry_error (err);
  err = gcry_cipher_setkey (cfx->hd, dek->key, dek->keylen);
  if (err)
    return map_gcry_error (err);
  
  for (i = 0; i < (nprefix + 2); i++ ) 
    {
      c = fgetc (in);
      if (c == EOF)
	return CDK_File_Error;
      temp[i] = c;
    }
  gcry_cipher_decrypt (cfx->hd, temp, nprefix + 2, NULL, 0);
  gcry_cipher_sync (cfx->hd);
  i = nprefix;
  if (temp[i - 2] != temp[i] || temp[i - 1] != temp[i + 1])
    return CDK_Chksum_Error;
  if (cfx->mdc)
    gcry_md_write (cfx->mdc, temp, nprefix + 2);
  if (cfx->blkmode.on)
    {
      cfx->blkmode.size -= (nprefix + 2);
      /* FIXME: to sub the MDC packet len (23) here seems to be wrong. */
    }
  return 0;
}


static cdk_error_t
finalize_mdc (gcry_md_hd_t md, const byte *buf, size_t nread)
{
  byte mdcbuf[20];
  int dlen = gcry_md_get_algo_dlen (GCRY_MD_SHA1);
  cdk_error_t rc;
  
  if (dlen != 20)
    return CDK_Inv_Algo;
  
  if (buf[nread - dlen - 2] == 0xD3 && buf[nread - dlen - 1] == 0x14)
    {
      gcry_md_write (md, buf, nread - dlen);
      gcry_md_final (md);
      memcpy (mdcbuf, gcry_md_read (md, GCRY_MD_SHA1), dlen);
      if (memcmp (mdcbuf, buf + nread - dlen, dlen))
	rc = CDK_Bad_MDC;
      else
	rc = CDK_Success;
      wipemem (mdcbuf, sizeof (mdcbuf));
      return rc;
    }
  
  return CDK_Inv_Packet;
}

  
static cdk_error_t
cipher_decode_file (void *opaque, FILE *in, FILE *out)
{
  cipher_filter_t *cfx = opaque;
  byte buf[BUFSIZE];
  int nread, nreq;
  cdk_error_t rc;

  if (!cfx || !in || !out)
    return CDK_Inv_Value;
  
  while (!feof (in)) 
    {
      _cdk_log_debug ("partial on=%d size=%lu\n",
		      cfx->blkmode.on, cfx->blkmode.size);
      nreq = cfx->blkmode.on? cfx->blkmode.size : DIM (buf);
      nread = fread (buf, 1, nreq, in);
      if (!nread)
	break;
      gcry_cipher_decrypt (cfx->hd, buf, nread, NULL, 0);
      if (feof (in) && cfx->mdc)
	{	  
	  rc = finalize_mdc (cfx->mdc, buf, nread);
	  if (rc)
	    {
	      wipemem (buf, sizeof (buf));
	      return rc;
	    }
	  /* We need to correct the size here to avoid the MDC
	     packet will be written to the output. */
	  nread -= 22;
	}     
      else if (cfx->mdc)
	gcry_md_write (cfx->mdc, buf, nread);
      fwrite (buf, 1, nread, out);
      if (cfx->blkmode.on) 
	{
	  cfx->blkmode.size = _cdk_pkt_read_len (in, &cfx->blkmode.on);
	  if (cfx->blkmode.size == (size_t)EOF)
	    return CDK_Inv_Packet;
        }
    }
  
  wipemem (buf, sizeof (buf));
  return 0;
}


static cdk_error_t
cipher_decode( void * opaque, FILE * in, FILE * out )
{
    cipher_filter_t * cfx = opaque;
    cdk_error_t rc;

    _cdk_log_debug( "cipher filter: decode\n" );
  
    if( !cfx || !in || !out )
        return CDK_Inv_Value;
  
    rc = read_header( cfx, in );
    if( !rc )
        rc = cipher_decode_file( cfx, in, out );
    return rc;
}
    

static cdk_error_t
cipher_encode( void * opaque, FILE * in, FILE * out )
{
    cipher_filter_t * cfx = opaque;
    cdk_error_t rc;

    _cdk_log_debug( "cipher filter: encode\n" );
  
    if( !cfx || !in || !out )
        return CDK_Inv_Value;

    cfx->datalen = fp_get_length (in);
    if (cfx->datalen < BUFSIZE && cfx->blkmode.on)
        cfx->blkmode.on = 0;
    rc = write_header (cfx, out);
    if( !rc )
        rc = cipher_encode_file (cfx, in, out);
    return rc;
}


cdk_error_t
_cdk_filter_cipher( void * opaque, int ctl, FILE * in, FILE * out )
{
    if( ctl == STREAMCTL_READ )
        return cipher_decode( opaque, in, out );
    else if( ctl == STREAMCTL_WRITE )
        return cipher_encode( opaque, in, out );
    else if( ctl == STREAMCTL_FREE ) {
        cipher_filter_t * cfx = opaque;
        if( cfx ) {
            _cdk_log_debug( "free cipher filter\n" );
            gcry_md_close( cfx->mdc );
            cfx->mdc = NULL;
            gcry_cipher_close( cfx->hd );
            cfx->hd = NULL;
        }
    }
    return CDK_Inv_Mode;
}

