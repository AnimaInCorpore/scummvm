/* ScummVM - Graphic Adventure Engine
 *
 * Atari STE beam-timed palette raster. Schedule layout: atari-ste-raster.h.
 *
 * Why a continuous loop and not a per-line interrupt: writing 16 palette
 * registers takes 160 of the ~188 border cycles of a low-resolution line,
 * and a Timer B interrupt reaches its handler with up to a whole line of
 * jitter whenever a TOS Timer C or IKBD handler holds IPL 6. Palette writes
 * therefore have to be beam-synchronised and cycle-exact for the whole
 * scene region, as in ste/raster.s (Spectrum512Painter). The engine gets
 * the CPU back for the rest of the frame:
 *
 *  - The VBL vector hook (independent of the TOS VBL queue and of vblsem,
 *    so it runs every frame) presents a pending screen/schedule pair,
 *    writes the palette of lines 0 and 1 and arms MFP Timer B to count one
 *    display-enable pulse, i.e. to fire at the end of line 0.
 *  - The Timer B handler synchronises to the first fetch of line 1 through
 *    the video counter, exactly like raster.s does at the top of its frame,
 *    writes line 1's border group, then runs the raster.s line loop for
 *    lines 2 .. lines-1 with all interrupts masked and returns.
 *  - With colour mixing (a pending second field, atari-ste-raster.h) the
 *    VBL hook instead alternates the pair's two fields every frame and
 *    writes each field's palette; Timer B is not armed.
 *  - Should the handler be entered so late that line 1 has already started
 *    more than 300 cycles ago, it synchronises to the next line instead
 *    (those lines keep the line-0 palette), and gives up on the frame after
 *    eight lines.
 *
 * The line loop takes 508 cycles at 60 Hz and 512 at 50 Hz; the rate is read
 * from the sync register every frame.
 *
 * Masking interrupts for the ~9.1 ms of the loop can swallow one of the
 * 200 Hz Timer C ticks (two expiries in one masked window leave a single
 * pending bit). The handler samples Timer C's counter on entry and counts a
 * lost tick when the second expiry fits into the window, so getMillis()
 * can add it back.
 */

#include "../../platform/atari/symbols.h"

	.global	SYM(atari_ste_raster_install)
	.global	SYM(atari_ste_raster_uninstall)
	.global	SYM(atari_ste_display_palette)
	.global	SYM(atari_ste_pending_palette)
	.global	SYM(atari_ste_pending_screen)
	.global	SYM(atari_ste_pending_screen2)
	.global	SYM(atari_ste_mix_split)
	.global	SYM(atari_ste_raster_lines)
	.global	SYM(atari_ste_raster_60hz)
	.global	SYM(atari_ste_raster_enable)
	.global	SYM(atari_ste_vbl_count)
	.global	SYM(atari_ste_raster_frames)
	.global	SYM(atari_ste_raster_resynced)
	.global	SYM(atari_ste_raster_skipped)
	.global	SYM(atari_ste_raster_lost_ticks)
	.global	SYM(atari_ste_raster_last_screen)
	.global	SYM(atari_ste_raster_last_palette)

	.text

/* ------------------------------------------------------------------------
 * long atari_ste_raster_install(void)   -- Supexec()
 * ---------------------------------------------------------------------- */
SYM(atari_ste_raster_install):
	move.w	sr,-(sp)
	move.w	#0x2700,sr
	tst.w	installed
	bne.s	install_done

	move.b	0xffff820a.w,old_sync
	tst.w	SYM(atari_ste_raster_60hz)
	beq.s	install_keep_rate
	bclr	#1,0xffff820a.w
install_keep_rate:
	| The video counter model below assumes no STE line stride or fine scroll.
	clr.b	0xffff820f.w
	clr.b	0xffff8265.w

	move.l	0x70.w,old_vbl
	move.l	#SYM(atari_ste_vbl),0x70.w
	move.l	0x120.w,old_timer_b
	move.l	#SYM(atari_ste_timer_b),0x120.w

	clr.b	0xfffffa1b.w			| Timer B stopped
	bclr	#0,0xfffffa0b.w			| no pending Timer B interrupt
	bclr	#0,0xfffffa0f.w			| not in service
	bset	#0,0xfffffa07.w			| enable
	bset	#0,0xfffffa13.w			| unmask

	move.w	#1,installed
install_done:
	move.w	(sp)+,sr
	moveq	#0,d0
	rts

/* ------------------------------------------------------------------------
 * long atari_ste_raster_uninstall(void)   -- Supexec()
 * ---------------------------------------------------------------------- */
SYM(atari_ste_raster_uninstall):
	move.w	sr,-(sp)
	move.w	#0x2700,sr
	tst.w	installed
	beq.s	uninstall_done

	bclr	#0,0xfffffa13.w			| mask
	bclr	#0,0xfffffa07.w			| disable
	clr.b	0xfffffa1b.w
	bclr	#0,0xfffffa0b.w
	bclr	#0,0xfffffa0f.w

	move.l	old_timer_b,0x120.w
	move.l	old_vbl,0x70.w
	move.b	old_sync,0xffff820a.w

	clr.l	SYM(atari_ste_display_palette)
	clr.l	SYM(atari_ste_pending_screen)
	clr.l	SYM(atari_ste_pending_screen2)
	clr.l	current_screen2
	clr.l	split_palette
	clr.w	installed
uninstall_done:
	move.w	(sp)+,sr
	moveq	#0,d0
	rts

/* ------------------------------------------------------------------------
 * VBL vector ($70) hook, chained in front of the TOS handler.
 * ---------------------------------------------------------------------- */
	.ascii	"XBRA"
	.ascii	"SCUM"
old_vbl:
	dc.l	0
SYM(atari_ste_vbl):
	movem.l	d0-d1/a0-a1,-(sp)
	addq.l	#1,SYM(atari_ste_vbl_count)

	| Present a pending pair unless the engine holds the TOS VBL lock.
	tst.w	0x452.w
	ble.s	vbl_no_present
	move.l	SYM(atari_ste_pending_screen),d0
	beq.s	vbl_no_present
	move.l	d0,current_screen
	move.l	SYM(atari_ste_pending_screen2),current_screen2
	move.l	SYM(atari_ste_pending_palette),SYM(atari_ste_display_palette)
	clr.l	SYM(atari_ste_pending_screen)
	bsr	set_video_base
vbl_no_present:

	move.l	SYM(atari_ste_display_palette),d0
	beq	vbl_done
	move.l	d0,a0

	| Colour mixing: show the other field of the pair on every frame, with
	| its own palette, which follows the first field's in the schedule.
	move.l	current_screen2,d0
	beq.s	vbl_single_field
	not.b	mix_field
	bne.s	vbl_second_field
	move.l	current_screen,d0
	bra.s	vbl_show_field
vbl_second_field:
	lea	64(a0),a0
vbl_show_field:
	bsr	set_video_base
	lea	0xffff8240.w,a1
	.rept	8
	move.l	(a0)+,(a1)+
	.endr
	| a0 now points at this field's verb-bar palette. With a split, Timer B
	| installs it after the last room line (event count of display-enable
	| pulses, as the raster uses).
	tst.w	SYM(atari_ste_mix_split)
	beq	vbl_done
	move.l	a0,split_palette
	clr.b	0xfffffa1b.w
	bclr	#0,0xfffffa0b.w
	move.b	SYM(atari_ste_raster_lines)+1,0xfffffa21.w
	move.b	#8,0xfffffa1b.w
	bra	vbl_done

vbl_single_field:
	lea	0xffff8240.w,a1
	.rept	8
	move.l	(a0)+,(a1)+
	.endr

	| Video counter value at which display line 1 starts.
	move.l	current_screen,d0
	add.l	#160,d0
	move.l	d0,line1_address

	| The per-line loop is armed only when explicitly enabled; otherwise the
	| whole frame keeps the palette written above (lines 0/1), i.e. a single
	| stable 16-colour palette. The beam-timed loop is still under bring-up.
	tst.w	SYM(atari_ste_raster_enable)
	beq.s	vbl_done
	| Arm Timer B: event count of one display-enable pulse.
	clr.b	0xfffffa1b.w
	bclr	#0,0xfffffa0b.w
	move.b	#1,0xfffffa21.w
	move.b	#8,0xfffffa1b.w
vbl_done:
	movem.l	(sp)+,d0-d1/a0-a1
	move.l	old_vbl(pc),-(sp)
	rts

| Video base for the frames to come (high and mid byte first: writing them
| clears the low byte on the STE) and the video counter for this frame,
| which is still in its vertical blank. d0: screen address; clobbers d1.
set_video_base:
	move.l	d0,d1
	swap	d1
	move.b	d1,0xffff8201.w
	move.b	d1,0xffff8205.w
	move.l	d0,d1
	lsr.w	#8,d1
	move.b	d1,0xffff8203.w
	move.b	d1,0xffff8207.w
	move.b	d0,0xffff820d.w
	move.b	d0,0xffff8209.w
	rts

/* ------------------------------------------------------------------------
 * Timer B handler: the beam-timed frame.
 *
 * Register use inside the timed section (same as ste/raster.s):
 *   a3 schedule read pointer   a4 $ffff8240   a5 $ffff8209   a6 delay sled
 *   a0-a2 palette write pointers   d0 counter offset   d1 sync line low byte
 *   d7 dbf count   d2 Timer C count on entry   d6 lost-tick threshold
 * ---------------------------------------------------------------------- */

/* One frame: sync, line-1 border group, line loop. Instruction for
 * instruction the sync and the loop are ste/raster.s; the extra sub.b is
 * paid for with one NOP less in the sled (75 NOPs + lea + 8 writes +
 * tail NOPs = the 476 cycles that follow raster.s's jmp), so the line
 * loop is reached with the same phase relative to the line's first fetch.
 *   tail  NOPs between line 1's border group and the loop: 3 at 60 Hz,
 *         4 at 50 Hz (line 2 starts 4 cycles later there)
 *   pad   NOPs per loop line: 0 at 60 Hz (508 cycles), 1 at 50 Hz (512)
 */
.macro	raster_frame tail, pad
	lea	delay\@,a6
wait\@:
	cmp.b	(a5),d1
	beq.s	wait\@
	move.b	(a5),d0
	sub.b	d1,d0
	add.l	d0,a6
	jmp	(a6)
delay\@:
	.rept	75
	nop
	.endr
	lea	(a4),a2
	.rept	8
	move.l	(a3)+,(a2)+
	.endr
	.rept	\tail
	nop
	.endr
line\@:
	nop
	lea	(a4),a0
	lea	(a4),a1
	lea	(a4),a2
	.rept	8
	move.l	(a3)+,(a0)+
	.endr
	.rept	8
	move.l	(a3)+,(a1)+
	.endr
	.rept	8
	move.l	(a3)+,(a2)+
	.endr
	.if	\pad
	nop
	.endif
	dbf	d7,line\@
	bra	raster_done
.endm

	.ascii	"XBRA"
	.ascii	"SCUM"
old_timer_b:
	dc.l	0
SYM(atari_ste_timer_b):
	tst.l	split_palette
	bne	timer_b_split
	move.w	#0x2700,sr
	movem.l	d0-d7/a0-a6,-(sp)
	clr.b	0xfffffa1b.w			| one shot: the VBL hook arms the next one
	move.b	0xfffffa23.w,d2			| Timer C count on entry

	move.l	SYM(atari_ste_display_palette),d0
	beq	raster_exit
	move.l	d0,a3
	move.l	d0,SYM(atari_ste_raster_last_palette)
	move.l	current_screen,SYM(atari_ste_raster_last_screen)

	move.l	line1_address,d4		| counter value the sync line starts at
	moveq	#0,d3				| lines given up on
	moveq	#64,d5				| polls left before giving up on the frame
	move.w	SYM(atari_ste_raster_lines),d7
	subq.w	#3,d7				| dbf count for a sync at line 1
	bmi	raster_late

	| Where is the beam relative to the sync line? Read the full counter
	| (it advances only while a line is being fetched).
find_line:
	moveq	#0,d0
	move.b	0xffff8205.w,d0
	lsl.w	#8,d0
	move.b	0xffff8207.w,d0
	lsl.l	#8,d0
	move.b	0xffff8209.w,d0
	sub.l	d4,d0
	bpl.s	line_reached
	subq.w	#1,d5				| sync line not reached yet: poll on
	bne.s	find_line
	bra	raster_late
line_reached:
	cmp.l	#12,d0			| lock only at the line start, so the sled
	bls.s	sync_line			| jump lands inside the sled; else advance a line
	add.l	#160,d4				| further in: sync one line later
	addq.w	#1,d3
	subq.w	#1,d7
	bmi	raster_late
	cmp.w	#8,d3
	bhi	raster_late
	bra.s	find_line

sync_line:
	move.b	d4,d1				| low byte the counter holds until the sync line's first fetch
	tst.w	d3
	beq.s	sync_data_ready
	addq.l	#1,SYM(atari_ste_raster_resynced)
	move.w	d3,d0				| skip the given-up lines' groups: the
	mulu.w	#96,d0				| sync line's border group is what
	add.l	d0,a3				| the sled writes
sync_data_ready:
	lea	32(a3),a3			| past the VBL palette
	lea	0xffff8240.w,a4
	lea	0xffff8209.w,a5
	moveq	#0,d0
	btst	#1,0xffff820a.w
	beq	sync_60hz
	move.w	#160,d6
	raster_frame 4,1
sync_60hz:
	move.w	#157,d6
	raster_frame 3,0

raster_done:
	| Two Timer C expiries fit into the masked window if the count on entry
	| was at most the threshold (9.1 ms window, 26 us per count, 5 ms period).
	cmp.b	d6,d2
	bhi.s	raster_no_lost_tick
	addq.l	#1,SYM(atari_ste_raster_lost_ticks)
raster_no_lost_tick:
	addq.l	#1,SYM(atari_ste_raster_frames)
	bra.s	raster_exit
raster_late:
	addq.l	#1,SYM(atari_ste_raster_skipped)
raster_exit:
	movem.l	(sp)+,d0-d7/a0-a6
	bclr	#0,0xfffffa0f.w			| end of interrupt (software EOI mode)
	rte

| Colour mixing with a verb-bar palette: the VBL hook armed this one-shot
| count and pointed split_palette at the shown field's verb-bar palette.
timer_b_split:
	movem.l	a0-a1,-(sp)
	clr.b	0xfffffa1b.w			| one shot: the VBL hook arms the next one
	move.l	split_palette,a0
	clr.l	split_palette
	lea	0xffff8240.w,a1
	.rept	8
	move.l	(a0)+,(a1)+
	.endr
	movem.l	(sp)+,a0-a1
	bclr	#0,0xfffffa0f.w			| end of interrupt (software EOI mode)
	rte

	.data
	.even
SYM(atari_ste_display_palette):
	dc.l	0
SYM(atari_ste_pending_palette):
	dc.l	0
SYM(atari_ste_pending_screen):
	dc.l	0
SYM(atari_ste_pending_screen2):
	dc.l	0
SYM(atari_ste_mix_split):
	dc.w	0
split_palette:
	dc.l	0
SYM(atari_ste_raster_lines):
	dc.w	144
SYM(atari_ste_raster_60hz):
	dc.w	0
SYM(atari_ste_raster_enable):
	dc.w	0
SYM(atari_ste_vbl_count):
	dc.l	0
SYM(atari_ste_raster_frames):
	dc.l	0
SYM(atari_ste_raster_resynced):
	dc.l	0
SYM(atari_ste_raster_skipped):
	dc.l	0
SYM(atari_ste_raster_lost_ticks):
	dc.l	0
SYM(atari_ste_raster_last_screen):
	dc.l	0
SYM(atari_ste_raster_last_palette):
	dc.l	0
current_screen:
	dc.l	0
current_screen2:
	dc.l	0
line1_address:
	dc.l	0
installed:
	dc.w	0
old_sync:
	dc.b	0
mix_field:
	dc.b	0
	.even
