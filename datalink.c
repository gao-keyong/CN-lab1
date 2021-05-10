#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "datalink.h"

#define DATA_TIMER  2500
#define ACK_TIMER	300
#define MAX_SEQ		127
#define NR_BUFS     ((MAX_SEQ+1)/2)

struct FRAME {
	unsigned char kind; /* FRAME_DATA */
	unsigned char ack;
	unsigned char seq;
	unsigned char data[PKT_LEN];
	unsigned int  padding;
};

int no_nak = 1;

static unsigned char data_timer_num = 0;

static unsigned char ack_expected = 0, next_frame_to_send = 0;
static unsigned char out_buf[NR_BUFS][PKT_LEN];

static unsigned char frame_expected = 0, too_far = NR_BUFS;
static unsigned char in_buf[NR_BUFS][PKT_LEN];

int arrived[NR_BUFS];
static unsigned char nbuffered;
static int phl_ready = 0;

static int between(unsigned char a, unsigned char b, unsigned char c)
{
	return (a <= b && b < c) || (a <= b && c < a) || (b < c&& c < a);
}

static void put_frame(unsigned char* frame, int len)
{
	*(unsigned int*)(frame + len) = crc32(frame, len);
	send_frame(frame, len + 4);
	phl_ready = 0;
}

static void send_data_frame(unsigned char frame_nr)
{
	struct FRAME s;

	s.kind = FRAME_DATA;
	s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
	s.seq = frame_nr;
	memcpy(s.data, out_buf[frame_nr % NR_BUFS], PKT_LEN);

	dbg_frame("Send DATA %d ACK %d, ID %d, Window  %d, PHL Queue %d\n", s.seq, s.ack, *(short*)s.data, (next_frame_to_send - ack_expected + MAX_SEQ + 1) % (MAX_SEQ + 1), phl_sq_len());

	put_frame((unsigned char*)&s, 3 + PKT_LEN);
	start_timer(frame_nr, DATA_TIMER);
	stop_ack_timer();
}

static void send_ack_frame(void)
{
	struct FRAME s;

	s.kind = FRAME_ACK;
	s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);

	dbg_frame("Send ACK  %d\n", s.ack);

	put_frame((unsigned char*)&s, 2);
	stop_ack_timer();
}

static void send_nak_frame(void)
{
	struct FRAME s;

	s.kind = FRAME_NAK;
	s.ack = (frame_expected + MAX_SEQ) % (MAX_SEQ + 1);
	no_nak = 0;

	dbg_frame("Send NAK  %d\n", s.ack);

	put_frame((unsigned char*)&s, 2);
	stop_ack_timer();
}

int main(int argc, char** argv)
{
	int event, arg;
	struct FRAME f;
	int len = 0;

	protocol_init(argc, argv);
	lprintf("Designed by Yin Hanyan, build: " __DATE__"  "__TIME__"\n");

	disable_network_layer();

	for (;;) {
		event = wait_for_event(&arg);

		switch (event) {
		case NETWORK_LAYER_READY:
			++nbuffered;
			get_packet(out_buf[next_frame_to_send % NR_BUFS]);
			send_data_frame(next_frame_to_send);
			next_frame_to_send = (next_frame_to_send + 1) % (MAX_SEQ + 1);
			break;

		case PHYSICAL_LAYER_READY:
			phl_ready = 1;
			break;

		case FRAME_RECEIVED:
			len = recv_frame((unsigned char*)&f, sizeof f);
			if (len < 5 || crc32((unsigned char*)&f, len) != 0) {
				dbg_event("**** Receiver Error, Bad CRC Checksum. Frame %d Expected. \n", frame_expected);
				if (no_nak || f.seq == frame_expected) {
					send_nak_frame();
				}
				else
				{
					start_ack_timer(ACK_TIMER);
				}
				break;
			}

			// no error data frame
			if (f.kind == FRAME_DATA) {
				dbg_frame("Recv DATA %d ACK %d, ID %d, Window %d\n", f.seq, f.ack, *(short*)f.data, (too_far - frame_expected + MAX_SEQ + 1) % (MAX_SEQ + 1));

				// 回发确认
				if (f.seq != frame_expected && no_nak) {
					send_nak_frame();
				}
				else
				{
					start_ack_timer(ACK_TIMER);
				}

				// 上交网络层
				if (between(frame_expected, f.seq, too_far) && !arrived[f.seq % NR_BUFS]) {
					arrived[f.seq % NR_BUFS] = 1;
					memcpy(in_buf[f.seq % NR_BUFS], f.data, PKT_LEN);
					while (arrived[frame_expected % NR_BUFS])
					{
						// dbg_event("put_packet %d\n", *(short*)in_buf[frame_expected % NR_BUFS]);
						put_packet(in_buf[frame_expected % NR_BUFS], PKT_LEN);
						no_nak = 1;
						arrived[frame_expected % NR_BUFS] = 0;
						frame_expected = (frame_expected + 1) % (MAX_SEQ + 1);
						too_far = (too_far + 1) % (MAX_SEQ + 1);
						start_ack_timer(ACK_TIMER);
					}
				}
			}

			// ack frame
			if (f.kind == FRAME_NAK) {
				dbg_frame("Recv NAK %d\n", f.ack);
				if (between(ack_expected, (f.ack + 1) % (MAX_SEQ + 1), next_frame_to_send)) {
					send_data_frame((f.ack + 1) % (MAX_SEQ + 1));
				}
			}

			while (between(ack_expected, f.ack, next_frame_to_send))
			{
				--nbuffered;
				stop_timer(ack_expected);
				ack_expected = (ack_expected + 1) % (MAX_SEQ + 1);
			}
			break;

		case DATA_TIMEOUT:
			dbg_event("---- DATA %d timeout\n", arg);
			send_data_frame(arg);
			start_timer((arg + 1) % (MAX_SEQ + 1), 800);
			for (int i = (arg + 2) % (MAX_SEQ + 1);between(arg, i, next_frame_to_send);i = (i + 1) % (MAX_SEQ + 1)) {
				start_timer(i, 800 + ACK_TIMER);
			}
			break;

		case ACK_TIMEOUT:
			// dbg_event("---- ACK timeout\n");
			send_ack_frame();
			break;
		}

		if (nbuffered < NR_BUFS && phl_ready)
			enable_network_layer();
		else
			disable_network_layer();

	}
}
