#include "nat64/filtering-and-updating.h"

#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/icmpv6.h>
#include <net/tcp.h>
#include <net/icmp.h>

#include "str-utils.h"
#include "config.h"
#include "pool6.h"
#include "rfc6052.h"
#include "rfc7915/6to4.h"
#include "nat64/joold.h"
#include "nat64/pool4/db.h"
#include "nat64/bib/db.h"

enum session_fate tcp_est_expire_cb(struct session_entry *session, void *arg)
{
	switch (session->state) {
	case ESTABLISHED:
		session->state = TRANS;
		session->update_time = jiffies;
		return FATE_PROBE;

	case V4_FIN_RCV:
	case V6_FIN_RCV:
		return FATE_RM;

	case V4_INIT:
	case V6_INIT:
	case TRANS:
	case V4_FIN_V6_FIN_RCV:
		WARN(true, "State %d is never supposed to be linked to the established timeout.",
				session->state);
		return FATE_RM;
	}

	WARN(true, "Unknown state found (%d); removing session entry.",
			session->state);
	return FATE_RM;
}

static void log_entries(struct bib_session *entries)
{
	struct session_entry *session = &entries->session;

	if (entries->bib_set) {
		log_debug("BIB entry: %pI6c#%u - %pI4#%u (%s)",
				&session->src6.l3, session->src6.l4,
				&session->src4.l3, session->src4.l4,
				l4proto_to_string(session->proto));
	} else {
		log_debug("BIB entry: None");
	}

	if (entries->session_set) {
		log_debug("Session entry: %pI6c#%u - %pI6c#%u | %pI4#%u - %pI4#%u (%s)",
				&session->src6.l3, session->src6.l4,
				&session->dst6.l3, session->dst6.l4,
				&session->src4.l3, session->src4.l4,
				&session->dst4.l3, session->dst4.l4,
				l4proto_to_string(session->proto));
	} else {
		log_debug("Session entry: None");
	}
}

static int succeed(struct xlation *state)
{
	log_entries(&state->entries);

	/*
	 * Sometimes the session doesn't change as a result of the state
	 * machine's schemes.
	 * No state change, no timeout change, no update time change.
	 *
	 * One might argue that we shouldn't joold the session in those cases.
	 * It's a lot more trouble than it's worth:
	 *
	 * - Calling joold_add() on the TCP SM state functions is incorrect
	 *   because the session's update_time and expirer haven't been updated
	 *   by that point. So what gets synchronizes is half-baked data.
	 * - Calling joold_add() on decide_fate() is a freaking mess because
	 *   we'd need to send the xlator and a boolean (indicating whether this
	 *   is packet or timer context) to it and all intermediate functions,
	 *   and these functions all already have too many arguments as it is.
	 *   It's bad design anyway; the session module belongs to a layer that
	 *   shouldn't be aware of the xlator.
	 * - These special no-changes cases are rare.
	 *
	 * So let's simplify everything by just joold_add()ing here.
	 */
	if (state->entries.session_set) {
		joold_add(state->jool.nat64.joold, &state->entries.session,
				state->jool.nat64.bib);
	}

	return 0;
}

static int breakdown(struct xlation *state)
{
	kfree_skb(state->in.skb);
	/* inc_stats(&state->in, IPSTATS_MIB_INDISCARDS); */
	/*
	 * TODO
	 * Notice that I'm suppressing the original error.
	 * For now this is fine because nobody really uses the specific code for
	 * anything.
	 */
	return -EINVAL;
}

/**
 * This is just a wrapper. Its sole intent is to minimize mess below.
 */
static int xlat_dst_6to4(struct xlation *state,
		struct ipv4_transport_addr *dst4)
{
	dst4->l4 = state->in.tuple.dst.addr6.l4;
	/* Error msg already printed. */
	return rfc6052_6to4(state->jool.pool6, &state->in.tuple.dst.addr6.l3,
			&dst4->l3);
}

/**
 * This is just a wrapper. Its sole intent is to minimize mess below.
 */
static int find_mask_domain(struct xlation *state, struct mask_domain **masks)
{
	*masks = mask_domain_find(state->jool.nat64.pool4, &state->in.tuple,
			state->jool.global->cfg.nat64.f_args);
	if (*masks)
		return 0;

	log_debug("Pool4 seems to be empty.");
	return -EINVAL;
}

/**
 * Filtering and Updating for IPv6-UDP and ICMP packets.
 *
 * This is RFC 6146, first halves of both sections 3.5.1 and 3.5.3.
 */
static int ipv6_simple(struct xlation *state)
{
	struct ipv4_transport_addr dst4;
	struct mask_domain *masks;
	int error;

	if (xlat_dst_6to4(state, &dst4))
		return breakdown(state);
	if (find_mask_domain(state, &masks))
		return breakdown(state);

	error = bib_add6(state->jool.nat64.bib, masks, &state->in.tuple, &dst4,
			&state->entries);

	mask_domain_put(masks);

	/* Error msg already printed. */
	return error ? breakdown(state) : succeed(state);
}

/**
 * Filtering and Updating for IPv4-UDP and ICMP packets.
 *
 * This is RFC 6146, second halves of both sections 3.5.1 and 3.5.3.
 */
static int ipv4_simple(struct xlation *state)
{
	struct ipv4_transport_addr *src4 = &state->in.tuple.src.addr4;
	struct ipv6_transport_addr dst6;
	int error;

	error = rfc6052_4to6(state->jool.pool6, &src4->l3, &dst6.l3);
	if (error)
		return breakdown(state); /* Error msg already printed. */
	dst6.l4 = src4->l4;

	error = bib_add4(state->jool.nat64.bib, &dst6, &state->in.tuple,
			&state->entries);

	/* TODO do the others have interesting things to say as well? */
	switch (error) {
	case 0:
		return succeed(state);
	case -ESRCH:
		log_debug("There is no BIB entry for the IPv4 packet.");
		/* inc_stats(&state->in, IPSTATS_MIB_INNOROUTES); */
		break;
	case -EPERM:
		log_debug("Packet was blocked by Address-Dependent Filtering.");
		/* icmp64_send(&state->in, ICMPERR_FILTER, 0); */
		break;
	default:
		log_debug("Errcode %d while finding a BIB entry.", error);
		/* icmp64_send(&state->in, ICMPERR_ADDR_UNREACHABLE, 0); */
		break;
	}

	return breakdown(state);
}

/**
 * Filtering and updating during the V4 INIT state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_v4_init_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;

	switch (pkt_l3_proto(pkt)) {
	case L3PROTO_IPV6:
		if (pkt_tcp_hdr(pkt)->syn) {
			if (session->has_stored)
				log_debug("Simultaneous Open!");
			session->state = ESTABLISHED;
			session->has_stored = false;
			return FATE_TIMER_EST;
		}
		break;

	/**
	 * "OMG WHAT IS THIS?!!!!1!1oneone"
	 *
	 * Well, basically, they don't seem to have tested the packet storage
	 * thing all that well while writing the RFC.
	 *
	 * This is a patch that helps type 2 packets work. This is the problem:
	 *
	 * - IPv4 node n4 writes a TCP SYN. Let's call this packet "A".
	 *   A arrives to the NAT64.
	 * - Let's say there is a BIB entry but no session that matches A, and
	 *   also, ADF is active, so the NAT64 decides to store A.
	 *   To this end, it creates and stores session entry [src6=a, dst6=b,
	 *   src4=c, dst4=d, proto=TCP, state=V4 INIT, stored=A].
	 *   A is not translated.
	 *
	 * The intent is that the NAT64 is now waiting for an IPv6 packet "B"
	 * that is the Simultaneous Open counterpart to A. If B arrives within 6
	 * seconds, A is allowed, and if it doesn't, then A is not allowed and
	 * will be ICMP errored.
	 * So far so good, right?
	 *
	 * Wrong.
	 *
	 * The problem is that A created a fully valid session that corresponds
	 * to itself. Because n4 doesn't receive an answer, it retries A. It
	 * does so before the 6-second timeout because sockets are impatient
	 * like that. So A2 arrives at the NAT64 and is translated successfully
	 * because there's now a valid session that matches it. In other words,
	 * A authorized itself despite ADF.
	 *
	 * One might argue that this would be a reason to not treat type 1 and 2
	 * packets differently: Simply store these bogus sessions away from the
	 * main database and the A2 session lookup will fail. This doesn't work
	 * either, because the whole thing is that this session needs to be
	 * lookupable in the 6-to-4 direction, otherwise B cannot cancel the
	 * ICMP error.
	 *
	 * Also, these sessions are mapped to a valid BIB entry, and as such
	 * need to prevent this entry from dying. This is hard to enforce when
	 * storing these sessions in another database.
	 *
	 * So the core of the issue is that the V4 INIT state lets v4 packets
	 * through even when ADF is active. Hence this switch case.
	 * (Because this only handles type 2 packets, ADF active = packet stored
	 * in this case.)
	 *
	 * Type 1 packets don't suffer from this problem because they aren't
	 * associated with a valid BIB entry.
	 *
	 * Similar to type 1 packets, we will assume that this retry is not
	 * entitled to a session timeout update. Or any session updates, for
	 * that matter. (See pktqueue_add())
	 */
	case L3PROTO_IPV4:
		if (session->has_stored) {
			log_debug("Simultaneous Open already exists.");
			return FATE_DROP;
		}
		break;
	}

	return FATE_PRESERVE;
}

/**
 * Filtering and updating during the V6 INIT state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_v6_init_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;

	if (pkt_tcp_hdr(pkt)->syn) {
		switch (pkt_l3_proto(pkt)) {
		case L3PROTO_IPV4:
			session->state = ESTABLISHED;
			return FATE_TIMER_EST;
		case L3PROTO_IPV6:
			return FATE_TIMER_TRANS;
		}
	}

	return FATE_PRESERVE;
}

/**
 * Filtering and updating during the ESTABLISHED state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_established_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;

	if (pkt_tcp_hdr(pkt)->fin) {
		switch (pkt_l3_proto(pkt)) {
		case L3PROTO_IPV4:
			session->state = V4_FIN_RCV;
			break;
		case L3PROTO_IPV6:
			session->state = V6_FIN_RCV;
			break;
		}
		return FATE_PRESERVE;

	} else if (pkt_tcp_hdr(pkt)->rst) {
		session->state = TRANS;
		return FATE_TIMER_TRANS;
	}

	return FATE_TIMER_EST;
}

static bool handle_rst_during_fin_rcv(struct xlation *state)
{
	return state->jool.global->cfg.nat64.handle_rst_during_fin_rcv;
}

/**
 * Filtering and updating during the V4 FIN RCV state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_v4_fin_rcv_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;
	struct tcphdr *hdr;

	if (pkt_l3_proto(pkt) == L3PROTO_IPV6) {
		hdr = pkt_tcp_hdr(pkt);
		if (hdr->fin) {
			session->state = V4_FIN_V6_FIN_RCV;
			return FATE_TIMER_TRANS;
		}
		if (hdr->rst && handle_rst_during_fin_rcv(state)) {
			/* https://github.com/NICMx/Jool/issues/212 */
			return FATE_TIMER_TRANS;
		}
	}

	return FATE_TIMER_EST;
}

/**
 * Filtering and updating during the V6 FIN RCV state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_v6_fin_rcv_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;
	struct tcphdr *hdr;

	if (pkt_l3_proto(pkt) == L3PROTO_IPV4) {
		hdr = pkt_tcp_hdr(pkt);
		if (hdr->fin) {
			session->state = V4_FIN_V6_FIN_RCV;
			return FATE_TIMER_TRANS;
		}
		if (hdr->rst && handle_rst_during_fin_rcv(state)) {
			/* https://github.com/NICMx/Jool/issues/212 */
			return FATE_TIMER_TRANS;
		}
	}

	return FATE_TIMER_EST;
}

/**
 * Filtering and updating during the V6 FIN + V4 FIN RCV state of the TCP state
 * machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_v4_fin_v6_fin_rcv_state(void)
{
	return FATE_PRESERVE; /* Only the timeout can change this state. */
}

/**
 * Filtering and updating done during the TRANS state of the TCP state machine.
 * Part of RFC 6146 section 3.5.2.2.
 */
static enum session_fate tcp_trans_state(struct session_entry *session,
		struct xlation *state)
{
	struct packet *pkt = &state->in;

	if (!pkt_tcp_hdr(pkt)->rst) {
		session->state = ESTABLISHED;
		return FATE_TIMER_EST;
	}

	return FATE_PRESERVE;
}

static enum session_fate tcp_state_machine(struct session_entry *session,
		void *arg)
{
	switch (session->state) {
	case ESTABLISHED:
		return tcp_established_state(session, arg);
	case V4_INIT:
		return tcp_v4_init_state(session, arg);
	case V6_INIT:
		return tcp_v6_init_state(session, arg);
	case V4_FIN_RCV:
		return tcp_v4_fin_rcv_state(session, arg);
	case V6_FIN_RCV:
		return tcp_v6_fin_rcv_state(session, arg);
	case V4_FIN_V6_FIN_RCV:
		return tcp_v4_fin_v6_fin_rcv_state();
	case TRANS:
		return tcp_trans_state(session, arg);
	}

	WARN(true, "Invalid state found: %u.", session->state);
	return FATE_RM;
}

/**
 * IPv6 half of RFC 6146 section 3.5.2.
 */
static int ipv6_tcp(struct xlation *state)
{
	struct ipv4_transport_addr dst4;
	struct collision_cb cb;
	struct mask_domain *masks;
	int error;

	if (xlat_dst_6to4(state, &dst4))
		return breakdown(state);
	if (find_mask_domain(state, &masks))
		return breakdown(state);

	cb.cb = tcp_state_machine;
	cb.arg = state;
	error = bib_add_tcp6(state->jool.nat64.bib, masks, &dst4, &state->in,
			&cb, &state->entries);

	mask_domain_put(masks);

	/* Error msg already printed. */
	return error ? breakdown(state) : succeed(state);
}

/**
 * IPv4 half of RFC 6146 section 3.5.2.
 */
static int ipv4_tcp(struct xlation *state)
{
	struct ipv4_transport_addr *src4 = &state->in.tuple.src.addr4;
	struct ipv6_transport_addr dst6;
	struct collision_cb cb;
	int error;

	error = rfc6052_4to6(state->jool.pool6, &src4->l3, &dst6.l3);
	if (error)
		return breakdown(state); /* Error msg already printed. */
	dst6.l4 = src4->l4;

	cb.cb = tcp_state_machine;
	cb.arg = state;
	error = bib_add_tcp4(state->jool.nat64.bib, &dst6, &state->in, &cb,
			&state->entries);

	/* Error msg already printed. */
	return error ? breakdown(state) : succeed(state);
}

/**
 * filtering_and_updating - Main F&U routine. Decides if @state->in should be
 * processed, updating binding and session information.
 */
int filtering_and_updating(struct xlation *state)
{
	struct packet *in = &state->in;
	struct ipv6hdr *hdr_ip6;
	int error = -EINVAL;

	log_debug("Step 2: Filtering and Updating");

	switch (pkt_l3_proto(in)) {
	case L3PROTO_IPV6:
		/* Get rid of hairpinning loops and unwanted packets. */
		hdr_ip6 = pkt_ip6_hdr(in);
		if (pool6_contains(state->jool.pool6, &hdr_ip6->saddr)) {
			log_debug("Hairpinning loop. Dropping...");
			/* inc_stats(in, IPSTATS_MIB_INADDRERRORS); */
			return breakdown(state);
		}
		if (!pool6_contains(state->jool.pool6, &hdr_ip6->daddr)) {
			log_debug("Packet does not belong to pool6.");
			return breakdown(state);
		}

		/* ICMP errors should not be filtered or affect the tables. */
		if (pkt_is_icmp6_error(in)) {
			log_debug("Packet is ICMPv6 error; skipping step...");
			return 0;
		}
		break;
	case L3PROTO_IPV4:
		/* Get rid of unexpected packets */
		if (!pool4db_contains(state->jool.nat64.pool4,
				in->tuple.l4_proto, &in->tuple.dst.addr4)) {
			log_debug("Packet does not belong to pool4.");
			return breakdown(state);
		}

		/* ICMP errors should not be filtered or affect the tables. */
		if (pkt_is_icmp4_error(in)) {
			log_debug("Packet is ICMPv4 error; skipping step...");
			return 0;
		}
		break;
	}

	switch (pkt_l4_proto(in)) {
	case L4PROTO_UDP:
		switch (pkt_l3_proto(in)) {
		case L3PROTO_IPV6:
			error = ipv6_simple(state);
			break;
		case L3PROTO_IPV4:
			error = ipv4_simple(state);
			break;
		}
		break;

	case L4PROTO_TCP:
		switch (pkt_l3_proto(in)) {
		case L3PROTO_IPV6:
			error = ipv6_tcp(state);
			break;
		case L3PROTO_IPV4:
			error = ipv4_tcp(state);
			break;
		}
		break;

	case L4PROTO_ICMP:
		switch (pkt_l3_proto(in)) {
		case L3PROTO_IPV6:
			if (state->jool.global->cfg.nat64.drop_icmp6_info) {
				log_debug("Packet is ICMPv6 info (ping); dropping due to policy.");
				/* inc_stats(in, IPSTATS_MIB_INDISCARDS); */
				kfree_skb(state->in.skb);
				return -EPERM;
			}

			error = ipv6_simple(state);
			break;
		case L3PROTO_IPV4:
			error = ipv4_simple(state);
			break;
		}
		break;

	case L4PROTO_OTHER:
		WARN(true, "Unknown layer 4 protocol: %d", pkt_l4_proto(in));
		return breakdown(state);
	}

	log_debug("Done: Step 2.");
	return error;
}