; Resident 32-voice sustain renderer. Two physical SRAM banks; no bank aliasing.
; Startup: [X word count, X table words, Y word count, Y table words], then
; VOICES records [memory space (0=X), table base, initial phase].
; Per 64-frame block: VOICES records [step, left gain Q23, right gain Q23].
; Phase persists on the DSP. Returns signed 16-bit stereo in 24-bit host words.
        include 'ioequ.inc'
        include 'config.inc'
        org     p:0
        jmp     start
        if      SSI
        org     p:$10
ssi_tx:
        movep   x:(r2)+,x:m_tx
        lua     (r7)+,r7
        org     p:$12
        jsr     ssi_error
        endif
        org     p:$40
start:
        movep   #1,x:m_pbc
        movep   #0,x:m_bcr
        move    #>-1,m0
        move    #>-1,m3
        move    #>-1,m4
        move    #>-1,m6
        if      SSI
        movep   #$1f8,x:m_pcc
        movep   #$3000,x:m_ipr
        movep   #0,x:m_crb
        movep   #$4100,x:m_cra
        move    #>RING_WORDS-1,m2
        move    #>RING_WORDS-1,m6
        move    #>-1,m7
        move    #>-1,m5
        move    #>$3000,r2
        move    #>$3000,a
        move    a1,y:$71
        clr     a
        move    a1,r7
        move    a1,r5
        move    a1,y:$72
        move    a1,y:$73
        move    a1,y:$75
        move    a1,y:$76
        move    #>BLOCKS,a
        move    a1,y:$74
        andi    #$fc,mr
        endif
        move    #>TABLE_BASE,r0
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,x0
        do      x0,load_x_done
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,x:(r0)+
load_x_done:
        move    #>TABLE_BASE,r0
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,x0
        do      x0,load_y_done
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,y:(r0)+
load_y_done:
        move    #>0,r4
        do      #VOICES*3,load_state_done
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,y:(r4)+
load_state_done:
        jclr    #1,x:m_hsr,*
        movep   #$524459,x:m_htx
block:
        if      SSI
wait_room:
        move    y:$72,a
        move    r7,x0
        sub     x0,a
        move    #>$ffff,x0
        and     x0,a
        move    a1,a                   ; clear the subtraction's extension byte
        move    #>$8000,x0
        cmp     x0,a
        jge     underrun_before_block
        move    #>RING_WORDS-FRAMES*2,x0
        cmp     x0,a
        jgt     wait_room
        jmp     have_room
underrun_before_block:
        ; Consume the current packet before returning ERR: the host may
        ; already be transmitting it. This keeps its fast send loop simple.
        move    #>1,a
        move    a1,y:$73
        movep   #$1a00,x:m_crb
        jclr    #m_tde,x:m_sr,*
        movep   #0,x:m_tx
have_room:
        move    y:$71,r6
        else
        move    #>$3000,r6
        endif
        clr     a
        do      #FRAMES*2,clear_done
        move    a1,x:(r6)+
clear_done:
        move    #>0,r4
        do      #VOICES,voices_done
        move    y:(r4)+,a
        move    a1,y:$70
        move    y:(r4)+,r0
        move    y:(r4),r3
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,a
        move    a1,n3
        jclr    #23,a1,resident_mode
        move    #>2,a
        move    a1,y:$70
resident_mode:
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,y0
        jclr    #0,x:m_hsr,*
        movep   x:m_hrx,y1
        move    #>TABLE_SIZE*128,x1
        if      SSI
        move    y:$71,r6
        else
        move    #>$3000,r6
        endif
        move    y:$70,a
        move    #>2,x0
        cmp     x0,a
        jeq     voice_stream
        tst     a
        jne     voice_y
        do      #FRAMES,frames_x_done
loop_x:
        move    r3,x0
        mpy     x0,x1,a
        move    a1,n0
        lua     (r3)+n3,r3
        move    x:(r0+n0),x0
        move    x:(r6)+,a
        mac     x0,y0,a x:(r6)-,b
        mac     x0,y1,b a1,x:(r6)+
        move    b1,x:(r6)+
frames_x_done:
        jmp     voice_done
voice_y:
        do      #FRAMES,frames_y_done
loop_y:
        move    r3,x0
        mpy     x0,x1,a
        move    a1,n0
        lua     (r3)+n3,r3
        move    y:(r0+n0),x0
        move    x:(r6)+,a
        mac     x0,y0,a x:(r6)-,b
        mac     x0,y1,b a1,x:(r6)+
        move    b1,x:(r6)+
frames_y_done:
        nop
        jmp     voice_done
voice_stream:
        do      #FRAMES/2,stream_done
loop_stream:
        jclr    #0,x:m_hsr,*
stream_read:
        movep   x:m_hrx,x0
        move    x0,y:$7a
        move    #>$008000,x1
        mpy     x0,x1,a
        move    #>$fffff0,x1
        and     x1,a
        move    a1,x0
        lua     (r3)+n3,r3
        move    x:(r6)+,a
        mac     x0,y0,a x:(r6)-,b
        mac     x0,y1,b a1,x:(r6)+
        move    b1,x:(r6)+
        move    y:$7a,x0
        move    #>$000800,x1
        mpy     x0,x1,a
        move    a0,x0
        move    #>$008000,x1
        mpy     x0,x1,a
        move    a1,x0
        lua     (r3)+n3,r3
        move    x:(r6)+,a
        mac     x0,y0,a x:(r6)-,b
        mac     x0,y1,b a1,x:(r6)+
        move    b1,x:(r6)+
stream_done:
        nop
voice_done:
        move    r3,y:(r4)+
voices_done:
        if      SSI
        move    y:$71,r6
        move    #>128,x1
        do      #FRAMES*2,scale_done
        move    x:(r6),x0
        mpy     x0,x1,a
        move    a0,x:(r6)+
scale_done:
        move    r6,y:$71
        move    y:$73,a
        tst     a
        jne     underrun
        move    y:$75,a
        tst     a
        jeq     account_block
        move    y:$72,a
        move    r7,x0
        sub     x0,a
        move    #>$ffff,x0
        and     x0,a
        move    a1,a                   ; normalize the wrapped unsigned distance
        move    #>$8000,x0
        cmp     x0,a
        jlt     account_block
        jmp     underrun
account_block:
        move    y:$72,a
        move    #>FRAMES*2,x0
        add     x0,a
        move    #>$ffff,x0
        and     x0,a
        move    a1,y:$72
        move    y:$75,b
        tst     b
        jne     block_ack
        move    #>PREFILL_WORDS,x0
        cmp     x0,a
        jne     block_ack
        move    #>1,b
        move    b1,y:$75
        ; The crossbar consumes the priming word in its right slot. A dummy
        ; leaves the first real left sample at the next complete frame.
        movep   #0,x:m_tx
        movep   #$5a00,x:m_crb
block_ack:
        jclr    #1,x:m_hsr,*
        movep   a1,x:m_htx
        move    y:$74,a
        move    #>1,x0
        sub     x0,a
        move    a1,y:$74
        tst     a
        jne     block
drain:
        move    y:$72,a
        move    r7,x0
        cmp     x0,a
        jne     drain
        jmp     stop_ssi
underrun:
        ; Abort instead of waiting for the 16-bit consumer counter to wrap.
        ; The current control packet has been completely consumed.
        move    #>1,a
        move    a1,y:$73
        movep   #$1a00,x:m_crb
        jclr    #1,x:m_hsr,*
        movep   #$455252,x:m_htx
stop_ssi:
        movep   #$1a00,x:m_crb
        jclr    #m_tde,x:m_sr,*
        movep   #0,x:m_tx
        jclr    #1,x:m_hsr,*
        movep   y:$73,x:m_htx
        jclr    #1,x:m_hsr,*
        move    r5,a
        movep   a1,x:m_htx
        move    r7,a
        jclr    #1,x:m_hsr,*
        movep   a1,x:m_htx
        jclr    #1,x:m_hsr,*
        movep   #$454e44,x:m_htx
finished:
        jmp     finished
ssi_error:
        movep   x:m_sr,y:$77
        movep   x:(r2)+,x:m_tx
        lua     (r7)+,r7
        lua     (r5)+,r5
        rti
        else
        move    #>$3000,r6
        do      #FRAMES*2,output_done
        jclr    #1,x:m_hsr,*
        movep   x:(r6)+,x:m_htx
output_done:
        jmp     block
        endif
        end
