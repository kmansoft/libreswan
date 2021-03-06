/* demultiplex incoming IKE messages
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2010,2013-2017 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2007-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2008-2011 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2010 Simon Deziel <simon@xelerance.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2011-2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2012-2017 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2015-2018 Andrew Cagney
 * Copyright (C) 2016-2018 Antony Antony <appu@phenome.org>
 * Copyright (C) 2017 Sahana Prasad <sahana.prasad07@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <libreswan.h>

#include "sysdep.h"
#include "constants.h"
#include "lswlog.h"

#include "defs.h"
#include "id.h"
#include "x509.h"
#include "pluto_x509.h"
#include "certs.h"
#include "connections.h"        /* needs id.h */
#include "state.h"
#include "packet.h"
#include "crypto.h"
#include "crypt_symkey.h"
#include "ike_alg.h"
#include "log.h"
#include "demux.h"      /* needs packet.h */
#include "pluto_crypt.h"  /* for pluto_crypto_req & pluto_crypto_req_cont */
#include "ikev2.h"
#include "ipsec_doi.h"  /* needs demux.h and state.h */
#include "timer.h"
#include "whack.h"      /* requires connections.h */
#include "server.h"
#include "spdb.h"
#include "nat_traversal.h"
#include "vendor.h"
#include "ip_address.h"
#include "ikev2_send.h"
#include "alg_info.h" /* for ike_info / esp_info */
#include "state_db.h"
#include "ietf_constants.h"
#include "ikev2_cookie.h"
#include "plutoalg.h" /* for default_ike_groups */
#include "ikev2_message.h"	/* for ikev2_decrypt_msg() */
#include "pluto_stats.h"
#include "keywords.h"

static bool is_msg_request(const struct msg_digest *md);

enum smf2_flags {
	/*
	 * Check the value of the I(Initiator) (IKE_I) flag in the
	 * header.
	 *
	 * The original initiator receives packets with the
	 * I(Initiator) bit clear, while the original resonder
	 * receives packets with the I(Initiator) bit set.
	 *
	 * The bit is used to identify the IKE SA initiator and
	 * responder SPIs (cookies) in the header (see 2.6. IKE SA
	 * SPIs and Cookies).  For incoming messages, the I(Initiator)
	 * flag in the header is used; for outgoing messages, the
	 * I(Initiator) flag is set according to ike.sa.st_sa_role.
	 *
	 * Arguably, this could be made a separate 3 state variable.
	 */
	SMF2_IKE_I_SET = LELEM(1),
	SMF2_IKE_I_CLEAR = LELEM(2),

	SMF2_SEND = LELEM(3),

	/*
	 * Is the MSG_R bit set.
	 *
	 * Requests have the bit clear, and responses have it set.
	 *
	 * Don't assume one of these flags are present.  Some state
	 * processors internally deal with both the request and the
	 * reply.
	 *
	 * In general, the relationship MSG_R != IKE_I does not hold
	 * (it just holds during the initial exchange).
	 */
	SMF2_MSG_R_SET = LELEM(5),
	SMF2_MSG_R_CLEAR = LELEM(6),

	/*
	 * Should the SK (secured-by-key) decryption and verification
	 * be skipped?
	 *
	 * The original responder, when it receives the encrypted AUTH
	 * payload, isn't yet ready to decrypt it - receiving the
	 * packet is what triggers the DH calculation needed before
	 * encryption can occur.
	 */
	SMF2_NO_SKEYSEED = LELEM(7),
};

/*
 * IKEv2 has slightly different states than IKEv1.
 *
 * IKEv2 puts all the responsibility for retransmission on the end that
 * wants to do something, usually, that the initiator. (But, not always
 * the original initiator, of the responder decides it needs to rekey first)
 *
 * Each exchange has a bit that indicates if it is an Initiator message,
 * or if it is a response.  The Responder never retransmits its messages
 * except in response to an Initiator retransmission.
 *
 * The message ID is *NOT* used in the cryptographic state at all, but instead
 * serves the role of a sequence number.  This makes the state machine far
 * simpler, and there really are no exceptions.
 *
 * The upper level state machine is therefore much simpler.
 * The lower level takes care of retransmissions, and the upper layer state
 * machine just has to worry about whether it needs to go into cookie mode,
 * etc.
 *
 * Like IKEv1, IKEv2 can have multiple child SAs.  Like IKEv1, each one of
 * the child SAs ("Phase 2") will get their own state. Unlike IKEv1,
 * an implementation may negotiate multiple CHILD_SAs at the same time
 * using different MessageIDs.  This is enabled by an option (a notify)
 * that the responder sends to the initiator.  The initiator may only
 * do concurrent negotiations if it sees the notify.
 *
 * XXX This implementation does not support concurrency, but it shouldn't be
 *     that hard to do.  The most difficult part will be to map the message IDs
 *     to the right state. Some CHILD_SAs may take multiple round trips,
 *     and each one will have to be mapped to the same state.
 *
 * The IKEv2 state values are chosen from the same state space as IKEv1.
 *
 */

/*
 * From RFC 5996 syntax: [optional] and {encrypted}
 *
 * Initiator                         Responder
 * -------------------------------------------------------------------
 *
 * IKE_SA_INIT exchange (initial exchange):
 *
 * HDR, SAi1, KEi, Ni            -->
 *                                 <--  HDR, SAr1, KEr, Nr, [CERTREQ]
 *
 * IKE_AUTH exchange (after IKE_SA_INIT exchange):
 *
 * HDR, SK {IDi, [CERT,] [CERTREQ,]
 *        [IDr,] AUTH, SAi2,
 *        TSi, TSr}              -->
 *                                 <--  HDR, SK {IDr, [CERT,] AUTH,
 *                                           SAr2, TSi, TSr}
 * [Parent SA (SAx1) established. Child SA (SAx2) may have been established]
 *
 *
 * Extended IKE_AUTH (see RFC 5996bis 2.6):
 *
 * HDR(A,0), SAi1, KEi, Ni  -->
 *                              <--  HDR(A,0), N(COOKIE)
 * HDR(A,0), N(COOKIE), SAi1,
 *     KEi, Ni  -->
 *                              <--  HDR(A,B), SAr1, KEr,
 *                                       Nr, [CERTREQ]
 * HDR(A,B), SK {IDi, [CERT,]
 *     [CERTREQ,] [IDr,] AUTH,
 *     SAi2, TSi, TSr}  -->
 *                              <--  HDR(A,B), SK {IDr, [CERT,]
 *                                       AUTH, SAr2, TSi, TSr}
 * [Parent SA (SAx1) established. Child SA (SAx2) may have been established]
 *
 *
 * CREATE_CHILD_SA Exchange (new child variant RFC 5996 1.3.1):
 *
 * HDR, SK {SA, Ni, [KEi],
 *            TSi, TSr}  -->
 *                              <--  HDR, SK {SA, Nr, [KEr],
 *                                       TSi, TSr}
 *
 *
 * CREATE_CHILD_SA Exchange (rekey child variant RFC 5996 1.3.3):
 *
 * HDR, SK {N(REKEY_SA), SA, Ni, [KEi],
 *     TSi, TSr}   -->
 *                    <--  HDR, SK {SA, Nr, [KEr],
 *                             TSi, TSr}
 *
 *
 * CREATE_CHILD_SA Exchange (rekey parent SA variant RFC 5996 1.3.2):
 *
 * HDR, SK {SA, Ni, KEi} -->
 *                            <--  HDR, SK {SA, Nr, KEr}
 */

/* Short forms for building payload type sets */

#define P(N) LELEM(ISAKMP_NEXT_v2##N)

/* From RFC 5996:
 *
 * 3.10 "Notify Payload": N payload may appear in any message
 *
 *      During the initial exchange (SA_INIT) (i.e., DH has been
 *      established) the notify payload can't be encrypted.  For all
 *      other exchanges it should be part of the SK (encrypted)
 *      payload (but beware the DH failure exception).
 *
 * 3.11 "Delete Payload": multiple D payloads may appear in an
 *	Informational exchange
 *
 * 3.12 "Vendor ID Payload": (multiple) may appear in any message
 *
 *      During the initial exchange (SA_INIT) (i.e., DH has been
 *      established) the vendor payload can't be encrypted.  For all
 *      other exchanges it should be part of the SK (encrypted)
 *      payload (but beware the DH failure exception).
 *
 * 3.15 "Configuration Payload":
 * 1.4 "The INFORMATIONAL Exchange": (multiple) Configuration Payloads
 *	may appear in an Informational exchange
 * 2.19 "Requesting an Internal Address on a Remote Network":
 *	In all cases, the CP payload MUST be inserted before the SA payload.
 *	In variations of the protocol where there are multiple IKE_AUTH
 *	exchanges, the CP payloads MUST be inserted in the messages
 *	containing the SA payloads.
 */

static const lset_t everywhere_payloads = P(N) | P(V);	/* can appear in any packet */
static const lset_t repeatable_payloads = P(N) | P(D) | P(CP) | P(V) | P(CERT) | P(CERTREQ);	/* if one can appear, many can appear */

/*
 * IKEv2 State transitions (aka microcodes).
 *
 * This table contains all possible state transitions, some of which
 * involve a message.
 *
 * During initialization this table parsed populating the
 * corresponding IKEv2 finite states.  While not the most efficient,
 * it seems to work.
 */

static /*const*/ struct state_v2_microcode v2_state_microcode_table[] = {

	/* no state:   --> CREATE_CHILD IKE Rekey Request
	 * HDR, SAi, KEi, Ni -->
	 */

	{ .story      = "Initiate CREATE_CHILD_SA IKE Rekey",
	  .state      = STATE_V2_REKEY_IKE_I0,
	  .next_state = STATE_V2_REKEY_IKE_I,
	  .flags      = SMF2_IKE_I_CLEAR | SMF2_MSG_R_SET | SMF2_SEND,
	  .processor  = NULL,
	  .timeout_event = EVENT_RETRANSMIT, },

	/* no state:   --> CREATE IPsec Rekey Request
	 * HDR, SAi1, N(REKEY_SA), {KEi,} Ni TSi TSr -->
	 */
	{ .story      = "Initiate CREATE_CHILD_SA IPsec Rekey SA",
	  .state      = STATE_V2_REKEY_CHILD_I0,
	  .next_state = STATE_V2_REKEY_CHILD_I,
	  .flags =      SMF2_IKE_I_CLEAR | SMF2_MSG_R_SET | SMF2_SEND,
	  .processor  = NULL,
	  .timeout_event = EVENT_RETRANSMIT, },

	/* no state:   --> CREATE IPsec Child Request
	 * HDR, SAi1, {KEi,} Ni TSi TSr -->
	 */
	{ .story      = "Initiate CREATE_CHILD_SA IPsec SA",
	  .state      = STATE_V2_CREATE_I0,
	  .next_state = STATE_V2_CREATE_I,
	  .flags =      SMF2_IKE_I_CLEAR | SMF2_MSG_R_SET | SMF2_SEND,
	  .processor  = NULL,
	  .timeout_event = EVENT_RETRANSMIT, },

	/* no state:   --> I1
	 * HDR, SAi1, KEi, Ni -->
	 */
	{ .story      = "initiate IKE_SA_INIT",
	  .state      = STATE_PARENT_I0,
	  .next_state = STATE_PARENT_I1,
	  .flags      = SMF2_IKE_I_CLEAR | SMF2_MSG_R_SET | SMF2_SEND,
	  .processor  = NULL,
	  .timeout_event = EVENT_RETRANSMIT, },

	/* STATE_PARENT_I1: R1B --> I1B
	 *                     <--  HDR, N
	 * HDR, N, SAi1, KEi, Ni -->
	 */
	{ .story      = "Initiator: process SA_INIT reply notification",
	  .state      = STATE_PARENT_I1,
	  .next_state = STATE_PARENT_I1,
	  .flags = SMF2_IKE_I_CLEAR | SMF2_MSG_R_SET | SMF2_SEND,
	  .req_clear_payloads = P(N),
	  .opt_clear_payloads = LEMPTY,
	  .processor = ikev2_IKE_SA_process_SA_INIT_response_notification,
	  .recv_type  = ISAKMP_v2_IKE_SA_INIT,
	  .timeout_event = EVENT_RETAIN, },

	/* STATE_PARENT_I1: R1 --> I2
	 *                     <--  HDR, SAr1, KEr, Nr, [CERTREQ]
	 * HDR, SK {IDi, [CERT,] [CERTREQ,]
	 *      [IDr,] AUTH, SAi2,
	 *      TSi, TSr}      -->
	 */
	{ .story      = "Initiator: process IKE_SA_INIT reply, initiate IKE_AUTH",
	  .state      = STATE_PARENT_I1,
	  .next_state = STATE_PARENT_I2,
	  .flags = SMF2_IKE_I_CLEAR | SMF2_MSG_R_SET | SMF2_SEND,
	  .req_clear_payloads = P(SA) | P(KE) | P(Nr),
	  .opt_clear_payloads = P(CERTREQ),
	  .processor  = ikev2_parent_inR1outI2,
	  .recv_type  = ISAKMP_v2_IKE_SA_INIT,
	  .timeout_event = EVENT_RETRANSMIT, },

	/* STATE_PARENT_I2: R2 -->
	 *                     <--  HDR, SK {IDr, [CERT,] AUTH,
	 *                               SAr2, TSi, TSr}
	 * [Parent SA established]
	 */
	{ .story      = "Initiator: process INVALID_SYNTAX AUTH notification",
	  .state      = STATE_PARENT_I2, .next_state = STATE_PARENT_I2,
	  .flags = SMF2_IKE_I_CLEAR | SMF2_MSG_R_SET,
	  .message_payloads = { .required = P(SK), },
	  .encrypted_payloads = { .required = P(N), .notification = v2N_INVALID_SYNTAX, },
	  .processor  = ikev2_auth_initiator_process_failure_notification,
	  .recv_type  = ISAKMP_v2_IKE_AUTH, },
	{ .story      = "Initiator: process AUTHENTICATION_FAILED AUTH notification",
	  .state      = STATE_PARENT_I2, .next_state = STATE_PARENT_I2,
	  .flags = SMF2_IKE_I_CLEAR | SMF2_MSG_R_SET,
	  .message_payloads = { .required = P(SK), },
	  .encrypted_payloads = { .required = P(N), .notification = v2N_AUTHENTICATION_FAILED, },
	  .processor  = ikev2_auth_initiator_process_failure_notification,
	  .recv_type  = ISAKMP_v2_IKE_AUTH, },
	{ .story      = "Initiator: process UNSUPPORTED_CRITICAL_PAYLOAD AUTH notification",
	  .state      = STATE_PARENT_I2, .next_state = STATE_PARENT_I2,
	  .flags = SMF2_IKE_I_CLEAR | SMF2_MSG_R_SET,
	  .message_payloads = { .required = P(SK), },
	  .encrypted_payloads = { .required = P(N), .notification = v2N_UNSUPPORTED_CRITICAL_PAYLOAD, },
	  .processor  = ikev2_auth_initiator_process_failure_notification,
	  .recv_type  = ISAKMP_v2_IKE_AUTH, },
	/*
	 * XXX: Danger! This state transition mashes the IKE SA's
	 * initial state and the CHILD SA's final state.  There should
	 * instead be two separate state transitions: IKE SA:
	 * STATE_PARENT_I2 -> STATE_PARENT_I3; CHILD SA: ??? ->
	 * STATE_V2_IPSEC_I->???.  The IKE SA could then initiate the
	 * CHILD SA's transaction.
	 */
	{ .story      = "Initiator: process IKE_AUTH response",
	  .state      = STATE_PARENT_I2,
	  .next_state = STATE_V2_IPSEC_I,
	  .flags = SMF2_IKE_I_CLEAR | SMF2_MSG_R_SET,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(IDr) | P(AUTH) | P(SA) | P(TSi) | P(TSr),
	  .opt_enc_payloads = P(CERT)|P(CP),
	  .processor  = ikev2_parent_inR2,
	  .recv_type  = ISAKMP_v2_IKE_AUTH,
	  .timeout_event = EVENT_SA_REPLACE, },
	{ .story      = "IKE SA: process IKE_AUTH response containing unknown notification",
	  .state      = STATE_PARENT_I2, .next_state = STATE_PARENT_I2,
	  .flags = SMF2_IKE_I_CLEAR | SMF2_MSG_R_SET,
	  .message_payloads = { .required = P(SK), },
	  .encrypted_payloads = { .required = P(N), },
	  .processor  = ikev2_auth_initiator_process_unknown_notification,
	  .recv_type  = ISAKMP_v2_IKE_AUTH, },

	/* no state: none I1 --> R1
	 *                <-- HDR, SAi1, KEi, Ni
	 * HDR, SAr1, KEr, Nr, [CERTREQ] -->
	 */
	{ .story      = "Respond to IKE_SA_INIT",
	  .state      = STATE_PARENT_R0,
	  .next_state = STATE_PARENT_R1,
	  .flags = SMF2_IKE_I_SET | SMF2_MSG_R_CLEAR | SMF2_SEND,
	  .req_clear_payloads = P(SA) | P(KE) | P(Ni),
	  .processor  = ikev2_parent_inI1outR1,
	  .recv_type  = ISAKMP_v2_IKE_SA_INIT,
	  .timeout_event = EVENT_SO_DISCARD, },

	/* STATE_PARENT_R1: I2 --> R2
	 *                  <-- HDR, SK {IDi, [CERT,] [CERTREQ,]
	 *                             [IDr,] AUTH, SAi2,
	 *                             TSi, TSr}
	 * HDR, SK {IDr, [CERT,] AUTH,
	 *      SAr2, TSi, TSr} -->
	 *
	 * [Parent SA established]
	 */
	{ .story      = "Responder: process IKE_AUTH request (no SKEYSEED)",
	  .state      = STATE_PARENT_R1,
	  .next_state = STATE_PARENT_R1,
	  .flags = SMF2_IKE_I_SET | SMF2_MSG_R_CLEAR | SMF2_SEND | SMF2_NO_SKEYSEED,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = LEMPTY,
	  .opt_enc_payloads = LEMPTY,
	  .processor  = ikev2_ike_sa_process_auth_request_no_skeyid,
	  .recv_type  = ISAKMP_v2_IKE_AUTH,
	  .timeout_event = EVENT_SA_REPLACE, },
	/*
	 * XXX: Danger! This state transition mashes the IKE SA's
	 * initial state and the CHILD SA's final state.  There should
	 * instead be two separate state transitions: IKE SA:
	 * STATE_PARENT_R1->STATE_PARENT_R2; CHILD SA::
	 * ???->STATE_V2_IPSEC_R.  The IKE SA could then initiate the
	 * CHILD SA's transaction.
	 */
	{ .story      = "Responder: process IKE_AUTH request",
	  .state      = STATE_PARENT_R1,
	  .next_state = STATE_V2_IPSEC_R,
	  .flags = SMF2_IKE_I_SET | SMF2_MSG_R_CLEAR | SMF2_SEND,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(IDi) | P(AUTH) | P(SA) | P(TSi) | P(TSr),
	  .opt_enc_payloads = P(CERT) | P(CERTREQ) | P(IDr) | P(CP),
	  .processor  = ikev2_ike_sa_process_auth_request,
	  .recv_type  = ISAKMP_v2_IKE_AUTH,
	  .timeout_event = EVENT_SA_REPLACE, },

	/*
	 * There are three different CREATE_CHILD_SA's invocations,
	 * this is the combined write up (not in RFC). See above for
	 * individual cases from RFC
	 *
	 * HDR, SK {SA, Ni, [KEi], [N(REKEY_SA)], [TSi, TSr]} -->
	 *                <-- HDR, SK {N}
	 *                <-- HDR, SK {SA, Nr, [KEr], [TSi, TSr]}
	 */

	/*
	 * Create Child SA Exchange to rekey IKE SA
	 * no state:   --> REKEY_IKE_R
	 * HDR, SAi1, KEi, Ni -->
	 *		<-- HDR, SAr1, KEr, Nr
	 */
	{ .story      = "Respond to CREATE_CHILD_SA IKE Rekey",
	  .state      = STATE_V2_REKEY_IKE_R,
	  .next_state = STATE_PARENT_R2,
	  .flags      = SMF2_MSG_R_CLEAR | SMF2_SEND,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) | P(KE),
	  .opt_enc_payloads = P(N),
	  .processor  = ikev2_child_ike_inIoutR,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_SA_REPLACE },

	{ .story      = "Process CREATE_CHILD_SA IKE Rekey Response",
	  .state      = STATE_V2_REKEY_IKE_I,
	  .next_state = STATE_PARENT_I3,
	  .flags      = SMF2_MSG_R_SET,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) |  P(KE),
	  .opt_enc_payloads = P(N),
	  .processor  = ikev2_child_ike_inR,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_SA_REPLACE, },

	{ .story      = "Process CREATE_CHILD_SA IPsec SA Response",
	  .state      = STATE_V2_CREATE_I,
	  .next_state = STATE_V2_IPSEC_I,
	  .flags      = SMF2_MSG_R_SET,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .opt_enc_payloads = P(KE) | P(N),
	  .processor  = ikev2_child_inR,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_SA_REPLACE, },

	{ .story      = "Respond to CREATE_CHILD_SA IPsec SA Request",
	  .state      = STATE_V2_CREATE_R,
	  .next_state = STATE_V2_IPSEC_R,
	  .flags      = SMF2_MSG_R_CLEAR | SMF2_SEND,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .opt_enc_payloads = P(KE) | P(N),
	  .processor  = ikev2_child_inIoutR,
	  .recv_type  = ISAKMP_v2_CREATE_CHILD_SA,
	  .timeout_event = EVENT_SA_REPLACE, },

	/* Informational Exchange */

	/* RFC 5996 1.4 "The INFORMATIONAL Exchange"
	 *
	 * HDR, SK {[N,] [D,] [CP,] ...}  -->
	 *   <--  HDR, SK {[N,] [D,] [CP], ...}
	 */

	{ .story      = "I3: INFORMATIONAL Request",
	  .state      = STATE_PARENT_I3,
	  .next_state = STATE_PARENT_I3,
	  .flags      = SMF2_IKE_I_SET,
	  .req_clear_payloads = P(SK),
	  .opt_enc_payloads = P(N) | P(D) | P(CP),
	  .processor  = process_encrypted_informational_ikev2,
	  .recv_type  = ISAKMP_v2_INFORMATIONAL,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "I3: INFORMATIONAL Response",
	  .state      = STATE_PARENT_I3,
	  .next_state = STATE_PARENT_I3,
	  .flags      = SMF2_IKE_I_CLEAR,
	  .req_clear_payloads = P(SK),
	  .opt_enc_payloads = P(N) | P(D) | P(CP),
	  .processor  = process_encrypted_informational_ikev2,
	  .recv_type  = ISAKMP_v2_INFORMATIONAL,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "R2: process INFORMATIONAL Request",
	  .state      = STATE_PARENT_R2,
	  .next_state = STATE_PARENT_R2,
	  .flags      = SMF2_IKE_I_SET,
	  .req_clear_payloads = P(SK),
	  .opt_enc_payloads = P(N) | P(D) | P(CP),
	  .processor  = process_encrypted_informational_ikev2,
	  .recv_type  = ISAKMP_v2_INFORMATIONAL,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "R2: process INFORMATIONAL Response",
	  .state      = STATE_PARENT_R2,
	  .next_state = STATE_PARENT_R2,
	  .flags      = SMF2_IKE_I_CLEAR,
	  .req_clear_payloads = P(SK),
	  .opt_enc_payloads = P(N) | P(D) | P(CP),
	  .processor  = process_encrypted_informational_ikev2,
	  .recv_type  = ISAKMP_v2_INFORMATIONAL,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "IKE_SA_DEL: process INFORMATIONAL",
	  .state      = STATE_IKESA_DEL,
	  .next_state = STATE_IKESA_DEL,
	  .flags      = 0,
	  .req_clear_payloads = P(SK),
	  .opt_enc_payloads = P(N) | P(D) | P(CP),
	  .processor  = process_encrypted_informational_ikev2,
	  .recv_type  = ISAKMP_v2_INFORMATIONAL,
	  .timeout_event = EVENT_RETAIN, },

	/* last entry */
	{ .story      = "roof",
	  .state      = STATE_IKEv2_ROOF }
};

void init_ikev2(void)
{
	dbg("checking IKEv2 state table");

	/*
	 * Fill in the states.
	 *
	 * This is a hack until each finite-state is a separate object
	 * with corresponding edges (aka microcodes).
	 *
	 * XXX: Long term goal is to have a constant finite_states[]
	 * contain constant pointers and this writeable array to just
	 * go away.
	 */
	static struct finite_state v2_states[STATE_IKEv2_ROOF - STATE_IKEv2_FLOOR];
	for (enum state_kind kind = STATE_IKEv2_FLOOR; kind < STATE_IKEv2_ROOF; kind++) {

		/* skip hardwired states */
		if (finite_states[kind] != NULL) {
			continue;
		}

		/* fill in using static struct */
		struct finite_state *fs = &v2_states[kind - STATE_IKEv2_FLOOR];
		fs->fs_kind = kind;
		finite_states[kind] = fs;

		fs->fs_name = enum_name(&state_names, fs->fs_kind);
		fs->fs_short_name = enum_short_name(&state_names, fs->fs_kind);
		fs->fs_story = enum_name(&state_stories, fs->fs_kind);

		/*
		 * Initialize .fs_category
		 *
		 * If/when struct finite_state is converted to a static
		 * structure, this all goes away.
		 */
		enum state_category cat;
		switch (fs->fs_kind) {

		case STATE_PARENT_I0:
			/*
			 * IKEv2 IKE SA initiator, while the the SA_INIT
			 * packet is being constructed, are in state.  Only
			 * once the packet has been sent out does it
			 * transition to STATE_PARENT_I1 and start being
			 * counted as half-open.
			 */
			cat = CAT_IGNORE;
			break;

		case STATE_PARENT_I1:
		case STATE_PARENT_R0:
		case STATE_PARENT_R1:
			/*
			 * Count I1 as half-open too because with ondemand,
			 * a plaintext packet (that is spoofed) will
			 * trigger an outgoing IKE SA.
			 */
			cat = CAT_HALF_OPEN_IKE_SA;
			break;

		case STATE_PARENT_I2:
			/*
			 * All IKEv1 MAIN modes except the first
			 * (half-open) and last ones are not
			 * authenticated.
			 */
			cat = CAT_OPEN_IKE_SA;
			break;

		case STATE_V2_CREATE_I0: /* isn't this an ipsec state */
		case STATE_V2_CREATE_I: /* isn't this an ipsec state */
		case STATE_V2_REKEY_IKE_I0:
		case STATE_V2_REKEY_IKE_I:
		case STATE_V2_REKEY_CHILD_I0: /* isn't this an ipsec state */
		case STATE_V2_REKEY_CHILD_I: /* isn't this an ipsec state */
		case STATE_V2_CREATE_R:
		case STATE_V2_REKEY_IKE_R:
		case STATE_V2_REKEY_CHILD_R:
			/*
			 * IKEv1 established states.
			 *
			 * XAUTH, seems to a second level of authentication
			 * performed after the connection is established and
			 * authenticated.
			 */
			cat = CAT_ESTABLISHED_IKE_SA;
			break;

		case STATE_PARENT_I3:
		case STATE_PARENT_R2:
			/*
			 * IKEv2 established states.
			 */
			cat = CAT_ESTABLISHED_IKE_SA;
			break;

		case STATE_V2_IPSEC_I:
		case STATE_V2_IPSEC_R:
			cat = CAT_ESTABLISHED_CHILD_SA;
			break;

		case STATE_IKESA_DEL:
			cat = CAT_ESTABLISHED_IKE_SA;
			break;

		case STATE_CHILDSA_DEL:
			cat = CAT_INFORMATIONAL;
			break;

		default:
			bad_case(fs->fs_kind);
		}
		fs->fs_category = cat;
	}

	/*
	 * Iterate over the state transitions filling in missing bits
	 * and checking for consistency.
	 */
	for (struct state_v2_microcode *t = v2_state_microcode_table;
	     t->state < STATE_IKEv2_ROOF; t++) {

		passert(t->state >= STATE_IKEv2_FLOOR);
		passert(t->state < STATE_IKEv2_ROOF);
		struct finite_state *from = &v2_states[t->state - STATE_IKEv2_FLOOR];

		passert(t->next_state >= STATE_IKEv2_FLOOR);
		passert(t->next_state < STATE_IKEv2_ROOF);
		const struct finite_state *to = finite_states[t->next_state];
		passert(to != NULL);

		DBGF(DBG_TMI, "processing IKEv2 state transition %s -> %s (%s)",
		     from->fs_short_name, to->fs_short_name, t->story);

		/*
		 * Point .fs_v2_microcode at the first transition.
		 * All other microcodes for that state should follow
		 * immediately after (or to put it another way,
		 * previous should match).
		 */
		if (from->fs_v2_transitions == NULL) {
			from->fs_v2_transitions = t;
		} else {
			passert(t[-1].state == t->state);
		}
		from->fs_nr_transitions++;

		/*
		 * Pack expected payloads et.al. into a structure.
		 *
		 * XXX: should be adding everywhere payloads here?!?
		 */
		if (t->req_clear_payloads != LEMPTY) {
			t->message_payloads.required = t->req_clear_payloads;
		}
		if (t->opt_clear_payloads != LEMPTY) {
			t->message_payloads.optional = t->opt_clear_payloads;
		}
		if (t->req_enc_payloads != LEMPTY) {
			t->encrypted_payloads.required = t->req_enc_payloads;
		}
		if (t->opt_enc_payloads != LEMPTY) {
			t->encrypted_payloads.optional = t->opt_enc_payloads;
		}
	}

	/*
	 * Finally list/verify the states.
	 */
	if (DBGP(DBG_BASE)) {
		for (enum state_kind kind = STATE_IKEv2_FLOOR; kind < STATE_IKEv2_ROOF; kind++) {
			const struct finite_state *from = finite_states[kind];
			passert(from != NULL);
			LSWLOG_DEBUG(buf) {
				fmt(buf, "  ");
				lswlog_finite_state(buf, from);
				fmt(buf, ":");
				if (from->fs_nr_transitions == 0) {
					lswlogs(buf, " <none>");
				}
			}
			for (unsigned ti = 0; ti < from->fs_nr_transitions; ti++) {
				const struct state_v2_microcode *t = &from->fs_v2_transitions[ti];
				const struct finite_state *to = finite_states[t->next_state];
				DBG_log("    -> %s %s (%s)", to->fs_short_name,
					enum_short_name(&timer_event_names,
							t->timeout_event),
					t->story);
			}
		}
	}
}

/*
 * split an incoming message into payloads
 */
static struct payload_summary ikev2_decode_payloads(struct msg_digest *md,
						    pb_stream *in_pbs,
						    enum next_payload_types_ikev2 np)
{
	struct payload_summary summary = {
		.parsed = true,
		.n = v2N_NOTHING_WRONG,
	};

	/*
	 * ??? zero out the digest descriptors -- might nuke
	 * ISAKMP_NEXT_v2SK digest!
	 *
	 * XXX: and v2SKF? Safer to leave them as is and just use new
	 * ones - always add to MD, never take away.
	 */

	/*
	 * XXX: Currently, when a message containing an SK payload is
	 * decoded, the encrypted payloads get appended to the
	 * previously decoded non-encrypted payloads.  For instance,
	 * given a message containing two notifications:
	 *
	 *     N(1), SK{ N(2) }
	 *
	 * The notification digest would contain both the unencrypted
	 * N(1) and encrypted N(2).  Since the unencrypted value is
	 * protected, while not very good, isn't really dangerous.
	 */

	while (np != ISAKMP_NEXT_v2NONE) {
		DBG(DBG_CONTROL,
		    DBG_log("Now let's proceed with payload (%s)",
			    enum_show(&ikev2_payload_names, np)));

		if (md->digest_roof >= elemsof(md->digest)) {
			loglog(RC_LOG_SERIOUS,
			       "more than %zu payloads in message; ignored",
			       elemsof(md->digest));
			summary.n = v2N_INVALID_SYNTAX;
			break;
		}

		/*
		 * *pd is the payload digest for this payload.
		 * It has three fields:
		 *	pbs is filled in by in_struct
		 *	payload is filled in by in_struct
		 *	next is filled in by list linking logic
		 */
		struct payload_digest *const pd = md->digest + md->digest_roof;

		/* map the payload onto a way to decode it */
		const struct_desc *sd = v2_payload_desc(np);

		if (sd == NULL) {
			/*
			 * This payload is unknown to us.  RFCs 4306
			 * and 5996 2.5 say that if the payload has
			 * the Critical Bit, we should be upset but if
			 * it does not, we should just ignore it.
			 */
			if (!in_struct(&pd->payload, &ikev2_generic_desc, in_pbs, &pd->pbs)) {
				loglog(RC_LOG_SERIOUS, "malformed payload in packet");
				summary.n = v2N_INVALID_SYNTAX;
				break;
			}
			if (pd->payload.v2gen.isag_critical & ISAKMP_PAYLOAD_CRITICAL) {
				/*
				 * It was critical.  See RFC 5996 1.5
				 * "Version Numbers and Forward
				 * Compatibility"
				 */
				const char *role;
				switch (v2_msg_role(md)) {
				case MESSAGE_REQUEST:
					role = "request";
					break;
				case MESSAGE_RESPONSE:
					role = "response";
					break;
				default:
					bad_case(v2_msg_role(md));
				}
				loglog(RC_LOG_SERIOUS,
				       "message %s contained an unknown critical payload type (%s)",
				       role, enum_show(&ikev2_payload_names, np));
				summary.n = v2N_UNSUPPORTED_CRITICAL_PAYLOAD;
				summary.data[0] = np;
				summary.data_size = 1;
				break;
			}
			loglog(RC_COMMENT,
				"non-critical payload ignored because it contains an unknown or unexpected payload type (%s) at the outermost level",
				enum_show(&ikev2_payload_names, np));
			np = pd->payload.generic.isag_np;
			continue;
		}

		if (np >= LELEM_ROOF) {
			DBG(DBG_CONTROL, DBG_log("huge next-payload %u", np));
			summary.n = v2N_INVALID_SYNTAX;
			break;
		}
		summary.repeated |= (summary.present & LELEM(np));
		summary.present |= LELEM(np);

		if (!in_struct(&pd->payload, sd, in_pbs, &pd->pbs)) {
			loglog(RC_LOG_SERIOUS, "malformed payload in packet");
			summary.n = v2N_INVALID_SYNTAX;
			break;
		}

		DBG(DBG_PARSING,
		    DBG_log("processing payload: %s (len=%zu)",
			    enum_show(&ikev2_payload_names, np),
			    pbs_left(&pd->pbs)));

		/*
		 * Place payload at the end of the chain for this type.
		 * This code appears in ikev1.c and ikev2.c.
		 */
		{
			/* np is a proper subscript for chain[] */
			passert(np < elemsof(md->chain));
			struct payload_digest **p = &md->chain[np];

			while (*p != NULL)
				p = &(*p)->next;
			*p = pd;
			pd->next = NULL;
		}

		/*
		 * XXX: should this do 'deeper' analysis of packets.
		 * For instance checking the SPI of a notification
		 * payload?  Probably not as the value may be ignored.
		 */

		/*
		 * Advance next payload.
		 */
		switch (np) {
		case ISAKMP_NEXT_v2SK:
		case ISAKMP_NEXT_v2SKF:
			/* RFC 5996 2.14 "Encrypted Payload":
			 *
			 * Next Payload - The payload type of the
			 * first embedded payload.  Note that this is
			 * an exception in the standard header format,
			 * since the Encrypted payload is the last
			 * payload in the message and therefore the
			 * Next Payload field would normally be zero.
			 * But because the content of this payload is
			 * embedded payloads and there was no natural
			 * place to put the type of the first one,
			 * that type is placed here.
			 */
			np = ISAKMP_NEXT_v2NONE;
			break;

		default:
			np = pd->payload.generic.isag_np;
			break;
		}

		md->digest_roof++;
	}

	return summary;
}

static struct ikev2_payload_errors ikev2_verify_payloads(struct msg_digest *md,
							 const struct payload_summary *summary,
							 const struct ikev2_expected_payloads *payloads)
{
	/*
	 * Convert SKF onto SK for the comparison (but only when it is
	 * on its own).
	 */
	lset_t seen = summary->present;
	if ((seen & (P(SKF)|P(SK))) == P(SKF)) {
		seen &= ~P(SKF);
		seen |= P(SK);
	}

	lset_t req_payloads = payloads->required;
	lset_t opt_payloads = payloads->optional;

	struct ikev2_payload_errors errors = {
		.bad = false,
		.excessive = summary->repeated & ~repeatable_payloads,
		.missing = req_payloads & ~seen,
		.unexpected = seen & ~req_payloads & ~opt_payloads & ~everywhere_payloads,
	};

	if ((errors.excessive | errors.missing | errors.unexpected) != LEMPTY) {
		errors.bad = true;
	}

	if (payloads->notification != v2N_NOTHING_WRONG) {
		bool found = false;
		for (struct payload_digest *pd = md->chain[ISAKMP_NEXT_v2N];
		     pd != NULL; pd = pd->next) {
			if (pd->payload.v2n.isan_type == payloads->notification) {
				found = true;
				break;
			}
		}
		if (!found) {
			errors.bad = true;
			errors.notification = payloads->notification;
		}
	}

	return errors;
}

/* report problems - but less so when OE */
static void ikev2_log_payload_errors(struct state *st, struct msg_digest *md,
				     const struct ikev2_payload_errors *errors)
{
	if (!DBGP(DBG_OPPO)) {
		/*
		 * ??? this logic is contorted.
		 * If we have no state, we act as if this is opportunistic.
		 * But if there is a state, but no connection,
		 * we act as if this is NOT opportunistic.
		 */
		if (st == NULL ||
		    (st->st_connection != NULL &&
		     (st->st_connection->policy & POLICY_OPPORTUNISTIC)))
		{
			return;
		}
	}

	LSWLOG_RC(RC_LOG_SERIOUS, buf) {
		const enum isakmp_xchg_types ix = md->hdr.isa_xchg;
		lswlogs(buf, "dropping unexpected ");
		lswlog_enum_short(buf, &ikev2_exchange_names, ix);
		lswlogs(buf, " message");
		/* we want to print and log the first notify payload */
		struct payload_digest *ntfy = md->chain[ISAKMP_NEXT_v2N];
		if (ntfy != NULL) {
			lswlogs(buf, " containing ");
			lswlog_enum_short(buf, &ikev2_notify_names,
					  ntfy->payload.v2n.isan_type);
			if (ntfy->next != NULL) {
				lswlogs(buf, "...");
			}
			lswlogs(buf, " notification");
		}
		if (md->message_payloads.parsed) {
			lswlogf(buf, "; message payloads: ");
			lswlog_enum_lset_short(buf, &ikev2_payload_names, ",",
					       md->message_payloads.present);
		}
		if (md->encrypted_payloads.parsed) {
			lswlogf(buf, "; encrypted payloads: ");
			lswlog_enum_lset_short(buf, &ikev2_payload_names, ",",
					       md->encrypted_payloads.present);
		}
		if (errors->missing != LEMPTY) {
			lswlogf(buf, "; missing payloads: ");
			lswlog_enum_lset_short(buf, &ikev2_payload_names, ",",
					       errors->missing);
		}
		if (errors->unexpected != LEMPTY) {
			lswlogf(buf, "; unexpected payloads: ");
			lswlog_enum_lset_short(buf, &ikev2_payload_names, ",",
					       errors->unexpected);
		}
		if (errors->excessive != LEMPTY) {
			lswlogf(buf, "; excessive payloads: ");
			lswlog_enum_lset_short(buf, &ikev2_payload_names, ",",
					       errors->excessive);
		}
		if (errors->notification != v2N_NOTHING_WRONG) {
			lswlogs(buf, "; missing notification ");
			lswlog_enum_short(buf, &ikev2_notify_names,
					  errors->notification);
		}
	}
}

static bool ikev2_check_fragment(struct msg_digest *md, struct state *st)
{
	struct ikev2_skf *skf = &md->chain[ISAKMP_NEXT_v2SKF]->payload.v2skf;

	/* ??? CLANG 3.5 thinks st might be NULL */
	if (!(st->st_connection->policy & POLICY_IKE_FRAG_ALLOW)) {
		DBG(DBG_CONTROL, DBG_log(
			"discarding IKE encrypted fragment - fragmentation not allowed by local policy (ike_frag=no)"));
		return FALSE;
	}

	if (!(st->st_seen_fragvid)) {
		DBG(DBG_CONTROL, DBG_log(
			    "discarding IKE encrypted fragment - remote never proposed fragmentation"));
		return FALSE;
	}

	DBG(DBG_CONTROL, DBG_log(
		"received IKE encrypted fragment number '%u', total number '%u', next payload '%u'",
		    skf->isaskf_number, skf->isaskf_total, skf->isaskf_np));

	/*
	 * Sanity check:
	 * fragment number must be 1 or greater (not 0)
	 * fragment number must be no greater than the total number of fragments
	 * total number of fragments must be no more than MAX_IKE_FRAGMENTS
	 * first fragment's next payload must not be ISAKMP_NEXT_v2NONE.
	 * later fragments' next payload must be ISAKMP_NEXT_v2NONE.
	 */
	if (!(skf->isaskf_number != 0 &&
	      skf->isaskf_number <= skf->isaskf_total &&
	      skf->isaskf_total <= MAX_IKE_FRAGMENTS &&
	      (skf->isaskf_number == 1) != (skf->isaskf_np == ISAKMP_NEXT_v2NONE)))
	{
		DBG(DBG_CONTROL, DBG_log(
			"ignoring invalid IKE encrypted fragment"));
		return FALSE;
	}

	if (st->st_v2_rfrags == NULL) {
		/* first fragment: must be good */
		return TRUE;
	}

	if (skf->isaskf_total != st->st_v2_rfrags->total) {
		/*
		 * total number of fragments changed.
		 * Either this fragment is wrong or all the
		 * stored fragments are wrong or superseded.
		 * The only reason the other end would have
		 * started over with a different number of fragments
		 * is because it decided to ratchet down the packet size
		 * (and thus increase total).
		 * OK: skf->isaskf_total > i->total
		 * Bad: skf->isaskf_total < i->total
		 */
		if (skf->isaskf_total > st->st_v2_rfrags->total) {
			DBG(DBG_CONTROL, DBG_log(
				"discarding saved fragments because this fragment has larger total"));
			release_fragments(st);
			return TRUE;
		} else {
			DBG(DBG_CONTROL, DBG_log(
				"ignoring odd IKE encrypted fragment (total shrank)"));
			return FALSE;
		}
	} else if (st->st_v2_rfrags->frags[skf->isaskf_number].cipher.ptr != NULL) {
		/* retain earlier fragment with same index */
		DBG(DBG_CONTROL, DBG_log(
			    "ignoring repeated IKE encrypted fragment"));
		return FALSE;
	} else {
		return TRUE;
	}
}

static bool ikev2_collect_fragment(struct msg_digest *md, struct state *st)
{
	struct ikev2_skf *skf = &md->chain[ISAKMP_NEXT_v2SKF]->payload.v2skf;
	pb_stream *e_pbs = &md->chain[ISAKMP_NEXT_v2SKF]->pbs;

	if (!ikev2_check_fragment(md, st)) {
		return FALSE;
	}

	/* if receiving fragments, respond with fragments too */
	if (!st->st_seen_fragments) {
		st->st_seen_fragments = TRUE;
		DBG(DBG_CONTROL,
		    DBG_log(" updated IKE fragment state to respond using fragments without waiting for re-transmits"));
	}

	/*
	 * Since the fragment check above can result in all fragments
	 * so-far being discarded; always check/fix frags.
	 */
	if (st->st_v2_rfrags == NULL) {
		st->st_v2_rfrags = alloc_thing(struct v2_ike_rfrags, "incoming v2_ike_rfrags");
		st->st_v2_rfrags->total = skf->isaskf_total;
	}

	passert(skf->isaskf_number < elemsof(st->st_v2_rfrags->frags));
	struct v2_ike_rfrag *frag = &st->st_v2_rfrags->frags[skf->isaskf_number];
	passert(frag->cipher.ptr == NULL);
	frag->iv = e_pbs->cur - md->packet_pbs.start;
	clonetochunk(frag->cipher, md->packet_pbs.start,
		     e_pbs->roof - md->packet_pbs.start,
		     "incoming IKEv2 encrypted fragment");

	if (skf->isaskf_number == 1) {
		st->st_v2_rfrags->first_np = skf->isaskf_np;
	}

	passert(st->st_v2_rfrags->count < st->st_v2_rfrags->total);
	st->st_v2_rfrags->count++;
	return st->st_v2_rfrags->count == st->st_v2_rfrags->total;
}

static struct state *process_v2_child_ix(struct msg_digest *md,
					 struct state *is_this_ike_or_child_sa)
{
	struct state *st; /* child state */

	/* for log */
	const char *what;
	const char *why = "";

	/* force pst to be parent state */
	/* ??? should we not already know whether this is a parent state? */
	struct ike_sa *ike = ike_sa(is_this_ike_or_child_sa);

	if (is_msg_request(md)) {
		/* this an IKE request and not a response */
		if (v2_child_sa_responder_with_msgid(ike, md->hdr.isa_msgid) != NULL) {
			what = "CREATE_CHILD_SA Request retransmission ignored";
			st = NULL;
		} else if (md->from_state == STATE_V2_CREATE_R) {
			what = "Child SA Request";
			st = ikev2_duplicate_state(ike, IPSEC_SA,
						   SA_RESPONDER);
			change_state(st, STATE_V2_CREATE_R);
			st->st_msgid = md->hdr.isa_msgid;
			refresh_state(st);
		} else {
			what = "IKE Rekey Request";
			st = ikev2_duplicate_state(ike, IKE_SA,
						   SA_RESPONDER);
			change_state(st, STATE_V2_REKEY_IKE_R); /* start with this */
			st->st_msgid = md->hdr.isa_msgid;
			refresh_state(st);
		}
	} else  {
		/* this a response */
		what = "Child SA Response";
		st = v2_child_sa_initiator_with_msgid(ike, md->hdr.isa_msgid);
		if (st == NULL) {
			switch (md->from_state) {
			case STATE_V2_CREATE_I:
				what = "IPsec Child Response";
				why = " no matching IPsec child state for this response";
				break;

			case STATE_V2_REKEY_IKE_I:
				what = "IKE Rekey Response";
				why = " no matching IKE Rekey state for this response";
				break;

			case STATE_V2_REKEY_CHILD_I:
				what = "IPsec Child Rekey Response";
				why = " no matching rekey child state for this response";
				break;
			default:
				/* ??? can this happen? */
				break;
			}
		}
	}

	if (st == NULL) {
		libreswan_log("rejecting %s CREATE_CHILD_SA%s hdr.isa_msgid: %u st_msgid_lastrecv %u",
			      what, why,
			      md->hdr.isa_msgid,
			      ike->sa.st_msgid_lastrecv);
	} else {
		bool st_busy = st->st_suspended_md != NULL || st->st_suspended_md != NULL;
		DBG(DBG_CONTROLMORE, {
			ipstr_buf b;
			char ca[CONN_INST_BUF];
			char cb[CONN_INST_BUF];
			DBG_log("\"%s\"%s #%lu received %s CREATE_CHILD_SA%s from %s:%u Child \"%s\"%s #%lu in %s %s",
				ike->sa.st_connection->name,
				fmt_conn_instance(ike->sa.st_connection, ca),
				ike->sa.st_serialno,
				what, why, ipstr(&md->sender, &b),
				hportof(&md->sender),
				st->st_connection->name,
				fmt_conn_instance(st->st_connection, cb),
				st->st_serialno,
				st->st_state_name,
				st_busy ? "is busy processing a response drop this message" :
					"will process it further");
		});

		if (st_busy)
			st = NULL; /* in the previous message */
	}

	return st;
}

/*
 * If this looks like a re-transmit return true and, possibly,
 * respond.
 */

static bool processed_retransmit(struct state *st,
				 struct msg_digest *md,
				 const enum isakmp_xchg_types ix)
{
	/*
	 * XXX: This solution is broken. If two exchanges (after the
	 * initial exchange) are interleaved, we ignore the first.
	 * This is https://bugs.libreswan.org/show_bug.cgi?id=185
	 *
	 * Beware of unsigned arrithmetic.
	 */
	dbg("#%lu st.st_msgid_lastrecv %d md.hdr.isa_msgid %08x",
	    st->st_serialno, st->st_msgid_lastrecv, md->hdr.isa_msgid);
	if (st->st_msgid_lastrecv != v2_INVALID_MSGID &&
	    st->st_msgid_lastrecv > md->hdr.isa_msgid) {
		/* this is an OLD retransmit. we can't do anything */
		libreswan_log("received too old retransmit: %u < %u",
			      md->hdr.isa_msgid,
			      st->st_msgid_lastrecv);
		return true;
	}

	if (st->st_msgid_lastrecv != md->hdr.isa_msgid) {
		/* presumably not a re-transmit */
		return false;
	}

	/* this should never happen */
	if (st->st_tpacket.len == 0 && st->st_v2_tfrags == NULL) {
		PEXPECT_LOG("retransmission for message ID: %u exchange %s failed lastreplied %u - we have no stored packet to retransmit",
			    st->st_msgid_lastrecv,
			    enum_name(&ikev2_exchange_names, ix),
			    st->st_msgid_lastreplied);
		return true;
	}

	if (st->st_msgid_lastreplied != st->st_msgid_lastrecv) {
		LSWDBGP(DBG_CONTROLMORE|DBG_RETRANSMITS, buf) {
			lswlog_retransmit_prefix(buf, st);
			lswlogf(buf, "cannot retransmit response for message ID: %u exchange %s lastreplied %u",
				st->st_msgid_lastrecv,
				enum_name(&ikev2_exchange_names, ix),
				st->st_msgid_lastreplied);
		}
		struct state *cst =  v2_child_sa_responder_with_msgid(ike_sa(st),
								      st->st_msgid_lastrecv);
		if (cst == NULL) {
			/* XXX: why? */
			return false; /* process the re-transtmited message */
		}
		LSWDBGP(DBG_CONTROLMORE|DBG_RETRANSMITS, buf) {
			lswlog_retransmit_prefix(buf, st);
			lswlogf(buf, "state #%lu %s is working on message ID: %u %s, retransmission ignored",
				cst->st_serialno,
				st->st_state_name,
				st->st_msgid_lastrecv,
				enum_name(&ikev2_exchange_names, ix));
		}
		return true;
	}

	/*
	 * XXX: IKEv1 saves the last received packet and compares.
	 * Would doing that be doing that (and say only saving the
	 * first fragment) be safer?
	 */
	if (md->hdr.isa_np == ISAKMP_NEXT_v2SKF) {
		struct ikev2_skf skf;
		pb_stream in_pbs = md->message_pbs; /* copy */
		if (!in_struct(&skf, &ikev2_skf_desc, &in_pbs, NULL)) {
			return true;
		}
		bool retransmit = skf.isaskf_number == 1;
		LSWDBGP(DBG_CONTROLMORE|DBG_RETRANSMITS, buf) {
			lswlog_retransmit_prefix(buf, st);
			lswlogf(buf, "%s message ID %u exchange %s fragment %u",
				retransmit ? "retransmitting response for" : "ignoring retransmit of",
				st->st_msgid_lastrecv,
				enum_name(&ikev2_exchange_names, ix),
				skf.isaskf_number);
		}
		if (retransmit) {
			send_recorded_v2_ike_msg(st, "ikev2-responder-retransmt (fragment 0)");
		}
	} else {
		LSWDBGP(DBG_CONTROLMORE|DBG_RETRANSMITS, buf) {
			lswlog_retransmit_prefix(buf, st);
			lswlogf(buf, "retransmit response for message ID: %u exchange %s",
				st->st_msgid_lastrecv,
				enum_name(&ikev2_exchange_names, ix));
		}
		send_recorded_v2_ike_msg(st, "ikev2-responder-retransmit");
	}

	return true;
}

/*
 * process an input packet, possibly generating a reply.
 *
 * If all goes well, this routine eventually calls a state-specific
 * transition function.
 *
 * This routine will not release_any_md(mdp).  It is expected that its
 * caller will do this.  In fact, it will zap *mdp to NULL if it thinks
 * **mdp should not be freed.  So the caller should be prepared for
 * *mdp being set to NULL.
 */
void ikev2_process_packet(struct msg_digest **mdp)
{
	struct msg_digest *md = *mdp;

	/* Look for an state that matches the various things we know:
	 *
	 * 1) exchange type received?
	 * 2) is it initiator or not?
	 */

	const enum isakmp_xchg_types ix = md->hdr.isa_xchg;
	const bool sent_by_ike_initiator = (md->hdr.isa_flags & ISAKMP_FLAGS_v2_IKE_I) != 0;

	DBG(DBG_CONTROL, {
			struct esb_buf ixb;
			switch (v2_msg_role(md)) {
			case MESSAGE_RESPONSE:
				DBG_log("I am receiving an IKEv2 Response %s",
					enum_showb(&ikev2_exchange_names, ix, &ixb));
				break;
			case MESSAGE_REQUEST:
				DBG_log("I am receiving an IKEv2 Request %s",
					enum_showb(&ikev2_exchange_names, ix, &ixb));
				break;
			default:
				bad_case(v2_msg_role(md));
			}
		});

	if (sent_by_ike_initiator) {
		DBG(DBG_CONTROL, DBG_log("I am the IKE SA Original Responder"));
	} else {
		DBG(DBG_CONTROL, DBG_log("I am the IKE SA Original Initiator"));
	}

	/*
	 * Find the state that the packet is sent to.
	 *
	 * The only time there isn't a state is when the responder
	 * first sees an SA_INIT request (or it's forgotten that it has
	 * seen it before).
	 */

	struct state *st;
	if (ix == ISAKMP_v2_IKE_SA_INIT) {
		/*
		 * The message ID of the initial exchange is always
		 * zero.
		 */
		if (md->hdr.isa_msgid != 0) {
			libreswan_log("dropping IKE_SA_INIT message containing non-zero message ID");
			return;
		}
		/*
		 * Now try to find the state
		 */
		switch (v2_msg_role(md)) {
		case MESSAGE_REQUEST:
			/* The initiator must send: IKE_I && !MSG_R */
			if (!sent_by_ike_initiator) {
				libreswan_log("dropping IKE_SA_INIT request with conflicting IKE initiator flag");
				return;
			}
			/*
			 * 3.1.  The IKE Header: This [SPIr] value
			 * MUST be zero in the first message of an IKE
			 * initial exchange (including repeats of that
			 * message including a cookie).
			 */
			if (!ike_spi_is_zero(&md->hdr.isa_ike_responder_spi)) {
				libreswan_log("dropping IKE_SA_INIT request with non-zero SPIr");
				return;
			}
			/*
			 * Look for a pre-existing IKE SA responder
			 * state using just the SPIi (SPIr in the
			 * message is zero so can't be used).
			 *
			 * If the lookup succeeds then there are
			 * several possibilities:
			 *
			 * State has Message ID == 0:
			 *
			 * Either it really is a duplicate; or it's a
			 * second (fake?) intiator sending the same
			 * SPIi at exactly the same time as the first
			 * (wow, what are the odds, it must be our
			 * lucky day!).
			 *
			 * Either way, the duplicate code needs to
			 * compare packets and decide if a retransmit
			 * or drop is required.  If the second
			 * initiator is real, then it will timeout and
			 * then retry with a new SPIi.
			 *
			 * State has Message ID > 0:
			 *
			 * Either it is an old duplicate; or, again,
			 * it's a second intiator sending the same
			 * SPIi only slightly later (again, what are
			 * the odds!).
			 *
			 * Several choices: let the duplicate code
			 * drop the packet, which is correct for an
			 * old duplicate message; or ignore the
			 * existing state and create a new one, which
			 * is good for the second initiator but not so
			 * good for an old duplicate.  Given an old
			 * duplicate is far more likely, handle that
			 * cleenly - let the duplicate code drop the
			 * packet.
			 */
			st = find_v2_ike_sa_by_initiator_spi(&md->hdr.isa_ike_initiator_spi);
			if (st != NULL) {
				/* duplicate code below will decide what to do */
				dbg("received what looks like a duplicate IKE_SA_INIT for #%lu",
				    st->st_serialno);
			} else if (drop_new_exchanges()) {
				/* only log for debug to prevent disk filling up */
				dbg("pluto is overloaded with half-open IKE SAs; dropping new exchange");
				return;
			} else {
				/*
				 * Always check for cookies! XXX: why?
				 *
				 * Because the v2N_COOKIE payload is
				 * first, parsing and verifying it
				 * should be relatively quick and
				 * cheap, right?
				 *
				 * No.  The equation uses v2Ni forcing
				 * the entire payload to be parsed.
				 */
				pexpect(!md->message_payloads.parsed);
				md->message_payloads = ikev2_decode_payloads(md,
									     &md->message_pbs,
									     md->hdr.isa_np);
				if (md->message_payloads.n != v2N_NOTHING_WRONG) {
					if (require_ddos_cookies()) {
						dbg("DDOS so not responding to invalid packet");
					} else {
						chunk_t data = chunk(md->message_payloads.data,
								     md->message_payloads.data_size);
						send_v2N_response_from_md(md, md->message_payloads.n,
									  &data);
					}
					return;
				}
				if (v2_rejected_initiator_cookie(md, require_ddos_cookies())) {
					dbg("pluto is overloaded and demanding cookies; dropping new exchange");
					return;
				}
				/* else - create a draft state here? */
			}
			/* update lastrecv later on */
			break;
		case MESSAGE_RESPONSE:
			/* The responder must send: !IKE_I && MSG_R. */
			if (sent_by_ike_initiator) {
				libreswan_log("dropping IKE_SA_INIT response with conflicting IKE initiator flag");
				return;
			}
			/*
			 * 2.6.  IKE SA SPIs and Cookies: When the
			 * IKE_SA_INIT exchange does not result in the
			 * creation of an IKE SA due to
			 * INVALID_KE_PAYLOAD, NO_PROPOSAL_CHOSEN, or
			 * COOKIE, the responder's SPI will be zero
			 * also in the response message.  However, if
			 * the responder sends a non-zero responder
			 * SPI, the initiator should not reject the
			 * response for only that reason.
			 *
			 * i.e., can't check response for non-zero
			 * SPIr.
			 */
			/*
			 * Look for a pre-existing IKE SA responder
			 * state using just the SPIi (SPIr in the
			 * message isn't known so can't be used).
			 *
			 * An IKE_SA_INIT error notification response
			 * (INVALID_KE, COOKIE) should contain a zero
			 * SPIr (it must be ignored).
			 *
			 * An IKE_SA_INIT success response will
			 * contain an as yet unknown but non-zero SPIr
			 * so looking for it won't work.
			 */
			st = find_v2_ike_sa_by_initiator_spi(&md->hdr.isa_ike_initiator_spi);
			if (st == NULL) {
				/*
				 * There should be a state matching
				 * the original initiator's cookie.
				 * Since there isn't someone's playing
				 * games.  Drop the packet.
				 */
				libreswan_log("no matching state for IKE_SA_INIT response; discarding packet");
				return;
			}
			/*
			 * Check that this is the first response seen
			 * by the state.  Which only happens when it
			 * is in the inital state.
			 *
			 * XXX: this is a simplified version of the
			 * more general Message ID check performed in
			 * the MESSAGE_RESPONSE path below.  It should
			 * be merged.
			 */
			if (st->st_msgid_lastack != v2_INVALID_MSGID) {
				libreswan_log("already processed IKE_SA_INIT response for state #%lu; discarding packet",
					      st->st_serialno);
				return;
			}
			/*
			 * Responder provided a cookie, record it.
			 *
			 * XXX: This is being done far too early.  The
			 * packet should first get some validation.
			 * It might also be an INVALID_KE or COOKIE
			 * response in which case SPIr shouldn't be
			 * updated at all.
			 */
			rehash_state(st, &md->hdr.isa_ike_responder_spi);
			break;
		default:
			bad_case(v2_msg_role(md));
		}
	} else if (v2_msg_role(md) == MESSAGE_REQUEST) {
		/*
		 * A (possibly new) request; start with the IKE SA
		 * with matching SPIs.  If it is a new CHILD SA
		 * request then the state machine will will morph ST
		 * into a child state before dispatching.
		 */
		st = find_v2_ike_sa(&md->hdr.isa_ike_spis);
		if (st == NULL) {
			struct esb_buf ixb;
			rate_log("%s message request has no corresponding IKE SA",
				 enum_show_shortb(&ikev2_exchange_names,
						  ix, &ixb));
			return;
		}
		/* update lastrecv later on */
	} else if (v2_msg_role(md) == MESSAGE_RESPONSE) {
		/*
		 * A response; find the IKE SA or CHILD SA that
		 * initiated the request.
		 *
		 * XXX: Why is the exchange type needed?  Surely state
		 * machine will figure out if the state X exchange is
		 * valid?
		 *
		 * Lets find out.
		 *
		 * XXX: Why is a CHILD SA specific and then an IKE SA
		 * specific lookup needed?  Surely a Message ID
		 * uniquely identifies the IKE or CHILD that sent the
		 * request?
		 *
		 * Lets find out.
		 *
		 * XXX: What happens when a duplicate response is
		 * received?  For instance, IKE SAs send requests with
		 * no state change.  Unless the state's Message ID is
		 * being thwacked, it will match and, potentially
		 * cause a re-processing of the state.  Perhaps that
		 * is why the IX parameter is needed?
		 *
		 * Lets find out.
		 *
		 * XXX: Why is an IKE SA lookup needed?  If there is
		 * no Message ID match then just log it - the IKE SA
		 * is being used to make the error message "pretty".
		 */
		st = find_state_ikev2_child(ix, &md->hdr.isa_ike_spis,
					    md->hdr.isa_msgid);
		if (DBGP(DBG_BASE)) {
			struct state *msgid_st = DBG_v2_sa_by_msgid(&md->hdr.isa_ike_spis,
								    md->hdr.isa_msgid);
			if (st != msgid_st) {
				DBG_log("state and msgid search mismatch");
			}
		}

		if (st == NULL) {
			/*
			 * Didn't find a child waiting on that message
			 * ID so presumably it isn't valid.
			 */
			st = find_v2_ike_sa(&md->hdr.isa_ike_spis);
			if (st == NULL) {
				rate_log("%s message response has no matching IKE SA",
					 enum_name(&ikev2_exchange_names, ix));
				return;
			}
			/*
			 * Check if it's an old packet being returned,
			 * and if so, drop it.  NOTE: in_struct()
			 * changed the byte order.
			 *
			 * Beware of unsigned arrithmetic.
			 */
			if (st->st_msgid_lastack != v2_INVALID_MSGID &&
			    st->st_msgid_lastack > md->hdr.isa_msgid) {
				/*
				 * An old response to our request?
				 */
				LSWDBGP(DBG_CONTROL|DBG_RETRANSMITS, buf) {
					lswlog_retransmit_prefix(buf, st);
					lswlogf(buf, "dropping retransmitted response with msgid %u from peer - we already processed %u.",
						md->hdr.isa_msgid, st->st_msgid_lastack);
				}
				return;
			}
			if (st->st_msgid_nextuse != v2_INVALID_MSGID &&
			    md->hdr.isa_msgid >= st->st_msgid_nextuse) {
				/*
				 * A reply for an unknown request (or
				 * request we've not yet sent)? Huh!
				 */
				dbg("dropping unasked response with msgid %u from peer (our last used msgid is %u)",
				     md->hdr.isa_msgid,
				     st->st_msgid_nextuse - 1);
				return;
			}
			/*
			 * Assume the request was generated by the IKE
			 * SA, for instance:
			 *
 			 * - as shown by ikev2-delete-02, the delete
			 *   response
			 *
			 * - (in theory), when an AUTH exchange
			 *   involves multiple messages (so the CHILD
			 *   SA can't be created early), the AUTH
			 *   response???
			 *
			 * - ???
			 *
			 * The log line lets find out.
			 */
			dbg("using IKE SA #%lu for response with msgid %u (msgid: %u; nextuse: %u, lastack: %u; lastrecv: %u, lastreplied: %u)",
			    st->st_serialno, md->hdr.isa_msgid, st->st_msgid,
			    st->st_msgid_nextuse, st->st_msgid_lastack,
			    st->st_msgid_lastrecv, st->st_msgid_lastreplied);

			if (DBGP(DBG_BASE)) {
				struct state *msgid_st = DBG_v2_sa_by_msgid(&md->hdr.isa_ike_spis,
									    md->hdr.isa_msgid);
				if (st != msgid_st) {
					DBG_log("state and msgid search mismatch");
				}
			}
		}
	} else {
		PASSERT_FAIL("message role %d invalid", v2_msg_role(md));
	}

	/*
	 * If there's a state, attribute all further logging to that
	 * state.
	 */
	if (st != NULL) {
		DBG(DBG_CONTROL,
		    DBG_log("found state #%lu", st->st_serialno));
		set_cur_state(st);
	}

	/*
	 * Now that cur-state has been set for logging, log if this
	 * packet is really bogus.
	 */
	if (md->fake_clone) {
		libreswan_log("IMPAIR: processing a fake (cloned) message");
	}

	/*
	 * Check ST's IKE SA's role against the I(Initiator) flag in
	 * the headers.
	 *
	 * ST!=NULL IFF IKE!=NULL, and ike_sa(NULL) handles this.
	 */
	struct ike_sa *const ike = ike_sa(st);
	if (st != NULL && ike == NULL) {
		PEXPECT_LOG("lost IKE SA for #%lu; dropping packet", st->st_serialno);
		/* XXX: should state be deleted? */
		return;
	}
	if (st != NULL) {
		switch (ike->sa.st_sa_role) {
		case SA_INITIATOR:
			if (sent_by_ike_initiator) {
				rate_log("IKE SA initiator received a message with I(Initiator) flag set; dropping packet");
				return;
			}
			break;
		case SA_RESPONDER:
			if (!sent_by_ike_initiator) {
				rate_log("IKE SA responder received a message with I(Initiator) flag clear; dropping packet");
				return;
			}
			break;
		default:
			bad_case(ike->sa.st_sa_role);
		}
	}

	/*
	 * If the state is busy, presumably doing something like
	 * crypto, skip further processing.
	 *
	 * For re-transmits, they should have been handled by the code
	 * above.
	 *
	 * For fragments, things only go busy once all fragments have
	 * been received (and re-transmitted fragments are ignored).
	 * If this changes then a lot more than this code will need to
	 * be moved.
	 */
	if (verbose_state_busy(st))
		return;

	/* was this is a recent retransmit. */
	if (st != NULL && v2_msg_role(md) == MESSAGE_REQUEST &&
	    processed_retransmit(st, md, ix)) {
		return;
	}

	ikev2_process_state_packet(ike, st, mdp);
}

void ikev2_process_state_packet(struct ike_sa *ike, struct state *st,
				struct msg_digest **mdp)
{
	struct msg_digest *md = *mdp;

	/*
	 * There is no "struct state" object if-and-only-if we're
	 * responding to a shiny new SA_INIT message.  The start-state
	 * transition will (probably) create the object.
	 *
	 * But what about when pluto, as the initial responder, is
	 * fending of an attack attack by sending back and requiring
	 * cookies - won't the cookie need a "struct state"?
	 * According to the RFC: no.  Instead a small table of
	 * constants can be used to generate cookies on the fly.
	 */
	const struct finite_state *from_state =
		st == NULL ? finite_states[STATE_PARENT_R0] : st->st_finite_state;
	dbg("#%lu in state %s: %s",
	     st != NULL ? st->st_serialno : 0,
	     from_state->fs_short_name, from_state->fs_story);

	struct ikev2_payload_errors message_payload_status = { .bad = false };
	struct ikev2_payload_errors encrypted_payload_status = { .bad = false };

	const enum isakmp_xchg_types ix = (*mdp)->hdr.isa_xchg;

	const struct state_v2_microcode *svm;
	for (svm = v2_state_microcode_table; svm->state != STATE_IKEv2_ROOF;
	     svm++) {
		/*
		 * For CREATE_CHILD_SA exchanges, the from_state is
		 * ignored.  See further down.
		 */
		if (svm->state != from_state->fs_kind && ix != ISAKMP_v2_CREATE_CHILD_SA)
			continue;
		if (svm->recv_type != ix)
			continue;
		/*
		 * Does the original [ike] initiator flag match?
		 */
		if (svm->flags & SMF2_IKE_I_SET) {
			if ((md->hdr.isa_flags & ISAKMP_FLAGS_v2_IKE_I) == 0)
				/* opps, clear */
				continue;
		}
		if (svm->flags & SMF2_IKE_I_CLEAR) {
			if ((md->hdr.isa_flags & ISAKMP_FLAGS_v2_IKE_I) != 0)
				/* opps, set */
				continue;
		}
		/*
		 * Does the message reply flag match?
		 */
		if (svm->flags & SMF2_MSG_R_SET) {
			if ((md->hdr.isa_flags & ISAKMP_FLAGS_v2_MSG_R) == 0)
				/* oops, clear */
				continue;
		}
		if (svm->flags & SMF2_MSG_R_CLEAR) {
			if ((md->hdr.isa_flags & ISAKMP_FLAGS_v2_MSG_R) != 0)
				/* opps, set */
				continue;
		}

		/*
		 * Since there is a state transition that looks like
		 * it might accept the packet, parse the clear payload
		 * and then continue matching.
		 */
		if (!md->message_payloads.parsed) {
			DBG(DBG_CONTROL, DBG_log("Unpacking clear payload for svm: %s", svm->story));
			md->message_payloads = ikev2_decode_payloads(md,
								     &md->message_pbs,
								     md->hdr.isa_np);
			if (md->message_payloads.n != v2N_NOTHING_WRONG) {
				/*
				 * Only respond if the message is an
				 * IKE_SA_INIT request.
				 *
				 * An IKE_SA_INIT response, like any
				 * other response, should never
				 * trigger a further response
				 * (ignoring an exception that doesn't
				 * apply here).
				 *
				 * For any other request (IKE_AUTH,
				 * CHILD_SA_..., ...), since this end
				 * is only allowed to respond after
				 * the SK payload has been verified,
				 * things must simply be dropped.
				 */
				if (ix == ISAKMP_v2_IKE_SA_INIT &&
				    v2_msg_role(md) == MESSAGE_REQUEST) {
					chunk_t data = chunk(md->message_payloads.data,
							     md->message_payloads.data_size);
					send_v2N_response_from_md(md, md->message_payloads.n,
								  &data);
				}
				/* replace (*mdp)->st with st ... */
				complete_v2_state_transition((*mdp)->st, mdp, STF_FAIL);
				return;
			}
		}

		/*
		 * Check the message payloads are as expected.
		 */
		struct ikev2_payload_errors message_payload_errors
			= ikev2_verify_payloads(md, &md->message_payloads,
						&svm->message_payloads);
		if (message_payload_errors.bad) {
			/* Save this failure for later logging. */
			message_payload_status = message_payload_errors;
			continue;
		}

		/*
		 * If there is no SK (or SKF) payload then checking is
		 * complete and things have matched.
		 *
		 * (.seen&(P(SK)|P(SKF))!=0 is equivalent.
		 */
		if (!(svm->message_payloads.required & P(SK))) {
			break;
		}

		/*
		 * SK payloads require state.
		 */
		passert(st != NULL);

		/*
		 * Since the encrypted payload appears plausible, deal
		 * with fragmentation.
		 */
		if (!md->encrypted_payloads.parsed) {
			/*
			 * Deal with fragmentation.  The function
			 * returns FALSE either when there are more
			 * fragments, the fragment is corrupt, the
			 * fragment is a duplicate, or the fragment
			 * count changed (it also drops all
			 * fragments).  Either way stop processing.
			 *
			 * Only upon _first_ arrival of the last
			 * fragment, does the function return TRUE.
			 * The the processing flow below can then
			 * continue to the SKEYSEED check.
			 *
			 * However, if SKEYSEED (g^{xy}) needed to be
			 * computed then this code will be re-entered
			 * with all fragments present (so "the"
			 * function should not be called).
			 */
			bool have_all_fragments =
				(st->st_v2_rfrags != NULL &&
				 st->st_v2_rfrags->count == st->st_v2_rfrags->total);
			/*
			 * XXX: Because fragments are only checked
			 * all-at-once after they have all arrived, a
			 * single corrupt fragment will cause all
			 * fragments being thrown away, and the entire
			 * process re-start (Is this tested?)
			 *
			 * XXX: This code should instead check
			 * fragments as they arrive.  That means
			 * kicking off the g^{xy} calculation in the
			 * background (if it were in the forground,
			 * the fragments would be dropped).  Later.
			 */
			if (md->message_payloads.present & P(SKF)) {
				if (have_all_fragments) {
					DBG(DBG_CONTROL,
					    DBG_log("already have all fragments, skipping fragment collection"));
				} else if (!ikev2_collect_fragment(md, st)) {
					return;
				}
			}
			/*
			 * For this state transition, does it only
			 * apply when there's no SKEYSEED?  If so, and
			 * SKEYSEED is missing, then things match; else
			 * things can't match.
			 */
			if (svm->flags & SMF2_NO_SKEYSEED) {
				if (ike->sa.hidden_variables.st_skeyid_calculated) {
					continue;
				} else {
					break;
				}
			}
			/*
			 * Decrypt the packet, checking it for
			 * integrity.  Anything lacking integrity is
			 * dropped.
			 */
			if (!ikev2_decrypt_msg(st, md)) {
				rate_log("encrypted payload seems to be corrupt; dropping packet");
				/*
				 * XXX: Setting/clearing md->st is to
				 * prop up nested code needing ST but
				 * not having it as a parameter.
				 */
				md->st = st;
				/* replace (*mdp)->st with st ... */
				complete_v2_state_transition((*mdp)->st, mdp, STF_IGNORE);
				return;
			}
			/*
			 * Unpack the protected (but possibly not
			 * authenticated) contents.
			 *
			 * When unpacking an AUTH packet, the other
			 * end hasn't yet been authenticated (and an
			 * INFORMATIONAL exchange immediately
			 * following AUTH be due to failed
			 * authentication).
			 *
			 * If there's something wrong, then the IKE SA
			 * gets abandoned, but a new new one may be
			 * initiated.
			 *
			 * See "2.21.2.  Error Handling in IKE_AUTH"
			 * and "2.21.3.  Error Handling after IKE SA
			 * is Authenticated".
			 *
			 * For UNSUPPORTED_CRITICAL_PAYLOAD, while the
			 * RFC clearly states that for the initial
			 * exchanges and an INFORMATIONAL exchange
			 * immediately following, the notification
			 * causes a delete, it says nothing for
			 * exchanges that follow.
			 *
			 * For moment treat it the same ?!?!?!.  Given
			 * the PAYLOAD ID that should identify the
			 * problem isn't being returned this is the
			 * least of our problems.
			 */
			struct payload_digest *sk = md->chain[ISAKMP_NEXT_v2SK];
			md->encrypted_payloads = ikev2_decode_payloads(md, &sk->pbs,
								       sk->payload.generic.isag_np);
			if (md->encrypted_payloads.n != v2N_NOTHING_WRONG) {
				switch (v2_msg_role(md)) {
				case MESSAGE_REQUEST:
				{
					chunk_t data = chunk(md->encrypted_payloads.data,
							     md->encrypted_payloads.data_size);
					send_v2N_response_from_state(ike_sa(st), *mdp,
								     md->encrypted_payloads.n,
								     &data);
					break;
				}
				case MESSAGE_RESPONSE:
					/* drop packet */
					break;
				default:
					bad_case(v2_msg_role(md));
				}
				/*
				 * XXX: Setting/clearing md->st is to
				 * prop up nested code needing ST but
				 * not having it as a parameter.
				 */
				md->st = st;
				/* replace (*mdp)->st with st ... */
				complete_v2_state_transition((*mdp)->st, mdp, STF_FATAL);
				return;
			}
		} /* else { go ahead } */
		struct ikev2_payload_errors encrypted_payload_errors
			= ikev2_verify_payloads(md, &md->encrypted_payloads,
						&svm->encrypted_payloads);
		if (encrypted_payload_errors.bad) {
			/* Save this failure for later logging. */
			encrypted_payload_status = encrypted_payload_errors;
			continue;
		}

		if (svm->state != from_state->fs_kind && ix == ISAKMP_v2_CREATE_CHILD_SA) {
			/*
			 * The IKE SA is receiving a CREATE_CHILD_SA
			 * request.  Unlike STATE_PARENT_R0 (and the
			 * initial responder) the R0 state isn't
			 * obvious - rekey IKE SA, rekey CHILD SA, and
			 * create CHILD SA are all slightly different.
			 *
			 * The code deals with this by ignoring the
			 * from_state, and then later, forcing MD's
			 * from state to values in the table.
			 */
			dbg("state #%lu forced to match CREATE_CHILD_SA from %s->%s by ignoring from state",
			    st->st_serialno,
			    enum_short_name(&state_names, svm->state),
			    enum_short_name(&state_names, svm->next_state));
		}

		/* must be the right state machine entry */
		break;
	}

	DBG(DBG_CONTROL, DBG_log("selected state microcode %s", svm->story));

	/* no useful state microcode entry? */
	if (svm->state == STATE_IKEv2_ROOF) {
		DBG(DBG_CONTROL, DBG_log("no useful state microcode entry found"));
		/* count all the error notifications */
		for (struct payload_digest *ntfy = md->chain[ISAKMP_NEXT_v2N];
		     ntfy != NULL; ntfy = ntfy->next) {
			pstat(ikev2_recv_notifies_e, ntfy->payload.v2n.isan_type);
		}
		if (message_payload_status.bad) {
			ikev2_log_payload_errors(st, md, &message_payload_status);
			/* replace (*mdp)->st with st ... */
			complete_v2_state_transition((*mdp)->st, mdp, STF_FAIL + v2N_INVALID_SYNTAX);
		} else if (encrypted_payload_status.bad) {
			ikev2_log_payload_errors(st, md, &encrypted_payload_status);
			/* replace (*mdp)->st with st ... */
			complete_v2_state_transition((*mdp)->st, mdp, STF_FAIL + v2N_INVALID_SYNTAX);
		} else if (!(md->hdr.isa_flags & ISAKMP_FLAGS_v2_MSG_R)) {
			/*
			 * We are the responder to this message so
			 * return something.
			 *
			 * XXX: For an encrypted response, wouldn't
			 * syntax error be better?  The IKE SPI is
			 * valid!
			 */
			if (st != NULL)
				send_v2N_response_from_state(ike_sa(st), md,
							     v2N_INVALID_IKE_SPI,
							     NULL/*no data*/);
			else
				send_v2N_response_from_md(md, v2N_INVALID_IKE_SPI,
							  NULL/*no data*/);
		}
		return;
	}

	md->from_state = svm->state;
	md->svm = svm;

	if (ix == ISAKMP_v2_CREATE_CHILD_SA) {
		/*
		 * XXX: This code was embedded in the end of the FSM
		 * search loop.  Since it was always executed when the
		 * state matches, move it out of the loop.  Suspect
		 * this, and the code below, really belong in the
		 * state transition function proper.
		 *
		 * XXX: Setting/clearing md->st is to preserve
		 * existing behaviour (what ever that was).
		 */
		md->st = st;
		struct state *pst = IS_CHILD_SA(md->st) ?
			state_with_serialno(md->st->st_clonedfrom) : md->st;
		/* going to switch to child st. before that update parent */
		if (!LHAS(pst->hidden_variables.st_nat_traversal, NATED_HOST))
			update_ike_endpoints(pst, md);
		md->st = NULL;

		/* bit further processing of create CREATE_CHILD_SA exchange */

		/* let's get a child state either new or existing to proceed */
		struct state *cst = process_v2_child_ix(md, st);
		if (cst == NULL) {
			/* no go. Could improve the status code? */
			/* replace (*mdp)->st with st ... */
			complete_v2_state_transition((*mdp)->st, mdp, STF_FAIL);
			return;
		}

		md->st = st;
		dbg("Message ID: why update IKE #%lu and not CHILD #%lu?",
		    st->st_serialno, cst->st_serialno);
		v2_msgid_update_counters(st, md);

		/* switch from parent state to child state */
		dbg("switching from parent? #%lu to child #%lu in FSM processor",
		    st->st_serialno, cst->st_serialno);
		st = cst;
	}

	md->st = st;

	DBG(DBG_CONTROL,
	    DBG_log("Now let's proceed with state specific processing"));

	DBG(DBG_PARSING, {
		    if (pbs_left(&md->message_pbs) != 0)
			    DBG_log("removing %d bytes of padding",
				    (int) pbs_left(&md->message_pbs));
	    });

	md->message_pbs.roof = md->message_pbs.cur;	/* trim padding (not actually legit) */

	DBG(DBG_CONTROL,
	    DBG_log("calling processor %s", svm->story));

	/*
	 * XXX: the initial responder has ST==NULL!  But that's ok as
	 * statetime_start() will fudge up a statetime_t for the
	 * not-yet-created state.
	 */
	statetime_t start = statetime_start(st);
	stf_status e = svm->processor(st, md);
	statetime_stop(&start, "processing: %s", svm->story);

	/*
	 * Processor may screw around with md->st, for instance
	 * switching it to the CHILD SA, or a newly created state.
	 * Hence use that version for now.
	 */

	/* replace (*mdp)->st with st ... */
	complete_v2_state_transition((*mdp)->st, mdp, e);
	/* our caller with release_any_md(mdp) */
}

static bool decode_peer_id_counted(struct ike_sa *ike,
				   struct msg_digest *md, int depth)
{
	if (depth > 10) {
		/* should not happen, but it would be nice to survive */
		libreswan_log("decoding IKEv2 peer ID failed due to confusion");
		return FALSE;
	}
	bool initiator = (md->hdr.isa_flags & ISAKMP_FLAGS_v2_MSG_R) != 0;
	struct connection *c = ike->sa.st_connection;

	struct payload_digest *const id_him = initiator ?
		md->chain[ISAKMP_NEXT_v2IDr] : md->chain[ISAKMP_NEXT_v2IDi];

	if (id_him == NULL) {
		libreswan_log("IKEv2 mode no peer ID (hisID)");
		return FALSE;
	}

	enum ike_id_type hik = id_him->payload.v2id.isai_type;	/* His Id Kind */

	struct id peer_id;

	if (!extract_peer_id(hik, &peer_id, &id_him->pbs)) {
		libreswan_log("IKEv2 mode peer ID extraction failed");
		return FALSE;
	}

	/* You Tarzan, me Jane? */
	struct payload_digest *const tarzan_pld = md->chain[ISAKMP_NEXT_v2IDr];

	struct id tarzan_id;
	struct id *tip = NULL;

	if (!initiator && tarzan_pld != NULL) {
		/*
		 * ??? problem with diagnostics: what we're calling "peer ID"
		 * is really our "peer's peer ID", in other words us!
		 */
		DBG(DBG_CONTROL, DBG_log("received IDr payload - extracting our alleged ID"));
		if (!extract_peer_id(tarzan_pld->payload.v2id.isai_type,
		     &tarzan_id, &tarzan_pld->pbs))
		{
			libreswan_log("Peer IDr payload extraction failed");
			return FALSE;
		}
		tip = &tarzan_id;
	}

	/*
	 * If there ar certs, try re-running the id check
	 */
	if (!ike->sa.st_peer_alt_id &&
	    ike->sa.st_remote_certs.verified != NULL) {
		if (match_certs_id(ike->sa.st_remote_certs.verified,
				   &c->spd.that.id,
				   c/*update-connection*/)) {
			dbg("X509: CERT and ID matches current connection");
			ike->sa.st_peer_alt_id = true;
		} else {
			if (initiator) {
				/* cannot switch connection so fail */
				libreswan_log("X509: CERT payload does not match connection ID");
				return FALSE;
			} else {
				dbg("X509: CERT payload does not match connection ID");
			}
		}
	}

	/* process any CERTREQ payloads */
	ikev2_decode_cr(md);

	/*
	 * Now that we've decoded the ID payload, let's see if we
	 * need to switch connections.
	 * We must not switch horses if we initiated:
	 * - if the initiation was explicit, we'd be ignoring user's intent
	 * - if opportunistic, we'll lose our HOLD info
	 */
	if (initiator) {
		if (!ike->sa.st_peer_alt_id &&
		    !same_id(&c->spd.that.id, &peer_id) &&
		    c->spd.that.id.kind != ID_FROMCERT) {
			char expect[IDTOA_BUF],
			     found[IDTOA_BUF];

			idtoa(&c->spd.that.id, expect, sizeof(expect));
			idtoa(&peer_id, found, sizeof(found));
			loglog(RC_LOG_SERIOUS,
				"we require IKEv2 peer to have ID '%s', but peer declares '%s'",
				expect, found);
			return FALSE;
		} else if (c->spd.that.id.kind == ID_FROMCERT) {
			if (peer_id.kind != ID_DER_ASN1_DN) {
				loglog(RC_LOG_SERIOUS, "peer ID is not a certificate type");
				return FALSE;
			}
			duplicate_id(&c->spd.that.id, &peer_id);
		}
	} else {
		/* why should refine_host_connection() update this? We pulled it from their packet */
		bool fromcert = peer_id.kind == ID_DER_ASN1_DN;
		uint16_t auth = md->chain[ISAKMP_NEXT_v2AUTH]->payload.v2a.isaa_type;
		enum keyword_authby authby = AUTH_NEVER;

		switch (auth) {
		case IKEv2_AUTH_RSA:
			authby = AUTH_RSASIG;
			break;
		case IKEv2_AUTH_PSK:
			authby = AUTH_PSK;
			break;
		case IKEv2_AUTH_NULL:
			authby = AUTH_NULL;
			break;
		case IKEv2_AUTH_DIGSIG:
			if (c->policy & POLICY_RSASIG) {
				authby = AUTH_RSASIG;
				break;
			}
			if (c->policy & POLICY_ECDSA) {
				authby = AUTH_ECDSA;
				break;
			}
			/* FALL THROUGH */
		case IKEv2_AUTH_NONE:
		default:
			DBG(DBG_CONTROL, DBG_log("ikev2 skipping refine_host_connection due to unknown policy"));
		}

		if (authby != AUTH_NEVER) {
			struct connection *r = NULL;

			if (authby != AUTH_NULL) {
				r = refine_host_connection(
					md->st, &peer_id, tip, FALSE /*initiator*/,
					LEMPTY /* auth_policy */, authby, &fromcert);
			}

			if (r == NULL) {
				char buf[IDTOA_BUF];

				idtoa(&peer_id, buf, sizeof(buf));
				DBG(DBG_CONTROL, DBG_log(
					"no suitable connection for peer '%s'", buf));
				/* can we continue with what we had? */
				if (!ike->sa.st_peer_alt_id &&
				    !same_id(&c->spd.that.id, &peer_id) &&
				    c->spd.that.id.kind != ID_FROMCERT)
				{
					if (LIN(POLICY_AUTH_NULL, c->policy) && tarzan_pld != NULL && tarzan_id.kind == ID_NULL) {
						libreswan_log("Peer ID '%s' expects us to have ID_NULL and connection allows AUTH_NULL - allowing",
							buf);
						ike->sa.st_peer_wants_null = TRUE;
						r = c;
					} else {
						libreswan_log("Peer ID '%s' mismatched on first found connection and no better connection found",
							buf);
						return FALSE;
					}
				} else {
					DBG(DBG_CONTROL, DBG_log("Peer ID matches and no better connection found - continuing with existing connection"));
					r = c;
				}
			}

			if (r != c) {
				char b1[CONN_INST_BUF];
				char b2[CONN_INST_BUF];

				/* apparently, r is an improvement on c -- replace */

				libreswan_log("switched from \"%s\"%s to \"%s\"%s",
					c->name,
					fmt_conn_instance(c, b1),
					r->name,
					fmt_conn_instance(r, b2));
				if (r->kind == CK_TEMPLATE || r->kind == CK_GROUP) {
					/* instantiate it, filling in peer's ID */
					r = rw_instantiate(r, &c->spd.that.host_addr,
						   NULL, &peer_id);
				}

				update_state_connection(md->st, r);
				/* redo from scratch so we read and check CERT payload */
				DBG(DBG_X509, DBG_log("retrying ikev2_decode_peer_id_and_certs() with new conn"));
				return decode_peer_id_counted(ike, md, depth + 1);

			} else if (c->spd.that.has_id_wildcards) {
				duplicate_id(&c->spd.that.id, &peer_id);
				c->spd.that.has_id_wildcards = FALSE;
			} else if (fromcert) {
				DBG(DBG_X509, DBG_log("copying ID for fromcert"));
				duplicate_id(&c->spd.that.id, &peer_id);
			}
		}
	}

	char idbuf[IDTOA_BUF];

	DBG(DBG_CONTROL, {
		dntoa_or_null(idbuf, IDTOA_BUF, c->spd.this.ca, "%none");
		DBG_log("offered CA: '%s'", idbuf);
	});

	idtoa(&peer_id, idbuf, sizeof(idbuf));

	if (!(c->policy & POLICY_OPPORTUNISTIC)) {
		libreswan_log("IKEv2 mode peer ID is %s: '%s'",
			enum_show(&ikev2_idtype_names, hik),
			idbuf);
	} else {
		DBG(DBG_OPPO, DBG_log("IKEv2 mode peer ID is %s: '%s'",
			enum_show(&ikev2_idtype_names, hik),
			idbuf));
	}

	return TRUE;
}

bool ikev2_decode_peer_id(struct msg_digest *md)
{
	return decode_peer_id_counted(ike_sa(md->st), md, 0);
}

/*
 * This logs to the main log (including peerlog!) the authentication
 * and encryption keys for an IKEv2 SA.  This is done in a format that
 * is compatible with tcpdump 4.0's -E option.
 *
 * The peerlog will be perfect.  The syslog will require that a cut
 * command is used to remove the initial text.
 * DANGER: this intentionally leaks cryptographic secrets.
 */
void ikev2_log_parentSA(const struct state *st)
{
	DBG(DBG_PRIVATE,
	{
		if (st->st_oakley.ta_integ == NULL ||
		    st->st_oakley.ta_encrypt == NULL)
			return;

		/* format initiator SPI */
		char tispi[3 + 2*IKE_SA_SPI_SIZE];
		(void)datatot(st->st_ike_spis.initiator.bytes, sizeof(st->st_ike_spis.initiator.bytes),
			'x',
			tispi, sizeof(tispi));

		/* format responder SPI */
		char trspi[3 + 2*IKE_SA_SPI_SIZE];
		(void)datatot(st->st_ike_spis.responder.bytes, sizeof(st->st_ike_spis.responder.bytes),
			'x',
			trspi, sizeof(trspi));

		const char *authalgo = st->st_oakley.ta_integ->integ_tcpdump_name;
		const char *encalgo = st->st_oakley.ta_encrypt->encrypt_tcpdump_name;

		/*
		 * Text of encryption key length (suffix for encalgo).
		 * No more than 3 digits, but compiler fears it might be 5.
		 */
		char tekl[6] = "";
		if (st->st_oakley.enckeylen != 0)
			snprintf(tekl, sizeof(tekl), "%u",
				 st->st_oakley.enckeylen);

		/* v2 IKE authentication key for initiator (256 bit bound) */
		chunk_t ai = chunk_from_symkey("ai", st->st_skey_ai_nss);
		char tai[3 + 2 * BYTES_FOR_BITS(256)] = "";
		(void)datatot(ai.ptr, ai.len, 'x', tai, sizeof(tai));
		free_chunk_contents(&ai);

		/* v2 IKE encryption key for initiator (256 bit bound) */
		chunk_t ei = chunk_from_symkey("ei", st->st_skey_ei_nss);
		char tei[3 + 2 * BYTES_FOR_BITS(256)] = "";
		(void)datatot(ei.ptr, ei.len, 'x', tei, sizeof(tei));
		free_chunk_contents(&ei);

		DBG_log("ikev2 I %s %s %s:%s %s%s:%s",
			tispi, trspi,
			authalgo, tai,
			encalgo, tekl, tei);

		/* v2 IKE authentication key for responder (256 bit bound) */
		chunk_t ar = chunk_from_symkey("ar", st->st_skey_ar_nss);
		char tar[3 + 2 * BYTES_FOR_BITS(256)] = "";
		(void)datatot(ar.ptr, ar.len, 'x', tar, sizeof(tar));
		free_chunk_contents(&ar);

		/* v2 IKE encryption key for responder (256 bit bound) */
		chunk_t er = chunk_from_symkey("er", st->st_skey_er_nss);
		char ter[3 + 2 * BYTES_FOR_BITS(256)] = "";
		(void)datatot(er.ptr, er.len, 'x', ter, sizeof(ter));
		free_chunk_contents(&er);

		DBG_log("ikev2 R %s %s %s:%s %s%s:%s",
			tispi, trspi,
			authalgo, tar,
			encalgo, tekl, ter);
	}
	);
}

static void schedule_next_send(struct state *st)
{
	struct initiate_list *p;
	struct state *cst = NULL;
	int i = 1;

	if (st->send_next_ix != NULL) {
		p = st->send_next_ix;
		cst = state_with_serialno(p->st_serialno);
		if (cst != NULL) {
			event_force(EVENT_v2_SEND_NEXT_IKE, cst);
			DBG(DBG_CONTROLMORE,
				DBG_log("#%lu send next using parent #%lu next message id=%u, waiting to send %d",
					cst->st_serialno, st->st_serialno,
					st->st_msgid_nextuse, i));
		}
		st->send_next_ix = st->send_next_ix->next;
		pfree(p);
	}
}

/*
 * Maintain or reset Message IDs.
 *
 * When resetting, need to fudge things up sufficient to fool
 * ikev2_update_msgid_counters(() into thinking that this is a shiny
 * new init request.
 */

void v2_msgid_restart_init_request(struct state *st, struct msg_digest *md)
{
	dbg("restarting Message ID of state #%lu", st->st_serialno);
	/* Ok? */
	st->st_msgid_lastack = v2_INVALID_MSGID;
	st->st_msgid_lastrecv = v2_INVALID_MSGID;
	st->st_msgid_nextuse = 0;
	st->st_msgid = 0;
	/*
	 * XXX: Why?!?
	 *
	 * Shouldn't the state transitions STATE_PARENT_I0 ->
	 * STATE_PARENT_I1 and STATE_PARENT_I1 -> STATE_PARENT_I1 be
	 * functionally 'identical'.
	 *
	 * Yes.  Unfortunately the code below does all sorts of magic
	 * involving the state's magic number and assumed attributes.
	 */
	md->svm = finite_states[STATE_PARENT_I0]->fs_v2_transitions;
	change_state(st, STATE_PARENT_I0);
	/*
	 * XXX: Why?!?
	 *
	 * Shouldn't MD be ignored!  After all it could be NULL.
	 *
	 * Yes.  unfortunately the code below still assumes that
	 * there's always an MD (the initiator does not have an MD so
	 * fake_md() and tries to use MD attributes to make decisions
	 * that belong in the state transition.
	 */
	if (md != NULL) {
		md->hdr.isa_flags &= ~ISAKMP_FLAGS_v2_MSG_R;
	}
}

/*
 * While there's always a state, there may not always be an incomming
 * message.  Hence, don't rely on md->st and instead explicitly pass
 * in ST.
 *
 * XXX: Should this looking at .st_state_transition->flags to decide
 * what to do?
 */
void v2_msgid_update_counters(struct state *st, struct msg_digest *md)
{
	if (st == NULL) {
		dbg("Message ID: current processor deleted the state nothing to update");
		return;
	}
	struct ike_sa *ike = ike_sa(st);

	/* message ID sequence for things we send (as initiator) */
	msgid_t st_msgid_lastack = ike->sa.st_msgid_lastack;
	msgid_t st_msgid_nextuse = ike->sa.st_msgid_nextuse;
	/* message ID sequence for things we receive (as responder) */
	msgid_t st_msgid_lastrecv = ike->sa.st_msgid_lastrecv;
	msgid_t st_msgid_lastreplied = ike->sa.st_msgid_lastreplied;

	/* update when sending a request */
	if (is_msg_request(md) &&
			(st->st_state == STATE_PARENT_I1 ||
			 st->st_state == STATE_V2_REKEY_IKE_I ||
			 st->st_state == STATE_V2_REKEY_CHILD_I ||
			 st->st_state == STATE_V2_CREATE_I)) {
		ike->sa.st_msgid_nextuse += 1;
		/* an informational exchange does its own increment */
	} else if (st->st_state == STATE_PARENT_I2) {
		ike->sa.st_msgid_nextuse += 1;
	}

	if (is_msg_response(md)) {
		/* we were initiator for this message exchange */
		if (md->hdr.isa_msgid == v2_FIRST_MSGID &&
				ike->sa.st_msgid_lastack == v2_INVALID_MSGID) {
			ike->sa.st_msgid_lastack = md->hdr.isa_msgid;
		} else if (md->hdr.isa_msgid > ike->sa.st_msgid_lastack) {
			ike->sa.st_msgid_lastack = md->hdr.isa_msgid;
		} /* else { lowever message id ignore it? } */
	} else {
		/* we were responder for this message exchange */
		if (md->hdr.isa_msgid > ike->sa.st_msgid_lastrecv) {
			ike->sa.st_msgid_lastrecv = md->hdr.isa_msgid;
		}
		/* first request from the other side */
		if (md->hdr.isa_msgid == v2_FIRST_MSGID &&
				ike->sa.st_msgid_lastrecv == v2_INVALID_MSGID) {
			ike->sa.st_msgid_lastrecv = v2_FIRST_MSGID;
		}
	}

	{
		msgid_t unack = ike->sa.st_msgid_nextuse -
			ike->sa.st_msgid_lastack - 1;

		if (unack < ike->sa.st_connection->ike_window) {
			schedule_next_send(&ike->sa);
		}
	}

	LSWDBGP(DBG_BASE, buf) {
		lswlogf(buf, "Message ID: '%s' IKE #%lu %s",
			st->st_connection->name,
			ike->sa.st_serialno, ike->sa.st_finite_state->fs_short_name);
		if (&ike->sa != st) {
			lswlogf(buf, "; CHILD #%lu %s",
				st->st_serialno, st->st_finite_state->fs_short_name);
		}
		lswlogf(buf, "; message-%s msgid=%u",
			is_msg_response(md) ? "resonse" : "request",
			md->hdr.isa_msgid);

		lswlogf(buf, "; initiator { lastack=%u", st_msgid_lastack);
		if (st_msgid_lastack != ike->sa.st_msgid_lastack) {
			lswlogf(buf, "->%u", ike->sa.st_msgid_lastack);
		}
		lswlogf(buf, " nextuse=%u", st_msgid_nextuse);
		if (st_msgid_nextuse != ike->sa.st_msgid_nextuse) {
			lswlogf(buf, "->%u", ike->sa.st_msgid_nextuse);
		}
		lswlogf(buf, " } responder { lastrecv=%u", st_msgid_lastrecv);
		if (st_msgid_lastrecv != ike->sa.st_msgid_lastrecv) {
			lswlogf(buf, "->%u", ike->sa.st_msgid_lastrecv);
		}
		lswlogf(buf, " lastreplied=%u", st_msgid_lastreplied);
		if (st_msgid_lastreplied != ike->sa.st_msgid_lastreplied) {
			lswlogf(buf, "->%u", ike->sa.st_msgid_lastreplied);
		}
		lswlogf(buf, " }");
	}
}

void log_ipsec_sa_established(const char *m, const struct state *st)
{
	/* log Child SA Traffic Selector details for admin's pleasure */
	const struct traffic_selector *a = &st->st_ts_this;
	const struct traffic_selector *b = &st->st_ts_that;
	char ba[RANGETOT_BUF], bb[RANGETOT_BUF];

	rangetot(&a->net, 0, ba, sizeof(ba));
	rangetot(&b->net, 0, bb, sizeof(bb));
	libreswan_log("%s [%s:%d-%d %d] -> [%s:%d-%d %d]",
			m,
			ba,
			a->startport,
			a->endport,
			a->ipprotoid,
			bb,
			b->startport,
			b->endport,
			b->ipprotoid);

	pstats_ipsec_sa++;
}

static void ikev2_child_emancipate(struct msg_digest *md)
{
	/* st grow up to be an IKE parent. not child anymore.  */

	struct child_sa *to = pexpect_child_sa(md->st);
	struct ike_sa *from = ike_sa(md->st);

	/* initialze the the new IKE SA. reset and message ID */
	to->sa.st_clonedfrom = SOS_NOBODY;
	to->sa.st_msgid_lastack = v2_INVALID_MSGID;
	to->sa.st_msgid_lastrecv = v2_INVALID_MSGID;
	to->sa.st_msgid_nextuse = v2_FIRST_MSGID;

	/* Switch to the new IKE SPIs */
	to->sa.st_ike_spis = to->sa.st_ike_rekey_spis;
	rehash_state_cookies_in_db(&to->sa);

	/* TO has correct IKE_SPI so can migrate */
	v2_migrate_children(from, to);

	/* child is now a parent */
	ikev2_ike_sa_established(pexpect_ike_sa(&to->sa), md->svm,
				 md->svm->next_state);
}

static void success_v2_state_transition(struct state *st, struct msg_digest *md)
{
	const struct state_v2_microcode *svm = md->svm;
	enum state_kind from_state = md->from_state;
	struct connection *c = st->st_connection;
	struct state *pst;
	enum rc_type w;

	pst = IS_CHILD_SA(st) ? state_with_serialno(st->st_clonedfrom) : st;

	if (from_state != svm->next_state) {
		DBG(DBG_CONTROL, DBG_log("IKEv2: transition from state %s to state %s",
			      enum_name(&state_names, from_state),
			      enum_name(&state_names, svm->next_state)));
	}


	if (from_state == STATE_V2_REKEY_IKE_R ||
	    from_state == STATE_V2_REKEY_IKE_I) {
		dbg("Message ID: updating counters for #%lu before emancipating",
		    md->st->st_serialno);
		v2_msgid_update_counters(md->st, md);
		ikev2_child_emancipate(md);
	} else  {
		change_state(st, svm->next_state);
		dbg("Message ID: updating counters for #%lu after switching state",
		    md->st->st_serialno);
		v2_msgid_update_counters(md->st, md);
	}

	w = RC_NEW_STATE + st->st_state;

	/*
	 * tell whack and log of progress; successful state
	 * transitions always advance (even when they go round to the
	 * same state).
	 */
	passert(st->st_state >= STATE_IKEv2_FLOOR);
	passert(st->st_state <  STATE_IKEv2_ROOF);

	void (*log_details)(struct lswlog *buf, struct state *st);
	if (IS_CHILD_SA_ESTABLISHED(st)) {
		log_ipsec_sa_established("negotiated connection", st);
		log_details = lswlog_child_sa_established;
		/* log our success and trigger detach */
		w = RC_SUCCESS;
	} else if (st->st_state == STATE_PARENT_I2 || st->st_state == STATE_PARENT_R1) {
		log_details = lswlog_ike_sa_established;
	} else {
		log_details = NULL;
	}

	/* tell whack and logs our progress - unless OE, then be quiet*/
	if (c == NULL || (c->policy & POLICY_OPPORTUNISTIC) == LEMPTY) {
		LSWLOG_RC(w, buf) {
			lswlogf(buf, "%s: %s", st->st_finite_state->fs_name,
				st->st_finite_state->fs_story);
			/* document SA details for admin's pleasure */
			if (log_details != NULL) {
				log_details(buf, st);
			}
		}
	}

	/* if requested, send the new reply packet */
	if (svm->flags & SMF2_SEND) {
		/*
		 * Adjust NAT but not for initial state (initial
		 * outbound message?).
		 *
		 * ??? why should STATE_PARENT_I1 be excluded?  XXX:
		 * and why, for that state, does ikev2_natd_lookup()
		 * call it.
		 *
		 * XXX: The "initial outbound message" check was first
		 * added by commit "pluto: various fixups associated
		 * with RFC 7383 code".  At the time a fake MD
		 * (created when an initiator initiates) had the magic
		 * state STATE_IKEv2_BASE and so it checked for that.
		 * What isn't clear is if the check was intended to
		 * block just an IKE SA initiating, or also block a
		 * CHILD SA initiate.
		 *
		 * XXX: STATE_PARENT_R1 (AUTH responder), in addition
		 * to the below, will also call nat*() explicitly.
		 * Perhaps multiple calls are benign?
		 *
		 * XXX: This is getting silly:
		 *
		 * - check for MD != NULL - while initial initiators
		 * don't have an incomming message it gets twarted by
		 * fake_md()
		 *
		 * - delete the call - IKE state transition code is
		 * already somewhat doing this and why would nat need
		 * to be updated during a child exchange
		 *
		 * - or what about an STF flag on the state?
		 */
		bool new_request = (from_state == STATE_PARENT_I0 ||
				    from_state == STATE_V2_CREATE_I0 ||
				    from_state == STATE_V2_REKEY_CHILD_I0 ||
				    from_state == STATE_V2_REKEY_IKE_I0);
		if (nat_traversal_enabled &&
		    !new_request &&
		    from_state != STATE_PARENT_R0 &&
		    from_state != STATE_PARENT_I1) {
			/* adjust our destination port if necessary */
			nat_traversal_change_port_lookup(md, pst);
		}

		DBG(DBG_CONTROL, {
			    ipstr_buf b;
			    DBG_log("sending V2 %s packet to %s:%u (from port %u)",
				    new_request ? "new request" :
				    "reply", ipstr(&st->st_remoteaddr, &b),
				    st->st_remoteport,
				    st->st_interface->port);
		    });

		send_recorded_v2_ike_msg(pst, enum_name(&state_names, from_state));
	}

	if (w == RC_SUCCESS) {
		DBG(DBG_CONTROL, DBG_log("releasing whack for #%lu (sock="PRI_FD")",
					 st->st_serialno, PRI_fd(st->st_whack_sock)));
		release_whack(st);

		/* XXX should call unpend again on parent SA */
		if (IS_CHILD_SA(st)) {
			/* with failed child sa, we end up here with an orphan?? */
			struct state *pst = state_with_serialno(st->st_clonedfrom);

			DBG(DBG_CONTROL, DBG_log("releasing whack and unpending for parent #%lu",
				pst->st_serialno));
			/* a better call unpend in ikev2_ike_sa_established? */
			unpend(pst, st->st_connection);
			release_whack(pst);
		}
	}

	/* Schedule for whatever timeout is specified */
	{
		enum event_type kind = svm->timeout_event;
		struct connection *c = st->st_connection;

		switch (kind) {
		case EVENT_RETRANSMIT:
			delete_event(st);
			dbg("success_v2_state_transition scheduling EVENT_RETRANSMIT of c->r_interval=%jdms",
			    deltamillisecs(c->r_interval));
			start_retransmits(st);
			break;

		case EVENT_SA_REPLACE: /* IKE or Child SA replacement event */
			v2_schedule_replace_event(st);
			break;

		case EVENT_SO_DISCARD:
			delete_event(st);
			event_schedule_s(kind, MAXIMUM_RESPONDER_WAIT, st);
			break;

		case EVENT_NULL:
			/*
			 * Is there really no case where we want to set no  timer?
			 * more likely an accident?
			 */
			DBG_log("V2 microcode entry (%s) has unspecified timeout_event",
					svm->story);
			break;

		case EVENT_RETAIN:
			/* the previous event is retained */
			break;

		default:
			bad_case(kind);
		}
		/*
		 * start liveness checks if set, making sure we only
		 * schedule once when moving from I2->I3 or R1->R2
		 */
		if (st->st_state != from_state &&
			st->st_state != STATE_UNDEFINED &&
			IS_CHILD_SA_ESTABLISHED(st) &&
			dpd_active_locally(st)) {
			DBG(DBG_DPD,
			    DBG_log("dpd enabled, scheduling ikev2 liveness checks"));
			deltatime_t delay = deltatime_max(c->dpd_delay, deltatime(MIN_LIVENESS));
			event_schedule(EVENT_v2_LIVENESS, delay, st);
		}
	}
}

static void log_stf_suspend(struct state *st, stf_status result)
{
	char b[CONN_INST_BUF];

	set_cur_state(st);      /* might have changed */

	fmt_conn_instance(st->st_connection, b);
	LSWDBGP(DBG_CONTROL, buf) {
		lswlogf(buf, "\"%s\"%s #%lu complete v2 state %s transition with ",
			st->st_connection->name, b, st->st_serialno,
			st->st_state_name);
		lswlog_v2_stf_status(buf, result);
		lswlogf(buf, " suspended from %s:%d",
			st->st_suspended_md_func,
			st->st_suspended_md_line);
	}
}

/*
 * Dependant on RESULT, either complete, suspend, abandon, or abort
 * (delete state) the state transition started by the state-specific
 * state transition function.
 *
 * Since this is function is meaningless without a state, ST really
 * should be non-NULL.
 *
 * XXX: A broken exception is when responding to an IKE_SA_INIT
 * request - the state machine calls the state transition function
 * with no state (trusting that the transition function will do the
 * job, but that isn't always true).  The fix is to create the state
 * before calling the state transition function (like is done for the
 * CHILD_SA code).
 *
 * Since, when initiating an exchange there is no message, code can't
 * assume that (*MDP) is non-NULL.
 *
 * XXX: Some state transition functions switch state part way (see
 * AUTH child code) and then tunnel the new state to this code via
 * (*MDP)->st (hence fake_md() and some callers passing in
 * (*MDP)->st).  The fix is for the AUTH code to handle the CHILD SA
 * as a nested or separate transition.
 *
 * XXX: The state transition structure (microcode) is stored in (*MDP)
 * forcing that structure to be created.  The fix is to store the
 * state's transition in the state.  As a bonus this makes determining
 * if a state is busy really really easy - if there's a
 * state-transition then it must be.
 *
 * This routine does not free (*MDP) (using release_any_md(mdp)).
 * However, when suspending a state transition, it will save it in ST
 * and zap (*MDP) so that the caller can't free it.  Hence, the caller
 * must be prepared for (*MDP) being set to NULL.
 *
 * XXX: At some point (*MDP) was being used for:
 *
 * - find st
 * - success_v2_state_transition(st, md);
 *   - for svm:
 *     - svm->next_state,
 *     - svm->flags & SMF2_SEND,
 *     - svm->timeout_event,
 *     -svm->flags, story
 *   - find from_state (st might be gone)
 *   - ikev2_update_msgid_counters(md);
 *   - nat_traversal_change_port_lookup(md, st)
 * - !(md->hdr.isa_flags & ISAKMP_FLAGS_v2_MSG_R) to gate Notify payloads/exchanges [WRONG]
 * - find note for STF_INTERNAL_ERROR
 * - find note for STF_FAIL (might not be part of result (STF_FAIL+note))
 *
 * We don't use these but complete_v1_state_transition does:
 * - record md->event_already_set
 * - remember_received_packet(st, md);
 * - fragvid, dpd, nortel
 */
void complete_v2_state_transition(struct state *st,
				  struct msg_digest **mdp,
				  stf_status result)
{
	/*
	 * XXX; until either .st becomes v1 only or is deleted.
	 */
	pexpect(mdp == NULL || *mdp == NULL || (*mdp)->st == st);

	/* statistics */
	if (result > STF_FAIL) {
		pstats(ike_stf, STF_FAIL);
	} else {
		pstats(ike_stf, result);
	}

	/*
	 * Since this is a state machine, there really should always
	 * be a state.
	 *
	 * Unfortunately #1: instead of always having a state and
	 * passing it round, state transition functions create the
	 * state locally and then try to tunnel it back using the
	 * received message's digest - *MDP->st.  The big offenders
	 * are IKE_SA_INIT and IKE_AUTH reponders
	 *
	 * Unfortunately #2: the initiator of an exchange doesn't have
	 * a received message's digest, but that's ok one is sometimes
	 * created using fake_md().
	 *
	 * Hence, expect any of MDP, *MDP, or *MDP->st to be NULL.
	 */
	struct msg_digest *md = (mdp != NULL ? (*mdp) /*NULL?*/ : NULL);
	set_cur_state(st); /* might have changed */ /* XXX: huh? */
	/* get the from state */
	const struct finite_state *from_state = (st != NULL ? st->st_finite_state
						 : finite_states[STATE_UNDEFINED]);
	const char *from_state_name = from_state->fs_name;

	/*
	 * XXX/SML:  There is no need to abort here in all cases where st is
	 * null, so moved this precondition to where it's needed.  Some previous
	 * logic appears to have been tooled to handle null state, and state might
	 * be null legitimately in certain failure cases (STF_FAIL + xxx).
	 *
	 * One condition for null state is when a new connection request packet
	 * arrives and there is no suitable matching configuration.  For example,
	 * ikev2_parent_inI1outR1() will return (STF_FAIL + NO_PROPOSAL_CHOSEN) but
	 * no state in this case.  While other failures may be better caught before
	 * this function is called, we should be graceful here.  And for this
	 * particular case, and similar failure cases, we want SEND_NOTIFICATION
	 * (below) to let the peer know why we've rejected the request.
	 *
	 * Another case of null state is return from ikev2_parent_inR1BoutI1B
	 * which returns STF_IGNORE.
	 *
	 * Another case occurs when we finish an Informational Exchange message
	 * that causes us to delete the IKE state.  In fact, that can be an
	 * STF_OK and yet have no remaining state object at this point.
	 */

	LSWDBGP(DBG_CONTROL, buf) {
		lswlogf(buf, "#%lu complete v2 state transition from %s",
			(st == NULL ? SOS_NOBODY : st->st_serialno),
			from_state->fs_short_name);
		if (md != NULL) {
			if (md->from_state != from_state->fs_kind) {
				lswlogs(buf, " md.from_state=");
				lswlog_enum_short(buf, &state_names, md->from_state);
			}
			if (md->svm != NULL) {
				if (md->svm->state != from_state->fs_kind) {
					lswlogs(buf, " svm.state=");
					lswlog_enum_short(buf, &state_names, md->svm->state);
				}
				lswlogs(buf, " to ");
				lswlog_enum_short(buf, &state_names, md->svm->next_state);
			}
		}
		lswlogf(buf, " with status ");
		lswlog_v2_stf_status(buf, result);
	}

	switch (result) {

	case STF_SUSPEND:
		if (pexpect(st != NULL)) {
			/*
			 * If this transition was triggered by an
			 * incoming packet, save it.
			 *
			 * XXX: some initiator code creates a fake MD
			 * (there isn't a real one); save that as
			 * well.
			 */
			if (*mdp != NULL) {
				suspend_md(st, mdp);
				passert(*mdp == NULL); /* ownership transfered */
			}
			log_stf_suspend(st, result);
		}
		return;

	case STF_IGNORE:
		LSWDBGP(DBG_CONTROL, buf) {
			lswlogs(buf, "complete v2 state transition with ");
			lswlog_v2_stf_status(buf, result);
		}
		return;

	case STF_OK:
		if (st == NULL) {
			DBG(DBG_CONTROL, DBG_log("STF_OK but no state object remains"));
		} else {
			/* advance the state */
			success_v2_state_transition(st, md);
		}
		break;

	case STF_INTERNAL_ERROR:
		whack_log(RC_INTERNALERR, "%s: internal error",
			  from_state_name);

		DBG(DBG_CONTROL,
		    DBG_log("state transition function for %s had internal error",
			    from_state_name));
		break;

	case STF_DROP:
		/* be vewy vewy quiet */
		if (st != NULL) {
			delete_state(st);
			md->st = st = NULL;
		}
		break;

	case STF_FATAL:
		passert(st != NULL);
		whack_log(RC_FATAL,
			  "encountered fatal error in state %s",
			  from_state_name);
		release_pending_whacks(st, "fatal error");
		delete_state(st);
		md->st = st = NULL;
		break;

	default:
		passert(result >= STF_FAIL);
		v2_notification_t notification = result > STF_FAIL ?
			result - STF_FAIL : v2N_NOTHING_WRONG;
		whack_log(RC_NOTIFICATION + notification,
			  "%s: %s",
			  from_state_name,
			  enum_name(&ikev2_notify_names, notification));

		if (notification != v2N_NOTHING_WRONG) {
			/* Only the responder sends a notification */
			if (!(md->hdr.isa_flags & ISAKMP_FLAGS_v2_MSG_R)) {
				struct state *pst = st;

				DBG(DBG_CONTROL, DBG_log("sending a notification reply"));
				/* We are the exchange responder */
				if (st != NULL && IS_CHILD_SA(st)) {
					pst = state_with_serialno(
							st->st_clonedfrom);
				}

				if (st == NULL) {
					send_v2N_response_from_md(md, notification, NULL);
				} else {
					send_v2N_response_from_state(ike_sa(pst), md,
								     notification,
								     NULL/*no data*/);
					if (md->hdr.isa_xchg == ISAKMP_v2_IKE_SA_INIT) {
						delete_state(st);
					} else {
						dbg("forcing #%lu to a discard event",
						    st->st_serialno);
						delete_event(st);
						event_schedule_s(EVENT_SO_DISCARD,
								 MAXIMUM_RESPONDER_WAIT,
								 st);
					}
				}
			}
		}

		DBG(DBG_CONTROL,
		    DBG_log("state transition function for %s failed: %s",
			    from_state_name,
			    notification == v2N_NOTHING_WRONG ?
				"<no reason given>" :
				enum_name(&ikev2_notify_names, notification)));
		break;
	}

}

v2_notification_t accept_v2_nonce(struct msg_digest *md,
				chunk_t *dest,
				const char *name)
{
	/*
	 * note ISAKMP_NEXT_v2Ni == ISAKMP_NEXT_v2Nr
	 * so when we refer to ISAKMP_NEXT_v2Ni, it might be ISAKMP_NEXT_v2Nr
	 */
	pb_stream *nonce_pbs = &md->chain[ISAKMP_NEXT_v2Ni]->pbs;
	size_t len = pbs_left(nonce_pbs);

	/*
	 * RFC 7296 Section 2.10:
	 * Nonces used in IKEv2 MUST be randomly chosen, MUST be at least 128
	 * bits in size, and MUST be at least half the key size of the
	 * negotiated pseudorandom function (PRF).  However, the initiator
	 * chooses the nonce before the outcome of the negotiation is known.
	 * Because of that, the nonce has to be long enough for all the PRFs
	 * being proposed.
	 *
	 * We will check for a minimum/maximum here. Once the PRF is selected,
	 * we verify the nonce is big enough.
	 */

	if (len < IKEv2_MINIMUM_NONCE_SIZE || len > IKEv2_MAXIMUM_NONCE_SIZE) {
		loglog(RC_LOG_SERIOUS, "%s length %zu not between %d and %d",
			name, len, IKEv2_MINIMUM_NONCE_SIZE, IKEv2_MAXIMUM_NONCE_SIZE);
		return v2N_INVALID_SYNTAX; /* ??? */
	}
	free_chunk_contents(dest);
	*dest = clone_in_pbs_left_as_chunk(nonce_pbs, "nonce");
	passert(len == dest->len);
	return v2N_NOTHING_WRONG;
}

/*
 * The role of a received (from network) message. RFC 7296 #3.1
 * "message is a response to a message containing the same Message ID."
 *
 * Separate from this is IKE role ORIGINAL_INITIATOR or ORIGINAL_RESPONDER
 * RFC 7296 2.2
 */
bool is_msg_response(const struct msg_digest *md)
{
	return (md->hdr.isa_flags & ISAKMP_FLAGS_v2_MSG_R) != 0;
}

/* message is a request */
bool is_msg_request(const struct msg_digest *md)
{
	return !is_msg_response(md);
}

void lswlog_v2_stf_status(struct lswlog *buf, unsigned status)
{
	if (status <= STF_FAIL) {
		lswlog_enum(buf, &stf_status_names, status);
	} else {
		lswlogs(buf, "STF_FAIL+");
		lswlog_enum(buf, &ikev2_notify_names, status - STF_FAIL);
	}
}

/*
 * Find the state object that match the following:
 *	st_msgid (IKEv2 Child responder state)
 *	parent duplicated from
 *	expected state
 *
 * XXX: can this use cookies?  Probably except after an IKE SA rekey
 * it isn't clear of all the children get re-hashed to the parent's
 * new slot?
 *
 * XXX: Looking at IS_CHILD_SA_RESPONDER() suggets this is testing the
 * re-key CHILD SA role, should this be looking elsewhere?
 */

struct state *v2_child_sa_responder_with_msgid(struct ike_sa *ike, msgid_t st_msgid)
{
	struct state *st = NULL;
	FOR_EACH_STATE_NEW2OLD(st) {
		if (IS_CHILD_SA(st) &&
		    st->st_clonedfrom == ike->sa.st_serialno &&
		    st->st_msgid == st_msgid) {
			if (IS_CHILD_SA_RESPONDER(st)) {
				pexpect(st->st_sa_role == SA_RESPONDER);
				return st;
			} else if (st->st_sa_role != SA_INITIATOR) {
				/*
				 * XXX: seemingly an IKE rekey can
				 * trigger this - the CHILD_SA created
				 * during the initial exchange is in
				 * state STATE_V2_IPSEC_R and that
				 * isn't covered by the above.
				 */
				/*
				 * XXX: seemingly an IKE rekey can
				 * cause this?
				 */
				LSWDBGP(DBG_BASE, buf) {
					lswlogf(buf, "child state #%lu has an unexpected SA role ",
						st->st_serialno);
					lswlog_keyname(buf, &sa_role_names, st->st_sa_role);
				}
			}
		}
	};
	dbg("no waiting child responder state matching pst #%lu msg id %u",
	    ike->sa.st_serialno, st_msgid);
	return NULL;
}

/*
 * Find the state object that match the following:
 *	st_msgid (IKE/IPsec initiator state)
 *	parent duplicated from
 *	expected state
 *
 * XXX: can this use cookies?  Probably except after an IKE SA rekey
 * it isn't clear of all the children get re-hashed to the parent's
 * new slot?
 *
 * XXX: Looking at IS_CHILD_IPSECSA_RESPONSE() suggets this is
 * checking the rekey CHILD SA exchange role.  Should it be looking
 * elsewhere?
 */

struct state *v2_child_sa_initiator_with_msgid(struct ike_sa *ike, msgid_t st_msgid)
{
	struct state *st = NULL;
	FOR_EACH_STATE_NEW2OLD(st) {
		if (IS_CHILD_SA(st) &&
		    st->st_clonedfrom == ike->sa.st_serialno &&
		    st->st_msgid == st_msgid) {
			if (IS_CHILD_IPSECSA_RESPONSE(st)) {
				pexpect(st->st_sa_role == SA_INITIATOR);
				return st;
			} else if (st->st_sa_role != SA_RESPONDER) {
				/*
				 * XXX: seemingly an IKE rekey can
				 * cause this?
				 */
				LSWDBGP(DBG_BASE, buf) {
					lswlogf(buf, "child state #%lu has an unexpected SA role ",
						st->st_serialno);
					lswlog_keyname(buf, &sa_role_names, st->st_sa_role);
				}
			}
		}
	};
	dbg("no waiting child initiator state matching pst #%lu msg id %u",
	    ike->sa.st_serialno, st_msgid);
	return NULL;
}

/* used by parent and child to emit v2N_IPCOMP_SUPPORTED if appropriate */
#include "kernel.h"
bool emit_v2N_compression(struct state *cst,
			bool OK,
			pb_stream *s)
{
	const struct connection *c = cst->st_connection;

	if ((c->policy & POLICY_COMPRESS) && OK) {
		uint16_t c_spi;

		DBG(DBG_CONTROL, DBG_log("Initiator child policy is compress=yes, sending v2N_IPCOMP_SUPPORTED for DEFLATE"));

		/* calculate and keep our CPI */
		if (cst->st_ipcomp.our_spi == 0) {
			/* CPI is stored in network low order end of an ipsec_spi_t */
			cst->st_ipcomp.our_spi = get_my_cpi(&c->spd, LIN(POLICY_TUNNEL, c->policy));
			c_spi = (uint16_t)ntohl(cst->st_ipcomp.our_spi);
			if (c_spi < IPCOMP_FIRST_NEGOTIATED) {
				/* get_my_cpi() failed */
				loglog(RC_LOG_SERIOUS, "kernel failed to calculate compression CPI (CPI=%d)", c_spi);
				return false;
			}
			DBG(DBG_CONTROL, DBG_log("Calculated compression CPI=%d", c_spi));
		} else {
			c_spi = (uint16_t)ntohl(cst->st_ipcomp.our_spi);
		}

		struct ikev2_notify_ipcomp_data d = {
			.ikev2_cpi = c_spi,
			.ikev2_notify_ipcomp_trans = IPCOMP_DEFLATE,
		};
		pb_stream d_pbs;

		bool r =
			emit_v2Npl(v2N_IPCOMP_SUPPORTED, s, &d_pbs) &&
			out_struct(&d, &ikev2notify_ipcomp_data_desc, &d_pbs, NULL);
		close_output_pbs(&d_pbs);
		return r;
	} else {
		DBG(DBG_CONTROL, DBG_log("Initiator child policy is compress=no, NOT sending v2N_IPCOMP_SUPPORTED"));
		return true;
	}
}
