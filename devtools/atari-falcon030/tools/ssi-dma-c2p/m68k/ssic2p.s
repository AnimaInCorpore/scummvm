; SSI DMA c2p: the 68030 half of the DSP chunky-to-planar test.
;
; Streams one 320x200 8-bit chunky screen through the DSP (dsp/ssic2p.asm)
; over the pass-through test's route, DMA playback into the SSI receiver and
; the SSI transmitter into DMA record, four stereo tracks each way, and
; checks what record wrote against the planar screen a 68030 reference c2p
; makes of the same pixels.
;
; The playback buffer is the DSP's marker (four words), the 64,000 chunky
; bytes and 128 words of padding, which clock the last groups out of the
; DSP's 32-word ring. Record starts with playback, so its buffer holds some
; words from before the marker's groups came round, then the planar screen:
; the program finds it by its fifth group and reports the offset, which a
; screen driver would need, and checks that both passes per clock put it at
; the same place.
;
; Per clock and pass it reports what the DSP saw: groups converted, the
; deepest backlog in its ring (over 24 words means it nearly fell a ring
; behind) and receive overruns. The clocks are the pass-through's:
; 25.175 MHz and 32 MHz at prescale 1, 49,170 and 62,500 frames (groups) a
; second.
;
; During a pass the 68030 STOPs until record's end interrupt (MFP GPIP7),
; the only one left unmasked (see super_pass): under Hatari any 68030 loop
; can cost the DSP a received word, and a lost word here shifts every later
; group. The pass line reports whether that interrupt came ("irq 1"); on a
; Falcon without it the pass falls back to polling.
;
; Results are printed and duplicated in SSIC2P.TXT beside the program; the
; last line starts with "RESULT:".

        include "xbios.i"
        include "common.i"

        global  start

; Keep the command words in sync with dsp/ssic2p.asm.
CMD_PING        equ     $010000
CMD_SYNC        equ     $020000
CMD_GROUPS      equ     $030000
CMD_BACKLOG     equ     $040000
CMD_OVERRUNS    equ     $050000
CMD_STATE       equ     $060000
CMD_ARM         equ     $070000
CMD_PROBE       equ     $080000
REPLY_PING      equ     $433250

DSP_ABILITY     equ     3
SLOTS           equ     8

SCREEN_BYTES    equ     320*200
SCREEN_WORDS    equ     SCREEN_BYTES/2
SCREEN_GROUPS   equ     SCREEN_BYTES/16
MARK_WORDS      equ     4
PAD_WORDS       equ     128
PLAY_WORDS      equ     MARK_WORDS+SCREEN_WORDS+PAD_WORDS
PLAY_BYTES      equ     PLAY_WORDS*2
REC_WORDS       equ     SCREEN_WORDS+1024
REC_BYTES       equ     REC_WORDS*2
SENTINEL        equ     $dead
ANCHOR          equ     4*8               ; the planar word the search looks for,
MATCH_WORDS     equ     8                 ; and how many from it must match
SEARCH_WORDS    equ     1024              ; record positions tried
BACKLOG_LIMIT   equ     24                ; words; the ring holds 32
PASSES          equ     2

; After the STOP, record's last word is polled about every 0.4-1.3 ms, up to
; PASS_POLLS times.
PASS_POLLS      equ     3000

        text

start:
        move.l  #results_buffer,results_ptr
        Cconws  txt_banner
        ; the DMA buffers must be in ST-RAM: the program's header flags leave
        ; TOS no choice but to load it, BSS included, there
        move.l  #play_buffer,play_base
        move.l  #rec_buffer,rec_base
        move.l  #play_buffer+PLAY_BYTES,play_end
        move.l  #rec_buffer+REC_BYTES,rec_end

        Cconws  txt_reserve
        Dsp_Reserve #16,#16
        tst.l   d0
        bmi     fail_step
        Cconws  txt_ok
        Cconws  txt_boot
        Dsp_ExecBoot ssic2p_boot_image,#SSIC2P_BOOT_WORDS,#DSP_ABILITY
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
        bsr     fill_play
        bsr     reference_c2p
        Cconws  txt_ok

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
        clr.l   cfg_index
config_loop:
        move.l  (a6)+,d0                  ; clock, or -1 terminator
        bmi     configs_done
        move.w  d0,cfg_clock
        move.l  (a6)+,d0
        move.w  d0,cfg_prescale
        move.l  (a6)+,cfg_name
        bsr     run_config
        addq.l  #1,cfg_index
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
; One crossbar clock: connect, arm the DSP, run PASSES passes.
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

        move.l  #-1,first_offset
        moveq   #1,d7
.pass:
        move.l  d7,pass_number
        bsr     run_pass
        addq.l  #1,d7
        cmpi.l  #PASSES,d7
        bls     .pass
        Buffoper #0
        rts

; One screen through the DSP, then the checks.
run_pass:
        movem.l d7/a6,-(sp)
        movea.l rec_base,a0
        move.l  #REC_WORDS-1,d0
.fill:
        move.w  #SENTINEL,(a0)+
        subq.l  #1,d0
        bpl     .fill
        move.l  #CMD_SYNC,d0
        bsr     dsp_exchange
        Supexec super_pass
        move.l  d0,pass_timeout

        move.l  #CMD_STATE,d0
        bsr     dsp_exchange
        move.l  d0,dsp_state
        move.l  #CMD_GROUPS,d0
        bsr     dsp_exchange
        move.l  d0,dsp_groups
        move.l  #CMD_BACKLOG,d0
        bsr     dsp_exchange
        move.l  d0,dsp_backlog
        move.l  #CMD_OVERRUNS,d0
        bsr     dsp_exchange
        move.l  d0,dsp_overruns

        lea     line_buffer,a0
        FSTR    txt_pass
        move.l  pass_number,d0
        bsr     fmt_u32
        FSTR    txt_groups
        move.l  dsp_groups,d0
        bsr     fmt_u32
        FSTR    txt_backlog
        move.l  dsp_backlog,d0
        bsr     fmt_u32
        FSTR    txt_overruns
        move.l  dsp_overruns,d0
        bsr     fmt_u32
        FSTR    txt_state
        move.l  dsp_state,d0
        bsr     fmt_u32
        FSTR    txt_irq
        moveq   #0,d0
        move.w  record_end_seen,d0
        bsr     fmt_u32
        ; the DSP found the marker, converted the screen, kept up, lost nothing
        cmpi.l  #1,dsp_state
        bne     .dsp_fail
        cmpi.l  #SCREEN_GROUPS,dsp_groups
        bcs     .dsp_fail
        cmpi.l  #BACKLOG_LIMIT,dsp_backlog
        bhi     .dsp_fail
        tst.l   dsp_overruns
        bne     .dsp_fail
        FSTR    txt_pass_ok
        bra     .dsp_lined
.dsp_fail:
        addq.w  #1,fail_count
        FSTR    txt_fail
.dsp_lined:
        bsr     line_done
        bsr     emit_line

        ; the DSP's receive pointer, SSI status, status register and group
        ; pointer as the pass left them
        lea     line_buffer,a0
        FSTR    txt_probe
        moveq   #0,d6
.probe:
        move.l  #CMD_PROBE,d0
        or.l    d6,d0
        bsr     dsp_exchange
        move.b  #' ',(a0)+
        bsr     fmt_hex16
        addq.l  #1,d6
        cmpi.l  #4,d6
        bcs     .probe
        bsr     line_done
        bsr     emit_line

        tst.l   pass_timeout
        beq     .recorded
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
        bsr     dump_record
        bsr     check_planar
        tst.l   d0
        bmi     .show_head
        movem.l (sp)+,d7/a6
        rts
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
        movem.l (sp)+,d7/a6
        rts

; Write the record buffer beside the program as C2P<clock><pass>.BIN, for a
; look at what went wrong: clock 0 is 25.175 MHz, 1 is 32 MHz.
dump_record:
        movem.l d0-d7/a0-a6,-(sp)
        lea     txt_dumpname,a0
        move.l  cfg_index,d0
        addi.b  #'0',d0
        move.b  d0,3(a0)
        move.l  pass_number,d0
        addi.b  #'0',d0
        move.b  d0,4(a0)
        Fcreate txt_dumpname,#0
        tst.l   d0
        bmi     .done
        move.w  d0,d4
        Fwrite  d4,#REC_BYTES,rec_buffer
        Fclose  d4
.done:
        movem.l (sp)+,d0-d7/a0-a6
        rts

; Locate the planar screen in the record buffer by its fifth group and
; compare it word for word. d0.l returns 0, or -1 when it was not found or
; did not match (the caller then shows the record's head).
check_planar:
        movea.l rec_base,a2
        moveq   #0,d6                     ; record position in words
.search:
        lea     planar_buffer+ANCHOR*2,a0
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
        moveq   #-1,d0
        rts

.found:
        sub.l   #ANCHOR,d6                ; the offset of planar word 0
        bmi     .lost
        move.l  d6,pass_offset
        lea     planar_buffer,a0
        lea     (a2,d6.l*2),a1
        move.l  #SCREEN_WORDS-1,d0
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

        lea     line_buffer,a0
        FSTR    txt_offset
        move.l  pass_offset,d0
        bsr     fmt_u32
        FSTR    txt_phase
        move.l  pass_offset,d0
        andi.l  #SLOTS-1,d0
        bsr     fmt_u32
        FSTR    txt_mismatches
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
        bsr     show_mismatches
        moveq   #-1,d0
        rts
.intact:
        ; the same offset in every pass: a screen driver can rely on it
        move.l  first_offset,d0
        bpl     .compare_offset
        move.l  pass_offset,first_offset
        bra     .offset_ok
.compare_offset:
        cmp.l   pass_offset,d0
        beq     .offset_ok
        FSTR    txt_offset_moved
        addq.w  #1,fail_count
        FSTR    txt_fail
        bra     .intact_lined
.offset_ok:
        FSTR    txt_pass_ok
.intact_lined:
        bsr     line_done
        bsr     emit_line
        moveq   #0,d0
        rts

; The first SHOW_MISMATCHES mismatched planar words, one line each: word
; index (plane = index mod 8, group = index / 8), then the recorded words
; from one before to one after it against the reference's.
SHOW_MISMATCHES equ     8
show_mismatches:
        movem.l d2-d7/a2-a4,-(sp)
        lea     planar_buffer,a3
        movea.l rec_base,a4
        move.l  pass_offset,d0
        lea     (a4,d0.l*2),a4
        moveq   #SHOW_MISMATCHES,d7
        moveq   #0,d6
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
        cmpi.l  #SCREEN_WORDS-1,d6
        bcs     .scan
.done:
        movem.l (sp)+,d2-d7/a2-a4
        rts

; The playback buffer: the marker, the screen's pixels from a 32-bit LCG's
; top bytes, and zero padding.
fill_play:
        lea     play_buffer,a0
        move.l  #$a55a5aa5,(a0)+
        move.l  #$c33c3cc3,(a0)+
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
        move.l  #PAD_WORDS-1,d1
.pad:
        clr.w   (a0)+
        subq.l  #1,d1
        bpl     .pad
        rts

; The reference: plane k's word for each 16 pixels, bit 15 the leftmost
; pixel's bit k, straight from the definition.
reference_c2p:
        lea     play_buffer+MARK_WORDS*2,a0
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

; ---------------------------------------------------------------------------
; Supervisor: one pass. Starts both DMAs once and STOPs until record's end
; interrupt: the sound system raises MFP GPIP7 when record ends ($ff8900 bit
; 1), the only MFP source left unmasked, with every other level masked by
; the STOP's IPL 5. Then it polls record's last word until it loses the
; sentinel, which also covers a wake-up too early, should the line's edge be
; the other way round on a real Falcon. d0.l returns 0, or -1 when the word
; has not changed after PASS_POLLS polls.
;
; The STOP matters under Hatari: it runs the DSP only between 68030
; instructions, and a DBF spin there costs 24-42 cycles an instruction, more
; than an SSI slot at times, so two slots could pass with no DSP cycles
; between them and cost the DSP a received word. A stopped 68030 advances in
; small steps. On a Falcon the STOP is just an idle wait.
; ---------------------------------------------------------------------------
MFP_AER         equ     $fffffa03
MFP_IERA        equ     $fffffa07
MFP_IERB        equ     $fffffa09
MFP_IPRA        equ     $fffffa0b
MFP_ISRA        equ     $fffffa0f
MFP_IMRA        equ     $fffffa13
MFP_IMRB        equ     $fffffa15
MFP_GPIP7_VECTOR equ    $13c              ; channel 15 at TOS's vector base $40
SND_INTERRUPTS  equ     $ffff8900         ; bit 1: record end raises GPIP7

super_pass:
        move.w  sr,-(sp)
        ori.w   #$0700,sr
        movea.l rec_end,a3
        subq.l  #2,a3                     ; the last record word
        ; only GPIP7, on its rising edge, may interrupt
        lea     mfp_saved,a0
        move.b  MFP_AER.w,(a0)+
        move.b  MFP_IERA.w,(a0)+
        move.b  MFP_IMRA.w,(a0)+
        move.b  MFP_IMRB.w,(a0)+
        move.b  SND_INTERRUPTS.w,(a0)+
        move.l  MFP_GPIP7_VECTOR.w,vector_saved
        move.l  #record_end_handler,MFP_GPIP7_VECTOR.w
        bset    #7,MFP_AER.w
        move.b  #$7f,MFP_IPRA.w           ; nothing pending on channel 15
        move.b  #$7f,MFP_ISRA.w
        clr.b   MFP_IMRB.w
        move.b  #$80,MFP_IMRA.w
        bset    #7,MFP_IERA.w
        bset    #1,SND_INTERRUPTS.w
        clr.w   record_end_seen
        move.b  #$11,SND_CONTROL.w        ; play and record, once each
        stop    #$2500
        ori.w   #$0700,sr
        lea     mfp_saved,a0
        move.b  (a0)+,MFP_AER.w
        move.b  (a0)+,MFP_IERA.w
        move.b  (a0)+,MFP_IMRA.w
        move.b  (a0)+,MFP_IMRB.w
        move.b  (a0)+,SND_INTERRUPTS.w
        move.l  vector_saved,MFP_GPIP7_VECTOR.w
        move.w  #PASS_POLLS-1,d3
.poll:
        bsr     clear_data_cache
        cmpi.w  #SENTINEL,(a3)
        bne     .recorded
        move.w  #POLL_SPIN-1,d4
.spin:
        dbf     d4,.spin
        dbf     d3,.poll
        move.w  (sp)+,sr
        moveq   #-1,d0
        rts
.recorded:
        bsr     clear_data_cache          ; for the comparison that follows
        move.w  (sp)+,sr
        moveq   #0,d0
        rts

; GPIP7: record has ended. The in-service bit is cleared by writing 0 to it.
record_end_handler:
        move.w  #1,record_end_seen
        move.b  #$7f,MFP_ISRA.w
        rte

        data

txt_banner:     dc.b 13,10,'SSI DMA c2p test',13,10
                dc.b '================',13,10,0
txt_reserve:    dc.b 'DSP reserve ..... ',0
txt_boot:       dc.b 'DSP boot ........ ',0
txt_ping:       dc.b 'DSP ping ........ ',0
txt_lock:       dc.b 'sound lock ...... ',0
txt_preparing:  dc.b 'reference c2p ... ',0
txt_ok:         dc.b 'ok',13,10,0
txt_failed:     dc.b 'FAILED',13,10,0
txt_header:     dc.b 'route: DMA play -> DSP SSI c2p -> DMA record, 4 tracks, one 320x200x8 screen',0
txt_config:     dc.b 'clock ',0
txt_regs:       dc.b '  regs',0
txt_pass:       dc.b '  pass ',0
txt_groups:     dc.b ': groups ',0
txt_backlog:    dc.b '  backlog ',0
txt_overruns:   dc.b ' words  overruns ',0
txt_state:      dc.b '  state ',0
txt_irq:        dc.b '  irq ',0
txt_probe:      dc.b '    dsp r7/ssisr/sr/r0:',0
txt_offset:     dc.b '    planar at word ',0
txt_phase:      dc.b ' (slot phase ',0
txt_mismatches: dc.b '), mismatched words ',0
txt_first:      dc.b ', first ',0
txt_last:       dc.b ', last ',0
txt_offset_moved: dc.b ', offset differs from pass 1',0
txt_not_found:  dc.b '    planar screen not found in the record buffer  FAIL',0
txt_timeout:    dc.b '    pass timed out: play at byte ',0
txt_timeout2:   dc.b ', record at byte ',0
txt_head:       dc.b '    record head:',0
txt_mismatch:   dc.b '      word ',0
txt_mismatch_want: dc.b ': want',0
txt_mismatch_got: dc.b '  got',0
txt_pass_ok:    dc.b '  PASS',0
txt_fail:       dc.b '  FAIL',0
txt_result_pass: dc.b 'RESULT: PASS',0
txt_result_fail: dc.b 'RESULT: FAIL (',0
txt_result_fail2: dc.b ' checks failed)',0
txt_filename:   dc.b 'SSIC2P.TXT',0
txt_dumpname:   dc.b 'C2P00.BIN',0
name_25m:       dc.b '25.175 MHz / 256 / 2 (49,170 groups/s)',0
name_32m:       dc.b '32 MHz / 256 / 2 (62,500 groups/s)',0
        even

; Clock, prescale, name; -1 terminated.
config_table:
        dc.l    CLK_25M,1,name_25m
        dc.l    CLK_32M,1,name_32m
        dc.l    -1

        include "ssic2p_boot.i"

        bss
        even
play_base:      ds.l 1
play_end:       ds.l 1
rec_base:       ds.l 1
rec_end:        ds.l 1
cfg_name:       ds.l 1
cfg_index:      ds.l 1
pass_number:    ds.l 1
pass_timeout:   ds.l 1
pass_offset:    ds.l 1
pass_mismatches: ds.l 1
first_offset:   ds.l 1
dsp_state:      ds.l 1
dsp_groups:     ds.l 1
dsp_backlog:    ds.l 1
dsp_overruns:   ds.l 1
dma_pointers:   ds.l 4
vector_saved:   ds.l 1
cfg_clock:      ds.w 1
cfg_prescale:   ds.w 1
fail_count:     ds.w 1
record_end_seen: ds.w 1
mfp_saved:      ds.b 6
        cnop    0,16
play_buffer:    ds.b PLAY_BYTES
rec_buffer:     ds.b REC_BYTES
planar_buffer:  ds.b SCREEN_BYTES

        include "common.s"

        end
