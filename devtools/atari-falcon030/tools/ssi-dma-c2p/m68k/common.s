; Shared routines, strings and buffers of the SSI DMA tests. Included at
; the end of each program, which defines txt_filename, the report's name.

        text

; ---------------------------------------------------------------------------
; Supervisor helpers.
; ---------------------------------------------------------------------------

; DMA writes to RAM do not reach the 68030's data cache, which still holds
; what the CPU wrote there, the sentinels; clear it before reading what
; record wrote (CACR bit 11, CD). Clobbers d1.
clear_data_cache:
        movec   cacr,d1
        bset    #11,d1
        movec   d1,cacr
        rts

; Timer C counts since the poll started, from d6 and d5.
fine_now:
        move.l  d6,d0
        mulu.l  #TIMER_RELOAD,d0
        moveq   #0,d1
        move.b  d5,d1
        sub.l   d1,d0
        add.l   #TIMER_RELOAD,d0
        rts

; Exchange one word with the DSP through the host port registers.
; in: d0.l = command   out: d0.l = reply
host_exchange:
.tx:
        btst    #1,HOST_ISR.w
        beq     .tx
        move.l  d0,HOST_DATA.w
.rx:
        btst    #0,HOST_ISR.w
        beq     .rx
        move.l  HOST_DATA.w,d0
        andi.l  #$00ffffff,d0
        rts

; The sound matrix registers as the route left them: DMA control, sound
; mode, source and destination routing, the prescalers, the track selects.
read_regs:
        lea     reg_buffer,a0
        move.w  $ffff8900.w,(a0)+
        move.w  $ffff8920.w,(a0)+
        move.w  $ffff8930.w,(a0)+
        move.w  $ffff8932.w,(a0)+
        move.w  $ffff8934.w,(a0)+
        move.w  $ffff8936.w,(a0)+
        rts

read_hz200:
        move.l  HZ200.w,d0
        rts

; ---------------------------------------------------------------------------
; User-mode helpers.
; ---------------------------------------------------------------------------

; Exchange one packed 24-bit word with the DSP through TOS.
; in: d0.l = command   out: d0.l = reply
dsp_exchange:
        movem.l d1-d7/a0-a6,-(sp)
        move.l  d0,dsp_tx_word
        clr.l   dsp_rx_word
        Dsp_BlkUnpacked dsp_tx_word,#1,dsp_rx_word,#1
        move.l  dsp_rx_word,d0
        movem.l (sp)+,d1-d7/a0-a6
        rts

; Current 200 Hz tick in d0.l.
get_ticks:
        movem.l d1-d2/a0-a2,-(sp)
        Supexec read_hz200
        movem.l (sp)+,d1-d2/a0-a2
        rts

; Let a new connection settle.
settle:
        movem.l d0/d3,-(sp)
        bsr     get_ticks
        move.l  d0,d3
.settle:
        bsr     get_ticks
        sub.l   d3,d0
        cmpi.l  #SETTLE_TICKS,d0
        bcs     .settle
        movem.l (sp)+,d0/d3
        rts

; Print line_buffer and append it to the results image.
emit_line:
        movem.l d0-d2/a0-a2,-(sp)
        Cconws  line_buffer
        lea     line_buffer,a1
        movea.l results_ptr,a0
.copy:
        move.b  (a1)+,d0
        beq     .done
        cmpa.l  #results_buffer_end,a0
        bcc     .done
        move.b  d0,(a0)+
        bra     .copy
.done:
        move.l  a0,results_ptr
        movem.l (sp)+,d0-d2/a0-a2
        rts

; Append the NUL-terminated fragment at a1 to (a0)+, without the NUL.
fmt_string:
        move.b  (a1)+,(a0)+
        bne     fmt_string
        subq.l  #1,a0
        rts

; Append the register snapshot as " 8900=xxxx 8920=xxxx ..." at (a0)+.
fmt_regs:
        movem.l d0-d2/a1-a2,-(sp)
        lea     reg_buffer,a2
        lea     reg_names,a1
        moveq   #6-1,d2
.reg:
        move.b  #' ',(a0)+
        bsr     fmt_string
        moveq   #0,d0
        move.w  (a2)+,d0
        bsr     fmt_hex16
        dbf     d2,.reg
        movem.l (sp)+,d0-d2/a1-a2
        rts

; Append d0.l as unsigned decimal at (a0)+.
fmt_u32:
        movem.l d0-d2,-(sp)
        moveq   #0,d2
.digit:
        moveq   #0,d1
        divu.l  #10,d1:d0
        addq.w  #1,d2
        move.w  d1,-(sp)
        tst.l   d0
        bne     .digit
.emit:
        move.w  (sp)+,d1
        addi.b  #'0',d1
        move.b  d1,(a0)+
        subq.w  #1,d2
        bne     .emit
        movem.l (sp)+,d0-d2
        rts

; Append the low 16 bits of d0.l as four hex digits at (a0)+.
fmt_hex16:
        movem.l d0-d2,-(sp)
        moveq   #4,d2
        swap    d0
        bra     fmt_hex_digits
; Append the low 8 bits of d0.l as two hex digits at (a0)+.
fmt_hex8:
        movem.l d0-d2,-(sp)
        moveq   #2,d2
        swap    d0
        rol.l   #8,d0
fmt_hex_digits:
        rol.l   #4,d0
        move.b  d0,d1
        andi.b  #$0f,d1
        cmpi.b  #10,d1
        bcs     .digit
        addi.b  #39,d1                    ; lowercase a-f
.digit:
        addi.b  #'0',d1
        move.b  d1,(a0)+
        subq.w  #1,d2
        bne     fmt_hex_digits
        movem.l (sp)+,d0-d2
        rts

; Append d0.l millihertz as "NNNNN.NNN" at (a0)+.
fmt_millihertz:
        movem.l d0-d1,-(sp)
        moveq   #0,d1
        divu.l  #1000,d1:d0
        bsr     fmt_u32
        move.b  #'.',(a0)+
        move.l  d1,d0
        bsr     fmt_pad3
        movem.l (sp)+,d0-d1
        rts

; Append d0.l Timer C counts as milliseconds, "NNN.NNN ms", at (a0)+.
fmt_fine:
        movem.l d0-d1,-(sp)
        mulu.l  #1000000,d1:d0            ; microseconds * FINE_HZ
        divu.l  #FINE_HZ,d1:d0
        moveq   #0,d1
        divu.l  #1000,d1:d0
        bsr     fmt_u32
        move.b  #'.',(a0)+
        move.l  d1,d0
        bsr     fmt_pad3
        FSTR    txt_ms
        movem.l (sp)+,d0-d1
        rts

; Append d0.l (0-999) as exactly three digits at (a0)+.
fmt_pad3:
        movem.l d0-d1,-(sp)
        divu.w  #100,d0
        move.b  d0,d1
        addi.b  #'0',d1
        move.b  d1,(a0)+
        clr.w   d0
        swap    d0
        divu.w  #10,d0
        move.b  d0,d1
        addi.b  #'0',d1
        move.b  d1,(a0)+
        swap    d0
        move.b  d0,d1
        addi.b  #'0',d1
        move.b  d1,(a0)+
        movem.l (sp)+,d0-d1
        rts

; Terminate the line at (a0) with CRLF and NUL.
line_done:
        move.b  #13,(a0)+
        move.b  #10,(a0)+
        clr.b   (a0)
        rts

; Write the accumulated report beside the program.
write_results:
        movem.l d4-d5,-(sp)
        Fcreate txt_filename,#0
        tst.l   d0
        bmi     .done
        move.w  d0,d4
        move.l  results_ptr,d5
        sub.l   #results_buffer,d5
        Fwrite  d4,d5,results_buffer
        Fclose  d4
.done:
        movem.l (sp)+,d4-d5
        rts

; Discard any buffered key, then require a fresh keypress so the report
; stays visible on a real Falcon desktop.
wait_exit_key:
.drain:
        Cconis
        tst.l   d0
        beq     .wait
        Cconin
        bra     .drain
.wait:
        Cconws  txt_exit
        Cconin
        rts

        data

txt_ms:         dc.b ' ms',0
txt_exit:       dc.b 13,10,'press any key to exit',13,10,0
reg_names:
        dc.b    '8900=',0
        dc.b    '8920=',0
        dc.b    '8930=',0
        dc.b    '8932=',0
        dc.b    '8934=',0
        dc.b    '8936=',0
        even

        bss
        even
results_ptr:    ds.l 1
dsp_tx_word:    ds.l 1
dsp_rx_word:    ds.l 1
reg_buffer:     ds.w 6
line_buffer:    ds.b 200
results_buffer: ds.b 4096
results_buffer_end:
