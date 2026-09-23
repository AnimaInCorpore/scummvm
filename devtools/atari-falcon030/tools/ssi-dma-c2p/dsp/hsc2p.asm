; SSI DMA c2p in handshake mode: the DSP converts a chunky 8-bit screen the
; sound DMA streams through it into Falcon 8-plane words, which DMA record
; writes back, and the DSP paces both transfers itself.
;
; In handshake mode the crossbar moves a word only when the DSP asks for it:
;   in   DMA playback delivers the next word after the DSP raises SC1 (port
;        C bit 4, a GPIO output): a write to PCD with that bit set
;   out  DMA record takes a word the DSP writes to TX while SC2 (bit 5) is
;        high; TDE comes back once it has
; so nothing is lost however late the DSP is, only delayed. That matters
; under Hatari, which runs the DSP between 68030 instructions and in the
; free-running mode loses words whenever an instruction outlasts a slot. It
; saves the offset and marker bookkeeping too: the DSP reads exactly the
; screen's chunky words and writes exactly its planar lines.
;
; I/O. Two fast interrupts, each two instructions:
;   receive   store RX at X:(r7) in a 32-word ring and ask for the next word
;   transmit  send Y:(r6) from a 32-word output ring
; The main loop meters both by their enable bits (see hs_gate): receive
; stays on while fewer than 16 words wait, transmit while at least 16 are
; queued, or to the end of a screen. A group takes about five crossbar
; ticks, so neither ring can overrun or run dry. r7, m7, r6 and m6 belong to
; the interrupts.
;
; A screen. The host sends CMD_GEOMETRY (lines, and the zero words each
; side of a line: the backend's planar lines have a 16-pixel margin) and
; then CMD_SYNC with the groups per line, which starts it: per line PAD
; zeros, the line's groups, PAD zeros. Record's buffer is exactly that long.
; The DSP asks for the first word before the host starts the DMA: Hatari's
; crossbar runs free until the DSP has asked once.
;
; Timing. The group conversion (C2PCORE.INC, from gen-c2p.py) is about 190
; instruction cycles with its loop, the gating about 30 and the interrupts 7
; a word: about 280 of the 325 a group lasts at 49,170 Hz frames. At
; 25.175 MHz the stream then runs at the crossbar's word rate, output bound:
; 176 words out for 160 in per 320-pixel line.
;
; The program is a standalone Dsp_ExecBoot image: P memory only, below
; P:$0200. Keep the command words in sync with the hosts.

        include 'ioequ.inc'

CMD_PING        equ     $010000
CMD_ARM         equ     $020000
CMD_GEOMETRY    equ     $030000         ; low 16 bits: lines << 4 | pad words
CMD_SYNC        equ     $040000         ; low byte: groups per line; starts a screen
CMD_STATE       equ     $050000         ; reply: 0 idle, 1 converting, 2 done
CMD_GROUPS      equ     $060000         ; reply: groups converted this screen
REPLY_PING      equ     $485332         ; "HS2"
REPLY_ERROR     equ     $ffffff

RIE             equ     15              ; SSI CRB bits
TIE             equ     14
SC1             equ     4               ; port C: the input handshake
SC2             equ     5               ; and the output one

; Internal X. Equates only: a ds block would emit an empty X section that
; the boot-image converter rejects.
GROUPS          equ     $0000
STATE           equ     $0001
PAD             equ     $0002
LINES           equ     $0003
GPL             equ     $0004           ; groups per line
FLUSH           equ     $0005           ; the screen's input is all in
TXSTART         equ     $0006           ; the screen's output has started
RING            equ     $0040           ; X in, Y out, 32 words each

; ---------------------------------------------------------------------------
        org     p:$0000
        jmp     hs_start

        org     p:$000c                 ; SSI receive data: fast interrupt
        movep   x:m_rx,x:(r7)+
        bset    #SC1,x:m_pcd            ; ask for the next word
        movep   x:m_rx,x:(r7)+          ; with exception: the same
        bset    #SC1,x:m_pcd
        movep   y:(r6)+,x:m_tx          ; SSI transmit data
        nop
        movep   y:(r6)+,x:m_tx          ; with exception: the same
        nop

; ---------------------------------------------------------------------------
        org     p:$0040
hs_start:
        movep   #1,x:m_pbc              ; host port
        movep   #$3000,x:m_ipr          ; SSI interrupts at level 2
        jsr     hs_arm
        andi    #$fc,mr

; Idle: host commands only.
hs_idle:
        jclr    #0,x:m_hsr,hs_idle
        jsr     hs_host
        jmp     hs_idle

; ---------------------------------------------------------------------------
; One screen: LINES lines of PAD zeros, GPL groups, PAD zeros; then the
; output drained and back to idle.
; ---------------------------------------------------------------------------
hs_screen:
        do      x:LINES,hs_screen_done
        jsr     hs_pad
        do      x:GPL,hs_line_done
        ; eight input words, and room for eight output words
hs_wait_input:
        jsr     hs_gate
        jsr     hs_input
        move    #>8,x0
        cmp     x0,a
        jlt     hs_wait_input
        jsr     hs_wait_room
        ; r1/r2 at input words 3 and 7, r4/r5 at output words 0 and 1
        move    r0,r1
        move    r0,r2
        move    r3,r4
        move    r3,r5
        move    (r1)+n1                 ; n1 = 3
        move    (r2)+n2                 ; n2 = 7
        move    (r5)+
        nop
        include 'C2PCORE.INC'
        move    (r0)+n0                 ; n0 = 8: the next group
        move    (r3)+n3                 ; n3 = 8
        move    x:GROUPS,a
        move    #>1,x0
        add     x0,a
        move    a1,x:GROUPS
hs_line_done:
        jsr     hs_pad
        nop                             ; a DO loop must not end on a JSR
hs_screen_done:
        ; all queued: the interrupts off, and the rest sent from here. With
        ; the interrupt still on, record's taking the last word could bring
        ; one more interrupt before this loop saw the queue empty, and that
        ; one would send a word nobody queued.
        bclr    #RIE,x:m_crb
        bclr    #TIE,x:m_crb
        move    #>1,x0
        move    x0,x:FLUSH
hs_drain:
        jsr     hs_output
        tst     a
        jeq     hs_drained
        jsr     hs_tx_start             ; in case nothing went out yet
        jclr    #6,x:m_sr,hs_drain      ; TDE: record took the last one
        movep   y:(r6)+,x:m_tx
        jmp     hs_drain
hs_drained:
        move    #>2,x0
        move    x0,x:STATE
        jmp     hs_idle

; PAD zero words into the output ring, once there is room.
hs_pad:
        jsr     hs_wait_room
        clr     a
        move    x:PAD,b
        tst     b
        jeq     hs_pad_done
        do      x:PAD,hs_pad_done
        move    a1,y:(r3)+
hs_pad_done:
        rts

; Wait until at most 16 words are queued, so eight more fit.
hs_wait_room:
        jsr     hs_gate
        jsr     hs_output
        move    #>16,x0
        cmp     x0,a
        jgt     hs_wait_room
        rts

; Input waiting, (r7 - r0) mod 32, and output queued, (r3 - r6) mod 32,
; into a. AND leaves a2 alone, so a1 goes back into a for a signed compare.
hs_input:
        move    r7,a
        move    r0,x0
        jmp     hs_ring
hs_output:
        move    r3,a
        move    r6,x0
hs_ring:
        sub     x0,a
        move    #>31,x0
        and     x0,a
        move    a1,a
        rts

; The gates: receive on while fewer than 16 words wait; transmit on while at
; least 16 are queued, which leaves eight in the queue however long a group
; takes. Only used before the screen's input is all in.
hs_gate:
        jsr     hs_input
        move    #>16,x0
        cmp     x0,a
        jge     hs_gate_rx_off
        bset    #RIE,x:m_crb
        jmp     hs_gate_tx
hs_gate_rx_off:
        bclr    #RIE,x:m_crb
hs_gate_tx:
        jsr     hs_output
        move    #>16,x0
        cmp     x0,a
        jlt     hs_gate_tx_off
        jsr     hs_tx_start
        bset    #TIE,x:m_crb
        rts
hs_gate_tx_off:
        bclr    #TIE,x:m_crb
        rts

; The screen's first word goes out from here: Hatari raises the transmit
; interrupt only once record has taken a word, whatever TDE says, so after
; arm, or with the last screen's word withdrawn, none would come to send it.
; The write also clears a pending one. Transmit interrupts must be off.
hs_tx_start:
        move    x:TXSTART,b
        tst     b
        jne     hs_tx_started
        move    #>1,x0
        move    x0,x:TXSTART
        movep   y:(r6)+,x:m_tx
hs_tx_started:
        rts

; ---------------------------------------------------------------------------
; Host commands, one word each, answered with one word; called once HRDF is
; set. CMD_SYNC does not return: it runs the screen.
; ---------------------------------------------------------------------------
hs_host:
        movep   x:m_hrx,y1              ; the whole command word
        move    y1,b
        move    #>$ff0000,x0
        and     x0,b
        move    y1,a                    ; the argument, low 16 bits
        move    #>$00ffff,x0
        and     x0,a
        move    #>CMD_PING,x0
        cmp     x0,b
        jeq     hs_cmd_ping
        move    #>CMD_ARM,x0
        cmp     x0,b
        jeq     hs_cmd_arm
        move    #>CMD_GEOMETRY,x0
        cmp     x0,b
        jeq     hs_cmd_geometry
        move    #>CMD_SYNC,x0
        cmp     x0,b
        jeq     hs_cmd_sync
        move    #>CMD_STATE,x0
        cmp     x0,b
        jeq     hs_cmd_state
        move    #>CMD_GROUPS,x0
        cmp     x0,b
        jeq     hs_cmd_groups
        movep   #REPLY_ERROR,x:m_htx
        rts
hs_cmd_ping:
        movep   #REPLY_PING,x:m_htx
        rts
hs_cmd_arm:
        jsr     hs_arm
        jmp     hs_reply_zero
hs_cmd_geometry:
        move    #>$00000f,x0            ; pad words in the low 4 bits
        and     x0,a    a1,b
        move    a1,x:PAD
        rep     #4                      ; lines above them
        lsr     b
        move    b1,x:LINES
        jmp     hs_reply_zero
hs_cmd_state:
        movep   x:STATE,x:m_htx
        rts
hs_cmd_groups:
        movep   x:GROUPS,x:m_htx
        rts
hs_cmd_sync:
        move    a1,x:GPL
        clr     a
        move    a1,x:GROUPS
        move    a1,x:FLUSH
        move    a1,x:TXSTART
        move    #>1,x0
        move    x0,x:STATE
        ori     #$03,mr                 ; rings and pointers, undisturbed
        bclr    #TIE,x:m_crb
        bclr    #RIE,x:m_crb
        move    #RING,r7
        move    #RING,r0
        move    #RING,r6
        move    #RING,r3
        andi    #$fc,mr
        movep   #0,x:m_htx
        bclr    #SC2,x:m_pcd            ; withdraw whatever TX still offered:
        bset    #SC2,x:m_pcd            ; record would take it first
        bset    #SC1,x:m_pcd            ; ask for the screen's first word
        bset    #RIE,x:m_crb
        movec   ssh,x0                  ; drop hs_host's return: run the
        jmp     hs_screen               ; screen instead of idling
hs_reply_zero:
        movep   #0,x:m_htx
        rts

; ---------------------------------------------------------------------------
; Program the SSI and port C for handshake transfers, and reset the rings:
; 16-bit words in normal, gated-clock mode, receiver and transmitter on,
; their interrupts off until a screen starts. SC1 and SC2 are GPIO outputs,
; SC2 high to let record take what TX holds.
; ---------------------------------------------------------------------------
hs_arm:
        ori     #$03,mr
        movep   #0,x:m_crb
        movep   #$1c8,x:m_pcc           ; SSI pins but SC1 and SC2
        movep   #$030,x:m_pcddr         ; SC1 and SC2 outputs
        movep   #$020,x:m_pcd           ; SC2 high, SC1 low
        movep   #$4000,x:m_cra          ; 16-bit words, one a frame
        move    #RING,r0
        clr     a
        do      #32,hs_arm_cleared
        move    a1,x:(r0)
        move    a1,y:(r0)+
hs_arm_cleared:
        move    #RING,r7
        move    #RING,r6
        move    #RING,r3
        move    #31,m7
        move    #31,m6
        move    #31,m0
        move    #31,m1
        move    #31,m2
        move    #31,m3
        move    #31,m4
        move    #31,m5
        move    #>3,n1                  ; the constant offsets of the group loop
        move    #>7,n2
        move    #>2,n4
        move    #>2,n5
        move    #>8,n0
        move    #>8,n3
        move    #RING,r0
        move    a1,x:STATE
        move    a1,x:FLUSH
        movep   #$3600,x:m_crb          ; RE, TE, gated clock, synchronous
        andi    #$fc,mr
        rts

        end
