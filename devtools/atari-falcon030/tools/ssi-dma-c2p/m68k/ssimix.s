; SSI slot sharing: the 68030 half of the test whether the DAC can play one
; slot pair of the four-track crossbar frame while the other slots carry
; other data, the way a DSP c2p could keep sound going.
;
; Five cases at 25.175 MHz / prescale 1 (49,170 frames a second, the codec's
; own maximum; it cannot use the 32 MHz clock):
;
;   1  DSP transmit to record and DAC, tone in pair 0, DAC on track 0
;   2  DSP transmit to record and DAC, tone in pair 1, DAC on track 1
;   3  DSP transmit to record and DAC, tone in pair 0, DAC on track 1: the
;      control, which should play no tone
;   4  DMA playback to the DSP and the DAC, the tone in pair 0 of the play
;      buffer, DAC on track 0; the DSP sends tags only
;   5  DSP transmit, tone in pair 3, DAC on track 3, record three tracks:
;      the layout a DSP c2p with sound would use, planar words in slots 0-5
;      for record and the sound in slots 6-7 for the DAC alone. Hardware
;      only: Hatari records every slot whatever the track count, and cannot
;      select DAC track 3. Its verdict is reported apart and left out of the
;      result.
;
; The tone is 1,024 Hz on the left and 2,049 Hz on the right (a 48-entry
; sine, one and two steps a frame); every other slot carries a tag, $s0a5
; for slot s. Each case lasts one 2.2 s playback, and the cases are kept
; apart by SILENCE_TICKS of zeros. The program cannot hear the DAC:
; mix-gate.py finds the cases in Hatari's sound recording by those silences
; and measures the tones there. What the program checks itself is a record
; snapshot of each case: which slot every word landed in, told by the tags,
; and whether the tone slots hold the tone.
;
; Results are printed and duplicated in SSIMIX.TXT; the last line starts
; with "RESULT:".

        include "xbios.i"
        include "common.i"

        global  start

; Keep the command words in sync with dsp/ssimix.asm.
CMD_PING        equ     $010000
CMD_ARM         equ     $020000
CMD_PAIR        equ     $030000
CMD_PHASE       equ     $040000
CMD_SILENT      equ     $050000
CMD_TABLE       equ     $060000
CMD_FRAMES      equ     $070000
REPLY_PING      equ     $4d4958

DSP_ABILITY     equ     3
SLOTS           equ     8
TABLE_LENGTH    equ     48
PAIR_NONE       equ     4
SOUND_LTATTEN   equ     0
SOUND_RTATTEN   equ     1

; Each case plays one single-shot buffer, 2.2 s of whole frames, and waits
; for it to end rather than stopping it: Hatari counts playback's slot
; position from power-on and never resets it when playback starts, so a
; playback aborted mid-frame would put the next case's pair 0 in another
; pair's slots. On a Falcon this is just a longer buffer.
PLAY_PERIODS    equ     2253              ; of 48 frames: 2.2 s
PLAY_FRAMES     equ     TABLE_LENGTH*PLAY_PERIODS
PLAY_BYTES      equ     PLAY_FRAMES*SLOTS*2
REC_FRAMES      equ     1024
REC_WORDS       equ     REC_FRAMES*SLOTS
REC_BYTES       equ     REC_WORDS*2
SENTINEL        equ     $dead
; A slot passes with its expected words in 99 % of the snapshot's frames
; (map_slots): a missed slot under Hatari (see run_case) shifts the rest of
; its frame.
SILENCE_TICKS   equ     100               ; 0.5 s of zeros around them

        text

start:
        move.l  #results_buffer,results_ptr
        Cconws  txt_banner
        move.l  #play_buffer,play_base
        move.l  #play_buffer+PLAY_BYTES,play_end
        move.l  #rec_buffer,rec_base
        move.l  #rec_buffer+REC_BYTES,rec_end

        Cconws  txt_reserve
        Dsp_Reserve #16,#16
        tst.l   d0
        bmi     fail_step
        Cconws  txt_ok
        Cconws  txt_boot
        Dsp_ExecBoot ssimix_boot_image,#SSIMIX_BOOT_WORDS,#DSP_ABILITY
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

        ; the sine table into the DSP, and the play buffer: tone in pair 0
        lea     sine_table,a2
        moveq   #TABLE_LENGTH-1,d7
.upload:
        moveq   #0,d0
        move.w  (a2)+,d0
        or.l    #CMD_TABLE,d0
        bsr     dsp_exchange
        dbf     d7,.upload
        bsr     fill_play

        lea     line_buffer,a0
        FSTR    txt_header
        bsr     line_done
        bsr     emit_line

        ; pin the matrix at the codec's maximum rate, unmuted
        Buffoper #0
        Sndstatus #SNDSTAT_RESET
        Soundcmd #SOUND_LTATTEN,#0
        Soundcmd #SOUND_RTATTEN,#0
        Soundcmd #SOUND_ADDERIN,#SOUND_MATRIXIN
        Setmode #SOUND_STEREO16
        Settracks #TRACKS4,#TRACKS4
        Setmontracks #0
        Dsptristate #1,#1
        Setbuffer #BUF_PLAY,play_base,play_end
        Setbuffer #BUF_RECORD,rec_base,rec_end

        bsr     silence
        lea     case_table,a6
case_loop:
        move.l  (a6)+,d0                  ; route, or -1 terminator
        bmi     cases_done
        move.l  d0,case_route
        move.l  (a6)+,case_pair
        move.l  (a6)+,case_monitor
        move.l  (a6)+,d0                  ; record tracks, counted from 0
        move.l  d0,case_rec_tracks
        addq.l  #1,d0
        add.l   d0,d0
        move.l  d0,case_rec_slots
        move.l  (a6)+,case_hardware
        move.l  (a6)+,case_name
        bsr     run_case
        bsr     silence
        bra     case_loop
cases_done:

        lea     line_buffer,a0
        FSTR    txt_hardware_summary
        moveq   #0,d0
        move.w  hardware_fail_count,d0
        bsr     fmt_u32
        FSTR    txt_hardware_summary2
        bsr     line_done
        bsr     emit_line
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

        Buffoper #0
        Dsptristate #0,#0
        Settracks #0,#0
        Setmontracks #0
        Devconnect #SRC_DMAPLAY,#DST_DAC,#CLK_25M,#1,#NO_SHAKE
        Unlocksnd
exit_dsp:
        Dsp_Unlock
exit_key:
        Pterm0

fail_step:
        Cconws  txt_failed
        bra     exit_key
fail_dsp:
        Cconws  txt_failed
        bra     exit_dsp

; ---------------------------------------------------------------------------
; Routes. 0: DSP transmit to record and DAC. 1: DMA playback to the DSP and
; the DAC, DSP transmit to record. Playback is idle whenever this runs.
; ---------------------------------------------------------------------------
connect:
        Buffoper #0
        tst.l   case_route
        bne     .from_play
        Devconnect #SRC_DMAPLAY,#DST_DSPRECV,#CLK_25M,#1,#NO_SHAKE
        Devconnect #SRC_DSPXMIT,#DST_DMAREC+DST_DAC,#CLK_25M,#1,#NO_SHAKE
        bra     .armed
.from_play:
        Devconnect #SRC_DMAPLAY,#DST_DSPRECV+DST_DAC,#CLK_25M,#1,#NO_SHAKE
        Devconnect #SRC_DSPXMIT,#DST_DMAREC,#CLK_25M,#1,#NO_SHAKE
.armed:
        move.l  #CMD_ARM+SLOTS-1,d0
        bsr     dsp_exchange
        rts

; SILENCE_TICKS of zeros from the DSP, on the DSP route.
silence:
        clr.l   case_route
        bsr     connect
        move.l  #CMD_SILENT+1,d0
        bsr     dsp_exchange
        move.l  #CMD_PAIR+PAIR_NONE,d0
        bsr     dsp_exchange
        Setmontracks #0
        Settracks #TRACKS4,#TRACKS4
        move.l  #SILENCE_TICKS,d0
        bsr     wait_ticks
        rts

; One case: route, tone pair, DAC track; a record snapshot, then the rest of
; the playback, and the snapshot's analysis once the case is over.
;
; While a case plays the 68030 only sleeps (see sleep_ticks): Hatari runs
; the DSP only between 68030 instructions, and a busy wait's long ones there
; (TOS calls, bus-contended ST-RAM) deliver two slots with no DSP cycles
; between them, so the DSP misses a slot, sends the one before twice, and
; the frame's words shift until the next frame sync.
run_case:
        movea.l rec_base,a0
        move.l  #REC_WORDS-1,d0
.fill:
        move.w  #SENTINEL,(a0)+
        subq.l  #1,d0
        bpl     .fill
        bsr     connect
        Settracks #TRACKS4,case_rec_tracks+2
        move.l  #CMD_SILENT+0,d0
        bsr     dsp_exchange
        move.l  case_pair,d0
        or.l    #CMD_PAIR,d0
        bsr     dsp_exchange
        move.w  case_monitor+2,d0
        Setmontracks d0
        bsr     get_ticks
        move.l  d0,case_start

        lea     line_buffer,a0
        FSTR    txt_case
        movea.l case_name,a1
        bsr     fmt_string
        bsr     line_done
        bsr     emit_line
        lea     line_buffer,a0
        FSTR    txt_started
        move.l  case_start,d0
        bsr     fmt_u32
        bsr     line_done
        bsr     emit_line

        ; a record snapshot of what the DSP sends once the route has
        ; settled, then the case proper: its playback's 2.2 s. Record comes
        ; first: under Hatari the last DMA to start or stop sets the GPIP7
        ; line, so a record started while playback runs would raise it early
        ; and playback's end would not wake the sleep.
        move.l  #20,d0
        bsr     wait_ticks
        Buffoper #OP_RECORD
        move.b  #$10,sleep_mask           ; until record ends, 21 ms
        Supexec super_sleep_dma
        Supexec route_play_end            ; before playback drives the line
        Buffoper #OP_PLAY                 ; once: see PLAY_PERIODS
        move.b  #$01,sleep_mask           ; until playback ends
        Supexec super_sleep_sound
        Supexec unroute_play_end

        Supexec clear_data_cache_super
        bsr     dump_record
        bsr     map_slots
        rts

clear_data_cache_super:
        bsr     clear_data_cache
        rts

; Sleep in STOP, waking on each interrupt, until the TOS tick reaches
; sleep_target. Supervisor only.
super_sleep_ticks:
        move.w  sr,-(sp)
.sleep:
        move.l  HZ200.w,d0
        cmp.l   sleep_target,d0
        bcc     .done
        stop    #$2300
        bra     .sleep
.done:
        move.w  (sp)+,sr
        rts

; Sleep likewise until the sound control bits in sleep_mask are clear.
super_sleep_dma:
        move.w  sr,-(sp)
.sleep:
        move.b  SND_CONTROL.w,d0
        and.b   sleep_mask,d0
        beq     .done
        stop    #$2300
        bra     .sleep
.done:
        move.w  (sp)+,sr
        rts

; Route playback's end to MFP GPIP7 ($ff8900 bit 0), and back. The route
; must be in place before playback starts: starting is what drives the line
; low, and its end raises it.
SND_INTERRUPTS  equ     $ffff8900
route_play_end:
        move.b  SND_INTERRUPTS.w,irq_saved
        move.b  #$01,SND_INTERRUPTS.w
        rts
unroute_play_end:
        move.b  irq_saved,SND_INTERRUPTS.w
        rts

; Sleep until the sound control bits in sleep_mask are clear, woken only by
; playback's end (see route_play_end): GPIP7 is the one MFP source left
; unmasked, and STOP's IPL 5 masks the VBL. Even the TOS timer tick stops
; meanwhile: its handler would wake the 68030 into long instructions 200
; times a second (see run_case). Supervisor only.
MFP_AER         equ     $fffffa03
MFP_IERA        equ     $fffffa07
MFP_IPRA        equ     $fffffa0b
MFP_ISRA        equ     $fffffa0f
MFP_IMRA        equ     $fffffa13
MFP_IMRB        equ     $fffffa15
MFP_GPIP7_VECTOR equ    $13c

super_sleep_sound:
        move.w  sr,-(sp)
        ori.w   #$0700,sr
        lea     mfp_saved,a0
        move.b  MFP_AER.w,(a0)+
        move.b  MFP_IERA.w,(a0)+
        move.b  MFP_IMRA.w,(a0)+
        move.b  MFP_IMRB.w,(a0)+
        move.l  MFP_GPIP7_VECTOR.w,vector_saved
        move.l  #sound_end_handler,MFP_GPIP7_VECTOR.w
        bset    #7,MFP_AER.w              ; the line rises when the DMA ends
        move.b  #$7f,MFP_IPRA.w
        move.b  #$7f,MFP_ISRA.w
        clr.b   MFP_IMRB.w
        move.b  #$80,MFP_IMRA.w
        bset    #7,MFP_IERA.w
.sleep:
        move.b  SND_CONTROL.w,d0
        and.b   sleep_mask,d0
        beq     .done
        stop    #$2500
        ori.w   #$0700,sr
        bra     .sleep
.done:
        lea     mfp_saved,a0
        move.b  (a0)+,MFP_AER.w
        move.b  (a0)+,MFP_IERA.w
        move.b  (a0)+,MFP_IMRA.w
        move.b  (a0)+,MFP_IMRB.w
        move.l  vector_saved,MFP_GPIP7_VECTOR.w
        move.w  (sp)+,sr
        rts

sound_end_handler:
        move.b  #$7f,MFP_ISRA.w
        rte

; The record snapshot beside the program as MIX<case>.BIN.
dump_record:
        movem.l d0-d7/a0-a6,-(sp)
        movea.l case_name,a0
        move.b  (a0),txt_dumpname+3       ; the case's digit
        Fcreate txt_dumpname,#0
        tst.l   d0
        bmi     .done
        move.w  d0,d4
        Fwrite  d4,#REC_BYTES,rec_buffer
        Fclose  d4
.done:
        movem.l (sp)+,d0-d7/a0-a6
        rts

; Wait d0.l TOS ticks, asleep.
wait_ticks:
        move.l  d0,-(sp)
        bsr     get_ticks
        add.l   (sp)+,d0
        move.l  d0,sleep_target
        Supexec super_sleep_ticks
        rts

; Tell each recorded word's slot from the tags, then count per slot the
; words that are its tag and the words that are tone. Reports one line and
; checks the case: the tone pair holds tone only and every other slot its
; tag only. Record takes case_rec_slots slots a frame, 8 or 6, the first
; ones: with three record tracks the tone pair 3 is not recorded at all.
map_slots:
        movem.l d2-d7/a2-a4,-(sp)
        move.l  case_rec_slots,d3         ; slots recorded a frame
        move.l  #REC_WORDS,d0             ; frames' worth in the snapshot,
        divu.l  d3,d0                     ; 99 % of them to pass a slot
        mulu.l  #99,d0
        divu.l  #100,d0
        move.l  d0,slot_quorum
        lea     slot_tags,a3
        lea     slot_tones,a4
        lea     slot_votes,a0
        moveq   #SLOTS-1,d0
.clear:
        clr.l   (a3)+
        clr.l   (a4)+
        clr.l   (a0)+
        dbf     d0,.clear
        ; every tag votes for the slot of record word 0: its slot minus its
        ; position, mod d3; a glitch in a frame outvoted by the rest
        lea     slot_votes,a3
        movea.l rec_base,a2
        moveq   #0,d6                     ; index
        moveq   #0,d7                     ; index mod d3
.vote:
        move.w  (a2,d6.l*2),d0
        move.w  d0,d1
        andi.w  #$0fff,d1
        cmpi.w  #$00a5,d1
        bne     .next_vote
        cmpi.w  #$7fff,d0
        bhi     .next_vote
        lsr.w   #8,d0
        lsr.w   #4,d0                     ; the tag's slot
        moveq   #0,d1
        move.w  d0,d1
        cmp.l   d3,d1                     ; not a recorded slot: no vote
        bcc     .next_vote
        sub.l   d7,d1
        bpl     .voted
        add.l   d3,d1
.voted:
        addq.l  #1,(a3,d1.l*4)
.next_vote:
        addq.l  #1,d7
        cmp.l   d3,d7
        bcs     .same_frame
        moveq   #0,d7
.same_frame:
        addq.l  #1,d6
        cmpi.l  #REC_WORDS,d6
        bcs     .vote
        moveq   #0,d5                     ; the winner
        moveq   #1,d1
.ballot:
        move.l  (a3,d1.l*4),d0
        cmp.l   (a3,d5.l*4),d0
        bls     .lower
        move.l  d1,d5
.lower:
        addq.l  #1,d1
        cmp.l   d3,d1
        bcs     .ballot
        tst.l   (a3,d5.l*4)
        bne     .mapped
        bsr     count_failure
        lea     line_buffer,a0
        FSTR    txt_no_tags
        bsr     line_done
        bsr     emit_line
        bra     .done
.mapped:
        move.l  d5,slot_of_first
        moveq   #0,d6                     ; index
        move.l  d5,d4                     ; this word's slot
.classify:
        move.w  (a2,d6.l*2),d0
        move.w  d4,d1
        lsl.w   #8,d1
        lsl.w   #4,d1
        ori.w   #$00a5,d1
        cmp.w   d1,d0
        bne     .not_tag
        lea     slot_tags,a3
        addq.l  #1,(a3,d4.l*4)
        bra     .classified
.not_tag:
        lea     sine_table,a0
        moveq   #TABLE_LENGTH-1,d2
.sine:
        cmp.w   (a0)+,d0
        dbeq    d2,.sine
        bne     .classified
        lea     slot_tones,a4
        addq.l  #1,(a4,d4.l*4)
.classified:
        addq.l  #1,d4
        cmp.l   d3,d4
        bcs     .same_slot_frame
        moveq   #0,d4
.same_slot_frame:
        addq.l  #1,d6
        cmpi.l  #REC_WORDS,d6
        bcs     .classify

        ; one line: slot: tags/tones, then the verdict
        lea     line_buffer,a0
        FSTR    txt_slots
        lea     slot_tags,a3
        lea     slot_tones,a4
        moveq   #0,d4
        moveq   #0,d7                     ; failures in this case
.report:
        move.b  #' ',(a0)+
        move.l  d4,d0
        bsr     fmt_u32
        move.b  #':',(a0)+
        move.l  (a3,d4.l*4),d0
        bsr     fmt_u32
        move.b  #'/',(a0)+
        move.l  (a4,d4.l*4),d0
        bsr     fmt_u32
        ; is this slot in the case's tone pair?
        move.l  d4,d0
        lsr.l   #1,d0
        cmp.l   case_pair,d0
        bne     .expect_tag
        tst.l   case_route                ; the DSP sends the tone only on
        bne     .expect_tag               ; route 0
        move.l  slot_quorum,d0
        cmp.l   (a4,d4.l*4),d0
        bls     .slot_ok
        addq.l  #1,d7
        bra     .slot_ok
.expect_tag:
        move.l  slot_quorum,d0
        cmp.l   (a3,d4.l*4),d0
        bls     .slot_ok
        addq.l  #1,d7
.slot_ok:
        addq.l  #1,d4
        cmp.l   d3,d4
        bcs     .report
        tst.l   d7
        bne     .case_fail
        FSTR    txt_pass
        bra     .lined
.case_fail:
        bsr     count_failure
        FSTR    txt_fail
.lined:
        tst.l   case_hardware
        beq     .judged
        FSTR    txt_hardware_only
.judged:
        bsr     line_done
        bsr     emit_line
.done:
        movem.l (sp)+,d2-d7/a2-a4
        rts

; A failed check, counted apart for a hardware-only case.
count_failure:
        tst.l   case_hardware
        bne     .hardware
        addq.w  #1,fail_count
        rts
.hardware:
        addq.w  #1,hardware_fail_count
        rts

; The play buffer: both tones in pair 0, tags elsewhere, PLAY_PERIODS times
; one period of 48 frames.
fill_play:
        lea     play_buffer,a0
        lea     sine_table,a1
        move.l  #PLAY_FRAMES-1,d4
        moveq   #0,d3                     ; frame within the period
.frame:
        move.w  (a1,d3.w*2),(a0)+         ; left: one step a frame
        move.w  d3,d0
        add.w   d0,d0
        cmpi.w  #TABLE_LENGTH,d0
        bcs     .right
        subi.w  #TABLE_LENGTH,d0
.right:
        move.w  (a1,d0.w*2),(a0)+         ; right: two
        move.w  #$20a5,d1
        moveq   #SLOTS-3,d2
.tag:
        move.w  d1,(a0)+
        addi.w  #$1000,d1
        dbf     d2,.tag
        addq.w  #1,d3
        cmpi.w  #TABLE_LENGTH,d3
        bcs     .next
        moveq   #0,d3
.next:
        subq.l  #1,d4
        bpl     .frame
        rts

        data

txt_banner:     dc.b 13,10,'SSI slot sharing test',13,10
                dc.b '=====================',13,10,0
txt_reserve:    dc.b 'DSP reserve ..... ',0
txt_boot:       dc.b 'DSP boot ........ ',0
txt_ping:       dc.b 'DSP ping ........ ',0
txt_lock:       dc.b 'sound lock ...... ',0
txt_ok:         dc.b 'ok',13,10,0
txt_failed:     dc.b 'FAILED',13,10,0
txt_header:     dc.b 'tone 1024 Hz left, 2049 Hz right in one slot pair; tags $s0a5 elsewhere; 49,170 frames/s',0
txt_case:       dc.b 'case ',0
txt_started:    dc.b '  started at tick ',0
txt_slots:      dc.b '  record slot:tags/tones',0
txt_no_tags:    dc.b '  record holds no tags  FAIL',0
txt_pass:       dc.b '  PASS',0
txt_fail:       dc.b '  FAIL',0
txt_result_pass: dc.b 'RESULT: PASS',0
txt_result_fail: dc.b 'RESULT: FAIL (',0
txt_result_fail2: dc.b ' checks failed)',0
txt_filename:   dc.b 'SSIMIX.TXT',0
txt_dumpname:   dc.b 'MIX0.BIN',0
name_dsp0:      dc.b '1 DSP transmit, tone in pair 0, DAC track 0',0
name_dsp1:      dc.b '2 DSP transmit, tone in pair 1, DAC track 1',0
name_control:   dc.b '3 DSP transmit, tone in pair 0, DAC track 1 (control)',0
name_play0:     dc.b '4 DMA playback, tone in pair 0, DAC track 0',0
name_c2p_layout: dc.b '5 DSP transmit, tone in pair 3, DAC track 3, record 3 tracks (hardware only)',0
txt_hardware_only: dc.b '  (hardware only)',0
txt_hardware_summary: dc.b 'hardware-only checks failed: ',0
txt_hardware_summary2: dc.b ' (not in RESULT: Hatari cannot model them)',0
        even

; Route, tone pair, DAC track, name; -1 terminated.
case_table:
        dc.l    0,0,0,TRACKS4,0,name_dsp0
        dc.l    0,1,1,TRACKS4,0,name_dsp1
        dc.l    0,0,1,TRACKS4,0,name_control
        dc.l    1,PAIR_NONE,0,TRACKS4,0,name_play0
        dc.l    0,3,3,TRACKS4-1,1,name_c2p_layout
        dc.l    -1

; 48 entries of 12,288 * sin(2 pi i / 48).
sine_table:
        dc.w    $0000,$0644,$0c6c,$125e,$1800,$1d38,$21f1,$2615
        dc.w    $2992,$2c59,$2e5d,$2f97,$3000,$2f97,$2e5d,$2c59
        dc.w    $2992,$2615,$21f1,$1d38,$1800,$125e,$0c6c,$0644
        dc.w    $0000,$f9bc,$f394,$eda2,$e800,$e2c8,$de0f,$d9eb
        dc.w    $d66e,$d3a7,$d1a3,$d069,$d000,$d069,$d1a3,$d3a7
        dc.w    $d66e,$d9eb,$de0f,$e2c8,$e800,$eda2,$f394,$f9bc

        include "ssimix_boot.i"

        bss
        even
play_base:      ds.l 1
play_end:       ds.l 1
rec_base:       ds.l 1
rec_end:        ds.l 1
case_route:     ds.l 1
case_pair:      ds.l 1
case_monitor:   ds.l 1
case_rec_tracks: ds.l 1
case_rec_slots: ds.l 1
case_hardware:  ds.l 1
case_name:      ds.l 1
slot_quorum:    ds.l 1
case_start:     ds.l 1
slot_of_first:  ds.l 1
sleep_target:   ds.l 1
vector_saved:   ds.l 1
sleep_mask:     ds.b 1
irq_saved:      ds.b 1
mfp_saved:      ds.b 6
        even
slot_votes:     ds.l SLOTS
slot_tags:      ds.l SLOTS
slot_tones:     ds.l SLOTS
fail_count:     ds.w 1
hardware_fail_count: ds.w 1
        cnop    0,16
play_buffer:    ds.b PLAY_BYTES
rec_buffer:     ds.b REC_BYTES

        include "common.s"

        end
