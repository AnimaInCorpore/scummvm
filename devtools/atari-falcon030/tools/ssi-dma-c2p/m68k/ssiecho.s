; SSI DMA pass-through: the 68030 half of the DMA c2p feasibility test.
;
; Routes DMA playback into the DSP's SSI receiver and the SSI transmitter
; into DMA record, four stereo tracks each way so all eight 16-bit slots of
; the crossbar frame carry data, and boots dsp/ssiecho.asm, which echoes
; every word. Per crossbar clock it then answers two questions:
;
;   throughput  Both DMAs loop over their buffers for 2 s while the DSP
;               counts received words, frame syncs and slot phase errors;
;               the rate is taken against MFP Timer C. A phase error means
;               the DSP loop missed or doubled a slot.
;   integrity   One pass of a 256,000-byte pseudo-random pattern (four
;               320x200 8-bit frames) into a record buffer 16 KB longer.
;               The pattern must come back whole at a single word offset;
;               the offset, its slot phase and the pass times are reported.
;
; The clocks are the internal 25.175 MHz one at prescale 1 (49,170 Hz
; frames, the codec's own maximum) and the internal 32 MHz one at prescale 1
; (62,500 Hz). The Falcon specification allows 32 MHz for the matrix devices
; other than the codec, and the DAC is not in this route; Hatari models it,
; but only a real Falcon can say whether the DSP link runs at it.
;
; Both measurements run in supervisor mode with interrupts masked, as loops
; of short instructions that reach the DSP through the host port registers
; and time themselves on Timer C's counter (TOS leaves it at 2.4576 MHz / 64
; / 192: 200 wraps a second, 38,400 counts). On hardware that keeps TOS out
; of the way; under Hatari it matters more, because Hatari runs the DSP only
; between 68030 instructions, so a long instruction, a trap or an interrupt
; can hand the DSP two SSI words with no cycles between them. The TOS
; 200 Hz tick misses the masked seconds.
;
; The sound matrix is pinned rather than inherited (see F030MXDRV's
; docs/architecture.md, "Falcon audio-path constraints"), and the DSP
; transmitter is re-armed after Devconnect. On exit DMA playback is routed
; back to the DAC.
;
; Results are printed and duplicated in SSIECHO.TXT beside the program; the
; last line starts with "RESULT:".

        include "xbios.i"
        include "common.i"

        global  start

; Keep the command words in sync with dsp/ssiecho.asm.
CMD_PING        equ     $010000
CMD_SNAPSHOT    equ     $020000
CMD_READ_FRAMES equ     $030000
CMD_READ_PHASE  equ     $040000
CMD_RESET       equ     $050000
CMD_ARM         equ     $060000
CMD_READ_SR     equ     $070000
REPLY_PING      equ     $454348

DSP_ABILITY     equ     3
SLOTS           equ     8                 ; 16-bit slots per crossbar frame


PLAY_BYTES      equ     256000            ; four 320x200 8-bit frames
PLAY_WORDS      equ     PLAY_BYTES/2
REC_BYTES       equ     PLAY_BYTES+16384
REC_WORDS       equ     REC_BYTES/2
SENTINEL        equ     $dead
ANCHOR          equ     64                ; the pattern word the search looks for,
MATCH_WORDS     equ     8                 ; and how many from it must match
SEARCH_WORDS    equ     4096              ; record positions tried

WINDOW_TICKS    equ     400               ; 2 s throughput window (Timer C)
PASS_TIMEOUT    equ     600               ; 3 s for one integrity pass


        text

start:
        move.l  #results_buffer,results_ptr
        Cconws  txt_banner
        ; the DMA buffers must be in ST-RAM: the program's header flags leave
        ; TOS no choice but to load it, BSS included, there
        move.l  #play_buffer,play_base
        move.l  #rec_buffer,rec_base

        ; every blocking step prints its label first, so a hang on real
        ; hardware leaves a dangling label naming the call
        Cconws  txt_reserve
        Dsp_Reserve #16,#16
        tst.l   d0
        bmi     fail_step
        Cconws  txt_ok
        Cconws  txt_boot
        Dsp_ExecBoot ssiecho_boot_image,#SSIECHO_BOOT_WORDS,#DSP_ABILITY
        Cconws  txt_ok
        Cconws  txt_ping
        move.l  #CMD_PING,d0
        bsr     dsp_exchange
        cmp.l   #REPLY_PING,d0
        bne     fail_dsp
        Cconws  txt_ok
        Cconws  txt_lock
        Locksnd
        cmpi.l  #1,d0
        bne     fail_dsp
        Cconws  txt_ok

        bsr     fill_pattern

        lea     line_buffer,a0
        FSTR    txt_header
        bsr     line_done
        bsr     emit_line

        ; pin the matrix: nothing inherited from the desktop reaches the route
        Buffoper #0
        Sndstatus #SNDSTAT_RESET
        Soundcmd #SOUND_ADDERIN,#SOUND_MATRIXIN
        Setmode #SOUND_STEREO16
        Settracks #TRACKS4,#TRACKS4
        Setmontracks #0

        lea     config_table,a6
config_loop:
        move.l  (a6)+,d0                  ; clock, or -1 terminator
        bmi     configs_done
        move.w  d0,cfg_clock
        move.l  (a6)+,d0
        move.w  d0,cfg_prescale
        move.l  (a6)+,cfg_expected        ; frame rate in mHz
        move.l  (a6)+,cfg_name
        bsr     run_config
        bra     config_loop
configs_done:

        lea     line_buffer,a0
        tst.w   fail_count
        bne     .failed
        FSTR    txt_result_pass
        bra     .summary
.failed:
        FSTR    txt_result_fail
        moveq   #0,d0
        move.w  fail_count,d0
        bsr     fmt_u32
        FSTR    txt_result_fail2
.summary:
        bsr     line_done
        bsr     emit_line
        bsr     write_results

        ; hand the DAC back to DMA playback
        Buffoper #0
        Dsptristate #0,#0
        Settracks #0,#0
        Devconnect #SRC_DMAPLAY,#DST_DAC,#CLK_25M,#1,#NO_SHAKE
        Unlocksnd
exit_dsp:
        Dsp_Unlock
exit_key:
        bsr     wait_exit_key
        Pterm0

fail_step:
        Cconws  txt_failed
        bra     exit_key
fail_dsp:
        Cconws  txt_failed
        bra     exit_dsp

; ---------------------------------------------------------------------------
; One crossbar clock: connect, then the throughput window and the integrity
; pass. cfg_* describe it.
; ---------------------------------------------------------------------------
run_config:
        lea     line_buffer,a0
        FSTR    txt_config
        movea.l cfg_name,a1
        bsr     fmt_string
        bsr     line_done
        bsr     emit_line

        Buffoper #0
        Dsptristate #1,#1
        Devconnect #SRC_DMAPLAY,#DST_DSPRECV,cfg_clock,cfg_prescale,#NO_SHAKE
        Devconnect #SRC_DSPXMIT,#DST_DMAREC,cfg_clock,cfg_prescale,#NO_SHAKE
        move.l  #CMD_ARM+SLOTS-1,d0
        bsr     dsp_exchange
        bsr     settle
        Setbuffer #BUF_PLAY,play_base,play_end
        Setbuffer #BUF_RECORD,rec_base,rec_end
        Supexec read_regs
        lea     line_buffer,a0
        FSTR    txt_regs
        bsr     fmt_regs
        bsr     line_done
        bsr     emit_line

        bsr     measure_throughput
        bsr     check_integrity
        Buffoper #0
        rts

; Both DMAs loop while super_window takes the DSP's counters on two Timer C
; wraps WINDOW_TICKS apart. The read latency is the same at both ends, so
; it cancels.
measure_throughput:
        Buffoper #OP_PLAY+OP_PLAY_REPEAT+OP_RECORD+OP_RECORD_REPEAT
        bsr     settle
        Supexec super_window
        move.l  #CMD_READ_SR,d0
        bsr     dsp_exchange
        move.l  d0,meas_sr
        Buffoper #0

        move.l  meas_w1,d0
        sub.l   meas_w0,d0
        andi.l  #$00ffffff,d0
        move.l  d0,meas_words
        move.l  meas_p1,d0
        sub.l   meas_p0,d0
        andi.l  #$00ffffff,d0
        move.l  d0,meas_phase
        move.l  meas_f1,d0
        sub.l   meas_f0,d0
        andi.l  #$00ffffff,d0
        move.l  d0,meas_frames

        ; frame rate in mHz = frames * FINE_HZ * 1000 / counts
        mulu.l  #FINE_HZ*1000,d1:d0
        divu.l  meas_fine,d1:d0
        move.l  d0,meas_rate
        ; bytes per second = words * 2 * FINE_HZ / counts
        move.l  meas_words,d0
        mulu.l  #2*FINE_HZ,d1:d0
        divu.l  meas_fine,d1:d0
        move.l  d0,meas_bytes

        lea     line_buffer,a0
        FSTR    txt_frames
        move.l  meas_frames,d0
        bsr     fmt_u32
        FSTR    txt_words
        move.l  meas_words,d0
        bsr     fmt_u32
        FSTR    txt_phase_errors
        move.l  meas_phase,d0
        bsr     fmt_u32
        FSTR    txt_window_ms
        move.l  meas_fine,d0
        bsr     fmt_fine
        FSTR    txt_ssisr
        move.l  meas_sr,d0
        bsr     fmt_hex8
        bsr     line_done
        bsr     emit_line

        lea     line_buffer,a0
        FSTR    txt_rate
        move.l  meas_rate,d0
        bsr     fmt_millihertz
        FSTR    txt_expected
        move.l  cfg_expected,d0
        bsr     fmt_millihertz
        FSTR    txt_throughput
        move.l  meas_bytes,d0
        bsr     fmt_u32
        FSTR    txt_bytes_s

        ; within 0.1 % of the model ...
        tst.l   meas_frames
        beq     .fail
        move.l  meas_rate,d0
        sub.l   cfg_expected,d0
        bpl     .abs
        neg.l   d0
.abs:
        move.l  cfg_expected,d2
        divu.l  #1000,d2
        cmp.l   d2,d0
        bhi     .fail
        ; ... no slot missed, and eight words per frame: both snapshots fall
        ; somewhere inside a frame, so the words may differ by up to seven
        tst.l   meas_phase
        bne     .fail
        move.l  meas_frames,d0
        lsl.l   #3,d0
        sub.l   meas_words,d0
        bpl     .abs2
        neg.l   d0
.abs2:
        cmpi.l  #SLOTS-1,d0
        bhi     .fail
        FSTR    txt_pass
        bra     .lined
.fail:
        addq.w  #1,fail_count
        FSTR    txt_fail
.lined:
        bsr     line_done
        bsr     emit_line
        rts

; One pass of the pattern into a sentinel-filled record buffer, then locate
; the pattern and compare it word for word.
check_integrity:
        movea.l rec_base,a0
        move.l  #REC_WORDS-1,d0
.fill:
        move.w  #SENTINEL,(a0)+
        subq.l  #1,d0
        bpl     .fill

        Supexec super_pass
        tst.l   d0
        beq     .recorded
        ; timed out: say how far each DMA got
        Buffptr dma_pointers
        Buffoper #0
        addq.w  #1,fail_count
        lea     line_buffer,a0
        FSTR    txt_timeout
        move.l  dma_pointers,d0
        sub.l   play_base,d0
        bsr     fmt_u32
        FSTR    txt_timeout2
        move.l  dma_pointers+4,d0
        sub.l   rec_base,d0
        bsr     fmt_u32
        FSTR    txt_fail
        bsr     line_done
        bsr     emit_line
        bra     .show_head
.recorded:
        lea     line_buffer,a0
        FSTR    txt_pass_line
        move.l  pass_rec_fine,d0
        bsr     fmt_fine
        FSTR    txt_pass_line2
        move.l  #REC_BYTES*(FINE_HZ/64),d0 ; bytes per second, in two steps
        divu.l  pass_rec_fine,d0          ; to stay inside 32 bits
        mulu.l  #64,d0
        bsr     fmt_u32
        FSTR    txt_pass_line3
        move.l  pass_words,d0
        bsr     fmt_u32
        FSTR    txt_phase_errors
        move.l  pass_phase,d0
        bsr     fmt_u32
        bsr     line_done
        bsr     emit_line

        ; locate the pattern by a run of words past its start, so damage to
        ; the first words is reported rather than hiding the rest
        movea.l rec_base,a2
        moveq   #0,d6                     ; record position in words
.search:
        movea.l play_base,a0
        lea     ANCHOR*2(a0),a0
        lea     (a2,d6.l*2),a1
        moveq   #MATCH_WORDS-1,d0
.prefix:
        cmpm.w  (a0)+,(a1)+
        bne     .next_position
        dbf     d0,.prefix
        bra     .found
.next_position:
        addq.l  #1,d6
        cmpi.l  #SEARCH_WORDS,d6
        bcs     .search
.lost:
        addq.w  #1,fail_count
        lea     line_buffer,a0
        FSTR    txt_not_found
        bsr     line_done
        bsr     emit_line
        bra     .show_head

.found:
        sub.l   #ANCHOR,d6                ; the offset of pattern word 0
        bmi     .lost
        move.l  d6,pass_offset
        movea.l play_base,a0
        lea     (a2,d6.l*2),a1
        move.l  #PLAY_WORDS-1,d0
        moveq   #0,d5                     ; mismatches
        moveq   #-1,d4                    ; first mismatch
        moveq   #-1,d7                    ; last mismatch
        moveq   #0,d3                     ; index
.compare:
        cmpm.w  (a0)+,(a1)+
        beq     .same
        addq.l  #1,d5
        move.l  d3,d7
        tst.l   d4
        bpl     .same
        move.l  d3,d4
.same:
        addq.l  #1,d3
        subq.l  #1,d0
        bpl     .compare
        move.l  d5,pass_mismatches
        move.l  d4,pass_first
        move.l  d7,pass_last

        lea     line_buffer,a0
        FSTR    txt_offset
        move.l  pass_offset,d0
        bsr     fmt_u32
        FSTR    txt_phase
        move.l  pass_offset,d0
        andi.l  #SLOTS-1,d0
        bsr     fmt_u32
        FSTR    txt_mismatches
        move.l  pass_mismatches,d0
        bsr     fmt_u32
        tst.l   pass_mismatches
        beq     .intact
        FSTR    txt_first
        move.l  pass_first,d0
        bsr     fmt_u32
        FSTR    txt_last
        move.l  pass_last,d0
        bsr     fmt_u32
        addq.w  #1,fail_count
        FSTR    txt_fail
        bsr     line_done
        bsr     emit_line
        bsr     show_mismatches
        bra     .show_head
.intact:
        FSTR    txt_pass
        bsr     line_done
        bsr     emit_line
        rts

; What arrived first, for a pass that failed.
.show_head:
        lea     line_buffer,a0
        FSTR    txt_head
        movea.l rec_base,a2
        moveq   #16-1,d2
.head_word:
        move.b  #' ',(a0)+
        moveq   #0,d0
        move.w  (a2)+,d0
        bsr     fmt_hex16
        dbf     d2,.head_word
        bsr     line_done
        bsr     emit_line
        rts

; The first SHOW_MISMATCHES mismatches of the located pattern, one line
; each: pattern index, then the recorded words from one before to one after
; it against the pattern's. Shows whether a word was dropped, doubled or
; changed.
SHOW_MISMATCHES equ     8
show_mismatches:
        movem.l d2-d7/a2-a4,-(sp)
        movea.l play_base,a3
        movea.l rec_base,a4
        move.l  pass_offset,d0
        lea     (a4,d0.l*2),a4            ; the record word of pattern word 0
        moveq   #SHOW_MISMATCHES,d7
        moveq   #0,d6                     ; index
.scan:
        move.w  (a3,d6.l*2),d0
        cmp.w   (a4,d6.l*2),d0
        beq     .next
        lea     line_buffer,a0
        FSTR    txt_mismatch
        move.l  d6,d0
        bsr     fmt_u32
        FSTR    txt_mismatch_want
        moveq   #-1,d5
.want:
        move.l  d6,d1
        add.l   d5,d1
        move.b  #' ',(a0)+
        moveq   #0,d0
        move.w  (a3,d1.l*2),d0
        bsr     fmt_hex16
        addq.l  #1,d5
        cmpi.l  #1,d5
        ble     .want
        FSTR    txt_mismatch_got
        moveq   #-1,d5
.got:
        move.l  d6,d1
        add.l   d5,d1
        move.b  #' ',(a0)+
        moveq   #0,d0
        move.w  (a4,d1.l*2),d0
        bsr     fmt_hex16
        addq.l  #1,d5
        cmpi.l  #1,d5
        ble     .got
        bsr     line_done
        bsr     emit_line
        subq.l  #1,d7
        beq     .done
.next:
        addq.l  #1,d6
        cmpi.l  #PLAY_WORDS-1,d6
        bcs     .scan
.done:
        movem.l (sp)+,d2-d7/a2-a4
        rts

; The pattern: the high halves of a 32-bit LCG, so no short run of it
; repeats within the buffer.
fill_pattern:
        movea.l play_base,a0
        move.l  #PLAY_WORDS-1,d1
        move.l  #$12345678,d0
.word:
        mulu.l  #1103515245,d0
        add.l   #12345,d0
        swap    d0
        move.w  d0,(a0)+
        swap    d0
        subq.l  #1,d1
        bpl     .word
        move.l  play_base,d0
        add.l   #PLAY_BYTES,d0
        move.l  d0,play_end
        move.l  rec_base,d0
        add.l   #REC_BYTES,d0
        move.l  d0,rec_end
        rts

; ---------------------------------------------------------------------------
; Supervisor routines, interrupts masked.
; ---------------------------------------------------------------------------

; The throughput window: snapshots of the DSP's counters into meas_c0 and
; meas_c1 about WINDOW_TICKS Timer C wraps apart, each right after a timer
; read; meas_fine gets the Timer C counts between the two reads. The
; snapshot latency is the same at both ends, so it cancels.
super_window:
        move.w  sr,-(sp)
        ori.w   #$0700,sr
        moveq   #0,d6
        move.b  MFP_TCDR.w,d5
        TIMER_POLL
        bsr     fine_now
        move.l  d0,d7
        lea     meas_c0,a5
        bsr     super_snapshot
.window:
        TIMER_POLL
        cmpi.l  #WINDOW_TICKS,d6
        bcs     .window
        bsr     fine_now
        sub.l   d7,d0
        move.l  d0,meas_fine
        lea     meas_c1,a5
        bsr     super_snapshot
        move.w  (sp)+,sr
        rts

; One integrity pass: reset the DSP's counters, start both DMAs once, and
; note in Timer C counts when record has filled its buffer, which is when
; its last word loses the sentinel. d0.l returns 0, or -1 when that has not
; happened after PASS_TIMEOUT wraps.
;
; The sound control register would say so too, but under Hatari reading it
; costs more than an SSI slot, and polling it cost the echo a word per read
; at 62,500 Hz; RAM is cheap to read everywhere.
super_pass:
        move.w  sr,-(sp)
        ori.w   #$0700,sr
        move.l  #CMD_RESET,d0
        bsr     host_exchange
        movea.l rec_end,a3
        subq.l  #2,a3                     ; the last record word
        moveq   #0,d6
        move.b  MFP_TCDR.w,d5
        move.b  #$11,SND_CONTROL.w        ; play and record, once each
.poll:
        TIMER_POLL
        bsr     clear_data_cache
        cmpi.w  #SENTINEL,(a3)
        bne     .recorded
        cmpi.l  #PASS_TIMEOUT,d6
        bcs     .poll
        move.w  (sp)+,sr
        moveq   #-1,d0
        rts
.recorded:
        bsr     fine_now
        move.l  d0,pass_rec_fine
        move.l  #CMD_SNAPSHOT,d0
        bsr     host_exchange
        move.l  d0,pass_words
        move.l  #CMD_READ_PHASE,d0
        bsr     host_exchange
        move.l  d0,pass_phase
        bsr     clear_data_cache          ; for the comparison that follows
        move.w  (sp)+,sr
        moveq   #0,d0
        rts


; Words, frames and phase errors of one DSP snapshot into (a5)+.
super_snapshot:
        move.l  #CMD_SNAPSHOT,d0
        bsr     host_exchange
        move.l  d0,(a5)+
        move.l  #CMD_READ_FRAMES,d0
        bsr     host_exchange
        move.l  d0,(a5)+
        move.l  #CMD_READ_PHASE,d0
        bsr     host_exchange
        move.l  d0,(a5)+
        rts

        data

txt_banner:     dc.b 13,10,'SSI DMA pass-through test',13,10
                dc.b '=========================',13,10,0
txt_reserve:    dc.b 'DSP reserve ..... ',0
txt_boot:       dc.b 'DSP boot ........ ',0
txt_ping:       dc.b 'DSP ping ........ ',0
txt_lock:       dc.b 'sound lock ...... ',0
txt_ok:         dc.b 'ok',13,10,0
txt_failed:     dc.b 'FAILED',13,10,0
txt_header:     dc.b 'route: DMA play -> DSP SSI echo -> DMA record, 4 tracks, 8 slots',0
txt_config:     dc.b 'clock ',0
txt_regs:       dc.b '  regs',0
txt_frames:     dc.b '  window: frames ',0
txt_words:      dc.b '  words ',0
txt_phase_errors: dc.b '  slot errors ',0
txt_window_ms:  dc.b '  in ',0
txt_ssisr:      dc.b '  ssisr $',0
txt_rate:       dc.b '  frame rate ',0
txt_expected:   dc.b ' Hz (expected ',0
txt_throughput: dc.b ' Hz)  throughput ',0
txt_bytes_s:    dc.b ' B/s each way',0
txt_pass_line:  dc.b '  pass: record buffer full after ',0
txt_pass_line2: dc.b ' (',0
txt_pass_line3: dc.b ' B/s), DSP words ',0
txt_offset:     dc.b '  pattern at word ',0
txt_phase:      dc.b ' (slot phase ',0
txt_mismatches: dc.b '), mismatched words ',0
txt_first:      dc.b ', first ',0
txt_last:       dc.b ', last ',0
txt_not_found:  dc.b '  pattern not found in the record buffer  FAIL',0
txt_timeout:    dc.b '  pass timed out: play at byte ',0
txt_timeout2:   dc.b ', record at byte ',0
txt_head:       dc.b '  record head:',0
txt_mismatch:   dc.b '    word ',0
txt_mismatch_want: dc.b ': want',0
txt_mismatch_got: dc.b '  got',0
txt_pass:       dc.b '  PASS',0
txt_fail:       dc.b '  FAIL',0
txt_result_pass: dc.b 'RESULT: PASS',0
txt_result_fail: dc.b 'RESULT: FAIL (',0
txt_result_fail2: dc.b ' checks failed)',0
txt_filename:   dc.b 'SSIECHO.TXT',0
name_25m:       dc.b '25.175 MHz / 256 / 2 (49,170 Hz frames)',0
name_32m:       dc.b '32 MHz / 256 / 2 (62,500 Hz frames)',0


; Clock, prescale, expected frame rate in mHz, name; -1 terminated.
config_table:
        dc.l    CLK_25M,1,49169922,name_25m
        dc.l    CLK_32M,1,62500000,name_32m
        dc.l    -1

        include "ssiecho_boot.i"

        bss
        even
play_base:      ds.l 1
play_end:       ds.l 1
rec_base:       ds.l 1
rec_end:        ds.l 1
cfg_expected:   ds.l 1
cfg_name:       ds.l 1
meas_c0:                                  ; words, frames, phase errors: keep
meas_w0:        ds.l 1                    ; each triple in super_snapshot's
meas_f0:        ds.l 1                    ; order
meas_p0:        ds.l 1
meas_c1:
meas_w1:        ds.l 1
meas_f1:        ds.l 1
meas_p1:        ds.l 1
meas_fine:      ds.l 1
meas_words:     ds.l 1
meas_frames:    ds.l 1
meas_phase:     ds.l 1
meas_rate:      ds.l 1
meas_bytes:     ds.l 1
meas_sr:        ds.l 1
pass_rec_fine:  ds.l 1
pass_words:     ds.l 1
pass_phase:     ds.l 1
pass_offset:    ds.l 1
pass_mismatches: ds.l 1
pass_first:     ds.l 1
pass_last:      ds.l 1
dma_pointers:   ds.l 4
cfg_clock:      ds.w 1
cfg_prescale:   ds.w 1
fail_count:     ds.w 1
        cnop    0,16
play_buffer:    ds.b PLAY_BYTES
rec_buffer:     ds.b REC_BYTES

        include "common.s"

        end
