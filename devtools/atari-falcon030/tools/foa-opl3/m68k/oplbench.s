; Falcon host for the OPL DSP synthesis benchmark.
;
; Boots dsp/opl.asm the same way the sibling player boots its kernel
; (Dsp_ExecBoot, no TOS loader in the path), uploads the tables and the
; operator state a host run of opl-kernel.h produced, renders a fixed number
; of frames, and writes them back for a word-for-word comparison against that
; reference. It plays nothing: no SSI, no codec, no interrupt.
;
; OPLDATA.BIN is big-endian 32-bit throughout:
;   'OPLD', channels, delayed carriers, frame count, block count,
;   then per block: space (0 = X, 1 = Y), address, word count, the words.

        include "xbios.i"

        global  start

CMD_PING        equ     $010000
CMD_WRITE_X     equ     $020000
CMD_WRITE_Y     equ     $030000
CMD_CHANNELS    equ     $040000
CMD_DELAYED     equ     $050000
CMD_RENDER      equ     $060000
CMD_READ_X      equ     $070000
CMD_REWIND      equ     $080000
CMD_RDELAYED    equ     $090000
REPLY_PING      equ     $4F504C

DSP_ABILITY     equ     3
FRAME_BASE      equ     $3000
DATA_MAGIC      equ     $4F504C44

        text

start:
        Cconws  txt_banner

        Dsp_Reserve #16,#16
        tst.l   d0
        bmi     fail_reserve

        Dsp_ExecBoot opl_boot_image,#OPL_BOOT_WORDS,#DSP_ABILITY

        move.l  #CMD_PING,d0
        bsr     dsp_exchange
        cmp.l   #REPLY_PING,d0
        bne     fail_ping
        Cconws  txt_booted

        ; ---- read the data image
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
        move.l  (a3)+,channel_count
        move.l  (a3)+,delayed_count
        move.l  (a3)+,rdelayed_count
        move.l  (a3)+,frame_count
        move.l  (a3)+,block_count

        ; ---- rewind first: the uploads carry the output pipeline state
        move.l  #CMD_REWIND,d0
        bsr     dsp_exchange

        ; ---- upload every block
        move.l  block_count,d7
        beq     blocks_done
upload_block:
        move.l  (a3)+,d6                ; space
        move.l  (a3)+,d5                ; address
        move.l  (a3)+,d4                ; word count
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
        beq     .block_done
.word_loop:
        move.l  (a3)+,d0
        bsr     dsp_exchange
        subq.l  #1,d3
        bne     .word_loop
.block_done:
        subq.l  #1,d7
        bne     upload_block
blocks_done:
        Cconws  txt_uploaded

        ; ---- configure and render
        move.l  #CMD_CHANNELS,d0
        or.l    channel_count,d0
        bsr     dsp_exchange
        move.l  #CMD_DELAYED,d0
        or.l    delayed_count,d0
        bsr     dsp_exchange
        move.l  #CMD_RDELAYED,d0
        or.l    rdelayed_count,d0
        bsr     dsp_exchange

        Cconws  txt_rendering
        move.l  #CMD_RENDER,d0
        or.l    frame_count,d0
        bsr     dsp_exchange
        Cconws  txt_rendered

        ; ---- read the frames back, two words each
        move.l  frame_count,d7
        add.l   d7,d7
        lea     frame_buffer,a4
        move.l  #FRAME_BASE,d6
        move.l  d7,d5
        beq     frames_done
read_frame_word:
        move.l  #CMD_READ_X,d0
        or.l    d6,d0
        bsr     dsp_exchange
        move.l  d0,(a4)+
        addq.l  #1,d6
        subq.l  #1,d5
        bne     read_frame_word
frames_done:

        Fcreate txt_framefile,#0
        tst.l   d0
        bmi     fail_create
        move.w  d0,file_handle
        move.l  d7,d0
        lsl.l   #2,d0
        Fwrite  file_handle,d0,frame_buffer
        Fclose  file_handle
        Cconws  txt_written
        Pterm0

fail_reserve:
        Cconws  txt_err_reserve
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
fail_create:
        Cconws  txt_err_create
die:
        Pterm0

; Exchange one packed 24-bit word with the DSP.
; in: d0.l = command   out: d0.l = reply
dsp_exchange:
        movem.l d1-d7/a0-a6,-(sp)
        move.l  d0,dsp_tx_word
        clr.l   dsp_rx_word
        Dsp_BlkUnpacked dsp_tx_word,#1,dsp_rx_word,#1
        move.l  dsp_rx_word,d0
        movem.l (sp)+,d1-d7/a0-a6
        rts

        data

txt_banner:     dc.b 13,10,'OPL DSP synthesis benchmark',13,10,0
txt_booted:     dc.b 'kernel booted',13,10,0
txt_uploaded:   dc.b 'tables and state uploaded',13,10,0
txt_rendering:  dc.b 'rendering',13,10,0
txt_rendered:   dc.b 'render complete',13,10,0
txt_written:    dc.b 'frames written',13,10,0
txt_err_reserve: dc.b 'error: Dsp_Reserve failed',13,10,0
txt_err_ping:   dc.b 'error: kernel did not answer',13,10,0
txt_err_open:   dc.b 'error: cannot open OPLDATA.BIN',13,10,0
txt_err_read:   dc.b 'error: cannot read OPLDATA.BIN',13,10,0
txt_err_magic:  dc.b 'error: OPLDATA.BIN is not an OPL image',13,10,0
txt_err_create: dc.b 'error: cannot create FRAMES.BIN',13,10,0
txt_datafile:   dc.b 'OPLDATA.BIN',0
txt_framefile:  dc.b 'FRAMES.BIN',0
        even

        include "opl_boot.i"

        bss
        even
file_handle:    ds.w 1
        even
data_length:    ds.l 1
channel_count:  ds.l 1
delayed_count:  ds.l 1
rdelayed_count: ds.l 1
frame_count:    ds.l 1
block_count:    ds.l 1
dsp_tx_word:    ds.l 1
dsp_rx_word:    ds.l 1
DATA_LIMIT      equ 256*1024
data_buffer:    ds.b DATA_LIMIT
frame_buffer:   ds.b 24*1024
