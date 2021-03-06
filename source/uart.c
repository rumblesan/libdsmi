/*
 *	uart.c - Nintendo DS SPI UART
 *
 *	Copyright Gottfried Haider & Gordan Savicic 2008-2009.
 *
 *	This program is free software: you can redistribute it and/or modify
 *	it under the terms of the GNU Lesser General Public License as published by
 *	the Free Software Foundation, either version 3 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU Lesser General Public License for more details.
 *
 *	You should have received a copy of the GNU Lesser General Public License
 *	along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <nds.h>
#include <nds/bios.h>	// for swi_delay()
#include <string.h>		// for memmove()
#include <time.h>		// for time()
#include <stdio.h>
#include "uart.h"
#include "spi.h"


#define UART_FIFO_BLOCK_SIZE	8				// number of bytes to purge at once from overflowing in-buffer
#define UART_IN_EMERG			8				// additional reserved bytes for in-buffer
#define UART_IN_SIZE			256				// size of in-buffer
#define UART_OUT_EMERG			4				// additional reserved bytes for out-buffer
#define UART_OUT_SIZE			256				// size of out-buffer
#define UART_SPI_RATE			2000			// default bps for spi timer
#define UART_SPI_SPEED			CARD_SPI_524_KHZ_CLOCK	// spi speed (see spi.h)
#define UART_TIMER_OFF			0xFF			// timer-off value (used for timer)


static float spi_rate = 0.0;					// true spi rate
static uint8 in[UART_IN_SIZE+UART_IN_EMERG];	// incoming buffer
static uint16 in_size = 0;						// number of bytes in in-buffer
static uint8 out[UART_OUT_SIZE+UART_OUT_EMERG];	// outgoing buffer
static uint16 out_head = 0;						// index of next byte to send [0..n]
static uint16 out_size = 0;						// number of bytes in out-buffer
static uint8 *prio_dest = NULL;					// destination buffer for raw data
static uint16 prio_head = 0;					// index of next raw-byte to send [0..n]
static uint32 prio_irq_bytes = 0;				// bitmask for disabling the timer irq
static uint16 prio_size = 0;					// number of raw-bytes in out buffer
static uint8 timer = UART_TIMER_OFF;			// timer number
static uint16 water_high = 0;					// 0 to turn off, 1..100
static uint16 water_low = 0;					// 0 to turn off, 1..100
static bool water_send = false;					// true if highwater notification has been send
static int inittimeout = 10;					// timeout for uart_init


// needed forward declarations
static void do_spi();
static void send_watermark(bool highwater);
static void timer_start();
static void timer_stop();


static void card_line_irq()
{
	// received a card line irq
	do_spi();
}


static void do_spi()
{
	static bool got_esc = false;
	uint8 read;
	
	// send byte
	if (out_head < out_size) {
		writeBlocking_cardSPI(out[out_head]);
		out_head++;
	} else {
		// write dummy byte
		writeBlocking_cardSPI(0x00);
	}
	
	// read in byte
	readBlocking_cardSPI(&read);
	
	// make sure the timer irq is on for the following byte
	timer_start();
	
	// handle raw (priority) buffer
	if (prio_head < prio_size) {
		// disable timer irq for certain bytes
		if (prio_irq_bytes & (1<<(prio_size-prio_head-2))) {
			timer_stop();
		}
		
		if (prio_head < 2) {
			// we don't redirect the first two bytes because they still 
			// contain normal payload
			prio_head++;
		} else {
			// route to seperate buffer (if set)
			if (prio_dest) {
				prio_dest[prio_head] = read;
			}
			prio_head++;
			return;
		}
	}
	
	// filter
	if (!got_esc && read == '\\') {
		// remove escape byte
		got_esc = true;
		return;
	} else if (got_esc) {
		// read can now be a null byte, a backslash, 0xff or any other char
		got_esc = false;
	} else if (read == 0x00 || read == 0xff) {
		// remove dummy byte (0x00)
		// remove byte we get when no cartridge is inserted (0xff)
		return;
	}
	
	// watermarks
	if (0 < water_high && in_size+1 >= water_high && !water_send) {
		// hit high water
		send_watermark(true);
		water_send = true;
	}
	if (0 < water_low && in_size+1 <= water_low && water_send) {
		// hit low water
		send_watermark(false);
		water_send = false;
	}
	
	// make some room
	if (in_size == UART_IN_SIZE) {
		// discard oldest bytes in queue
		memmove(in, in+UART_FIFO_BLOCK_SIZE, UART_IN_SIZE-UART_FIFO_BLOCK_SIZE);
		in_size -= UART_FIFO_BLOCK_SIZE;
	}
	
	// add byte to buffer
	in[in_size] = read;
	in_size++;
}


static void lock()
{
	if (timer != UART_TIMER_OFF) {
		irqDisable(BIT(timer+3)|IRQ_CARD_LINE);
	}
}


static void send_watermark(bool highwater)
{
	uint8 msg[] = { '\\', 'w', 0x00 };
	
	if (highwater) {
		msg[2] = 0x01;
	}
	uart_write_prio(msg, 3, NULL, 0x00);
}


static void timer_irq()
{
	do_spi();
}


static void timer_start()
{
	if (timer != UART_TIMER_OFF) {
		TIMER_CR(timer) |= TIMER_ENABLE;
	}
}


static void timer_stop()
{
	if (timer != UART_TIMER_OFF) {
		TIMER_CR(timer) &= ~TIMER_ENABLE;
	}
}


static void unlock()
{
	if (timer != UART_TIMER_OFF) {
		irqEnable(BIT(timer+3)|IRQ_CARD_LINE);
	}
}


// library functions

bool uart_init()
{
	int8 i;
	uint8 ver;
	
	if (timer != UART_TIMER_OFF)
		return false;
	
	// setup access
#ifdef ARM9
	REG_EXMEMCNT &= ~ARM7_OWNS_CARD;
#else
	REG_EXMEMCNT |= ARM7_OWNS_CARD;
#endif	// ARM9
	
	init_cardSPI();
	config_cardSPI(UART_SPI_SPEED, 1);
	
	// enable spi irqs
	irqSet(IRQ_CARD_LINE, card_line_irq);
	irqEnable(IRQ_CARD_LINE);
	
	// probe timers;
	for (i=3; 0<=i; i--) {
		if (TIMER_CR(i) & TIMER_ENABLE)
			continue;
		timer = i;
		break;
	}
	
	if (timer != UART_TIMER_OFF) {
		// enable timer irqs
		irqSet((IRQ_MASK)BIT(i+3), timer_irq);
		irqEnable((IRQ_MASK)BIT(i+3));
		// set default bps and enable timer
		uart_set_spi_rate(UART_SPI_RATE);
		// wait for the card to be ready
		do {

			ver = uart_firmware_ver();
			uart_wait();
			if (ver != 0x00 && ver != 0xff) {
			
				return true;
				
			} else if (inittimeout == 0 || ver == 0x00) {
				disable_cardSPI();
				irqDisable((IRQ_MASK)BIT(i+3));
				
				return false;
			}
			
			inittimeout--;
			
		} while (true);
	}
	
	return false;
}


uint16 uart_write(uint8 *buf, uint16 size)
{
	uint16 i;
	uint16 ret = 0;	

	// prevent do_spi() from changing buffers
	lock();
	
	// remove digested bytes from queue
	if (out_head > 0) {
		memmove(out, out+out_head, out_size-out_head);
		out_size -= out_head;
		out_head = 0;
	}
	
	// add buffer
	for (i=0; i<size; i++) {
		if (*(buf+i) == 0x00) {
			// escape null-bytes
			if (out_size + 2 > UART_OUT_SIZE)
				break;
			out[out_size] = '\\';
			out[out_size+1] = 0x00;
			out_size += 2;
		} else if (*(buf+i) == '\\') {
			// escape backslashes
			if (out_size + 2 > UART_OUT_SIZE)
				break;
			out[out_size] = '\\';
			out[out_size+1] = '\\';
			out_size += 2;
		} else {
			if (out_size + 1 > UART_OUT_SIZE)
				break;
			out[out_size] = *(buf+i);
			out_size++;
		}
		ret++;
	}
	
	unlock();
	
	
	return ret;
}


void uart_send(char *s)
{
	uint16 len = strlen(s);
	uint16 send = 0;
	
	while (send < len) {
		send += uart_write((uint8*)s+send, len-send);
		uart_wait();
	}
}


void uart_sendc(char c)
{
	while (1 != uart_write((uint8*)&c, 1)) {
		uart_wait();
	}
}


void uart_flush()
{
	while (out_head < out_size) {
		uart_wait();
	}
}


uint16 uart_available()
{
	return in_size;
}


uint16 uart_read(uint8 *dest, uint16 size)
{
	uint16 read = size;
	
	
	// prevent do_spi() from changing buffers
	lock();
	
	if (in_size < read)
		read = in_size;
	memcpy(dest, in, read);
	
	// move remaining in-buffer
	memmove(in, in+read, in_size-read);
	in_size -= read;
	
	unlock();
	
	
	return read;
}


uint16 uart_readstr(char *dest, uint16 size)
{
	uint16 len;
	
	len = uart_read((uint8*)dest, size-1);
	// make string null-terminated
	if (0 < size)
		dest[len] = '\0';
	
	// return the number of characters (sans null)
	return len;
}


uint16 uart_readln(char *dest, uint16 size, char nl)
{
	uint16 i;
	
	
	// prevent do_spi() from changing buffers
	lock();
	
	// look for newline
	for (i=0; i<in_size; i++) {
		if (in[i] == nl) {
			break;
		}
	}
	
	// nothing found?
	if (i == in_size) {
		size = 0;
		goto out;
	}
	
	// reserve a character for null-termination
	size--;
	
	// copy to destination
	if (i+1 < size)
		size = i+1;
	memcpy(dest, in+i+1-size, size);
	
	// make string null-terminated
	dest[size] = '\0';
	
	// move remaining in-buffer
	if (i+1 < in_size) {
		memmove(in, in+i+1, in_size-i-1);
		in_size -= i+1;
	}
	
out:
	unlock();
	
	
	// return the number of characters (sans null)
	return size;
}


bool uart_requeue(uint8 *src, uint16 size)
{

	// prevent do_spi() from changing buffers
	lock();
	
	// we are only doing this if we are not throwing away other
	// valid bytes while doing so
	if (size+in_size <= UART_IN_SIZE+UART_IN_EMERG) {
		memmove(in+size, in, in_size);
		memcpy(out, src, size);
		out_size += size;	
		unlock();
		
		return true;
	} else {
		unlock();
		
		return false;
	}
}


void uart_wait()
{
	if (timer != UART_TIMER_OFF) {
		swiIntrWait(0, BIT(timer+3)|IRQ_CARD_LINE);
	}
}


void uart_set_bps(uint32 bps)
{
	uint8 msg[] = { '\\', 'b', 0x00, 0x00, 0x00, 0x00 };
	
	msg[2] = (bps>>24)&0xff;
	msg[3] = (bps>>16)&0xff;
	msg[4] = (bps>>8)&0xff;
	msg[5] = (bps)&0xff;
	
	uart_write_prio(msg, 6, msg, 0x00);
	uart_wait_prio(0);
}


void uart_set_spi_rate(uint32 bps)
{
	if (timer == UART_TIMER_OFF)
		return;
	
	if (bps <= 32768) {
		TIMER_DATA(timer) = timerFreqToTicks_1024(bps);
		TIMER_CR(timer) = TIMER_DIV_1024|TIMER_ENABLE|TIMER_IRQ_REQ;
		spi_rate = 33.51392 / ((0x2000000 >> 10) / bps) * 1000;
	} else if (bps <= 131072) {
		TIMER_DATA(timer) = timerFreqToTicks_256(bps);
		TIMER_CR(timer) = TIMER_DIV_256|TIMER_ENABLE|TIMER_IRQ_REQ;
		spi_rate = 33.51392 / ((0x2000000 >> 8) / bps) * 1000;
	} else if (bps <= 524288) {
		TIMER_DATA(timer) = timerFreqToTicks_64(bps);
		TIMER_CR(timer) = TIMER_DIV_64|TIMER_ENABLE|TIMER_IRQ_REQ;
		spi_rate = 33.51392 / ((0x2000000 >> 6) / bps) * 1000;
	} else {
		TIMER_DATA(timer) = timerFreqToTicks_1(bps);
		TIMER_CR(timer) = TIMER_DIV_1|TIMER_ENABLE|TIMER_IRQ_REQ;
		spi_rate = 33.51392 / (0x2000000 / bps) * 1000;
	}
}


void uart_set_watermarks(uint16 high, uint16 low)
{
	water_high = UART_IN_SIZE*high/100;
	water_low = UART_IN_SIZE*low/100;
}


float uart_get_spi_rate()
{
	if (timer == UART_TIMER_OFF)
		return 0.0;
	
	// not exactly sure if the spi_rate code in uart_set_spi_rate() 
	// is correct (also see timer.h)
	
	return spi_rate;
}


void uart_write_prio(uint8 *buf, uint16 size, uint8 *dest, uint32 irq_bytes)
{
	
	
	// prevent do_spi() from changing buffers
	lock();
	
	// check if we exceed the buffer size
	if (UART_OUT_SIZE+UART_OUT_EMERG < size)
		return;
	
	if (size+out_size-out_head <= UART_OUT_SIZE+UART_OUT_EMERG) {
		memmove(out+size, out+out_head, out_size-out_head);
	} else {
		// preserve the newer bytes in the queue
		memmove(out+size, out+out_size-UART_OUT_SIZE+UART_OUT_EMERG-size, UART_OUT_SIZE+UART_OUT_EMERG-size);
	}
	memcpy(out, buf, size);
	
	// in the previous step we implicitly threw the bytes already 
	// digested, that's why out_size ends up being the priority 
	// bytes plus the remainder of the queue
	out_size = size+out_size-out_head;
	out_head = 0;
	
	prio_dest = dest;
	prio_head = 0;
	prio_irq_bytes = irq_bytes;
	prio_size = size;
	
	unlock();
	
	
}


bool uart_wait_prio(uint8 timeout)
{
	time_t start = time(NULL);
	
	// wait for sending to finish
	do {
		if (prio_head == prio_size) {
			// we're done, cleanup
			prio_size = 0;
			prio_head = 0;
			return true;
		}
		swiDelay(0);
	} while (timeout == 0 || time(NULL)-start <= timeout);
	
	// we timed out, cleanup
	lock();
	out_head = prio_size;
	prio_size = 0;
	prio_head = 0;
	unlock();
	
	// restart timer
	timer_start();
	
	return false;
}


uint8 uart_firmware_ver()
{
	uint8 msg[] = { '\\', 'v', 0x00 };
	
	uart_write_prio(msg, 3, msg, 0x00);
	uart_wait_prio(0);
	
	return msg[2];
}


void uart_close()
{
	if (timer != UART_TIMER_OFF) {
			// disable timer
			TIMER_CR(timer) &= ~TIMER_ENABLE;
			irqDisable((IRQ_MASK)BIT(timer+3));
			irqClear((IRQ_MASK)BIT(timer+3));
			timer = UART_TIMER_OFF;
	}
	// disable spi irqs
	irqDisable(IRQ_CARD_LINE);
	irqClear(IRQ_CARD_LINE);
	
	disable_cardSPI();
}
