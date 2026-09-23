; SSI slot sharing: can the DAC play one slot pair of the crossbar frame
; while the other six carry other data?
;
; The DSP transmits a frame of eight 16-bit slots, as the c2p route does. In
; the slot pair the host names (the "tone pair") it sends a test tone, a
; 48-entry sine table at one step a frame on the left and two on the right;
; in every other slot a tag, $s0a5 for slot s, or zero in silent mode. The
; host routes the stream to DMA record and to the DAC and picks the DAC's
; track with Setmontracks: the DAC should then play the tone and nothing of
; the tags, and record shows which slot each word landed in.
;
; The frame's eight words wait in FRAME, a modulo-8 ring that r2 walks one
; word a slot. Slots are counted from the transmit frame sync: TDE with TFS
; set means the word just sent was slot 0's, so the next word written is
; slot PHASE's (1 as Hatari models it; the host can set another), and r2
; restarts there. The tone words are refreshed then too, after that word
; has gone, so the whole slot path is a poll and one MOVEP. The receiver is
; left alone: nothing here reads it.
;
; The program is a standalone Dsp_ExecBoot image: P memory only, below
; P:$0200, no interrupts. Keep the command words in sync with m68k/ssimix.s.

        include 'ioequ.inc'

CMD_PING        equ     $010000
CMD_ARM         equ     $020000         ; low byte: slots per frame - 1
CMD_PAIR        equ     $030000         ; low byte: tone pair, 0-3, or 4 for none
CMD_PHASE       equ     $040000         ; low byte: slot written after a frame sync
CMD_SILENT      equ     $050000         ; low byte 1: zeros instead of tags
CMD_TABLE       equ     $060000         ; low 16 bits: next sine table entry
CMD_FRAMES      equ     $070000         ; reply: frame syncs seen
REPLY_PING      equ     $4d4958         ; "MIX"
REPLY_ERROR     equ     $ffffff

TABLE_LENGTH    equ     48

; Internal X, by equates (see ssiecho.asm).
RESTART         equ     $0000           ; FRAME + PHASE: where r2 restarts
PAIR            equ     $0001           ; the tone pair, 0-4
SILENT          equ     $0002
FRAME           equ     $0010           ; eight words, an 8-aligned ring
SPARE           equ     $0018           ; where the tone goes with no pair
TABLE           equ     $0040           ; 48 words, a 64-aligned modulo ring

        org     p:$0000
        jmp     mix_start

        org     p:$0040
mix_start:
        movep   #1,x:m_pbc              ; host port
        movep   #$1f8,x:m_pcc           ; port C pins to the SSI
        clr     a
        move    #TABLE,r0
        do      #TABLE_LENGTH,mix_table_cleared
        move    a1,x:(r0)+
mix_table_cleared:
        move    #TABLE,r6               ; the table's write pointer
        move    a1,x:SILENT
        move    #0,r7                   ; frame syncs seen
        move    #>FRAME+1,x0
        move    x0,x:RESTART
        move    #FRAME,r2
        move    #7,m2
        move    #TABLE,r4               ; left: one step a frame
        move    #TABLE,r5               ; right: two
        move    #TABLE_LENGTH-1,m4
        move    #TABLE_LENGTH-1,m5
        move    #>2,n5
        move    #>4,a                   ; no tone until the host names a pair
        jsr     mix_set_pair
        move    #>7,a
        jsr     mix_arm

; The slot path: wait for TDE, restart the frame on TFS, send one word.
mix_loop:
        jclr    #6,x:m_sr,mix_host      ; TDE: the transmitter wants a word
        jset    #2,x:m_sr,mix_sync      ; TFS: slot 0's word just left
        movep   x:(r2)+,x:m_tx
        jmp     mix_loop
mix_sync:
        move    x:RESTART,r2
        move    (r7)+
        nop
        movep   x:(r2)+,x:m_tx
        ; the next frame's tone words, into its pair or SPARE
        move    x:(r4)+,x0
        move    x:(r5)+n5,x1
        move    x0,x:(r3)+
        move    x1,x:(r3)-
        jmp     mix_loop

; ---------------------------------------------------------------------------
; Host commands, one word each, answered with one word.
; ---------------------------------------------------------------------------
mix_host:
        jclr    #0,x:m_hsr,mix_loop
        movep   x:m_hrx,y1
        move    y1,b
        move    #>$ff0000,x0
        and     x0,b
        move    y1,a                    ; the argument, low 16 bits
        move    #>$00ffff,x0
        and     x0,a
        move    #>CMD_PING,x0
        cmp     x0,b
        jeq     mix_cmd_ping
        move    #>CMD_ARM,x0
        cmp     x0,b
        jeq     mix_cmd_arm
        move    #>CMD_PAIR,x0
        cmp     x0,b
        jeq     mix_cmd_pair
        move    #>CMD_PHASE,x0
        cmp     x0,b
        jeq     mix_cmd_phase
        move    #>CMD_SILENT,x0
        cmp     x0,b
        jeq     mix_cmd_silent
        move    #>CMD_TABLE,x0
        cmp     x0,b
        jeq     mix_cmd_table
        move    #>CMD_FRAMES,x0
        cmp     x0,b
        jeq     mix_cmd_frames
        movep   #REPLY_ERROR,x:m_htx
        jmp     mix_loop
mix_cmd_ping:
        movep   #REPLY_PING,x:m_htx
        jmp     mix_loop
mix_cmd_arm:
        jsr     mix_arm
        jmp     mix_reply_zero
mix_cmd_pair:
        jsr     mix_set_pair
        jmp     mix_reply_zero
mix_cmd_phase:
        move    #>FRAME,x0
        add     x0,a
        move    a1,x:RESTART
        jmp     mix_reply_zero
mix_cmd_silent:
        move    a1,x:SILENT
        move    x:PAIR,a
        jsr     mix_set_pair
        jmp     mix_reply_zero
mix_cmd_table:
        rep     #8                      ; 16 bits into bits 23..8
        lsl     a
        move    a1,x:(r6)+
        jmp     mix_reply_zero
mix_cmd_frames:
        move    r7,x0
        movep   x0,x:m_htx
        jmp     mix_loop
mix_reply_zero:
        movep   #0,x:m_htx
        jmp     mix_loop

; Fill FRAME with the tags ($s0a5 for slot s), or zeros when SILENT, and
; point r3 at the tone pair a1 names (0-3), or at SPARE for 4 or more.
; The tone words land there from the next frame sync on.
mix_set_pair:
        move    a1,x:PAIR
        move    #FRAME,r0
        move    x:SILENT,b
        tst     b
        jne     mix_pair_zeros
        move    #>$00a500,b
        move    #>$100000,x0
        do      #8,mix_pair_tagged
        move    b1,x:(r0)+
        add     x0,b
mix_pair_tagged:
        jmp     mix_pair_pointer
mix_pair_zeros:
        clr     b
        do      #8,mix_pair_pointer
        move    b1,x:(r0)+
mix_pair_pointer:
        move    #>4,x0
        cmp     x0,a
        jlt     mix_pair_slots
        move    #SPARE,r3
        rts
mix_pair_slots:
        lsl     a
        move    #>FRAME,x0
        add     x0,a
        move    a1,r3
        rts

; Program the SSI from scratch: 16-bit words, a1+1 slots per network frame,
; receiver and transmitter on, polled. in: a1 = slots per frame - 1
mix_arm:
        movep   #0,x:m_crb
        rep     #8
        lsl     a
        move    #>$4000,x0              ; WL = 16 bits
        or      x0,a
        movep   a1,x:m_cra
        movep   #$3a00,x:m_crb          ; RE, TE, network, synchronous
        movep   #0,x:m_tx
        rts

        end
