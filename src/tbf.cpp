/* Copied from gprs_bssgp_pcu.cpp
 *
 * Copyright (C) 2012 Ivan Klyuchnikov
 * Copyright (C) 2012 Andreas Eversberg <jolly@eversberg.eu>
 * Copyright (C) 2013 by Holger Hans Peter Freyther
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <bts.h>
#include <tbf.h>
#include <rlc.h>
#include <encoding.h>
#include <gprs_rlcmac.h>
#include <gprs_debug.h>
#include <gprs_bssgp_pcu.h>

extern "C" {
#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
}

#include <errno.h>
#include <string.h>

/* After sending these frames, we poll for ack/nack. */
#define POLL_ACK_AFTER_FRAMES 20

/* If acknowledgement to downlink assignment should be polled */
#define POLLING_ASSIGNMENT_DL 1
#define POLLING_ASSIGNMENT_UL 1

static const struct gprs_rlcmac_cs gprs_rlcmac_cs[] = {
/*	frame length	data block	max payload */
	{ 0,		0,		0  },
	{ 23,		23,		20 }, /* CS-1 */
	{ 34,		33,		30 }, /* CS-2 */
	{ 40,		39,		36 }, /* CS-3 */
	{ 54,		53,		50 }, /* CS-4 */
};

extern "C" {
int bssgp_tx_llc_discarded(struct bssgp_bvc_ctx *bctx, uint32_t tlli,
                           uint8_t num_frames, uint32_t num_octets);
}

extern void *tall_pcu_ctx;

static void tbf_timer_cb(void *_tbf);

inline gprs_rlcmac_bts *gprs_rlcmac_tbf::bts_data() const
{
	return bts->bts_data();
}

static inline void tbf_update_ms_class(struct gprs_rlcmac_tbf *tbf,
					const uint8_t ms_class)
{
	if (!tbf->ms_class && ms_class)
		tbf->ms_class = ms_class;
}

static inline void tbf_assign_imsi(struct gprs_rlcmac_tbf *tbf,
					const char *imsi)
{
	strncpy(tbf->meas.imsi, imsi, sizeof(tbf->meas.imsi) - 1);
}

static struct gprs_rlcmac_tbf *tbf_lookup_dl(BTS *bts,
					const uint32_t tlli, const char *imsi)
{
	/* TODO: look up by IMSI first, then tlli, then old_tlli */
	return bts->tbf_by_tlli(tlli, GPRS_RLCMAC_DL_TBF);
}

static int tbf_append_data(struct gprs_rlcmac_tbf *tbf,
				struct gprs_rlcmac_bts *bts,
				const uint8_t ms_class,
				const uint16_t pdu_delay_csec,
				const uint8_t *data, const uint16_t len)
{
	LOGP(DRLCMAC, LOGL_INFO, "TBF: APPEND TFI: %u TLLI: 0x%08x\n", tbf->tfi, tbf->tlli);
	if (tbf->state_is(GPRS_RLCMAC_WAIT_RELEASE)) {
		LOGP(DRLCMAC, LOGL_DEBUG, "TBF in WAIT RELEASE state "
			"(T3193), so reuse TBF\n");
		memcpy(tbf->llc_frame, data, len);
		tbf->llc_length = len;
		/* reset rlc states */
		memset(&tbf->dir.dl, 0, sizeof(tbf->dir.dl));
		/* keep to flags */
		tbf->state_flags &= GPRS_RLCMAC_FLAG_TO_MASK;
		tbf->state_flags &= ~(1 << GPRS_RLCMAC_FLAG_CCCH);
		tbf_update_ms_class(tbf, ms_class);
		tbf->update();
		tbf->bts->trigger_dl_ass(tbf, tbf, NULL);
	} else {
		/* the TBF exists, so we must write it in the queue
		 * we prepend lifetime in front of PDU */
		struct timeval *tv;
		struct msgb *llc_msg = msgb_alloc(len + sizeof(*tv),
			"llc_pdu_queue");
		if (!llc_msg)
			return -ENOMEM;
		tv = (struct timeval *)msgb_put(llc_msg, sizeof(*tv));

		uint16_t delay_csec;
		if (bts->force_llc_lifetime)
			delay_csec = bts->force_llc_lifetime;
		else
			delay_csec = pdu_delay_csec;
		/* keep timestap at 0 for infinite delay */
		if (delay_csec != 0xffff) {
			/* calculate timestamp of timeout */
			gettimeofday(tv, NULL);
			tv->tv_usec += (delay_csec % 100) * 10000;
			tv->tv_sec += delay_csec / 100;
			if (tv->tv_usec > 999999) {
				tv->tv_usec -= 1000000;
				tv->tv_sec++;
			}
		}
		memcpy(msgb_put(llc_msg, len), data, len);
		msgb_enqueue(&tbf->llc_queue, llc_msg);
		tbf_update_ms_class(tbf, ms_class);
	}

	return 0;
}

static int tbf_new_dl_assignment(struct gprs_rlcmac_bts *bts,
				const char *imsi,
				const uint32_t tlli, const uint8_t ms_class,
				const uint8_t *data, const uint16_t len)
{
	uint8_t trx, ta, ss;
	int8_t use_trx;
	struct gprs_rlcmac_tbf *old_tbf, *tbf;
	int8_t tfi; /* must be signed */
	int rc;

	/* check for uplink data, so we copy our informations */
#warning "Do the same look up for IMSI, TLLI and OLD_TLLI"
#warning "Refactor the below lines... into a new method"
	tbf = bts->bts->tbf_by_tlli(tlli, GPRS_RLCMAC_UL_TBF);
	if (tbf && tbf->dir.ul.contention_resolution_done
	 && !tbf->dir.ul.final_ack_sent) {
		use_trx = tbf->trx_no;
		ta = tbf->ta;
		ss = 0;
		old_tbf = tbf;
	} else {
		use_trx = -1;
		/* we already have an uplink TBF, so we use that TA */
		if (tbf)
			ta = tbf->ta;
		else {
			/* recall TA */
			rc = bts->bts->timing_advance()->recall(tlli);
			if (rc < 0) {
				LOGP(DRLCMAC, LOGL_NOTICE, "TA unknown"
					", assuming 0\n");
				ta = 0;
			} else
				ta = rc;
		}
		ss = 1; /* PCH assignment only allows one timeslot */
		old_tbf = NULL;
	}

	// Create new TBF (any TRX)
#warning "Copy and paste with alloc_ul_tbf"
	tfi = bts->bts->tfi_find_free(GPRS_RLCMAC_DL_TBF, &trx, use_trx);
	if (tfi < 0) {
		LOGP(DRLCMAC, LOGL_NOTICE, "No PDCH resource\n");
		/* FIXME: send reject */
		return -EBUSY;
	}
	/* set number of downlink slots according to multislot class */
	tbf = tbf_alloc(bts, tbf, GPRS_RLCMAC_DL_TBF, tfi, trx, ms_class, ss);
	if (!tbf) {
		LOGP(DRLCMAC, LOGL_NOTICE, "No PDCH resource\n");
		/* FIXME: send reject */
		return -EBUSY;
	}
	tbf->tlli = tlli;
	tbf->tlli_valid = 1;
	tbf->ta = ta;

	LOGP(DRLCMAC, LOGL_DEBUG,
		"TBF: [DOWNLINK] START TFI: %d TLLI: 0x%08x \n",
		tbf->tfi, tbf->tlli);

	/* new TBF, so put first frame */
	memcpy(tbf->llc_frame, data, len);
	tbf->llc_length = len;

	/* trigger downlink assignment and set state to ASSIGN.
	 * we don't use old_downlink, so the possible uplink is used
	 * to trigger downlink assignment. if there is no uplink,
	 * AGCH is used. */
	tbf->bts->trigger_dl_ass(tbf, old_tbf, imsi);

	/* store IMSI for debugging purpose. TODO: it is more than debugging */
	tbf_assign_imsi(tbf, imsi);
	return 0;
}

/**
 * TODO: split into unit test-able parts...
 */
int tbf_handle(struct gprs_rlcmac_bts *bts,
		const uint32_t tlli, const char *imsi,
		const uint8_t ms_class, const uint16_t delay_csec,
		const uint8_t *data, const uint16_t len)
{
	struct gprs_rlcmac_tbf *tbf;

	/* check for existing TBF */
	tbf = tbf_lookup_dl(bts->bts, tlli, imsi);
	if (tbf) {
		int rc = tbf_append_data(tbf, bts, ms_class,
						delay_csec, data, len);
		if (rc >= 0)
			tbf_assign_imsi(tbf, imsi);
		return rc;
	} 

	return tbf_new_dl_assignment(bts, imsi, tlli, ms_class, data, len);
}

struct gprs_rlcmac_tbf *tbf_alloc_ul(struct gprs_rlcmac_bts *bts,
	int8_t use_trx, uint8_t ms_class,
	uint32_t tlli, uint8_t ta, struct gprs_rlcmac_tbf *dl_tbf)
{
	uint8_t trx;
	struct gprs_rlcmac_tbf *tbf;
	uint8_t tfi;

#warning "Copy and paste with tbf_new_dl_assignment"
	/* create new TBF, use sme TRX as DL TBF */
	tfi = bts->bts->tfi_find_free(GPRS_RLCMAC_UL_TBF, &trx, use_trx);
	if (tfi < 0) {
		LOGP(DRLCMAC, LOGL_NOTICE, "No PDCH resource\n");
		/* FIXME: send reject */
		return NULL;
	}
	/* use multislot class of downlink TBF */
	tbf = tbf_alloc(bts, dl_tbf, GPRS_RLCMAC_UL_TBF, tfi, trx, ms_class, 0);
	if (!tbf) {
		LOGP(DRLCMAC, LOGL_NOTICE, "No PDCH resource\n");
		/* FIXME: send reject */
		return NULL;
	}
	tbf->tlli = tlli;
	tbf->tlli_valid = 1; /* no contention resolution */
	tbf->dir.ul.contention_resolution_done = 1;
	tbf->ta = ta; /* use current TA */
	tbf_new_state(tbf, GPRS_RLCMAC_ASSIGN);
	tbf->state_flags |= (1 << GPRS_RLCMAC_FLAG_PACCH);
	tbf_timer_start(tbf, 3169, bts->t3169, 0);

	return tbf;
}

static void tbf_unlink_pdch(struct gprs_rlcmac_tbf *tbf)
{
	struct gprs_rlcmac_pdch *pdch;
	int ts;

	if (tbf->direction == GPRS_RLCMAC_UL_TBF) {
		tbf->trx->ul_tbf[tbf->tfi] = NULL;
		for (ts = 0; ts < 8; ts++) {
			pdch = tbf->pdch[ts];
			if (pdch)
				pdch->ul_tbf[tbf->tfi] = NULL;
			tbf->pdch[ts] = NULL;
		}
	} else {
		tbf->trx->dl_tbf[tbf->tfi] = NULL;
		for (ts = 0; ts < 8; ts++) {
			pdch = tbf->pdch[ts];
			if (pdch)
				pdch->dl_tbf[tbf->tfi] = NULL;
			tbf->pdch[ts] = NULL;
		}
	}
}

void tbf_free(struct gprs_rlcmac_tbf *tbf)
{
	struct msgb *msg;

	/* Give final measurement report */
	gprs_rlcmac_rssi_rep(tbf);
	gprs_rlcmac_lost_rep(tbf);

	debug_diagram(tbf->bts, tbf->diag, "+---------------+");
	debug_diagram(tbf->bts, tbf->diag, "|    THE END    |");
	debug_diagram(tbf->bts, tbf->diag, "+---------------+");
	LOGP(DRLCMAC, LOGL_INFO, "Free %s TBF=%d with TLLI=0x%08x.\n",
		(tbf->direction == GPRS_RLCMAC_UL_TBF) ? "UL" : "DL", tbf->tfi,
		tbf->tlli);
	if (tbf->ul_ass_state != GPRS_RLCMAC_UL_ASS_NONE)
		LOGP(DRLCMAC, LOGL_ERROR, "Software error: Pending uplink "
			"assignment. This may not happen, because the "
			"assignment message never gets transmitted. Please "
			"be sure not to free in this state. PLEASE FIX!\n");
	if (tbf->dl_ass_state != GPRS_RLCMAC_DL_ASS_NONE)
		LOGP(DRLCMAC, LOGL_ERROR, "Software error: Pending downlink "
			"assignment. This may not happen, because the "
			"assignment message never gets transmitted. Please "
			"be sure not to free in this state. PLEASE FIX!\n");
	tbf->stop_timer();
	#warning "TODO: Could/Should generate  bssgp_tx_llc_discarded"
	while ((msg = msgb_dequeue(&tbf->llc_queue))) {
		tbf->bts->dropped_frame();
		msgb_free(msg);
	}
	tbf_unlink_pdch(tbf);
	llist_del(&tbf->list);

	if (tbf->direction == GPRS_RLCMAC_UL_TBF)
		tbf->bts->tbf_ul_freed();
	else
		tbf->bts->tbf_dl_freed();

	LOGP(DRLCMAC, LOGL_DEBUG, "********** TBF ends here **********\n");
	talloc_free(tbf);
}

int gprs_rlcmac_tbf::update()
{
	struct gprs_rlcmac_tbf *ul_tbf = NULL;
	struct gprs_rlcmac_bts *bts_data = bts->bts_data();
	int rc;

	LOGP(DRLCMAC, LOGL_DEBUG, "********** TBF update **********\n");

	if (direction != GPRS_RLCMAC_DL_TBF)
		return -EINVAL;

	if (!ms_class) {
		LOGP(DRLCMAC, LOGL_DEBUG, "- Cannot update, no class\n");
		return -EINVAL;
	}

	ul_tbf = bts->tbf_by_tlli(tlli, GPRS_RLCMAC_UL_TBF);

	tbf_unlink_pdch(this);
	rc = bts_data->alloc_algorithm(bts_data, ul_tbf, this, bts_data->alloc_algorithm_curst, 0);
	/* if no resource */
	if (rc < 0) {
		LOGP(DRLCMAC, LOGL_ERROR, "No resource after update???\n");
		return -rc;
	}

	return 0;
}

int tbf_assign_control_ts(struct gprs_rlcmac_tbf *tbf)
{
	if (tbf->control_ts == 0xff)
		LOGP(DRLCMAC, LOGL_INFO, "- Setting Control TS %d\n",
			tbf->first_common_ts);
	else if (tbf->control_ts != tbf->first_common_ts)
		LOGP(DRLCMAC, LOGL_INFO, "- Changing Control TS %d\n",
			tbf->first_common_ts);
	tbf->control_ts = tbf->first_common_ts;

	return 0;
}

static const char *tbf_state_name[] = {
	"NULL",
	"ASSIGN",
	"FLOW",
	"FINISHED",
	"WAIT RELEASE",
	"RELEASING",
};

void tbf_new_state(struct gprs_rlcmac_tbf *tbf,
	enum gprs_rlcmac_tbf_state state)
{
	debug_diagram(tbf->bts, tbf->diag, "->%s", tbf_state_name[state]);
	LOGP(DRLCMAC, LOGL_DEBUG, "%s TBF=%d changes state from %s to %s\n",
		(tbf->direction == GPRS_RLCMAC_UL_TBF) ? "UL" : "DL", tbf->tfi,
		tbf_state_name[tbf->state], tbf_state_name[state]);
	tbf->set_state(state);
}

void tbf_timer_start(struct gprs_rlcmac_tbf *tbf, unsigned int T,
			unsigned int seconds, unsigned int microseconds)
{
	if (!osmo_timer_pending(&tbf->timer))
		LOGP(DRLCMAC, LOGL_DEBUG, "Starting %s TBF=%d timer %u.\n",
			(tbf->direction == GPRS_RLCMAC_UL_TBF) ? "UL" : "DL",
			tbf->tfi, T);
	else
		LOGP(DRLCMAC, LOGL_DEBUG, "Restarting %s TBF=%d timer %u "
			"while old timer %u pending \n",
			(tbf->direction == GPRS_RLCMAC_UL_TBF) ? "UL" : "DL",
			tbf->tfi, T, tbf->T);

	tbf->T = T;
	tbf->num_T_exp = 0;

	/* Tunning timers can be safely re-scheduled. */
	tbf->timer.data = tbf;
	tbf->timer.cb = &tbf_timer_cb;

	osmo_timer_schedule(&tbf->timer, seconds, microseconds);
}

void gprs_rlcmac_tbf::stop_t3191()
{
	return stop_timer();
}

void gprs_rlcmac_tbf::stop_timer()
{
	if (osmo_timer_pending(&timer)) {
		LOGP(DRLCMAC, LOGL_DEBUG, "Stopping %s TBF=%d timer %u.\n",
			(direction == GPRS_RLCMAC_UL_TBF) ? "UL" : "DL", tfi, T);
		osmo_timer_del(&timer);
	}
}

void gprs_rlcmac_tbf::poll_timeout()
{
	LOGP(DRLCMAC, LOGL_NOTICE, "Poll timeout for %s TBF=%d\n",
		(direction == GPRS_RLCMAC_UL_TBF) ? "UL" : "DL", tfi);

	poll_state = GPRS_RLCMAC_POLL_NONE;

	if (ul_ack_state == GPRS_RLCMAC_UL_ACK_WAIT_ACK) {
		if (!(state_flags & (1 << GPRS_RLCMAC_FLAG_TO_UL_ACK))) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- Timeout for polling "
				"PACKET CONTROL ACK for PACKET UPLINK ACK\n");
			rlcmac_diag();
			state_flags |= (1 << GPRS_RLCMAC_FLAG_TO_UL_ACK);
		}
		ul_ack_state = GPRS_RLCMAC_UL_ACK_NONE;
		debug_diagram(bts, this->diag, "timeout UL-ACK");
		if (state_is(GPRS_RLCMAC_FINISHED)) {
			dir.ul.n3103++;
			if (dir.ul.n3103 == bts->bts_data()->n3103) {
				LOGP(DRLCMAC, LOGL_NOTICE,
					"- N3103 exceeded\n");
				debug_diagram(bts, diag, "N3103 exceeded");
				tbf_new_state(this, GPRS_RLCMAC_RELEASING);
				tbf_timer_start(this, 3169, bts->bts_data()->t3169, 0);
				return;
			}
			/* reschedule UL ack */
			ul_ack_state = GPRS_RLCMAC_UL_ACK_SEND_ACK;
		}
	} else if (ul_ass_state == GPRS_RLCMAC_UL_ASS_WAIT_ACK) {
		if (!(state_flags & (1 << GPRS_RLCMAC_FLAG_TO_UL_ASS))) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- Timeout for polling "
				"PACKET CONTROL ACK for PACKET UPLINK "
				"ASSIGNMENT.\n");
			rlcmac_diag();
			state_flags |= (1 << GPRS_RLCMAC_FLAG_TO_UL_ASS);
		}
		ul_ass_state = GPRS_RLCMAC_UL_ASS_NONE;
		debug_diagram(bts, diag, "timeout UL-ASS");
		n3105++;
		if (n3105 == bts_data()->n3105) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- N3105 exceeded\n");
			debug_diagram(bts, diag, "N3105 exceeded");
			tbf_new_state(this, GPRS_RLCMAC_RELEASING);
			tbf_timer_start(this, 3195, bts_data()->t3195, 0);
			return;
		}
		/* reschedule UL assignment */
		ul_ass_state = GPRS_RLCMAC_UL_ASS_SEND_ASS;
	} else if (dl_ass_state == GPRS_RLCMAC_DL_ASS_WAIT_ACK) {
		if (!(state_flags & (1 << GPRS_RLCMAC_FLAG_TO_DL_ASS))) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- Timeout for polling "
				"PACKET CONTROL ACK for PACKET DOWNLINK "
				"ASSIGNMENT.\n");
			rlcmac_diag();
			state_flags |= (1 << GPRS_RLCMAC_FLAG_TO_DL_ASS);
		}
		dl_ass_state = GPRS_RLCMAC_DL_ASS_NONE;
		debug_diagram(bts, diag, "timeout DL-ASS");
		n3105++;
		if (n3105 == bts->bts_data()->n3105) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- N3105 exceeded\n");
			debug_diagram(bts, diag, "N3105 exceeded");
			tbf_new_state(this, GPRS_RLCMAC_RELEASING);
			tbf_timer_start(this, 3195, bts_data()->t3195, 0);
			return;
		}
		/* reschedule DL assignment */
		dl_ass_state = GPRS_RLCMAC_DL_ASS_SEND_ASS;
	} else if (direction == GPRS_RLCMAC_DL_TBF) {
		if (!(state_flags & (1 << GPRS_RLCMAC_FLAG_TO_DL_ACK))) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- Timeout for polling "
				"PACKET DOWNLINK ACK.\n");
			rlcmac_diag();
			state_flags |= (1 << GPRS_RLCMAC_FLAG_TO_DL_ACK);
		}
		debug_diagram(bts, diag, "timeout DL-ACK");
		n3105++;
		if (n3105 == bts->bts_data()->n3105) {
			LOGP(DRLCMAC, LOGL_NOTICE, "- N3105 exceeded\n");
			debug_diagram(bts, diag, "N3105 exceeded");
			tbf_new_state(this, GPRS_RLCMAC_RELEASING);
			tbf_timer_start(this, 3195, bts_data()->t3195, 0);
			return;
		}
		/* resend IMM.ASS on CCCH on timeout */
		if ((state_flags & (1 << GPRS_RLCMAC_FLAG_CCCH))
		 && !(state_flags & (1 << GPRS_RLCMAC_FLAG_DL_ACK))) {
			LOGP(DRLCMAC, LOGL_DEBUG, "Re-send dowlink assignment "
				"for TBF=%d on PCH (IMSI=%s)\n", tfi,
				dir.dl.imsi);
			/* send immediate assignment */
			bts->snd_dl_ass(this, 0, dir.dl.imsi);
			dir.dl.wait_confirm = 1;
		}
	} else
		LOGP(DRLCMAC, LOGL_ERROR, "- Poll Timeout, but no event!\n");
}

struct gprs_rlcmac_tbf *tbf_alloc(struct gprs_rlcmac_bts *bts,
	struct gprs_rlcmac_tbf *old_tbf, enum gprs_rlcmac_tbf_direction dir,
	uint8_t tfi, uint8_t trx,
	uint8_t ms_class, uint8_t single_slot)
{
	struct gprs_rlcmac_tbf *tbf;
	int rc;

#ifdef DEBUG_DIAGRAM
	/* hunt for first free number in diagram */
	int diagram_num;
	for (diagram_num = 0; ; diagram_num++) {
		llist_for_each_entry(tbf, &bts->ul_tbfs, list) {
			if (tbf->diag == diagram_num)
				goto next_diagram;
		}
		llist_for_each_entry(tbf, &bts->dl_tbfs, list) {
			if (tbf->diag == diagram_num)
				goto next_diagram;
		}
		break;
next_diagram:
		continue;
	}
#endif

	LOGP(DRLCMAC, LOGL_DEBUG, "********** TBF starts here **********\n");
	LOGP(DRLCMAC, LOGL_INFO, "Allocating %s TBF: TFI=%d TRX=%d "
		"MS_CLASS=%d\n", (dir == GPRS_RLCMAC_UL_TBF) ? "UL" : "DL",
		tfi, trx, ms_class);

	if (trx >= 8 || tfi >= 32)
		return NULL;

	tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_tbf);
	if (!tbf)
		return NULL;

	tbf->bts = bts->bts;
#ifdef DEBUG_DIAGRAM
	tbf->diag = diagram_num;
#endif
	tbf->direction = dir;
	tbf->tfi = tfi;
	tbf->trx_no = trx;
	tbf->trx = &bts->trx[trx];
	tbf->arfcn = bts->trx[trx].arfcn;
	tbf->ms_class = ms_class;
	tbf->ws = 64;
	tbf->sns = 128;
	/* select algorithm */
	rc = bts->alloc_algorithm(bts, old_tbf, tbf, bts->alloc_algorithm_curst,
		single_slot);
	/* if no resource */
	if (rc < 0) {
		talloc_free(tbf);
		return NULL;
	}
	/* assign control ts */
	tbf->control_ts = 0xff;
	rc = tbf_assign_control_ts(tbf);
	/* if no resource */
	if (rc < 0) {
		talloc_free(tbf);
		return NULL;
	}

	/* set timestamp */
	gettimeofday(&tbf->meas.dl_bw_tv, NULL);
	gettimeofday(&tbf->meas.rssi_tv, NULL);
	gettimeofday(&tbf->meas.dl_loss_tv, NULL);

	INIT_LLIST_HEAD(&tbf->llc_queue);
	if (dir == GPRS_RLCMAC_UL_TBF) {
		llist_add(&tbf->list, &bts->ul_tbfs);
		tbf->bts->tbf_ul_created();
	} else {
		llist_add(&tbf->list, &bts->dl_tbfs);
		tbf->bts->tbf_dl_created();
	}

	debug_diagram(bts->bts, tbf->diag, "+-----------------+");
	debug_diagram(bts->bts, tbf->diag, "|NEW %s TBF TFI=%2d|",
		(dir == GPRS_RLCMAC_UL_TBF) ? "UL" : "DL", tfi);
	debug_diagram(bts->bts, tbf->diag, "+-----------------+");

	return tbf;
}

static void tbf_timer_cb(void *_tbf)
{
	struct gprs_rlcmac_tbf *tbf = (struct gprs_rlcmac_tbf *)_tbf;
	tbf->handle_timeout();
}

void gprs_rlcmac_tbf::handle_timeout()
{
	LOGP(DRLCMAC, LOGL_DEBUG, "%s TBF=%d timer %u expired.\n",
		(direction == GPRS_RLCMAC_UL_TBF) ? "UL" : "DL", tfi, T);

	num_T_exp++;

	switch (T) {
	case 0: /* assignment */
		if ((state_flags & (1 << GPRS_RLCMAC_FLAG_PACCH))) {
			if (state_is(GPRS_RLCMAC_ASSIGN)) {
				LOGP(DRLCMAC, LOGL_NOTICE, "Releasing due to "
					"PACCH assignment timeout.\n");
				tbf_free(this);
			} else
				LOGP(DRLCMAC, LOGL_ERROR, "Error: TBF is not "
					"in assign state\n");
		}
		if ((state_flags & (1 << GPRS_RLCMAC_FLAG_CCCH))) {
			/* change state to FLOW, so scheduler will start transmission */
			dir.dl.wait_confirm = 0;
			if (state_is(GPRS_RLCMAC_ASSIGN)) {
				tbf_new_state(this, GPRS_RLCMAC_FLOW);
				tbf_assign_control_ts(this);
			} else
				LOGP(DRLCMAC, LOGL_NOTICE, "Continue flow after "
					"IMM.ASS confirm\n");
		}
		break;
	case 3169:
	case 3191:
	case 3195:
		LOGP(DRLCMAC, LOGL_NOTICE, "TBF T%d timeout during "
			"transsmission\n", T);
		rlcmac_diag();
		/* fall through */
	case 3193:
		if (T == 3193)
		        debug_diagram(bts, diag, "T3193 timeout");
		LOGP(DRLCMAC, LOGL_DEBUG, "TBF will be freed due to timeout\n");
		/* free TBF */
		tbf_free(this);
		break;
	default:
		LOGP(DRLCMAC, LOGL_ERROR, "Timer expired in unknown mode: %u\n", T);
	}
}

int gprs_rlcmac_tbf::rlcmac_diag()
{
	if ((state_flags & (1 << GPRS_RLCMAC_FLAG_CCCH)))
		LOGP(DRLCMAC, LOGL_NOTICE, "- Assignment was on CCCH\n");
	if ((state_flags & (1 << GPRS_RLCMAC_FLAG_PACCH)))
		LOGP(DRLCMAC, LOGL_NOTICE, "- Assignment was on PACCH\n");
	if ((state_flags & (1 << GPRS_RLCMAC_FLAG_UL_DATA)))
		LOGP(DRLCMAC, LOGL_NOTICE, "- Uplink data was received\n");
	else if (direction == GPRS_RLCMAC_UL_TBF)
		LOGP(DRLCMAC, LOGL_NOTICE, "- No uplink data received yet\n");
	if ((state_flags & (1 << GPRS_RLCMAC_FLAG_DL_ACK)))
		LOGP(DRLCMAC, LOGL_NOTICE, "- Downlink ACK was received\n");
	else if (direction == GPRS_RLCMAC_DL_TBF)
		LOGP(DRLCMAC, LOGL_NOTICE, "- No downlink ACK received yet\n");

	return 0;
}

struct msgb *gprs_rlcmac_tbf::llc_dequeue(bssgp_bvc_ctx *bctx)
{
	struct msgb *msg;
	struct timeval *tv, tv_now;
	uint32_t octets = 0, frames = 0;

	gettimeofday(&tv_now, NULL);

	while ((msg = msgb_dequeue(&llc_queue))) {
		tv = (struct timeval *)msg->data;
		msgb_pull(msg, sizeof(*tv));
		if (tv->tv_sec /* not infinite */
		 && (tv_now.tv_sec > tv->tv_sec /* and secs expired */
		  || (tv_now.tv_sec == tv->tv_sec /* .. or if secs equal .. */
		   && tv_now.tv_usec > tv->tv_usec))) { /* .. usecs expired */
			LOGP(DRLCMACDL, LOGL_NOTICE, "Discarding LLC PDU of "
				"DL TBF=%d, because lifetime limit reached\n",
				tfi);
			bts->timedout_frame();
			frames++;
			octets += msg->len;
			msgb_free(msg);
			continue;
		}
		break;
	}

	if (frames) {
		if (frames > 0xff)
			frames = 0xff;
		if (octets > 0xffffff)
			octets = 0xffffff;
		bssgp_tx_llc_discarded(bctx, tlli, frames, octets);
	}

	return msg;
}

void gprs_rlcmac_tbf::update_llc_frame(struct msgb *msg)
{
	/* TODO: bounds check */
	memcpy(llc_frame, msg->data, msg->len);
	llc_length = msg->len;
}

/*
 * Store received block data in LLC message(s) and forward to SGSN
 * if complete.
 */
int gprs_rlcmac_tbf::assemble_forward_llc(uint8_t *data, uint8_t len)
{
	struct rlc_ul_header *rh = (struct rlc_ul_header *)data;
	uint8_t e, m;
	struct rlc_li_field *li;
	uint8_t frame_offset[16], offset = 0, chunk;
	int i, frames = 0;

	LOGP(DRLCMACUL, LOGL_DEBUG, "- Assembling frames: (len=%d)\n", len);

	data += 3;
	len -= 3;
	e = rh->e; /* if extended */
	m = 1; /* more frames, that means: the first frame */

	/* Parse frame offsets from length indicator(s), if any. */
	while (1) {
		if (frames == (int)sizeof(frame_offset)) {
			LOGP(DRLCMACUL, LOGL_ERROR, "Too many frames in "
				"block\n");
			return -EINVAL;
		}
		frame_offset[frames++] = offset;
		LOGP(DRLCMACUL, LOGL_DEBUG, "-- Frame %d starts at offset "
			"%d\n", frames, offset);
		if (!len)
			break;
		/* M == 0 and E == 0 is not allowed in this version. */
		if (!m && !e) {
			LOGP(DRLCMACUL, LOGL_NOTICE, "UL DATA TBF=%d "
				"ignored, because M='0' and E='0'.\n",
				this->tfi);
			return 0;
		}
		/* no more frames in this segment */
		if (e) {
			break;
		}
		/* There is a new frame and an LI that delimits it. */
		if (m) {
			li = (struct rlc_li_field *)data;
			LOGP(DRLCMACUL, LOGL_DEBUG, "-- Delimiter len=%d\n",
				li->li);
			/* Special case: LI == 0
			 * If the last segment would fit precisely into the
			 * rest of the RLC MAC block, there would be no way
			 * to delimit that this segment ends and is not
			 * continued in the next block.
			 * The special LI (0) is used to force the segment to
			 * extend into the next block, so it is delimited there.
			 * This LI must be skipped. Also it is the last LI.
			 */
			if (li->li == 0) {
				data++;
				len--;
				m = 1; /* M is ignored, we know there is more */
				break; /* handle E as '1', so we break! */
			}
			e = li->e;
			m = li->m;
			offset += li->li;
			data++;
			len--;
			continue;
		}
	}
	if (!m) {
		LOGP(DRLCMACUL, LOGL_DEBUG, "- Last frame carries spare "
			"data\n");
	}

	LOGP(DRLCMACUL, LOGL_DEBUG, "- Data length after length fields: %d\n",
		len);
	/* TLLI */
	if (rh->ti) {
		if (len < 4) {
			LOGP(DRLCMACUL, LOGL_NOTICE, "UL DATA TLLI out of "
				"frame border\n");
			return -EINVAL;
		}
		data += 4;
		len -= 4;
		LOGP(DRLCMACUL, LOGL_DEBUG, "- Length after skipping TLLI: "
			"%d\n", len);
	}

	/* PFI */
	if (rh->pi) {
		LOGP(DRLCMACUL, LOGL_ERROR, "ERROR: PFI not supported, "
			"please disable in SYSTEM INFORMATION\n");
		if (len < 1) {
			LOGP(DRLCMACUL, LOGL_NOTICE, "UL DATA PFI out of "
				"frame border\n");
			return -EINVAL;
		}
		data++;
		len--;
		LOGP(DRLCMACUL, LOGL_DEBUG, "- Length after skipping PFI: "
			"%d\n", len);
	}

	/* Now we have:
	 * - a list of frames offsets: frame_offset[]
	 * - number of frames: i
	 * - m == 0: Last frame carries spare data (end of TBF).
	 */

	/* Check if last offset would exceed frame. */
	if (offset > len) {
		LOGP(DRLCMACUL, LOGL_NOTICE, "UL DATA TBF=%d ignored, "
			"because LI delimits data that exceeds block size.\n",
			this->tfi);
		return -EINVAL;
	}

	/* create LLC frames */
	for (i = 0; i < frames; i++) {
		/* last frame ? */
		if (i == frames - 1) {
			/* no more data in last frame */
			if (!m)
				break;
			/* data until end of frame */
			chunk = len - frame_offset[i];
		} else {
			/* data until next frame */
			chunk = frame_offset[i + 1] - frame_offset[i];
		}
		LOGP(DRLCMACUL, LOGL_DEBUG, "-- Appending chunk (len=%d) to "
			"frame at %d.\n", chunk, this->llc_index);
		if (this->llc_index + chunk > LLC_MAX_LEN) {
			LOGP(DRLCMACUL, LOGL_NOTICE, "LLC frame exceeds "
				"maximum size.\n");
			chunk = LLC_MAX_LEN - this->llc_index;
		}
		memcpy(this->llc_frame + this->llc_index, data + frame_offset[i],
			chunk);
		this->llc_index += chunk;
		/* not last frame. */
		if (i != frames - 1) {
			/* send frame to SGSN */
			LOGP(DRLCMACUL, LOGL_INFO, "Complete UL frame for "
				"TBF=%d: len=%d\n", this->tfi, this->llc_index);
			gprs_rlcmac_tx_ul_ud(this);
			this->llc_index = 0; /* reset frame space */
		/* also check if CV==0, because the frame may fill up the
		 * block precisely, then it is also complete. normally the
		 * frame would be extended into the next block with a 0-length
		 * delimiter added to this block. */
		} else if (rh->cv == 0) {
			/* send frame to SGSN */
			LOGP(DRLCMACUL, LOGL_INFO, "Complete UL frame for "
				"TBF=%d that fits precisely in last block: "
				"len=%d\n", this->tfi, this->llc_index);
			gprs_rlcmac_tx_ul_ud(this);
			this->llc_index = 0; /* reset frame space */
		}
	}

	return 0;
}

/*
 * Create DL data block
 * The messages are fragmented and forwarded as data blocks.
 */
struct msgb *gprs_rlcmac_tbf::create_dl_acked_block(uint32_t fn, uint8_t ts)
{
	struct rlc_dl_header *rh;
	struct rlc_li_field *li;
	uint8_t block_length; /* total length of block, including spare bits */
	uint8_t block_data; /* usable data of block, w/o spare bits, inc. MAC */
	struct msgb *msg, *dl_msg;
	uint8_t bsn;
	uint16_t mod_sns = sns - 1;
	uint16_t mod_sns_half = (sns >> 1) - 1;
	uint16_t index;
	uint8_t *delimiter, *data, *e_pointer;
	uint8_t len;
	uint16_t space, chunk;
	int first_fin_ack = 0;

	LOGP(DRLCMACDL, LOGL_DEBUG, "DL DATA TBF=%d downlink (V(A)==%d .. "
		"V(S)==%d)\n", tfi, dir.dl.v_a, dir.dl.v_s);

do_resend:
	/* check if there is a block with negative acknowledgement */
	for (bsn = dir.dl.v_a; bsn != dir.dl.v_s; 
	     bsn = (bsn + 1) & mod_sns) {
		index = (bsn & mod_sns_half);
		if (dir.dl.v_b[index] == 'N'
		 || dir.dl.v_b[index] == 'X') {
			LOGP(DRLCMACDL, LOGL_DEBUG, "- Resending BSN %d\n",
				bsn);
			/* re-send block with negative aknowlegement */
			dir.dl.v_b[index] = 'U'; /* unacked */
			goto tx_block;
		}
	}

	/* if the window has stalled, or transfer is complete,
	 * send an unacknowledged block */
	if (state_is(GPRS_RLCMAC_FINISHED)
	 || ((dir.dl.v_s - dir.dl.v_a) & mod_sns) == ws) {
	 	int resend = 0;

		if (state_is(GPRS_RLCMAC_FINISHED))
			LOGP(DRLCMACDL, LOGL_DEBUG, "- Restarting at BSN %d, "
				"because all blocks have been transmitted.\n",
					dir.dl.v_a);
		else
			LOGP(DRLCMACDL, LOGL_NOTICE, "- Restarting at BSN %d, "
				"because all window is stalled.\n",
					dir.dl.v_a);
		/* If V(S) == V(A) and finished state, we would have received
		 * acknowledgement of all transmitted block. In this case we
		 * would have transmitted the final block, and received ack
		 * from MS. But in this case we did not receive the final ack
		 * indication from MS. This should never happen if MS works
		 * correctly. */
		if (dir.dl.v_s == dir.dl.v_a) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "- MS acked all blocks, "
				"so we re-transmit final block!\n");
			/* we just send final block again */
			index = ((dir.dl.v_s - 1) & mod_sns_half);
			goto tx_block;
		}
		
		/* cycle through all unacked blocks */
		for (bsn = dir.dl.v_a; bsn != dir.dl.v_s;
		     bsn = (bsn + 1) & mod_sns) {
			index = (bsn & mod_sns_half);
			if (dir.dl.v_b[index] == 'U') {
				/* mark to be re-send */
				dir.dl.v_b[index] = 'X';
				resend++;
			}
		}
		/* At this point there should be at leasst one unacked block
		 * to be resent. If not, this is an software error. */
		if (resend == 0) {
			LOGP(DRLCMACDL, LOGL_ERROR, "Software error: "
				"There are no unacknowledged blocks, but V(A) "
				" != V(S). PLEASE FIX!\n");
			/* we just send final block again */
			index = ((dir.dl.v_s - 1) & mod_sns_half);
			goto tx_block;
		}
		goto do_resend;
	}

	LOGP(DRLCMACDL, LOGL_DEBUG, "- Sending new block at BSN %d\n",
		dir.dl.v_s);

	/* now we still have untransmitted LLC data, so we fill mac block */
	index = dir.dl.v_s & mod_sns_half;
	data = rlc_block[index];
#warning "Selection of the CS doesn't belong here"
	if (cs == 0) {
		cs = bts_data()->initial_cs_dl;
		if (cs < 1 || cs > 4)
			cs = 1;
	}
	block_length = gprs_rlcmac_cs[cs].block_length;
	block_data = gprs_rlcmac_cs[cs].block_data;
	memset(data, 0x2b, block_data); /* spare bits will be left 0 */
	rh = (struct rlc_dl_header *)data;
	rh->pt = 0; /* Data Block */
	rh->rrbp = rh->s_p = 0; /* Polling, set later, if required */
	rh->usf = 7; /* will be set at scheduler */
	rh->pr = 0; /* FIXME: power reduction */
	rh->tfi = tfi; /* TFI */
	rh->fbi = 0; /* Final Block Indicator, set late, if true */
	rh->bsn = dir.dl.v_s; /* Block Sequence Number */
	rh->e = 0; /* Extension bit, maybe set later */
	e_pointer = data + 2; /* points to E of current chunk */
	data += 3;
	delimiter = data; /* where next length header would be stored */
	space = block_data - 3;
	while (1) {
		chunk = llc_length - llc_index;
		/* if chunk will exceed block limit */
		if (chunk > space) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "-- Chunk with length %d "
				"larger than space (%d) left in block: copy "
				"only remaining space, and we are done\n",
				chunk, space);
			/* block is filled, so there is no extension */
			*e_pointer |= 0x01;
			/* fill only space */
			memcpy(data, llc_frame + llc_index, space);
			/* incement index */
			llc_index += space;
			/* return data block as message */
			break;
		}
		/* if FINAL chunk would fit precisely in space left */
		if (chunk == space && llist_empty(&llc_queue)) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "-- Chunk with length %d "
				"would exactly fit into space (%d): because "
				"this is a final block, we don't add length "
				"header, and we are done\n", chunk, space);
			LOGP(DRLCMACDL, LOGL_INFO, "Complete DL frame for "
				"TBF=%d that fits precisely in last block: "
				"len=%d\n", tfi, llc_length);
			gprs_rlcmac_dl_bw(this, llc_length);
			/* block is filled, so there is no extension */
			*e_pointer |= 0x01;
			/* fill space */
			memcpy(data, llc_frame + llc_index, space);
			/* reset LLC frame */
			llc_index = llc_length = 0;
			/* final block */
			rh->fbi = 1; /* we indicate final block */
			tbf_new_state(this, GPRS_RLCMAC_FINISHED);
			/* return data block as message */
			break;
		}
		/* if chunk would fit exactly in space left */
		if (chunk == space) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "-- Chunk with length %d "
				"would exactly fit into space (%d): add length "
				"header with LI=0, to make frame extend to "
				"next block, and we are done\n", chunk, space);
			/* make space for delimiter */
			if (delimiter != data)
				memcpy(delimiter + 1, delimiter,
					data - delimiter);
			data++;
			space--;
			/* add LI with 0 length */
			li = (struct rlc_li_field *)delimiter;
			li->e = 1; /* not more extension */
			li->m = 0; /* shall be set to 0, in case of li = 0 */
			li->li = 0; /* chunk fills the complete space */
			// no need to set e_pointer nor increase delimiter
			/* fill only space, which is 1 octet less than chunk */
			memcpy(data, llc_frame + llc_index, space);
			/* incement index */
			llc_index += space;
			/* return data block as message */
			break;
		}
		LOGP(DRLCMACDL, LOGL_DEBUG, "-- Chunk with length %d is less "
			"than remaining space (%d): add length header to "
			"to delimit LLC frame\n", chunk, space);
		/* the LLC frame chunk ends in this block */
		/* make space for delimiter */
		if (delimiter != data)
			memcpy(delimiter + 1, delimiter, data - delimiter);
		data++;
		space--;
		/* add LI to delimit frame */
		li = (struct rlc_li_field *)delimiter;
		li->e = 0; /* Extension bit, maybe set later */
		li->m = 0; /* will be set later, if there is more LLC data */
		li->li = chunk; /* length of chunk */
		e_pointer = delimiter; /* points to E of current delimiter */
		delimiter++;
		/* copy (rest of) LLC frame to space */
		memcpy(data, llc_frame + llc_index, chunk);
		data += chunk;
		space -= chunk;
		LOGP(DRLCMACDL, LOGL_INFO, "Complete DL frame for TBF=%d: "
			"len=%d\n", tfi, llc_length);
		gprs_rlcmac_dl_bw(this, llc_length);
		/* reset LLC frame */
		llc_index = llc_length = 0;
		/* dequeue next LLC frame, if any */
		msg = llc_dequeue(gprs_bssgp_pcu_current_bctx());
		if (msg) {
			LOGP(DRLCMACDL, LOGL_INFO, "- Dequeue next LLC for "
				"TBF=%d (len=%d)\n", tfi, msg->len);
			update_llc_frame(msg);
			msgb_free(msg);
		}
		/* if we have more data and we have space left */
		if (space > 0 && llc_length) {
			li->m = 1; /* we indicate more frames to follow */
			continue;
		}
		/* if we don't have more LLC frames */
		if (!llc_length) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "-- Final block, so we "
				"done.\n");
			li->e = 1; /* we cannot extend */
			rh->fbi = 1; /* we indicate final block */
			first_fin_ack = 1;
				/* + 1 indicates: first final ack */
			tbf_new_state(this, GPRS_RLCMAC_FINISHED);
			break;
		}
		/* we have no space left */
		LOGP(DRLCMACDL, LOGL_DEBUG, "-- No space left, so we are "
			"done.\n");
		li->e = 1; /* we cannot extend */
		break;
	}
	LOGP(DRLCMACDL, LOGL_DEBUG, "data block: %s\n",
		osmo_hexdump(rlc_block[index], block_length));
	rlc_block_len[index] = block_length;
	/* raise send state and set ack state array */
	dir.dl.v_b[index] = 'U'; /* unacked */
	dir.dl.v_s = (dir.dl.v_s + 1) & mod_sns; /* inc send state */

tx_block:
	/* from this point on, new block is sent or old block is resent */

	/* get data and header from current block */
	data = rlc_block[index];
	len = rlc_block_len[index];
	rh = (struct rlc_dl_header *)data;

	/* Clear Polling, if still set in history buffer */
	rh->s_p = 0;
		
	/* poll after POLL_ACK_AFTER_FRAMES frames, or when final block is tx.
	 */
	if (dir.dl.tx_counter >= POLL_ACK_AFTER_FRAMES || first_fin_ack) {
		if (first_fin_ack) {
			LOGP(DRLCMACDL, LOGL_DEBUG, "- Scheduling Ack/Nack "
				"polling, because first final block sent.\n");
		} else {
			LOGP(DRLCMACDL, LOGL_DEBUG, "- Scheduling Ack/Nack "
				"polling, because %d blocks sent.\n",
				POLL_ACK_AFTER_FRAMES);
		}
		/* scheduling not possible, because: */
		if (poll_state != GPRS_RLCMAC_POLL_NONE)
			LOGP(DRLCMAC, LOGL_DEBUG, "Polling is already "
				"sheduled for TBF=%d, so we must wait for "
				"requesting downlink ack\n", tfi);
		else if (control_ts != ts)
			LOGP(DRLCMAC, LOGL_DEBUG, "Polling cannot be "
				"sheduled in this TS %d, waiting for "
				"TS %d\n", ts, control_ts);
		else if (bts->sba()->find(trx_no, ts, (fn + 13) % 2715648))
			LOGP(DRLCMAC, LOGL_DEBUG, "Polling cannot be "
				"sheduled, because single block alllocation "
				"already exists\n");
		else  {
			LOGP(DRLCMAC, LOGL_DEBUG, "Polling sheduled in this "
				"TS %d\n", ts);
			dir.dl.tx_counter = 0;
			/* start timer whenever we send the final block */
			if (rh->fbi == 1)
				tbf_timer_start(this, 3191, bts_data()->t3191, 0);

			/* schedule polling */
			poll_state = GPRS_RLCMAC_POLL_SCHED;
			poll_fn = (fn + 13) % 2715648;

#ifdef DEBUG_DIAGRAM
			debug_diagram(bts, diag, "poll DL-ACK");
			if (first_fin_ack)
				debug_diagram(bts, diag, "(is first FINAL)");
			if (rh->fbi)
				debug_diagram(bts, diag, "(FBI is set)");
#endif

			/* set polling in header */
			rh->rrbp = 0; /* N+13 */
			rh->s_p = 1; /* Polling */

			/* Increment TX-counter */
			dir.dl.tx_counter++;
		}
	} else {
		/* Increment TX-counter */
		dir.dl.tx_counter++;
	}

	/* return data block as message */
	dl_msg = msgb_alloc(len, "rlcmac_dl_data");
	if (!dl_msg)
		return NULL;
	memcpy(msgb_put(dl_msg, len), data, len);

	return dl_msg;
}

struct msgb *gprs_rlcmac_tbf::create_dl_ass(uint32_t fn)
{
	struct msgb *msg;
	struct gprs_rlcmac_tbf *new_tbf;
	int poll_ass_dl = POLLING_ASSIGNMENT_DL;

	if (poll_ass_dl && direction == GPRS_RLCMAC_DL_TBF
	 && control_ts != first_common_ts) {
		LOGP(DRLCMAC, LOGL_NOTICE, "Cannot poll for downlink "
			"assigment, because MS cannot reply. (control TS=%d, "
			"first common TS=%d)\n", control_ts,
			first_common_ts);
		poll_ass_dl = 0;
	}
	if (poll_ass_dl) {
		if (poll_state != GPRS_RLCMAC_POLL_NONE) {
			LOGP(DRLCMAC, LOGL_DEBUG, "Polling is already sheduled "
				"for TBF=%d, so we must wait for downlink "
				"assignment...\n", tfi);
				return NULL;
		}
		if (bts->sba()->find(trx_no, control_ts, (fn + 13) % 2715648)) {
			LOGP(DRLCMACUL, LOGL_DEBUG, "Polling is already "
				"scheduled for single block allocation...\n");
			return NULL;
		}
	}

	/* on uplink TBF we get the downlink TBF to be assigned. */
	if (direction == GPRS_RLCMAC_UL_TBF) {
		/* be sure to check first, if contention resolution is done,
		 * otherwise we cannot send the assignment yet */
		if (!dir.ul.contention_resolution_done) {
			LOGP(DRLCMAC, LOGL_DEBUG, "Cannot assign DL TBF now, "
				"because contention resolution is not "
				"finished.\n");
			return NULL;
		}
		#warning "THIS should probably go over the IMSI too"
		new_tbf = bts->tbf_by_tlli(tlli, GPRS_RLCMAC_DL_TBF);
	} else
		new_tbf = this;
	if (!new_tbf) {
		LOGP(DRLCMACDL, LOGL_ERROR, "We have a schedule for downlink "
			"assignment at uplink TBF=%d, but there is no downlink "
			"TBF\n", tfi);
		dl_ass_state = GPRS_RLCMAC_DL_ASS_NONE;
		return NULL;
	}

	msg = msgb_alloc(23, "rlcmac_dl_ass");
	if (!msg)
		return NULL;
	bitvec *ass_vec = bitvec_alloc(23);
	if (!ass_vec) {
		msgb_free(msg);
		return NULL;
	}
	bitvec_unhex(ass_vec,
		"2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b");
	LOGP(DRLCMAC, LOGL_INFO, "TBF: START TFI: %u TLLI: 0x%08x Packet Downlink Assignment (PACCH)\n", new_tbf->tfi, new_tbf->tlli);
	RlcMacDownlink_t * mac_control_block = (RlcMacDownlink_t *)talloc_zero(tall_pcu_ctx, RlcMacDownlink_t);
	Encoding::write_packet_downlink_assignment(mac_control_block, tfi,
		(direction == GPRS_RLCMAC_DL_TBF), new_tbf,
		poll_ass_dl, bts_data()->alpha, bts_data()->gamma, -1, 0);
	LOGP(DRLCMAC, LOGL_DEBUG, "+++++++++++++++++++++++++ TX : Packet Downlink Assignment +++++++++++++++++++++++++\n");
	encode_gsm_rlcmac_downlink(ass_vec, mac_control_block);
	LOGPC(DCSN1, LOGL_NOTICE, "\n");
	LOGP(DRLCMAC, LOGL_DEBUG, "------------------------- TX : Packet Downlink Assignment -------------------------\n");
	bitvec_pack(ass_vec, msgb_put(msg, 23));
	bitvec_free(ass_vec);
	talloc_free(mac_control_block);

	if (poll_ass_dl) {
		poll_state = GPRS_RLCMAC_POLL_SCHED;
		poll_fn = (fn + 13) % 2715648;
		dl_ass_state = GPRS_RLCMAC_DL_ASS_WAIT_ACK;
	} else {
		dl_ass_state = GPRS_RLCMAC_DL_ASS_NONE;
		tbf_new_state(new_tbf, GPRS_RLCMAC_FLOW);
		tbf_assign_control_ts(new_tbf);
		/* stop pending assignment timer */
		new_tbf->stop_timer();

	}
	debug_diagram(bts, diag, "send DL-ASS");

	return msg;
}

struct msgb *gprs_rlcmac_tbf::create_ul_ass(uint32_t fn)
{
	struct msgb *msg;
	struct gprs_rlcmac_tbf *new_tbf;

#if POLLING_ASSIGNMENT_UL == 1
	if (poll_state != GPRS_RLCMAC_POLL_NONE) {
		LOGP(DRLCMACUL, LOGL_DEBUG, "Polling is already "
			"sheduled for TBF=%d, so we must wait for uplink "
			"assignment...\n", tfi);
			return NULL;
	}
	if (bts->sba()->find(trx_no, control_ts, (fn + 13) % 2715648)) {
		LOGP(DRLCMACUL, LOGL_DEBUG, "Polling is already scheduled for "
			"single block allocation...\n");
			return NULL;
	}
#endif

	/* on down TBF we get the uplink TBF to be assigned. */
#warning "Probably want to find by IMSI too"
	if (direction == GPRS_RLCMAC_DL_TBF)
		new_tbf = bts->tbf_by_tlli(tlli, GPRS_RLCMAC_UL_TBF);
	else
		new_tbf = this;

	if (!new_tbf) {
		LOGP(DRLCMACUL, LOGL_ERROR, "We have a schedule for uplink "
			"assignment at downlink TBF=%d, but there is no uplink "
			"TBF\n", tfi);
		ul_ass_state = GPRS_RLCMAC_UL_ASS_NONE;
		return NULL;
	}

	msg = msgb_alloc(23, "rlcmac_ul_ass");
	if (!msg)
		return NULL;
	LOGP(DRLCMAC, LOGL_INFO, "TBF: START TFI: %u TLLI: 0x%08x Packet Uplink Assignment (PACCH)\n", new_tbf->tfi, new_tbf->tlli);
	bitvec *ass_vec = bitvec_alloc(23);
	if (!ass_vec) {
		msgb_free(msg);
		return NULL;
	}
	bitvec_unhex(ass_vec,
		"2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b");
	Encoding::write_packet_uplink_assignment(bts_data(), ass_vec, tfi,
		(direction == GPRS_RLCMAC_DL_TBF), tlli,
		tlli_valid, new_tbf, POLLING_ASSIGNMENT_UL, bts_data()->alpha,
		bts_data()->gamma, -1);
	bitvec_pack(ass_vec, msgb_put(msg, 23));
	RlcMacDownlink_t * mac_control_block = (RlcMacDownlink_t *)talloc_zero(tall_pcu_ctx, RlcMacDownlink_t);
	LOGP(DRLCMAC, LOGL_DEBUG, "+++++++++++++++++++++++++ TX : Packet Uplink Assignment +++++++++++++++++++++++++\n");
	decode_gsm_rlcmac_downlink(ass_vec, mac_control_block);
	LOGPC(DCSN1, LOGL_NOTICE, "\n");
	LOGP(DRLCMAC, LOGL_DEBUG, "------------------------- TX : Packet Uplink Assignment -------------------------\n");
	bitvec_free(ass_vec);
	talloc_free(mac_control_block);

#if POLLING_ASSIGNMENT_UL == 1
	poll_state = GPRS_RLCMAC_POLL_SCHED;
	poll_fn = (fn + 13) % 2715648;
	ul_ass_state = GPRS_RLCMAC_UL_ASS_WAIT_ACK;
#else
	ul_ass_state = GPRS_RLCMAC_UL_ASS_NONE;
	tbf_new_state(new_tbf, GPRS_RLCMAC_FLOW);
	tbf_assign_control_ts(new_tbf);
#endif
	debug_diagram(bts, diag, "send UL-ASS");

	return msg;
}

struct msgb *gprs_rlcmac_tbf::create_ul_ack(uint32_t fn)
{
	int final = (state_is(GPRS_RLCMAC_FINISHED));
	struct msgb *msg;

	if (final) {
		if (poll_state != GPRS_RLCMAC_POLL_NONE) {
			LOGP(DRLCMACUL, LOGL_DEBUG, "Polling is already "
				"sheduled for TBF=%d, so we must wait for "
				"final uplink ack...\n", tfi);
			return NULL;
		}
		if (bts->sba()->find(trx_no, control_ts, (fn + 13) % 2715648)) {
			LOGP(DRLCMACUL, LOGL_DEBUG, "Polling is already "
				"scheduled for single block allocation...\n");
			return NULL;
		}
	}

	msg = msgb_alloc(23, "rlcmac_ul_ack");
	if (!msg)
		return NULL;
	bitvec *ack_vec = bitvec_alloc(23);
	if (!ack_vec) {
		msgb_free(msg);
		return NULL;
	}
	bitvec_unhex(ack_vec,
		"2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b");
	RlcMacDownlink_t * mac_control_block = (RlcMacDownlink_t *)talloc_zero(tall_pcu_ctx, RlcMacDownlink_t);
	Encoding::write_packet_uplink_ack(bts_data(), mac_control_block, this, final);
	encode_gsm_rlcmac_downlink(ack_vec, mac_control_block);
	bitvec_pack(ack_vec, msgb_put(msg, 23));
	bitvec_free(ack_vec);
	talloc_free(mac_control_block);

	/* now we must set this flag, so we are allowed to assign downlink
	 * TBF on PACCH. it is only allowed when TLLI is acknowledged. */
	dir.ul.contention_resolution_done = 1;

	if (final) {
		poll_state = GPRS_RLCMAC_POLL_SCHED;
		poll_fn = (fn + 13) % 2715648;
		/* waiting for final acknowledge */
		ul_ack_state = GPRS_RLCMAC_UL_ACK_WAIT_ACK;
		dir.ul.final_ack_sent = 1;
	} else
		ul_ack_state = GPRS_RLCMAC_UL_ACK_NONE;
	debug_diagram(bts->bts, diag, "send UL-ACK");

	return msg;
}

int gprs_rlcmac_tbf::snd_dl_ack(uint8_t final, uint8_t ssn, uint8_t *rbb)
{
	char show_rbb[65], show_v_b[RLC_MAX_SNS + 1];
	uint16_t mod_sns = sns - 1;
	uint16_t mod_sns_half = (sns >> 1) - 1;
	int i; /* must be signed */
	int16_t dist; /* must be signed */
	uint8_t bit;
	uint16_t bsn;
	struct msgb *msg;
	uint16_t lost = 0, received = 0;

	LOGP(DRLCMACDL, LOGL_DEBUG, "TBF=%d downlink acknowledge\n", tfi);

	if (!final) {
		/* show received array in debug (bit 64..1) */
		for (i = 63; i >= 0; i--) {
			bit = (rbb[i >> 3]  >>  (7 - (i&7)))   & 1;
			show_rbb[i] = (bit) ? '1' : 'o';
		}
		show_rbb[64] = '\0';
		LOGP(DRLCMACDL, LOGL_DEBUG, "- ack:  (BSN=%d)\"%s\""
			"(BSN=%d)  1=ACK o=NACK\n", (ssn - 64) & mod_sns,
			show_rbb, (ssn - 1) & mod_sns);

		/* apply received array to receive state (SSN-64..SSN-1) */
		/* calculate distance of ssn from V(S) */
		dist = (dir.dl.v_s - ssn) & mod_sns;
		/* check if distance is less than distance V(A)..V(S) */
		if (dist >= ((dir.dl.v_s - dir.dl.v_a) & mod_sns)) {
			/* this might happpen, if the downlink assignment
			 * was not received by ms and the ack refers
			 * to previous TBF
			 * FIXME: we should implement polling for
			 * control ack!*/
			LOGP(DRLCMACDL, LOGL_NOTICE, "- ack range is out of "
				"V(A)..V(S) range (DL TBF=%d) Free TFB!\n", tfi);
				return 1; /* indicate to free TBF */
		}
		/* SSN - 1 is in range V(A)..V(S)-1 */
		for (i = 63, bsn = (ssn - 1) & mod_sns;
		     i >= 0 && bsn != ((dir.dl.v_a - 1) & mod_sns);
		     i--, bsn = (bsn - 1) & mod_sns) {
			bit = (rbb[i >> 3]  >>  (7 - (i&7)))   & 1;
			if (bit) {
				LOGP(DRLCMACDL, LOGL_DEBUG, "- got "
					"ack for BSN=%d\n", bsn);
				if (dir.dl.v_b[bsn & mod_sns_half]
								!= 'A')
					received++;
				dir.dl.v_b[bsn & mod_sns_half] = 'A';
			} else {
				LOGP(DRLCMACDL, LOGL_DEBUG, "- got "
					"NACK for BSN=%d\n", bsn);
				dir.dl.v_b[bsn & mod_sns_half] = 'N';
				lost++;
			}
		}
		/* report lost and received packets */
		gprs_rlcmac_received_lost(this, received, lost);

		/* raise V(A), if possible */
		for (i = 0, bsn = dir.dl.v_a; bsn != dir.dl.v_s;
		     i++, bsn = (bsn + 1) & mod_sns) {
			if (dir.dl.v_b[bsn & mod_sns_half] == 'A') {
				dir.dl.v_b[bsn & mod_sns_half] = 'I';
					/* mark invalid */
				dir.dl.v_a = (dir.dl.v_a + 1)
								& mod_sns;
			} else
				break;
		}

		/* show receive state array in debug (V(A)..V(S)-1) */
		for (i = 0, bsn = dir.dl.v_a; bsn != dir.dl.v_s;
		     i++, bsn = (bsn + 1) & mod_sns) {
			show_v_b[i] = dir.dl.v_b[bsn & mod_sns_half];
			if (show_v_b[i] == 0)
				show_v_b[i] = ' ';
		}
		show_v_b[i] = '\0';
		LOGP(DRLCMACDL, LOGL_DEBUG, "- V(B): (V(A)=%d)\"%s\""
			"(V(S)-1=%d)  A=Acked N=Nacked U=Unacked "
			"X=Resend-Unacked\n", dir.dl.v_a, show_v_b,
			(dir.dl.v_s - 1) & mod_sns);

		if (state_is(GPRS_RLCMAC_FINISHED)
		 && dir.dl.v_s == dir.dl.v_a) {
			LOGP(DRLCMACDL, LOGL_NOTICE, "Received acknowledge of "
				"all blocks, but without final ack "
				"inidcation (don't worry)\n");
		}
		return 0;
	}

	LOGP(DRLCMACDL, LOGL_DEBUG, "- Final ACK received.\n");
	debug_diagram(ts, diag, "got Final ACK");
	/* range V(A)..V(S)-1 */
	for (bsn = dir.dl.v_a; bsn != dir.dl.v_s;
	     bsn = (bsn + 1) & mod_sns) {
		if (dir.dl.v_b[bsn & mod_sns_half] != 'A')
			received++;
	}

	/* report all outstanding packets as received */
	gprs_rlcmac_received_lost(this, received, lost);

	/* check for LLC PDU in the LLC Queue */
	msg = llc_dequeue(gprs_bssgp_pcu_current_bctx());
	if (!msg) {
		/* no message, start T3193, change state to RELEASE */
		LOGP(DRLCMACDL, LOGL_DEBUG, "- No new message, so we "
			"release.\n");
		/* start T3193 */
		debug_diagram(bts, diag, "start T3193");
		tbf_timer_start(this, 3193,
			bts_data()->t3193_msec / 1000,
			(bts_data()->t3193_msec % 1000) * 1000);
		tbf_new_state(this, GPRS_RLCMAC_WAIT_RELEASE);

		return 0;
	}
	#warning "Copy and paste on the sender path"
	update_llc_frame(msg);
	msgb_free(msg);

	/* we have a message, so we trigger downlink assignment, and there
	 * set the state to ASSIGN. also we set old_downlink, because we
	 * re-use this tbf. */
	LOGP(DRLCMAC, LOGL_DEBUG, "Trigger dowlink assignment on PACCH, "
		"because another LLC PDU has arrived in between\n");
	memset(&dir.dl, 0, sizeof(dir.dl)); /* reset RLC states */
	state_flags &= GPRS_RLCMAC_FLAG_TO_MASK; /* keep TO flags */
	state_flags &= ~(1 << GPRS_RLCMAC_FLAG_CCCH);
	update();
	bts->trigger_dl_ass(this, this, NULL);
	return 0;
}

void gprs_rlcmac_tbf::free_all(struct gprs_rlcmac_trx *trx)
{
	for (uint8_t tfi = 0; tfi < 32; tfi++) {
		struct gprs_rlcmac_tbf *tbf;

		tbf = trx->ul_tbf[tfi];
		if (tbf)
			tbf_free(tbf);
		tbf = trx->dl_tbf[tfi];
		if (tbf)
			tbf_free(tbf);
	}
}

void gprs_rlcmac_tbf::free_all(struct gprs_rlcmac_pdch *pdch)
{
	for (uint8_t tfi = 0; tfi < 32; tfi++) {
		struct gprs_rlcmac_tbf *tbf;

		tbf = pdch->ul_tbf[tfi];
		if (tbf)
			tbf_free(tbf);
		tbf = pdch->dl_tbf[tfi];
		if (tbf)
			tbf_free(tbf);
	}
}
