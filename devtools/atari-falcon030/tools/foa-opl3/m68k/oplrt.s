; Falcon host for the practical OPL DSP kernel bench.
;
; Boots dsp/oplrt.asm through the sibling player's two-stage loader (the
; kernel no longer fits the 512 words Dsp_ExecBoot can place), uploads the
; tables and initial records from OPLDATA.BIN, then for each chunk uploads
; its events, renders its blocks and reads the frames back into FRAMES.BIN
; for a word-for-word comparison against the host reference. It plays
; nothing: no SSI, no codec, no interrupt.
;
; OPLDATA.BIN is big-endian 32-bit throughout:
;   'OPLR', upload block count, per block: space (0 = X, 1 = Y), address,
;   word count, the words; then chunk count, per chunk: block count, event
;   count, and two words per event.

        include "xbios.i"

        global  start

CMD_PING        equ     $010000
CMD_WRITE_X     equ     $020000
CMD_WRITE_Y     equ     $030000
CMD_EVENTS      equ     $040000
CMD_RENDER      equ     $050000
CMD_READ_X      equ     $060000
CMD_REWIND      equ     $070000
CMD_STOP        equ     $080000
REPLY_PING      equ     $4F5052

DSP_ABILITY     equ     3
FRAME_BASE      equ     $2000
EVENT_BASE      equ     $3000
DATA_MAGIC      equ     $4F504C52

        text

start:
        Cconws  txt_banner

        Dsp_Reserve #16,#16
        tst.l   d0
        bmi     fail_reserve

        ; XBIOS boots at most 512 internal-P words: the loader, which then
        ; streams the complete sparse program and acknowledges once.
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
        move.l  (a3)+,block_count

        ; ---- upload every block
        move.l  block_count,d7
        beq     blocks_done
upload_block:
        move.l  (a3)+,d6                ; space
        move.l  (a3)+,d5                ; address
        move.l  (a3)+,d4                ; word count
        bsr     upload_words
        subq.l  #1,d7
        bne     upload_block
blocks_done:
        Cconws  txt_uploaded

        Fcreate txt_framefile,#0
        tst.l   d0
        bmi     fail_create
        move.w  d0,out_handle

        move.l  (a3)+,chunk_count
        Cconws  txt_rendering
chunk_loop:
        tst.l   chunk_count
        beq     chunks_done
        move.l  (a3)+,chunk_blocks
        move.l  (a3)+,event_count

        move.l  #CMD_REWIND,d0
        bsr     dsp_exchange

        ; events into the DSP table, two words each
        move.l  event_count,d4
        add.l   d4,d4
        moveq   #0,d6                   ; X
        move.l  #EVENT_BASE,d5
        bsr     upload_words
        move.l  #CMD_EVENTS,d0
        or.l    event_count,d0
        bsr     dsp_exchange

        move.l  #CMD_RENDER,d0
        or.l    chunk_blocks,d0
        bsr     dsp_exchange

        ; ---- read the frames back, one word each
        move.l  chunk_blocks,d7
        lsl.l   #5,d7                   ; 32 frames per block
        lea     frame_buffer,a4
        move.l  #FRAME_BASE,d6
        move.l  d7,d5
read_frame_word:
        move.l  #CMD_READ_X,d0
        or.l    d6,d0
        bsr     dsp_exchange
        move.l  d0,(a4)+
        addq.l  #1,d6
        subq.l  #1,d5
        bne     read_frame_word

        move.l  d7,d0
        lsl.l   #2,d0
        Fwrite  out_handle,d0,frame_buffer
        subq.l  #1,chunk_count
        bra     chunk_loop
chunks_done:
        Fclose  out_handle
        move.l  #CMD_STOP,d0
        bsr     dsp_exchange
        Cconws  txt_written
        Pterm0

; Upload d4.l words from (a3) to DSP space d6 (0 X, 1 Y) at address d5.
; Every word is acknowledged by the kernel.
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

txt_banner:     dc.b 13,10,'OPL practical DSP kernel bench',13,10,0
txt_booted:     dc.b 'kernel booted',13,10,0
txt_uploaded:   dc.b 'tables and records uploaded',13,10,0
txt_rendering:  dc.b 'rendering',13,10,0
txt_written:    dc.b 'frames written',13,10,0
txt_err_reserve: dc.b 'error: Dsp_Reserve failed',13,10,0
txt_err_load:   dc.b 'error: the stage-two loader did not acknowledge',13,10,0
txt_err_ping:   dc.b 'error: kernel did not answer',13,10,0
txt_err_open:   dc.b 'error: cannot open OPLDATA.BIN',13,10,0
txt_err_read:   dc.b 'error: cannot read OPLDATA.BIN',13,10,0
txt_err_magic:  dc.b 'error: OPLDATA.BIN is not an OPLR image',13,10,0
txt_err_create: dc.b 'error: cannot create FRAMES.BIN',13,10,0
txt_datafile:   dc.b 'OPLDATA.BIN',0
txt_framefile:  dc.b 'FRAMES.BIN',0
        even

        include "oplrt_image.i"

        bss
        even
file_handle:    ds.w 1
out_handle:     ds.w 1
        even
data_length:    ds.l 1
block_count:    ds.l 1
chunk_count:    ds.l 1
chunk_blocks:   ds.l 1
event_count:    ds.l 1
dsp_tx_word:    ds.l 1
dsp_rx_word:    ds.l 1
dsp_stage2_reply: ds.l 1
DATA_LIMIT      equ 2048*1024
data_buffer:    ds.b DATA_LIMIT
frame_buffer:   ds.b 16*1024
