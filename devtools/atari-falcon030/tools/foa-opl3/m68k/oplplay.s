; Falcon host for the practical OPL DSP kernel's stream mode.
;
; Boots dsp/oplrt.asm through the two-stage loader, uploads the tables and
; records from OPLDATA.BIN, routes the DSP's SSI to the DAC at 49.170 kHz,
; and streams the periods of PLAYDATA.BIN through the production refill
; protocol with direct host-port writes paced on TXDE. At the end it stops
; the stream, queries the period, late and checksum counters, and writes
; them to RESULT.BIN for the gate.
;
; PLAYDATA.BIN is big-endian 32-bit: 'OPLP', period count, then per period:
; event count, two words per event, and a PCM flag (0: silent).

        include "xbios.i"

        global  start

CMD_PING        equ     $010000
CMD_WRITE_X     equ     $020000
CMD_WRITE_Y     equ     $030000
CMD_STREAM_START equ    $090000
CMD_REFILL      equ     $0A0000
CMD_STREAM_STOP equ     $0B0000
CMD_STATUS      equ     $0C0000
CMD_CHECKSUM    equ     $0D0000
REPLY_PING      equ     $4F5052
REPLY_READY     equ     $524459

DSP_ABILITY     equ     3
DATA_MAGIC      equ     $4F504C52
PLAY_MAGIC      equ     $4F504C50

DSP_HOST_ISR    equ     $ffffa202
DSP_HOST_DATA   equ     $ffffa204

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

        text

start:
        Cconws  txt_banner

        Dsp_Reserve #16,#16
        tst.l   d0
        bmi     fail_reserve
        Dsp_ExecBoot dsp_bootstrap_image,#DSP_BOOT_WORDS,#DSP_ABILITY
        clr.l   dsp_stage2_reply
        Dsp_BlkUnpacked dsp_program_image,#DSP_STAGE2_TRANSFER_WORDS,dsp_stage2_reply,#1
        move.l  dsp_stage2_reply,d0
        cmp.l   #DSP_STAGE2_REPLY_OK,d0
        bne     fail_load
        move.l  #CMD_PING,d0
        bsr     dsp_exchange
        cmp.l   #REPLY_PING,d0
        bne     fail_ping
        Cconws  txt_booted

        ; ---- tables and records
        Fopen   txt_datafile,#0
        tst.l   d0
        bmi     fail_open
        move.w  d0,file_handle
        Fread   file_handle,#DATA_LIMIT,data_buffer
        move.l  d0,data_length
        Fclose  file_handle
        tst.l   data_length
        ble     fail_read
        lea     data_buffer,a3
        cmp.l   #DATA_MAGIC,(a3)+
        bne     fail_magic
        move.l  (a3)+,d7
        beq     blocks_done
upload_block:
        move.l  (a3)+,d6
        move.l  (a3)+,d5
        move.l  (a3)+,d4
        bsr     upload_words
        subq.l  #1,d7
        bne     upload_block
blocks_done:
        Cconws  txt_uploaded

        ; ---- the periods
        Fopen   txt_playfile,#0
        tst.l   d0
        bmi     fail_open
        move.w  d0,file_handle
        Fread   file_handle,#PLAY_LIMIT,play_buffer
        move.l  d0,play_length
        Fclose  file_handle
        tst.l   play_length
        ble     fail_read
        lea     play_buffer,a3
        cmp.l   #PLAY_MAGIC,(a3)+
        bne     fail_magic
        move.l  (a3)+,period_count

        ; ---- the codec: the DSP transmits to the DAC at 49.170 kHz
        Locksnd
        cmpi.l  #1,d0
        bne     fail_sound
        Buffoper #SOUND_DMA_STOP
        Sndstatus #SNDSTAT_RESET
        Soundcmd #SOUND_ADDERIN,#SOUND_MATRIXIN
        Setmode #SOUND_STEREO16
        Settracks #0,#0
        Setmontracks #0
        Dsptristate #1,#0
        Devconnect #SOUND_DSP_XMIT,#SOUND_DAC,#SOUND_CLK25M,#SOUND_CLK50K,#SOUND_NO_SHAKE

        move.l  #CMD_STREAM_START,d0
        bsr     dsp_exchange
        Cconws  txt_streaming

period_loop:
        tst.l   period_count
        beq     periods_done
        ; the payload words follow the count in the file: count, events, flag
        move.l  (a3),d0                 ; event count
        add.l   d0,d0
        addq.l  #2,d0                   ; plus the count word and the flag word
        move.l  d0,payload_words
        move.l  a3,payload_base
        lsl.l   #2,d0
        add.l   d0,a3
        bsr     submit_period
        subq.l  #1,period_count
        bra     period_loop
periods_done:
        ; let the last rendered periods play out before stopping
        move.w  #6,d3
drain_loop:
        Vsync
        dbra    d3,drain_loop
        move.l  #CMD_STATUS,d0
        bsr     dsp_exchange
        move.l  d0,result_status
        move.l  #CMD_CHECKSUM,d0
        bsr     dsp_exchange
        move.l  d0,result_checksum

        ; the gate quits the emulator at the stop command: write first
        Fcreate txt_resultfile,#0
        tst.l   d0
        bmi     fail_create
        move.w  d0,file_handle
        Fwrite  file_handle,#8,result_status
        Fclose  file_handle
        Cconws  txt_written
        move.l  #CMD_STREAM_STOP,d0
        bsr     dsp_exchange
        Unlocksnd
        Pterm0

; One period: announce, wait for READY, blast the payload paced on TXDE,
; wait for the acknowledgement. The host port is supervisor-only.
submit_period:
        Supexec submit_period_super
        rts

submit_period_super:
        movem.l d0-d3/a0,-(sp)
        move.w  sr,-(sp)
        ori.w   #$0700,sr
.tx_wait:
        btst    #1,DSP_HOST_ISR
        beq.s   .tx_wait
        move.l  #CMD_REFILL,DSP_HOST_DATA
        bsr     recv_word
        cmp.l   #REPLY_READY,d0
        bne     .bad_reply
        movea.l payload_base,a0
        move.l  payload_words,d3
.word_loop:
        btst    #1,DSP_HOST_ISR
        beq.s   .word_loop
        move.l  (a0)+,DSP_HOST_DATA
        subq.l  #1,d3
        bne.s   .word_loop
        bsr     recv_word               ; the OK acknowledgement
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
; every word acknowledged by the kernel.
upload_words:
        move.l  #CMD_WRITE_X,d0
        tst.l   d6
        beq     .space_ready
        move.l  #CMD_WRITE_Y,d0
.space_ready:
        bsr     dsp_exchange
        move.l  d5,d0
        bsr     dsp_exchange
        move.l  d4,d0
        bsr     dsp_exchange
        move.l  d4,d3
        beq     .done
.word_loop:
        move.l  (a3)+,d0
        bsr     dsp_exchange
        subq.l  #1,d3
        bne     .word_loop
.done:
        rts

fail_reserve:
        Cconws  txt_err_reserve
        bra     die
fail_load:
        Cconws  txt_err_load
        bra     die
fail_ping:
        Cconws  txt_err_ping
        bra     die
fail_open:
        Cconws  txt_err_open
        bra     die
fail_read:
        Cconws  txt_err_read
        bra     die
fail_magic:
        Cconws  txt_err_magic
        bra     die
fail_sound:
        Cconws  txt_err_sound
        bra     die
fail_create:
        Cconws  txt_err_create
die:
        Pterm0

dsp_exchange:
        movem.l d1-d7/a0-a6,-(sp)
        move.l  d0,dsp_tx_word
        clr.l   dsp_rx_word
        Dsp_BlkUnpacked dsp_tx_word,#1,dsp_rx_word,#1
        move.l  dsp_rx_word,d0
        movem.l (sp)+,d1-d7/a0-a6
        rts

        data

txt_banner:     dc.b 13,10,'OPL practical DSP kernel stream test',13,10,0
txt_booted:     dc.b 'kernel booted',13,10,0
txt_uploaded:   dc.b 'tables and records uploaded',13,10,0
txt_streaming:  dc.b 'streaming',13,10,0
txt_written:    dc.b 'result written',13,10,0
txt_err_reserve: dc.b 'error: Dsp_Reserve failed',13,10,0
txt_err_load:   dc.b 'error: the stage-two loader did not acknowledge',13,10,0
txt_err_ping:   dc.b 'error: kernel did not answer',13,10,0
txt_err_open:   dc.b 'error: cannot open an input file',13,10,0
txt_err_read:   dc.b 'error: cannot read an input file',13,10,0
txt_err_magic:  dc.b 'error: an input file has the wrong magic',13,10,0
txt_err_sound:  dc.b 'error: Locksnd failed',13,10,0
txt_err_create: dc.b 'error: cannot create RESULT.BIN',13,10,0
txt_datafile:   dc.b 'OPLDATA.BIN',0
txt_playfile:   dc.b 'PLAYDATA.BIN',0
txt_resultfile: dc.b 'RESULT.BIN',0
        even

        include "oplrt_image.i"

        bss
        even
file_handle:    ds.w 1
        even
data_length:    ds.l 1
play_length:    ds.l 1
period_count:   ds.l 1
payload_base:   ds.l 1
payload_words:  ds.l 1
protocol_errors: ds.l 1
result_status:  ds.l 1
result_checksum: ds.l 1
dsp_tx_word:    ds.l 1
dsp_rx_word:    ds.l 1
dsp_stage2_reply: ds.l 1
DATA_LIMIT      equ 256*1024
PLAY_LIMIT      equ 4096*1024
data_buffer:    ds.b DATA_LIMIT
play_buffer:    ds.b PLAY_LIMIT
