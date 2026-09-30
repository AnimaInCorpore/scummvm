; FOA.TOS - the Fate of Atlantis AdLib score, played by the Falcon030 DSP.
;
; A standalone Falcon program. It embeds:
;   - the DSP kernel (foa-opl3/dsp/oplrt.asm, the very image the ScummVM
;     build boots), through the same two-stage loader;
;   - the kernel's tables and operator records (foa-tables.bin);
;   - the score (foa-score.bin): the register stream ScummVM's AdLib driver
;     writes while the game plays its opening, decoded by the practical
;     kernel's register decoder into the sample-stamped parameter events of
;     768-frame periods, exactly what AtariDspOPL hands the DSP in the game.
;
; The transport is the game's: the DSP transmits stereo to the DAC at
; 49.170 kHz from its own SSI ring, and each period reaches it as a READY
; handshake and a paced host-port blast of the events plus a PCM flag (no
; speech or effects here, so always silent). The kernel answers READY only
; when it has a half ring free, so the DSP paces this program at real time.
; Every acknowledgement carries the kernel's period and late counters.
;
; Any key quits. When the score ends the DSP is rebooted and it plays again.
;
; Both binary files are big-endian 32-bit words:
;   foa-tables.bin  'OPLR', upload block count, then per block: space
;                   (0 X, 1 Y), address, word count, the words
;   foa-score.bin   'OPLP', period count, then per period: event count, two
;                   words per event, and a PCM flag (0: silent)

        include "xbios.i"

        global  start

CMD_PING        equ     $010000
CMD_WRITE_X     equ     $020000
CMD_WRITE_Y     equ     $030000
CMD_STREAM_START equ    $090000
CMD_REFILL      equ     $0A0000
CMD_STREAM_STOP equ     $0B0000
REPLY_PING      equ     $4F5052
REPLY_READY     equ     $524459

DSP_ABILITY     equ     3
TABLES_MAGIC    equ     $4F504C52
SCORE_MAGIC     equ     $4F504C50

DSP_HOST_ISR    equ     $ffffa202
DSP_HOST_DATA   equ     $ffffa204

; A period is 768 frames of 49,170 Hz: 15.619 ms.
PERIOD_US       equ     15619

SOUND_STEREO16  equ     1
SOUND_DSP_XMIT  equ     1
SOUND_DAC       equ     8
SOUND_CLK25M    equ     0
SOUND_CLK50K    equ     1
SOUND_NO_SHAKE  equ     1
SOUND_ADDERIN   equ     4
SOUND_MATRIXIN  equ     2
SOUND_DMA_STOP  equ     0
SNDSTAT_RESET   equ     1

        macro   Dsp_Lock
        move.w  #104,-(sp)
        trap    #14
        addq.l  #2,sp
        endm

        text

start:
        Cconws  txt_banner

        Dsp_Lock
        tst.l   d0
        bne     fail_lock
        Dsp_Reserve #16,#16
        tst.l   d0
        bmi     fail_reserve
        Locksnd
        cmpi.l  #1,d0
        bne     fail_sound

        cmp.l   #TABLES_MAGIC,tables_data
        bne     fail_magic
        ; the score's header: the period count, and its length in seconds
        lea     score_data,a0
        cmp.l   #SCORE_MAGIC,(a0)
        bne     fail_magic
        move.l  4(a0),d0
        move.l  d0,total_periods
        bsr     periods_to_seconds
        move.l  d0,total_seconds
        clr.b   quit_flag

play_score:
        bsr     boot_dsp
        tst.l   d0
        bne     fail_boot
        Supexec upload_tables_super
        bsr     route_codec
        Cconws  txt_playing
        move.l  #CMD_STREAM_START,d0
        bsr     dsp_exchange

        lea     score_data,a3
        move.l  (a3)+,d0                ; magic
        move.l  (a3)+,period_count      ; count
        clr.l   periods_done
        clr.l   last_ack
        move.l  #-1,shown_seconds

period_loop:
        tst.l   period_count
        beq     score_done
        ; the payload words follow the count in the file: count, events, flag
        move.l  (a3),d0                 ; event count
        add.l   d0,d0
        addq.l  #2,d0                   ; plus the count word and the flag word
        move.l  d0,payload_words
        move.l  a3,payload_base
        lsl.l   #2,d0
        add.l   d0,a3
        Supexec submit_period_super
        subq.l  #1,period_count
        addq.l  #1,periods_done
        bsr     show_progress
        Cconis
        tst.w   d0
        beq     period_loop
        Cconin
        st      quit_flag
score_done:
        ; let the last rendered periods play out before stopping
        move.w  #12,d3
drain_loop:
        Vsync
        dbra    d3,drain_loop
        move.l  #CMD_STREAM_STOP,d0
        bsr     dsp_exchange
        tst.b   quit_flag
        bne     finish
        ; a moment of silence between rounds
        Cconws  txt_again
        move.w  #50,d3
gap_loop:
        Vsync
        dbra    d3,gap_loop
        bra     play_score

finish:
        Cconws  txt_bye
        Unlocksnd
        Dsp_Unlock
        Pterm0

; ---- the DSP

; Boot the kernel: the XBIOS loader takes at most 512 internal words, which
; then stream the sparse program and acknowledge once. Returns d0 = 0 if
; the kernel answers its ping.
boot_dsp:
        Dsp_ExecBoot dsp_bootstrap_image,#DSP_BOOT_WORDS,#DSP_ABILITY
        clr.l   dsp_stage2_reply
        Dsp_BlkUnpacked dsp_program_image,#DSP_STAGE2_TRANSFER_WORDS,dsp_stage2_reply,#1
        move.l  dsp_stage2_reply,d0
        cmp.l   #DSP_STAGE2_REPLY_OK,d0
        bne     .fail
        move.l  #CMD_PING,d0
        bsr     dsp_exchange
        cmp.l   #REPLY_PING,d0
        bne     .fail
        moveq   #0,d0
        rts
.fail:
        moveq   #1,d0
        rts

; The kernel's tables and operator records, every word acknowledged. About
; 15,000 words, so over the raw host port instead of an XBIOS call apiece.
upload_tables_super:
        movem.l d3-d7/a3,-(sp)
        bsr     upload_tables
        movem.l (sp)+,d3-d7/a3
        rts

upload_tables:
        lea     tables_data,a3
        cmp.l   #TABLES_MAGIC,(a3)+
        bne     .done
        move.l  (a3)+,d7
        beq     .done
.block:
        move.l  (a3)+,d6
        move.l  (a3)+,d5
        move.l  (a3)+,d4
        bsr     upload_words
        subq.l  #1,d7
        bne     .block
.done:
        rts

; The codec: the DSP transmits to the DAC at 49.170 kHz. The matrix state
; outlives whatever set it, so every route is pinned.
route_codec:
        Buffoper #SOUND_DMA_STOP
        Sndstatus #SNDSTAT_RESET
        Soundcmd #SOUND_ADDERIN,#SOUND_MATRIXIN
        Setmode #SOUND_STEREO16
        Settracks #0,#0
        Setmontracks #0
        Dsptristate #1,#0
        Devconnect #SOUND_DSP_XMIT,#SOUND_DAC,#SOUND_CLK25M,#SOUND_CLK50K,#SOUND_NO_SHAKE
        rts

; One period: announce, wait for READY, blast the payload paced on TXDE,
; wait for the acknowledgement. The host port is supervisor-only. READY can
; take up to a period, so the wait runs with the interrupts as they were;
; only the blast and the acknowledgement run with them masked.
submit_period_super:
        movem.l d0-d3/a0,-(sp)
        move.w  sr,-(sp)
.tx_wait:
        btst    #1,DSP_HOST_ISR
        beq.s   .tx_wait
        move.l  #CMD_REFILL,DSP_HOST_DATA
        bsr     recv_word
        cmp.l   #REPLY_READY,d0
        bne     .bad_reply
        ori.w   #$0700,sr
        movea.l payload_base,a0
        move.l  payload_words,d3
.word_loop:
        btst    #1,DSP_HOST_ISR
        beq.s   .word_loop
        move.l  (a0)+,DSP_HOST_DATA
        subq.l  #1,d3
        bne.s   .word_loop
        bsr     recv_word               ; the OK acknowledgement
        move.l  d0,last_ack
        bra.s   .done
.bad_reply:
        addq.l  #1,protocol_errors
.done:
        move.w  (sp)+,sr
        movem.l (sp)+,d0-d3/a0
        rts

; The low data byte must be read last: reading it clears RXDF.
recv_word:
        btst    #0,DSP_HOST_ISR
        beq.s   recv_word
        moveq   #0,d0
        move.b  DSP_HOST_DATA+1,d0
        lsl.l   #8,d0
        move.b  DSP_HOST_DATA+2,d0
        lsl.l   #8,d0
        move.b  DSP_HOST_DATA+3,d0
        rts

; Upload d4.l words from (a3) to DSP space d6 (0 X, 1 Y) at address d5,
; every word acknowledged by the kernel. Supervisor mode.
upload_words:
        move.l  #CMD_WRITE_X,d0
        tst.l   d6
        beq     .space_ready
        move.l  #CMD_WRITE_Y,d0
.space_ready:
        bsr     exchange_raw
        move.l  d5,d0
        bsr     exchange_raw
        move.l  d4,d0
        bsr     exchange_raw
        move.l  d4,d3
        beq     .done
.word_loop:
        move.l  (a3)+,d0
        bsr     exchange_raw
        subq.l  #1,d3
        bne     .word_loop
.done:
        rts

; One word to the kernel and its answer, over the host port. Supervisor mode.
exchange_raw:
.tx_wait:
        btst    #1,DSP_HOST_ISR
        beq.s   .tx_wait
        move.l  d0,DSP_HOST_DATA
        bra     recv_word

dsp_exchange:
        movem.l d1-d7/a0-a6,-(sp)
        move.l  d0,dsp_tx_word
        clr.l   dsp_rx_word
        Dsp_BlkUnpacked dsp_tx_word,#1,dsp_rx_word,#1
        move.l  dsp_rx_word,d0
        movem.l (sp)+,d1-d7/a0-a6
        rts

; ---- the display

; d0 = periods, returns d0 = seconds
periods_to_seconds:
        mulu.l  #PERIOD_US,d0
        divu.l  #1000000,d0
        rts

; "\r  mm:ss / mm:ss   late nnnn   any key quits  ", once per second played
show_progress:
        move.l  periods_done,d0
        bsr     periods_to_seconds
        cmp.l   shown_seconds,d0
        beq     .same
        move.l  d0,shown_seconds
        move.l  d0,d4
        lea     line_buffer,a0
        move.b  #13,(a0)+
        move.b  #' ',(a0)+
        move.b  #' ',(a0)+
        move.l  d4,d0
        bsr     put_time
        move.b  #' ',(a0)+
        move.b  #'/',(a0)+
        move.b  #' ',(a0)+
        move.l  total_seconds,d0
        bsr     put_time
        lea     txt_late,a1
        bsr     put_string
        move.l  last_ack,d0
        lsr.l   #8,d0
        lsr.l   #4,d0                   ; the acknowledgement's late count
        andi.l  #$fff,d0
        bsr     put_number
        lea     txt_hint,a1
        bsr     put_string
        clr.b   (a0)
        Cconws  line_buffer
.same:
        rts

; d0 = seconds -> "mm:ss" at (a0)+
put_time:
        andi.l  #$ffff,d0
        divu.w  #60,d0
        move.l  d0,d1
        swap    d1                      ; d1.w = seconds
        andi.l  #$ffff,d0               ; minutes
        cmpi.w  #99,d0
        bls.s   .minutes_ok
        moveq   #99,d0
.minutes_ok:
        bsr     put_two
        move.b  #':',(a0)+
        move.w  d1,d0
        bra     put_two

; d0.w < 100 -> two digits at (a0)+
put_two:
        andi.l  #$ffff,d0
        divu.w  #10,d0
        add.b   #'0',d0
        move.b  d0,(a0)+
        swap    d0
        add.b   #'0',d0
        move.b  d0,(a0)+
        rts

; d0.w -> decimal without leading zeros at (a0)+
put_number:
        movem.l d1-d2,-(sp)
        andi.l  #$ffff,d0
        moveq   #0,d2
.push:
        divu.w  #10,d0
        move.l  d0,d1
        swap    d1
        move.w  d1,-(sp)
        addq.w  #1,d2
        andi.l  #$ffff,d0
        bne.s   .push
.pop:
        move.w  (sp)+,d1
        add.b   #'0',d1
        move.b  d1,(a0)+
        subq.w  #1,d2
        bne.s   .pop
        movem.l (sp)+,d1-d2
        rts

; (a1) zero-terminated -> (a0)+
put_string:
.copy:
        move.b  (a1)+,d0
        beq.s   .done
        move.b  d0,(a0)+
        bra.s   .copy
.done:
        rts

; ---- failures

fail_lock:
        Cconws  txt_err_lock
        bra     die
fail_reserve:
        Cconws  txt_err_reserve
        bra     die
fail_boot:
        Cconws  txt_err_boot
        bra     die
fail_magic:
        Cconws  txt_err_magic
        bra     die
fail_sound:
        Cconws  txt_err_sound
die:
        Cconws  txt_press
        Cconin
        Pterm0

        data

txt_banner:     dc.b 13,10
                dc.b 'Indiana Jones and the Fate of Atlantis',13,10
                dc.b 'AdLib score played by the Falcon030 DSP',13,10,13,10
                dc.b 'The register stream of ScummVM',39,'s AdLib driver, decoded as in the',13,10
                dc.b 'ScummVM Falcon build and rendered by its DSP OPL2 kernel',13,10
                dc.b '(nine two-operator channels, 32-frame blocks, 49.17 kHz).',13,10,13,10,0
txt_playing:    dc.b 'Playing.',13,10,0
txt_again:      dc.b 13,10,'Again.',13,10,0
txt_bye:        dc.b 13,10,0
txt_late:       dc.b '   late periods ',0
txt_hint:       dc.b '   (any key quits)   ',0
txt_press:      dc.b 13,10,'Press a key.',13,10,0
txt_err_lock:   dc.b 'error: the DSP is in use',13,10,0
txt_err_reserve: dc.b 'error: Dsp_Reserve failed',13,10,0
txt_err_boot:   dc.b 'error: the DSP kernel did not start',13,10,0
txt_err_magic:  dc.b 'error: the embedded data is damaged',13,10,0
txt_err_sound:  dc.b 'error: Locksnd failed',13,10,0
        even

        include "oplrt_image.i"

        even
tables_data:
        incbin  "foa-tables.bin"
        even
score_data:
        incbin  "foa-score.bin"
        even

        bss
        even
total_periods:  ds.l 1
total_seconds:  ds.l 1
period_count:   ds.l 1
periods_done:   ds.l 1
shown_seconds:  ds.l 1
payload_base:   ds.l 1
payload_words:  ds.l 1
protocol_errors: ds.l 1
last_ack:       ds.l 1
dsp_tx_word:    ds.l 1
dsp_rx_word:    ds.l 1
dsp_stage2_reply: ds.l 1
quit_flag:      ds.b 1
                even
line_buffer:    ds.b 128
