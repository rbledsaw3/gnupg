/* mainproc.c - handle packets
 *	Copyright (c) 1997 by Werner Koch (dd9jn)
 *
 * This file is part of G10.
 *
 * G10 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * G10 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "packet.h"
#include "iobuf.h"
#include "memory.h"
#include "options.h"
#include "util.h"
#include "cipher.h"
#include "keydb.h"
#include "filter.h"
#include "cipher.h"
#include "main.h"
#include "status.h"


/****************
 * Structure to hold the context
 */

typedef struct {
    PKT_public_cert *last_pubkey;
    PKT_secret_cert *last_seckey;
    PKT_user_id     *last_user_id;
    md_filter_context_t mfx;
    DEK *dek;
    int last_was_pubkey_enc;
    KBNODE list;   /* the current list of packets */
    int have_data;
    IOBUF iobuf;    /* used to get the filename etc. */
} *CTX;



static void list_node( CTX c, KBNODE node );
static void proc_tree( CTX c, KBNODE node );


static void
release_list( CTX c )
{
    if( !c->list )
	return;
    proc_tree(c, c->list );
    release_kbnode( c->list );
    c->list = NULL;
}


static int
add_onepass_sig( CTX c, PACKET *pkt )
{
    KBNODE node;

    if( c->list ) { /* add another packet */
	if( c->list->pkt->pkttype != PKT_ONEPASS_SIG ) {
	   log_error("add_onepass_sig: another packet is in the way\n");
	   release_list( c );
	}
	add_kbnode( c->list, new_kbnode( pkt ));
    }
    else /* insert the first one */
	c->list = node = new_kbnode( pkt );

    return 1;
}


static int
add_public_cert( CTX c, PACKET *pkt )
{
    release_list( c );
    c->list = new_kbnode( pkt );
    return 1;
}

static int
add_secret_cert( CTX c, PACKET *pkt )
{
    release_list( c );
    c->list = new_kbnode( pkt );
    return 1;
}


static int
add_user_id( CTX c, PACKET *pkt )
{
    if( !c->list ) {
	log_error("orphaned user id\n" );
	return 0;
    }
    add_kbnode( c->list, new_kbnode( pkt ) );
    return 1;
}


static int
add_signature( CTX c, PACKET *pkt )
{
    KBNODE node;

    if( pkt->pkttype == PKT_SIGNATURE && !c->list ) {
	/* This is the first signature for a following datafile.
	 * G10 does not write such packets, instead it always uses
	 * onepass-sig packets.  The drawback of PGP's method
	 * of prepending the signtaure to the data is,
	 * that it is not possible to make a signature from data read
	 * from stdin.	(Anyway, G10 is able to read these stuff) */
	node = new_kbnode( pkt );
	c->list = node;
	return 1;
    }
    else if( !c->list )
	return 0; /* oops (invalid packet sequence)*/
    else if( !c->list->pkt )
	BUG();	/* so nicht */

    /* add a new signature node id at the end */
    node = new_kbnode( pkt );
    add_kbnode( c->list, node );
    return 1;
}


static void
proc_pubkey_enc( CTX c, PACKET *pkt )
{
    PKT_pubkey_enc *enc;
    int result = 0;

    c->last_was_pubkey_enc = 1;
    enc = pkt->pkt.pubkey_enc;
    /*printf("enc: encrypted by a pubkey with keyid %08lX\n", enc->keyid[1] );*/
    if( enc->pubkey_algo == PUBKEY_ALGO_ELGAMAL
	|| enc->pubkey_algo == PUBKEY_ALGO_RSA	) {
	m_free(c->dek ); /* paranoid: delete a pending DEK */
	c->dek = m_alloc_secure( sizeof *c->dek );
	if( (result = get_session_key( enc, c->dek )) ) {
	    /* error: delete the DEK */
	    m_free(c->dek); c->dek = NULL;
	}
    }
    else
	result = G10ERR_PUBKEY_ALGO;

    if( result == -1 )
	;
    else if( !result ) {
	if( opt.verbose > 1 )
	    log_info( "pubkey_enc packet: Good DEK\n" );
    }
    else
	log_error( "pubkey_enc packet: %s\n", g10_errstr(result));
    free_packet(pkt);
}



static void
proc_encrypted( CTX c, PACKET *pkt )
{
    int result = 0;

    /*printf("dat: %sencrypted data\n", c->dek?"":"conventional ");*/
    if( !c->dek && !c->last_was_pubkey_enc ) {
	/* assume this is conventional encrypted data */
	c->dek = m_alloc_secure( sizeof *c->dek );
	c->dek->algo = opt.def_cipher_algo;
	result = make_dek_from_passphrase( c->dek, 0 );
    }
    else if( !c->dek )
	result = G10ERR_NO_SECKEY;
    if( !result )
	result = decrypt_data( pkt->pkt.encrypted, c->dek );
    m_free(c->dek); c->dek = NULL;
    if( result == -1 )
	;
    else if( !result ) {
	if( opt.verbose > 1 )
	    log_info("encryption okay\n");
    }
    else {
	log_error("encryption failed: %s\n", g10_errstr(result));
    }
    free_packet(pkt);
    c->last_was_pubkey_enc = 0;
}


static void
proc_plaintext( CTX c, PACKET *pkt )
{
    PKT_plaintext *pt = pkt->pkt.plaintext;
    int rc;

    if( opt.verbose )
	log_info("original file name='%.*s'\n", pt->namelen, pt->name);
    free_md_filter_context( &c->mfx );
    /* fixme: take the digest algo(s) to use from the
     * onepass_sig packet (if we have these)
     * And look at the sigclass to check wether we should use the
     * textmode filter (sigclass 0x01)
     */
    c->mfx.md = md_open( DIGEST_ALGO_RMD160, 0);
    md_enable( c->mfx.md, DIGEST_ALGO_MD5 );
    rc = handle_plaintext( pt, &c->mfx );
    if( rc )
	log_error( "handle plaintext failed: %s\n", g10_errstr(rc));
    free_packet(pkt);
    c->last_was_pubkey_enc = 0;
}


static void
proc_compressed( CTX c, PACKET *pkt )
{
    PKT_compressed *zd = pkt->pkt.compressed;
    int rc;

    /*printf("zip: compressed data packet\n");*/
    rc = handle_compressed( zd );
    if( rc )
	log_error("uncompressing failed: %s\n", g10_errstr(rc));
    free_packet(pkt);
    c->last_was_pubkey_enc = 0;
}




/****************
 * check the signature
 * Returns: 0 = valid signature or an error code
 */
static int
do_check_sig( CTX c, KBNODE node )
{
    PKT_signature *sig;
    MD_HANDLE md;
    int algo, rc;

    assert( node->pkt->pkttype == PKT_SIGNATURE );
    sig = node->pkt->pkt.signature;

    if( sig->pubkey_algo == PUBKEY_ALGO_ELGAMAL )
	algo = sig->d.elg.digest_algo;
    else if(sig->pubkey_algo == PUBKEY_ALGO_RSA )
	algo = sig->d.rsa.digest_algo;
    else
	return G10ERR_PUBKEY_ALGO;
    if( (rc=check_digest_algo(algo)) )
	return rc;

    if( sig->sig_class == 0x00 ) {
	md = md_copy( c->mfx.md );
    }
    else if( sig->sig_class == 0x01 ) {
	/* how do we know that we have to hash the (already hashed) text
	 * in canonical mode ??? (calculating both modes???) */
	md = md_copy( c->mfx.md );
    }
    else if( (sig->sig_class&~3) == 0x10 ) { /* classes 0x10 .. 0x13 */
	if( c->list->pkt->pkttype == PKT_PUBLIC_CERT ) {
	    KBNODE n1 = find_prev_kbnode( c->list, node, PKT_USER_ID );

	    if( n1 ) {
		if( c->list->pkt->pkt.public_cert->mfx.md )
		    md = md_copy( c->list->pkt->pkt.public_cert->mfx.md );
		else
		    BUG();
		md_write( md, n1->pkt->pkt.user_id->name, n1->pkt->pkt.user_id->len);
	    }
	    else {
		log_error("invalid parent packet for sigclass 0x10\n");
		return G10ERR_SIG_CLASS;
	    }
	}
	else {
	    log_error("invalid root packet for sigclass 0x10\n");
	    return G10ERR_SIG_CLASS;
	}
    }
    else
	return G10ERR_SIG_CLASS;
    rc = signature_check( sig, md );
    md_close(md);

    return rc;
}



static void
print_userid( PACKET *pkt )
{
    if( !pkt )
	BUG();
    if( pkt->pkttype != PKT_USER_ID ) {
	printf("ERROR: unexpected packet type %d", pkt->pkttype );
	return;
    }
    print_string( stdout,  pkt->pkt.user_id->name, pkt->pkt.user_id->len );
}


static void
print_fingerprint( PKT_public_cert *pkc, PKT_secret_cert *skc )
{
    byte *array, *p;
    size_t i, n;

    p = array = skc? fingerprint_from_skc( skc, &n )
		   : fingerprint_from_pkc( pkc, &n );
    printf("     Key fingerprint =");
    if( n == 20 ) {
	for(i=0; i < n ; i++, i++, p += 2 ) {
	    if( i == 10 )
		putchar(' ');
	    printf(" %02X%02X", *p, p[1] );
	}
    }
    else {
	for(i=0; i < n ; i++, p++ ) {
	    if( i && !(i%8) )
		putchar(' ');
	    printf(" %02X", *p );
	}
    }
    putchar('\n');
    m_free(array);
}


/****************
 * List the certificate in a user friendly way
 */

static void
list_node( CTX c, KBNODE node )
{
    int any=0;

    if( !node )
	;
    else if( node->pkt->pkttype == PKT_PUBLIC_CERT ) {
	PKT_public_cert *pkc = node->pkt->pkt.public_cert;

	printf("pub  %4u%c/%08lX %s ", nbits_from_pkc( pkc ),
				      pubkey_letter( pkc->pubkey_algo ),
				      (ulong)keyid_from_pkc( pkc, NULL ),
				      datestr_from_pkc( pkc )	  );
	/* and now list all userids with their signatures */
	for( node = node->next; node; node = node->next ) {
	    if( node->pkt->pkttype == PKT_USER_ID ) {
		KBNODE n;

		if( any )
		    printf( "%*s", 31, "" );
		print_userid( node->pkt );
		putchar('\n');
		if( opt.fingerprint && !any )
		    print_fingerprint( pkc, NULL );
		for( n=node->next; n; n = n->next ) {
		    if( n->pkt->pkttype == PKT_USER_ID )
			break;
		    if( n->pkt->pkttype == PKT_SIGNATURE )
			list_node(c,  n );
		}
		any=1;
	    }
	}
	if( !any )
	    printf("ERROR: no user id!\n");
    }
    else if( node->pkt->pkttype == PKT_SECRET_CERT ) {
	PKT_secret_cert *skc = node->pkt->pkt.secret_cert;

	printf("sec  %4u%c/%08lX %s ", nbits_from_skc( skc ),
				      pubkey_letter( skc->pubkey_algo ),
				      (ulong)keyid_from_skc( skc, NULL ),
				      datestr_from_skc( skc )	  );
	/* and now list all userids */
	while( (node = find_next_kbnode(node, PKT_USER_ID)) ) {
	    print_userid( node->pkt );
	    putchar('\n');
	    if( opt.fingerprint && !any )
		print_fingerprint( NULL, skc );
	    any=1;
	}
	if( !any )
	    printf("ERROR: no user id!\n");
    }
    else if( node->pkt->pkttype == PKT_SIGNATURE  ) {
	PKT_signature *sig = node->pkt->pkt.signature;
	int rc2=0;
	size_t n;
	char *p;
	int sigrc = ' ';

	if( !opt.list_sigs )
	    return;

	fputs("sig", stdout);
	if( opt.check_sigs ) {
	    fflush(stdout);
	    switch( (rc2=do_check_sig( c, node )) ) {
	      case 0:		       sigrc = '!'; break;
	      case G10ERR_BAD_SIGN:    sigrc = '-'; break;
	      case G10ERR_NO_PUBKEY:   sigrc = '?'; break;
	      default:		       sigrc = '%'; break;
	    }
	}
	printf("%c       %08lX %s   ",
		sigrc, (ulong)sig->keyid[1], datestr_from_sig(sig));
	if( sigrc == '%' )
	    printf("[%s] ", g10_errstr(rc2) );
	else if( sigrc == '?' )
	    ;
	else {
	    p = get_user_id( sig->keyid, &n );
	    print_string( stdout, p, n );
	    m_free(p);
	}
	putchar('\n');
    }
    else
	log_error("invalid node with packet of type %d\n", node->pkt->pkttype);
}


int
proc_packets( IOBUF a )
{
    CTX c = m_alloc_clear( sizeof *c );
    PACKET *pkt = m_alloc( sizeof *pkt );
    int rc;
    int newpkt;

    c->iobuf = a;
    init_packet(pkt);
    while( (rc=parse_packet(a, pkt)) != -1 ) {
	/* cleanup if we have an illegal data structure */
	if( c->dek && pkt->pkttype != PKT_ENCRYPTED ) {
	    log_error("oops: valid pubkey enc packet not followed by data\n");
	    m_free(c->dek); c->dek = NULL; /* burn it */
	}

	if( rc ) {
	    free_packet(pkt);
	    if( rc == G10ERR_INVALID_PACKET )
		break;
	    continue;
	}
	newpkt = -1;
	if( opt.list_packets ) {
	    switch( pkt->pkttype ) {
	      case PKT_PUBKEY_ENC:  proc_pubkey_enc( c, pkt ); break;
	      case PKT_ENCRYPTED:   proc_encrypted( c, pkt ); break;
	      case PKT_COMPRESSED:  proc_compressed( c, pkt ); break;
	      default: newpkt = 0; break;
	    }
	}
	else {
	    switch( pkt->pkttype ) {
	      case PKT_PUBLIC_CERT: newpkt = add_public_cert( c, pkt ); break;
	      case PKT_SECRET_CERT: newpkt = add_secret_cert( c, pkt ); break;
	      case PKT_USER_ID:     newpkt = add_user_id( c, pkt ); break;
	      case PKT_SIGNATURE:   newpkt = add_signature( c, pkt ); break;
	      case PKT_PUBKEY_ENC:  proc_pubkey_enc( c, pkt ); break;
	      case PKT_ENCRYPTED:   proc_encrypted( c, pkt ); break;
	      case PKT_PLAINTEXT:   proc_plaintext( c, pkt ); break;
	      case PKT_COMPRESSED:  proc_compressed( c, pkt ); break;
	      case PKT_ONEPASS_SIG: newpkt = add_onepass_sig( c, pkt ); break;
	      default: newpkt = 0; break;
	    }
	}
	if( pkt->pkttype != PKT_SIGNATURE )
	    c->have_data = pkt->pkttype == PKT_PLAINTEXT;

	if( newpkt == -1 )
	    ;
	else if( newpkt ) {
	    pkt = m_alloc( sizeof *pkt );
	    init_packet(pkt);
	}
	else
	    free_packet(pkt);
    }

    release_list( c );
    m_free(c->dek);
    free_packet( pkt );
    m_free( pkt );
    free_md_filter_context( &c->mfx );
    m_free( c );
    return 0;
}


static void
print_keyid( FILE *fp, u32 *keyid )
{
    size_t n;
    char *p = get_user_id( keyid, &n );
    print_string( fp, p, n );
    m_free(p);
}



static int
check_sig_and_print( CTX c, KBNODE node )
{
    PKT_signature *sig = node->pkt->pkt.signature;
    int rc;

    rc = do_check_sig(c, node );
    if( !rc ) {
	write_status( STATUS_GOODSIG );
	log_info("Good signature from ");
	print_keyid( stderr, sig->keyid );
	putc('\n', stderr);
    }
    else if( rc == G10ERR_BAD_SIGN ) {
	write_status( STATUS_BADSIG );
	log_error("BAD signature from ");
	print_keyid( stderr, sig->keyid );
	putc('\n', stderr);
	if( opt.batch )
	    g10_exit(1);
    }
    else {
	write_status( STATUS_ERRSIG );
	log_error("Can't check signature made by %08lX: %s\n",
		   (ulong)sig->keyid[1], g10_errstr(rc) );
    }
    return rc;
}


/****************
 * Process the tree which starts at node
 */
static void
proc_tree( CTX c, KBNODE node )
{
    KBNODE n1;
    int rc;

    if( opt.list_packets )
	return;

    if( node->pkt->pkttype == PKT_PUBLIC_CERT )
	list_node( c, node );
    else if( node->pkt->pkttype == PKT_SECRET_CERT )
	list_node( c, node );
    else if( node->pkt->pkttype == PKT_ONEPASS_SIG ) {
	/* check all signatures */
	if( !c->have_data ) {
	    free_md_filter_context( &c->mfx );
	    /* prepare to create all requested message digests */
	    c->mfx.md = md_open(0, 0);
	    for( n1 = node; (n1 = find_next_kbnode(n1, PKT_SIGNATURE )); ) {
		md_enable( c->mfx.md,
			   digest_algo_from_sig(n1->pkt->pkt.signature));
	    }
	    /* ask for file and hash it */
	    rc = ask_for_detached_datafile( &c->mfx,
					    iobuf_get_fname(c->iobuf));
	    if( rc ) {
		log_error("can't hash datafile: %s\n", g10_errstr(rc));
		return;
	    }
	}

	for( n1 = node; (n1 = find_next_kbnode(n1, PKT_SIGNATURE )); )
	    check_sig_and_print( c, n1 );
    }
    else if( node->pkt->pkttype == PKT_SIGNATURE ) {
	PKT_signature *sig = node->pkt->pkt.signature;

	if( !c->have_data && (sig->sig_class&~3) == 0x10 ) {
	    log_info("old style signature\n");
	    if( !c->have_data ) {
		free_md_filter_context( &c->mfx );
		c->mfx.md = md_open(digest_algo_from_sig(sig), 0);
		rc = ask_for_detached_datafile( &c->mfx,
						iobuf_get_fname(c->iobuf));
		if( rc ) {
		    log_error("can't hash datafile: %s\n", g10_errstr(rc));
		    return;
		}
	    }
	}

	check_sig_and_print( c, node );
    }
    else
	log_error("proc_tree: invalid root packet\n");

}



