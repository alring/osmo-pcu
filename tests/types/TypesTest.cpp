/*
 * TypesTest.cpp Test the primitive data types
 *
 * Copyright (C) 2013 by Holger Hans Peter Freyther
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "bts.h"
#include "tbf.h"
#include "gprs_debug.h"

extern "C" {
#include <osmocom/core/application.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
}

void *tall_pcu_ctx;
int16_t spoof_mnc = 0, spoof_mcc = 0;

static void test_llc(void)
{
	{
		uint8_t data[LLC_MAX_LEN] = {1, 2, 3, 4, };
		uint8_t out;
		gprs_llc llc;
		llc.init();

		OSMO_ASSERT(llc.chunk_size() == 0);
		OSMO_ASSERT(llc.remaining_space() == LLC_MAX_LEN);
		OSMO_ASSERT(llc.frame_length() == 0);

		llc.put_frame(data, 2);
		OSMO_ASSERT(llc.remaining_space() == LLC_MAX_LEN - 2);
		OSMO_ASSERT(llc.frame_length() == 2);
		OSMO_ASSERT(llc.chunk_size() == 2);
		OSMO_ASSERT(llc.frame[0] == 1);
		OSMO_ASSERT(llc.frame[1] == 2);

		llc.append_frame(&data[3], 1);
		OSMO_ASSERT(llc.remaining_space() == LLC_MAX_LEN - 3);
		OSMO_ASSERT(llc.frame_length() == 3);
		OSMO_ASSERT(llc.chunk_size() == 3);

		/* consume two bytes */
		llc.consume(&out, 1);
		OSMO_ASSERT(llc.remaining_space() == LLC_MAX_LEN - 3);
		OSMO_ASSERT(llc.frame_length() == 3);
		OSMO_ASSERT(llc.chunk_size() == 2);

		/* check that the bytes are as we expected */
		OSMO_ASSERT(llc.frame[0] == 1);
		OSMO_ASSERT(llc.frame[1] == 2);
		OSMO_ASSERT(llc.frame[2] == 4);

		/* now fill the frame */
		llc.append_frame(data, llc.remaining_space() - 1);
		OSMO_ASSERT(llc.fits_in_current_frame(1));
		OSMO_ASSERT(!llc.fits_in_current_frame(2));
	}	
}

static void test_rlc()
{
	{
		struct gprs_rlc_data rlc = { 0, };
		memset(rlc.block, 0x23, RLC_MAX_LEN);
		uint8_t *p = rlc.prepare(20);
		OSMO_ASSERT(p == rlc.block);
		for (int i = 0; i < 20; ++i)
			OSMO_ASSERT(p[i] == 0x2B);
		for (int i = 20; i < RLC_MAX_LEN; ++i)
			OSMO_ASSERT(p[i] == 0x0);
	}
}

static void test_rlc_v_b()
{
	{
		gprs_rlc_v_b vb;
		vb.reset();

		for (size_t i = 0; i < RLC_MAX_SNS; ++i)
			OSMO_ASSERT(vb.is_invalid(i));

		vb.mark_unacked(23);
		OSMO_ASSERT(vb.is_unacked(23));

		vb.mark_nacked(23);
		OSMO_ASSERT(vb.is_nacked(23));

		vb.mark_acked(23);
		OSMO_ASSERT(vb.is_acked(23));

		vb.mark_resend(23);
		OSMO_ASSERT(vb.is_resend(23));

		vb.mark_invalid(23);
		OSMO_ASSERT(vb.is_invalid(23));
	}
}

static void test_rlc_v_n()
{
	{
		gprs_rlc_v_n vn;
		vn.reset();

		OSMO_ASSERT(!vn.is_received(0x23));
		OSMO_ASSERT(vn.state(0x23) == ' ');

		vn.mark_received(0x23);
		OSMO_ASSERT(vn.is_received(0x23));
		OSMO_ASSERT(vn.state(0x23) == 'R');

		vn.mark_missing(0x23);
		OSMO_ASSERT(!vn.is_received(0x23));
		OSMO_ASSERT(vn.state(0x23) == 'N');
	}
}

static void test_rlc_dl_ul_basic()
{
	{
		gprs_rlc_dl_window dl_win = { 0, };
		OSMO_ASSERT(dl_win.window_empty());
		OSMO_ASSERT(!dl_win.window_stalled());
		OSMO_ASSERT(dl_win.distance() == 0);

		dl_win.increment_send();
		OSMO_ASSERT(!dl_win.window_empty());
		OSMO_ASSERT(!dl_win.window_stalled());
		OSMO_ASSERT(dl_win.distance() == 1);

		for (int i = 1; i < 64; ++i) {
			dl_win.increment_send();
			OSMO_ASSERT(!dl_win.window_empty());
			OSMO_ASSERT(dl_win.distance() == i + 1);
		}

		OSMO_ASSERT(dl_win.distance() == 64);
		OSMO_ASSERT(dl_win.window_stalled());

		dl_win.raise(1);
		OSMO_ASSERT(dl_win.distance() == 63);
		OSMO_ASSERT(!dl_win.window_stalled());
		for (int i = 62; i >= 0; --i) {
			dl_win.raise(1);
			OSMO_ASSERT(dl_win.distance() == i);
		}

		OSMO_ASSERT(dl_win.distance() == 0);
		OSMO_ASSERT(dl_win.window_empty());

		dl_win.increment_send();
		dl_win.increment_send();
		dl_win.increment_send();
		dl_win.increment_send();
		OSMO_ASSERT(dl_win.distance() == 4);

		for (int i = 0; i < 128; ++i) {
			dl_win.increment_send();
			dl_win.increment_send();
			dl_win.raise(2);
			OSMO_ASSERT(dl_win.distance() == 4);
		}
	}

	{
		gprs_rlc_ul_window ul_win = { 0, };
		gprs_rlc_v_n v_n;
		int count;

		v_n.reset();

		OSMO_ASSERT(ul_win.is_in_window(0));
		OSMO_ASSERT(ul_win.is_in_window(63));
		OSMO_ASSERT(!ul_win.is_in_window(64));

		OSMO_ASSERT(!v_n.is_received(0));

		/* simulate to have received 0, 1 and 5 */
		OSMO_ASSERT(ul_win.is_in_window(0));
		v_n.mark_received(0);
		ul_win.raise_v_r(0, &v_n);
		count = ul_win.raise_v_q(&v_n);
		OSMO_ASSERT(v_n.is_received(0));
		OSMO_ASSERT(ul_win.v_q() == 1);
		OSMO_ASSERT(ul_win.v_r() == 1);
		OSMO_ASSERT(count == 1);

		OSMO_ASSERT(ul_win.is_in_window(1));
		v_n.mark_received(1);
		ul_win.raise_v_r(1, &v_n);
		count = ul_win.raise_v_q(&v_n);
		OSMO_ASSERT(v_n.is_received(0));
		OSMO_ASSERT(ul_win.v_q() == 2);
		OSMO_ASSERT(ul_win.v_r() == 2);
		OSMO_ASSERT(count == 1);

		OSMO_ASSERT(ul_win.is_in_window(5));
		v_n.mark_received(5);
		ul_win.raise_v_r(5, &v_n);
		count = ul_win.raise_v_q(&v_n);
		OSMO_ASSERT(v_n.is_received(0));
		OSMO_ASSERT(ul_win.v_q() == 2);
		OSMO_ASSERT(ul_win.v_r() == 6);
		OSMO_ASSERT(count == 0);

		OSMO_ASSERT(ul_win.is_in_window(65));
		OSMO_ASSERT(ul_win.is_in_window(2));
		OSMO_ASSERT(v_n.is_received(5));
		v_n.mark_received(65);
		ul_win.raise_v_r(65, &v_n);
		count = ul_win.raise_v_q(&v_n);
		OSMO_ASSERT(count == 0);
		OSMO_ASSERT(v_n.is_received(5));
		OSMO_ASSERT(ul_win.v_q() == 2);
		OSMO_ASSERT(ul_win.v_r() == 66);

		OSMO_ASSERT(ul_win.is_in_window(2));
		OSMO_ASSERT(!ul_win.is_in_window(66));
		v_n.mark_received(2);
		ul_win.raise_v_r(2, &v_n);
		count = ul_win.raise_v_q(&v_n);
		OSMO_ASSERT(count == 1);
		OSMO_ASSERT(ul_win.v_q() == 3);
		OSMO_ASSERT(ul_win.v_r() == 66);

		OSMO_ASSERT(ul_win.is_in_window(66));
		v_n.mark_received(66);
		ul_win.raise_v_r(66, &v_n);
		count = ul_win.raise_v_q(&v_n);
		OSMO_ASSERT(count == 0);
		OSMO_ASSERT(ul_win.v_q() == 3);
		OSMO_ASSERT(ul_win.v_r() == 67);

		for (int i = 3; i <= 67; ++i) {
			v_n.mark_received(i);
			ul_win.raise_v_r(i, &v_n);
			ul_win.raise_v_q(&v_n);
		}

		OSMO_ASSERT(ul_win.v_q() == 68);
		OSMO_ASSERT(ul_win.v_r() == 68);

		v_n.mark_received(68);
		ul_win.raise_v_r(68, &v_n);
		count = ul_win.raise_v_q(&v_n);
		OSMO_ASSERT(ul_win.v_q() == 69);
		OSMO_ASSERT(ul_win.v_r() == 69);
		OSMO_ASSERT(count == 1);

		/* now test the wrapping */
		OSMO_ASSERT(ul_win.is_in_window(4));
		OSMO_ASSERT(!ul_win.is_in_window(5));
		v_n.mark_received(4);
		ul_win.raise_v_r(4, &v_n);
		count = ul_win.raise_v_q(&v_n);
		OSMO_ASSERT(count == 0);
	}
}

int main(int argc, char **argv)
{
	osmo_init_logging(&gprs_log_info);
	log_set_use_color(osmo_stderr_target, 0);
	log_set_print_filename(osmo_stderr_target, 0);

	printf("Making some basic type testing.\n");
	test_llc();
	test_rlc();
	test_rlc_v_b();
	test_rlc_v_n();
	test_rlc_dl_ul_basic();
	return EXIT_SUCCESS;
}

/*
 * stubs that should not be reached
 */
extern "C" {
void l1if_pdch_req() { abort(); }
void l1if_connect_pdch() { abort(); }
void l1if_close_pdch() { abort(); }
void l1if_open_pdch() { abort(); }
}
