; SSI DMA c2p in handshake mode: the 68030 half of the test of
; dsp/hsc2p.asm, the kernel a screen driver would use.
;
; Streams one 320x200 8-bit chunky screen through the DSP per pass, DMA
; playback into the SSI receiver and the SSI transmitter into DMA record,
; both in handshake mode at 25.175 MHz / prescale 1, and checks what record
; wrote word for word against the planar screen a 68030 reference c2p makes
; of the same pixels, laid out as the backend's planar screen is: per line
; PAD zero words, the line's 160 plane words, PAD zero words.
;
; In handshake mode the DSP asks for every word it takes and releases every
; word it gives, so the transfer cannot lose words however busy the 68030
; is. Pass 2 keeps it busy with long instructions the whole time, which in
; the free-running mode cost the DSP words under Hatari; here it must come
; out as exact as the quiet passes. Each pass reports its time on the TOS
; 200 Hz tick, and the rate that makes.
;
; Results are printed and duplicated in HSC2P.TXT beside the program; the
; last line starts with "RESULT:".

        include "xbios.i"
        include "common.i"

        global  start

; Keep the command words in sync with dsp/hsc2p.asm.
CMD_PING        equ     $010000
CMD_ARM         equ     $020000
CMD_GEOMETRY    equ     $030000           ; lines << 4 | pad words
CMD_SYNC        equ     $040000
CMD_STATE       equ     $050000
CMD_GROUPS      equ     $060000
REPLY_PING      equ     $485332

DSP_ABILITY     equ     3
HANDSHAKE       equ     0                 ; Devconnect protocol

WIDTH           equ     320
HEIGHT          equ     200
SCREEN_BYTES    equ     WIDTH*HEIGHT
SCREEN_WORDS    equ     SCREEN_BYTES/2
SCREEN_GROUPS   equ     SCREEN_BYTES/16
GROUPS_PER_LINE equ     WIDTH/16
PAD             equ     8                 ; zero words each side of a line
LINE_WORDS      equ     PAD+GROUPS_PER_LINE*8+PAD
REC_WORDS       equ     LINE_WORDS*HEIGHT
REC_BYTES       equ     REC_WORDS*2
SENTINEL        equ     $dead
PASSES          equ     3
BUSY_PASS       equ     2
PASS_TIMEOUT    equ     600               ; ticks: 3 s


        text

start:
        move.l  #results_buffer,results_ptr
        Cconws  txt_banner
        move.l  #chunky_buffer,d0
        move.l  d0,play_base
        add.l   #SCREEN_BYTES,d0
        move.l  d0,play_end
        move.l  #rec_buffer,d0
        move.l  d0,rec_base
        add.l   #REC_BYTES,d0
        move.l  d0,rec_end

        Cconws  txt_reserve
        Dsp_Reserve #16,#16
        tst.l   d0
        bmi     fail_step
        Cconws  txt_ok
        Cconws  txt_boot
        Dsp_ExecBoot hsc2p_boot_image,#HSC2P_BOOT_WORDS,#DSP_ABILITY
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

        Cconws  txt_preparing
        bsr     fill_chunky
        bsr     reference_c2p
        bsr     expected_record
        Cconws  txt_ok

        lea     line_buffer,a0
        FSTR    txt_header
        bsr     line_done
        bsr     emit_line

        ; pin the matrix, then the handshake route
        Buffoper #0
        Sndstatus #SNDSTAT_RESET
        Soundcmd #SOUND_ADDERIN,#SOUND_MATRIXIN
        Setmode #SOUND_STEREO16
        Settracks #TRACKS4,#TRACKS4
        Setmontracks #0
        Dsptristate #1,#1
        Devconnect #SRC_DMAPLAY,#DST_DSPRECV,#CLK_25M,#1,#HANDSHAKE
        Devconnect #SRC_DSPXMIT,#DST_DMAREC,#CLK_25M,#1,#HANDSHAKE
        move.l  #CMD_ARM,d0
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

        moveq   #1,d7
.pass:
        move.l  d7,pass_number
        bsr     run_pass
        addq.l  #1,d7
        cmpi.l  #PASSES,d7
        bls     .pass

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
; One screen through the DSP, timed, then the checks.
; ---------------------------------------------------------------------------
run_pass:
        movem.l d7,-(sp)
        movea.l rec_base,a0
        move.l  #REC_WORDS-1,d0
.fill:
        move.w  #SENTINEL,(a0)+
        subq.l  #1,d0
        bpl     .fill
        move.l  #CMD_GEOMETRY+(HEIGHT<<4)+PAD,d0
        bsr     dsp_exchange
        bsr     wait_tick_edge
        move.l  d0,pass_start
        move.l  #CMD_SYNC+GROUPS_PER_LINE,d0  ; the DSP asks for the first word
        bsr     dsp_exchange
        Buffoper #OP_PLAY+OP_RECORD
        cmpi.l  #BUSY_PASS,pass_number
        beq     .busy
.quiet:
        Buffoper #-1
        btst    #2,d0                     ; record still running
        beq     .recorded
        bsr     get_ticks
        sub.l   pass_start,d0
        cmpi.l  #PASS_TIMEOUT,d0
        bcs     .quiet
        bra     .timed_out
; Busy: long instructions among short ones, the way a game keeps a 68030
; busy, with a look at record every 64 rounds.
.busy:
        moveq   #63,d6
.busy_round:
        movem.l d0-d7/a0-a2,-(sp)
        movem.l (sp)+,d0-d7/a0-a2
        move.l  d6,d0
        addq.l  #7,d0
        moveq   #0,d1
        divu.l  #7,d1:d0
        mulu.l  #13,d0
        dbf     d6,.busy_round
        Buffoper #-1
        btst    #2,d0
        beq     .recorded
        bsr     get_ticks
        sub.l   pass_start,d0
        cmpi.l  #PASS_TIMEOUT,d0
        bcs     .busy
.timed_out:
        Buffoper #0
        addq.w  #1,fail_count
        lea     line_buffer,a0
        FSTR    txt_timeout
        bsr     line_done
        bsr     emit_line
        bra     .report
.recorded:
        bsr     get_ticks
        sub.l   pass_start,d0
        move.l  d0,pass_ticks
.report:
        Supexec clear_data_cache_super
        move.l  #CMD_STATE,d0
        bsr     dsp_exchange
        move.l  d0,dsp_state
        move.l  #CMD_GROUPS,d0
        bsr     dsp_exchange
        move.l  d0,dsp_groups

        lea     line_buffer,a0
        FSTR    txt_pass
        move.l  pass_number,d0
        bsr     fmt_u32
        lea     txt_quiet,a1
        cmpi.l  #BUSY_PASS,pass_number
        bne     .named
        lea     txt_busy,a1
.named:
        bsr     fmt_string
        move.l  pass_ticks,d0
        mulu.l  #5,d0                     ; ms
        bsr     fmt_u32
        FSTR    txt_ms_rate
        move.l  #SCREEN_BYTES*200,d0      ; chunky bytes a second
        move.l  pass_ticks,d1
        beq     .no_rate
        divu.l  d1,d0
        bsr     fmt_u32
.no_rate:
        FSTR    txt_groups
        move.l  dsp_groups,d0
        bsr     fmt_u32
        FSTR    txt_state
        move.l  dsp_state,d0
        bsr     fmt_u32
        ; the DSP converted the whole screen and finished
        cmpi.l  #2,dsp_state
        bne     .dsp_fail
        cmpi.l  #SCREEN_GROUPS,dsp_groups
        bne     .dsp_fail
        FSTR    txt_pass_ok
        bra     .dsp_lined
.dsp_fail:
        addq.w  #1,fail_count
        FSTR    txt_fail
.dsp_lined:
        bsr     line_done
        bsr     emit_line
        bsr     check_record
        movem.l (sp)+,d7
        rts

clear_data_cache_super:
        bsr     clear_data_cache
        rts

; Spin until the TOS tick advances; d0.l returns the fresh tick value.
wait_tick_edge:
        move.l  d3,-(sp)
        bsr     get_ticks
        move.l  d0,d3
.spin:
        bsr     get_ticks
        cmp.l   d3,d0
        beq     .spin
        move.l  (sp)+,d3
        rts

; Record's buffer against the expected lines, word for word, from word 0.
check_record:
        movea.l rec_base,a1
        lea     expected_buffer,a0
        move.l  #REC_WORDS-1,d0
        moveq   #0,d5                     ; mismatches
        moveq   #-1,d4                    ; first
        moveq   #-1,d7                    ; last
        moveq   #0,d3
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
        lea     line_buffer,a0
        FSTR    txt_record
        move.l  d5,d0
        bsr     fmt_u32
        tst.l   d5
        beq     .intact
        FSTR    txt_first
        move.l  d4,d0
        bsr     fmt_u32
        FSTR    txt_last
        move.l  d7,d0
        bsr     fmt_u32
        addq.w  #1,fail_count
        FSTR    txt_fail
        bsr     line_done
        bsr     emit_line
        ; the words around the first mismatch
        lea     line_buffer,a0
        FSTR    txt_around
        movea.l rec_base,a2
        move.l  d4,d0
        subq.l  #2,d0
        bpl     .from
        moveq   #0,d0
.from:
        lea     (a2,d0.l*2),a2
        moveq   #8-1,d2
.word:
        move.b  #' ',(a0)+
        moveq   #0,d0
        move.w  (a2)+,d0
        bsr     fmt_hex16
        dbf     d2,.word
        bsr     line_done
        bsr     emit_line
        rts
.intact:
        FSTR    txt_pass_ok
        bsr     line_done
        bsr     emit_line
        rts

; The screen's pixels, from a 32-bit LCG's top bytes (as in ssic2p.s).
fill_chunky:
        lea     chunky_buffer,a0
        move.l  #SCREEN_BYTES-1,d1
        move.l  #$12345678,d0
.pixel:
        mulu.l  #1103515245,d0
        add.l   #12345,d0
        move.l  d0,d2
        rol.l   #8,d2
        move.b  d2,(a0)+
        subq.l  #1,d1
        bpl     .pixel
        rts

; The reference: plane k's word for each 16 pixels, bit 15 the leftmost
; pixel's bit k, straight from the definition.
reference_c2p:
        lea     chunky_buffer,a0
        lea     planar_buffer,a2
        move.l  #SCREEN_GROUPS-1,d6
.group:
        moveq   #0,d7                     ; plane
.plane:
        movea.l a0,a1
        moveq   #0,d0
        moveq   #16-1,d2
.bit:
        move.b  (a1)+,d1
        lsr.b   d7,d1
        lsr.b   #1,d1                     ; bit k into X
        addx.w  d0,d0
        dbf     d2,.bit
        move.w  d0,(a2)+
        addq.w  #1,d7
        cmpi.w  #8,d7
        bcs     .plane
        lea     16(a0),a0
        subq.l  #1,d6
        bpl     .group
        rts

; What record should write: per line PAD zeros, the line's plane words, PAD
; zeros.
expected_record:
        lea     planar_buffer,a1
        lea     expected_buffer,a0
        move.w  #HEIGHT-1,d7
.line:
        moveq   #PAD-1,d0
.left:
        clr.w   (a0)+
        dbf     d0,.left
        move.w  #GROUPS_PER_LINE*8-1,d0
.planes:
        move.w  (a1)+,(a0)+
        dbf     d0,.planes
        moveq   #PAD-1,d0
.right:
        clr.w   (a0)+
        dbf     d0,.right
        dbf     d7,.line
        rts

        data

txt_banner:     dc.b 13,10,'SSI DMA c2p, handshake mode',13,10
                dc.b '===========================',13,10,0
txt_reserve:    dc.b 'DSP reserve ..... ',0
txt_boot:       dc.b 'DSP boot ........ ',0
txt_ping:       dc.b 'DSP ping ........ ',0
txt_lock:       dc.b 'sound lock ...... ',0
txt_preparing:  dc.b 'reference c2p ... ',0
txt_ok:         dc.b 'ok',13,10,0
txt_failed:     dc.b 'FAILED',13,10,0
txt_header:     dc.b 'route: DMA play -> DSP c2p -> DMA record, handshake, 25.175 MHz / 256 / 2; lines 8+160+8 words',0
txt_regs:       dc.b '  regs',0
txt_pass:       dc.b '  pass ',0
txt_quiet:      dc.b ' (quiet): ',0
txt_busy:       dc.b ' (busy): ',0
txt_ms_rate:    dc.b ' ms, chunky B/s ',0
txt_groups:     dc.b '  groups ',0
txt_state:      dc.b '  state ',0
txt_record:     dc.b '    record: mismatched words ',0
txt_first:      dc.b ', first ',0
txt_last:       dc.b ', last ',0
txt_around:     dc.b '    around the first:',0
txt_timeout:    dc.b '    pass timed out  FAIL',0
txt_pass_ok:    dc.b '  PASS',0
txt_fail:       dc.b '  FAIL',0
txt_result_pass: dc.b 'RESULT: PASS',0
txt_result_fail: dc.b 'RESULT: FAIL (',0
txt_result_fail2: dc.b ' checks failed)',0
txt_filename:   dc.b 'HSC2P.TXT',0
        even

        include "hsc2p_boot.i"

        bss
        even
play_base:      ds.l 1
play_end:       ds.l 1
rec_base:       ds.l 1
rec_end:        ds.l 1
pass_number:    ds.l 1
pass_start:     ds.l 1
pass_ticks:     ds.l 1
dsp_state:      ds.l 1
dsp_groups:     ds.l 1

fail_count:     ds.w 1
        cnop    0,16
chunky_buffer:  ds.b SCREEN_BYTES
rec_buffer:     ds.b REC_BYTES
planar_buffer:  ds.b SCREEN_BYTES
expected_buffer: ds.b REC_BYTES

        include "common.s"

        end
